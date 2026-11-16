// Integer data processing: moves, arithmetic and logic, shifts, bitfields, extends, conditional selects and compares,
// multiplies and divides, bit and byte reversal. ACNHRecomp's recomp/emit_int.py (IntOps), ported.
// Python's unbounded ints are masked to the register width wherever they are printed, so uint64 arithmetic (mask_bits,
// shl64) gives the same digits.
#include "emitter.h"

namespace rc {

#define RC_DEF(name) \
    std::string Emitter::name([[maybe_unused]] const Function& f, [[maybe_unused]] const Insn& i, \
                              [[maybe_unused]] const Ops& ops)

// ---- integer data processing ----

RC_DEF(i_mov) {
    Reg d = regop(i, ops.at(0));
    if (!d.gp() || (ops.at(1).type == ARM64_OP_REG && !regop(i, ops[1]).gp())) return v_mov(f, i, ops);
    if (ops[1].type == ARM64_OP_IMM) return d.w(hxu(uint64_t(ops[1].imm) & mask_bits(d.bits())) + "ull");
    return d.w(regop(i, ops[1]).r());
}

RC_DEF(i_movz) {
    Reg d = regop(i, ops.at(0));
    uint64_t v = shl64(uint64_t(ops.at(1).imm), ops[1].shift.type ? ops[1].shift.value : 0);
    return d.w(hxu(v & mask_bits(d.bits())) + "ull");
}

RC_DEF(i_movn) {
    Reg d = regop(i, ops.at(0));
    uint64_t v = ~shl64(uint64_t(ops.at(1).imm), ops[1].shift.type ? ops[1].shift.value : 0);
    return d.w(hxu(v & mask_bits(d.bits())) + "ull");
}

RC_DEF(i_movk) {
    Reg d = regop(i, ops.at(0));
    int64_t sh = ops.at(1).shift.type ? ops[1].shift.value : 0;
    uint64_t mask = shl64(0xFFFF, sh) & mask_bits(d.bits());
    // (imm & 0xFFFF) << sh is not masked in Python: sh <= 48, so it fits
    return d.w(cat("(", d.r(), " & ", hxu(~mask & mask_bits(d.bits())), "ull) | ",
                   hxu(shl64(uint64_t(ops[1].imm) & 0xFFFF, sh)), "ull"));
}

RC_DEF(i_adrp) { return regop(i, ops.at(0)).w(cat(B, " + ", hx(ops.at(1).imm), "ull")); }

std::string Emitter::arith(const Insn& i, const Ops& ops, const std::string& op, bool setflags) {
    Reg d = regop(i, ops.at(0));
    int bits = d.bits() == 64 ? 64 : 32;
    Reg n = regop(i, ops.at(1));
    std::string a = n.r();
    std::string b = shifted(i, ops.at(2), bits);
    if (setflags) {
        std::string fn = op == "+" ? "adds" : "subs";
        return d.w(op == "-" ? cat("nx::", fn, dec(bits), "(c, ", a, ", ", b, ")")
                             : cat("nx::adds", dec(bits), "(c, ", a, ", ", b, ", 0)"));
    }
    return d.w(cat(u(bits), "(", a, ") ", op, " ", b));
}

RC_DEF(i_add) {
    if (!regop(i, ops.at(0)).gp()) return v_bin(i, ops, "a + b");
    return arith(i, ops, "+", false);
}

RC_DEF(i_sub) {
    if (!regop(i, ops.at(0)).gp()) return v_bin(i, ops, "a - b");
    return arith(i, ops, "-", false);
}

RC_DEF(i_adds) { return arith(i, ops, "+", true); }

RC_DEF(i_subs) { return arith(i, ops, "-", true); }

RC_DEF(i_cmp) {
    Reg n = regop(i, ops.at(0));
    int bits = n.bits() == 64 ? 64 : 32;
    std::string b = shifted(i, ops.at(1), bits);
    return cat("nx::subs", dec(bits), "(c, ", n.r(), ", ", b, ");");
}

RC_DEF(i_cmn) {
    Reg n = regop(i, ops.at(0));
    int bits = n.bits() == 64 ? 64 : 32;
    std::string b = shifted(i, ops.at(1), bits);
    return cat("nx::adds", dec(bits), "(c, ", n.r(), ", ", b, ", 0);");
}

RC_DEF(i_neg) {
    Reg d = regop(i, ops.at(0));
    if (!d.gp()) return v_un(i, ops, "-a", VOpt{.signed_ = true});
    int bits = d.bits();
    return d.w(cat(u(bits), "(0) - ", shifted(i, ops.at(1), bits)));
}

RC_DEF(i_negs) {
    Reg d = regop(i, ops.at(0));
    return d.w(cat("nx::subs", dec(d.bits()), "(c, 0, ", shifted(i, ops.at(1), d.bits()), ")"));
}

RC_DEF(i_adc) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat(u(d.bits()), "(", n, " + ", m, " + c.cf)"));
}

RC_DEF(i_adcs) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat("nx::adds", dec(d.bits()), "(c, ", n, ", ", m, ", c.cf)"));
}

RC_DEF(i_sbc) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat(u(d.bits()), "(", n, " + ", u(d.bits()), "(~", m, ") + c.cf)"));
}

RC_DEF(i_sbcs) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat("nx::adds", dec(d.bits()), "(c, ", n, ", ", u(d.bits()), "(~", m, "), c.cf)"));
}

std::string Emitter::logic(const Insn& i, const Ops& ops, const std::string& expr, bool setflags, bool invert) {
    Reg d = regop(i, ops.at(0));
    if (!d.gp()) throw Unhandled{"vector logic here"};
    int bits = d.bits();
    std::string a = regop(i, ops.at(1)).r();
    std::string b = shifted(i, ops.at(2), bits);
    if (invert) b = cat(u(bits), "(~", b, ")");
    std::string v = cat(u(bits), "(", pyformat(expr, {{"a", a}, {"b", b}}), ")");
    if (setflags) return cat("{ ", u(bits), " r = ", v, "; nx::logic", dec(bits), "(c, r); ", d.w("r"), " }");
    return d.w(v);
}

RC_DEF(i_and) {
    if (!regop(i, ops.at(0)).gp()) return v_bin(i, ops, "a & b", VOpt{.lanes = "bytes"});
    return logic(i, ops, "{a} & {b}");
}

RC_DEF(i_ands) { return logic(i, ops, "{a} & {b}", true); }

RC_DEF(i_orr) {
    if (!regop(i, ops.at(0)).gp()) {
        if (ops.at(1).type == ARM64_OP_IMM) return v_imm_logic(i, ops, "|");
        return v_bin(i, ops, "a | b", VOpt{.lanes = "bytes"});
    }
    return logic(i, ops, "{a} | {b}");
}

RC_DEF(i_eor) {
    if (!regop(i, ops.at(0)).gp()) return v_bin(i, ops, "a ^ b", VOpt{.lanes = "bytes"});
    return logic(i, ops, "{a} ^ {b}");
}

RC_DEF(i_bic) {
    if (!regop(i, ops.at(0)).gp()) {
        if (ops.at(1).type == ARM64_OP_IMM) return v_imm_logic(i, ops, "&~");
        return v_bin(i, ops, "a & ~b", VOpt{.lanes = "bytes"});
    }
    return logic(i, ops, "{a} & {b}", false, true);
}

RC_DEF(i_bics) { return logic(i, ops, "{a} & {b}", true, true); }

RC_DEF(i_orn) {
    if (!regop(i, ops.at(0)).gp()) return v_bin(i, ops, "a | ~b", VOpt{.lanes = "bytes"});
    return logic(i, ops, "{a} | {b}", false, true);
}

RC_DEF(i_eon) { return logic(i, ops, "{a} ^ {b}", false, true); }

RC_DEF(i_tst) {
    Reg n = regop(i, ops.at(0));
    int bits = n.bits();
    std::string b = shifted(i, ops.at(1), bits);
    return cat("nx::logic", dec(bits), "(c, ", u(bits), "(", n.r(), " & ", b, "));");
}

RC_DEF(i_mvn) {
    Reg d = regop(i, ops.at(0));
    if (!d.gp()) return v_un(i, ops, "~a", VOpt{.lanes = "bytes"});
    return d.w(cat(u(d.bits()), "(~", shifted(i, ops.at(1), d.bits()), ")"));
}

std::string Emitter::shift_op(const Insn& i, const Ops& ops, const std::string& kind) {
    Reg d = regop(i, ops.at(0));
    int bits = d.bits();
    std::string a = regop(i, ops.at(1)).r();
    std::string n;
    if (ops.at(2).type == ARM64_OP_IMM)
        n = dec(ops[2].imm);
    else
        n = cat("(", regop(i, ops[2]).r(), " & ", dec(bits - 1), ")");
    std::string B_ = dec(bits);
    if (kind == "lsl") return d.w(cat(u(bits), "(", a, ") << ", n));
    if (kind == "lsr") return d.w(cat(u(bits), "(", a, ") >> ", n));
    if (kind == "asr") return d.w(cat("nx::asr", B_, "(", a, ", unsigned(", n, "))"));
    if (kind == "ror") return d.w(cat("nx::ror", B_, "(", a, ", unsigned(", n, "))"));
    throw std::runtime_error("shift_op: " + kind);
}

RC_DEF(i_lsl) { return shift_op(i, ops, "lsl"); }

RC_DEF(i_lsr) { return shift_op(i, ops, "lsr"); }

RC_DEF(i_asr) { return shift_op(i, ops, "asr"); }

RC_DEF(i_ror) { return shift_op(i, ops, "ror"); }

RC_DEF(i_extr) {
    Reg d = regop(i, ops.at(0));
    int bits = d.bits();
    std::string hi = regop(i, ops.at(1)).r();
    std::string lo = regop(i, ops.at(2)).r();
    int64_t n = ops.at(3).imm;
    if (n == 0) return d.w(lo);
    return d.w(cat("(", u(bits), "(", lo, ") >> ", dec(n), ") | (", u(bits), "(", hi, ") << ", dec(bits - n), ")"));
}

// Extract `width` bits at `lsb` from source, place at `to_pos` (0 = extract).
std::string Emitter::bitfield(const Insn& i, const Ops& ops, int64_t lsb, int64_t width, bool signed_, bool insert,
                              int64_t to_pos) {
    Reg d = regop(i, ops.at(0));
    int bits = d.bits();
    std::string src = regop(i, ops.at(1)).r();
    uint64_t field_mask = mask_bits(width);
    std::string v;
    if (signed_)
        v = cat(u(bits), "(", s(bits), "(", u(bits), "(", src, ") << ", dec(bits - lsb - width), ") >> ",
                dec(bits - width), ")");
    else
        v = cat("((", u(bits), "(", src, ") >> ", dec(lsb), ") & ", hxu(field_mask), "ull)");
    int64_t pos = to_pos;
    if (pos) v = cat(u(bits), "(", v, " << ", dec(pos), ")");
    if (insert) {
        uint64_t keep = ~shl64(field_mask, pos) & mask_bits(bits);
        v = cat("(", d.r(), " & ", hxu(keep), "ull) | (", v, " & ", hxu(shl64(field_mask, pos) & mask_bits(bits)),
                "ull)");
    }
    return d.w(v);
}

RC_DEF(i_ubfx) { return bitfield(i, ops, ops.at(2).imm, ops.at(3).imm, false); }

RC_DEF(i_sbfx) { return bitfield(i, ops, ops.at(2).imm, ops.at(3).imm, true); }

RC_DEF(i_ubfiz) { return bitfield(i, ops, 0, ops.at(3).imm, false, false, ops.at(2).imm); }

RC_DEF(i_sbfiz) {
    Reg d = regop(i, ops.at(0));
    int bits = d.bits();
    int64_t lsb = ops.at(2).imm, width = ops.at(3).imm;
    std::string src = regop(i, ops.at(1)).r();
    return d.w(cat(u(bits), "(", s(bits), "(", u(bits), "(", src, ") << ", dec(bits - width), ") >> ",
                   dec(bits - width - lsb), ")"));
}

RC_DEF(i_bfi) { return bitfield(i, ops, 0, ops.at(3).imm, false, true, ops.at(2).imm); }

RC_DEF(i_bfxil) { return bitfield(i, ops, ops.at(2).imm, ops.at(3).imm, false, true); }

RC_DEF(i_ubfm) { throw Unhandled{"raw ubfm"}; }

std::string Emitter::ext(const Insn& i, const Ops& ops, const std::string& t) {
    Reg d = regop(i, ops.at(0));
    std::string src = regop(i, ops.at(1)).r();
    return d.w(starts_with(t, "int") ? cat(u(d.bits()), "(", s(d.bits()), "(", t, "(", src, ")))")
                                     : cat(u(d.bits()), "(", t, "(", src, "))"));
}

RC_DEF(i_sxtb) { return ext(i, ops, "int8_t"); }

RC_DEF(i_sxth) { return ext(i, ops, "int16_t"); }

RC_DEF(i_sxtw) { return ext(i, ops, "int32_t"); }

RC_DEF(i_uxtb) { return ext(i, ops, "uint8_t"); }

RC_DEF(i_uxth) { return ext(i, ops, "uint16_t"); }

RC_DEF(i_csel) {
    Reg d = regop(i, ops.at(0));
    std::string c = cond(i);
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat(c, " ? ", n, " : ", m));
}

RC_DEF(i_csinc) {
    Reg d = regop(i, ops.at(0));
    std::string c = cond(i);
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat(c, " ? ", u(d.bits()), "(", n, ") : ", u(d.bits()), "(", m, " + 1)"));
}

RC_DEF(i_csinv) {
    Reg d = regop(i, ops.at(0));
    std::string c = cond(i);
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat(c, " ? ", u(d.bits()), "(", n, ") : ", u(d.bits()), "(~", m, ")"));
}

RC_DEF(i_csneg) {
    Reg d = regop(i, ops.at(0));
    std::string c = cond(i);
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat(c, " ? ", u(d.bits()), "(", n, ") : ", u(d.bits()), "(0 - ", m, ")"));
}

RC_DEF(i_cset) {
    Reg d = regop(i, ops.at(0));
    return d.w(cat("(", cond(i), ") ? 1 : 0"));
}

RC_DEF(i_csetm) {
    Reg d = regop(i, ops.at(0));
    std::string U = u(d.bits());
    return d.w(cat("(", cond(i), ") ? ", U, "(~", U, "(0)) : ", U, "(0)"));
}

RC_DEF(i_cinc) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string U = u(d.bits());
    return d.w(cat("(", cond(i), ") ? ", U, "(", n, " + 1) : ", U, "(", n, ")"));
}

RC_DEF(i_cinv) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string U = u(d.bits());
    return d.w(cat("(", cond(i), ") ? ", U, "(~", n, ") : ", U, "(", n, ")"));
}

RC_DEF(i_cneg) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string U = u(d.bits());
    return d.w(cat("(", cond(i), ") ? ", U, "(0 - ", n, ") : ", U, "(", n, ")"));
}

std::string Emitter::ccmp(const Insn& i, const Ops& ops, const std::string& fn) {
    Reg n = regop(i, ops.at(0));
    int bits = n.bits();
    std::string b = shifted(i, ops.at(1), bits);
    std::string c = cond(i);
    return cat("if (", c, ") nx::", fn, dec(bits), "(c, ", n.r(), ", ", b, fn == "adds" ? ", 0" : "",
               "); else nx::set_nzcv(c, ", dec(ops.at(2).imm), ");");
}

RC_DEF(i_ccmp) { return ccmp(i, ops, "subs"); }

RC_DEF(i_ccmn) { return ccmp(i, ops, "adds"); }

RC_DEF(i_madd) {
    Reg d = regop(i, ops.at(0));
    std::string U = u(d.bits());
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    std::string a = regop(i, ops.at(3)).r();
    return d.w(cat(U, "(", a, " + ", U, "(", n, ") * ", U, "(", m, "))"));
}

RC_DEF(i_msub) {
    Reg d = regop(i, ops.at(0));
    std::string U = u(d.bits());
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    std::string a = regop(i, ops.at(3)).r();
    return d.w(cat(U, "(", a, " - ", U, "(", n, ") * ", U, "(", m, "))"));
}

RC_DEF(i_mul) {
    Reg d = regop(i, ops.at(0));
    if (!d.gp()) return v_bin(i, ops, "a * b");
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    std::string U = u(d.bits());
    return d.w(cat(U, "(", U, "(", n, ") * ", U, "(", m, "))"));
}

RC_DEF(i_mneg) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    std::string U = u(d.bits());
    return d.w(cat(U, "(0 - ", U, "(", n, ") * ", U, "(", m, "))"));
}

std::string Emitter::long_mul(const Insn& i, const Ops& ops, bool signed_, const char* acc) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    std::string t = signed_ ? "int64_t(int32_t" : "uint64_t(uint32_t";
    std::string prod = cat("uint64_t(", t, "(", n, ")) * ", t, "(", m, ")))");
    if (acc == nullptr) return d.w(prod);
    std::string a = regop(i, ops.at(3)).r();
    return d.w(cat(a, " ", acc, " ", prod));
}

RC_DEF(i_smull) {
    if (!regop(i, ops.at(0)).gp()) return v_long(i, ops, "a * b", true, false);
    return long_mul(i, ops, true);
}

RC_DEF(i_umull) {
    if (!regop(i, ops.at(0)).gp()) return v_long(i, ops, "a * b", false, false);
    return long_mul(i, ops, false);
}

RC_DEF(i_smaddl) { return long_mul(i, ops, true, "+"); }

RC_DEF(i_umaddl) { return long_mul(i, ops, false, "+"); }

RC_DEF(i_smsubl) { return long_mul(i, ops, true, "-"); }

RC_DEF(i_umsubl) { return long_mul(i, ops, false, "-"); }

RC_DEF(i_smulh) {
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return regop(i, ops.at(0)).w(cat("uint64_t(nx::smulh(int64_t(", n, "), int64_t(", m, ")))"));
}

RC_DEF(i_umulh) {
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return regop(i, ops.at(0)).w(cat("nx::umulh(", n, ", ", m, ")"));
}

RC_DEF(i_udiv) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat("nx::udiv", dec(d.bits()), "(", n, ", ", m, ")"));
}

RC_DEF(i_sdiv) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat(u(d.bits()), "(nx::sdiv", dec(d.bits()), "(", s(d.bits()), "(", n, "), ", s(d.bits()), "(", m,
                   ")))"));
}

RC_DEF(i_clz) {
    Reg d = regop(i, ops.at(0));
    if (!d.gp()) {  // clz v0.4s, v1.4s
        int bits = vinfo(ops[0]).first;
        return v_un(i, ops, cat("std::countl_zero(uint", dec(bits), "_t(a))"));
    }
    return d.w(cat("std::countl_zero(", u(d.bits()), "(", regop(i, ops.at(1)).r(), "))"));
}

RC_DEF(i_cls) {
    Reg d = regop(i, ops.at(0));
    if (!d.gp()) {  // cls v0.4s, v1.4s
        std::string Bt = dec(vinfo(ops[0]).first);
        return v_un(i, ops,
                    cat("std::countl_zero(uint", Bt, "_t((uint", Bt, "_t(a) ^ (uint", Bt, "_t(a) << 1)) | 1))"));
    }
    std::string n = regop(i, ops.at(1)).r();
    std::string U = u(d.bits());
    return d.w(cat("std::countl_zero(", U, "((", U, "(", n, ") ^ (", U, "(", n, ") << 1)) | 1)) "));  // (sic) space
}

RC_DEF(i_rbit) {
    Reg d = regop(i, ops.at(0));
    return d.w(cat("nx::rbit", dec(d.bits()), "(", regop(i, ops.at(1)).r(), ")"));
}

RC_DEF(i_rev) {
    Reg d = regop(i, ops.at(0));
    return d.w(cat("nx::bswap(", u(d.bits()), "(", regop(i, ops.at(1)).r(), "))"));
}

RC_DEF(i_rev16) {
    Reg d = regop(i, ops.at(0));
    if (!d.gp()) return v_rev(i, ops, 16);
    return d.w(cat("nx::rev16_", dec(d.bits()), "(", regop(i, ops.at(1)).r(), ")"));
}

RC_DEF(i_rev32) {
    Reg d = regop(i, ops.at(0));
    if (!d.gp()) return v_rev(i, ops, 32);
    return d.w(cat("nx::rev32_64(", regop(i, ops.at(1)).r(), ")"));
}

RC_DEF(i_rev64) { return v_rev(i, ops, 64); }

}  // namespace rc
