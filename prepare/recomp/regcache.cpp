// recomp/regcache.py, ported. The Python works on the emitted text with regular expressions; each one is matched here
// by hand with the same semantics (word boundaries, the look-behind of CALL, the order of alternatives), so the
// rewritten text is the same. The environment switches of the Python (NX_REGCACHE, NX_REGCACHE_REGS,
// NX_REGCACHE_FLAGS, NX_REGCACHE_V) are honoured the same way.
//   REG         \bc\.x\[(\d+)\]
//   WRITE       \bx(\d+)\s*(?:[-+|&^]?=(?!=)|\+\+|--)
//   GOTO        \bgoto L_([0-9A-F]+);
//   LABEL       ^L_([0-9A-F]+):;$
//   CALL        (?<![\w.>])((?:js_\[\w+\]\.)?[A-Za-z_][\w:]*)(<[^<>()]*>)?\(c([,)])
//   FLAG        \bc\.([nzcv]f)\b
//   FLAG_HELPER \b(nx::(?:subs|adds|logic)(?:32|64)|nx::set_nzcv|nx::get_nzcv|nx::fcmpe?)(<[^<>()]*>)?\(c([,)])
//   FLAG_WRITE  \bnx::(?:subs|adds|logic)(?:32|64)\(fl_|\bnx::set_nzcv\(fl_|\bnx::fcmpe?(?:<[^<>()]*>)?\(fl_|\bfl_\.[nzcv]f\s*=(?!=)
//   VREG        \bc\.v\[(\d+)\]
//   VWRITE      \bvr(\d+)(?:\.\w+\[[^\]]*\])?\s*(?:[-+*/|&^]?=(?!=)|\+\+|--)|memcpy\(&vr(\d+)\b
#include "regcache.h"

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include "pyfmt.h"

namespace rc {

namespace {

using sv = std::string_view;

bool w(char ch) { return is_word(ch); }
bool dig(char ch) { return ch >= '0' && ch <= '9'; }
bool bnd_before(sv s, size_t i) { return i == 0 || !w(s[i - 1]); }  // \b before a word character at i
size_t digits_end(sv s, size_t i) {
    while (i < s.size() && dig(s[i])) i++;
    return i;
}
bool at(sv s, size_t i, sv t) { return s.substr(i, t.size()) == t; }

struct Settings {
    bool enabled = true, flags = true, vectors = true;
    std::set<int> cached;
    Settings() {
        auto env = [](const char* n, const char* d) {
            const char* v = std::getenv(n);
            return std::string(v ? v : d);
        };
        enabled = env("NX_REGCACHE", "1") != "0";
        std::string regs = env("NX_REGCACHE_REGS", "all");
        if (regs == "all") {
            for (int k = 0; k < 32; k++) cached.insert(k);
        } else {
            size_t p = 0;
            while (p <= regs.size()) {
                size_t q = regs.find(',', p);
                std::string x = regs.substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (!x.empty()) cached.insert(std::stoi(x));
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        flags = env("NX_REGCACHE_FLAGS", "1") != "0";
        vectors = env("NX_REGCACHE_V", "1") != "0";
    }
};
const Settings& settings() {
    static const Settings s;
    return s;
}

// REG / VREG: "c.x[" or "c.v[" (tag) + digits + "]" at a word boundary; fn(start, end, number)
template <class Fn>
void each_ctx_reg(sv s, char tag, Fn&& fn) {
    size_t i = 0;
    while (i + 4 < s.size()) {
        if (s[i] == 'c' && s[i + 1] == '.' && s[i + 2] == tag && s[i + 3] == '[' && bnd_before(s, i)) {
            size_t e = digits_end(s, i + 4);
            if (e > i + 4 && e < s.size() && s[e] == ']') {
                fn(i, e + 1, std::atoi(std::string(s.substr(i + 4, e - i - 4)).c_str()));
                i = e + 1;
                continue;
            }
        }
        i++;
    }
}

// FLAG: c.[nzcv]f with word boundaries on both sides; fn(start, end, "nf")
template <class Fn>
void each_flag(sv s, Fn&& fn) {
    size_t i = 0;
    while (i + 4 <= s.size()) {
        if (s[i] == 'c' && s[i + 1] == '.' && std::strchr("nzcv", s[i + 2]) && s[i + 2] && s[i + 3] == 'f' &&
            bnd_before(s, i) && (i + 4 == s.size() || !w(s[i + 4]))) {
            fn(i, i + 4, s.substr(i + 2, 2));
            i += 4;
            continue;
        }
        i++;
    }
}

// [<...>] at i: the end of a '<' [^<>()]* '>' group, or i when there is none
size_t angle_group(sv s, size_t i) {
    if (i < s.size() && s[i] == '<') {
        size_t k = i + 1;
        while (k < s.size() && s[k] != '<' && s[k] != '>' && s[k] != '(' && s[k] != ')') k++;
        if (k < s.size() && s[k] == '>') return k + 1;
    }
    return i;
}

// the name alternatives of FLAG_HELPER / FLAG_WRITE at i: the end of the name, or 0
size_t flag_fn_name(sv s, size_t i, bool fcmp_too) {
    if (!at(s, i, "nx::")) return 0;
    size_t k = i + 4;
    for (sv a : {sv("subs"), sv("adds"), sv("logic")})
        if (at(s, k, a) && (at(s, k + a.size(), "32") || at(s, k + a.size(), "64"))) return k + a.size() + 2;
    if (at(s, k, "set_nzcv")) return k + 8;
    if (fcmp_too && at(s, k, "get_nzcv")) return k + 8;
    if (at(s, k, "fcmp")) return at(s, k + 4, "e") ? k + 5 : k + 4;
    return 0;
}

// FLAG_HELPER: fn(start, name_end, group2_end, after "(c") where the match ends after [,)]
template <class Fn>
void each_flag_helper(sv s, Fn&& fn) {
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == 'n' && bnd_before(s, i)) {
            size_t ne = flag_fn_name(s, i, true);
            if (ne) {
                // fcmpe? : with the 'e' first, then without (the second can only match when the first fails)
                for (int pass = 0; pass < 2; pass++) {
                    size_t name_end = ne;
                    if (pass == 1) {
                        if (!(at(s, i, "nx::fcmpe") && ne == i + 9)) break;
                        name_end = i + 8;
                    }
                    size_t g2 = angle_group(s, name_end);
                    if (at(s, g2, "(c") && g2 + 2 < s.size() && (s[g2 + 2] == ',' || s[g2 + 2] == ')')) {
                        fn(i, name_end, g2);
                        i = g2 + 3;
                        goto next;
                    }
                }
            }
        }
        i++;
    next:;
    }
}

// The match starts of FLAG_WRITE, in order (finditer)
std::vector<size_t> flag_writes(sv s) {
    std::vector<size_t> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t end = 0;
        if (bnd_before(s, i) && s[i] == 'n') {
            size_t k = i + 4;
            if (at(s, i, "nx::")) {
                for (sv a : {sv("subs"), sv("adds"), sv("logic")})
                    if (!end && at(s, k, a) && (at(s, k + a.size(), "32") || at(s, k + a.size(), "64")) &&
                        at(s, k + a.size() + 2, "(fl_"))
                        end = k + a.size() + 6;
                if (!end && at(s, k, "set_nzcv(fl_")) end = k + 12;
                if (!end && at(s, k, "fcmp")) {
                    for (size_t ne : {k + 5, k + 4}) {  // fcmpe? with the 'e' first
                        if (ne == k + 5 && !at(s, k + 4, "e")) continue;
                        size_t g = angle_group(s, ne);
                        if (at(s, g, "(fl_")) {
                            end = g + 4;
                            break;
                        }
                    }
                }
            }
        }
        if (!end && bnd_before(s, i) && at(s, i, "fl_.") && i + 6 <= s.size() && std::strchr("nzcv", s[i + 4]) && s[i + 4] &&
            s[i + 5] == 'f') {
            size_t k = i + 6;
            while (k < s.size() && std::isspace(uint8_t(s[k]))) k++;
            if (k < s.size() && s[k] == '=' && !(k + 1 < s.size() && s[k + 1] == '=')) end = k + 1;
        }
        if (end) {
            out.push_back(i);
            i = end;
        } else {
            i++;
        }
    }
    return out;
}

// the assignment operator of WRITE / VWRITE at i (after \s*): its end, or 0. ops: the optional character class.
size_t assign_op(sv s, size_t i, sv ops) {
    while (i < s.size() && std::isspace(uint8_t(s[i]))) i++;
    if (i >= s.size()) return 0;
    auto eq_at = [&](size_t k) { return k < s.size() && s[k] == '=' && !(k + 1 < s.size() && s[k + 1] == '='); };
    if (ops.find(s[i]) != sv::npos && eq_at(i + 1)) return i + 2;
    if (eq_at(i)) return i + 1;
    if (at(s, i, "++") || at(s, i, "--")) return i + 2;
    return 0;
}

// WRITE: (start, register) of every match
std::vector<std::pair<size_t, int>> writes(sv s) {
    std::vector<std::pair<size_t, int>> out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == 'x' && bnd_before(s, i)) {
            size_t e = digits_end(s, i + 1);
            if (e > i + 1) {
                size_t end = assign_op(s, e, "-+|&^");
                if (end) {
                    out.push_back({i, std::atoi(std::string(s.substr(i + 1, e - i - 1)).c_str())});
                    i = end;
                    continue;
                }
            }
        }
        i++;
    }
    return out;
}

// VWRITE: (start, vector register) of every match
std::vector<std::pair<size_t, int>> vwrites(sv s) {
    std::vector<std::pair<size_t, int>> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t end = 0;
        int reg = 0;
        if (at(s, i, "vr") && bnd_before(s, i)) {
            size_t e = digits_end(s, i + 2);
            if (e > i + 2) {
                // (?:\.\w+\[[^\]]*\])? first, then without it
                size_t g = e;
                if (g < s.size() && s[g] == '.') {
                    size_t k = g + 1;
                    while (k < s.size() && w(s[k])) k++;
                    if (k > g + 1 && k < s.size() && s[k] == '[') {
                        size_t c = s.find(']', k + 1);
                        if (c != sv::npos) g = c + 1;
                    }
                }
                end = assign_op(s, g, "-+*/|&^");
                if (!end && g != e) end = assign_op(s, e, "-+*/|&^");
                if (end) reg = std::atoi(std::string(s.substr(i + 2, e - i - 2)).c_str());
            }
        }
        if (!end && at(s, i, "memcpy(&vr")) {
            size_t e = digits_end(s, i + 10);
            if (e > i + 10 && (e == s.size() || !w(s[e]))) {
                end = e;
                reg = std::atoi(std::string(s.substr(i + 10, e - i - 10)).c_str());
            }
        }
        if (end) {
            out.push_back({i, reg});
            i = end;
        } else {
            i++;
        }
    }
    return out;
}

// GOTO: the label names of every "goto L_XXX;"
std::vector<std::string> gotos(sv s) {
    std::vector<std::string> out;
    size_t i = 0;
    while ((i = s.find("goto L_", i)) != sv::npos) {
        if (bnd_before(s, i)) {
            size_t k = i + 7, e = k;
            while (e < s.size() && (dig(s[e]) || (s[e] >= 'A' && s[e] <= 'F'))) e++;
            if (e > k && e < s.size() && s[e] == ';') {
                out.emplace_back(s.substr(k, e - k));
                i = e + 1;
                continue;
            }
        }
        i += 1;
    }
    return out;
}

// LABEL on a stripped line
bool label_of(sv ln, std::string& name) {
    ln = strip(ln);
    if (!at(ln, 0, "L_") || ln.size() < 5 || !ends_with(ln, ":;")) return false;
    sv h = ln.substr(2, ln.size() - 4);
    if (h.empty()) return false;
    for (char ch : h)
        if (!(dig(ch) || (ch >= 'A' && ch <= 'F'))) return false;
    name = std::string(h);
    return true;
}

const std::set<std::string, std::less<>> PURE = {
    "nx::subs32", "nx::subs64", "nx::adds32", "nx::adds64", "nx::logic32", "nx::logic64", "nx::set_nzcv",
    "nx::get_nzcv", "nx::clrex", "nx::ldxr", "nx::stxr", "nx::ldxp", "nx::stxp", "nx::fcmp", "nx::fcmpe",
    "nx::mrs", "nx::msr"};
const char* RESTORE = "js_[jr_].restore(c);";

// _call_end: just past the ');' that closes the call whose '(' is at start
size_t call_end(sv s, size_t start) {
    int depth = 0;
    size_t k = start;
    while (k < s.size()) {
        char ch = s[k];
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
            if (depth == 0) {
                k++;
                break;
            }
        } else if (ch == '"') {
            size_t q = s.find('"', k + 1);
            if (q == sv::npos) throw std::runtime_error("regcache: unterminated string");
            k = q;
        }
        k++;
    }
    if (k < s.size() && s[k] == ';') k++;
    return k;
}

// _calls: (start, end) of the context-escaping calls of a statement
std::vector<std::pair<size_t, size_t>> calls(sv line) {
    std::vector<std::pair<size_t, size_t>> res;
    size_t pos = 0, i = 0;
    while (i < line.size()) {
        bool lb = i == 0 || !(w(line[i - 1]) || line[i - 1] == '.' || line[i - 1] == '>');
        size_t g1_end = 0, g2_end = 0;
        if (lb) {
            for (int pass = 0; pass < 2 && !g1_end; pass++) {
                size_t k = i;
                if (pass == 0) {  // (?:js_\[\w+\]\.)
                    if (!at(line, k, "js_[")) continue;
                    size_t e = k + 4;
                    while (e < line.size() && w(line[e])) e++;
                    if (e == k + 4 || !at(line, e, "].")) continue;
                    k = e + 2;
                }
                if (k >= line.size() || !(std::isalpha(uint8_t(line[k])) || line[k] == '_')) continue;
                size_t e = k + 1;
                while (e < line.size() && (w(line[e]) || line[e] == ':')) e++;
                size_t g2 = angle_group(line, e);
                if (at(line, g2, "(c") && g2 + 2 < line.size() && (line[g2 + 2] == ',' || line[g2 + 2] == ')')) {
                    g1_end = e, g2_end = g2;
                }
            }
        }
        if (!g1_end) {
            i++;
            continue;
        }
        size_t mstart = i, mend = g2_end + 3;
        sv g1 = line.substr(i, g1_end - i);
        if (!(mstart < pos || PURE.count(g1))) {
            size_t paren = g2_end;  // m.start() + len(group 1) + len(group 2)
            size_t end = call_end(line, paren);
            if (line[end - 1] != ';') throw std::runtime_error("regcache: call inside an expression: " + std::string(strip(line)));
            res.push_back({mstart, end});
            pos = end;
        }
        i = mend;
    }
    return res;
}

// _terminal: the statement never falls through to the next line
bool terminal(sv stmt) {
    size_t c = stmt.find(" // ");
    sv s = strip(stmt.substr(0, c));
    return at(s, 0, "goto ") || s == "return;" || at(s, 0, "throw") ||
           (at(s, 0, "{") && ends_with(s, "return; }") && !at(s, 0, "{ if")) ||
           (at(s, 0, "switch (") && contains(s, "default: nx::call")) || at(s, 0, "nx::unimpl(");
}

constexpr int FL = 32, VB = 33;

}  // namespace

std::vector<std::string> regcache_apply(std::vector<std::string> lines, size_t header_index) {
    const Settings& st = settings();
    if (!st.enabled) return lines;
    std::set<int> used_set, v_set;
    bool flags = false;
    for (const auto& ln : lines) {
        each_ctx_reg(ln, 'x', [&](size_t, size_t, int n) {
            if (st.cached.count(n)) used_set.insert(n);
        });
        if (st.vectors) each_ctx_reg(ln, 'v', [&](size_t, size_t, int n) { v_set.insert(n); });
        if (st.flags && !flags) {
            each_flag(ln, [&](size_t, size_t, sv) { flags = true; });
            if (!flags) each_flag_helper(ln, [&](size_t, size_t, size_t) { flags = true; });
        }
    }
    std::vector<int> used(used_set.begin(), used_set.end()), vused(v_set.begin(), v_set.end());
    if (used.empty() && !flags && vused.empty()) return lines;
    std::string fill_all;
    for (int n : used) fill_all += cat(" x", dec(n), " = c.x[", dec(n), "];");
    for (int n : vused) fill_all += cat(" vr", dec(n), " = c.v[", dec(n), "];");
    if (flags) fill_all += " fl_ = {c.nf, c.zf, c.cf, c.vf};";

    auto rewrite = [&](const std::string& ln) -> std::string {
        if (contains(ln, RESTORE)) return ln;
        std::string o;
        size_t p = 0;
        each_ctx_reg(ln, 'x', [&](size_t a, size_t e, int n) {
            o.append(ln, p, a - p);
            o += st.cached.count(n) ? cat("x", dec(n)) : ln.substr(a, e - a);
            p = e;
        });
        o.append(ln, p, std::string::npos);
        if (!vused.empty()) {
            std::string v;
            p = 0;
            each_ctx_reg(o, 'v', [&](size_t a, size_t e, int n) {
                v.append(o, p, a - p);
                v += cat("vr", dec(n));
                p = e;
            });
            v.append(o, p, std::string::npos);
            o = std::move(v);
        }
        if (flags) {
            std::string v;
            p = 0;
            each_flag(o, [&](size_t a, size_t e, sv name) {
                v.append(o, p, a - p);
                v += cat("fl_.", name);
                p = e;
            });
            v.append(o, p, std::string::npos);
            o = std::move(v);
            v.clear();
            p = 0;
            each_flag_helper(o, [&](size_t a, size_t, size_t g2) {
                v.append(o, p, a - p);
                v.append(o, a, g2 - a);  // the name and its <...>
                v += "(fl_";
                p = g2 + 2;  // the [,)] stays
            });
            v.append(o, p, std::string::npos);
            o = std::move(v);
        }
        return o;
    };
    for (auto& ln : lines) ln = rewrite(ln);
    size_t n = lines.size();

    struct Ev {
        size_t pos;
        int kind;  // 0 sync ("sync" < "w"), 1 write
        size_t val;
        bool operator<(const Ev& o) const { return std::tie(pos, kind, val) < std::tie(o.pos, o.kind, o.val); }
    };
    struct Info {
        std::vector<Ev> ev;
        bool cond = false;
        std::vector<size_t> succ;
    };
    std::vector<Info> info(n);
    std::map<std::string, size_t> labels;
    for (size_t k = 0; k < n; k++) {
        std::string name;
        if (label_of(lines[k], name)) labels[name] = k;
    }
    for (size_t k = 0; k < n; k++) {
        const std::string& ln = lines[k];
        if (k == header_index || contains(ln, RESTORE)) {
            if (k + 1 < n) info[k].succ.push_back(k + 1);
            continue;
        }
        sv code = sv(ln).substr(0, ln.find(" // "));
        auto& ev = info[k].ev;
        for (auto [p, r] : writes(code)) ev.push_back({p, 1, size_t(r)});
        for (size_t p : flag_writes(code)) ev.push_back({p, 1, size_t(FL)});
        for (auto [p, r] : vwrites(code)) ev.push_back({p, 1, size_t(VB + r)});
        for (auto [s0, e0] : calls(code)) ev.push_back({s0, 0, e0});
        std::sort(ev.begin(), ev.end());
        sv cs = strip(code);
        info[k].cond = at(cs, 0, "if ") || at(cs, 0, "if(") || at(cs, 0, "switch");
        for (auto& t : gotos(code)) {
            auto it = labels.find(t);
            if (it != labels.end()) info[k].succ.push_back(it->second);
        }
        if (!terminal(code) && k + 1 < n) info[k].succ.push_back(k + 1);
    }

    // forward may-dirty dataflow (bit masks: x0..x31, flags at 32, v0..v31 from 33), to a fixed point
    using Mask = std::bitset<VB + 32>;
    std::vector<Mask> din(n);
    auto transfer = [&](size_t k, Mask d) {
        for (const Ev& e : info[k].ev) {
            if (e.kind == 1) d.set(e.val);
            else if (!info[k].cond) d.reset();
        }
        return d;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t k = 0; k < n; k++) {
            Mask o = transfer(k, din[k]);
            for (size_t sk : info[k].succ) {
                Mask nd = din[sk] | o;
                if (nd != din[sk]) din[sk] = nd, changed = true;
            }
        }
    }
    auto spill_of = [&](const Mask& mask) {
        std::string sp;
        for (int r : used)
            if (mask.test(size_t(r))) sp += cat("c.x[", dec(r), "] = x", dec(r), "; ");
        for (int r : vused)
            if (mask.test(size_t(VB + r))) sp += cat("c.v[", dec(r), "] = vr", dec(r), "; ");
        if (flags && mask.test(FL)) sp += "c.nf = fl_.nf; c.zf = fl_.zf; c.cf = fl_.cf; c.vf = fl_.vf; ";
        return sp;
    };

    std::vector<std::string> res;
    res.reserve(n + 3);
    for (size_t k = 0; k < n; k++) {
        const std::string& ln = lines[k];
        if (k == header_index) {
            res.push_back(ln);
            if (!used.empty()) {
                std::vector<std::string> d;
                for (int r : used) d.push_back(cat("x", dec(r), " = c.x[", dec(r), "]"));
                res.push_back("    uint64_t " + join(d, ", ") + ";");
            }
            if (flags) res.push_back("    nx::Flags fl_{c.nf, c.zf, c.cf, c.vf};");
            if (!vused.empty()) {
                std::vector<std::string> d;
                for (int r : vused) d.push_back(cat("vr", dec(r), " = c.v[", dec(r), "]"));
                res.push_back("    nx::V128 " + join(d, ", ") + ";");
            }
            continue;
        }
        if (contains(ln, RESTORE)) {
            res.push_back(ln + fill_all);
            continue;
        }
        size_t cut = ln.find(" // ");
        std::string code = ln.substr(0, cut), rest = cut == std::string::npos ? "" : ln.substr(cut);
        Mask d = din[k];
        std::string out;
        size_t pos = 0;
        for (const Ev& e : info[k].ev) {
            if (e.kind == 1) {
                d.set(e.val);
                continue;
            }
            out.append(code, pos, e.pos - pos);
            out += cat("{ ", spill_of(d), code.substr(e.pos, e.val - e.pos), fill_all, " }");
            pos = e.val;
            if (!info[k].cond) d.reset();
        }
        out.append(code, pos, std::string::npos);
        if (contains(out, "return;")) {  // re.sub(r"\breturn;", "{ <spill>return; }", code)
            std::string rep = cat("{ ", spill_of(d), "return; }"), o2;
            size_t i = 0;
            while (i < out.size()) {
                if (at(out, i, "return;") && bnd_before(out, i)) {
                    o2 += rep;
                    i += 7;
                } else {
                    o2 += out[i++];
                }
            }
            out = std::move(o2);
        }
        res.push_back(out + rest);
    }
    return res;
}

}  // namespace rc
