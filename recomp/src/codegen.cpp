// PowerPC -> C++ translation. Each guest function becomes one C++ function; intra-function
// branches become gotos, calls become C++ calls, returns (blr) become `return`.
#include "codegen.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ppc_decode.h"

namespace fs = std::filesystem;

namespace recomp {

using namespace ppc;

namespace {

std::string F(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

const std::map<uint32_t, std::string>* g_names = nullptr;

// f_801C31B4_ARInit — the symbol (if known) is appended for readable code and backtraces.
std::string fname(uint32_t a) {
    std::string s = F("f_%08X", a);
    if (g_names) {
        auto it = g_names->find(a);
        if (it != g_names->end()) {
            s += '_';
            for (char c : it->second) s += (isalnum(uint8_t(c)) || c == '_') ? c : '_';
        }
    }
    return s;
}
std::string label(uint32_t a) { return F("L_%08X", a); }
std::string r(uint32_t n) { return F("ctx.r[%u]", n); }
std::string ra0(uint32_t n) { return n ? r(n) : "0u"; }
std::string fp(uint32_t n) { return F("ctx.ps0[%u]", n); }
std::string fp1(uint32_t n) { return F("ctx.ps1[%u]", n); }
std::string imm(int32_t v) { return F("0x%Xu", uint32_t(v)); }

class Emitter {
public:
    Emitter(const gc::Dol& dol, const Analysis& an, const Function& fn, CodegenStats& st)
        : dol_(dol), an_(an), fn_(fn), st_(st) {}

    std::string run(bool comments);

private:
    void emit(const std::string& s) { out_ << "    " << s << "\n"; }
    void unimpl(uint32_t a, uint32_t i) {
        st_.unimplemented++;
        st_.unimplemented_by_op[F("%s/%u", mnemonic(i).c_str(), OPCD(i) == 4 || OPCD(i) == 19 || OPCD(i) == 31 ||
                                         OPCD(i) == 59 || OPCD(i) == 63 ? XO10(i) : 0)]++;
        emit(F("ssne_unimplemented(ctx, 0x%08X, 0x%08X);", a, i));
    }
    std::string bc_cond(uint32_t i, bool use_ctr);
    void branch_to(uint32_t from, uint32_t target, bool link, uint32_t next);
    bool integer(uint32_t a, uint32_t i);
    bool op31(uint32_t a, uint32_t i);
    bool op19(uint32_t a, uint32_t i);
    bool fpu(uint32_t a, uint32_t i);
    bool paired(uint32_t a, uint32_t i);
    bool loadstore(uint32_t a, uint32_t i);
    void rc(uint32_t i, const std::string& reg) { if (Rc(i)) emit("cr0_update(ctx, " + reg + ");"); }
    void fprc(uint32_t i) {
        if (Rc(i)) emit("ctx.cr[1] = uint8_t(ctx.fpscr >> 28);");
    }

    const gc::Dol& dol_;
    const Analysis& an_;
    const Function& fn_;
    CodegenStats& st_;
    std::ostringstream out_;
};

std::string Emitter::bc_cond(uint32_t i, bool use_ctr) {
    uint32_t bo = RD(i), bi = RA(i);
    std::string c;
    if (use_ctr && !(bo & 4)) c = (bo & 2) ? "ctx.ctr == 0" : "ctx.ctr != 0";
    if (!(bo & 0x10)) {
        std::string b = F("cr_bit(ctx, %u)", bi);
        if (!(bo & 8)) b = "!" + b;
        c = c.empty() ? b : "(" + c + ") && " + b;
    }
    return c.empty() ? "true" : c;
}

void Emitter::branch_to(uint32_t from, uint32_t target, bool link, uint32_t next) {
    if (link) {
        emit(F("ctx.lr = 0x%08X;", next));
        if (an_.is_start(target)) emit(fname(target) + "(ctx);");
        else emit(F("ssne_call(ctx, 0x%08X);", target));
        return;
    }
    if (target >= fn_.start && target < fn_.end) {
        if (target <= from) emit("SSNE_POLL();");  // loop back-edge: interrupts can be delivered here
        emit("goto " + label(target) + ";");
        return;
    }
    // Tail call into another function.
    if (an_.is_start(target)) emit(fname(target) + "(ctx); return;");
    else emit(F("ssne_call(ctx, 0x%08X); return;", target));
}

std::string Emitter::run(bool comments) {
    std::string display = fn_.name.empty() ? "" : " // " + fn_.name;
    out_ << "void " << fname(fn_.start) << "(PPCContext& ctx) {" << display << "\n";
    out_ << F("    SSNE_HOOK(0x%08X);\n", fn_.start);
    for (uint32_t a = fn_.start; a < fn_.end; a += 4) {
        uint32_t i = 0;
        dol_.read32(a, i);
        st_.instructions++;
        if (fn_.labels.count(a)) out_ << label(a) << ":\n";
        if (comments) out_ << F("    // %08X: %08X %s\n", a, i, mnemonic(i).c_str());
        bool ok = false;
        uint32_t op = OPCD(i);
        switch (op) {
        case 4: ok = paired(a, i); break;
        case 16: {                                   // bc
            uint32_t t = branch_target(a, i);
            std::string c = bc_cond(i, true);
            if (!(RD(i) & 4)) emit("ctx.ctr--;");
            out_ << "    if (" << c << ") {\n";
            branch_to(a, t, LK(i), a + 4);
            out_ << "    }\n";
            ok = true;
            break;
        }
        case 17: emit(F("ssne_syscall(ctx, 0x%08X);", a)); ok = true; break;
        case 18: branch_to(a, branch_target(a, i), LK(i), a + 4); ok = true; break;
        case 19: ok = op19(a, i); break;
        case 31: ok = op31(a, i); break;
        case 59: case 63: ok = fpu(a, i); break;
        case 56: case 57: case 60: case 61: ok = paired(a, i); break;
        default:
            if (op >= 32 && op <= 55) ok = loadstore(a, i);
            else ok = integer(a, i);
        }
        if (!ok) unimpl(a, i);
    }
    // Falling off the end = continuing into the next function (rare; happens with split functions).
    if (an_.is_start(fn_.end)) out_ << "    " << fname(fn_.end) << "(ctx);\n";
    out_ << "}\n\n";
    return out_.str();
}

bool Emitter::integer(uint32_t a, uint32_t i) {
    uint32_t d = RD(i), s = RD(i), A = RA(i);
    switch (OPCD(i)) {
    case 3: emit("/* twi: trap ignored */"); return true;
    case 7: emit(r(d) + " = uint32_t(int32_t(" + r(A) + ") * " + std::to_string(SIMM(i)) + ");"); return true;
    case 8:
        emit(F("{ uint32_t a_ = %s; %s = %s - a_; ctx.xer_ca = a_ <= %s; }", r(A).c_str(), r(d).c_str(),
               imm(SIMM(i)).c_str(), imm(SIMM(i)).c_str()));
        return true;
    case 10:
        emit(F("cr_set_cmp(ctx, %u, %s < %uu, %s > %uu, %s == %uu);", CRFD(i), r(A).c_str(), UIMM(i),
               r(A).c_str(), UIMM(i), r(A).c_str(), UIMM(i)));
        return true;
    case 11:
        emit(F("{ int32_t a_ = int32_t(%s); cr_set_cmp(ctx, %u, a_ < %d, a_ > %d, a_ == %d); }", r(A).c_str(),
               CRFD(i), SIMM(i), SIMM(i), SIMM(i)));
        return true;
    case 12: case 13:
        emit(F("{ uint32_t a_ = %s; %s = a_ + %s; ctx.xer_ca = %s < a_; }", r(A).c_str(), r(d).c_str(),
               imm(SIMM(i)).c_str(), r(d).c_str()));
        if (OPCD(i) == 13) emit("cr0_update(ctx, " + r(d) + ");");
        return true;
    case 14: emit(r(d) + " = " + ra0(A) + " + " + imm(SIMM(i)) + ";"); return true;
    case 15: emit(r(d) + " = " + ra0(A) + " + " + imm(int32_t(UIMM(i) << 16)) + ";"); return true;
    case 20: {
        uint32_t m = rot_mask(MB(i), ME(i));
        emit(F("%s = (rotl32(%s, %u) & 0x%08Xu) | (%s & 0x%08Xu);", r(A).c_str(), r(s).c_str(), SH(i), m,
               r(A).c_str(), ~m));
        rc(i, r(A));
        return true;
    }
    case 21: {
        uint32_t m = rot_mask(MB(i), ME(i));
        emit(F("%s = rotl32(%s, %u) & 0x%08Xu;", r(A).c_str(), r(s).c_str(), SH(i), m));
        rc(i, r(A));
        return true;
    }
    case 23: {
        uint32_t m = rot_mask(MB(i), ME(i));
        emit(F("%s = rotl32(%s, %s & 31) & 0x%08Xu;", r(A).c_str(), r(s).c_str(), r(RB(i)).c_str(), m));
        rc(i, r(A));
        return true;
    }
    case 24: emit(F("%s = %s | 0x%Xu;", r(A).c_str(), r(s).c_str(), UIMM(i))); return true;
    case 25: emit(F("%s = %s | 0x%Xu;", r(A).c_str(), r(s).c_str(), UIMM(i) << 16)); return true;
    case 26: emit(F("%s = %s ^ 0x%Xu;", r(A).c_str(), r(s).c_str(), UIMM(i))); return true;
    case 27: emit(F("%s = %s ^ 0x%Xu;", r(A).c_str(), r(s).c_str(), UIMM(i) << 16)); return true;
    case 28: emit(F("%s = %s & 0x%Xu;", r(A).c_str(), r(s).c_str(), UIMM(i))); emit("cr0_update(ctx, " + r(A) + ");"); return true;
    case 29: emit(F("%s = %s & 0x%Xu;", r(A).c_str(), r(s).c_str(), UIMM(i) << 16)); emit("cr0_update(ctx, " + r(A) + ");"); return true;
    }
    return false;
}

bool Emitter::op19(uint32_t a, uint32_t i) {
    uint32_t xo = XO10(i);
    uint32_t bt = RD(i), ba = RA(i), bb = RB(i);
    auto crop = [&](const char* expr) {
        emit(F("{ uint32_t a_ = cr_bit(ctx, %u), b_ = cr_bit(ctx, %u); cr_set_bit(ctx, %u, (%s) & 1); }", ba, bb,
               bt, expr));
        return true;
    };
    switch (xo) {
    case 0: emit(F("ctx.cr[%u] = ctx.cr[%u];", CRFD(i), CRFS(i))); return true; // mcrf
    case 16: {                                                                   // bclr
        std::string c = bc_cond(i, true);
        if (!(RD(i) & 4)) emit("ctx.ctr--;");
        if (LK(i)) {
            // Call through LR (rare): target is the old LR.
            out_ << "    if (" << c << ") { uint32_t t_ = ctx.lr; ctx.lr = " << F("0x%08X", a + 4)
                 << "; ssne_call(ctx, t_); }\n";
        } else if (c == "true") {
            emit("return;");
        } else {
            out_ << "    if (" << c << ") return;\n";
        }
        return true;
    }
    case 528: {                                                                  // bcctr
        std::string c = bc_cond(i, false);
        out_ << "    if (" << c << ") {\n";
        if (LK(i)) {
            emit(F("ctx.lr = 0x%08X;", a + 4));
            emit("ssne_call(ctx, ctx.ctr);");
        } else {
            auto jt = fn_.jump_tables.find(a);
            if (jt != fn_.jump_tables.end()) {
                emit("switch (ctx.ctr) {");
                std::set<uint32_t> seen;
                for (uint32_t t : jt->second)
                    if (seen.insert(t).second) emit(F("case 0x%08Xu: goto %s;", t, label(t).c_str()));
                emit("}");
            }
            emit("ssne_call(ctx, ctx.ctr); return;");
        }
        out_ << "    }\n";
        return true;
    }
    case 33: return crop("~(a_ | b_)");  // crnor
    case 129: return crop("a_ & ~b_");   // crandc
    case 193: return crop("a_ ^ b_");    // crxor
    case 225: return crop("~(a_ & b_)"); // crnand
    case 257: return crop("a_ & b_");    // crand
    case 289: return crop("~(a_ ^ b_)"); // creqv
    case 417: return crop("a_ | ~b_");   // crorc
    case 449: return crop("a_ | b_");    // cror
    case 150: emit("/* isync */"); return true;
    case 50:                                                                     // rfi
        // SDK uses rfi to change MSR (MMU on) and "return" to SRR0. Address translation is
        // implicit on PC, so this is a return when SRR0 == LR, otherwise a jump.
        emit("ctx.msr = ctx.srr1;");
        emit("if (ctx.srr0 == ctx.lr) return;");
        emit("ssne_call(ctx, ctx.srr0); return;");
        return true;
    }
    return false;
}

bool Emitter::op31(uint32_t a, uint32_t i) {
    uint32_t d = RD(i), s = RD(i), A = RA(i), B = RB(i);
    uint32_t xo = XO10(i), xo9 = XO9(i);
    std::string rd = r(d), ra = r(A), rb = r(B), rs = r(s);
    auto ea = [&]() { return "(" + ra0(A) + " + " + rb + ")"; };
    auto eau = [&]() { return "(" + ra + " + " + rb + ")"; };
    if (OE(i) && xo9 != 0) {
        // Overflow-enabled forms set XER[OV/SO]. Rare in compiled game code: flag them.
        switch (xo9) { case 266: case 40: case 235: case 491: case 459: case 104: case 10: case 8: case 138: case 136:
            case 202: case 200: case 234: case 232:
            emit("/* OE=1: XER[OV] not modeled */");
        }
    }
    switch (xo9) {
    case 266: emit(rd + " = " + ra + " + " + rb + ";"); rc(i, rd); return true;                     // add
    case 10: emit(F("{ uint32_t a_ = %s; %s = a_ + %s; ctx.xer_ca = %s < a_; }", ra.c_str(), rd.c_str(), rb.c_str(), rd.c_str())); rc(i, rd); return true; // addc
    case 138: emit(F("{ uint64_t t_ = uint64_t(%s) + %s + ctx.xer_ca; %s = uint32_t(t_); ctx.xer_ca = uint32_t(t_ >> 32); }", ra.c_str(), rb.c_str(), rd.c_str())); rc(i, rd); return true; // adde
    case 202: emit(F("{ uint64_t t_ = uint64_t(%s) + ctx.xer_ca; %s = uint32_t(t_); ctx.xer_ca = uint32_t(t_ >> 32); }", ra.c_str(), rd.c_str())); rc(i, rd); return true; // addze
    case 234: emit(F("{ uint64_t t_ = uint64_t(%s) + ctx.xer_ca + 0xFFFFFFFFull; %s = uint32_t(t_); ctx.xer_ca = uint32_t(t_ >> 32); }", ra.c_str(), rd.c_str())); rc(i, rd); return true; // addme
    case 40: emit(rd + " = " + rb + " - " + ra + ";"); rc(i, rd); return true;                       // subf
    case 8: emit(F("{ uint32_t a_ = %s, b_ = %s; %s = b_ - a_; ctx.xer_ca = b_ >= a_; }", ra.c_str(), rb.c_str(), rd.c_str())); rc(i, rd); return true; // subfc
    case 136: emit(F("{ uint64_t t_ = uint64_t(~%s) + %s + ctx.xer_ca; %s = uint32_t(t_); ctx.xer_ca = uint32_t(t_ >> 32); }", ra.c_str(), rb.c_str(), rd.c_str())); rc(i, rd); return true; // subfe
    case 200: emit(F("{ uint64_t t_ = uint64_t(uint32_t(~%s)) + ctx.xer_ca; %s = uint32_t(t_); ctx.xer_ca = uint32_t(t_ >> 32); }", ra.c_str(), rd.c_str())); rc(i, rd); return true; // subfze
    case 232: emit(F("{ uint64_t t_ = uint64_t(uint32_t(~%s)) + ctx.xer_ca + 0xFFFFFFFFull; %s = uint32_t(t_); ctx.xer_ca = uint32_t(t_ >> 32); }", ra.c_str(), rd.c_str())); rc(i, rd); return true; // subfme
    case 104: emit(rd + " = uint32_t(-int32_t(" + ra + "));"); rc(i, rd); return true;               // neg
    case 235: emit(rd + " = uint32_t(int32_t(" + ra + ") * int32_t(" + rb + "));"); rc(i, rd); return true; // mullw
    case 75: emit(rd + " = uint32_t((int64_t(int32_t(" + ra + ")) * int32_t(" + rb + ")) >> 32);"); rc(i, rd); return true; // mulhw
    case 11: emit(rd + " = uint32_t((uint64_t(" + ra + ") * " + rb + ") >> 32);"); rc(i, rd); return true; // mulhwu
    case 491: emit(F("{ int32_t a_ = int32_t(%s), b_ = int32_t(%s); %s = (b_ == 0 || (a_ == INT32_MIN && b_ == -1)) ? (a_ < 0 ? 0xFFFFFFFFu : 0u) : uint32_t(a_ / b_); }", ra.c_str(), rb.c_str(), rd.c_str())); rc(i, rd); return true; // divw
    case 459: emit(F("{ uint32_t b_ = %s; %s = b_ ? %s / b_ : 0u; }", rb.c_str(), rd.c_str(), ra.c_str())); rc(i, rd); return true; // divwu
    }
    switch (xo) {
    case 0:  emit(F("{ int32_t a_ = int32_t(%s), b_ = int32_t(%s); cr_set_cmp(ctx, %u, a_ < b_, a_ > b_, a_ == b_); }", ra.c_str(), rb.c_str(), CRFD(i))); return true; // cmp
    case 32: emit(F("{ uint32_t a_ = %s, b_ = %s; cr_set_cmp(ctx, %u, a_ < b_, a_ > b_, a_ == b_); }", ra.c_str(), rb.c_str(), CRFD(i))); return true;                   // cmpl
    case 4: emit("/* tw: trap ignored */"); return true;
    case 19: emit(rd + " = cr_get(ctx);"); return true;                                               // mfcr
    case 144: emit(F("cr_put(ctx, %s, 0x%02X);", rs.c_str(), CRM(i))); return true;                    // mtcrf
    case 512: emit(F("ctx.cr[%u] = uint8_t((ctx.xer_so << 3) | (ctx.xer_ov << 2) | (ctx.xer_ca << 1)); ctx.xer_so = ctx.xer_ov = ctx.xer_ca = 0;", CRFD(i))); return true; // mcrxr
    case 83: emit(rd + " = ctx.msr;"); return true;                                                   // mfmsr
    case 146: emit("ctx.msr = " + rs + ";"); return true;                                             // mtmsr
    case 339: {                                                                                       // mfspr
        uint32_t spr = SPR(i);
        if (spr == 8) emit(rd + " = ctx.lr;");
        else if (spr == 9) emit(rd + " = ctx.ctr;");
        else if (spr == 1) emit(rd + " = (ctx.xer_so << 31) | (ctx.xer_ov << 30) | (ctx.xer_ca << 29);");
        else if (spr >= 912 && spr <= 919) emit(rd + F(" = ctx.gqr[%u];", spr - 912));
        else emit(rd + F(" = ssne_mfspr(ctx, %u);", spr));
        return true;
    }
    case 467: {                                                                                       // mtspr
        uint32_t spr = SPR(i);
        if (spr == 8) emit("ctx.lr = " + rs + ";");
        else if (spr == 9) emit("ctx.ctr = " + rs + ";");
        else if (spr == 1) emit("ctx.xer_so = " + rs + " >> 31; ctx.xer_ov = (" + rs + " >> 30) & 1; ctx.xer_ca = (" + rs + " >> 29) & 1;");
        else if (spr >= 912 && spr <= 919) emit(F("ctx.gqr[%u] = %s;", spr - 912, rs.c_str()));
        else emit(F("ssne_mtspr(ctx, %u, %s);", spr, rs.c_str()));
        return true;
    }
    case 371: emit(rd + (SPR(i) == 269 ? " = uint32_t(ssne_timebase() >> 32);" : " = uint32_t(ssne_timebase());")); return true; // mftb
    case 28:  emit(ra + " = " + rs + " & " + rb + ";"); rc(i, ra); return true;                       // and
    case 60:  emit(ra + " = " + rs + " & ~" + rb + ";"); rc(i, ra); return true;                      // andc
    case 444: emit(ra + " = " + rs + " | " + rb + ";"); rc(i, ra); return true;                       // or / mr
    case 412: emit(ra + " = " + rs + " | ~" + rb + ";"); rc(i, ra); return true;                      // orc
    case 124: emit(ra + " = ~(" + rs + " | " + rb + ");"); rc(i, ra); return true;                    // nor
    case 316: emit(ra + " = " + rs + " ^ " + rb + ";"); rc(i, ra); return true;                       // xor
    case 284: emit(ra + " = ~(" + rs + " ^ " + rb + ");"); rc(i, ra); return true;                    // eqv
    case 476: emit(ra + " = ~(" + rs + " & " + rb + ");"); rc(i, ra); return true;                    // nand
    case 26:  emit(ra + " = cntlzw(" + rs + ");"); rc(i, ra); return true;
    case 954: emit(ra + " = uint32_t(int32_t(int8_t(" + rs + ")));"); rc(i, ra); return true;         // extsb
    case 922: emit(ra + " = uint32_t(int32_t(int16_t(" + rs + ")));"); rc(i, ra); return true;        // extsh
    case 24:  emit(F("{ uint32_t n_ = %s & 63; %s = n_ > 31 ? 0u : %s << n_; }", rb.c_str(), ra.c_str(), rs.c_str())); rc(i, ra); return true; // slw
    case 536: emit(F("{ uint32_t n_ = %s & 63; %s = n_ > 31 ? 0u : %s >> n_; }", rb.c_str(), ra.c_str(), rs.c_str())); rc(i, ra); return true; // srw
    case 792: emit(F("{ uint32_t n_ = %s & 63; int32_t s_ = int32_t(%s); if (n_ > 31) { %s = uint32_t(s_ >> 31); ctx.xer_ca = s_ < 0; } else { %s = uint32_t(s_ >> n_); ctx.xer_ca = s_ < 0 && n_ && (uint32_t(s_) << (32 - n_)) != 0; } }",
                   rb.c_str(), rs.c_str(), ra.c_str(), ra.c_str())); rc(i, ra); return true;          // sraw
    case 824: {                                                                                       // srawi
        uint32_t n = SH(i);
        if (n == 0) emit(ra + " = " + rs + "; ctx.xer_ca = 0;");
        else emit(F("{ int32_t s_ = int32_t(%s); %s = uint32_t(s_ >> %u); ctx.xer_ca = s_ < 0 && (uint32_t(s_) << %u) != 0; }", rs.c_str(), ra.c_str(), n, 32 - n));
        rc(i, ra);
        return true;
    }
    // loads/stores (indexed)
    case 23:  emit(rd + " = mem_read32" + ea() + ";"); return true;                                   // lwzx
    case 55:  emit(F("{ uint32_t ea_ = %s; %s = mem_read32(ea_); %s = ea_; }", eau().c_str(), rd.c_str(), ra.c_str())); return true; // lwzux
    case 87:  emit(rd + " = mem_read8" + ea() + ";"); return true;                                    // lbzx
    case 119: emit(F("{ uint32_t ea_ = %s; %s = mem_read8(ea_); %s = ea_; }", eau().c_str(), rd.c_str(), ra.c_str())); return true;
    case 279: emit(rd + " = mem_read16" + ea() + ";"); return true;                                   // lhzx
    case 311: emit(F("{ uint32_t ea_ = %s; %s = mem_read16(ea_); %s = ea_; }", eau().c_str(), rd.c_str(), ra.c_str())); return true;
    case 343: emit(rd + " = uint32_t(int32_t(int16_t(mem_read16" + ea() + ")));"); return true;      // lhax
    case 375: emit(F("{ uint32_t ea_ = %s; %s = uint32_t(int32_t(int16_t(mem_read16(ea_)))); %s = ea_; }", eau().c_str(), rd.c_str(), ra.c_str())); return true;
    case 534: emit(rd + " = SSNE_BSWAP32(mem_read32" + ea() + ");"); return true;                     // lwbrx
    case 790: emit(rd + " = SSNE_BSWAP16(mem_read16" + ea() + ");"); return true;                     // lhbrx
    case 151: emit("mem_write32(" + ea() + ", " + rs + ");"); return true;                            // stwx
    case 183: emit(F("{ uint32_t ea_ = %s; mem_write32(ea_, %s); %s = ea_; }", eau().c_str(), rs.c_str(), ra.c_str())); return true;
    case 215: emit("mem_write8(" + ea() + ", uint8_t(" + rs + "));"); return true;                    // stbx
    case 247: emit(F("{ uint32_t ea_ = %s; mem_write8(ea_, uint8_t(%s)); %s = ea_; }", eau().c_str(), rs.c_str(), ra.c_str())); return true;
    case 407: emit("mem_write16(" + ea() + ", uint16_t(" + rs + "));"); return true;                  // sthx
    case 439: emit(F("{ uint32_t ea_ = %s; mem_write16(ea_, uint16_t(%s)); %s = ea_; }", eau().c_str(), rs.c_str(), ra.c_str())); return true;
    case 662: emit("mem_write32(" + ea() + ", SSNE_BSWAP32(" + rs + "));"); return true;              // stwbrx
    case 918: emit("mem_write16(" + ea() + ", SSNE_BSWAP16(uint16_t(" + rs + ")));"); return true;    // sthbrx
    case 535: emit(F("ctx.ps0[%u] = ctx.ps1[%u] = double(u2f(mem_read32%s));", d, d, ea().c_str())); return true; // lfsx
    case 567: emit(F("{ uint32_t ea_ = %s; ctx.ps0[%u] = ctx.ps1[%u] = double(u2f(mem_read32(ea_))); %s = ea_; }", eau().c_str(), d, d, ra.c_str())); return true;
    case 599: emit(F("ctx.ps0[%u] = u2d(mem_read64%s);", d, ea().c_str())); return true;             // lfdx
    case 631: emit(F("{ uint32_t ea_ = %s; ctx.ps0[%u] = u2d(mem_read64(ea_)); %s = ea_; }", eau().c_str(), d, ra.c_str())); return true;
    case 663: emit(F("mem_write32(%s, f2u(float(ctx.ps0[%u])));", ea().c_str(), s)); return true;    // stfsx
    case 695: emit(F("{ uint32_t ea_ = %s; mem_write32(ea_, f2u(float(ctx.ps0[%u]))); %s = ea_; }", eau().c_str(), s, ra.c_str())); return true;
    case 727: emit(F("mem_write64(%s, d2u(ctx.ps0[%u]));", ea().c_str(), s)); return true;           // stfdx
    case 759: emit(F("{ uint32_t ea_ = %s; mem_write64(ea_, d2u(ctx.ps0[%u])); %s = ea_; }", eau().c_str(), s, ra.c_str())); return true;
    case 983: emit(F("mem_write32(%s, uint32_t(d2u(ctx.ps0[%u])));", ea().c_str(), s)); return true;  // stfiwx
    case 1014: emit(F("{ uint32_t ea_ = %s & ~31u; for (int k_ = 0; k_ < 32; k_ += 4) mem_write32(ea_ + k_, 0); }", ea().c_str())); return true; // dcbz
    case 86: case 54: case 278: case 246: case 470: case 982: case 598: case 854: case 566: case 306: case 370:
        emit("/* cache/sync op: no-op on PC */"); return true; // dcbf dcbst dcbt dcbtst dcbi icbi sync eieio tlbsync tlbie tlbia
    case 595: case 659: emit(rd + " = 0; /* mfsr/mfsrin: no MMU segments on PC */"); return true;
    case 210: case 242: emit("/* mtsr/mtsrin ignored */"); return true;
    case 597: case 725: return false;                                                                 // lswi/stswi (rare)
    }
    return false;
}

bool Emitter::loadstore(uint32_t a, uint32_t i) {
    uint32_t d = RD(i), A = RA(i);
    std::string off = imm(SIMM(i));
    std::string ea = "(" + ra0(A) + " + " + off + ")";
    std::string eau = "(" + r(A) + " + " + off + ")";
    auto upd = [&](const std::string& body) {  // body uses ea_
        emit("{ uint32_t ea_ = " + eau + "; " + body + " " + r(A) + " = ea_; }");
    };
    std::string rd = r(d);
    switch (OPCD(i)) {
    case 32: emit(rd + " = mem_read32" + ea + ";"); return true;
    case 33: upd(rd + " = mem_read32(ea_);"); return true;
    case 34: emit(rd + " = mem_read8" + ea + ";"); return true;
    case 35: upd(rd + " = mem_read8(ea_);"); return true;
    case 36: emit("mem_write32(" + ea + ", " + rd + ");"); return true;
    case 37: upd("mem_write32(ea_, " + rd + ");"); return true;
    case 38: emit("mem_write8(" + ea + ", uint8_t(" + rd + "));"); return true;
    case 39: upd("mem_write8(ea_, uint8_t(" + rd + "));"); return true;
    case 40: emit(rd + " = mem_read16" + ea + ";"); return true;
    case 41: upd(rd + " = mem_read16(ea_);"); return true;
    case 42: emit(rd + " = uint32_t(int32_t(int16_t(mem_read16" + ea + ")));"); return true;
    case 43: upd(rd + " = uint32_t(int32_t(int16_t(mem_read16(ea_))));"); return true;
    case 44: emit("mem_write16(" + ea + ", uint16_t(" + rd + "));"); return true;
    case 45: upd("mem_write16(ea_, uint16_t(" + rd + "));"); return true;
    case 46: emit(F("{ uint32_t ea_ = %s; for (int k_ = %u; k_ < 32; k_++, ea_ += 4) ctx.r[k_] = mem_read32(ea_); }", ea.c_str(), d)); return true; // lmw
    case 47: emit(F("{ uint32_t ea_ = %s; for (int k_ = %u; k_ < 32; k_++, ea_ += 4) mem_write32(ea_, ctx.r[k_]); }", ea.c_str(), d)); return true; // stmw
    case 48: emit(F("ctx.ps0[%u] = ctx.ps1[%u] = double(u2f(mem_read32%s));", d, d, ea.c_str())); return true;  // lfs
    case 49: upd(F("ctx.ps0[%u] = ctx.ps1[%u] = double(u2f(mem_read32(ea_)));", d, d)); return true;
    case 50: emit(F("ctx.ps0[%u] = u2d(mem_read64%s);", d, ea.c_str())); return true;                         // lfd
    case 51: upd(F("ctx.ps0[%u] = u2d(mem_read64(ea_));", d)); return true;
    case 52: emit(F("mem_write32(%s, f2u(float(ctx.ps0[%u])));", ea.c_str(), d)); return true;                 // stfs
    case 53: upd(F("mem_write32(ea_, f2u(float(ctx.ps0[%u])));", d)); return true;
    case 54: emit(F("mem_write64(%s, d2u(ctx.ps0[%u]));", ea.c_str(), d)); return true;                        // stfd
    case 55: upd(F("mem_write64(ea_, d2u(ctx.ps0[%u]));", d)); return true;
    }
    return false;
}

bool Emitter::fpu(uint32_t a, uint32_t i) {
    bool single = OPCD(i) == 59;
    uint32_t d = RD(i), A = RA(i), B = RB(i), C = RC(i);
    std::string fd = fp(d), fa = fp(A), fb = fp(B), fc = fp(C);
    auto res = [&](const std::string& expr) {
        if (single) emit(F("ctx.ps0[%u] = ctx.ps1[%u] = round_single(%s);", d, d, expr.c_str()));
        else emit(fd + " = " + expr + ";");
        fprc(i);
        return true;
    };
    switch (XO5(i)) {
    case 18: return res(fa + " / " + fb);
    case 20: return res(fa + " - " + fb);
    case 21: return res(fa + " + " + fb);
    case 22: return res("std::sqrt(" + fb + ")");
    case 24: return res("1.0 / " + fb);                                            // fres
    case 25: return res(fa + " * " + fc);
    case 26: return res("1.0 / std::sqrt(" + fb + ")");                            // frsqrte
    case 28: return res(fa + " * " + fc + " - " + fb);                             // fmsub
    case 29: return res(fa + " * " + fc + " + " + fb);                             // fmadd
    case 30: return res("-(" + fa + " * " + fc + " - " + fb + ")");                // fnmsub
    case 31: return res("-(" + fa + " * " + fc + " + " + fb + ")");                // fnmadd
    case 23: if (!single) return res("(" + fa + " >= 0.0 ? " + fc + " : " + fb + ")"); break; // fsel
    }
    if (single) return false;
    switch (XO10(i)) {
    case 0:  emit(F("fcmp(ctx, %u, %s, %s);", CRFD(i), fa.c_str(), fb.c_str())); return true;  // fcmpu
    case 32: emit(F("fcmp(ctx, %u, %s, %s);", CRFD(i), fa.c_str(), fb.c_str())); return true;  // fcmpo
    case 12: emit(F("ctx.ps0[%u] = round_single(%s);", d, fb.c_str())); fprc(i); return true;  // frsp
    case 14: case 15: emit(F("set_fpr_int(ctx, %u, fctiw_sat(%s));", d, XO10(i) == 15 ? fb.c_str() : ("std::nearbyint(" + fb + ")").c_str())); return true; // fctiw(z)
    case 40: emit(fd + " = -" + fb + ";"); fprc(i); return true;                                   // fneg
    case 72: emit(fd + " = " + fb + ";"); fprc(i); return true;                                    // fmr
    case 136: emit(fd + " = -std::fabs(" + fb + ");"); fprc(i); return true;                       // fnabs
    case 264: emit(fd + " = std::fabs(" + fb + ");"); fprc(i); return true;                        // fabs
    case 583: emit(F("ctx.ps0[%u] = u2d(uint64_t(ctx.fpscr));", d)); return true;                  // mffs
    case 711: emit(F("ctx.fpscr = uint32_t(d2u(ctx.ps0[%u]));", B)); return true;                  // mtfsf (approx.)
    case 70: case 38: case 134: case 64: emit("/* fpscr bit op ignored */"); return true;          // mtfsb0/1, mtfsfi, mcrfs
    }
    return false;
}

bool Emitter::paired(uint32_t a, uint32_t i) {
    uint32_t d = RD(i), A = RA(i), B = RB(i), C = RC(i);
    switch (OPCD(i)) {
    case 56: emit(F("psq_load(ctx, %u, %s + %s, %u, %u);", d, ra0(A).c_str(), imm(PSQ_D(i)).c_str(), PSQ_W(i), PSQ_I(i))); return true;
    case 57: emit(F("{ uint32_t ea_ = %s + %s; psq_load(ctx, %u, ea_, %u, %u); %s = ea_; }", r(A).c_str(), imm(PSQ_D(i)).c_str(), d, PSQ_W(i), PSQ_I(i), r(A).c_str())); return true;
    case 60: emit(F("psq_store(ctx, %u, %s + %s, %u, %u);", d, ra0(A).c_str(), imm(PSQ_D(i)).c_str(), PSQ_W(i), PSQ_I(i))); return true;
    case 61: emit(F("{ uint32_t ea_ = %s + %s; psq_store(ctx, %u, ea_, %u, %u); %s = ea_; }", r(A).c_str(), imm(PSQ_D(i)).c_str(), d, PSQ_W(i), PSQ_I(i), r(A).c_str())); return true;
    }
    // opcode 4
    auto both = [&](const std::string& e0, const std::string& e1) {
        emit(F("{ double t0_ = round_single(%s), t1_ = round_single(%s); ctx.ps0[%u] = t0_; ctx.ps1[%u] = t1_; }",
               e0.c_str(), e1.c_str(), d, d));
        fprc(i);
        return true;
    };
    std::string a0 = fp(A), a1 = fp1(A), b0 = fp(B), b1 = fp1(B), c0 = fp(C), c1 = fp1(C);
    switch (XO10(i)) {
    case 0:  emit(F("fcmp(ctx, %u, %s, %s);", CRFD(i), a0.c_str(), b0.c_str())); return true;  // ps_cmpu0
    case 32: emit(F("fcmp(ctx, %u, %s, %s);", CRFD(i), a0.c_str(), b0.c_str())); return true;  // ps_cmpo0
    case 64: emit(F("fcmp(ctx, %u, %s, %s);", CRFD(i), a1.c_str(), b1.c_str())); return true;  // ps_cmpu1
    case 96: emit(F("fcmp(ctx, %u, %s, %s);", CRFD(i), a1.c_str(), b1.c_str())); return true;  // ps_cmpo1
    case 40:  return both("-" + b0, "-" + b1);                                        // ps_neg
    case 72:  return both(b0, b1);                                                    // ps_mr
    case 136: return both("-std::fabs(" + b0 + ")", "-std::fabs(" + b1 + ")");        // ps_nabs
    case 264: return both("std::fabs(" + b0 + ")", "std::fabs(" + b1 + ")");          // ps_abs
    case 528: return both(a0, b0);                                                    // ps_merge00
    case 560: return both(a0, b1);                                                    // ps_merge01
    case 592: return both(a1, b0);                                                    // ps_merge10
    case 624: return both(a1, b1);                                                    // ps_merge11
    case 1014: emit(F("{ uint32_t ea_ = (%s + %s) & ~31u; for (int k_ = 0; k_ < 32; k_ += 4) mem_write32(ea_ + k_, 0); }", ra0(A).c_str(), r(B).c_str())); return true; // dcbz_l
    }
    switch (XO6(i)) {
    case 6:  emit(F("psq_load(ctx, %u, %s + %s, %u, %u);", d, ra0(A).c_str(), r(B).c_str(), PSQX_W(i), PSQX_I(i))); return true;
    case 7:  emit(F("psq_store(ctx, %u, %s + %s, %u, %u);", d, ra0(A).c_str(), r(B).c_str(), PSQX_W(i), PSQX_I(i))); return true;
    case 38: emit(F("{ uint32_t ea_ = %s + %s; psq_load(ctx, %u, ea_, %u, %u); %s = ea_; }", r(A).c_str(), r(B).c_str(), d, PSQX_W(i), PSQX_I(i), r(A).c_str())); return true;
    case 39: emit(F("{ uint32_t ea_ = %s + %s; psq_store(ctx, %u, ea_, %u, %u); %s = ea_; }", r(A).c_str(), r(B).c_str(), d, PSQX_W(i), PSQX_I(i), r(A).c_str())); return true;
    }
    switch (XO5(i)) {
    case 10: return both(a0 + " + " + b1, c1);                                        // ps_sum0
    case 11: return both(c0, a0 + " + " + b1);                                        // ps_sum1
    case 12: return both(a0 + " * " + c0, a1 + " * " + c0);                           // ps_muls0
    case 13: return both(a0 + " * " + c1, a1 + " * " + c1);                           // ps_muls1
    case 14: return both(a0 + " * " + c0 + " + " + b0, a1 + " * " + c0 + " + " + b1); // ps_madds0
    case 15: return both(a0 + " * " + c1 + " + " + b0, a1 + " * " + c1 + " + " + b1); // ps_madds1
    case 18: return both(a0 + " / " + b0, a1 + " / " + b1);                           // ps_div
    case 20: return both(a0 + " - " + b0, a1 + " - " + b1);                           // ps_sub
    case 21: return both(a0 + " + " + b0, a1 + " + " + b1);                           // ps_add
    case 23: return both("(" + a0 + " >= 0.0 ? " + c0 + " : " + b0 + ")",
                         "(" + a1 + " >= 0.0 ? " + c1 + " : " + b1 + ")");            // ps_sel
    case 24: return both("1.0 / " + b0, "1.0 / " + b1);                               // ps_res
    case 25: return both(a0 + " * " + c0, a1 + " * " + c1);                           // ps_mul
    case 26: return both("1.0 / std::sqrt(" + b0 + ")", "1.0 / std::sqrt(" + b1 + ")"); // ps_rsqrte
    case 28: return both(a0 + " * " + c0 + " - " + b0, a1 + " * " + c1 + " - " + b1); // ps_msub
    case 29: return both(a0 + " * " + c0 + " + " + b0, a1 + " * " + c1 + " + " + b1); // ps_madd
    case 30: return both("-(" + a0 + " * " + c0 + " - " + b0 + ")", "-(" + a1 + " * " + c1 + " - " + b1 + ")"); // ps_nmsub
    case 31: return both("-(" + a0 + " * " + c0 + " + " + b0 + ")", "-(" + a1 + " * " + c1 + " + " + b1 + ")"); // ps_nmadd
    }
    return false;
}

} // namespace

bool generate(const gc::Dol& dol, const Analysis& an, const CodegenOptions& opt, CodegenStats& stats,
              std::string& error) {
    g_names = &an.names;
    std::error_code ec;
    fs::create_directories(opt.out_dir, ec);
    if (ec) { error = "cannot create " + opt.out_dir; return false; }

    const char* prelude =
        "// Generated by ssne-recomp. Do not edit; do not redistribute (derived from the game binary).\n"
        "#include \"recomp_funcs.h\"\n\n";

    {   // header with declarations
        std::ofstream h(fs::path(opt.out_dir) / "recomp_funcs.h");
        h << "#pragma once\n#include <ssne/ppc_context.h>\n#include <ssne/recomp_hooks.h>\n\n";
        for (auto& f : an.functions) h << "void " << fname(f.start) << "(PPCContext& ctx);\n";
    }
    size_t file_index = 0;
    for (size_t k = 0; k < an.functions.size(); k += opt.functions_per_file, file_index++) {
        std::ofstream o(fs::path(opt.out_dir) / F("funcs_%03zu.cpp", file_index));
        o << prelude;
        for (size_t j = k; j < an.functions.size() && j < k + opt.functions_per_file; j++) {
            Emitter e(dol, an, an.functions[j], stats);
            o << e.run(opt.emit_comments);
        }
        if (!o) { error = "write failed"; return false; }
    }
    {   // lookup table
        std::ofstream t(fs::path(opt.out_dir) / "func_table.cpp");
        t << prelude;
        t << "const SSNEFuncEntry g_ssne_funcs[] = {\n";
        for (auto& f : an.functions)
            t << F("    {0x%08Xu, %s, \"%s\"},\n", f.start, fname(f.start).c_str(), f.name.c_str());
        t << "};\n";
        t << F("const size_t g_ssne_func_count = %zu;\n", an.functions.size());
        t << F("const uint32_t g_ssne_entry = 0x%08Xu;\n", dol.entry);
        t << "const bool g_ssne_has_game_code = true;\n";
    }
    return true;
}

} // namespace recomp
