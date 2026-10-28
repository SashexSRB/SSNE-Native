// GPU renderer regression testing: recorded GX frames can be saved to disk (SSNE_GPU_CAPTURE=dir:first:count,
// counted in display frames) and executed again later by tools/gpu_replay.cpp. Replaying the same capture with
// two builds of the renderer and comparing the output images checks renderer changes without depending on the
// game's timing.
//
// File format (little-endian, one file per frame): "SSNEGPUF" u32 version, then sections of counted records.
// Textures are stored decoded (RGBA8 + mips); DrawCfg texture pointers are written as texture indices.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "gx_gpu_frame.h"

namespace ssne::gx {

namespace {
constexpr uint32_t kVersion = 3;  // 2: copies into part of an earlier copy (base_copy_id ...); 3: GPU vertex processing

template <class T> void put(FILE* f, const T& v) { fwrite(&v, sizeof(T), 1, f); }
template <class T> bool get(FILE* f, T& v) { return fread(&v, sizeof(T), 1, f) == 1; }
void put_vec32(FILE* f, const std::vector<uint32_t>& v) {
    put(f, uint32_t(v.size()));
    if (!v.empty()) fwrite(v.data(), 4, v.size(), f);
}
bool get_vec32(FILE* f, std::vector<uint32_t>& v) {
    uint32_t n;
    if (!get(f, n)) return false;
    v.resize(n);
    return n == 0 || fread(v.data(), 4, n, f) == n;
}
} // namespace

// Called by the recorder for every submitted frame.
void gpu_capture_frame(const GpuFrame& fr) {
    static const char* env = getenv("SSNE_GPU_CAPTURE");
    if (!env) return;
    static std::string dir;
    static long first = 0, count = 0, seen = 0;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        std::string e = env;
        size_t a = e.find(':'), b = a == std::string::npos ? a : e.find(':', a + 1);
        dir = e.substr(0, a);
        first = a == std::string::npos ? 0 : atol(e.substr(a + 1).c_str());
        count = b == std::string::npos ? 30 : atol(e.substr(b + 1).c_str());
    }
    long n = seen++;
    if (n < first || n >= first + count) return;
    char name[64];
    snprintf(name, sizeof(name), "/frame_%05ld.bin", n - first);
    gpu_capture_frame_to(fr, dir + name);
}

void gpu_capture_frame_to(const GpuFrame& fr, const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite("SSNEGPUF", 1, 8, f);
    put(f, kVersion);
    put(f, fr.wide);
    // textures referenced by the frame's states
    std::unordered_map<const Texture*, uint32_t> index;
    std::vector<const Texture*> list;
    for (const GpuState& st : fr.states)
        for (const Texture* t : st.cfg.tex)
            if (t && !index.count(t)) { index[t] = uint32_t(list.size()); list.push_back(t); }
    put(f, uint32_t(list.size()));
    for (const Texture* t : list) {
        put(f, int32_t(t->w)); put(f, int32_t(t->h)); put(f, t->id); put(f, t->efb_copy);
        put(f, t->copy_fmt); put(f, t->tex_fmt); put(f, uint8_t(t->intensity));
        put_vec32(f, t->rgba);
        put(f, uint32_t(t->mips.size()));
        for (const auto& m : t->mips) put_vec32(f, m);
    }
    put(f, uint32_t(fr.states.size()));
    for (const GpuState& st : fr.states) {
        GpuState copy = st;
        for (auto& t : copy.cfg.tex) t = t ? reinterpret_cast<const Texture*>(uintptr_t(index[t]) + 1) : nullptr;
        put(f, uint32_t(sizeof(GpuState)));
        fwrite(&copy, sizeof(GpuState), 1, f);
    }
    put(f, uint32_t(fr.cmds.size()));
    for (const GpuCmd& c : fr.cmds) {
        put(f, uint8_t(c.type));
        put(f, c.state); put(f, c.vtx_off); put(f, c.stride); put(f, c.ntex); put(f, c.vcount); put(f, c.idx_first);
        put(f, c.idx_count);
        put(f, c.x); put(f, c.y); put(f, c.w); put(f, c.h); put(f, c.color); put(f, c.z);
        put(f, uint8_t(c.cu)); put(f, uint8_t(c.au)); put(f, uint8_t(c.zu)); put(f, uint8_t(c.half));
        put(f, c.copy_id); put(f, c.out_w); put(f, c.out_h);
#ifdef SSNE_GPU_CMD_HAS_ZSNAP
        put(f, uint8_t(c.zsnap));
#else
        put(f, uint8_t(1));
#endif
        put(f, c.base_copy_id); put(f, c.base_w); put(f, c.base_h); put(f, c.dst_row);
    }
    put(f, uint32_t(fr.vtx.size()));
    fwrite(fr.vtx.data(), 1, fr.vtx.size(), f);
    put_vec32(f, fr.idx);
    fclose(f);
}

std::unique_ptr<GpuFrame> gpu_load_captured_frame(const std::string& path, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return nullptr; }
    auto fail = [&](const char* why) { err = path + ": " + why; fclose(f); return std::unique_ptr<GpuFrame>(); };
    char magic[8];
    uint32_t ver;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "SSNEGPUF", 8) || !get(f, ver) || ver < 1 || ver > kVersion) return fail("bad header");
    auto fr = std::make_unique<GpuFrame>();
    get(f, fr->wide);
    uint32_t nt;
    if (!get(f, nt)) return fail("truncated");
    std::vector<std::shared_ptr<Texture>> texs(nt);
    for (auto& t : texs) {
        t = std::make_shared<Texture>();
        int32_t w, h;
        uint8_t inten;
        get(f, w); get(f, h); get(f, t->id); get(f, t->efb_copy); get(f, t->copy_fmt); get(f, t->tex_fmt); get(f, inten);
        t->w = w; t->h = h; t->intensity = inten != 0;
        if (!get_vec32(f, t->rgba)) return fail("truncated texture");
        uint32_t nm;
        get(f, nm);
        t->mips.resize(nm);
        for (auto& m : t->mips) if (!get_vec32(f, m)) return fail("truncated mip");
        fr->textures.push_back(t);
    }
    uint32_t ns;
    get(f, ns);
    fr->states.resize(ns);
    for (GpuState& st : fr->states) {
        uint32_t sz;
        if (!get(f, sz) || sz != sizeof(GpuState)) return fail("state layout differs from this build");
        fread(&st, sizeof(GpuState), 1, f);
        for (auto& t : st.cfg.tex) {
            uintptr_t i = reinterpret_cast<uintptr_t>(t);
            t = (i && i <= texs.size()) ? texs[i - 1].get() : nullptr;
        }
    }
    uint32_t nc;
    get(f, nc);
    fr->cmds.resize(nc);
    for (GpuCmd& c : fr->cmds) {
        uint8_t type, cu, au, zu, half, zsnap;
        get(f, type);
        c.type = GpuCmd::Type(type);
        get(f, c.state); get(f, c.vtx_off); get(f, c.stride); get(f, c.ntex); get(f, c.vcount); get(f, c.idx_first);
        get(f, c.idx_count);
        get(f, c.x); get(f, c.y); get(f, c.w); get(f, c.h); get(f, c.color); get(f, c.z);
        get(f, cu); get(f, au); get(f, zu); get(f, half);
        c.cu = cu; c.au = au; c.zu = zu; c.half = half;
        get(f, c.copy_id); get(f, c.out_w); get(f, c.out_h);
        get(f, zsnap);
#ifdef SSNE_GPU_CMD_HAS_ZSNAP
        c.zsnap = zsnap != 0;
#endif
        if (ver >= 2) { get(f, c.base_copy_id); get(f, c.base_w); get(f, c.base_h); get(f, c.dst_row); }
    }
    uint32_t nv;
    get(f, nv);
    fr->vtx.resize(nv);
    if (fread(fr->vtx.data(), 1, nv, f) != nv) return fail("truncated vertices");
    if (!get_vec32(f, fr->idx)) return fail("truncated indices");
    fclose(f);
    return fr;
}

} // namespace ssne::gx
