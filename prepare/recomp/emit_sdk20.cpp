// Instructions first needed by SDK 20 (Animal Crossing: New Horizons 3.0.2): saturating shifts and absolute value,
// signed long add across lanes, bfc, umnegl, and two hints that do nothing here (csdb, MTE's stg). ACNHRecomp's
// recomp/emit_sdk20.py (Sdk20Ops), ported: placed before ExtOps in the MRO, its i_pmull extends ExtOps' version.
// The saturation flag (FPSR.QC) is not modelled, as elsewhere in the recompiler.
#include "emitter.h"

namespace rc {

#define RC_DEF(name) \
    std::string Emitter::name([[maybe_unused]] const Function& f, [[maybe_unused]] const Insn& i, \
                              [[maybe_unused]] const Ops& ops)

// Speculation barrier: no speculation to fence off in recompiled code.
RC_DEF(i_csdb) { return ";"; }

// MTE tag stores: the Switch has no memory tagging. Only a writeback form would change the base register.
RC_DEF(i_stg) {
    if (ops.size() > 2 || i.writeback) throw Unhandled{"stg with writeback"};
    return ";";
}

RC_DEF(i_bfc) {  // bfc wd, #lsb, #width: clear the field
    Reg d = regop(i, ops.at(0));
    int64_t lsb = ops.at(1).imm, width = ops.at(2).imm;
    uint64_t keep = ~shl64(mask_bits(width), lsb) & mask_bits(d.bits());
    return d.w(cat(d.r(), " & ", hxu(keep), "ull"));
}

RC_DEF(i_umnegl) {  // umnegl xd, wn, wm = -(zext(wn) * zext(wm))
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat("uint64_t(0) - uint64_t(uint32_t(", n, ")) * uint64_t(uint32_t(", m, "))"));
}

RC_DEF(i_smnegl) {
    Reg d = regop(i, ops.at(0));
    std::string n = regop(i, ops.at(1)).r();
    std::string m = regop(i, ops.at(2)).r();
    return d.w(cat("uint64_t(0 - int64_t(int32_t(", n, ")) * int64_t(int32_t(", m, ")))"));
}

RC_DEF(i_saddlv) {
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1));
    auto [bits, n] = vinfo(ops.at(1));
    return cat("{ int64_t s_ = 0; for (int k = 0; k < ", dec(n), "; k++) s_ += c.v[", dec(a.n), "].s", dec(bits),
               "[k]; ", "nx::V128 r = nx::vzero(); r.s", dec(bits * 2), "[0] = int", dec(bits * 2),
               "_t(s_); c.v[", dec(d.n), "] = r; }");
}

RC_DEF(i_sqabs) {
    int bits = vinfo(ops.at(0)).first;
    return v_un(i, ops, cat("nx::sat<int", dec(bits), "_t>(a < 0 ? -int64_t(a) : int64_t(a))"), VOpt{.signed_ = true});
}

RC_DEF(i_sqneg) {
    int bits = vinfo(ops.at(0)).first;
    return v_un(i, ops, cat("nx::sat<int", dec(bits), "_t>(-int64_t(a))"), VOpt{.signed_ = true});
}

// Signed saturating shift of lane value `a` by the signed amount `sh` (negative: right shift, truncating or
// rounding). Shift counts are clamped so the 128-bit intermediate never overflows.
std::string Emitter::_sq_shift(int bits, const std::string& a, const std::string& sh, bool rounding) {
    std::string b = dec(bits);
    std::string left = cat("nx::sat128<int", b, "_t>((__int128)int64_t(", a, ") << (", sh, " > ", b, " ? ", b, " : ",
                           sh, "))");
    std::string right;
    if (rounding) {
        // (a + 2^(n-1)) >> n, in 128 bits
        right = cat("int", b, "_t(-", sh, " > ", b, " ? 0 : (((__int128)int64_t(", a, ") + ((__int128)1 << (-", sh,
                    " - 1))) >> -", sh, "))");
    } else {
        right = cat("int", b, "_t(int64_t(", a, ") >> (-", sh, " > ", dec(bits - 1), " ? ", dec(bits - 1), " : -", sh,
                    "))");
    }
    return cat("((", sh, ") >= 0 ? ", left, " : ", right, ")");
}

std::string Emitter::sq_shl(const Insn& i, const Ops& ops, bool rounding) {
    auto [bits, n] = vinfo(ops.at(0));
    if (ops.at(2).type == ARM64_OP_IMM) {
        if (rounding) throw Unhandled{"sqrshl immediate"};
        return v_un(i, pyslice(ops, 0, 2),
                    cat("nx::sat128<int", dec(bits), "_t>((__int128)int64_t(a) << ", dec(ops[2].imm), ")"),
                    VOpt{.signed_ = true});
    }
    std::string sh = cat("int(int8_t(S1.u", dec(bits), "[k]))");
    return v_bin(i, ops, _sq_shift(bits, "a", sh, rounding), VOpt{.signed_ = true});
}

RC_DEF(i_sqshl) { return sq_shl(i, ops, false); }

RC_DEF(i_sqrshl) { return sq_shl(i, ops, true); }

RC_DEF(i_uqshl) {
    auto [bits, n] = vinfo(ops.at(0));
    std::string b = dec(bits);
    std::string mx = cat("uint", b, "_t(~0ull)");
    if (ops.at(2).type == ARM64_OP_IMM) {
        int64_t sh = ops[2].imm;
        std::string shs = dec(sh);
        return v_un(i, pyslice(ops, 0, 2),
                    sh ? cat("(uint64_t(a) >> (", b, " - ", shs, ")) ? ", mx, " : uint", b, "_t(uint64_t(a) << ", shs,
                             ")")
                       : std::string("a"));
    }
    std::string s_ = cat("int(int8_t(S1.u", b, "[k]))");  // Python's local `s`
    return v_bin(i, ops,
                 cat(s_, " >= 0 ? (", s_, " >= ", b, " ? (a ? ", mx, " : 0) : ((unsigned __int128)a << ", s_, ") > ",
                     mx, " ? ", mx, " : ", "uint", b, "_t(uint64_t(a) << ", s_, ")) : (-", s_, " >= ", b,
                     " ? 0 : uint64_t(a) >> -", s_, ")"));
}

// Shift and insert (main: pixel packing). sli keeps the destination's low `n` bits, sri its high `n` bits.
RC_DEF(i_sli) {
    [[maybe_unused]] auto [bits, n] = vinfo(ops.at(0));
    int64_t sh = ops.at(2).imm;
    uint64_t keep = mask_bits(sh);
    return v_shift_imm(i, ops, cat("(({a} << {n}) | (uint64_t({o}) & ", hxu(keep), "ull))"), false, true);
}

RC_DEF(i_sri) {
    auto [bits, n] = vinfo(ops.at(0));
    int64_t sh = ops.at(2).imm;
    if (sh >= bits) return ";";  // a shift by the lane size inserts nothing
    uint64_t keep = ~(mask_bits(bits) >> sh) & mask_bits(bits);
    return v_shift_imm(i, ops, cat("(({a} >> {n}) | (uint64_t({o}) & ", hxu(keep), "ull))"), false, true);
}

// Polynomial (carry-less) multiply. 1Q (crypto) is ExtOps'; 8B -> 8H lanes here.
RC_DEF(i_pmull) {
    if (ops.at(0).vas == ARM64_VAS_1Q) return ExtOps_i_pmull(f, i, ops);  // super().i_pmull
    Reg d = regop(i, ops.at(0)), a = regop(i, ops.at(1)), b = regop(i, ops.at(2));
    std::string base = i.mnemonic == "pmull2" ? "8" : "0";
    return cat("{ nx::V128 A_ = c.v[", dec(a.n), "], B_ = c.v[", dec(b.n),
               "]; nx::V128 r = nx::vzero(); for (int k = 0; k < 8; k++) { ", "uint16_t x_ = A_.u8[", base,
               " + k], p_ = 0; uint8_t y_ = B_.u8[", base, " + k]; ",
               "for (int j = 0; j < 8; j++) if (y_ >> j & 1) p_ ^= uint16_t(x_ << j); r.u16[k] = p_; } c.v[",
               dec(d.n), "] = r; }");
}

// Table lookup that keeps the destination lane for an out-of-range index (tbl writes 0 there).
RC_DEF(i_tbx) {
    auto [bits, n] = vinfo(ops.at(0));
    Reg d = regop(i, ops.at(0));
    std::vector<Reg> tables;
    for (const Op& o : pyslice(ops, 1, -1)) tables.push_back(regop(i, o));
    Reg idx = regop(i, ops.at(ops.size() - 1));
    std::vector<std::string> cps;
    for (size_t k = 0; k < tables.size(); k++)
        cps.push_back(cat("std::memcpy(t_ + ", dec(int64_t(16 * k)), ", &c.v[", dec(tables[k].n), "], 16);"));
    std::string cp = join(cps, " ");
    std::string clear = n == 16 ? "" : " r.u64[1] = 0;";
    return cat("{ uint8_t t_[64]; ", cp, " nx::V128 I_ = c.v[", dec(idx.n), "]; nx::V128 r = c.v[", dec(d.n), "]; ",
               "for (int k = 0; k < ", dec(n), "; k++) if (I_.u8[k] < ", dec(int64_t(16 * tables.size())),
               ") r.u8[k] = t_[I_.u8[k]];", clear, " c.v[", dec(d.n), "] = r; }");
}

}  // namespace rc
