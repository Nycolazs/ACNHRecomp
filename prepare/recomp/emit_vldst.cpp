// AdvSIMD structured loads and stores (ld1-ld4, st1-st4, single lanes, replicating ldNr). ACNHRecomp's
// recomp/emit_vldst.py (VecLdSt), ported.
#include "emitter.h"

namespace rc {

#define RC_DEF(name) \
    std::string Emitter::name([[maybe_unused]] const Function& f, [[maybe_unused]] const Insn& i, \
                              [[maybe_unused]] const Ops& ops)

Emitter::VList Emitter::vlist(const Insn& i, const Ops& ops) {
    MemOps mo = mem_ops(ops);
    VList v;
    for (const Op& o : mo.regs) v.rs.push_back(regop(i, o));
    v.regs = mo.regs;
    v.mem = mo.mem;
    v.after = mo.after;
    return v;
}

namespace {

// re.search(r"\}\[(\d+)\]", s): the digits of the first match, empty when there is none
std::string lane_search(const std::string& s) {
    for (size_t p = s.find("}["); p != std::string::npos; p = s.find("}[", p + 1)) {
        size_t q = p + 2;
        while (q < s.size() && s[q] >= '0' && s[q] <= '9') q++;
        if (q > p + 2 && q < s.size() && s[q] == ']') return s.substr(p + 2, q - p - 2);
    }
    return "";
}

}  // namespace

std::string Emitter::vldst(const Insn& i, const Ops& ops, int nstruct, bool store, bool replicate) {
    VList v = vlist(i, ops);
    const std::vector<Reg>& rs = v.rs;
    const Op& o0 = v.regs.at(0);
    auto [bits, lanes] = VAS().at(o0.vas);
    auto [a, wb] = address(i, need(v.mem), v.after);
    if (i.writeback && v.after == nullptr) {
        // Immediate post-index: capstone drops the operand; it always equals the bytes transferred.
        int per_reg = (o0.vector_index != -1 || replicate) ? bits / 8 : bits * lanes / 8;
        wb = cat("c.x[", dec(Reg(i.reg_name(v.mem->mem.base)).n), "] += ", dec(int64_t(per_reg * rs.size())), ";");
        a = cat("c.x[", dec(Reg(i.reg_name(v.mem->mem.base)).n), "]");
    }
    std::vector<std::string> parts = {cat("uint64_t a = ", a, ";")};
    int esz = bits / 8;
    std::string lane_m = lane_search(i.op_str);  // capstone omits the lane index on multi-register lists
    std::string Bt = dec(bits);
    if (o0.vector_index != -1 || !lane_m.empty()) {  // single lane
        int64_t lane = o0.vector_index != -1 ? o0.vector_index : std::stoll(lane_m);
        for (size_t k = 0; k < rs.size(); k++) {
            if (store)
                parts.push_back(cat("nx::st<uint", Bt, "_t>(a + ", dec(int64_t(k * esz)), ", c.v[", dec(rs[k].n),
                                    "].u", Bt, "[", dec(lane), "]);"));
            else
                parts.push_back(cat("c.v[", dec(rs[k].n), "].u", Bt, "[", dec(lane), "] = nx::ld<uint", Bt,
                                    "_t>(a + ", dec(int64_t(k * esz)), ");"));
        }
    } else if (replicate) {
        for (size_t k = 0; k < rs.size(); k++)
            parts.push_back(cat("{ uint", Bt, "_t e_ = nx::ld<uint", Bt, "_t>(a + ", dec(int64_t(k * esz)),
                                "); nx::V128 r_ = nx::vzero(); ", "for (int j = 0; j < ", dec(lanes),
                                "; j++) r_.u", Bt, "[j] = e_; c.v[", dec(rs[k].n), "] = r_; }"));
    } else if (nstruct == 1) {
        int size = bits * lanes / 8;
        for (size_t k = 0; k < rs.size(); k++) {
            if (store)
                parts.push_back(cat("std::memcpy(reinterpret_cast<void*>(a + ", dec(int64_t(k * size)), "), &c.v[",
                                    dec(rs[k].n), "], ", dec(size), ");"));
            else
                parts.push_back(cat("{ nx::V128 r_ = nx::vzero(); std::memcpy(&r_, reinterpret_cast<const void*>(a + ",
                                    dec(int64_t(k * size)), "), ", dec(size), "); c.v[", dec(rs[k].n), "] = r_; }"));
        }
    } else {
        // Interleaved: element j of structure s is at a + (j * nstruct + s) * esz.
        if (store) {
            for (size_t s_ = 0; s_ < rs.size(); s_++)
                parts.push_back(cat("for (int j = 0; j < ", dec(lanes), "; j++) nx::st<uint", Bt, "_t>(a + (j * ",
                                    dec(nstruct), " + ", dec(int64_t(s_)), ") * ", dec(esz), ", c.v[", dec(rs[s_].n),
                                    "].u", Bt, "[j]);"));
        } else {
            std::vector<std::string> tmp;
            for (size_t s_ = 0; s_ < rs.size(); s_++) {
                std::string t = cat("t", dec(int64_t(s_)));
                parts.push_back(cat("nx::V128 ", t, " = nx::vzero(); for (int j = 0; j < ", dec(lanes), "; j++) ", t,
                                    ".u", Bt, "[j] = nx::ld<uint", Bt, "_t>(a + (j * ", dec(nstruct), " + ",
                                    dec(int64_t(s_)), ") * ", dec(esz), ");"));
                tmp.push_back(cat("c.v[", dec(rs[s_].n), "] = ", t, ";"));
            }
            parts.insert(parts.end(), tmp.begin(), tmp.end());
        }
    }
    parts.push_back(wb);
    std::vector<std::string> nonempty;
    for (const auto& p : parts)
        if (!p.empty()) nonempty.push_back(p);
    return "{ " + join(nonempty, " ") + " }";
}

RC_DEF(i_ld1) { return vldst(i, ops, 1, false); }

RC_DEF(i_st1) { return vldst(i, ops, 1, true); }

RC_DEF(i_ld1r) { return vldst(i, ops, 1, false, true); }

RC_DEF(i_ld2) { return vldst(i, ops, 2, false); }

RC_DEF(i_st2) { return vldst(i, ops, 2, true); }

RC_DEF(i_ld3) { return vldst(i, ops, 3, false); }

RC_DEF(i_st3) { return vldst(i, ops, 3, true); }

RC_DEF(i_ld4) { return vldst(i, ops, 4, false); }

RC_DEF(i_st4) { return vldst(i, ops, 4, true); }

RC_DEF(i_ld2r) { return vldst(i, ops, 2, false, true); }

RC_DEF(i_ld3r) { return vldst(i, ops, 3, false, true); }

RC_DEF(i_ld4r) { return vldst(i, ops, 4, false, true); }

}  // namespace rc
