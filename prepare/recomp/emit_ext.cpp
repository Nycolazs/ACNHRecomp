// Instructions first needed by SDK 16 (Super Mario Bros. Wonder): AdvSIMD pairwise, reductions and rounding forms,
// exclusive pairs, ARMv8.1 LSE atomics and the crypto extension. ACNHRecomp's recomp/emit_ext.py (ExtOps), ported.
#include "emitter.h"

namespace rc {

#define RC_DEF(name) \
    std::string Emitter::name([[maybe_unused]] const Function& f, [[maybe_unused]] const Insn& i, \
                              [[maybe_unused]] const Ops& ops)

namespace {

// LSE = re.compile(r"^(ldadd|ldclr|ldeor|ldset|ldsmax|ldsmin|ldumax|ldumin|stadd|stclr|steor|stset|stsmax|stsmin|"
//                  r"stumax|stumin|swp|casp|cas)(al|a|l)?(b|h)?$")
// The alternatives are tried in that order; the rest of the mnemonic splits one way at most into (al|a|l)?(b|h)?.
bool lse_parse(const std::string& mn, std::string& base, std::string& size) {
    static const char* const bases[] = {"ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin", "ldumax",
                                        "ldumin", "stadd", "stclr", "steor", "stset", "stsmax", "stsmin",
                                        "stumax", "stumin", "swp", "casp", "cas"};
    static const char* const orders[] = {"al", "a", "l", ""};
    static const char* const sizes[] = {"b", "h", ""};
    for (const char* b : bases) {
        if (!starts_with(mn, b)) continue;
        std::string_view rest = std::string_view(mn).substr(std::strlen(b));
        for (const char* o : orders)
            for (const char* z : sizes)
                if (rest == cat(o, z)) {
                    base = b, size = z;
                    return true;
                }
    }
    return false;
}

}  // namespace

// LSE atomics (ARMv8.1): ld<op>/st<op>, swp, cas, casp with a/l/al ordering and b/h sizes. Every ordering is emitted
// as seq_cst, which is at least as strong.
bool Emitter::lse_match(const std::string& mn) const {
    std::string base, size;
    return lse_parse(mn, base, size);
}

const std::map<std::string, std::string>& Emitter::LSE_OPS() {
    static const std::map<std::string, std::string> m = {
        {"add", "__atomic_fetch_add(p_, v_, __ATOMIC_SEQ_CST)"},
        {"clr", "__atomic_fetch_and(p_, T_(~v_), __ATOMIC_SEQ_CST)"},
        {"eor", "__atomic_fetch_xor(p_, v_, __ATOMIC_SEQ_CST)"},
        {"set", "__atomic_fetch_or(p_, v_, __ATOMIC_SEQ_CST)"},
        {"smax", "nx::amo_minmax<T_, true, true>(p_, v_)"},
        {"smin", "nx::amo_minmax<T_, true, false>(p_, v_)"},
        {"umax", "nx::amo_minmax<T_, false, true>(p_, v_)"},
        {"umin", "nx::amo_minmax<T_, false, false>(p_, v_)"}};
    return m;
}

std::string Emitter::lse(const Insn& i, const Ops& ops) {
    std::string base, size;  // m.group(1), m.group(3) ("" for None)
    if (!lse_parse(i.mnemonic, base, size)) throw Unhandled{"lse form"};
    MemOps mo = mem_ops(ops);
    std::vector<Reg> rs;
    for (const Op& o : mo.regs) rs.push_back(regop(i, o));
    std::string addr = cat("c.x[", dec(Reg(i.reg_name(need(mo.mem).mem.base)).n), "]");
    int dflt = rs.at(0).bits();  // .get(size, rs[0].bits) evaluates the default first
    int bits = size == "b" ? 8 : size == "h" ? 16 : dflt;
    std::string t = u(bits);
    if (base == "casp") {  // casp x0, x1, x2, x3, [x4]: compare (x0, x1), store (x2, x3), old value to (x0, x1)
        if (rs.size() != 4) throw std::runtime_error("casp: not 4 registers");  // Python's unpacking ValueError
        const Reg &s0 = rs[0], &s1 = rs[1], &t0 = rs[2], &t1 = rs[3];
        if (bits == 32)
            return cat("{ uint64_t e_ = uint64_t(uint32_t(", s0.r(), ")) | uint64_t(uint32_t(", s1.r(), ")) << 32; ",
                       "uint64_t n_ = uint64_t(uint32_t(", t0.r(), ")) | uint64_t(uint32_t(", t1.r(), ")) << 32; ",
                       "__atomic_compare_exchange_n(reinterpret_cast<uint64_t*>(", addr,
                       "), &e_, n_, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); ", s0.w("uint32_t(e_)"), " ",
                       s1.w("uint32_t(e_ >> 32)"), " }");
        return cat("{ unsigned __int128 e_ = (unsigned __int128)", s1.r(), " << 64 | ", s0.r(), "; ",
                   "unsigned __int128 n_ = (unsigned __int128)", t1.r(), " << 64 | ", t0.r(), "; ",
                   "__atomic_compare_exchange_n(reinterpret_cast<unsigned __int128*>(", addr,
                   "), &e_, n_, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); ", s0.w("uint64_t(e_)"), " ",
                   s1.w("uint64_t(e_ >> 64)"), " }");
    }
    if (base == "cas") {  // cas ws, wt, [xn]: compare ws, store wt, old value to ws
        if (rs.size() != 2) throw std::runtime_error("cas: not 2 registers");
        const Reg &rs_ = rs[0], &rt = rs[1];
        return cat("{ ", t, " e_ = ", t, "(", rs_.r(), "); __atomic_compare_exchange_n(reinterpret_cast<", t, "*>(",
                   addr, "), &e_, ", t, "(", rt.r(), "), false, ", "__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); ",
                   rs_.w("e_"), " }");
    }
    if (base == "swp") {
        if (rs.size() != 2) throw std::runtime_error("swp: not 2 registers");
        const Reg &rs_ = rs[0], &rt = rs[1];
        return cat("{ ", t, " o_ = __atomic_exchange_n(reinterpret_cast<", t, "*>(", addr, "), ", t, "(", rs_.r(),
                   "), __ATOMIC_SEQ_CST); ", rt.w("o_"), " }");
    }
    std::string op = base.substr(2);
    const Reg& rs_ = rs.at(0);
    const Reg* rt = rs.size() > 1 && starts_with(base, "ld") ? &rs[1] : nullptr;
    std::string stmt = cat("{ using T_ = ", t, "; T_* p_ = reinterpret_cast<T_*>(", addr, "); T_ v_ = T_(", rs_.r(),
                           "); T_ o_ = ", LSE_OPS().at(op), "; ");
    return stmt + (rt ? rt->w("o_") : std::string("(void)o_;")) + " }";
}

RC_DEF(i_xpaclri) { return ";"; }  // strip a pointer authentication code from x30: we never sign pointers

RC_DEF(i_ldaxp) {  // ldaxp x0, x1, [x2]
    MemOps mo = mem_ops(ops);
    Reg lo = regop(i, mo.regs.at(0)), hi = regop(i, mo.regs.at(1));
    std::string t = u(lo.bits());
    std::string base = dec(Reg(i.reg_name(need(mo.mem).mem.base)).n);
    return cat("{ ", t, " lo_, hi_; nx::ldxp<", t, ">(c, c.x[", base, "], lo_, hi_); ", lo.w("lo_"), " ", hi.w("hi_"),
               " }");
}

RC_DEF(i_stlxp) {  // stlxp w0, x1, x2, [x3]
    MemOps mo = mem_ops(ops);
    std::vector<Reg> r3;
    for (const Op& o : pyslice(mo.regs, 0, 3)) r3.push_back(regop(i, o));
    if (r3.size() != 3) throw std::runtime_error("stlxp: not 3 registers");
    const Reg &st = r3[0], &lo = r3[1], &hi = r3[2];
    std::string t = u(lo.bits());
    std::string base = dec(Reg(i.reg_name(need(mo.mem).mem.base)).n);
    return st.w(cat("nx::stxp<", t, ">(c, c.x[", base, "], ", t, "(", lo.r(), "), ", t, "(", hi.r(), "))"));
}

// SADALP / UADALP: pairwise long add, accumulated into the destination.
std::string Emitter::adalp(const Insn& i, const Ops& ops, bool signed_) {
    auto [dbits, n] = vinfo(ops.at(0));
    int sbits = dbits / 2;
    std::string kind = signed_ ? "s" : "u";
    std::string wide = signed_ ? "int64_t" : "uint64_t";
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1));
    // The 64-bit forms (.4h, .2s, .1d) clear the upper half of the destination, as every 64-bit vector op does.
    std::string hi = n * dbits == 64 ? " r.u64[1] = 0;" : "";
    std::string D = dec(dbits), S = dec(sbits);
    return cat("{ nx::V128 S = c.v[", dec(a.n), "]; nx::V128 r = c.v[", dec(d.n), "]; for (int k = 0; k < ", dec(n),
               "; k++) ", "r.", kind, D, "[k] = ", signed_ ? "int" : "uint", D, "_t(", wide, "(r.", kind, D,
               "[k]) + ", wide, "(S.", kind, S, "[2 * k]) + S.", kind, S, "[2 * k + 1]);", hi, " ", "c.v[", dec(d.n),
               "] = r; }");
}

RC_DEF(i_sadalp) { return adalp(i, ops, true); }

RC_DEF(i_uadalp) { return adalp(i, ops, false); }

RC_DEF(i_rshrn) {
    // (a + 2^(n-1)) >> n without the add overflowing 64-bit lanes
    return narrow(i, ops, "(({a} >> {n}) + (({a} >> ({n} - 1)) & 1))");
}

RC_DEF(i_sminv) {
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1));
    auto [bits, n] = vinfo(ops.at(1));
    std::string A = dec(a.n), Bt = dec(bits);
    return cat("{ int", Bt, "_t s_ = c.v[", A, "].s", Bt, "[0]; for (int k = 1; k < ", dec(n), "; k++) s_ = c.v[", A,
               "].s", Bt, "[k] < s_ ? c.v[", A, "].s", Bt, "[k] : s_; ", "nx::V128 r = nx::vzero(); r.s", Bt,
               "[0] = s_; c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_uminv) {
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1));
    auto [bits, n] = vinfo(ops.at(1));
    std::string A = dec(a.n), Bt = dec(bits);
    return cat("{ uint", Bt, "_t s_ = c.v[", A, "].u", Bt, "[0]; for (int k = 1; k < ", dec(n), "; k++) s_ = c.v[", A,
               "].u", Bt, "[k] < s_ ? c.v[", A, "].u", Bt, "[k] : s_; ", "nx::V128 r = nx::vzero(); r.u", Bt,
               "[0] = s_; c.v[", dec(d.n), "] = r; }");
}

// Pairwise op: the lower half of the result from adjacent pairs of the first source, the upper half from the second.
// Scalar form (fmaxp s0, v1.2s) reduces the two lanes of one source.
std::string Emitter::pairwise(const Insn& i, const Ops& ops, const std::string& expr, const std::string& kind) {
    Reg d = regop(i, ops.at(0));
    if (!ops[0].vas) {
        Reg a = regop(i, ops.at(1));
        int bits = VAS().at(ops[1].vas).first;
        std::string e = subst(expr, {{"a", cat("c.v[", dec(a.n), "].", kind, dec(bits), "[0]")},
                                     {"b", cat("c.v[", dec(a.n), "].", kind, dec(bits), "[1]")}});
        return cat("{ nx::V128 r = nx::vzero(); r.", kind, dec(bits), "[0] = ", e, "; c.v[", dec(d.n), "] = r; }");
    }
    auto [bits, n] = vinfo(ops[0]);
    Reg a = regop(i, ops.at(1)), b = regop(i, ops.at(2));
    std::string t = kind + dec(bits);
    std::string ea = subst(expr, {{"a", cat("A_.", t, "[2 * k]")}, {"b", cat("A_.", t, "[2 * k + 1]")}});
    std::string eb = subst(expr, {{"a", cat("B_.", t, "[2 * k]")}, {"b", cat("B_.", t, "[2 * k + 1]")}});
    return cat("{ nx::V128 A_ = c.v[", dec(a.n), "], B_ = c.v[", dec(b.n),
               "]; nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(n / 2), "; k++) ", "{ r.", t, "[k] = ", ea,
               "; r.", t, "[k + ", dec(n / 2), "] = ", eb, "; } c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_fmaxp) { return pairwise(i, ops, "nx::fmax_(a, b)", "f"); }

RC_DEF(i_fminp) { return pairwise(i, ops, "nx::fmin_(a, b)", "f"); }

RC_DEF(i_fmaxnmp) { return pairwise(i, ops, "nx::fmaxnm(a, b)", "f"); }

RC_DEF(i_fminnmp) { return pairwise(i, ops, "nx::fminnm(a, b)", "f"); }

RC_DEF(i_smaxp) { return pairwise(i, ops, "a > b ? a : b", "s"); }

RC_DEF(i_sminp) { return pairwise(i, ops, "a < b ? a : b", "s"); }

RC_DEF(i_umaxp) { return pairwise(i, ops, "a > b ? a : b", "u"); }

RC_DEF(i_uminp) { return pairwise(i, ops, "a < b ? a : b", "u"); }

// FACGE / FACGT: compare absolute values, all-ones mask per lane.
std::string Emitter::fac(const Insn& i, const Ops& ops, const std::string& op) {
    auto [bits, n] = vinfo(ops.at(0));
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1)), b = regop(i, ops.at(2));
    std::string Bt = dec(bits);
    return cat("{ nx::V128 S = c.v[", dec(a.n), "], T = c.v[", dec(b.n),
               "]; nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(n), "; k++) ", "r.u", Bt,
               "[k] = std::fabs(S.f", Bt, "[k]) ", op, " std::fabs(T.f", Bt, "[k]) ? uint", Bt,
               "_t(~0ull) : 0; c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_facge) { return fac(i, ops, ">="); }

RC_DEF(i_facgt) { return fac(i, ops, ">"); }

RC_DEF(i_sqrdmulh) {
    int bits = vinfo(ops.at(0)).first;
    // 2ab + round overflows int64 only for MIN * MIN (32-bit lanes), which saturates to MAX anyway.
    return v_bin(i, ops,
                 cat("(int64_t(a) * int64_t(b) == (int64_t(1) << ", dec(2 * bits - 2), ")) ? int", dec(bits),
                     "_t(~0ull >> ", dec(65 - bits), ") : ", "nx::sat<int", dec(bits),
                     "_t>((int64_t(a) * int64_t(b) * 2 + (int64_t(1) << ", dec(bits - 1), ")) >> ", dec(bits), ")"),
                 VOpt{.signed_ = true});
}

RC_DEF(i_shsub) { return v_bin(i, ops, "(int64_t(a) - int64_t(b)) >> 1", VOpt{.signed_ = true}); }

RC_DEF(i_uhsub) { return v_bin(i, ops, "(int64_t(a) - int64_t(b)) >> 1"); }

RC_DEF(i_srshl) {
    // Shift by the signed low byte of b; right shifts round: (a >> n) + bit n-1 of a (no overflow for 64-bit lanes).
    int bits = vinfo(ops.at(0)).first;
    std::string sh = cat("int8_t(S1.u", dec(bits), "[k])");
    return v_bin(i, ops,
                 cat(sh, " >= 0 ? (", sh, " >= ", dec(bits), " ? 0 : int64_t(uint64_t(int64_t(a)) << ", sh, ")) : ",
                     "(-", sh, " >= ", dec(bits), " ? 0 : (int64_t(a) >> -", sh, ") + ((int64_t(a) >> (-", sh,
                     " - 1)) & 1))"),
                 VOpt{.signed_ = true});
}

RC_DEF(i_uabdl) { return widen(i, ops, "{a} > {b} ? {a} - {b} : {b} - {a}", false); }

RC_DEF(i_sabdl) { return widen(i, ops, "{a} > {b} ? {a} - {b} : {b} - {a}", true); }

RC_DEF(i_umlsl) { return widen(i, ops, "{o} - {a} * {b}", false); }

RC_DEF(i_smlsl) { return widen(i, ops, "{o} - {a} * {b}", true); }

// crypto extension: helpers in nx/cpu.h
RC_DEF(ExtOps_i_pmull) {
    if (ops.at(0).vas != ARM64_VAS_1Q) throw Unhandled{"pmull on bytes"};
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1)), b = regop(i, ops.at(2));
    std::string lane = i.mnemonic == "pmull2" ? "1" : "0";
    return cat("c.v[", dec(d.n), "] = nx::clmul64(c.v[", dec(a.n), "].u64[", lane, "], c.v[", dec(b.n), "].u64[", lane,
               "]);");
}

std::string Emitter::crypto2(const Insn& i, const Ops& ops, const std::string& fn) {
    Reg d = regop(i, ops.at(0)), n = regop(i, ops.at(1));
    return cat("c.v[", dec(d.n), "] = nx::", fn, "(c.v[", dec(d.n), "], c.v[", dec(n.n), "]);");
}

std::string Emitter::crypto1(const Insn& i, const Ops& ops, const std::string& fn) {
    Reg d = regop(i, ops.at(0)), n = regop(i, ops.at(1));
    return cat("c.v[", dec(d.n), "] = nx::", fn, "(c.v[", dec(n.n), "]);");
}

std::string Emitter::crypto3(const Insn& i, const Ops& ops, const std::string& fn) {
    Reg d = regop(i, ops.at(0)), n = regop(i, ops.at(1)), m = regop(i, ops.at(2));
    return cat("c.v[", dec(d.n), "] = nx::", fn, "(c.v[", dec(d.n), "], c.v[", dec(n.n), "], c.v[", dec(m.n), "]);");
}

// The Python lambdas (i_aese = lambda self, f, i, ops: self.crypto2(i, ops, "aese"), ...)
RC_DEF(i_aese) { return crypto2(i, ops, "aese"); }
RC_DEF(i_aesd) { return crypto2(i, ops, "aesd"); }
RC_DEF(i_aesmc) { return crypto1(i, ops, "aesmc"); }
RC_DEF(i_aesimc) { return crypto1(i, ops, "aesimc"); }
RC_DEF(i_sha1su1) { return crypto2(i, ops, "sha1su1"); }
RC_DEF(i_sha256su0) { return crypto2(i, ops, "sha256su0"); }
RC_DEF(i_sha1su0) { return crypto3(i, ops, "sha1su0"); }
RC_DEF(i_sha256h) { return crypto3(i, ops, "sha256h"); }
RC_DEF(i_sha256h2) { return crypto3(i, ops, "sha256h2"); }
RC_DEF(i_sha256su1) { return crypto3(i, ops, "sha256su1"); }

std::string Emitter::sha1_rounds(const Insn& i, const Ops& ops, const std::string& fn) {  // sha1c q0, s1, v2.4s
    Reg d = regop(i, ops.at(0)), e = regop(i, ops.at(1)), w = regop(i, ops.at(2));
    return cat("c.v[", dec(d.n), "] = nx::", fn, "(c.v[", dec(d.n), "], c.v[", dec(e.n), "].u32[0], c.v[", dec(w.n),
               "]);");
}

RC_DEF(i_sha1c) { return sha1_rounds(i, ops, "sha1c"); }
RC_DEF(i_sha1p) { return sha1_rounds(i, ops, "sha1p"); }
RC_DEF(i_sha1m) { return sha1_rounds(i, ops, "sha1m"); }

RC_DEF(i_sha1h) {
    Reg d = regop(i, ops.at(0)), n = regop(i, ops.at(1));
    return cat("{ nx::V128 r = nx::vzero(); r.u32[0] = nx::sha1h(c.v[", dec(n.n), "].u32[0]); c.v[", dec(d.n),
               "] = r; }");
}

}  // namespace rc
