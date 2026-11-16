// AdvSIMD (NEON): widening and narrowing forms, permutes and reductions. ACNHRecomp's recomp/emit_neon_wide.py
// (NeonWideOps), ported.
#include <cmath>

#include "emitter.h"

namespace rc {

#define RC_DEF(name) \
    std::string Emitter::name([[maybe_unused]] const Function& f, [[maybe_unused]] const Insn& i, \
                              [[maybe_unused]] const Ops& ops)

// ---- widening / narrowing ----

// Destination lanes are 2x the source lanes. `2` variants read the upper half of narrow sources.
std::string Emitter::widen(const Insn& i, const Ops& ops, const std::string& expr, bool signed_, bool second_wide,
                           [[maybe_unused]] bool acc) {
    auto [dbits, n] = vinfo(ops.at(0));
    bool hi = ends_with(i.mnemonic, "2");
    int sbits = dbits / 2;
    std::string kind = signed_ ? "s" : "u";
    Reg d = regop(i, ops[0]);
    std::vector<std::string> srcs, pre;
    std::vector<Op> srcops =
        ops.size() > 2 && ops[2].type == ARM64_OP_REG ? pyslice(ops, 1, 3) : pyslice(ops, 1, 2);
    std::string W = signed_ ? "int64_t" : "uint64_t";
    for (size_t k = 0; k < srcops.size(); k++) {
        const Op& o = srcops[k];
        Reg r = regop(i, o);
        std::string K = dec(int64_t(k));
        pre.push_back(cat("nx::V128 S", K, " = c.v[", dec(r.n), "];"));
        bool wide = second_wide && k == 0;
        if (o.vector_index != -1) {
            srcs.push_back(cat(W, "(S", K, ".", kind, dec(sbits), "[", dec(o.vector_index), "])"));
        } else if (wide) {
            srcs.push_back(cat(W, "(S", K, ".", kind, dec(dbits), "[k])"));
        } else {
            std::string idx = hi ? cat("k + ", dec(n)) : "k";
            srcs.push_back(cat(W, "(S", K, ".", kind, dec(sbits), "[", idx, "])"));
        }
    }
    int64_t imm = ops.size() > 2 && ops[2].type == ARM64_OP_IMM ? ops[2].imm : 0;
    std::string e = pyformat(expr, {{"a", srcs.at(0)},
                                    {"b", srcs.size() > 1 ? srcs[1] : std::string("0")},
                                    {"n", dec(imm)},
                                    {"o", cat("O.", kind, dec(dbits), "[k]")}});
    return cat("{ ", join(pre, " "), " nx::V128 O = c.v[", dec(d.n), "]; nx::V128 r = nx::vzero(); for (int k = 0; k < ",
               dec(n), "; k++) ", "r.", kind, dec(dbits), "[k] = ", signed_ ? "int" : "uint", dec(dbits), "_t(", e,
               "); c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_ushll) { return widen(i, ops, "{a} << {n}", false); }

RC_DEF(i_sshll) { return widen(i, ops, "{a} << {n}", true); }

RC_DEF(i_uxtl) { return widen(i, ops, "{a}", false); }

RC_DEF(i_sxtl) { return widen(i, ops, "{a}", true); }

RC_DEF(i_shll) {
    int bits = vinfo(ops.at(0)).first / 2;
    return widen(i, ops, "{a} << " + dec(bits), false);
}

RC_DEF(i_uaddw) { return widen(i, ops, "{a} + {b}", false, true); }

RC_DEF(i_saddw) { return widen(i, ops, "{a} + {b}", true, true); }

RC_DEF(i_usubw) { return widen(i, ops, "{a} - {b}", false, true); }

RC_DEF(i_ssubw) { return widen(i, ops, "{a} - {b}", true, true); }

RC_DEF(i_uaddl) { return widen(i, ops, "{a} + {b}", false); }

RC_DEF(i_saddl) { return widen(i, ops, "{a} + {b}", true); }

RC_DEF(i_usubl) { return widen(i, ops, "{a} - {b}", false); }

RC_DEF(i_ssubl) { return widen(i, ops, "{a} - {b}", true); }

std::string Emitter::v_long(const Insn& i, const Ops& ops, [[maybe_unused]] const std::string& expr, bool signed_,
                            [[maybe_unused]] bool high) {
    return widen(i, ops, "{a} * {b}", signed_);
}

// The Python lambdas
RC_DEF(i_umull2) { return widen(i, ops, "{a} * {b}", false); }
RC_DEF(i_smull2) { return widen(i, ops, "{a} * {b}", true); }

RC_DEF(i_umlal) { return widen(i, ops, "{o} + {a} * {b}", false); }

RC_DEF(i_smlal) { return widen(i, ops, "{o} + {a} * {b}", true); }

std::string Emitter::pairwise_long(const Insn& i, const Ops& ops, bool signed_) {
    auto [dbits, n] = vinfo(ops.at(0));
    int sbits = dbits / 2;
    std::string kind = signed_ ? "s" : "u";
    Reg d = regop(i, ops[0]), a = regop(i, ops.at(1));
    std::string S = kind + dec(sbits);
    return cat("{ nx::V128 S = c.v[", dec(a.n), "]; nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(n), "; k++) ",
               "r.", kind, dec(dbits), "[k] = ", signed_ ? "int" : "uint", dec(dbits), "_t(",
               signed_ ? "int64_t" : "uint64_t", "(S.", S, "[2 * k]) + S.", S, "[2 * k + 1]); c.v[", dec(d.n),
               "] = r; }");
}

RC_DEF(i_saddlp) { return pairwise_long(i, ops, true); }

RC_DEF(i_uaddlp) { return pairwise_long(i, ops, false); }

// Destination lanes are half the source lanes; `2` variants write the upper half and keep the lower.
std::string Emitter::narrow(const Insn& i, const Ops& ops, const std::string& expr, const std::string& src_kind,
                            const std::string& dst_kind) {
    auto [dbits, n] = vinfo(ops.at(0));
    bool hi = ends_with(i.mnemonic, "2");
    int sbits = dbits * 2;
    Reg d = regop(i, ops[0]), a = regop(i, ops.at(1));
    int cnt = hi ? n / 2 : n;
    int base = hi ? n / 2 : 0;
    int64_t imm = ops.size() > 2 && ops[2].type == ARM64_OP_IMM ? ops[2].imm : 0;
    std::string sty = src_kind == "s" ? "int64_t" : "uint64_t";
    std::string e =
        pyformat(expr, {{"a", cat(sty, "(S.", src_kind, dec(sbits), "[k])")}, {"n", dec(imm)}, {"dbits", dec(dbits)}});
    std::string init = hi ? cat("c.v[", dec(d.n), "]") : "nx::vzero()";
    return cat("{ nx::V128 S = c.v[", dec(a.n), "]; nx::V128 r = ", init, "; for (int k = 0; k < ", dec(cnt), "; k++) ",
               "r.", dst_kind, dec(dbits), "[", dec(base), " + k] = ", dst_kind == "s" ? "int" : "uint", dec(dbits),
               "_t(", e, "); c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_xtn) { return narrow(i, ops, "{a}"); }

RC_DEF(i_shrn) { return narrow(i, ops, "{a} >> {n}"); }

RC_DEF(i_sqshrn) { return narrow(i, ops, "nx::sat<int{dbits}_t>({a} >> {n})", "s", "s"); }

RC_DEF(i_sqrshrn) {
    return narrow(i, ops, "nx::sat<int{dbits}_t>(({a} + (int64_t(1) << ({n} - 1))) >> {n})", "s", "s");
}

RC_DEF(i_sqadd) {
    int bits = vinfo(ops.at(0)).first;
    return v_bin(i, ops, cat("nx::sat<int", dec(bits), "_t>(int64_t(a) + int64_t(b))"), VOpt{.signed_ = true});
}

RC_DEF(i_sqsub) {
    int bits = vinfo(ops.at(0)).first;
    return v_bin(i, ops, cat("nx::sat<int", dec(bits), "_t>(int64_t(a) - int64_t(b))"), VOpt{.signed_ = true});
}

RC_DEF(i_uqadd) {
    std::string Bt = dec(vinfo(ops.at(0)).first);
    return v_bin(i, ops, cat("(uint64_t(a) + uint64_t(b)) > uint", Bt, "_t(~0ull) ? uint", Bt, "_t(~0ull) : a + b"));
}

RC_DEF(i_uqsub) { return v_bin(i, ops, "a > b ? a - b : 0"); }

RC_DEF(i_uaddlv) {
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1));
    auto [bits, n] = vinfo(ops[1]);
    return cat("{ uint64_t s_ = 0; for (int k = 0; k < ", dec(n), "; k++) s_ += c.v[", dec(a.n), "].u", dec(bits),
               "[k]; ", "nx::V128 r = nx::vzero(); r.u", dec(bits * 2), "[0] = uint", dec(bits * 2), "_t(s_); c.v[",
               dec(d.n), "] = r; }");
}

RC_DEF(i_smaxv) {
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1));
    auto [bits, n] = vinfo(ops[1]);
    std::string A = dec(a.n), Bt = dec(bits);
    return cat("{ int", Bt, "_t s_ = c.v[", A, "].s", Bt, "[0]; for (int k = 1; k < ", dec(n), "; k++) s_ = c.v[", A,
               "].s", Bt, "[k] > s_ ? c.v[", A, "].s", Bt, "[k] : s_; ", "nx::V128 r = nx::vzero(); r.s", Bt,
               "[0] = s_; c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_frsqrts) { return fbin(i, ops, "nx::frsqrts(a, b)"); }

RC_DEF(i_frecps) { return fbin(i, ops, "nx::frecps(a, b)"); }

RC_DEF(i_sqxtun) { return narrow(i, ops, "nx::sat<uint{dbits}_t>({a})", "s"); }

RC_DEF(i_sqxtn) { return narrow(i, ops, "nx::sat<int{dbits}_t>({a})", "s", "s"); }

RC_DEF(i_uqxtn) { return narrow(i, ops, "{a} > uint{dbits}_t(~0ull) ? uint{dbits}_t(~0ull) : {a}"); }

RC_DEF(i_sqshrun) { return narrow(i, ops, "nx::sat<uint{dbits}_t>({a} >> {n})", "s"); }

RC_DEF(i_sqrshrun) {
    return narrow(i, ops, "nx::sat<uint{dbits}_t>(({a} + (int64_t(1) << ({n} - 1))) >> {n})", "s");
}

RC_DEF(i_fcvtn) {
    auto [dbits, n] = vinfo(ops.at(0));
    bool hi = ends_with(i.mnemonic, "2");
    Reg d = regop(i, ops[0]), a = regop(i, ops.at(1));
    int cnt = hi ? n / 2 : n;
    int base = hi ? n / 2 : 0;
    std::string init = hi ? cat("c.v[", dec(d.n), "]") : "nx::vzero()";
    std::string body;
    if (dbits == 32)
        body = cat("r.f32[", dec(base), " + k] = float(S.f64[k]);");
    else
        body = cat("r.u16[", dec(base), " + k] = nx::f32_to_f16(S.f32[k]);");
    return cat("{ nx::V128 S = c.v[", dec(a.n), "]; nx::V128 r = ", init, "; for (int k = 0; k < ", dec(cnt), "; k++) ",
               body, " c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_fcvtl) {
    auto [dbits, n] = vinfo(ops.at(0));
    bool hi = ends_with(i.mnemonic, "2");
    Reg d = regop(i, ops[0]), a = regop(i, ops.at(1));
    std::string off = hi ? cat("k + ", dec(n)) : "k";
    std::string body = dbits == 64 ? cat("r.f64[k] = double(S.f32[", off, "]);")
                                   : cat("r.f32[k] = nx::f16_to_f32(S.u16[", off, "]);");
    return cat("{ nx::V128 S = c.v[", dec(a.n), "]; nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(n), "; k++) ",
               body, " c.v[", dec(d.n), "] = r; }");
}

std::string Emitter::v_cvt_to_int(const Insn& i, const Ops& ops, char rnd, bool signed_) {
    auto [bits, n] = vinfo(ops.at(0));
    Reg d = regop(i, ops[0]), a = regop(i, ops.at(1));
    std::string v = cat("S.f", dec(bits), "[k]");
    if (ops.size() > 2) v = cat("(", v, ") * ", pyfloat(std::ldexp(1.0, int(ops[2].imm))));  // float(2 ** imm)!r
    switch (rnd) {
        case 'z': v = cat("std::trunc(", v, ")"); break;
        case 'm': v = cat("std::floor(", v, ")"); break;
        case 'p': v = cat("std::ceil(", v, ")"); break;
        case 'a': v = cat("nx::round_away(", v, ")"); break;
        case 'n': v = cat("nx::round_even(", v, ")"); break;
        default: throw std::runtime_error("v_cvt_to_int: rounding");
    }
    std::string t = cat(signed_ ? "int" : "uint", dec(bits), "_t");
    return cat("{ nx::V128 S = c.v[", dec(a.n), "]; nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(n), "; k++) ",
               "r.", signed_ ? "s" : "u", dec(bits), "[k] = nx::fcvt_sat<", t, ">(", v, "); c.v[", dec(d.n),
               "] = r; }");
}

// ---- permutes ----

RC_DEF(i_ext) {
    auto [bits, n] = vinfo(ops.at(0));
    int nbytes = n * bits / 8;
    Reg d = regop(i, ops[0]), a = regop(i, ops.at(1)), b = regop(i, ops.at(2));
    int64_t imm = ops.at(3).imm;
    std::string N = dec(nbytes);
    return cat("{ uint8_t t_[32]; std::memcpy(t_, &c.v[", dec(a.n), "], ", N, "); std::memcpy(t_ + ", N, ", &c.v[",
               dec(b.n), "], ", N, "); ", "nx::V128 r = nx::vzero(); std::memcpy(&r, t_ + ", dec(imm), ", ", N,
               "); c.v[", dec(d.n), "] = r; }");
}

std::string Emitter::permute(const Insn& i, const Ops& ops, const std::string& body) {
    auto [bits, n] = vinfo(ops.at(0));
    Reg d = regop(i, ops[0]), a = regop(i, ops.at(1)), b = regop(i, ops.at(2));
    return cat("{ nx::V128 A_ = c.v[", dec(a.n), "], B_ = c.v[", dec(b.n), "]; nx::V128 r = nx::vzero(); ",
               "for (int k = 0; k < ", dec(n / 2), "; k++) { ",
               pyformat(body, {{"t", cat("u", dec(bits))}, {"h", dec(n / 2)}}), " } c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_zip1) { return permute(i, ops, "r.{t}[2 * k] = A_.{t}[k]; r.{t}[2 * k + 1] = B_.{t}[k];"); }

RC_DEF(i_zip2) { return permute(i, ops, "r.{t}[2 * k] = A_.{t}[k + {h}]; r.{t}[2 * k + 1] = B_.{t}[k + {h}];"); }

RC_DEF(i_uzp1) { return permute(i, ops, "r.{t}[k] = A_.{t}[2 * k]; r.{t}[k + {h}] = B_.{t}[2 * k];"); }

RC_DEF(i_uzp2) { return permute(i, ops, "r.{t}[k] = A_.{t}[2 * k + 1]; r.{t}[k + {h}] = B_.{t}[2 * k + 1];"); }

RC_DEF(i_trn1) { return permute(i, ops, "r.{t}[2 * k] = A_.{t}[2 * k]; r.{t}[2 * k + 1] = B_.{t}[2 * k];"); }

RC_DEF(i_trn2) {
    return permute(i, ops, "r.{t}[2 * k] = A_.{t}[2 * k + 1]; r.{t}[2 * k + 1] = B_.{t}[2 * k + 1];");
}

std::string Emitter::v_rev(const Insn& i, const Ops& ops, int container) {
    auto [bits, n] = vinfo(ops.at(0));
    Reg d = regop(i, ops[0]), a = regop(i, ops.at(1));
    std::string per = dec(container / bits);
    std::string Bt = dec(bits);
    return cat("{ nx::V128 S = c.v[", dec(a.n), "]; nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(n), "; k++) ",
               "r.u", Bt, "[k] = S.u", Bt, "[(k / ", per, ") * ", per, " + (", per, " - 1 - k % ", per, ")]; c.v[",
               dec(d.n), "] = r; }");
}

RC_DEF(i_tbl) {
    auto [bits, n] = vinfo(ops.at(0));
    Reg d = regop(i, ops[0]);
    std::vector<Reg> tables;
    for (const Op& o : pyslice(ops, 1, -1)) tables.push_back(regop(i, o));
    Reg idx = regop(i, ops.at(ops.size() - 1));
    std::vector<std::string> cps;
    for (size_t k = 0; k < tables.size(); k++)
        cps.push_back(cat("std::memcpy(t_ + ", dec(int64_t(16 * k)), ", &c.v[", dec(tables[k].n), "], 16);"));
    return cat("{ uint8_t t_[64]; ", join(cps, " "), " nx::V128 I_ = c.v[", dec(idx.n),
               "]; nx::V128 r = nx::vzero(); ", "for (int k = 0; k < ", dec(n), "; k++) r.u8[k] = I_.u8[k] < ",
               dec(int64_t(16 * tables.size())), " ? t_[I_.u8[k]] : 0; c.v[", dec(d.n), "] = r; }");
}

// ---- reductions ----

RC_DEF(i_addv) {
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1));
    auto [bits, n] = vinfo(ops[1]);
    return cat("{ uint64_t s_ = 0; for (int k = 0; k < ", dec(n), "; k++) s_ += c.v[", dec(a.n), "].u", dec(bits),
               "[k]; ", "nx::V128 r = nx::vzero(); r.u", dec(bits), "[0] = uint", dec(bits), "_t(s_); c.v[", dec(d.n),
               "] = r; }");
}

RC_DEF(i_umaxv) {
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1));
    auto [bits, n] = vinfo(ops[1]);
    std::string A = dec(a.n), Bt = dec(bits);
    return cat("{ uint", Bt, "_t s_ = 0; for (int k = 0; k < ", dec(n), "; k++) s_ = c.v[", A, "].u", Bt,
               "[k] > s_ ? c.v[", A, "].u", Bt, "[k] : s_; ", "nx::V128 r = nx::vzero(); r.u", Bt, "[0] = s_; c.v[",
               dec(d.n), "] = r; }");
}

RC_DEF(i_fmaxnmv) {
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1));
    return cat("{ nx::V128 S = c.v[", dec(a.n),
               "]; float s_ = nx::fmaxnm(nx::fmaxnm(S.f32[0], S.f32[1]), nx::fmaxnm(S.f32[2], S.f32[3])); ",
               "nx::V128 r = nx::vzero(); r.f32[0] = s_; c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_fminnmv) {
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1));
    return cat("{ nx::V128 S = c.v[", dec(a.n),
               "]; float s_ = nx::fminnm(nx::fminnm(S.f32[0], S.f32[1]), nx::fminnm(S.f32[2], S.f32[3])); ",
               "nx::V128 r = nx::vzero(); r.f32[0] = s_; c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_faddp) {
    Reg d = regop(i, ops.at(0));
    if (!ops[0].vas) {  // faddp s0, v1.2s
        Reg a = regop(i, ops.at(1));
        int bits = VAS().at(ops[1].vas).first;
        std::string A = dec(a.n), Bt = dec(bits);
        return fw(d, cat("c.v[", A, "].f", Bt, "[0] + c.v[", A, "].f", Bt, "[1]"));
    }
    auto [bits, n] = vinfo(ops[0]);
    Reg a = regop(i, ops.at(1)), b = regop(i, ops.at(2));
    std::string Bt = dec(bits), H = dec(n / 2);
    return cat("{ nx::V128 A_ = c.v[", dec(a.n), "], B_ = c.v[", dec(b.n),
               "]; nx::V128 r = nx::vzero(); for (int k = 0; k < ", H, "; k++) ", "{ r.f", Bt, "[k] = A_.f", Bt,
               "[2 * k] + A_.f", Bt, "[2 * k + 1]; r.f", Bt, "[k + ", H, "] = B_.f", Bt, "[2 * k] + B_.f", Bt,
               "[2 * k + 1]; } c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_addp) {
    Reg d = regop(i, ops.at(0));
    if (!ops[0].vas) {  // addp d0, v1.2d
        Reg a = regop(i, ops.at(1));
        return cat("{ nx::V128 r = nx::vzero(); r.u64[0] = c.v[", dec(a.n), "].u64[0] + c.v[", dec(a.n),
                   "].u64[1]; c.v[", dec(d.n), "] = r; }");
    }
    auto [bits, n] = vinfo(ops[0]);
    Reg a = regop(i, ops.at(1)), b = regop(i, ops.at(2));
    std::string Bt = dec(bits), H = dec(n / 2);
    return cat("{ nx::V128 A_ = c.v[", dec(a.n), "], B_ = c.v[", dec(b.n),
               "]; nx::V128 r = nx::vzero(); for (int k = 0; k < ", H, "; k++) ", "{ r.u", Bt, "[k] = A_.u", Bt,
               "[2 * k] + A_.u", Bt, "[2 * k + 1]; r.u", Bt, "[k + ", H, "] = B_.u", Bt, "[2 * k] + B_.u", Bt,
               "[2 * k + 1]; } c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_cnt) { return v_un(i, ops, "std::popcount(a)"); }

RC_DEF(i_not) { return v_un(i, ops, "~a", VOpt{.lanes = "bytes"}); }

RC_DEF(i_abs) { return v_un(i, ops, "a < 0 ? -a : a", VOpt{.signed_ = true}); }

RC_DEF(i_frecpe) { return fun(i, ops, "1.0f / a"); }

RC_DEF(i_frsqrte) { return fun(i, ops, "1.0f / std::sqrt(a)"); }

}  // namespace rc
