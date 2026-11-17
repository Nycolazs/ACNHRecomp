// Python's formatting, as the recompiler (ACNHRecomp's recomp/, ported here) writes its C++ with f-strings: the
// generated code must come out byte for byte the same, so every number is printed the way Python prints it.
//   hx(v)        f"{v:#x}"   (a negative value as -0x..)
//   hx8(v)       f"{v:08X}"  (function and label names)
//   dec(v)       str(v)
//   pyfloat(d)   repr(d)     (the shortest round-trip digits, Python's choice of fixed or exponent form)
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <type_traits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rc {

inline std::string hx(int64_t v) {
    char b[24];
    if (v < 0) {
        std::snprintf(b, sizeof b, "-0x%llx", (unsigned long long)(-(uint64_t)v));
        return b;
    }
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}
inline std::string hxu(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}
inline std::string hx8(uint64_t v) {  // {v:08X}
    char b[24];
    std::snprintf(b, sizeof b, "%08llX", (unsigned long long)v);
    return b;
}
inline std::string hX(uint64_t v) {  // {v:X}
    char b[24];
    std::snprintf(b, sizeof b, "%llX", (unsigned long long)v);
    return b;
}
inline std::string hx010(uint64_t v) {  // {v:#010x}
    char b[24];
    std::snprintf(b, sizeof b, "0x%08llx", (unsigned long long)v);
    return b;
}
inline std::string dec(int64_t v) { return std::to_string(v); }

// repr(float): the shortest digits that read back as d; fixed form when the decimal exponent is in [-4, 16), else
// d.ddde+XX (at least two exponent digits); always a ".0" on a whole number in fixed form.
inline std::string pyfloat(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    // the shortest scientific form that reads back as d (std::to_chars' choice; not available before macOS 13.3)
    char buf[64];
    for (int prec = 0; prec <= 16; prec++) {
        std::snprintf(buf, sizeof buf, "%.*e", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    std::string s(buf);  // [-]D[.DDD]e[+-]XX
    bool neg = s[0] == '-';
    if (neg) s.erase(0, 1);
    size_t e = s.find('e');
    std::string mant = s.substr(0, e);
    int exp = std::stoi(s.substr(e + 1));
    std::string digits;  // the digits only: the C library writes the locale's decimal point ("2,5e+00" in a
                         // pt_BR or it_IT session on Linux, where GTK sets the locale)
    for (char ch : mant)
        if (ch >= '0' && ch <= '9') digits += ch;
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string out;
    if (exp >= -4 && exp < 16) {
        if (exp >= 0) {
            std::string ip = digits.substr(0, std::min<size_t>(digits.size(), size_t(exp) + 1));
            while (ip.size() < size_t(exp) + 1) ip += '0';
            std::string fp = digits.size() > size_t(exp) + 1 ? digits.substr(size_t(exp) + 1) : "0";
            out = ip + "." + fp;
        } else {
            out = "0." + std::string(size_t(-exp - 1), '0') + digits;
        }
    } else {
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        char eb[16];
        std::snprintf(eb, sizeof eb, "e%c%02d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
        out += eb;
    }
    return neg ? "-" + out : out;
}

// Concatenation of pieces: cat("a", x, "b") with strings, string_views, chars and C strings.
inline void cat_into(std::string&) {}
template <class T, class... R>
void cat_into(std::string& o, const T& t, const R&... r) {
    if constexpr (std::is_same_v<T, char>)
        o += t;
    else
        o += std::string_view(t);
    cat_into(o, r...);
}
template <class... R>
std::string cat(const R&... r) {
    std::string o;
    cat_into(o, r...);
    return o;
}

inline bool is_word(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; }

// re.sub(r"\b(name)\b", value, s) for one name made of word characters.
inline std::string replace_word(std::string_view s, std::string_view name, std::string_view value) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, name.size(), name) == 0 && (i == 0 || !is_word(s[i - 1])) &&
            (i + name.size() == s.size() || !is_word(s[i + name.size()]))) {
            out += value;
            i += name.size();
        } else {
            out += s[i++];
        }
    }
    return out;
}

inline bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
inline bool ends_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}
inline bool contains(std::string_view s, std::string_view p) { return s.find(p) != std::string_view::npos; }

// str.replace(a, b) (every occurrence) and str.replace(a, b, 1)
inline std::string replace_all(std::string s, std::string_view a, std::string_view b) {
    if (a.empty()) return s;
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) {
        s.replace(p, a.size(), b);
        p += b.size();
    }
    return s;
}
inline std::string replace_first(std::string s, std::string_view a, std::string_view b) {
    size_t p = s.find(a);
    if (p != std::string::npos) s.replace(p, a.size(), b);
    return s;
}

// str.strip() (ASCII whitespace)
inline std::string_view strip(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

// Python int arithmetic that the emitter masks to at most 64 bits: (1 << n) - 1 and v << n, exact in the low 64 bits
// (a shift by 64 or more leaves nothing there).
inline uint64_t mask_bits(int64_t n) { return n >= 64 ? ~0ull : n <= 0 ? 0 : (1ull << n) - 1; }
inline uint64_t shl64(uint64_t v, int64_t n) { return n >= 64 || n < 0 ? 0 : v << n; }

// Keyword arguments of str.format / subst: {name, value} pairs
using Kw = std::initializer_list<std::pair<std::string_view, std::string_view>>;

// template.format(name=value, ...): {name} fields and {{ / }} escapes (a missing name is Python's KeyError)
inline std::string pyformat(std::string_view t, Kw kw) {
    std::string o;
    for (size_t i = 0; i < t.size(); i++) {
        char ch = t[i];
        if (ch == '{' && i + 1 < t.size() && t[i + 1] == '{') {
            o += '{', i++;
        } else if (ch == '}' && i + 1 < t.size() && t[i + 1] == '}') {
            o += '}', i++;
        } else if (ch == '{') {
            size_t e = t.find('}', i);
            if (e == std::string_view::npos) throw std::runtime_error("format: single '{'");
            std::string_view name = t.substr(i + 1, e - i - 1);
            bool found = false;
            for (const auto& [k, v] : kw)
                if (k == name) {
                    o += v, found = true;
                    break;
                }
            if (!found) throw std::runtime_error("format: KeyError " + std::string(name));
            i = e;
        } else if (ch == '}') {
            throw std::runtime_error("format: single '}'");
        } else {
            o += ch;
        }
    }
    return o;
}

// emit.subst: re.sub(r"\b([abo])\b", ...) with the placeholders given (the others stay as they are), in one pass.
inline std::string subst(std::string_view expr, Kw vals) {
    std::string o;
    for (size_t i = 0; i < expr.size(); i++) {
        char ch = expr[i];
        if ((ch == 'a' || ch == 'b' || ch == 'o') && (i == 0 || !is_word(expr[i - 1])) &&
            (i + 1 == expr.size() || !is_word(expr[i + 1]))) {
            bool found = false;
            for (const auto& [k, v] : vals)
                if (k.size() == 1 && k[0] == ch) {
                    o += v, found = true;
                    break;
                }
            if (found) continue;
        }
        o += ch;
    }
    return o;
}

// str.isdigit (ASCII digits, non-empty)
inline bool isdigits(std::string_view s) {
    if (s.empty()) return false;
    for (char ch : s)
        if (ch < '0' || ch > '9') return false;
    return true;
}

// v[a:b] (a negative index counts from the end; out-of-range bounds are clamped, as Python's slices are)
template <class T>
std::vector<T> pyslice(const std::vector<T>& v, int64_t a, int64_t b) {
    int64_t n = int64_t(v.size());
    if (a < 0) a += n;
    if (b < 0) b += n;
    a = a < 0 ? 0 : a > n ? n : a;
    b = b < 0 ? 0 : b > n ? n : b;
    return b > a ? std::vector<T>(v.begin() + a, v.begin() + b) : std::vector<T>();
}

// " ".join / ", ".join over strings
template <class C>
std::string join(const C& parts, std::string_view sep) {
    std::string o;
    bool first = true;
    for (const auto& p : parts) {
        if (!first) o += sep;
        o += p;
        first = false;
    }
    return o;
}

}  // namespace rc
