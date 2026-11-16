// Scalar floating point: arithmetic, fused multiply-add, rounding, conversions, compares and moves. ACNHRecomp's
// recomp/emit_fp.py (FpOps), ported.
#include <cmath>

#include "emitter.h"

namespace rc {

#define RC_DEF(name) \
    std::string Emitter::name([[maybe_unused]] const Function& f, [[maybe_unused]] const Insn& i, \
                              [[maybe_unused]] const Ops& ops)

namespace {

// float(2 ** imm)!r
std::string pow2(int64_t e) { return pyfloat(std::ldexp(1.0, int(e))); }

}  // namespace

// ---- scalar floating point ----

// Read a scalar FP register as float/double.
std::string Emitter::fr(const Reg& r) {
    if (r.kind == "s") return cat("c.v[", dec(r.n), "].f32[0]");
    if (r.kind == "d") return cat("c.v[", dec(r.n), "].f64[0]");
    if (r.kind == "h") return cat("nx::f16_to_f32(c.v[", dec(r.n), "].u16[0])");
    throw Unhandled{"fp read " + r.name};
}

// Write a scalar FP register (upper bits cleared).
std::string Emitter::fw(const Reg& r, const std::string& expr) {
    if (r.kind == "h")
        return cat("{ uint16_t h_ = nx::f32_to_f16(float(", expr,
                   ")); nx::V128 t_ = nx::vzero(); t_.u16[0] = h_; c.v[", dec(r.n), "] = t_; }");
    std::string field = r.kind == "s" ? "f32" : r.kind == "d" ? "f64" : "";
    if (field.empty()) throw Unhandled{"fp write " + r.name};
    std::string ty = r.kind == "s" ? "float" : "double";
    return cat("{ ", ty, " r_ = ", ty, "(", expr, "); nx::V128 t_ = nx::vzero(); t_.", field, "[0] = r_; c.v[",
               dec(r.n), "] = t_; }");
}

bool Emitter::is_vec(const Insn& i, const Op& o) {
    return o.type == ARM64_OP_REG && (o.vas || o.vector_index != -1) && !regop(i, o).gp();
}

std::string Emitter::fbin(const Insn& i, const Ops& ops, const std::string& expr) {
    if (is_vec(i, ops.at(0))) return v_bin(i, ops, expr, VOpt{.fp = true});
    std::vector<Reg> r3 = regops(i, pyslice(ops, 0, 3), 3);
    Reg d = r3[0], n = r3[1], m = r3[2];
    std::string m_expr;
    if (is_vec(i, ops[2]))  // scalar by element: fmul s0, s1, v2.s[1]
        m_expr = velem(i, ops[2], "f", &d);
    else
        m_expr = fr(m);
    std::string a = cat("(", fr(n), ")");
    return fw(d, subst(expr, {{"a", a}, {"b", cat("(", m_expr, ")")}}));
}

// Element o.vector_index of a vector register. Without an arrangement the size comes from the register's own kind,
// or from `like` (the scalar destination) when Capstone names the whole vector (v2 for v2.s[1]).
std::string Emitter::velem(const Insn& i, const Op& o, const std::string& kind, const Reg* like) {
    Reg r = regop(i, o);
    int bits;
    if (o.vas) {
        bits = VAS().at(o.vas).first;
    } else {
        std::string k = (r.kind == "v" || r.kind == "q") && like != nullptr ? like->kind : r.kind;
        if (k != "b" && k != "h" && k != "s" && k != "d") throw Unhandled{"element size of " + r.name};
        bits = k == "b" ? 8 : k == "h" ? 16 : k == "s" ? 32 : 64;
    }
    return cat("c.v[", dec(r.n), "].", kind, dec(bits), "[", dec(o.vector_index), "]");
}

RC_DEF(i_fadd) { return fbin(i, ops, "a + b"); }

RC_DEF(i_fsub) { return fbin(i, ops, "a - b"); }

RC_DEF(i_fmul) { return fbin(i, ops, "a * b"); }

RC_DEF(i_fdiv) { return fbin(i, ops, "a / b"); }

RC_DEF(i_fnmul) { return fbin(i, ops, "-(a * b)"); }

RC_DEF(i_fabd) { return fbin(i, ops, "std::fabs(a - b)"); }

RC_DEF(i_fmaxnm) { return fbin(i, ops, "nx::fmaxnm(a, b)"); }

RC_DEF(i_fminnm) { return fbin(i, ops, "nx::fminnm(a, b)"); }

RC_DEF(i_fmax) { return fbin(i, ops, "nx::fmax_(a, b)"); }

RC_DEF(i_fmin) { return fbin(i, ops, "nx::fmin_(a, b)"); }

std::string Emitter::fma(const Insn& i, const Ops& ops, const std::string& kind) {
    std::vector<Reg> r4 = regops(i, pyslice(ops, 0, 4), 4);
    Reg d = r4[0], n = r4[1], m = r4[2], a = r4[3];
    std::string N = fr(n);
    std::string M = fr(m);
    std::string Aa = fr(a);
    std::string e;
    if (kind == "fmadd") e = cat("std::fma(", N, ", ", M, ", ", Aa, ")");
    else if (kind == "fmsub") e = cat("std::fma(-(", N, "), ", M, ", ", Aa, ")");
    else if (kind == "fnmadd") e = cat("std::fma(-(", N, "), ", M, ", -(", Aa, "))");
    else if (kind == "fnmsub") e = cat("std::fma(", N, ", ", M, ", -(", Aa, "))");
    else throw std::runtime_error("fma: " + kind);
    return fw(d, e);
}

RC_DEF(i_fmadd) { return fma(i, ops, "fmadd"); }

RC_DEF(i_fmsub) { return fma(i, ops, "fmsub"); }

RC_DEF(i_fnmadd) { return fma(i, ops, "fnmadd"); }

RC_DEF(i_fnmsub) { return fma(i, ops, "fnmsub"); }

std::string Emitter::fun(const Insn& i, const Ops& ops, const std::string& expr) {
    if (is_vec(i, ops.at(0))) return v_un(i, ops, expr, VOpt{.fp = true});
    Reg d = regop(i, ops[0]), n = regop(i, ops.at(1));
    return fw(d, subst(expr, {{"a", cat("(", fr(n), ")")}}));
}

RC_DEF(i_fneg) { return fun(i, ops, "-a"); }

RC_DEF(i_fabs) { return fun(i, ops, "std::fabs(a)"); }

RC_DEF(i_fsqrt) { return fun(i, ops, "std::sqrt(a)"); }

RC_DEF(i_frintm) { return fun(i, ops, "std::floor(a)"); }

RC_DEF(i_frintp) { return fun(i, ops, "std::ceil(a)"); }

RC_DEF(i_frintz) { return fun(i, ops, "std::trunc(a)"); }

RC_DEF(i_frinta) { return fun(i, ops, "nx::round_away(a)"); }

RC_DEF(i_frintn) { return fun(i, ops, "nx::round_even(a)"); }

RC_DEF(i_fcmp) {
    Reg n = regop(i, ops.at(0));
    std::string b = ops.at(1).type == ARM64_OP_FP || ops[1].type == ARM64_OP_IMM ? std::string("0.0")
                                                                                 : fr(regop(i, ops[1]));
    std::string ty = n.kind == "s" || n.kind == "h" ? "float" : "double";
    std::string N = fr(n);
    return cat("nx::fcmp<", ty, ">(c, ", N, ", ", ty, "(", b, "));");
}

RC_DEF(i_fccmp) {
    Reg n = regop(i, ops.at(0)), m = regop(i, ops.at(1));
    std::string ty = n.kind == "s" || n.kind == "h" ? "float" : "double";
    std::string c = cond(i);
    std::string N = fr(n);
    std::string M = fr(m);
    return cat("if (", c, ") nx::fcmp<", ty, ">(c, ", N, ", ", M, "); else nx::set_nzcv(c, ", dec(ops.at(2).imm), ");");
}

RC_DEF(i_fcsel) {
    std::vector<Reg> r3 = regops(i, pyslice(ops, 0, 3), 3);
    Reg d = r3[0], n = r3[1], m = r3[2];
    std::string c = cond(i);
    std::string N = fr(n);
    std::string M = fr(m);
    return fw(d, cat("(", c, ") ? ", N, " : ", M));
}

RC_DEF(i_fmov) {
    Reg d = regop(i, ops.at(0));
    const Op& o = ops.at(1);
    if (is_vec(i, ops[0]) && ops[0].vector_index != -1)  // fmov v0.d[1], x1
        return cat("c.v[", dec(d.n), "].u64[", dec(ops[0].vector_index), "] = ", regop(i, o).r(), ";");
    if (o.type == ARM64_OP_FP) {
        if (is_vec(i, ops[0])) {
            auto [bits, lanes] = VAS().at(ops[0].vas);
            std::string t = bits == 32 ? "f32" : "f64";
            return cat("{ nx::V128 r = nx::vzero(); for (int k = 0; k < ", dec(lanes), "; k++) r.", t,
                       "[k] = ", pyfloat(o.fp), "; c.v[", dec(d.n), "] = r; }");
        }
        return fw(d, pyfloat(o.fp));
    }
    if (o.type == ARM64_OP_REG) {
        Reg n = regop(i, o);
        if (is_vec(i, o) && o.vector_index != -1)  // fmov x0, v1.d[1]
            return d.w(cat("c.v[", dec(n.n), "].u64[", dec(o.vector_index), "]"));
        if (d.gp() && !n.gp()) return d.w(cat("c.v[", dec(n.n), "].u", dec(d.bits()), "[0]"));
        if (!d.gp() && n.gp())
            return cat("{ nx::V128 t_ = nx::vzero(); t_.u", dec(d.bits()), "[0] = ", u(d.bits()), "(", n.r(),
                       "); c.v[", dec(d.n), "] = t_; }");
        return cat("{ nx::V128 t_ = nx::vzero(); t_.u", dec(d.bits()), "[0] = c.v[", dec(n.n), "].u", dec(d.bits()),
                   "[0]; c.v[", dec(d.n), "] = t_; }");
    }
    if (o.type == ARM64_OP_IMM) return fw(d, "0.0");  // fmov s0, #0? capstone sometimes gives imm
    throw Unhandled{"fmov form"};
}

RC_DEF(i_fcvt) {
    Reg d = regop(i, ops.at(0)), n = regop(i, ops.at(1));
    return fw(d, fr(n));
}

std::string Emitter::cvt_to_int(const Insn& i, const Ops& ops, char rnd, bool signed_) {
    if (is_vec(i, ops.at(0))) return v_cvt_to_int(i, ops, rnd, signed_);
    Reg d = regop(i, ops[0]), n = regop(i, ops.at(1));
    if (!d.gp() && d.kind != "s" && d.kind != "d") throw Unhandled{"fcvt to simd scalar"};
    std::string v = fr(n);
    if (ops.size() > 2) v = cat("(", v, ") * ", pow2(ops[2].imm));  // fixed point
    switch (rnd) {
        case 'z': v = cat("std::trunc(", v, ")"); break;
        case 'm': v = cat("std::floor(", v, ")"); break;
        case 'p': v = cat("std::ceil(", v, ")"); break;
        case 'a': v = cat("nx::round_away(", v, ")"); break;
        case 'n': v = cat("nx::round_even(", v, ")"); break;
        default: throw std::runtime_error("cvt_to_int: rounding");
    }
    std::string t = signed_ ? s(d.bits()) : u(d.bits());
    if (!d.gp())  // fcvtzs s0, s1: the integer stays in the SIMD register
        return cat("{ nx::V128 t_ = nx::vzero(); t_.u", dec(d.bits()), "[0] = ", u(d.bits()), "(nx::fcvt_sat<", t,
                   ">(", v, ")); ", "c.v[", dec(d.n), "] = t_; }");
    return d.w(cat(u(d.bits()), "(nx::fcvt_sat<", t, ">(", v, "))"));
}

RC_DEF(i_fcvtzs) { return cvt_to_int(i, ops, 'z', true); }

RC_DEF(i_fcvtzu) { return cvt_to_int(i, ops, 'z', false); }

RC_DEF(i_fcvtms) { return cvt_to_int(i, ops, 'm', true); }

RC_DEF(i_fcvtmu) { return cvt_to_int(i, ops, 'm', false); }

RC_DEF(i_fcvtps) { return cvt_to_int(i, ops, 'p', true); }

RC_DEF(i_fcvtpu) { return cvt_to_int(i, ops, 'p', false); }

RC_DEF(i_fcvtas) { return cvt_to_int(i, ops, 'a', true); }

RC_DEF(i_fcvtau) { return cvt_to_int(i, ops, 'a', false); }

RC_DEF(i_fcvtns) { return cvt_to_int(i, ops, 'n', true); }

RC_DEF(i_fcvtnu) { return cvt_to_int(i, ops, 'n', false); }

std::string Emitter::cvt_from_int(const Insn& i, const Ops& ops, bool signed_) {
    if (is_vec(i, ops.at(0))) {
        const char* fi = signed_ ? "s" : "u";
        if (ops.size() > 2 && ops[2].type == ARM64_OP_IMM) {  // scvtf v0.4s, v0.4s, #fbits
            std::string ty = vinfo(ops[0]).first == 32 ? "float" : "double";
            return v_un(i, ops, cat(ty, "(a) / ", ty, "(", pow2(ops[2].imm), ")"), VOpt{.fp_from_int = fi});
        }
        return v_un(i, ops, "a", VOpt{.fp_from_int = fi});
    }
    Reg d = regop(i, ops[0]), n = regop(i, ops.at(1));
    std::string ty = d.kind == "s" || d.kind == "h" ? "float" : "double";
    std::string v;
    if (n.gp())
        v = cat(signed_ ? s(n.bits()) : u(n.bits()), "(", n.r(), ")");
    else  // scvtf s0, s1 (integer in simd register)
        v = cat("c.v[", dec(n.n), "].", signed_ ? "s" : "u", dec(n.bits()), "[0]");
    v = cat(ty, "(", v, ")");
    if (ops.size() > 2) v = cat(v, " / ", ty, "(", pow2(ops[2].imm), ")");
    return fw(d, v);
}

RC_DEF(i_scvtf) { return cvt_from_int(i, ops, true); }

RC_DEF(i_ucvtf) { return cvt_from_int(i, ops, false); }

}  // namespace rc
