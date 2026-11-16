// Capstone (third_party/capstone, the exact version ACNHRecomp's Python recompiler uses) seen the way the Python
// binding shows it: an instruction's mnemonic and operand string, and its operands with the same field names
// (o.type, o.imm, o.reg, o.fp, o.shift.type/value, o.ext, o.vas, o.vector_index, o.mem.base/index/disp).
#pragma once

#include <capstone/capstone.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rc {

struct Op {
    int type = 0;  // ARM64_OP_*
    unsigned reg = 0;
    int64_t imm = 0;
    double fp = 0;
    struct {
        int type = 0;  // ARM64_SFT_*
        unsigned value = 0;
    } shift;
    int ext = 0;  // ARM64_EXT_*
    int vas = 0;  // ARM64_VAS_*
    int vector_index = -1;
    struct {
        unsigned base = 0, index = 0;
        int32_t disp = 0;
    } mem;
};

struct Insn {
    uint64_t address = 0;
    std::string mnemonic, op_str;
    std::vector<Op> operands;
    int cc = 0;  // ARM64_CC_*
    bool writeback = false;
    uint8_t bytes[4] = {};
    csh handle = 0;
    // i.reg_name(r): nullptr where the Python binding gives None
    const char* reg_name(unsigned r) const { return cs_reg_name(handle, r); }
};

// One disassembler (not shareable between threads: one per worker).
class Disasm {
public:
    explicit Disasm(bool detail, bool skipdata = false);
    ~Disasm();
    Disasm(const Disasm&) = delete;
    Disasm& operator=(const Disasm&) = delete;
    // cs.disasm(code[0:4], addr) with detail: false where Capstone decodes nothing
    bool one(const uint8_t* code, uint64_t addr, Insn& out);
    // cs.disasm_lite over a buffer: fn(addr, mnemonic, op_str) per instruction, in order
    template <class Fn>
    void lite(const uint8_t* code, size_t size, uint64_t addr, Fn&& fn) {
        cs_insn* ins = cs_malloc(h_);
        const uint8_t* p = code;
        uint64_t a = addr;
        while (size && cs_disasm_iter(h_, &p, &size, &a, ins)) fn(ins->address, ins->mnemonic, ins->op_str);
        cs_free(ins, 1);
    }
    csh handle() const { return h_; }

private:
    csh h_ = 0;
    cs_insn* buf_ = nullptr;
};

}  // namespace rc
