#include "insn.h"

#include <cstring>
#include <stdexcept>

namespace rc {

Disasm::Disasm(bool detail, bool skipdata) {
    if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &h_) != CS_ERR_OK) throw std::runtime_error("capstone: cs_open failed");
    if (detail) cs_option(h_, CS_OPT_DETAIL, CS_OPT_ON);
    if (skipdata) cs_option(h_, CS_OPT_SKIPDATA, CS_OPT_ON);
    buf_ = cs_malloc(h_);
}

Disasm::~Disasm() {
    if (buf_) cs_free(buf_, 1);
    if (h_) cs_close(&h_);
}

bool Disasm::one(const uint8_t* code, uint64_t addr, Insn& out) {
    const uint8_t* p = code;
    size_t size = 4;
    uint64_t a = addr;
    if (!cs_disasm_iter(h_, &p, &size, &a, buf_)) return false;
    const cs_insn* i = buf_;
    out.address = i->address;
    out.mnemonic = i->mnemonic;
    out.op_str = i->op_str;
    std::memcpy(out.bytes, i->bytes, 4);
    out.handle = h_;
    const cs_arm64& d = i->detail->arm64;
    out.cc = d.cc;
    out.writeback = d.writeback;
    out.operands.resize(d.op_count);
    for (int k = 0; k < d.op_count; k++) {
        const cs_arm64_op& s = d.operands[k];
        Op& o = out.operands[size_t(k)];
        o = Op{};
        o.type = s.type;
        o.vector_index = s.vector_index;
        o.vas = s.vas;
        o.shift.type = s.shift.type;
        o.shift.value = s.shift.value;
        o.ext = s.ext;
        // the binding's properties all read the same C union, whatever the operand's type
        o.reg = s.reg;
        o.imm = s.imm;
        o.fp = s.fp;
        o.mem.base = s.mem.base, o.mem.index = s.mem.index, o.mem.disp = s.mem.disp;
    }
    return true;
}

}  // namespace rc
