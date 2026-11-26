// NookKit JSON patches (https://github.com/acnhmodding/nookkit): JSON files in a mod's romfs/NookKit/<kind>/, as
// NookKit's Skyline plugin reads them from content:/NookKit/<kind>:
//   Message  messages of the Message/<Archive>_<Locale>.sarc.zs archives (replaced or added)
//   Bcsv     rows of the Bcsv/<Table>.bcsv tables (replaced or added)
//   Pack     whole BYML files inside the Pack/<Archive>.pack archives (replaced)
// NookKit's other parts (pages, key guides, sequences, lists) are an API for Rust plugins, not data.
//
// NookKit hooks the game's loaders (MessageFile::findText, BcsvMgr::loadResource, FileCache::insert), at addresses
// known for 3.0.3 only. Here nothing of the game is hooked and no file is written: when the RomFS mounts, each file a
// patch touches is rebuilt in memory from the game's own (or a LayeredFS mod's) and served instead as a virtual file
// (hle_fs.cpp), on every version of the game. The game sizes the buffers of these files from
// System/Resource/ResourceSizeTable.srsizetable (a few hundred bytes to spare), so the entry of a file that grew is
// raised the same way, in memory.
//
// Each kind loads its files in name order within a mod, mods from the last listed to the first, so the first listed
// mod wins a label, row or file, as it wins a file in LayeredFS. A file name a higher mod also has is hidden.
//
// Message: a patch file holds one patch or an array of them (NookKit's nookkit-formats/src/msbt/patch.rs):
//   { "file": "Version", "archive": "LayoutMsg", "messages": [
//       { "label": "0001", "text": { "default": "NookKit + Ver.", "EUru": "..." }, "attributes": { "like": "0002" } } ] }
// "file" is the msbt's name in its archive, without ".msbt"; "archive" is optional (the archive that holds the file is
// looked up then). "text" is keyed by locale (USen, EUru, JPja, ...) or "default", with NookKit's {{...}} tags.
// "attributes": {"like": "<label>"} copies another label's ATR1 entry, {"hex": "..."} is the entry itself; a new
// label without them gets zeros.
//
// Bcsv (nookkit-formats/src/bcsv/patch.rs): { "file": "ItemParam", "rows": [ { "UniqueID u16": 2, ... }, ... ] }. A row
// maps column names (their CRC32 is the column's hash) or "0x%08X" hashes to a number, a string (text for a
// "... stringN" column, hex bytes otherwise) or {"crc32": "..."} / {"mmh3": "..."}, a string's hash.
//
// Pack (nookkit-formats/src/byml/patch.rs, Heaven Tool's PackPatch):
//   { "file": "Param/NpcHouse/wol13.byml", "archive": "StaticParam", "data": { ...the whole file as plain JSON... } }
// An integer is an Int (an Int64 past 32 bits), a number with a point or an exponent a Float.
#include "nookkit.h"

#include <zstd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string_view>

#include <nx/runtime.h>

namespace fs = std::filesystem;

namespace nx {
namespace nookkit {
namespace {

// ---- JSON -------------------------------------------------------------------------------------------------------

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* get(const char* key) const {
        for (auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }
};

void put_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | cp >> 6), out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | cp >> 12), out += char(0x80 | (cp >> 6 & 0x3F)), out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | cp >> 18), out += char(0x80 | (cp >> 12 & 0x3F)), out += char(0x80 | (cp >> 6 & 0x3F)),
            out += char(0x80 | (cp & 0x3F));
    }
}

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string error;

    bool fail(const char* what) {
        if (error.empty()) {
            size_t line = 1 + size_t(std::count(s.begin(), s.begin() + std::min(i, s.size()), '\n'));
            error = std::string(what) + " at line " + std::to_string(line);
        }
        return false;
    }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    }
    bool hex4(uint32_t& v) {
        if (i + 4 > s.size()) return fail("a short \\u escape");
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return fail("a bad \\u escape");
        }
        return true;
    }
    bool string(std::string& out) {
        i++;  // the opening quote
        for (;;) {
            if (i >= s.size()) return fail("an unterminated string");
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return fail("an unterminated string");
            switch (char e = s[i++]) {
                case '"': case '\\': case '/': out += e; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i, 2, "\\u") == 0) {
                        uint32_t lo;
                        i += 2;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else return fail("a lone surrogate");
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: return fail("an unknown escape");
            }
        }
    }
    bool value(Json& v, int depth = 0) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("a missing value");
        char c = s[i];
        if (c == '"') {
            v.kind = Json::String;
            return string(v.str);
        }
        if (c == '[') {
            v.kind = Json::Array;
            i++;
            ws();
            if (i < s.size() && s[i] == ']') return ++i, true;
            for (;;) {
                v.arr.emplace_back();
                if (!value(v.arr.back(), depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') return ++i, true;
                return fail("expected , or ]");
            }
        }
        if (c == '{') {
            v.kind = Json::Object;
            i++;
            ws();
            if (i < s.size() && s[i] == '}') return ++i, true;
            for (;;) {
                ws();
                if (i >= s.size() || s[i] != '"') return fail("expected a key");
                v.obj.emplace_back();
                if (!string(v.obj.back().first)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected :");
                i++;
                if (!value(v.obj.back().second, depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') return ++i, true;
                return fail("expected , or }");
            }
        }
        for (const char* lit : {"true", "false", "null"})
            if (s.compare(i, std::strlen(lit), lit) == 0) {
                v.kind = lit[0] == 'n' ? Json::Null : Json::Bool;
                v.str = lit;
                i += std::strlen(lit);
                return true;
            }
        size_t start = i;
        while (i < s.size() && std::strchr("+-0123456789.eE", s[i])) i++;
        if (i == start) return fail("an unexpected character");
        v.kind = Json::Number;
        v.str = s.substr(start, i - start);  // its text: 30 is an integer, 30.0 and 3e1 floats (serde_json's rule)
        return true;
    }
};

// ---- MsgStd text: NookKit's tags (nookkit-formats/src/msbt/tags.rs, encode) ---------------------------------------

constexpr char16_t kOpen = 0x0E, kClose = 0x0F;

void put_utf16(std::u16string& out, std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        auto b = uint8_t(s[i]);
        uint32_t cp;
        int n;
        if (b < 0x80) cp = b, n = 1;
        else if ((b & 0xE0) == 0xC0) cp = b & 0x1F, n = 2;
        else if ((b & 0xF0) == 0xE0) cp = b & 0x0F, n = 3;
        else cp = b & 0x07, n = 4;
        for (int k = 1; k < n && i + k < s.size(); k++) cp = cp << 6 | (uint8_t(s[i + k]) & 0x3F);
        i += size_t(n);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += char16_t(0xD800 + (cp >> 10));
            out += char16_t(0xDC00 + (cp & 0x3FF));
        } else {
            out += char16_t(cp);
        }
    }
}

std::string trim(std::string_view s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string_view::npos ? std::string() : std::string(s.substr(a, b - a + 1));
}

bool from_hex(const std::string& hex, std::string& out) {
    if (hex.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned v;
        if (!std::isxdigit(uint8_t(hex[i])) || !std::isxdigit(uint8_t(hex[i + 1])) ||
            std::sscanf(hex.c_str() + i, "%2x", &v) != 1)
            return false;
        out += char(v);
    }
    return true;
}

struct Attrs {
    std::vector<std::pair<std::string, std::string>> pairs;
    std::string error;

    bool parse(std::string_view s) {
        for (;;) {
            while (!s.empty() && std::isspace(uint8_t(s[0]))) s.remove_prefix(1);
            if (s.empty()) return true;
            size_t eq = s.find('=');
            if (eq == std::string_view::npos) return error = "expected key=\"value\" in \"" + std::string(s) + "\"", false;
            std::string key = trim(s.substr(0, eq)), value;
            s.remove_prefix(eq + 1);
            while (!s.empty() && std::isspace(uint8_t(s[0]))) s.remove_prefix(1);
            if (!s.empty() && s[0] == '"') {
                size_t k = 1;
                for (;; k++) {
                    if (k >= s.size()) return error = "unterminated value for " + key, false;
                    if (s[k] == '"') break;
                    if (s[k] == '\\') {
                        if (++k >= s.size()) return error = "an escape at the end of a value", false;
                    }
                    value += s[k];
                }
                s.remove_prefix(k + 1);
            } else {
                size_t end = 0;
                while (end < s.size() && !std::isspace(uint8_t(s[end]))) end++;
                value = std::string(s.substr(0, end));
                s.remove_prefix(end);
            }
            pairs.emplace_back(key, value);
        }
    }
    bool take(const char* key, std::string& out) {
        for (auto it = pairs.begin(); it != pairs.end(); ++it)
            if (it->first == key) {
                out = it->second;
                pairs.erase(it);
                return true;
            }
        return error = std::string("missing ") + key, false;
    }
    bool take_u16(const char* key, char16_t& out) {
        std::string v;
        if (!take(key, v)) return false;
        char* end = nullptr;
        unsigned long n = v.compare(0, 2, "0x") == 0 ? std::strtoul(v.c_str() + 2, &end, 16) : std::strtoul(v.c_str(), &end, 10);
        if (v.empty() || v == "0x" || *end || n > 0xFFFF) return error = std::string(key) + "=\"" + v + "\" isn't a u16", false;
        out = char16_t(n);
        return true;
    }
};

bool push_tag(std::u16string& out, char16_t group, char16_t kind, const std::string& params, std::string& error) {
    if (params.size() % 2 || params.size() > 0xFFFF) return error = "tag params must be an even number of bytes", false;
    out += {kOpen, group, kind, char16_t(params.size())};
    for (size_t i = 0; i < params.size(); i += 2) out += char16_t(uint8_t(params[i]) | uint8_t(params[i + 1]) << 8);
    return true;
}

std::string le16(char16_t v) { return {char(v & 0xFF), char(v >> 8)}; }

bool encode_tag(std::string_view body_in, std::u16string& out, std::string& error) {
    std::string body = trim(body_in);
    size_t sp = 0;
    while (sp < body.size() && !std::isspace(uint8_t(body[sp]))) sp++;
    std::string name = body.substr(0, sp);
    Attrs a;
    if (!a.parse(std::string_view(body).substr(sp))) return error = a.error, false;
    char16_t u = 0, v = 0;
    bool ok = true;
    if (name == "Ruby") {
        std::string value;
        if ((ok = a.take_u16("byteSpan", u) && a.take("value", value))) {
            std::u16string units;
            put_utf16(units, value);
            std::string params = le16(u) + le16(char16_t(units.size() * 2));
            for (char16_t c : units) params += le16(c);
            ok = push_tag(out, 0, 0, params, error);
        }
    } else if (name == "Font") {
        ok = a.take_u16("face", u) && push_tag(out, 0, 1, le16(u), error);
    } else if (name == "Size") {
        ok = a.take_u16("percent", u) && push_tag(out, 0, 2, le16(u), error);
    } else if (name == "Color") {
        ok = a.take_u16("index", u) && push_tag(out, 0, 3, le16(u), error);
    } else if (name == "PageBreak") {
        ok = push_tag(out, 0, 4, {}, error);
    } else if (name == "Tag") {
        std::string hex, data;
        if ((ok = a.take_u16("group", u) && a.take_u16("type", v))) {
            if (a.take("data", hex) && !from_hex(hex, data)) return error = "data=\"" + hex + "\" isn't hex", false;
            a.error.clear();
            ok = push_tag(out, u, v, data, error);
        }
    } else if (name == "/Tag") {
        if ((ok = a.take_u16("group", u) && a.take_u16("type", v))) out += {kClose, u, v};
    } else {
        return error = "unknown tag " + name, false;
    }
    if (!ok) {
        if (error.empty()) error = a.error;
        return false;
    }
    if (!a.pairs.empty()) return error = "unknown attribute " + a.pairs[0].first, false;
    return true;
}

bool encode_text(const std::string& text, std::u16string& out, std::string& error) {
    size_t p = 0;
    while (p < text.size()) {
        if (text.compare(p, 4, "{{{{") == 0) {
            out += u"{{";
            p += 4;
        } else if (text.compare(p, 2, "{{") == 0) {
            size_t end = text.find("}}", p + 2);
            if (end == std::string::npos) return error = "at byte " + std::to_string(p) + ": a tag without its closing }}", false;
            if (!encode_tag(std::string_view(text).substr(p + 2, end - p - 2), out, error)) {
                error = "at byte " + std::to_string(p) + ": " + error;
                return false;
            }
            p = end + 2;
        } else {
            size_t next = std::min(text.find("{{", p), text.size());
            put_utf16(out, std::string_view(text).substr(p, next - p));
            p = next;
        }
    }
    out += char16_t(0);
    return true;
}

// ---- bytes ------------------------------------------------------------------------------------------------------

uint16_t u16(const std::string& b, size_t at) { return uint16_t(uint8_t(b[at]) | uint8_t(b[at + 1]) << 8); }
uint32_t u32(const std::string& b, size_t at) { return uint32_t(u16(b, at)) | uint32_t(u16(b, at + 2)) << 16; }
void put32(std::string& b, size_t at, uint32_t v) {
    for (int k = 0; k < 4; k++) b[at + size_t(k)] = char(v >> (8 * k));
}
void add32(std::string& b, uint32_t v) {
    b.resize(b.size() + 4);
    put32(b, b.size() - 4, v);
}

// ---- MSBT (LBL1, ATR1, TXT2; little endian, UTF-16) --------------------------------------------------------------

struct Msbt {
    std::string head;  // the 0x20-byte header
    std::vector<std::pair<std::string, std::string>> sections;  // each section's 16-byte header, then its data
    std::vector<std::vector<std::pair<std::string, uint32_t>>> buckets;  // LBL1: labels by hash bucket, in file order
    std::vector<std::string> texts;  // TXT2, raw UTF-16 bytes
    bool has_atr = false;
    uint32_t atr_size = 0;
    std::vector<std::string> atrs;
    std::string atr_tail;  // whatever follows the ATR1 entries

    static uint32_t hash(const std::string& label, size_t buckets) {
        uint32_t h = 0;
        for (char c : label) h = h * 0x492 + uint8_t(c);
        return uint32_t(h % buckets);
    }
    bool parse(const std::string& b) {
        if (b.size() < 0x20 || b.compare(0, 8, "MsgStdBn") != 0 || uint8_t(b[8]) != 0xFF || uint8_t(b[9]) != 0xFE ||
            b[0xC] != 1)
            return false;  // big endian or not UTF-16: not the game's
        head = b.substr(0, 0x20);
        size_t at = 0x20;
        bool lbl = false, txt = false;
        for (unsigned n = u16(b, 0xE); n--;) {
            if (at + 16 > b.size()) return false;
            size_t size = u32(b, at + 4);
            if (at + 16 + size > b.size()) return false;
            std::string hdr = b.substr(at, 16), d = b.substr(at + 16, size);
            std::string magic = hdr.substr(0, 4);
            if (magic == "LBL1") {
                if (d.size() < 4) return false;
                uint32_t count = u32(d, 0);
                if (!count || 4 + size_t(count) * 8 > d.size()) return false;
                buckets.resize(count);
                for (uint32_t k = 0; k < count; k++) {
                    uint32_t labels = u32(d, 4 + k * 8);
                    size_t p = u32(d, 8 + k * 8);
                    while (labels--) {
                        if (p >= d.size() || p + 1 + uint8_t(d[p]) + 4 > d.size()) return false;
                        size_t len = uint8_t(d[p]);
                        buckets[k].emplace_back(d.substr(p + 1, len), u32(d, p + 1 + len));
                        p += 1 + len + 4;
                    }
                }
                lbl = true;
            } else if (magic == "TXT2") {
                if (d.size() < 4) return false;
                uint32_t count = u32(d, 0);
                if (4 + size_t(count) * 4 > d.size()) return false;
                for (uint32_t k = 0; k < count; k++) {
                    size_t s = u32(d, 4 + k * 4), e = k + 1 < count ? u32(d, 8 + k * 4) : d.size();
                    if (s > e || e > d.size()) return false;
                    texts.push_back(d.substr(s, e - s));
                }
                txt = true;
            } else if (magic == "ATR1") {
                if (d.size() < 8) return false;
                uint32_t count = u32(d, 0);
                atr_size = u32(d, 4);
                if (8 + uint64_t(count) * atr_size > d.size()) return false;
                for (uint32_t k = 0; k < count; k++) atrs.push_back(d.substr(8 + size_t(k) * atr_size, atr_size));
                atr_tail = d.substr(8 + size_t(count) * atr_size);
                has_atr = true;
            }
            sections.emplace_back(hdr, d);
            at = (at + 16 + size + 15) & ~size_t(15);
        }
        return lbl && txt && (!has_atr || atrs.size() == texts.size());
    }
    const uint32_t* index(const std::string& label) const {
        for (auto& bucket : buckets[hash(label, buckets.size())])
            if (bucket.first == label) return &bucket.second;
        return nullptr;
    }
    std::string write() const {
        std::string out = head;
        for (auto& [hdr, old] : sections) {
            std::string magic = hdr.substr(0, 4), d;
            if (magic == "LBL1") {
                add32(d, uint32_t(buckets.size()));
                size_t p = 4 + buckets.size() * 8;
                for (auto& bucket : buckets) {
                    add32(d, uint32_t(bucket.size()));
                    add32(d, uint32_t(p));
                    for (auto& l : bucket) p += 1 + l.first.size() + 4;
                }
                for (auto& bucket : buckets)
                    for (auto& [label, idx] : bucket) {
                        d += char(label.size());
                        d += label;
                        add32(d, idx);
                    }
            } else if (magic == "TXT2") {
                add32(d, uint32_t(texts.size()));
                size_t p = 4 + texts.size() * 4;
                for (auto& t : texts) add32(d, uint32_t(p)), p += t.size();
                for (auto& t : texts) d += t;
            } else if (magic == "ATR1") {
                add32(d, uint32_t(atrs.size()));
                add32(d, atr_size);
                for (auto& a : atrs) d += a;
                d += atr_tail;
            } else {
                d = old;
            }
            std::string h = hdr;
            put32(h, 4, uint32_t(d.size()));
            out += h;
            out += d;
            while (out.size() % 16) out += char(0xAB);
        }
        put32(out, 0x12, uint32_t(out.size()));
        return out;
    }
};

// The patch's messages put into one msbt for `locale`; the number changed.
int apply(Msbt& m, const FilePatch& patch, const std::string& locale, const std::string& file) {
    int changed = 0;
    for (auto& [label, msg] : patch.labels) {
        const std::u16string* units = msg.text_for(locale);
        if (!units) continue;
        if (label.empty() || label.size() > 255) {
            log("nookkit: %s: %s: label \"%s\" can't be stored", msg.source.c_str(), file.c_str(), label.c_str());
            continue;
        }
        const uint32_t* found = m.index(label);
        uint32_t idx;
        if (found) {
            idx = *found;
            if (idx >= m.texts.size()) continue;
        } else {
            idx = uint32_t(m.texts.size());
            m.texts.emplace_back();
            if (m.has_atr) m.atrs.emplace_back(m.atr_size, '\0');
            m.buckets[Msbt::hash(label, m.buckets.size())].emplace_back(label, idx);
        }
        std::string& t = m.texts[idx];
        t.clear();
        for (char16_t c : *units) t += le16(c);
        if (m.has_atr) {
            if (msg.attr == Message::Attr::Like) {
                if (const uint32_t* other = m.index(msg.like); other && *other < m.atrs.size()) m.atrs[idx] = m.atrs[*other];
                else log("nookkit: %s: %s %s: no label %s to take attributes from", msg.source.c_str(), file.c_str(),
                         label.c_str(), msg.like.c_str());
            } else if (msg.attr == Message::Attr::Bytes) {
                if (msg.bytes.size() == m.atr_size) m.atrs[idx] = msg.bytes;
                else log("nookkit: %s: %s attributes are %zu bytes, not %u", msg.source.c_str(), file.c_str(),
                         msg.bytes.size(), m.atr_size);
            }
        }
        changed++;
    }
    return changed;
}

// ---- SARC (little endian) ---------------------------------------------------------------------------------------

struct SarcFile {
    std::string name;
    size_t node, start, end;  // its SFAT node; its data, from the start of the archive
};

bool sarc_files(const std::string& d, std::vector<SarcFile>& out, size_t& data_off) {
    if (d.size() < 0x14 || d.compare(0, 4, "SARC") != 0 || uint8_t(d[6]) != 0xFF || uint8_t(d[7]) != 0xFE) return false;
    size_t hlen = u16(d, 4);
    data_off = u32(d, 0xC);
    if (hlen + 12 > d.size() || d.compare(hlen, 4, "SFAT") != 0) return false;
    size_t slen = u16(d, hlen + 4), n = u16(d, hlen + 6);
    size_t sfnt = hlen + slen + 16 * n;
    if (sfnt + 8 > d.size() || data_off > d.size()) return false;
    for (size_t i = 0; i < n; i++) {
        size_t node = hlen + slen + 16 * i;
        uint32_t attr = u32(d, node + 4);
        size_t s = data_off + u32(d, node + 8), e = data_off + u32(d, node + 12);
        if (s > e || e > d.size()) return false;
        std::string name;
        if (attr & 0x01000000) {
            size_t p = sfnt + 8 + size_t(attr & 0xFFFF) * 4;
            if (p >= d.size()) return false;
            name = d.c_str() + p;
        }
        out.push_back({name, node, s, e});
    }
    return true;
}

// The archive with some files' contents replaced; files keep their order and the original's alignment
// (tools/msg_rewrap.py, sarc_replace).
std::string sarc_replace(const std::string& d, const std::vector<SarcFile>& files, size_t data_off,
                         const std::map<std::string, std::string>& repl) {
    size_t align = 0x2000;
    for (auto& f : files)
        while ((f.start - data_off) % align) align /= 2;
    align = std::max<size_t>(align, 8);
    std::string head = d.substr(0, data_off), body;
    for (auto& f : files) {
        auto it = repl.find(f.name);
        while (body.size() % align) body += '\0';
        size_t start = body.size();
        if (it != repl.end()) body += it->second;
        else body.append(d, f.start, f.end - f.start);
        put32(head, f.node + 8, uint32_t(start));
        put32(head, f.node + 12, uint32_t(body.size()));
    }
    std::string out = head + body;
    put32(out, 8, uint32_t(out.size()));
    return out;
}

// ---- zstd, Yaz0 -------------------------------------------------------------------------------------------------

bool unzstd(const std::string& in, std::string& out) {
    ZSTD_DStream* z = ZSTD_createDStream();
    if (!z) return false;
    ZSTD_initDStream(z);
    ZSTD_inBuffer src{in.data(), in.size(), 0};
    std::string chunk(ZSTD_DStreamOutSize(), '\0');
    size_t r = 1;
    while (src.pos < src.size && r) {
        ZSTD_outBuffer dst{chunk.data(), chunk.size(), 0};
        r = ZSTD_decompressStream(z, &dst, &src);
        if (ZSTD_isError(r)) break;
        out.append(chunk.data(), dst.pos);
    }
    ZSTD_freeDStream(z);
    return !ZSTD_isError(r) && r == 0;
}

std::string zstd(const std::string& in) {
    std::string out(ZSTD_compressBound(in.size()), '\0');
    size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3);
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
}

bool unyaz0(const std::string& in, std::string& out) {
    if (in.size() < 16 || in.compare(0, 4, "Yaz0") != 0) return false;
    size_t size = size_t(uint8_t(in[4])) << 24 | size_t(uint8_t(in[5])) << 16 | size_t(uint8_t(in[6])) << 8 | uint8_t(in[7]);
    out.clear();
    out.reserve(size);
    size_t p = 16;
    while (out.size() < size) {
        if (p >= in.size()) return false;
        uint8_t code = uint8_t(in[p++]);
        for (int bit = 7; bit >= 0 && out.size() < size; bit--) {
            if (code >> bit & 1) {
                if (p >= in.size()) return false;
                out += in[p++];
                continue;
            }
            if (p + 2 > in.size()) return false;
            uint8_t b1 = uint8_t(in[p]), b2 = uint8_t(in[p + 1]);
            p += 2;
            size_t dist = (size_t(b1 & 0xF) << 8 | b2) + 1, n = b1 >> 4;
            if (n == 0) {
                if (p >= in.size()) return false;
                n = size_t(uint8_t(in[p++])) + 0x12;
            } else {
                n += 2;
            }
            if (dist > out.size()) return false;
            for (size_t k = 0; k < n && out.size() < size; k++) out += out[out.size() - dist];
        }
    }
    return true;
}

// Literals only: the table is read once, a smaller file buys nothing.
std::string yaz0(const std::string& in, const std::string& header) {
    std::string out = header.substr(0, 16);
    for (int k = 0; k < 4; k++) out[4 + size_t(k)] = char(in.size() >> (24 - 8 * k));
    for (size_t p = 0; p < in.size(); p += 8) {
        out += char(0xFF);
        out.append(in, p, std::min<size_t>(8, in.size() - p));
    }
    return out;
}

uint32_t crc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFF;
    for (char ch : s) {
        c ^= uint8_t(ch);
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320 & (0 - (c & 1)));
    }
    return ~c;
}

bool decode_patch(const std::string& source, const Json& p, std::string& file, FilePatch& out, std::string& error) {
    if (p.kind != Json::Object) return error = "a patch isn't an object", false;
    for (auto& [k, v] : p.obj)
        if (k != "file" && k != "archive" && k != "messages") return error = "unknown field \"" + k + "\"", false;
    const Json* f = p.get("file");
    const Json* a = p.get("archive");
    const Json* msgs = p.get("messages");
    if (!f || f->kind != Json::String) return error = "missing \"file\"", false;
    if (!msgs || msgs->kind != Json::Array) return error = file + ": missing \"messages\"", false;
    if (a && a->kind != Json::String) return error = "\"archive\" isn't a string", false;
    file = f->str;
    if (a) out.archive = a->str;
    for (const Json& m : msgs->arr) {
        if (m.kind != Json::Object) return error = file + ": a message isn't an object", false;
        for (auto& [k, v] : m.obj)
            if (k != "label" && k != "text" && k != "attributes") return error = file + ": unknown field \"" + k + "\"", false;
        const Json* label = m.get("label");
        const Json* text = m.get("text");
        const Json* attrs = m.get("attributes");
        if (!label || label->kind != Json::String) return error = file + ": a message without \"label\"", false;
        if (!text || text->kind != Json::Object) return error = file + " " + label->str + ": missing \"text\"", false;
        Message msg;
        msg.source = source;
        for (auto& [locale, value] : text->obj) {
            if (value.kind != Json::String) return error = file + " " + label->str + " [" + locale + "] isn't a string", false;
            std::string e;
            if (!encode_text(value.str, msg.text[locale], e))
                return error = file + " " + label->str + " [" + locale + "] " + e, false;
        }
        if (attrs && attrs->kind != Json::Null) {
            const Json* like = attrs->get("like");
            const Json* hex = attrs->get("hex");
            if (attrs->kind != Json::Object || attrs->obj.size() != 1 || (!like && !hex))
                return error = file + " " + label->str + ": \"attributes\" takes one of \"like\" or \"hex\"", false;
            const Json* v = like ? like : hex;
            if (v->kind != Json::String) return error = file + " " + label->str + ": attributes aren't a string", false;
            if (like) {
                msg.attr = Message::Attr::Like, msg.like = v->str;
            } else {
                msg.attr = Message::Attr::Bytes;
                if (!from_hex(v->str, msg.bytes))
                    return error = file + " " + label->str + ": attributes \"" + v->str + "\" isn't hex", false;
            }
        }
        out.labels[label->str] = std::move(msg);
    }
    return true;
}

// One patch or an array of them: each through `decode`, all or nothing.
template <typename T, typename F>
bool decode_all(const std::string& json, std::vector<T>& out, std::string* error, F decode) {
    Parser parser{json};
    Json root;
    std::string e;
    if (!parser.value(root)) e = parser.error;
    else if ((parser.ws(), parser.i != json.size())) e = "trailing characters";
    if (e.empty()) {
        if (root.kind == Json::Array) {
            for (const Json& p : root.arr) {
                out.emplace_back();
                if (!decode(p, out.back(), e)) break;
            }
        } else {
            out.emplace_back();
            decode(root, out.back(), e);
        }
    }
    if (!e.empty() && error) *error = e;
    return e.empty();
}

bool only_fields(const Json& o, std::initializer_list<const char*> names, std::string& error) {
    if (o.kind != Json::Object) return error = "a patch isn't an object", false;
    for (auto& [k, v] : o.obj)
        if (std::none_of(names.begin(), names.end(), [&](const char* n) { return k == n; }))
            return error = "unknown field \"" + k + "\"", false;
    return true;
}

bool is_float_text(const std::string& t) { return t.find_first_of(".eE") != std::string::npos; }

bool integer_text(const std::string& t, int64_t& v) {
    errno = 0;
    char* end = nullptr;
    v = std::strtoll(t.c_str(), &end, 10);
    return !t.empty() && !*end && errno == 0;
}

// ---- BCSV (nookkit-formats/src/bcsv) -------------------------------------------------------------------------------

// MurmurHash3 x86_32, seed 0: some tables hash their values with it instead of CRC32.
uint32_t mmh3(const std::string& s) {
    auto rotl = [](uint32_t x, int r) { return x << r | x >> (32 - r); };
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    uint32_t h = 0;
    size_t blocks = s.size() / 4 * 4;
    for (size_t i = 0; i < blocks; i += 4) {
        h ^= rotl(u32(s, i) * c1, 15) * c2;
        h = rotl(h, 13) * 5 + 0xe6546b64;
    }
    if (s.size() > blocks) {
        uint32_t k = 0;
        for (size_t i = s.size(); i-- > blocks;) k = k << 8 | uint8_t(s[i]);
        h ^= rotl(k * c1, 15) * c2;
    }
    h ^= uint32_t(s.size());
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    return h ^ (h >> 16);
}

// The columns that identify a row, the first one a table has (bcsv/patch.rs KEY_COLUMNS).
const char* const kKeyColumns[] = {"UniqueID u16",       "Label string8",          "ItemUniqueID u16",
                                   "StateName string48", "NumberingId u16",        "ResourceName string33",
                                   "StageName string32", "ItemFrom string32",      "TVProgramName.hshCstringRef",
                                   "GroundAttributeUniqueID u16",                  "NpcRoleSetID u16"};
constexpr uint32_t kUnnamedKeyColumn = 0x37571146;  // the MessageCardSelect tables' key, whose name isn't known

bool decode_cell(const std::string& key, const Json& v, BcsvCell& c, std::string& error) {
    c.key = key;
    if (key.size() == 10 && key.compare(0, 2, "0x") == 0) {
        char* end = nullptr;
        c.hash = uint32_t(std::strtoul(key.c_str() + 2, &end, 16));
        if (*end || !std::isxdigit(uint8_t(key[2]))) return error = key + ": not a column hash", false;
    } else {
        c.hash = crc32(key);
    }
    if (v.kind == Json::Number) {
        if (is_float_text(v.str)) {
            float f = float(std::strtod(v.str.c_str(), nullptr));
            c.kind = BcsvCell::Float;
            std::memcpy(&c.value, &f, 4);
            return true;
        }
        int64_t n;
        if (integer_text(v.str, n) && n >= 0 && n < (int64_t(1) << 32)) {
            c.kind = BcsvCell::Unsigned, c.value = uint32_t(n);
            return true;
        }
        if (integer_text(v.str, n) && n >= INT32_MIN && n < 0) {
            c.kind = BcsvCell::Signed, c.value = uint32_t(int32_t(n));
            return true;
        }
        return error = key + ": " + v.str + " doesn't fit 32 bits", false;
    }
    if (v.kind == Json::String) {
        if (key.find(" string") != std::string::npos) {
            c.kind = BcsvCell::Text, c.bytes = v.str;
            return true;
        }
        c.kind = BcsvCell::Bytes;
        if (!from_hex(v.str, c.bytes)) return error = key + ": \"" + v.str + "\" isn't hex", false;
        return true;
    }
    if (v.kind == Json::Object && v.obj.size() == 1 && v.obj[0].second.kind == Json::String &&
        (v.obj[0].first == "crc32" || v.obj[0].first == "mmh3")) {
        c.kind = BcsvCell::Hash;
        c.value = v.obj[0].first == "crc32" ? crc32(v.obj[0].second.str) : mmh3(v.obj[0].second.str);
        return true;
    }
    return error = key + ": expected a number, a string or {\"crc32\" | \"mmh3\": string}", false;
}

bool decode_bcsv(const std::string& source, const Json& p, std::pair<std::string, BcsvSource>& out, std::string& error) {
    if (!only_fields(p, {"file", "rows"}, error)) return false;
    const Json* f = p.get("file");
    const Json* rows = p.get("rows");
    if (!f || f->kind != Json::String) return error = "missing \"file\"", false;
    if (!rows || rows->kind != Json::Array) return error = f->str + ": missing \"rows\"", false;
    out.first = f->str;
    out.second.name = source;
    for (const Json& r : rows->arr) {
        if (r.kind != Json::Object) return error = f->str + ": a row isn't an object", false;
        auto& row = out.second.rows.emplace_back();
        for (auto& [key, v] : r.obj)
            if (!decode_cell(key, v, row.emplace_back(), error)) return false;
    }
    return true;
}

struct BcsvColumn {
    uint32_t hash;
    size_t offset, size;
};

int64_t cell_integer(const BcsvCell& c) { return c.kind == BcsvCell::Signed ? int64_t(int32_t(c.value)) : int64_t(c.value); }

std::string cell_fits(const BcsvCell& c, size_t size) {
    switch (c.kind) {
        case BcsvCell::Unsigned:
        case BcsvCell::Signed: {
            int64_t v = cell_integer(c), bits = int64_t(size) * 8;  // a u8 that ends a row is padded to 4
            if (size < 1 || size > 4 || v < -(int64_t(1) << (bits - 1)) || v >= (int64_t(1) << bits))
                return c.key + ": " + std::to_string(v) + " doesn't fit " + std::to_string(size) + " bytes";
            return {};
        }
        case BcsvCell::Float:
        case BcsvCell::Hash:
            return size == 4 ? std::string() : c.key + ": the column is " + std::to_string(size) + " bytes, not 4";
        case BcsvCell::Text:
            return c.bytes.size() < size ? std::string()
                                         : c.key + ": text of " + std::to_string(c.bytes.size()) + " bytes in " +
                                               std::to_string(size) + " (a NUL must fit)";
        case BcsvCell::Bytes:
            return c.bytes.size() == size ? std::string()
                                          : c.key + ": " + std::to_string(c.bytes.size()) + " bytes, the column has " +
                                                std::to_string(size);
    }
    return {};
}

void write_cell(const BcsvCell& c, char* out, size_t size) {
    std::memset(out, 0, size);
    if (c.kind == BcsvCell::Unsigned || c.kind == BcsvCell::Signed) {
        uint64_t v = uint64_t(cell_integer(c));
        for (size_t k = 0; k < size && k < 8; k++) out[k] = char(v >> (8 * k));
    } else if (c.kind == BcsvCell::Float || c.kind == BcsvCell::Hash) {
        for (size_t k = 0; k < size && k < 4; k++) out[k] = char(c.value >> (8 * k));
    } else {
        std::memcpy(out, c.bytes.data(), std::min(size, c.bytes.size()));
    }
}

// ---- BYML (nookkit-formats/src/byml/document.rs) ------------------------------------------------------------------

uint8_t byml_type(const BymlNode& n) {
    switch (n.kind) {
        case BymlNode::String: return 0xA0;
        case BymlNode::Array: return 0xC0;
        case BymlNode::Dict: return 0xC1;
        case BymlNode::Bool: return 0xD0;
        case BymlNode::Int: return 0xD1;
        case BymlNode::Float: return 0xD2;
        case BymlNode::Int64: return 0xD4;
        case BymlNode::Null: return 0xFF;
    }
    return 0xFF;
}

struct BymlWriter {
    std::string out;
    bool be = false;
    std::vector<std::string> keys, strings;

    void put(uint64_t v, int n) {
        for (int k = 0; k < n; k++) out += char(v >> (8 * (be ? n - 1 - k : k)));
    }
    void fill32(size_t at, uint32_t v) {
        for (int k = 0; k < 4; k++) out[at + size_t(k)] = char(v >> (8 * (be ? 3 - k : k)));
    }
    void align4() {
        while (out.size() % 4) out += '\0';
    }
    static uint32_t index(const std::vector<std::string>& t, const std::string& s) {
        return uint32_t(std::lower_bound(t.begin(), t.end(), s) - t.begin());
    }
    uint32_t table(const std::vector<std::string>& t) {
        if (t.empty()) return 0;
        uint32_t start = uint32_t(out.size());
        put(0xC2, 1), put(t.size(), 3);
        size_t at = 4 + (t.size() + 1) * 4;
        for (auto& s : t) put(at, 4), at += s.size() + 1;
        put(at, 4);
        for (auto& s : t) out += s, out += '\0';
        align4();
        return start;
    }
    bool value(const BymlNode& n, uint32_t& v, int depth = 0) {
        if (depth > 256 || out.size() > 0xFFFFFFF0) return false;
        switch (n.kind) {
            case BymlNode::String: v = index(strings, n.str); return true;
            case BymlNode::Bool: v = n.num != 0; return true;
            case BymlNode::Int: v = uint32_t(int32_t(n.num)); return true;
            case BymlNode::Float: std::memcpy(&v, &n.f, 4); return true;
            case BymlNode::Null: v = 0; return true;
            case BymlNode::Int64: v = uint32_t(out.size()), put(uint64_t(n.num), 8); return true;
            case BymlNode::Array: {
                if (n.arr.size() > 0xFFFFFF) return false;
                v = uint32_t(out.size());
                put(0xC0, 1), put(n.arr.size(), 3);
                for (auto& item : n.arr) put(byml_type(item), 1);
                align4();
                size_t slots = out.size();
                out.resize(out.size() + n.arr.size() * 4);
                for (size_t k = 0; k < n.arr.size(); k++) {
                    uint32_t item;
                    if (!value(n.arr[k], item, depth + 1)) return false;
                    fill32(slots + k * 4, item);
                }
                return true;
            }
            case BymlNode::Dict: {
                if (n.dict.size() > 0xFFFFFF) return false;
                v = uint32_t(out.size());
                put(0xC1, 1), put(n.dict.size(), 3);
                std::vector<size_t> slots;
                for (auto& [key, item] : n.dict) {
                    put(index(keys, key), 3), put(byml_type(item), 1);
                    slots.push_back(out.size());
                    out.resize(out.size() + 4);
                }
                size_t k = 0;
                for (auto& [key, item] : n.dict) {
                    uint32_t x;
                    if (!value(item, x, depth + 1)) return false;
                    fill32(slots[k++], x);
                }
                return true;
            }
        }
        return false;
    }
};

bool collect_strings(const BymlNode& n, std::set<std::string>& keys, std::set<std::string>& strings, int depth = 0) {
    if (depth > 256) return false;
    if (n.kind == BymlNode::String) {
        if (n.str.find('\0') != std::string::npos) return false;
        strings.insert(n.str);
    }
    for (auto& item : n.arr)
        if (!collect_strings(item, keys, strings, depth + 1)) return false;
    for (auto& [key, item] : n.dict) {
        if (key.find('\0') != std::string::npos) return false;
        keys.insert(key);
        if (!collect_strings(item, keys, strings, depth + 1)) return false;
    }
    return true;
}

bool byml_from_json(const Json& j, BymlNode& n, std::string& error, int depth = 0) {
    if (depth > 256) return error = "nesting too deep", false;
    switch (j.kind) {
        case Json::Null: n.kind = BymlNode::Null; return true;
        case Json::Bool: n.kind = BymlNode::Bool, n.num = j.str == "true"; return true;
        case Json::String: n.kind = BymlNode::String, n.str = j.str; return true;
        case Json::Number:
            if (is_float_text(j.str)) {
                n.kind = BymlNode::Float, n.f = float(std::strtod(j.str.c_str(), nullptr));
                return true;
            }
            if (!integer_text(j.str, n.num)) return error = j.str + " doesn't fit 64 bits", false;
            n.kind = n.num >= INT32_MIN && n.num <= INT32_MAX ? BymlNode::Int : BymlNode::Int64;
            return true;
        case Json::Array:
            n.kind = BymlNode::Array;
            for (auto& item : j.arr)
                if (!byml_from_json(item, n.arr.emplace_back(), error, depth + 1)) return false;
            return true;
        case Json::Object:
            n.kind = BymlNode::Dict;
            for (auto& [key, item] : j.obj) {
                if (n.dict.count(key)) return error = "the key \"" + key + "\" twice", false;
                if (!byml_from_json(item, n.dict[key], error, depth + 1)) return false;
            }
            return true;
    }
    return false;
}

bool decode_pack(const std::string& source, const Json& p, std::tuple<std::string, std::string, PackFile>& out,
                 std::string& error) {
    if (!only_fields(p, {"file", "archive", "data"}, error)) return false;
    const Json* f = p.get("file");
    const Json* a = p.get("archive");
    const Json* d = p.get("data");
    if (!f || f->kind != Json::String) return error = "missing \"file\"", false;
    if (!a || a->kind != Json::String) return error = f->str + ": missing \"archive\"", false;
    if (!d) return error = f->str + ": missing \"data\"", false;
    std::get<0>(out) = a->str;
    std::get<1>(out) = f->str;
    std::get<2>(out).source = source;
    if (!byml_from_json(*d, std::get<2>(out).root, error)) return error = f->str + ": " + error, false;
    return true;
}

}  // namespace

const std::u16string* Message::text_for(const std::string& locale) const {
    auto it = text.find(locale);
    if (it == text.end()) it = text.find("default");
    return it == text.end() ? nullptr : &it->second;
}

bool Patches::add(const std::string& source, const std::string& json, std::string* error) {
    Parser parser{json};
    Json root;
    std::string e;
    if (!parser.value(root)) e = parser.error;
    else if ((parser.ws(), parser.i != json.size())) e = "trailing characters";
    std::vector<std::pair<std::string, FilePatch>> decoded;
    if (e.empty()) {
        if (root.kind == Json::Array) {
            for (const Json& p : root.arr) {
                decoded.emplace_back();
                if (!decode_patch(source, p, decoded.back().first, decoded.back().second, e)) break;
            }
        } else {
            decoded.emplace_back();
            decode_patch(source, root, decoded.back().first, decoded.back().second, e);
        }
    }
    if (!e.empty()) {
        if (error) *error = e;
        return false;
    }
    for (auto& [file, patch] : decoded) {
        FilePatch& into = files[file];
        if (!patch.archive.empty()) into.archive = patch.archive;
        for (auto& [label, msg] : patch.labels) {
            auto old = into.labels.find(label);
            if (old != into.labels.end() && old->second.source != source)
                log("nookkit: message patch %s replaces %s %s from %s", source.c_str(), file.c_str(), label.c_str(),
                    old->second.source.c_str());
            into.labels[label] = std::move(msg);
        }
    }
    return true;
}

size_t Patches::count() const {
    size_t n = 0;
    for (auto& [f, p] : files) n += p.labels.size();
    return n;
}

std::vector<std::string> archive_files(const std::string& sarc_zs) {
    std::string raw;
    std::vector<SarcFile> files;
    size_t data_off = 0;
    std::vector<std::string> names;
    if (unzstd(sarc_zs, raw) && sarc_files(raw, files, data_off))
        for (auto& f : files) names.push_back(f.name);
    return names;
}

std::string patch_archive(const std::string& sarc_zs, const std::string& locale,
                          const std::map<std::string, const FilePatch*>& patches, int* messages, size_t* old_size,
                          size_t* new_size) {
    *messages = 0;
    std::string raw;
    std::vector<SarcFile> files;
    size_t data_off = 0;
    if (!unzstd(sarc_zs, raw) || !sarc_files(raw, files, data_off)) return {};
    std::map<std::string, std::string> repl;
    for (auto& f : files) {
        if (f.name.size() < 5 || f.name.compare(f.name.size() - 5, 5, ".msbt") != 0) continue;
        std::string file = f.name.substr(0, f.name.size() - 5);
        auto it = patches.find(file);
        if (it == patches.end()) continue;
        Msbt m;
        if (!m.parse(raw.substr(f.start, f.end - f.start))) {
            log("nookkit: %s (%s) isn't an MSBT it can patch", f.name.c_str(), locale.c_str());
            continue;
        }
        if (int n = apply(m, *it->second, locale, file)) {
            repl[f.name] = m.write();
            *messages += n;
        }
    }
    if (repl.empty()) return {};
    std::string out = sarc_replace(raw, files, data_off, repl);
    *old_size = raw.size();
    *new_size = out.size();
    return zstd(out);
}

std::string raise_rstb(const std::string& table, const std::map<std::string, int64_t>& growth) {
    bool packed = table.compare(0, 4, "Yaz0") == 0;
    std::string t;
    if (packed ? !unyaz0(table, t) : (t = table, false)) return {};
    if (t.size() < 12 || t.compare(0, 4, "RSTC") != 0) return {};
    size_t unique = u32(t, 4), collided = u32(t, 8);
    if (12 + unique * 12 + collided * (128 + 8) > t.size()) return {};
    for (auto& [name, add] : growth) {
        size_t at = 0;
        for (size_t k = 0; k < collided && !at; k++) {
            size_t e = 12 + unique * 12 + k * 136;
            if (std::strncmp(t.c_str() + e, name.c_str(), 128) == 0) at = e + 128;
        }
        uint32_t crc = crc32(name);
        for (size_t k = 0; k < unique && !at; k++)
            if (u32(t, 12 + k * 12) == crc) at = 12 + k * 12 + 4;
        if (!at) continue;  // not in the table: the game sizes the file itself
        put32(t, at, uint32_t(std::min<int64_t>(int64_t(u32(t, at)) + add, 0xFFFFFFFF)));
    }
    return packed ? yaz0(t, table) : t;
}

int roundtrip_failures(const std::string& sarc_zs) {
    std::string raw;
    std::vector<SarcFile> files;
    size_t data_off = 0;
    if (!unzstd(sarc_zs, raw) || !sarc_files(raw, files, data_off)) return -1;
    int bad = 0;
    for (auto& f : files) {
        Msbt m;
        std::string b = raw.substr(f.start, f.end - f.start);
        bad += !m.parse(b) || m.write() != b;
    }
    return bad;
}

std::u16string message_text(const std::string& sarc_zs, const std::string& file, const std::string& label) {
    std::string raw;
    std::vector<SarcFile> files;
    size_t data_off = 0;
    if (!unzstd(sarc_zs, raw) || !sarc_files(raw, files, data_off)) return {};
    for (auto& f : files) {
        Msbt m;
        if (f.name != file + ".msbt" || !m.parse(raw.substr(f.start, f.end - f.start))) continue;
        const uint32_t* idx = m.index(label);
        if (!idx || *idx >= m.texts.size()) return {};
        std::u16string out;
        for (size_t k = 0; k + 1 < m.texts[*idx].size(); k += 2) out += char16_t(u16(m.texts[*idx], k));
        return out;
    }
    return {};
}

std::string sarc_file(const std::string& sarc, const std::string& name) {
    std::vector<SarcFile> files;
    size_t data_off = 0;
    if (sarc_files(sarc, files, data_off))
        for (auto& f : files)
            if (f.name == name) return sarc.substr(f.start, f.end - f.start);
    return {};
}

bool BcsvPatches::add(const std::string& source, const std::string& json, std::string* error) {
    std::vector<std::pair<std::string, BcsvSource>> decoded;
    if (!decode_all(json, decoded, error, [&](const Json& p, auto& out, std::string& e) { return decode_bcsv(source, p, out, e); }))
        return false;
    for (auto& [file, src] : decoded) files[file].push_back(std::move(src));
    return true;
}

std::string BcsvPatches::apply(const std::string& file, const std::string& d, int* replaced, int* added) const {
    *replaced = *added = 0;
    auto sources = files.find(file);
    if (sources == files.end() || d.size() < 12) return {};
    size_t rows = u32(d, 0), row_size = u32(d, 4), nfields = u16(d, 8);
    size_t p = 12;
    if (d[10]) {  // an ExtHeader before the fields
        if (d.size() < p + 16 || d.compare(p, 4, "VSCB") != 0) return {};
        p += 16;
    }
    size_t rows_at = p + nfields * 8;
    if (rows_at > d.size() || (rows && row_size > (d.size() - rows_at) / rows)) {
        log("nookkit: bcsv %s: not a table it can read", file.c_str());
        return {};
    }
    std::vector<BcsvColumn> cols;
    for (size_t k = 0; k < nfields; k++) {
        size_t off = u32(d, p + k * 8 + 4), next = k + 1 < nfields ? u32(d, p + k * 8 + 12) : row_size;
        if (next < off) {  // sizes come from the offsets, which the files list in order
            log("nookkit: bcsv %s: its columns are out of order", file.c_str());
            return {};
        }
        cols.push_back({u32(d, p + k * 8), off, next - off});
    }
    auto column = [&](uint32_t hash) -> const BcsvColumn* {
        for (auto& c : cols)
            if (c.hash == hash) return &c;
        return nullptr;
    };
    const BcsvColumn* key = nullptr;
    for (const char* name : kKeyColumns)
        if (!key) key = column(crc32(name));
    if (!key) key = column(kUnnamedKeyColumn);
    if (key && !key->size) key = nullptr;

    // The sources whose every cell fits the table, then the row each of their rows goes to.
    std::vector<std::pair<const std::vector<BcsvCell>*, size_t>> patched;
    std::string added_keys;
    for (const BcsvSource& src : sources->second) {
        std::string why;
        for (auto& row : src.rows)
            for (auto& c : row)
                if (why.empty()) {
                    const BcsvColumn* col = column(c.hash);
                    why = col ? cell_fits(c, col->size) : c.key + ": the table has no such column";
                }
        if (!why.empty()) {
            log("nookkit: bcsv patch %s on %s skipped: %s", src.name.c_str(), file.c_str(), why.c_str());
            continue;
        }
        for (auto& row : src.rows) {
            size_t target = SIZE_MAX;
            std::string probe(key ? key->size : 0, '\0');
            if (key) {
                const BcsvCell* kc = nullptr;
                for (auto& c : row)
                    if (c.hash == key->hash) kc = &c;  // the last one, as written
                if (kc) {
                    write_cell(*kc, probe.data(), probe.size());
                    for (size_t r = 0; r < rows && target == SIZE_MAX; r++)
                        if (d.compare(rows_at + r * row_size + key->offset, key->size, probe) == 0) target = r;
                    for (size_t a = 0; a * key->size < added_keys.size() && target == SIZE_MAX; a++)
                        if (added_keys.compare(a * key->size, key->size, probe) == 0) target = rows + a;
                }
            }
            if (target != SIZE_MAX) {
                ++*replaced;
            } else {
                added_keys += probe;
                target = rows + size_t((*added)++);
            }
            patched.emplace_back(&row, target);
        }
    }
    if (patched.empty()) return {};

    size_t total = rows + size_t(*added);
    std::string out = d.substr(0, rows_at + rows * row_size);
    out.resize((rows_at + total * row_size + 3) & ~size_t(3), '\0');
    put32(out, 0, uint32_t(total));
    // A row's first four bytes hold its offset in the file unless a column starts there.
    if (row_size >= 4 && std::all_of(cols.begin(), cols.end(), [](const BcsvColumn& c) { return c.offset >= 4; }))
        for (size_t r = rows; r < total; r++) put32(out, rows_at + r * row_size, uint32_t(rows_at + r * row_size));
    for (auto& [row, target] : patched)
        for (auto& c : *row)
            if (const BcsvColumn* col = column(c.hash))
                write_cell(c, out.data() + rows_at + target * row_size + col->offset, col->size);
    return out;
}

std::string write_byml(const BymlNode& root, bool big_endian, uint16_t version) {
    if (root.kind != BymlNode::Array && root.kind != BymlNode::Dict && root.kind != BymlNode::Null) return {};
    std::set<std::string> keys, strings;
    if (!collect_strings(root, keys, strings)) return {};
    BymlWriter w;
    w.be = big_endian;
    w.keys.assign(keys.begin(), keys.end());
    w.strings.assign(strings.begin(), strings.end());
    w.out = big_endian ? "BY" : "YB";
    w.put(version, 2);
    w.out.resize(16);
    w.fill32(4, w.table(w.keys));
    w.fill32(8, w.table(w.strings));
    if (root.kind != BymlNode::Null) {
        uint32_t at;
        if (!w.value(root, at)) return {};
        w.fill32(12, at);
    }
    return w.out;
}

bool PackPatches::add(const std::string& source, const std::string& json, std::string* error) {
    std::vector<std::tuple<std::string, std::string, PackFile>> decoded;
    if (!decode_all(json, decoded, error, [&](const Json& p, auto& out, std::string& e) { return decode_pack(source, p, out, e); }))
        return false;
    for (auto& [archive, file, pf] : decoded) {
        auto& slot = archives[archive][file];
        if (!slot.source.empty() && slot.source != source)
            log("nookkit: pack patch %s replaces %s/%s from %s", source.c_str(), archive.c_str(), file.c_str(),
                slot.source.c_str());
        slot = std::move(pf);
    }
    return true;
}

std::string PackPatches::apply(const std::string& archive, const std::string& sarc, int* count) const {
    *count = 0;
    auto patches = archives.find(archive);
    std::vector<SarcFile> files;
    size_t data_off = 0;
    if (patches == archives.end()) return {};
    if (!sarc_files(sarc, files, data_off)) {
        log("nookkit: Pack/%s.pack isn't an archive it can read", archive.c_str());
        return {};
    }
    std::map<std::string, std::string> repl;
    for (auto& [name, pf] : patches->second) {
        auto f = std::find_if(files.begin(), files.end(), [&](const SarcFile& s) { return s.name == name; });
        if (f == files.end()) {
            log("nookkit: pack patch %s on %s/%s: not in the archive, add a new file as a loose file", pf.source.c_str(),
                archive.c_str(), name.c_str());
            continue;
        }
        // The original's byte order and version.
        bool big;
        uint16_t version;
        if (f->end - f->start >= 4 && sarc.compare(f->start, 2, "YB") == 0) {
            big = false, version = u16(sarc, f->start + 2);
        } else if (f->end - f->start >= 4 && sarc.compare(f->start, 2, "BY") == 0) {
            big = true, version = uint16_t(uint8_t(sarc[f->start + 2]) << 8 | uint8_t(sarc[f->start + 3]));
        } else {
            version = 0, big = false;
        }
        std::string bytes = version >= 1 && version <= 4 ? write_byml(pf.root, big, version) : std::string();
        if (bytes.empty()) {
            log("nookkit: pack patch %s on %s/%s: %s", pf.source.c_str(), archive.c_str(), name.c_str(),
                version >= 1 && version <= 4 ? "can't be written as BYML" : "the game's file isn't BYML");
            continue;
        }
        repl[name] = std::move(bytes);
        ++*count;
    }
    return repl.empty() ? std::string() : sarc_replace(sarc, files, data_off, repl);
}

}  // namespace nookkit

namespace {

// shortcut: the archive and locale names of the game's Message folder (same in 1.0.0 to 3.0.3); a new archive
// in a later update needs adding here, or "archive" in the patch is not enough.
const char* const kArchives[] = {"Dialog",  "LayoutMsg", "Mail",    "String",  "System",   "TalkFtr",
                                 "TalkNNpc", "TalkObj",  "TalkSNpc", "TalkSys", "Tutorials"};
const char* const kLocales[] = {"USen", "USes", "USfr", "EUen", "EUde", "EUes", "EUfr",
                                "EUit", "EUnl", "EUru", "JPja", "KRko", "CNzh", "TWzh"};
constexpr const char* kRstb = "System/Resource/ResourceSizeTable.srsizetable";

std::string read_host_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);  // a wide path on Windows: mod folders may have any name
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// The JSON files of romfs/NookKit/<kind>/ in every enabled mod, as (name, path) in the order they load.
std::vector<std::pair<std::string, fs::path>> json_files(const char* kind) {
    std::set<std::string> seen;  // a file name another (higher) mod already has: hidden, as in LayeredFS
    std::vector<std::pair<std::string, fs::path>> order;
    for (const std::string& root : romfs_mod_roots()) {
        fs::path dir = fs::u8path(root) / "NookKit" / kind;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> jsons;
        for (auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".json") jsons.push_back(e.path());
        std::sort(jsons.begin(), jsons.end());
        std::vector<std::pair<std::string, fs::path>> mine;
        for (auto& p : jsons) {
            auto u = p.filename().u8string();
            std::string name(u.begin(), u.end());
            if (seen.insert(name).second) mine.emplace_back(name, p);
        }
        order.insert(order.begin(), mine.begin(), mine.end());  // lower mods load first, so higher ones win
    }
    return order;
}

// Every JSON file of NookKit/<kind>/ added to `set`; the number read.
template <typename Set>
int load_patches(const char* kind, Set& set) {
    int read = 0;
    for (auto& [name, path] : json_files(kind)) {
        std::string error;
        read++;
        if (!set.add(name, read_host_file(path), &error)) log("nookkit: %s patch %s: %s", kind, name.c_str(), error.c_str());
    }
    return read;
}

// What a file that grew from `before` to `after` bytes adds to its ResourceSizeTable entry: whole blocks of the
// largest load alignment (0x2000, archives), at least what any smaller alignment needs.
void grew(std::map<std::string, int64_t>& growth, const std::string& name, size_t before, size_t after) {
    if (after > before) growth[name] = int64_t((after - before + 0x1FFF) & ~size_t(0x1FFF));
}

void install_bcsv(std::map<std::string, int64_t>& growth) {
    nookkit::BcsvPatches patches;
    int read = load_patches("Bcsv", patches);
    if (patches.files.empty()) return;
    log("nookkit: %zu patched tables from %d files", patches.files.size(), read);
    for (auto& [file, sources] : patches.files) {
        std::string path = "Bcsv/" + file + ".bcsv", data = read_game_file(path);
        if (data.empty()) {
            log("nookkit: bcsv patch for %s: the game has no %s", file.c_str(), path.c_str());
            continue;
        }
        int replaced = 0, added = 0;
        std::string patched = patches.apply(file, data, &replaced, &added);
        if (patched.empty()) continue;
        grew(growth, path, data.size(), patched.size());
        log("nookkit: %s: %d rows replaced, %d added (in memory)", path.c_str(), replaced, added);
        add_virtual_file(path, std::move(patched));
    }
}

void install_pack(std::map<std::string, int64_t>& growth) {
    nookkit::PackPatches patches;
    int read = load_patches("Pack", patches);
    if (patches.archives.empty()) return;
    size_t n = 0;
    for (auto& [a, files] : patches.archives) n += files.size();
    log("nookkit: %zu patched pack files from %d files", n, read);
    for (auto& [archive, files] : patches.archives) {
        std::string path = "Pack/" + archive + ".pack", data = read_game_file(path);
        if (data.empty()) {
            log("nookkit: pack patch for %s: the game has no %s", archive.c_str(), path.c_str());
            continue;
        }
        int count = 0;
        std::string patched = patches.apply(archive, data, &count);
        if (patched.empty()) continue;
        grew(growth, path, data.size(), patched.size());
        log("nookkit: %s: %d files replaced (in memory, %zu -> %zu bytes)", path.c_str(), count, data.size(),
            patched.size());
        add_virtual_file(path, std::move(patched));
    }
}

void install_messages(std::map<std::string, int64_t>& growth) {
    using namespace nookkit;
    Patches patches;
    int read = load_patches("Message", patches);
    if (patches.files.empty()) return;
    log("nookkit: %zu patched messages from %d files", patches.count(), read);

    // The archive of each patched file: as the patch says, else the one that holds it (file names are the same in
    // every locale, so the first locale found is looked at).
    std::map<std::string, std::map<std::string, const FilePatch*>> by_archive;
    std::vector<std::string> unplaced;
    for (auto& [file, p] : patches.files) {
        if (!p.archive.empty()) by_archive[p.archive][file] = &p;
        else unplaced.push_back(file);
    }
    if (!unplaced.empty()) {
        for (const char* archive : kArchives) {
            std::string zs;
            for (const char* locale : kLocales)
                if (!(zs = read_game_file(std::string("Message/") + archive + "_" + locale + ".sarc.zs")).empty()) break;
            for (const std::string& name : archive_files(zs)) {
                auto it = std::find(unplaced.begin(), unplaced.end(), name.substr(0, name.size() - 5));
                if (name.size() > 5 && it != unplaced.end()) {
                    by_archive[archive][*it] = &patches.files[*it];
                    unplaced.erase(it);
                }
            }
        }
        for (auto& file : unplaced) log("nookkit: message patch for %s: no Message archive has %s.msbt", file.c_str(), file.c_str());
    }

    for (auto& [archive, files] : by_archive) {
        int found = 0;
        for (const char* locale : kLocales) {
            std::string path = "Message/" + archive + "_" + locale + ".sarc.zs";
            std::string zs = read_game_file(path);
            if (zs.empty()) continue;
            found++;
            int messages = 0;
            size_t old_size = 0, new_size = 0;
            std::string patched = patch_archive(zs, locale, files, &messages, &old_size, &new_size);
            if (patched.empty()) continue;
            grew(growth, path.substr(0, path.size() - 3), old_size, new_size);  // named without .zs
            add_virtual_file(path, std::move(patched));
            log("nookkit: %s: %d messages patched (in memory, %zu -> %zu bytes)", path.c_str(), messages, old_size,
                new_size);
        }
        if (!found) log("nookkit: no Message/%s_<locale>.sarc.zs in the game", archive.c_str());
    }
}

void install() {
    std::map<std::string, int64_t> growth;  // RSTB name -> bytes to add
    install_messages(growth);
    install_bcsv(growth);
    install_pack(growth);
    if (growth.empty()) return;
    std::string raised = nookkit::raise_rstb(read_game_file(kRstb), growth);
    if (raised.empty()) {
        log("nookkit: %s can't be read: the patched files that grew may not load", kRstb);
        return;
    }
    add_virtual_file(kRstb, std::move(raised));
    log("nookkit: %s: %zu files' sizes raised (in memory)", kRstb, growth.size());
}

}  // namespace

void nookkit_install() {
    static std::once_flag once;
    std::call_once(once, install);
}

}  // namespace nx
