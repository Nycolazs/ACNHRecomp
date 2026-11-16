// AArch64 -> C++ instruction emitter (ACNHRecomp's recomp/emit.py and its mixins emit_*.py, ported). Every method
// keeps the Python name and builds the same text: the generated code must be byte for byte what Python writes.
//   emit.cpp            the Emitter core (emit.py): registers, operands, addressing, the function body, branches,
//                       loads/stores, system instructions
//   emit_*.cpp          the mixins (emit_int.py, emit_fp.py, ...), declared in emit_mixins.inc
// An instruction handler is i_<mnemonic> ('.' as '_'), found through the dispatch table (handlers()).
#pragma once

#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "analysis.h"
#include "insn.h"
#include "pyfmt.h"

namespace rc {

using Ops = std::vector<Op>;

// An instruction the emitter does not handle (becomes an nx::unimpl trap, counted)
struct Unhandled {
    std::string what;
};

inline std::string u(int bits) { return "uint" + std::to_string(bits) + "_t"; }
inline std::string s(int bits) { return "int" + std::to_string(bits) + "_t"; }

// A register operand: kind "x" (64-bit gp), "w" (32-bit gp), "zr64"/"zr32", or fp/simd b/h/s/d/q/v.
struct Reg {
    std::string name, kind;
    int n = -1;  // -1: None
    explicit Reg(const char* name);
    bool gp() const { return kind == "x" || kind == "w" || kind == "zr64" || kind == "zr32"; }
    int bits() const;
    bool zr() const { return kind.rfind("zr", 0) == 0; }
    std::string r() const;                      // read as unsigned of its width
    std::string w(const std::string& expr) const;  // the statement writing expr
};

extern const char* const B;  // "NXB"
const std::map<int, std::string>& CC();
const std::map<int, std::pair<int, int>>& VAS();  // arrangement -> (element bits, lanes)

class Emitter {
public:
    Emitter(const Analysis& an, std::string prefix);

    const Analysis& an;
    const Module& m;
    std::set<uint64_t> entries;
    std::string prefix;
    std::set<uint64_t> trace_sp;  // NX_TRACE_SP (debug)
    std::set<uint64_t> hooks;
    std::map<std::string, uint64_t> unhandled;
    uint64_t handled = 0;
    Disasm cs{true};

    // per function
    std::set<uint64_t> labels;
    std::vector<uint64_t> setjmp_sites;

    std::string function(const Function& f);

    // ---- emit.py helpers ----
    Reg regop(const Insn& i, const Op& o) const { return Reg(i.reg_name(o.reg)); }
    std::string fn(uint64_t addr) const { return prefix + "_" + hx8(addr); }
    std::string shifted(const Insn& i, const Op& o, int bits);
    std::string cond(const Insn& i) const;
    uint64_t target(const Op& o) const { return uint64_t(o.imm); }
    std::string goto_or_tail(const Function& f, uint64_t t);
    std::pair<std::string, std::string> address(const Insn& i, const Op& memo, const Op* after);
    std::string insn(const Function& f, const Insn& i);
    std::string crc(const Insn& i, const Ops& ops, uint32_t poly, int size);
    uint32_t sysreg(const Insn& i) const;
    struct MemOps {
        std::vector<Op> regs;
        const Op* mem = nullptr;
        const Op* after = nullptr;
    };
    MemOps mem_ops(const Ops& ops) const;
    std::string load_into(const Reg& r, const std::string& addr, int size, bool signed_);
    std::string store_from(const Reg& r, const std::string& addr, int size);
    std::string ldst(const Insn& i, const Ops& ops, int size = 0, bool signed_ = false, bool store = false,
                     bool pair = false);
    std::string acq(const Insn& i, const Ops& ops, int size);
    std::string rel(const Insn& i, const Ops& ops, int size);
    std::string ldx(const Insn& i, const Ops& ops, int size);
    std::string stx(const Insn& i, const Ops& ops, int size);

    // ---- handlers of emit.py ----
#define RC_H(name) std::string name(const Function& f, const Insn& i, const Ops& ops)
    RC_H(i_b); RC_H(i_bl); RC_H(i_blr); RC_H(i_br); RC_H(i_ret); RC_H(i_cbz); RC_H(i_cbnz); RC_H(i_tbz); RC_H(i_tbnz);
    RC_H(i_nop); RC_H(i_dc); RC_H(i_crc32b); RC_H(i_crc32h); RC_H(i_crc32w); RC_H(i_crc32x); RC_H(i_crc32cb);
    RC_H(i_crc32ch); RC_H(i_crc32cw); RC_H(i_crc32cx); RC_H(i_ngc); RC_H(i_ngcs); RC_H(i_dmb); RC_H(i_clrex);
    RC_H(i_udf); RC_H(i_brk); RC_H(i_svc); RC_H(i_mrs); RC_H(i_msr); RC_H(i_ldr); RC_H(i_ldrb); RC_H(i_ldrh);
    RC_H(i_ldrsb); RC_H(i_ldrsh); RC_H(i_ldrsw); RC_H(i_str); RC_H(i_strb); RC_H(i_strh); RC_H(i_ldp); RC_H(i_ldpsw);
    RC_H(i_stp); RC_H(i_ldar); RC_H(i_ldarb); RC_H(i_ldarh); RC_H(i_stlr); RC_H(i_stlrb); RC_H(i_stlrh);
    RC_H(i_ldaxr); RC_H(i_ldaxrb); RC_H(i_ldaxrh); RC_H(i_stlxr); RC_H(i_stlxrb); RC_H(i_stlxrh);

    // ---- the mixins (emit_*.cpp): their helpers and handlers ----
#include "emit_mixins.inc"
#undef RC_H

    using Handler = std::string (Emitter::*)(const Function&, const Insn&, const Ops&);
    // "i_<name>" -> handler, with the Python aliases (i_ldur = i_ldr, ...) and its MRO (the first class wins)
    static const std::map<std::string, Handler>& handlers();
};

// The mixins add their entries to the dispatch table (emit_*.cpp)
void register_emit(std::map<std::string, Emitter::Handler>& h);  // emit.cpp
void register_mixins(std::map<std::string, Emitter::Handler>& h);  // the mixins, in MRO order (emit_mixins.cpp)

}  // namespace rc
