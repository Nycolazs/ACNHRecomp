// recomp/emit.py: the Emitter core. Each guest function becomes `void <prefix>_XXXXXXXX(nx::CPU& c)`; registers live in
// the context (regcache.cpp moves them into locals), branches inside the function are gotos, jump tables switches,
// calls direct when the target is known and through nx::call otherwise; an instruction the emitter does not handle
// becomes an nx::unimpl trap and is counted.
#include <cstdlib>
#include <cstring>

#include "emitter.h"
#include "regcache.h"

namespace rc {

const char* const B = "NXB";

const std::map<int, std::string>& CC() {
    static const std::map<int, std::string> m = {
        {ARM64_CC_EQ, "c.zf"}, {ARM64_CC_NE, "!c.zf"}, {ARM64_CC_HS, "c.cf"}, {ARM64_CC_LO, "!c.cf"},
        {ARM64_CC_MI, "c.nf"}, {ARM64_CC_PL, "!c.nf"}, {ARM64_CC_VS, "c.vf"}, {ARM64_CC_VC, "!c.vf"},
        {ARM64_CC_HI, "(c.cf && !c.zf)"}, {ARM64_CC_LS, "(!c.cf || c.zf)"}, {ARM64_CC_GE, "(c.nf == c.vf)"},
        {ARM64_CC_LT, "(c.nf != c.vf)"}, {ARM64_CC_GT, "(!c.zf && c.nf == c.vf)"},
        {ARM64_CC_LE, "(c.zf || c.nf != c.vf)"}, {ARM64_CC_AL, "true"}, {ARM64_CC_NV, "true"}};
    return m;
}

const std::map<int, std::pair<int, int>>& VAS() {
    static const std::map<int, std::pair<int, int>> m = {
        {ARM64_VAS_16B, {8, 16}}, {ARM64_VAS_8B, {8, 8}}, {ARM64_VAS_8H, {16, 8}}, {ARM64_VAS_4H, {16, 4}},
        {ARM64_VAS_4S, {32, 4}}, {ARM64_VAS_2S, {32, 2}}, {ARM64_VAS_2D, {64, 2}}, {ARM64_VAS_1D, {64, 1}},
        {ARM64_VAS_1B, {8, 1}}, {ARM64_VAS_1H, {16, 1}}, {ARM64_VAS_1S, {32, 1}}, {ARM64_VAS_1Q, {128, 1}},
        {ARM64_VAS_4B, {8, 4}}, {ARM64_VAS_2H, {16, 2}}};
    return m;
}

constexpr uint32_t SYSREG_TPIDR = 0xDE82, SYSREG_TPIDRRO = 0xDE83, SYSREG_FPCR = 0xDA20, SYSREG_FPSR = 0xDA21,
                   SYSREG_NZCV = 0xDA10;

// ---- Reg ----

Reg::Reg(const char* nm) {
    if (!nm) throw Unhandled{"register without a name"};  // an operand Capstone decodes but cannot name
    name = nm;
    if (name == "sp") kind = "x", n = 31;
    else if (name == "wsp") kind = "w", n = 31;
    else if (name == "xzr") kind = "zr64";
    else if (name == "wzr") kind = "zr32";
    else if (name == "fp") kind = "x", n = 29;
    else if (name == "lr") kind = "x", n = 30;
    else {
        bool digits = name.size() > 1;
        for (size_t k = 1; k < name.size(); k++) digits &= std::isdigit(uint8_t(name[k])) != 0;
        if (std::strchr("xwbhsdqv", name[0]) && name[0] && digits) {
            kind = std::string(1, name[0]);
            n = std::stoi(name.substr(1));
        } else {
            throw Unhandled{"register " + name};
        }
    }
}

int Reg::bits() const {
    if (kind == "x" || kind == "zr64") return 64;
    if (kind == "w" || kind == "zr32") return 32;
    if (kind == "b") return 8;
    if (kind == "h") return 16;
    if (kind == "s") return 32;
    if (kind == "d") return 64;
    return 128;  // q, v
}

std::string Reg::r() const {
    if (zr()) return bits() == 64 ? "0ull" : "0u";
    if (bits() == 64) return cat("c.x[", dec(n), "]");
    return cat("uint32_t(c.x[", dec(n), "])");
}

std::string Reg::w(const std::string& expr) const {
    if (zr()) return cat("(void)(", expr, ");");
    if (bits() == 64) return cat("c.x[", dec(n), "] = uint64_t(", expr, ");");
    return cat("c.x[", dec(n), "] = uint32_t(", expr, ");");
}

// ---- Emitter ----

Emitter::Emitter(const Analysis& a, std::string pfx) : an(a), m(*a.module), prefix(std::move(pfx)) {
    for (const Function& f : an.functions) entries.insert(f.start);
    if (const char* t = std::getenv("NX_TRACE_SP")) {  // debug: NX_TRACE_SP=0xADDR,...
        std::string s = t;
        size_t p = 0;
        while (p <= s.size()) {
            size_t q = s.find(',', p);
            std::string x = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (!x.empty()) trace_sp.insert(std::strtoull(x.c_str(), nullptr, 16));
            if (q == std::string::npos) break;
            p = q + 1;
        }
    }
}

std::string Emitter::shifted(const Insn& i, const Op& o, int bits) {
    if (o.type == ARM64_OP_IMM) {
        // Python: (imm << shift) & mask, with arbitrary precision
        uint64_t v = uint64_t(o.imm);
        if (o.shift.type == ARM64_SFT_LSL) v = o.shift.value >= 64 ? 0 : v << o.shift.value;
        uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
        return cat(u(bits), "(", hxu(v & mask), "ull)");
    }
    Reg r = regop(i, o);
    std::string v = r.r();
    if (o.ext) {
        const char* src;
        switch (o.ext) {
        case ARM64_EXT_UXTB: src = "uint8_t"; break;
        case ARM64_EXT_UXTH: src = "uint16_t"; break;
        case ARM64_EXT_UXTW: src = "uint32_t"; break;
        case ARM64_EXT_UXTX: src = "uint64_t"; break;
        case ARM64_EXT_SXTB: src = "int8_t"; break;
        case ARM64_EXT_SXTH: src = "int16_t"; break;
        case ARM64_EXT_SXTW: src = "int32_t"; break;
        case ARM64_EXT_SXTX: src = "int64_t"; break;
        default: throw std::runtime_error("shifted: extend");  // Python: KeyError
        }
        v = cat(u(bits), "(", starts_with(src, "int") ? s(bits) : u(bits), "(", src, "(", v, ")))");
        if (o.shift.type == ARM64_SFT_LSL && o.shift.value) v = cat(u(bits), "(", v, " << ", dec(o.shift.value), ")");
        return v;
    }
    if (o.shift.type && o.shift.value) {
        std::string n = dec(o.shift.value);
        int t = o.shift.type;
        if (t == ARM64_SFT_LSL) return cat(u(bits), "(", u(bits), "(", v, ") << ", n, ")");
        if (t == ARM64_SFT_LSR) return cat(u(bits), "(", u(bits), "(", v, ") >> ", n, ")");
        if (t == ARM64_SFT_ASR) return cat(u(bits), "(", s(bits), "(", v, ") >> ", n, ")");
        if (t == ARM64_SFT_ROR) return cat("nx::ror", dec(bits), "(", v, ", ", n, ")");
        throw Unhandled{"shift"};
    }
    return cat(u(bits), "(", v, ")");
}

std::string Emitter::cond(const Insn& i) const {
    auto it = CC().find(i.cc);
    if (it == CC().end()) throw std::runtime_error("cond: condition code");  // Python: KeyError
    return it->second;
}

std::string Emitter::goto_or_tail(const Function& f, uint64_t t) {
    if (f.start <= t && t < f.end) {
        labels.insert(t);
        return cat("goto L_", hX(t), ";");
    }
    if (entries.count(t)) return cat("{ ", fn(t), "(c); return; }");
    return cat("{ nx::call(c, ", B, " + ", hxu(t), "ull); return; }");
}

std::pair<std::string, std::string> Emitter::address(const Insn& i, const Op& memo, const Op* after) {
    Reg base(i.reg_name(memo.mem.base));
    std::string bexpr = cat("c.x[", dec(base.n), "]");
    std::string idx;
    if (memo.mem.index) {
        Reg ir(i.reg_name(memo.mem.index));
        std::string iv = ir.r();
        if (memo.ext == ARM64_EXT_SXTW) iv = cat("uint64_t(int64_t(int32_t(", iv, ")))");
        else if (memo.ext == ARM64_EXT_UXTW) iv = cat("uint64_t(uint32_t(", iv, "))");
        else if (memo.ext == ARM64_EXT_SXTX) iv = cat("uint64_t(", iv, ")");
        if (memo.shift.type == ARM64_SFT_LSL && memo.shift.value) iv = cat("(uint64_t(", iv, ") << ", dec(memo.shift.value), ")");
        idx = cat(" + uint64_t(", iv, ")");
    }
    int64_t disp = memo.mem.disp;
    std::string post;
    bool has_post = false;
    if (after) {
        if (after->type == ARM64_OP_IMM) post = cat(hxu(uint64_t(after->imm)), "ull"), has_post = true;
        else if (after->type == ARM64_OP_REG) post = Reg(i.reg_name(after->reg)).r(), has_post = true;
    }
    if (has_post)  // post-index (Capstone does not always flag writeback for structured ld/st)
        return {bexpr + idx, cat("c.x[", dec(base.n), "] += ", post, ";")};
    std::string a = disp ? cat(bexpr, idx, " + ", hxu(uint64_t(disp)), "ull") : bexpr + idx;
    if (i.writeback) return {a, cat("c.x[", dec(base.n), "] = a;")};
    return {a, ""};
}

std::string Emitter::function(const Function& f) {
    labels.clear();
    setjmp_sites.clear();
    std::vector<std::pair<uint64_t, std::string>> body;
    std::vector<uint8_t> code(m.image.begin() + long(f.start), m.image.begin() + long(f.end));
    bool last_term = false;
    Insn i;
    for (uint64_t a = f.start; a < f.end; a += 4) {
        if (!cs.one(code.data() + (a - f.start), a, i)) {
            uint32_t word;
            std::memcpy(&word, code.data() + (a - f.start), 4);
            body.push_back({a, cat("nx::unimpl(c, ", B, " + ", hxu(a), "ull, \"undecodable ", hx010(word), "\");")});
            last_term = true;
            continue;
        }
        std::string stmt;
        try {
            stmt = insn(f, i);
            handled++;
        } catch (const Unhandled& e) {
            unhandled[i.mnemonic]++;
            std::string txt = replace_all(i.mnemonic + " " + i.op_str, "\"", "'");
            stmt = cat("nx::unimpl(c, ", B, " + ", hxu(a), "ull, \"", txt, "\"); // ", e.what);
        }
        if (hooks.count(a)) stmt = cat("nx_hook(c, ", B, " + ", hxu(a), "ull); ", stmt);
        if (trace_sp.count(f.start)) {
            std::string first = i.op_str.substr(0, i.op_str.find(','));
            if (contains(first, "sp") || (contains(i.op_str, "[sp") && i.writeback))
                stmt += cat(" nx::trace_sp(c, ", B, " + ", hxu(a), "ull);");
        }
        body.push_back({a, cat(stmt, " // ", i.mnemonic, " ", i.op_str)});
        const std::string& mn = i.mnemonic;
        last_term = mn == "b" || mn == "br" || mn == "ret" || mn == "udf" || mn == "brk" || ends_with(stmt, "return; }");
    }
    std::vector<std::string> out = {cat("void ", fn(f.start), "(nx::CPU& NX_RESTRICT c) {")};
    for (uint64_t h : hooks)
        if (f.start <= h && h < f.end) {
            out.push_back("    void nx_hook(nx::CPU&, uint64_t);");
            break;
        }
    const auto& sj = setjmp_sites;
    if (!sj.empty()) {
        // functions that call setjmp run inside a try: a longjmp on one of their buffers resumes after the call
        out.push_back(cat("    nx::JmpState js_[", dec(int64_t(sj.size())), "]; int jr_ = -1;"));
        out.push_back("    for (;;) { try {");
        std::vector<std::string> cases;
        for (size_t k = 0; k < sj.size(); k++) cases.push_back(cat("case ", dec(int64_t(k)), ": goto L_", hX(sj[k]), ";"));
        out.push_back(cat("    switch (jr_) { ", join(cases, " "), " default: break; }"));
    }
    for (auto& [a, stmt] : body) {
        if (labels.count(a)) out.push_back(cat("L_", hX(a), ":;"));
        out.push_back("    " + stmt);
    }
    if (!last_term)  // falls off the end: continue into whatever follows
        out.push_back("    " + (entries.count(f.end) ? cat(fn(f.end), "(c);") : cat("nx::call(c, ", B, " + ", hxu(f.end), "ull);")));
    if (!sj.empty()) {
        out.push_back("    return;");
        out.push_back("    } catch (const nx::LongJmp& e_) {");
        out.push_back("        jr_ = -1;");
        out.push_back(cat("        for (int k = 0; k < ", dec(int64_t(sj.size())), "; k++) if (js_[k].buf == e_.buf) { jr_ = k; break; }"));
        out.push_back("        if (jr_ < 0) throw;");
        out.push_back("        js_[jr_].restore(c); c.x[0] = e_.val ? e_.val : 1;");
        out.push_back("    } }");
    }
    out.push_back("}");
    return join(regcache_apply(std::move(out), 0), "\n");
}

std::string Emitter::insn(const Function& f, const Insn& i) {
    const std::string& mn = i.mnemonic;
    std::string key = "i_" + replace_all(mn, ".", "_");
    const auto& h = handlers();
    auto it = h.find(key);
    if (it != h.end()) return (this->*(it->second))(f, i, i.operands);
    if (starts_with(mn, "b.")) return cat("if (", cond(i), ") ", goto_or_tail(f, target(i.operands.at(0))));
    if (lse_match(mn)) return lse(i, i.operands);
    throw Unhandled{"no handler"};
}

// ---- branches ----

std::string Emitter::i_b(const Function& f, const Insn&, const Ops& ops) { return goto_or_tail(f, target(ops.at(0))); }

std::string Emitter::i_bl(const Function&, const Insn& i, const Ops& ops) {
    uint64_t t = target(ops.at(0));
    auto is_setjmp = [](const std::map<uint64_t, std::string>& mp, uint64_t a) {
        auto it = mp.find(a);
        return it != mp.end() && (it->second == "setjmp" || it->second == "_setjmp" || it->second == "__setjmp");
    };
    if (is_setjmp(an.plt, t) || is_setjmp(an.plt_local, t) || is_setjmp(an.names, t)) {
        // setjmp: remember the callee-saved state; a later longjmp on this buffer lands at the next label
        size_t k = setjmp_sites.size();
        setjmp_sites.push_back(i.address + 4);
        labels.insert(i.address + 4);
        return cat("js_[", dec(int64_t(k)), "].save(c); c.x[0] = 0;");
    }
    std::string call = entries.count(t) ? cat(fn(t), "(c);") : cat("nx::call(c, ", B, " + ", hxu(t), "ull);");
    return cat("c.x[30] = ", B, " + ", hxu(i.address + 4), "ull; ", call);
}

std::string Emitter::i_blr(const Function&, const Insn& i, const Ops& ops) {
    Reg r = regop(i, ops.at(0));
    return cat("{ uint64_t t = ", r.r(), "; c.x[30] = ", B, " + ", hxu(i.address + 4), "ull; nx::call(c, t); }");
}

std::string Emitter::i_br(const Function& f, const Insn& i, const Ops& ops) {
    Reg r = regop(i, ops.at(0));
    auto jt = an.jump_tables.find(i.address);
    if (jt != an.jump_tables.end()) {
        // targets inside the function are labels; any other is a tail call (goto_or_tail)
        std::vector<std::string> cases;
        for (uint64_t t : jt->second) cases.push_back(cat("case ", B, " + ", hxu(t), "ull: ", goto_or_tail(f, t)));
        return cat("switch (", r.r(), ") { ", join(cases, " "), " default: nx::call(c, ", r.r(), "); return; }");
    }
    return cat("{ nx::call(c, ", r.r(), "); return; }");
}

std::string Emitter::i_ret(const Function&, const Insn& i, const Ops& ops) {
    if (!ops.empty() && regop(i, ops[0]).n != 30) return cat("{ nx::call(c, ", regop(i, ops[0]).r(), "); return; }");
    return "return;";
}

std::string Emitter::i_cbz(const Function& f, const Insn& i, const Ops& ops) {
    return cat("if (", regop(i, ops.at(0)).r(), " == 0) ", goto_or_tail(f, target(ops.at(1))));
}
std::string Emitter::i_cbnz(const Function& f, const Insn& i, const Ops& ops) {
    return cat("if (", regop(i, ops.at(0)).r(), " != 0) ", goto_or_tail(f, target(ops.at(1))));
}
std::string Emitter::i_tbz(const Function& f, const Insn& i, const Ops& ops) {
    return cat("if (((", regop(i, ops.at(0)).r(), " >> ", dec(ops.at(1).imm), ") & 1) == 0) ", goto_or_tail(f, target(ops.at(2))));
}
std::string Emitter::i_tbnz(const Function& f, const Insn& i, const Ops& ops) {
    return cat("if (((", regop(i, ops.at(0)).r(), " >> ", dec(ops.at(1).imm), ") & 1) != 0) ", goto_or_tail(f, target(ops.at(2))));
}

// ---- misc ----

std::string Emitter::i_nop(const Function&, const Insn&, const Ops&) { return ";"; }

std::string Emitter::i_dc(const Function&, const Insn& i, const Ops&) {
    if (contains(i.op_str, "zva")) {  // zero a 64-byte block (DCZID_EL0 reports 64)
        size_t c1 = i.op_str.find(',');
        std::string second = c1 == std::string::npos ? "" : i.op_str.substr(c1 + 1, i.op_str.find(',', c1 + 1) - c1 - 1);
        Reg r{std::string(strip(second)).c_str()};
        return cat("std::memset(reinterpret_cast<void*>(c.x[", dec(r.n), "] & ~63ull), 0, 64);");
    }
    return ";";  // cache maintenance
}

std::string Emitter::crc(const Insn& i, const Ops& ops, uint32_t poly, int size) {
    Reg d = regop(i, ops.at(0)), n = regop(i, ops.at(1)), mm = regop(i, ops.at(2));
    return d.w(cat("nx::crc32<", dec(size), ">(", n.r(), ", ", mm.r(), ", ", hxu(poly), "u)"));
}
std::string Emitter::i_crc32b(const Function&, const Insn& i, const Ops& o) { return crc(i, o, 0xEDB88320, 1); }
std::string Emitter::i_crc32h(const Function&, const Insn& i, const Ops& o) { return crc(i, o, 0xEDB88320, 2); }
std::string Emitter::i_crc32w(const Function&, const Insn& i, const Ops& o) { return crc(i, o, 0xEDB88320, 4); }
std::string Emitter::i_crc32x(const Function&, const Insn& i, const Ops& o) { return crc(i, o, 0xEDB88320, 8); }
std::string Emitter::i_crc32cb(const Function&, const Insn& i, const Ops& o) { return crc(i, o, 0x82F63B78, 1); }
std::string Emitter::i_crc32ch(const Function&, const Insn& i, const Ops& o) { return crc(i, o, 0x82F63B78, 2); }
std::string Emitter::i_crc32cw(const Function&, const Insn& i, const Ops& o) { return crc(i, o, 0x82F63B78, 4); }
std::string Emitter::i_crc32cx(const Function&, const Insn& i, const Ops& o) { return crc(i, o, 0x82F63B78, 8); }

std::string Emitter::i_ngc(const Function&, const Insn& i, const Ops& ops) {
    Reg d = regop(i, ops.at(0));
    return d.w(cat(u(d.bits()), "(", u(d.bits()), "(~", regop(i, ops.at(1)).r(), ") + c.cf)"));
}
std::string Emitter::i_ngcs(const Function&, const Insn& i, const Ops& ops) {
    Reg d = regop(i, ops.at(0));
    return d.w(cat("nx::adds", dec(d.bits()), "(c, 0, ", u(d.bits()), "(~", regop(i, ops.at(1)).r(), "), c.cf)"));
}
std::string Emitter::i_dmb(const Function&, const Insn&, const Ops&) { return "nx::barrier();"; }
std::string Emitter::i_clrex(const Function&, const Insn&, const Ops&) { return "nx::clrex(c);"; }
std::string Emitter::i_udf(const Function&, const Insn& i, const Ops&) {
    return cat("nx::unimpl(c, ", B, " + ", hxu(i.address), "ull, \"udf\");");
}
std::string Emitter::i_brk(const Function&, const Insn& i, const Ops&) {
    return cat("nx::unimpl(c, ", B, " + ", hxu(i.address), "ull, \"brk\");");
}
std::string Emitter::i_svc(const Function&, const Insn& i, const Ops& ops) {
    return cat("nx::svc(c, ", dec(ops.at(0).imm), ", ", B, " + ", hxu(i.address), "ull);");
}

uint32_t Emitter::sysreg(const Insn& i) const {
    uint32_t word;
    std::memcpy(&word, i.bytes, 4);
    return (word >> 5) & 0xFFFF;
}

std::string Emitter::i_mrs(const Function&, const Insn& i, const Ops& ops) {
    Reg d = regop(i, ops.at(0));
    uint32_t sr = sysreg(i);
    std::string v = sr == SYSREG_TPIDR ? "c.tpidr" : sr == SYSREG_TPIDRRO ? "c.tpidrro" : sr == SYSREG_FPCR ? "c.fpcr"
                  : sr == SYSREG_FPSR ? "c.fpsr" : sr == SYSREG_NZCV ? "nx::get_nzcv(c)" : cat("nx::mrs(c, ", hxu(sr), ")");
    return d.w(v);
}

std::string Emitter::i_msr(const Function&, const Insn& i, const Ops& ops) {
    uint32_t word;
    std::memcpy(&word, i.bytes, 4);
    if ((word >> 20) != 0xD51) throw Unhandled{"msr pstate"};
    uint32_t sr = sysreg(i);
    std::string v = Reg(i.reg_name(ops.back().reg)).r();
    if (sr == SYSREG_TPIDR) return cat("c.tpidr = ", v, ";");
    if (sr == SYSREG_FPCR) return cat("c.fpcr = ", v, ";");
    if (sr == SYSREG_FPSR) return cat("c.fpsr = ", v, ";");
    if (sr == SYSREG_NZCV) return cat("nx::set_nzcv(c, uint32_t(", v, " >> 28));");
    return cat("nx::msr(c, ", hxu(sr), ", ", v, ");");
}

// ---- loads / stores ----

Emitter::MemOps Emitter::mem_ops(const Ops& ops) const {
    MemOps r;
    for (const Op& o : ops) {
        if (o.type == ARM64_OP_MEM) r.mem = &o;
        else if (!r.mem) r.regs.push_back(o);
        else r.after = &o;
    }
    return r;
}

std::string Emitter::load_into(const Reg& r, const std::string& addr, int size, bool signed_) {
    if (r.gp()) {
        if (signed_) return r.w(cat(u(r.bits()), "(", s(r.bits()), "(nx::ld<", s(size * 8), ">(", addr, ")))"));
        return r.w(cat("nx::ld<", u(size * 8), ">(", addr, ")"));
    }
    if (size == 16) return cat("c.v[", dec(r.n), "] = nx::ld<nx::V128>(", addr, ");");
    return cat("{ nx::V128 t_ = nx::vzero(); t_.u", dec(size * 8), "[0] = nx::ld<", u(size * 8), ">(", addr, "); c.v[", dec(r.n), "] = t_; }");
}

std::string Emitter::store_from(const Reg& r, const std::string& addr, int size) {
    if (r.gp()) return cat("nx::st<", u(size * 8), ">(", addr, ", ", u(size * 8), "(", r.r(), "));");
    if (size == 16) return cat("nx::st<nx::V128>(", addr, ", c.v[", dec(r.n), "]);");
    return cat("nx::st<", u(size * 8), ">(", addr, ", c.v[", dec(r.n), "].u", dec(size * 8), "[0]);");
}

std::string Emitter::ldst(const Insn& i, const Ops& ops, int size, bool signed_, bool store, bool pair) {
    MemOps mo = mem_ops(ops);
    if (!mo.mem) {
        // literal: ldr x0, #addr (the address is an immediate operand, not a register)
        std::string addr = cat(B, " + ", hx(ops.back().imm), "ull");
        Reg r = regop(i, ops.at(0));
        int sz = size ? size : r.bits() / 8;
        return load_into(r, addr, sz, signed_);
    }
    std::vector<Reg> rs;
    for (const Op& o : mo.regs) rs.push_back(regop(i, o));
    int sz = size ? size : rs.at(0).bits() / 8;
    auto [a, wb] = address(i, *mo.mem, mo.after);
    std::vector<std::string> parts = {cat("uint64_t a = ", a, ";")};
    if (store) {
        for (size_t k = 0; k < rs.size(); k++) parts.push_back(store_from(rs[k], k ? cat("a + ", dec(int64_t(k) * sz)) : "a", sz));
        parts.push_back(wb);
    } else if (pair) {
        // load everything first, so base-register overlap cannot corrupt the address
        for (size_t k = 0; k < rs.size(); k++)
            parts.push_back(cat("auto v", dec(int64_t(k)), " = nx::ld<", sz == 16 ? "nx::V128" : (signed_ ? s(sz * 8) : u(sz * 8)),
                                ">(a + ", dec(int64_t(k) * sz), ");"));
        parts.push_back(wb);
        for (size_t k = 0; k < rs.size(); k++) {
            const Reg& r = rs[k];
            std::string vk = "v" + dec(int64_t(k));
            if (r.gp()) parts.push_back(r.w(signed_ ? cat(u(r.bits()), "(", s(r.bits()), "(", vk, "))") : vk));
            else if (sz == 16) parts.push_back(cat("c.v[", dec(r.n), "] = ", vk, ";"));
            else parts.push_back(cat("{ nx::V128 t_ = nx::vzero(); t_.u", dec(sz * 8), "[0] = ", vk, "; c.v[", dec(r.n), "] = t_; }"));
        }
    } else {
        parts.push_back(wb);
        parts.push_back(load_into(rs.at(0), "a", sz, signed_));
        if (!wb.empty() && rs[0].gp() && rs[0].n == Reg(i.reg_name(mo.mem->mem.base)).n) throw Unhandled{"load writeback to base"};
    }
    std::vector<std::string> keep;
    for (auto& p : parts)
        if (!p.empty()) keep.push_back(p);
    return "{ " + join(keep, " ") + " }";
}

std::string Emitter::i_ldr(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops); }
std::string Emitter::i_ldrb(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops, 1); }
std::string Emitter::i_ldrh(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops, 2); }
std::string Emitter::i_ldrsb(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops, 1, true); }
std::string Emitter::i_ldrsh(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops, 2, true); }
std::string Emitter::i_ldrsw(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops, 4, true); }
std::string Emitter::i_str(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops, 0, false, true); }
std::string Emitter::i_strb(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops, 1, false, true); }
std::string Emitter::i_strh(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops, 2, false, true); }
std::string Emitter::i_ldp(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops, 0, false, false, true); }
std::string Emitter::i_ldpsw(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops, 4, true, false, true); }
std::string Emitter::i_stp(const Function&, const Insn& i, const Ops& ops) { return ldst(i, ops, 0, false, true); }

// acquire / release / exclusive
std::string Emitter::acq(const Insn& i, const Ops& ops, int size) {
    MemOps mo = mem_ops(ops);
    Reg d = regop(i, mo.regs.at(0));
    int sz = size ? size : d.bits() / 8;
    return d.w(cat("nx::ld_acq<", u(sz * 8), ">(c.x[", dec(Reg(i.reg_name(mo.mem->mem.base)).n), "])"));
}
std::string Emitter::rel(const Insn& i, const Ops& ops, int size) {
    MemOps mo = mem_ops(ops);
    Reg r = regop(i, mo.regs.at(0));
    int sz = size ? size : r.bits() / 8;
    return cat("nx::st_rel<", u(sz * 8), ">(c.x[", dec(Reg(i.reg_name(mo.mem->mem.base)).n), "], ", u(sz * 8), "(", r.r(), "));");
}
std::string Emitter::i_ldar(const Function&, const Insn& i, const Ops& ops) { return acq(i, ops, 0); }
std::string Emitter::i_ldarb(const Function&, const Insn& i, const Ops& ops) { return acq(i, ops, 1); }
std::string Emitter::i_ldarh(const Function&, const Insn& i, const Ops& ops) { return acq(i, ops, 2); }
std::string Emitter::i_stlr(const Function&, const Insn& i, const Ops& ops) { return rel(i, ops, 0); }
std::string Emitter::i_stlrb(const Function&, const Insn& i, const Ops& ops) { return rel(i, ops, 1); }
std::string Emitter::i_stlrh(const Function&, const Insn& i, const Ops& ops) { return rel(i, ops, 2); }

std::string Emitter::ldx(const Insn& i, const Ops& ops, int size) {
    MemOps mo = mem_ops(ops);
    Reg d = regop(i, mo.regs.at(0));
    int sz = size ? size : d.bits() / 8;
    return d.w(cat("nx::ldxr<", u(sz * 8), ">(c, c.x[", dec(Reg(i.reg_name(mo.mem->mem.base)).n), "])"));
}
std::string Emitter::stx(const Insn& i, const Ops& ops, int size) {
    MemOps mo = mem_ops(ops);
    Reg st = regop(i, mo.regs.at(0)), r = regop(i, mo.regs.at(1));
    int sz = size ? size : r.bits() / 8;
    return st.w(cat("nx::stxr<", u(sz * 8), ">(c, c.x[", dec(Reg(i.reg_name(mo.mem->mem.base)).n), "], ", u(sz * 8), "(", r.r(), "))"));
}
std::string Emitter::i_ldaxr(const Function&, const Insn& i, const Ops& ops) { return ldx(i, ops, 0); }
std::string Emitter::i_ldaxrb(const Function&, const Insn& i, const Ops& ops) { return ldx(i, ops, 1); }
std::string Emitter::i_ldaxrh(const Function&, const Insn& i, const Ops& ops) { return ldx(i, ops, 2); }
std::string Emitter::i_stlxr(const Function&, const Insn& i, const Ops& ops) { return stx(i, ops, 0); }
std::string Emitter::i_stlxrb(const Function&, const Insn& i, const Ops& ops) { return stx(i, ops, 1); }
std::string Emitter::i_stlxrh(const Function&, const Insn& i, const Ops& ops) { return stx(i, ops, 2); }

void register_emit(std::map<std::string, Emitter::Handler>& h) {
    using E = Emitter;
    std::pair<const char*, E::Handler> list[] = {
        {"i_b", &E::i_b}, {"i_bl", &E::i_bl}, {"i_blr", &E::i_blr}, {"i_br", &E::i_br}, {"i_ret", &E::i_ret},
        {"i_cbz", &E::i_cbz}, {"i_cbnz", &E::i_cbnz}, {"i_tbz", &E::i_tbz}, {"i_tbnz", &E::i_tbnz},
        {"i_nop", &E::i_nop}, {"i_hint", &E::i_nop}, {"i_prfm", &E::i_nop}, {"i_prfum", &E::i_nop},
        {"i_yield", &E::i_nop}, {"i_wfe", &E::i_nop}, {"i_wfi", &E::i_nop}, {"i_sev", &E::i_nop}, {"i_sevl", &E::i_nop},
        {"i_dc", &E::i_dc}, {"i_ic", &E::i_nop}, {"i_autia1716", &E::i_nop}, {"i_autib1716", &E::i_nop},
        {"i_pacia1716", &E::i_nop}, {"i_pacib1716", &E::i_nop},
        {"i_crc32b", &E::i_crc32b}, {"i_crc32h", &E::i_crc32h}, {"i_crc32w", &E::i_crc32w}, {"i_crc32x", &E::i_crc32x},
        {"i_crc32cb", &E::i_crc32cb}, {"i_crc32ch", &E::i_crc32ch}, {"i_crc32cw", &E::i_crc32cw},
        {"i_crc32cx", &E::i_crc32cx}, {"i_ngc", &E::i_ngc}, {"i_ngcs", &E::i_ngcs}, {"i_dmb", &E::i_dmb},
        {"i_dsb", &E::i_dmb}, {"i_isb", &E::i_nop}, {"i_clrex", &E::i_clrex}, {"i_udf", &E::i_udf},
        {"i_brk", &E::i_brk}, {"i_svc", &E::i_svc}, {"i_mrs", &E::i_mrs}, {"i_msr", &E::i_msr},
        {"i_ldr", &E::i_ldr}, {"i_ldur", &E::i_ldr}, {"i_ldrb", &E::i_ldrb}, {"i_ldurb", &E::i_ldrb},
        {"i_ldrh", &E::i_ldrh}, {"i_ldurh", &E::i_ldrh}, {"i_ldrsb", &E::i_ldrsb}, {"i_ldursb", &E::i_ldrsb},
        {"i_ldrsh", &E::i_ldrsh}, {"i_ldursh", &E::i_ldrsh}, {"i_ldrsw", &E::i_ldrsw}, {"i_ldursw", &E::i_ldrsw},
        {"i_str", &E::i_str}, {"i_stur", &E::i_str}, {"i_strb", &E::i_strb}, {"i_sturb", &E::i_strb},
        {"i_strh", &E::i_strh}, {"i_sturh", &E::i_strh}, {"i_ldp", &E::i_ldp}, {"i_ldnp", &E::i_ldp},
        {"i_ldpsw", &E::i_ldpsw}, {"i_stp", &E::i_stp}, {"i_stnp", &E::i_stp},
        {"i_ldar", &E::i_ldar}, {"i_ldarb", &E::i_ldarb}, {"i_ldarh", &E::i_ldarh}, {"i_stlr", &E::i_stlr},
        {"i_stlrb", &E::i_stlrb}, {"i_stlrh", &E::i_stlrh}, {"i_ldaxr", &E::i_ldaxr}, {"i_ldxr", &E::i_ldaxr},
        {"i_ldaxrb", &E::i_ldaxrb}, {"i_ldxrb", &E::i_ldaxrb}, {"i_ldaxrh", &E::i_ldaxrh}, {"i_ldxrh", &E::i_ldaxrh},
        {"i_stlxr", &E::i_stlxr}, {"i_stxr", &E::i_stlxr}, {"i_stlxrb", &E::i_stlxrb}, {"i_stxrb", &E::i_stlxrb},
        {"i_stlxrh", &E::i_stlxrh}, {"i_stxrh", &E::i_stlxrh}};
    for (auto& [n, f] : list) h.emplace(n, f);
}

const std::map<std::string, Emitter::Handler>& Emitter::handlers() {
    static const std::map<std::string, Handler> h = [] {
        std::map<std::string, Handler> m;
        register_emit(m);     // Emitter's own methods come first in the MRO
        register_mixins(m);   // then Sdk20Ops, ExtOps, VecLdSt, IntOps, FpOps, NeonOps, NeonWideOps
        return m;
    }();
    return h;
}

}  // namespace rc
