// GX command processor: parses the FIFO / display-list command stream and drives the pipeline.
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

#include "gx_internal.h"
#include "../state_io.h"
#include "ssne/gx.h"
#include "ssne/ppc_context.h"
#include "ssne/scheduler.h"

namespace ssne {

namespace gx {
State g;
Counters counters;
}

namespace {

using namespace gx;

std::mutex G;
std::vector<uint8_t> g_pending;
// Bytes g_pending must hold before parse() can make progress (0 = unknown, parse on the next write). The game writes
// a draw command 4 bytes at a time; without this, every write re-parsed the incomplete command just to find it still
// incomplete. Only the head of g_pending is ever unparsed, and the CP registers that size it (VCD/VAT) only change
// through parsed commands or a state load, so the requirement stays valid until it is met.
size_t g_need = 0;
uint16_t g_pe[0x40];
std::atomic<uint64_t> s_bytes{0}, s_cmds{0}, s_prims{0}, s_verts{0}, s_dls{0}, s_copies{0}, s_done{0}, s_tokens{0};
std::atomic<uint32_t> s_last_token{0}, s_unknown{0};
std::vector<Vtx> g_vtx;  // scratch

void raise_pe(bool finish) {
    uint16_t& ctrl = g_pe[0x0A >> 1];
    if (finish) {
        ctrl |= 0x8;
        s_done++;
        if (ctrl & 0x2) raise_interrupt(IRQ_PE_FINISH);
    } else {
        ctrl |= 0x4;
        if (ctrl & 0x1) raise_interrupt(IRQ_PE_TOKEN);
    }
}

void load_bp(uint32_t v) {
    uint32_t reg = v >> 24, val = v & 0xFFFFFF;
    if (reg == 0xFE) { g.bp_mask = val; return; }
    g.bp[reg] = (g.bp[reg] & ~g.bp_mask) | (val & g.bp_mask);
    g.bp_mask = 0xFFFFFF;
    switch (reg) {
    case 0x45: raise_pe(true); break;                                       // PE_DONE
    case 0x47: g_pe[0x0E >> 1] = uint16_t(val); s_tokens++; s_last_token = val & 0xFFFF; break;
    case 0x48: g_pe[0x0E >> 1] = uint16_t(val); s_tokens++; s_last_token = val & 0xFFFF; raise_pe(false); break;
    case 0x52: s_copies++; efb_copy(g.bp[0x52]); break;                     // EFB copy (display / texture)
    case 0x65: load_tlut(g.bp[0x65]); break;                                // load TLUT into TMEM
    default: bp_written(reg, g.bp[reg]); break;
    }
}

inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void load_xf(const uint8_t* p) {
    uint32_t h = rd32(p);
    uint32_t count = (h >> 16) + 1, addr = h & 0xFFFF;
    for (uint32_t k = 0; k < count; k++, addr++) {
        uint32_t v = rd32(p + 4 + 4 * k);
        if (addr < 0x1000) g.xf_mem[addr] = v;
        else if (addr < 0x1100) g.xf_reg[addr - 0x1000] = v;
    }
}

size_t parse(const uint8_t* p, size_t n, int depth);

void call_display_list(uint32_t addr, uint32_t size, int depth) {
    if (depth > 4 || size > 0x400000) return;
    std::vector<uint8_t> dl(size);
    // Same bytes as mem_read8 one by one: a list in plain RAM (no MMIO / locked cache, no wrap) is copied at once.
    uint64_t end = uint64_t(addr) + size;
    if (size && end <= 0x100000000ull && (addr >> 24) == ((end - 1) >> 24) && !ssne::is_mmio(addr) &&
        (addr >> 28) != 0xE && (addr & (ssne::kRamSize - 1)) + uint64_t(size) <= ssne::kRamSize)
        memcpy(dl.data(), ssne::host_ptr(addr), size);
    else
        for (uint32_t i = 0; i < size; i++) dl[i] = mem_read8(addr + i);
    s_dls++;
    parse(dl.data(), dl.size(), depth + 1);
}

size_t parse(const uint8_t* p, size_t n, int depth) {
    size_t i = 0;
    while (i < n) {
        uint8_t op = p[i];
        size_t need;
        switch (op) {
        case 0x00: need = 1; break;
        case 0x08: need = 6; break;
        case 0x10: {
            if (n - i < 5) return i;
            need = 5 + 4 * ((rd32(p + i + 1) >> 16) + 1);
            break;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: need = 5; break;
        case 0x40: need = 9; break;
        case 0x44: need = 1; break;
        case 0x48: need = 1; break;
        case 0x61: need = 5; break;
        default:
            if ((op & 0x80) && op <= 0xBF) {
                if (n - i < 3) return i;
                uint32_t count = uint32_t(p[i + 1]) << 8 | p[i + 2];
                int vat = op & 7;
                need = 3 + size_t(count) * vertex_size(vat);
                if (n - i < need) return i;
                s_prims++;
                s_verts += count;
                decode_vertices(vat, p + i + 3, count, g_vtx);
                draw(op & 0xF8, g_vtx);
                i += need;
                s_cmds++;
                continue;
            }
            s_unknown = op;
            need = 1;
        }
        if (n - i < need) return i;
        switch (op) {
        case 0x08: g.cp[p[i + 1]] = rd32(p + i + 2); break;
        case 0x10: load_xf(p + i + 1); break;
        case 0x20: load_indexed_xf(12, rd32(p + i + 1)); break;
        case 0x28: load_indexed_xf(13, rd32(p + i + 1)); break;
        case 0x30: load_indexed_xf(14, rd32(p + i + 1)); break;
        case 0x38: load_indexed_xf(15, rd32(p + i + 1)); break;
        case 0x40: call_display_list(rd32(p + i + 1), rd32(p + i + 5), depth); break;
        case 0x61: load_bp(rd32(p + i + 1)); break;
        default: break;
        }
        i += need;
        s_cmds++;
    }
    return i;
}

// Size parse() needs to make progress on an unparsed head command `p[0..n)` (see g_need); 0 = parse anyway.
size_t head_need(const uint8_t* p, size_t n) {
    if (!n) return 0;
    uint8_t op = p[0];
    if (op == 0x10) return n < 5 ? 5 : 5 + 4 * size_t((rd32(p + 1) >> 16) + 1);
    if ((op & 0x80) && op <= 0xBF) return n < 3 ? 3 : 3 + size_t(uint32_t(p[1]) << 8 | p[2]) * vertex_size(op & 7);
    return 0;
}

} // namespace

void gx_fifo_write(uint32_t value, int size) {
    std::lock_guard<std::mutex> lk(G);
    size_t old = g_pending.size();
    g_pending.resize(old + size_t(size));
    for (int k = 0; k < size; k++) g_pending[old + size_t(k)] = uint8_t(value >> (8 * (size - 1 - k)));
    s_bytes += size;
    if (g_pending.size() < g_need) return;  // the head command is still incomplete
    size_t used = parse(g_pending.data(), g_pending.size(), 0);
    if (used) g_pending.erase(g_pending.begin(), g_pending.begin() + long(used));
    g_need = head_need(g_pending.data(), g_pending.size());
}

uint16_t gx_pe_read16(uint32_t off) { std::lock_guard<std::mutex> lk(G); return g_pe[(off & 0x7F) >> 1]; }

void gx_pe_write16(uint32_t off, uint16_t v) {
    std::lock_guard<std::mutex> lk(G);
    uint16_t& r = g_pe[(off & 0x7F) >> 1];
    if ((off & 0x7F) == 0x0A) {
        uint16_t status = r & 0xC;
        r = uint16_t((v & 0x3) | (status & ~(v & 0xC)));
        return;
    }
    r = v;
}

uint16_t gx_cp_read16(uint32_t off) { return (off & 0xFF) == 0 ? 0x000E : 0; }
void gx_cp_write16(uint32_t, uint16_t) {}

GxStats gx_stats() {
    return {s_bytes.load(), s_cmds.load(), s_prims.load(), s_verts.load(), s_dls.load(),
            s_copies.load(), s_done.load(), s_tokens.load(), s_last_token.load(), s_unknown.load()};
}

void gx_state_save(StateWriter& w) {
    std::lock_guard<std::mutex> lk(G);
    w.tag("GX01");
    w.bytes(g.cp, sizeof(g.cp)); w.bytes(g.xf_mem, sizeof(g.xf_mem)); w.bytes(g.xf_reg, sizeof(g.xf_reg));
    w.bytes(g.bp, sizeof(g.bp)); w.u32(g.bp_mask);
    w.block(g.tmem, sizeof(g.tmem));
    w.vec(g_pending);
    w.bytes(g_pe, sizeof(g_pe));
    int16_t regs[4][4], konst[4][4];
    tev_regs_access(regs, konst, false);
    w.bytes(regs, sizeof(regs)); w.bytes(konst, sizeof(konst));
}

bool gx_state_load(StateReader& r) {
    std::lock_guard<std::mutex> lk(G);
    if (!r.tag("GX01")) return false;
    r.bytes(g.cp, sizeof(g.cp)); r.bytes(g.xf_mem, sizeof(g.xf_mem)); r.bytes(g.xf_reg, sizeof(g.xf_reg));
    r.bytes(g.bp, sizeof(g.bp)); g.bp_mask = r.u32();
    if (!r.block(g.tmem, sizeof(g.tmem))) return false;
    r.vec(g_pending);
    g_need = 0;  // re-derive from the loaded FIFO contents and registers on the next write
    r.bytes(g_pe, sizeof(g_pe));
    int16_t regs[4][4], konst[4][4];
    r.bytes(regs, sizeof(regs)); r.bytes(konst, sizeof(konst));
    if (!r.ok) return false;
    tev_regs_access(regs, konst, true);
    texture_cache_clear();
    return true;
}

} // namespace ssne
