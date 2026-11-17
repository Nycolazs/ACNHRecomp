// AdvSIMD (NEON): lane helpers, element-wise arithmetic and logic, compares, shifts by immediate. ACNHRecomp's
// recomp/emit_neon.py (NeonOps), ported.
#include "emitter.h"

namespace rc {

#define RC_DEF(name) \
    std::string Emitter::name([[maybe_unused]] const Function& f, [[maybe_unused]] const Insn& i, \
                              [[maybe_unused]] const Ops& ops)

namespace {

// {("f", 32): "float", ("f", 64): "double"}.get((kind, bits), f"{'int' if kind == 's' else 'uint'}{bits}_t")
std::string lane_ty(const std::string& kind, int bits) {
    if (kind == "f" && bits == 32) return "float";
    if (kind == "f" && bits == 64) return "double";
    return cat(kind == "s" ? "int" : "uint", dec(bits), "_t");
}

}  // namespace

// ---- NEON ----

std::pair<int, int> Emitter::vinfo(const Op& o) {
    if (!o.vas) {
        // AdvSIMD scalar form (sqadd s0, s0, s1; sub d0, d0, d1): one lane of the register's size.
        const char* name = o.type == ARM64_OP_REG ? cs_reg_name(cs.handle(), o.reg) : nullptr;
        if (name && *name && std::strchr("bhsd", name[0]) && isdigits(name + 1) && o.vector_index == -1) {
            char k = name[0];
            return {k == 'b' ? 8 : k == 'h' ? 16 : k == 's' ? 32 : 64, 1};
        }
        throw Unhandled{"vector without arrangement"};
    }
    return VAS().at(o.vas);
}

std::string Emitter::lane_t(int bits, const std::string& kind) { return cat(kind, dec(bits)); }

// Lane k of a vector source operand; by-element operands broadcast their lane.
std::string Emitter::v_src(const Insn& i, const Op& o, int bits, const std::string& kind, const std::string& k) {
    Reg r = regop(i, o);
    if (o.vector_index != -1) return cat("c.v[", dec(r.n), "].", kind, dec(bits), "[", dec(o.vector_index), "]");
    return cat("c.v[", dec(r.n), "].", kind, dec(bits), "[", k, "]");
}

std::string Emitter::v_loop(const Insn& i, const Op& dst, int lanes, [[maybe_unused]] int bits,
                            const std::function<std::string()>& body_fn, bool keep_dst) {
    Reg d = regop(i, dst);
    std::string init = keep_dst ? cat("c.v[", dec(d.n), "]") : "nx::vzero()";
    return cat("{ nx::V128 r = ", init, "; for (int k = 0; k < ", dec(lanes), "; k++) { ", body_fn(), " } c.v[",
               dec(d.n), "] = r; }");
}

// Lanewise binary op; o (in expr) is the destination as an input.
std::string Emitter::v_bin(const Insn& i, const Ops& ops, const std::string& expr, const VOpt& opt) {
    auto [bits, n] = vinfo(ops.at(0));
    if (opt.lanes && std::string(opt.lanes) == "bytes") {
        n = n * bits / 8;
        bits = 8;
    }
    std::string kind = opt.fp ? "f" : (opt.signed_ ? "s" : "u");
    Reg d = regop(i, ops[0]);
    std::vector<std::string> pre;
    std::vector<Op> srcops = pyslice(ops, 1, 3);
    for (size_t k = 0; k < srcops.size(); k++) {
        Reg r = regop(i, srcops[k]);
        pre.push_back(cat("nx::V128 S", dec(int64_t(k)), " = c.v[", dec(r.n), "];"));
    }
    std::string t = kind + dec(bits);
    std::string a = cat("S0.", t, "[k]");
    std::string b;
    if (ops.at(2).vector_index != -1)
        b = cat("S1.", t, "[", dec(ops[2].vector_index), "]");
    else
        b = cat("S1.", t, "[k]");
    std::string o_ = cat("O.", t, "[k]");
    std::string e = subst(expr, {{"a", a}, {"b", b}, {"o", o_}});
    std::string ty = lane_ty(kind, bits);
    pre.push_back(cat("nx::V128 O = c.v[", dec(d.n), "];"));
    return cat("{ ", join(pre, " "), " nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(n), "; k++) r.", t,
               "[k] = ", ty, "(", e, "); ", "c.v[", dec(d.n), "] = r; }");
}

std::string Emitter::v_un(const Insn& i, const Ops& ops, const std::string& expr, const VOpt& opt) {
    auto [bits, n] = vinfo(ops.at(0));
    if (opt.lanes && std::string(opt.lanes) == "bytes") {
        n = n * bits / 8;
        bits = 8;
    }
    std::string kind = opt.fp || opt.fp_from_int ? "f" : (opt.signed_ ? "s" : "u");
    std::string skind = opt.fp_from_int ? std::string(opt.fp_from_int) : kind;
    Reg d = regop(i, ops[0]), m = regop(i, ops.at(1));
    std::string ty = lane_ty(kind, bits);
    std::string src = ops[1].vector_index == -1 ? cat("S.", skind, dec(bits), "[k]")
                                                : cat("S.", skind, dec(bits), "[", dec(ops[1].vector_index), "]");
    std::string e = subst(expr, {{"a", src}});
    return cat("{ nx::V128 S = c.v[", dec(m.n), "]; nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(n), "; k++) ",
               "r.", kind, dec(bits), "[k] = ", ty, "(", e, "); c.v[", dec(d.n), "] = r; }");
}

std::string Emitter::v_mov([[maybe_unused]] const Function& f, const Insn& i, const Ops& ops) {
    const Op &d = ops.at(0), &n = ops.at(1);
    Reg dr = regop(i, d);
    if (n.type == ARM64_OP_REG) {
        Reg nr = regop(i, n);
        if (d.vector_index != -1 && nr.gp()) {  // mov v0.s[1], w8  (ins)
            int bits = d.vas ? VAS().at(d.vas).first : 32;
            return cat("c.v[", dec(dr.n), "].u", dec(bits), "[", dec(d.vector_index), "] = uint", dec(bits), "_t(",
                       nr.r(), ");");
        }
        if (d.vector_index != -1 && n.vector_index != -1) {  // mov v0.s[1], v1.s[0]
            int bits = d.vas ? VAS().at(d.vas).first : 32;
            return cat("c.v[", dec(dr.n), "].u", dec(bits), "[", dec(d.vector_index), "] = c.v[", dec(nr.n), "].u",
                       dec(bits), "[", dec(n.vector_index), "];");
        }
        if (dr.gp() && n.vector_index != -1) {  // mov w0, v0.s[1]  (umov)
            int bits = n.vas ? VAS().at(n.vas).first : dr.bits();
            return dr.w(cat("c.v[", dec(nr.n), "].u", dec(bits), "[", dec(n.vector_index), "]"));
        }
        if (!dr.gp() && n.vector_index != -1) {  // mov s0, v1.s[1]  (dup scalar)
            int bits = n.vas ? VAS().at(n.vas).first : dr.bits();
            return cat("{ nx::V128 t_ = nx::vzero(); t_.u", dec(bits), "[0] = c.v[", dec(nr.n), "].u", dec(bits), "[",
                       dec(n.vector_index), "]; c.v[", dec(dr.n), "] = t_; }");
        }
        if (d.vas && n.vas) {  // mov v0.16b, v1.16b
            auto [bits, lanes] = VAS().at(d.vas);
            if (bits * lanes == 64)
                return cat("{ nx::V128 t_ = nx::vzero(); t_.u64[0] = c.v[", dec(nr.n), "].u64[0]; c.v[", dec(dr.n),
                           "] = t_; }");
            return cat("c.v[", dec(dr.n), "] = c.v[", dec(nr.n), "];");
        }
    }
    throw Unhandled{"mov vector form"};
}

RC_DEF(i_umov) { return v_mov(f, i, ops); }

RC_DEF(i_smov) {
    Reg d = regop(i, ops.at(0)), n = regop(i, ops.at(1));
    int bits = VAS().at(ops[1].vas).first;
    return d.w(cat(u(d.bits()), "(", s(d.bits()), "(c.v[", dec(n.n), "].s", dec(bits), "[", dec(ops[1].vector_index),
                   "]))"));
}

RC_DEF(i_ins) { return v_mov(f, i, ops); }

RC_DEF(i_dup) {
    auto [bits, lanes] = vinfo(ops.at(0));
    Reg d = regop(i, ops[0]), n = regop(i, ops.at(1));
    std::string Bt = dec(bits);
    std::string v;
    if (n.gp())
        v = cat("uint", Bt, "_t(", n.r(), ")");
    else
        v = cat("c.v[", dec(n.n), "].u", Bt, "[", dec(ops[1].vector_index), "]");
    return cat("{ uint", Bt, "_t e_ = ", v, "; nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(lanes),
               "; k++) r.u", Bt, "[k] = e_; c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_movi) {
    Reg d = regop(i, ops.at(0));
    uint64_t imm = uint64_t(ops.at(1).imm);  // printed masked to 64 bits or less: exact
    if (ops[1].shift.type == ARM64_SFT_LSL)
        imm = shl64(imm, ops[1].shift.value);
    else if (ops[1].shift.type == ARM64_SFT_MSL)
        imm = shl64(imm, ops[1].shift.value) | mask_bits(ops[1].shift.value);
    if (!ops[0].vas)  // movi d0, #imm  (64-bit scalar, byte mask)
        return cat("{ nx::V128 r = nx::vzero(); r.u64[0] = ", hxu(imm), "ull; c.v[", dec(d.n), "] = r; }");
    auto [bits, lanes] = vinfo(ops[0]);
    return cat("{ nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(lanes), "; k++) r.u", dec(bits), "[k] = ",
               "uint", dec(bits), "_t(", hxu(imm & mask_bits(bits)), "ull); c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_mvni) {
    Reg d = regop(i, ops.at(0));
    auto [bits, lanes] = vinfo(ops[0]);
    // printed unmasked ({imm:#x}): an 8-bit immediate shifted by at most 24, so int64 holds it exactly
    int64_t imm = ops.at(1).imm;
    if (ops[1].shift.type == ARM64_SFT_LSL)
        imm = int64_t(shl64(uint64_t(imm), ops[1].shift.value));
    else if (ops[1].shift.type == ARM64_SFT_MSL)
        imm = int64_t(shl64(uint64_t(imm), ops[1].shift.value) | mask_bits(ops[1].shift.value));
    return cat("{ nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(lanes), "; k++) r.u", dec(bits), "[k] = ",
               "uint", dec(bits), "_t(~", hx(imm), "ull); c.v[", dec(d.n), "] = r; }");
}

std::string Emitter::v_imm_logic(const Insn& i, const Ops& ops, const std::string& op) {
    Reg d = regop(i, ops.at(0));
    auto [bits, lanes] = vinfo(ops[0]);
    // printed unmasked, as in i_mvni
    int64_t imm = int64_t(shl64(uint64_t(ops.at(1).imm), ops[1].shift.type ? ops[1].shift.value : 0));
    std::string Bt = dec(bits);
    std::string e = op == "|" ? cat("r.u", Bt, "[k] | uint", Bt, "_t(", hx(imm), "ull)")
                              : cat("r.u", Bt, "[k] & uint", Bt, "_t(~", hx(imm), "ull)");
    std::string clear = bits * lanes == 128 ? "" : " r.u64[1] = 0;";
    return cat("{ nx::V128 r = c.v[", dec(d.n), "]; for (int k = 0; k < ", dec(lanes), "; k++) r.u", Bt, "[k] = ", e,
               ";", clear, " c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_fmla) {
    if (!ops.at(0).vas) {  // scalar by element: fmla s0, s1, v2.s[1]
        Reg d = regop(i, ops[0]), n = regop(i, ops.at(1));
        std::string N = fr(n);
        std::string E = velem(i, ops.at(2), "f");
        std::string D = fr(d);
        return fw(d, cat("std::fma(", N, ", ", E, ", ", D, ")"));
    }
    return v_bin(i, ops, "std::fma(a, b, o)", VOpt{.fp = true});
}

RC_DEF(i_fmls) {
    if (!ops.at(0).vas) {
        Reg d = regop(i, ops[0]), n = regop(i, ops.at(1));
        std::string N = fr(n);
        std::string E = velem(i, ops.at(2), "f");
        std::string D = fr(d);
        return fw(d, cat("std::fma(-(", N, "), ", E, ", ", D, ")"));
    }
    return v_bin(i, ops, "std::fma(-a, b, o)", VOpt{.fp = true});
}

RC_DEF(i_mla) { return v_bin(i, ops, "o + a * b"); }

RC_DEF(i_mls) { return v_bin(i, ops, "o - a * b"); }

RC_DEF(i_bsl) { return v_bin(i, ops, "(o & a) | (~o & b)", VOpt{.lanes = "bytes"}); }

RC_DEF(i_bit) { return v_bin(i, ops, "(o & ~b) | (a & b)", VOpt{.lanes = "bytes"}); }

RC_DEF(i_bif) { return v_bin(i, ops, "(o & b) | (a & ~b)", VOpt{.lanes = "bytes"}); }

RC_DEF(i_smax) { return v_bin(i, ops, "a > b ? a : b", VOpt{.signed_ = true}); }

RC_DEF(i_smin) { return v_bin(i, ops, "a < b ? a : b", VOpt{.signed_ = true}); }

RC_DEF(i_umax) { return v_bin(i, ops, "a > b ? a : b"); }

RC_DEF(i_umin) { return v_bin(i, ops, "a < b ? a : b"); }

RC_DEF(i_urhadd) { return v_bin(i, ops, "(uint64_t(a) + uint64_t(b) + 1) >> 1"); }

RC_DEF(i_srhadd) { return v_bin(i, ops, "(int64_t(a) + int64_t(b) + 1) >> 1", VOpt{.signed_ = true}); }

RC_DEF(i_uhadd) { return v_bin(i, ops, "(uint64_t(a) + uint64_t(b)) >> 1"); }

RC_DEF(i_shadd) { return v_bin(i, ops, "(int64_t(a) + int64_t(b)) >> 1", VOpt{.signed_ = true}); }

RC_DEF(i_sabd) { return v_bin(i, ops, "a > b ? a - b : b - a", VOpt{.signed_ = true}); }

RC_DEF(i_uabd) { return v_bin(i, ops, "a > b ? a - b : b - a"); }

RC_DEF(i_sqdmulh) {
    int bits = vinfo(ops.at(0)).first;
    return v_bin(i, ops, cat("nx::sat<int", dec(bits), "_t>((int64_t(a) * int64_t(b) * 2) >> ", dec(bits), ")"),
                 VOpt{.signed_ = true});
}

RC_DEF(i_sshl) {
    std::string Bt = dec(vinfo(ops.at(0)).first);
    std::string sh = cat("int8_t(S1.u", Bt, "[k])");
    return v_bin(i, ops,
                 cat(sh, " >= 0 ? (", sh, " >= ", Bt, " ? 0 : int64_t(a) << ", sh, ") : (", sh, " <= -", Bt,
                     " ? (a < 0 ? -1 : 0) : int64_t(a) >> -", sh, ")"),
                 VOpt{.signed_ = true});
}

RC_DEF(i_ushl) {
    std::string Bt = dec(vinfo(ops.at(0)).first);
    std::string sh = cat("int8_t(S1.u", Bt, "[k])");
    return v_bin(i, ops,
                 cat(sh, " >= 0 ? (", sh, " >= ", Bt, " ? 0 : uint64_t(a) << ", sh, ") : (", sh, " <= -", Bt,
                     " ? 0 : uint64_t(a) >> -", sh, ")"));
}

std::string Emitter::cmp_mask(const Insn& i, const Ops& ops, const std::string& op, const std::string& kind) {
    auto [bits, n] = vinfo(ops.at(0));
    Reg d = regop(i, ops[0]), a = regop(i, ops.at(1));
    std::string Bt = dec(bits);
    std::string b;
    if (ops.at(2).type == ARM64_OP_IMM || ops[2].type == ARM64_OP_FP)
        b = "0";
    else
        b = cat("c.v[", dec(regop(i, ops[2]).n), "].", kind, Bt, "[k]");
    int tn = ops[2].type == ARM64_OP_REG ? regop(i, ops[2]).n : a.n;
    std::string head = cat("{ nx::V128 S = c.v[", dec(a.n), "]; nx::V128 T = c.v[", dec(tn),
                           "]; nx::V128 r = nx::vzero(); ");
    int tn2 = ops[2].type == ARM64_OP_REG ? regop(i, ops[2]).n : a.n;
    return head + cat("for (int k = 0; k < ", dec(n), "; k++) r.u", Bt, "[k] = (S.", kind, Bt, "[k] ", op, " ",
                      replace_all(b, cat("c.v[", dec(tn2), "]"), "T"), ") ? uint", Bt, "_t(~0ull) : 0; c.v[",
                      dec(d.n), "] = r; }");
}

RC_DEF(i_cmeq) {
    if (!ops.at(0).vas) {  // cmeq d0, d1, d2 / cmeq d0, d1, #0
        Reg d = regop(i, ops[0]), n = regop(i, ops.at(1));
        std::string b = ops.at(2).type != ARM64_OP_REG ? std::string("0")
                                                       : cat("c.v[", dec(regop(i, ops[2]).n), "].u64[0]");
        return cat("{ uint64_t r_ = c.v[", dec(n.n), "].u64[0] == ", b, " ? ~0ull : 0; nx::V128 t_ = nx::vzero(); ",
                   "t_.u64[0] = r_; c.v[", dec(d.n), "] = t_; }");
    }
    return cmp_mask(i, ops, "==", "u");
}

RC_DEF(i_cmgt) { return cmp_mask(i, ops, ">", "s"); }

RC_DEF(i_cmge) { return cmp_mask(i, ops, ">=", "s"); }

RC_DEF(i_cmhi) { return cmp_mask(i, ops, ">", "u"); }

RC_DEF(i_cmhs) { return cmp_mask(i, ops, ">=", "u"); }

RC_DEF(i_cmlt) { return cmp_mask(i, ops, "<", "s"); }

RC_DEF(i_cmle) { return cmp_mask(i, ops, "<=", "s"); }

RC_DEF(i_cmtst) { return v_bin(i, ops, "(a & b) ? ~0ull : 0"); }

RC_DEF(i_fcmeq) { return cmp_mask(i, ops, "==", "f"); }

RC_DEF(i_fcmgt) { return cmp_mask(i, ops, ">", "f"); }

RC_DEF(i_fcmge) { return cmp_mask(i, ops, ">=", "f"); }

RC_DEF(i_fcmlt) { return cmp_mask(i, ops, "<", "f"); }

RC_DEF(i_fcmle) { return cmp_mask(i, ops, "<=", "f"); }

// shifts by immediate
std::string Emitter::v_shift_imm(const Insn& i, const Ops& ops, const std::string& expr, bool signed_, bool acc) {
    auto [bits, n] = vinfo(ops.at(0));
    Reg d = regop(i, ops[0]), a = regop(i, ops.at(1));
    int64_t sh = ops.at(2).imm;
    std::string kind = signed_ ? "s" : "u";
    std::string t = kind + dec(bits);
    std::string e = pyformat(expr, {{"a", signed_ ? cat("int64_t(S.", t, "[k])") : cat("uint64_t(S.", t, "[k])")},
                                    {"n", dec(sh)},
                                    {"bits", dec(bits)},
                                    {"o", cat("r.", t, "[k]")}});
    std::string init = acc ? cat("c.v[", dec(d.n), "]") : "nx::vzero()";
    return cat("{ nx::V128 S = c.v[", dec(a.n), "]; nx::V128 r = ", init, "; for (int k = 0; k < ", dec(n), "; k++) ",
               "r.", t, "[k] = ", signed_ ? "int" : "uint", dec(bits), "_t(", e, "); ") +
           (bits * n == 128 ? "" : "r.u64[1] = 0; ") + cat("c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_shl) { return v_shift_imm(i, ops, "{a} << {n}"); }

RC_DEF(i_ushr) { return v_shift_imm(i, ops, "({n} >= {bits}) ? 0 : ({a} >> {n})"); }

RC_DEF(i_sshr) { return v_shift_imm(i, ops, "{a} >> ({n} >= {bits} ? {bits} - 1 : {n})", true); }

RC_DEF(i_srshr) { return v_shift_imm(i, ops, "({a} + (int64_t(1) << ({n} - 1))) >> {n}", true); }

RC_DEF(i_urshr) { return v_shift_imm(i, ops, "({a} + (uint64_t(1) << ({n} - 1))) >> {n}"); }

RC_DEF(i_ssra) { return v_shift_imm(i, ops, "{o} + ({a} >> ({n} >= {bits} ? {bits} - 1 : {n}))", true, true); }

RC_DEF(i_usra) { return v_shift_imm(i, ops, "{o} + (({n} >= {bits}) ? 0 : ({a} >> {n}))", false, true); }

RC_DEF(i_srsra) { return v_shift_imm(i, ops, "{o} + (({a} + (int64_t(1) << ({n} - 1))) >> {n})", true, true); }

RC_DEF(i_ursra) { return v_shift_imm(i, ops, "{o} + (({a} + (uint64_t(1) << ({n} - 1))) >> {n})", false, true); }

}  // namespace rc
