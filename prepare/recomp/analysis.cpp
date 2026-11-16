// recomp/analysis.py: whole-module analysis on Capstone's lite disassembly (mnemonic + operand string): functions
// (FDE bounds, entries from symbols, calls, PLT stubs, pointers), PLT stubs, jump tables. The operand strings are
// read the way the Python's regular expressions read them, so the results are the same.
#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>

#include "analysis.h"
#include "insn.h"
#include "pyfmt.h"

namespace rc {

void Insns::init(uint64_t start, uint64_t size) {
    t0 = start, n = size / 4;
    mn_.assign(n, 0);
    op_.assign(n, 0);
}

void Insns::add(uint64_t a, const char* mn, const char* op) {
    auto it = mn_ids_.find(mn);
    uint16_t k;
    if (it == mn_ids_.end()) {
        k = uint16_t(names_.size());
        names_.emplace_back(mn);
        mn_ids_.emplace(mn, k);
    } else {
        k = it->second;
    }
    auto jt = op_ids_.find(op);
    uint32_t o;
    if (jt == op_ids_.end()) {
        o = uint32_t(ops_.size());
        ops_.emplace_back(op);
        op_ids_.emplace(op, o);
    } else {
        o = jt->second;
    }
    size_t i = size_t((a - t0) >> 2);
    mn_[i] = k;
    op_[i] = o;
}

namespace {

using Fdes = std::vector<Fde>;

// _imm: int(s, 0) of Capstone's immediates ("0x1f", "-0x10", "12", "-3")
int64_t imm_of(std::string_view s) {
    bool neg = !s.empty() && s[0] == '-';
    if (neg) s.remove_prefix(1);
    uint64_t v = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (char ch : s.substr(2)) v = v * 16 + uint64_t(std::isdigit(uint8_t(ch)) ? ch - '0' : std::tolower(ch) - 'a' + 10);
    } else {
        for (char ch : s) v = v * 10 + uint64_t(ch - '0');
    }
    return neg ? -int64_t(v) : int64_t(v);
}

// IMM = r"#(-?0x[0-9a-f]+|-?\d+)": every match's group, in order (findall); search = the first
std::vector<std::string_view> imms(std::string_view op) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while ((i = op.find('#', i)) != std::string_view::npos) {
        size_t j = i + 1, k = j;
        if (k < op.size() && op[k] == '-') k++;
        bool hex = k + 1 < op.size() && op[k] == '0' && op[k + 1] == 'x' && k + 2 < op.size() &&
                   std::isxdigit(uint8_t(op[k + 2])) && !std::isupper(uint8_t(op[k + 2]));
        if (hex) {
            size_t e = k + 2;
            while (e < op.size() && (std::isdigit(uint8_t(op[e])) || (op[e] >= 'a' && op[e] <= 'f'))) e++;
            out.push_back(op.substr(j, e - j));
            i = e;
            continue;
        }
        size_t e = k;
        while (e < op.size() && std::isdigit(uint8_t(op[e]))) e++;
        if (e > k) {
            out.push_back(op.substr(j, e - j));
            i = e;
            continue;
        }
        i = j;
    }
    return out;
}

std::optional<int64_t> first_imm(std::string_view op) {
    auto v = imms(op);
    if (v.empty()) return std::nullopt;
    return imm_of(v[0]);
}

// _regno: x9 / w9 -> "9"; fp/x29 -> "29", lr/x30 -> "30"; anything else -> its name (an int never equals a name)
std::string regno(std::string_view r) {
    r = strip(r);
    if (r == "fp" || r == "x29" || r == "w29") return "29";
    if (r == "lr" || r == "x30" || r == "w30") return "30";
    if (r.size() >= 2 && (r[0] == 'x' || r[0] == 'w')) {
        bool digits = true;
        for (char ch : r.substr(1)) digits &= std::isdigit(uint8_t(ch)) != 0;
        if (digits) return std::to_string(std::stoll(std::string(r.substr(1))));
    }
    return "~" + std::string(r);  // a name, never equal to a number
}

// _ops: split on top-level commas, each part stripped, empty last part dropped
std::vector<std::string> split_ops(std::string_view op) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char ch : op) {
        if (ch == '[' || ch == '{') depth++;
        else if (ch == ']' || ch == '}') depth--;
        if (ch == ',' && depth == 0) {
            out.emplace_back(strip(cur));
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!strip(cur).empty()) out.emplace_back(strip(cur));
    return out;
}

const std::set<std::string> BRANCH_OUT = {"b", "bl", "cbz", "cbnz", "tbz", "tbnz"};
const std::set<std::string> NO_DEST = {
    "str", "strb", "strh", "stur", "sturb", "sturh", "stp", "stlr", "stlrb", "stlrh", "st1", "st2", "st3", "st4",
    "cmp", "cmn", "tst", "fcmp", "fcmpe", "ccmp", "ccmn", "fccmp", "b", "bl", "br", "blr", "ret", "cbz", "cbnz",
    "tbz", "tbnz", "prfm", "nop", "udf", "brk", "svc", "msr", "dmb", "dsb", "isb", "clrex", "hint"};

// COND_B = r"^b\.(\w+)$"
bool cond_b(const std::string& mn) {
    if (mn.size() < 3 || mn[0] != 'b' || mn[1] != '.') return false;
    for (size_t k = 2; k < mn.size(); k++)
        if (!is_word(mn[k])) return false;
    return true;
}

struct Def {
    uint64_t a;
    std::string mn;
    std::vector<std::string> ops;
};

// _find_def: the last instruction in f before `before` (linear, backwards) that writes register `reg`
std::optional<Def> find_def(const Analysis& an, const Function& f, uint64_t before, const std::string& reg) {
    int64_t a = int64_t(before) - 4;
    while (a >= int64_t(f.start)) {
        auto [mn, op] = an.insns.get(uint64_t(a));
        if (!mn.empty() && !NO_DEST.count(mn)) {
            auto ops = split_ops(op);
            if (!ops.empty() && regno(ops[0]) == reg) return Def{uint64_t(a), mn, ops};
            if ((mn == "ldp" || mn == "ldpsw") && ops.size() > 1 && regno(ops[1]) == reg) return Def{uint64_t(a), mn, ops};
        }
        a -= 4;
    }
    return std::nullopt;
}

// _const: the constant value of reg at `before`, from adrp[+add] / adr / mov
std::optional<int64_t> const_of(const Analysis& an, const Function& f, uint64_t before, const std::string& reg, int depth = 0) {
    auto d = find_def(an, f, before, reg);
    if (!d || depth > 4) return std::nullopt;
    const auto& ops = d->ops;
    if (d->mn == "adr" || d->mn == "adrp") {
        auto v = ops.size() > 1 ? first_imm(ops[1]) : std::nullopt;
        if (!v) throw std::runtime_error("analysis: adr without an immediate");
        return v;
    }
    if (d->mn == "add" && ops.size() == 3 && starts_with(ops[2], "#") && regno(ops[1]) != "~sp") {
        auto base = const_of(an, f, d->a, regno(ops[1]), depth + 1);
        if (!base) return std::nullopt;
        return *base + imm_of(std::string_view(ops[2]).substr(1));
    }
    if (d->mn == "mov" && ops.size() > 1 && starts_with(ops[1], "#")) return imm_of(std::string_view(ops[1]).substr(1));
    if (d->mn == "mov" && ops.size() > 1) return const_of(an, f, d->a, regno(ops[1]), depth + 1);
    return std::nullopt;
}

// _bound: entries from `cmp idx, #N` + b.hi / b.hs before the load
std::optional<int64_t> bound(const Analysis& an, const Function& f, uint64_t before, const std::string& idx) {
    std::set<std::string> regs = {idx};
    int64_t a = int64_t(before) - 4;
    int64_t lo = std::max<int64_t>(int64_t(f.start), int64_t(before) - 4 * 40);
    while (a >= lo) {
        auto [mn, op] = an.insns.get(uint64_t(a));
        auto ops = split_ops(op);
        bool copy = ops.size() >= 2 && !ops[1].empty() && (ops[1][0] == 'x' || ops[1][0] == 'w') &&
                    (mn == "mov" || mn == "uxtw" || mn == "sxtw" ||
                     (mn == "and" && ops.size() == 3 && (ops[2] == "#0xffffffff" || ops[2] == "#4294967295")));
        if (copy && regs.count(regno(ops[0]))) {
            regs.erase(regno(ops[0]));
            regs.insert(regno(ops[1]));
            a -= 4;
            continue;
        }
        if (mn == "cmp" && ops.size() == 2 && regs.count(regno(ops[0])) && starts_with(ops[1], "#")) {
            int64_t n = imm_of(std::string_view(ops[1]).substr(1));
            for (int k = 1; k < 6; k++) {
                const std::string& bm = an.insns.get(uint64_t(a) + 4 * uint64_t(k)).first;
                if (bm == "b.hi") return n + 1;
                if (bm == "b.hs") return n;
            }
            return std::nullopt;
        }
        if (!mn.empty() && !NO_DEST.count(mn) && !ops.empty() && regs.count(regno(ops[0]))) return std::nullopt;
        a -= 4;
    }
    return std::nullopt;
}

// re.search(r"(lsl|sxtw|uxtw) #(\d+)", s): the shift amount
std::optional<int> shift_amount(const std::string& s) {
    for (size_t i = 0; i < s.size(); i++) {
        for (const char* w : {"lsl", "sxtw", "uxtw"}) {
            size_t n = std::strlen(w);
            if (s.compare(i, n, w) == 0 && i + n + 1 < s.size() && s[i + n] == ' ' && s[i + n + 1] == '#') {
                size_t k = i + n + 2, e = k;
                while (e < s.size() && std::isdigit(uint8_t(s[e]))) e++;
                if (e > k) return std::stoi(s.substr(k, e - k));
            }
        }
    }
    return std::nullopt;
}

// _jump_table: the targets of a compressed clang jump table ending at br, or nothing
std::optional<std::vector<uint64_t>> jump_table(const Analysis& an, const Function& f, uint64_t br) {
    const Module& m = *an.module;
    std::string reg = regno(an.insns.op_str(br));
    auto d = find_def(an, f, br, reg);
    if (!d || d->mn != "add" || d->ops.size() < 3) return std::nullopt;
    uint64_t a_add = d->a;
    const auto& ops = d->ops;
    int shift = 0;
    {
        std::vector<std::string> rest(ops.begin() + 2, ops.end());
        if (auto sh = shift_amount(join(rest, ","))) shift = *sh;
    }
    std::optional<Def> ld;
    std::optional<int64_t> anchor;
    for (int pass = 0; pass < 2; pass++) {
        const std::string& ent_op = pass == 0 ? ops[2] : ops[1];
        const std::string& anc_op = pass == 0 ? ops[1] : ops[2];
        auto cand = find_def(an, f, a_add, regno(ent_op));
        if (cand && starts_with(cand->mn, "ldr") && contains(join(cand->ops, ","), "[")) {
            ld = cand;
            anchor = const_of(an, f, a_add, regno(anc_op));
            if (pass == 1 && shift) return std::nullopt;  // shifted anchor (anc_op is ops[2])
            break;
        }
    }
    if (!ld || !anchor) return std::nullopt;
    const std::string& ld_mn = ld->mn;
    int size = ld_mn == "ldrb" ? 1 : ld_mn == "ldrh" ? 2 : ld_mn == "ldrsb" ? 1 : ld_mn == "ldrsh" ? 2
             : ld_mn == "ldrsw" ? 4 : ld_mn == "ldr" ? 4 : 0;
    bool signed_ = ld_mn == "ldrsb" || ld_mn == "ldrsh" || ld_mn == "ldrsw";
    // re.match(r"\[(\w+)(?:, (\w+))?", ld_ops[1])
    if (!size || ld->ops.size() < 2) return std::nullopt;
    const std::string& mem = ld->ops[1];
    if (mem.empty() || mem[0] != '[') return std::nullopt;
    size_t k = 1;
    while (k < mem.size() && is_word(mem[k])) k++;
    if (k == 1) return std::nullopt;
    std::string g1 = mem.substr(1, k - 1), g2;
    if (mem.compare(k, 2, ", ") == 0) {
        size_t e = k + 2;
        while (e < mem.size() && is_word(mem[e])) e++;
        if (e > k + 2) g2 = mem.substr(k + 2, e - k - 2);
    }
    if (g2.empty()) return std::nullopt;
    std::string base_reg = regno(g1), idx = regno(g2);
    auto count = bound(an, f, ld->a, idx);

    auto entries = [&](int64_t table, int& misses) {
        int64_t limit = count ? *count : 256;
        std::vector<uint64_t> out;
        misses = 0;
        for (int64_t kk = 0; kk < limit; kk++) {
            int64_t p = table + kk * size;
            if (p + size > int64_t(m.image.size())) break;
            uint64_t raw = 0;
            for (int b = 0; b < size; b++) raw |= uint64_t(m.image[size_t(p + b)]) << (8 * b);
            int64_t v = signed_ ? (size == 1 ? int8_t(raw) : size == 2 ? int16_t(raw) : int32_t(raw)) : int64_t(raw);
            int64_t t = *anchor + (v << shift);  // Python: arbitrary precision; these stay far inside 64 bits
            if (!(int64_t(f.start) <= t && t < int64_t(f.end)) || !an.insns.contains(uint64_t(t))) {
                misses++;
                if (!count && misses > 4) break;
                continue;
            }
            out.push_back(uint64_t(t));
        }
        return out;
    };
    auto sorted_set = [](std::vector<uint64_t> v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    };
    auto table = const_of(an, f, ld->a, base_reg);
    if (table && m.in_image(*table)) {
        int misses;
        auto out = entries(*table, misses);
        if (out.empty()) return std::nullopt;
        return sorted_set(out);
    }
    if (!count) return std::nullopt;
    std::vector<int64_t> candidates;
    for (uint64_t a = f.start; a < f.end; a += 4) {
        auto [mn, op] = an.insns.get(a);
        auto o = split_ops(op);
        if ((mn == "adrp" || mn == "adr" || mn == "add") && !o.empty() && regno(o[0]) == base_reg) {
            auto c = const_of(an, f, a + 4, base_reg);
            if (c && m.in_image(*c) && std::find(candidates.begin(), candidates.end(), *c) == candidates.end())
                candidates.push_back(*c);
        }
    }
    std::vector<uint64_t> found;
    for (int64_t c : candidates) {
        int misses;
        auto out = entries(c, misses);
        if (!out.empty() && misses == 0) found.insert(found.end(), out.begin(), out.end());
    }
    if (found.empty()) return std::nullopt;
    return sorted_set(found);
}

bool looks_like_code_pointer(const Analysis& an, uint64_t a) {
    for (int k = 1; k < 4; k++) {
        auto [mn, op] = an.insns.get(a + 4 * uint64_t(k));
        if (mn == "add" && contains(op, "lsl #2")) return false;
    }
    return true;
}

bool in_plt(const Analysis& an, uint64_t a) {
    for (int64_t d : {12, 8, 4, 0}) {
        int64_t s = int64_t(a) - d;
        if (s >= 0 && an.plt.count(uint64_t(s)) && s <= int64_t(a) && int64_t(a) < s + 16) return true;
    }
    return false;
}

std::vector<Function> ranges(const Analysis& an, const std::vector<uint64_t>& entries, const Fdes& fdes,
                             const std::map<uint64_t, uint64_t>& fde_bounds) {
    uint64_t t0 = an.module->text[0], text_end = t0 + an.module->text[1];
    std::vector<uint64_t> fde_starts;
    for (auto& f : fdes) fde_starts.push_back(f.start);
    std::vector<Function> funcs;
    funcs.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); i++) {
        uint64_t e = entries[i];
        if (an.plt.count(e)) {
            funcs.push_back({e, e + 16, false, ""});
            continue;
        }
        auto fb = fde_bounds.find(e);
        if (fb != fde_bounds.end()) {
            funcs.push_back({e, fb->second, true, ""});
            continue;
        }
        size_t j = size_t(std::upper_bound(fde_starts.begin(), fde_starts.end(), e) - fde_starts.begin());
        if (j > 0 && e < fdes[j - 1].start + fdes[j - 1].size) {
            funcs.push_back({e, fdes[j - 1].start + fdes[j - 1].size, false, ""});
            continue;
        }
        uint64_t nxt = i + 1 < entries.size() ? entries[i + 1] : text_end;
        size_t k = j;  // bisect_right(fde_starts, e)
        if (k < fde_starts.size()) nxt = std::min(nxt, fde_starts[k]);
        funcs.push_back({e, nxt, false, ""});
    }
    for (auto& f : funcs) {
        auto it = an.names.find(f.start);
        f.name = it != an.names.end() ? it->second : "";
    }
    return funcs;
}

}  // namespace

void analyze(const Module& m, Analysis& an, const std::function<void(const std::string&)>& log,
             const std::vector<uint64_t>& patched) {
    uint64_t t0 = m.text[0], tn = m.text[1];
    an.module = &m;
    an.insns.init(t0, tn);
    {
        Disasm cs(false, true);
        const uint64_t step = 1 << 16;
        for (uint64_t s = t0; s < t0 + tn; s += step) {
            uint64_t len = std::min(step, t0 + tn - s);
            cs.lite(m.image.data() + s, len, s, [&](uint64_t a, const char* mn, const char* op) {
                if (std::strcmp(mn, ".byte") != 0) an.insns.add(a, mn, op);
            });
        }
    }
    an.insns.done();
    const Insns& insns = an.insns;
    uint64_t text_end = t0 + tn;

    // nso.plt_imports: GOT slot -> imported symbol name (JUMP_SLOT, GLOB_DAT, ABS64); the last relocation wins
    for (const Reloc& r : m.relocs)
        if ((r.type == R_AARCH64_JUMP_SLOT || r.type == R_AARCH64_GLOB_DAT || r.type == R_AARCH64_ABS64) && r.sym) {
            const Symbol& s = m.symbols.at(r.sym);
            if (s.imported()) an.got_imports[r.offset] = s.name;
        }
    for (const Reloc& r : m.relocs)
        if ((r.type == R_AARCH64_JUMP_SLOT || r.type == R_AARCH64_GLOB_DAT) && r.sym && !m.symbols.at(r.sym).imported())
            an.got_local[r.offset] = m.symbols[r.sym].name;
    for (const Symbol& s : m.symbols)
        if (!s.name.empty() && !s.imported() && m.in_text(s.value)) an.names[s.value] = s.name;

    // PLT stubs: adrp x16, page / ldr x17, [x16, #off] / add x16, x16, #off / br x17
    insns.each([&](uint64_t a, const std::string& mn, const std::string& op) {
        if (mn == "adrp" && starts_with(op, "x16,")) {
            auto n1 = insns.get(a + 4), n3 = insns.get(a + 12);
            if (n1.first == "ldr" && starts_with(n1.second, "x17, [x16") && n3.first == "br" && n3.second == "x17") {
                int64_t page = *first_imm(op);
                auto off = first_imm(n1.second);
                uint64_t got = uint64_t(page + (off ? *off : 0));
                auto gi = an.got_imports.find(got);
                an.plt[a] = gi != an.got_imports.end() ? gi->second : "got_" + hxu(got).substr(2);
                auto gl = an.got_local.find(got);
                if (gl != an.got_local.end()) an.plt_local[a] = gl->second;
            }
        }
    });

    // seeds
    Fdes fdes;
    for (const Fde& f : parse_ehframe(m))
        if (t0 <= f.start && f.start < text_end) fdes.push_back(f);
    std::set<uint64_t> entries;
    for (auto& f : fdes) entries.insert(f.start);
    for (auto& [a, _] : an.plt) entries.insert(a);
    for (auto& [a, _] : an.names) entries.insert(a);
    for (uint64_t a : m.init_array)
        if (t0 <= a && a < text_end) entries.insert(a);
    for (uint64_t a : m.fini_array)
        if (t0 <= a && a < text_end) entries.insert(a);
    for (const Reloc& r : m.relocs)
        if (r.type == R_AARCH64_RELATIVE && int64_t(t0) <= r.addend && r.addend < int64_t(text_end) &&
            insns.contains(uint64_t(r.addend)))
            entries.insert(uint64_t(r.addend));
    insns.each([&](uint64_t a, const std::string& mn, const std::string& op) {
        if (mn == "bl") {
            entries.insert(uint64_t(*first_imm(op)));
        } else if (mn == "adr") {
            uint64_t t = uint64_t(*first_imm(op));
            if (t0 <= t && t < text_end && insns.contains(t) && looks_like_code_pointer(an, a)) entries.insert(t);
        }
    });
    if (insns.contains(t0)) entries.insert(t0);
    std::map<uint64_t, uint64_t> fde_bounds;
    for (auto& f : fdes) fde_bounds[f.start] = f.start + f.size;  // a later FDE of the same start wins, as a dict does
    std::set<uint64_t> grown(patched.begin(), patched.end());
    if (!grown.empty()) {
        for (auto& [start, end] : fde_bounds)
            while (grown.count(end) && !entries.count(end)) end += 4;
    }
    log("  seeds: " + std::to_string(fdes.size()) + " FDE, " + std::to_string(an.plt.size()) + " PLT stubs, " +
        std::to_string(entries.size()) + " entries");

    // fixpoint: ranges, jump tables, out-of-range branch targets promoted to entries
    for (int it = 0;; it++) {
        if (it == 400) throw std::runtime_error("analysis: branch targets still growing after 400 passes");
        an.functions = ranges(an, std::vector<uint64_t>(entries.begin(), entries.end()), fdes, fde_bounds);
        an.jump_tables.clear();
        std::set<uint64_t> fresh;
        for (const Function& f : an.functions) {
            if (an.plt.count(f.start)) continue;
            for (uint64_t a = f.start; a < f.end; a += 4) {
                const std::string* mnp = insns.mnemonic(a);
                if (!mnp) continue;
                const std::string& mn = *mnp;
                if (BRANCH_OUT.count(mn) || cond_b(mn)) {
                    if (mn == "bl") continue;
                    auto all = imms(insns.op_str(a));
                    uint64_t t = uint64_t(imm_of(all.back()));
                    if (!(f.start <= t && t < f.end) && insns.contains(t) && !entries.count(t)) fresh.insert(t);
                } else if (mn == "br") {
                    auto tg = jump_table(an, f, a);
                    if (tg) {
                        // Keep every function's view: the table is cut to each function's bounds, and a function
                        // that overlaps this br must not lose the targets another one saw (ACNHRecomp 8d0f374, issue
                        // #58: the SpeedFix's entry 0x10cac40 in the middle of a table; a lost case became a call
                        // into a function's middle). sorted(set(old) | set(new)), as analysis.py does.
                        std::vector<uint64_t>& seen = an.jump_tables[a];
                        seen.insert(seen.end(), tg->begin(), tg->end());
                        std::sort(seen.begin(), seen.end());
                        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
                    }
                }
            }
        }
        for (const Function& f : an.functions)
            for (uint64_t a = f.start; a < f.end; a += 4) {
                auto jt = an.jump_tables.find(a);
                if (jt == an.jump_tables.end()) continue;
                for (uint64_t t : jt->second)
                    if (!(f.start <= t && t < f.end) && insns.contains(t) && !entries.count(t)) fresh.insert(t);
            }
        log("  pass " + std::to_string(it) + ": " + std::to_string(an.functions.size()) + " functions, " +
            std::to_string(an.jump_tables.size()) + " jump tables, +" + std::to_string(fresh.size()) + " entries");
        if (fresh.empty()) break;
        entries.insert(fresh.begin(), fresh.end());
    }

    insns.each([&](uint64_t a, const std::string& mn, const std::string&) {
        if (mn == "br" && !an.jump_tables.count(a) && !in_plt(an, a)) an.unresolved_br.push_back(a);
    });
}

}  // namespace rc
