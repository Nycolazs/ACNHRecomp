#include "mod_manager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

#include "save_log.h"
#include "save_storage.h"

#if defined(NX_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace fs = std::filesystem;

namespace nx::mod_manager {

namespace {

using nx::save_storage::copy_tree;
using nx::save_storage::path_utf8;
using nx::save_storage::u8_path;

constexpr uint64_t kMaxZipBytes = 1ull << 30;  // 1 GiB: RomFS overlays are typically small

void set_err(std::string* out, const std::string& v) {
    if (out) *out = v;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

bool valid_id(const std::string& id) {
    if (id.empty() || id == "." || id == "..") return false;
    for (char ch : id) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '\0') return false;
    }
    return true;
}

bool is_managed(const std::string& id) { return id == "ptbr" || id == "speedfix60"; }

fs::path mods_root(const std::string& data_root) { return u8_path(data_root) / "mods"; }

fs::path config_path(const std::string& data_root) { return u8_path(data_root) / "config.ini"; }

bool ieq_romfs(const std::string& name) {
    std::string l = lower(name);
    return l == "romfs";
}

bool ieq_exefs(const std::string& name) {
    std::string l = lower(name);
    return l == "exefs" || l == "exefs_patches";
}

bool ieq_nookkit(const std::string& name) { return lower(name) == "nookkit"; }

// Left in an imported mod's folder when its pack had ExeFS code, which is not imported: listed as such after.
constexpr const char* kExefsMark = "exefs-not-imported.txt";

// Wrapper folders allowed above a mod's romfs/: atmosphere/contents/<title id>/romfs is three.
constexpr int kMaxWrappers = 3;

// NookKit's JSON patches (runtime/src/nookkit.cpp) go in romfs/NookKit/<kind>/: the kind of a loose one.
// shortcut: told by its field names, not parsed; the runtime reports a patch that does not read.
const char* nookkit_kind(const std::string& json) {
    if (json.find("\"messages\"") != std::string::npos) return "Message";
    if (json.find("\"rows\"") != std::string::npos) return "Bcsv";
    if (json.find("\"data\"") != std::string::npos && json.find("\"archive\"") != std::string::npos) return "Pack";
    return nullptr;
}

bool ends_with_json(const std::string& s) { return s.size() > 5 && lower(s.substr(s.size() - 5)) == ".json"; }

// A pack whose only code is NookKit's runtime (Skyline in exefs/, NookKit's own plugins in romfs/skyline/plugins/)
// and which has JSON patches for it: ACNHRecomp applies the patches itself, so there is no ExeFS to warn about.
// `files` are '/' separated, relative to the pack (the folder that holds romfs/).
bool exefs_is_nookkit(const std::vector<std::string>& files) {
    bool patches = false;
    for (const std::string& f : files) {
        std::string l = lower(f), name = l.substr(l.rfind('/') + 1);
        if (l.rfind("romfs/nookkit/", 0) == 0) patches = true;
        else if (l.rfind("romfs/skyline/plugins/", 0) == 0 && name.find("nookkit") == std::string::npos) return false;
        else if (l.rfind("exefs/", 0) == 0 && name != "main.npdm" && name.rfind("subsdk", 0) != 0) return false;
        else if (l.rfind("exefs_patches/", 0) == 0 || l.rfind("skyline/", 0) == 0 || l.rfind("atmosphere/", 0) == 0)
            return false;
    }
    return patches;
}

// ---- config.ini game.mods ---------------------------------------------------

std::vector<std::string> split_mods(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : csv) {
        if (c == ',') {
            cur = trim(cur);
            if (!cur.empty() && valid_id(cur)) out.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    cur = trim(cur);
    if (!cur.empty() && valid_id(cur)) out.push_back(cur);
    // De-dupe while keeping first occurrence (LayeredFS order).
    std::vector<std::string> uniq;
    std::set<std::string> seen;
    for (const std::string& n : out) {
        if (seen.insert(n).second) uniq.push_back(n);
    }
    return uniq;
}

std::string join_mods(const std::vector<std::string>& ids) {
    std::ostringstream o;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) o << ',';
        o << ids[i];
    }
    return o.str();
}

std::vector<std::string> read_enabled(const std::string& data_root) {
    std::ifstream f(config_path(data_root));
    if (!f) return {};
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (!section.empty() && k.find('.') == std::string::npos) k = section + "." + k;
        if (k == "game.mods") return split_mods(v);
    }
    return {};
}

bool write_game_mods_key(const std::string& data_root, const std::string& value, std::string* error) {
    fs::path path = config_path(data_root);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    std::vector<std::pair<std::string, std::string>> entries;
    {
        std::ifstream f(path);
        std::string line, section;
        while (std::getline(f, line)) {
            std::string raw = line;
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            if (line.front() == '[' && line.back() == ']') {
                section = trim(line.substr(1, line.size() - 2));
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            if (!section.empty() && k.find('.') == std::string::npos) k = section + "." + k;
            if (k == "game.mods") continue;
            entries.emplace_back(k, v);
        }
    }
    if (!value.empty()) entries.emplace_back("game.mods", value);

    std::vector<std::string> sections;
    for (const auto& [k, v] : entries) {
        (void)v;
        std::string s = k.substr(0, k.find('.'));
        if (std::find(sections.begin(), sections.end(), s) == sections.end()) sections.push_back(s);
    }
    if (sections.empty() && !value.empty()) sections.push_back("game");

    fs::path tmp = u8_path(path_utf8(path) + ".tmp");
    std::ofstream o(tmp, std::ios::trunc);
    if (!o) return set_err(error, "cannot write config.ini"), false;
    o << "# ACNHRecomp settings\n";
    for (const std::string& s : sections) {
        o << "\n[" << s << "]\n";
        for (const auto& [k, v] : entries)
            if (k.compare(0, s.size() + 1, s + ".") == 0) o << k.substr(s.size() + 1) << " = " << v << "\n";
    }
    o.close();
    if (!o) return set_err(error, "cannot write config.ini"), false;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return set_err(error, "cannot replace config.ini: " + ec.message()), false;
    }
    return true;
}

fs::path find_child(const fs::path& dir, bool (*named)(const std::string&)) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        if (named(path_utf8(it->path().filename()))) return it->path();
    }
    return {};
}

fs::path find_romfs_child(const fs::path& dir) { return find_child(dir, ieq_romfs); }

bool dir_has_exefs(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string name = path_utf8(it->path().filename());
        if (ieq_exefs(name)) return true;
        if (lower(name) == "atmosphere") return true;
        // skyline plugins often live under romfs/skyline — still RomFS; flag ExeFS sibling only.
    }
    // Also: top-level skyline/ next to romfs is an ExeFS-style plugin layout some packs use.
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        if (lower(path_utf8(it->path().filename())) == "skyline") return true;
    }
    return false;
}

enum class Found { None, One, Several };

// The folder `named` (romfs/, NookKit/) in `path` itself or up to kMaxWrappers folders below it, the shallowest;
// *pack_root is the folder that holds it.
Found find_named_root(const fs::path& path, bool (*named)(const std::string&), fs::path* found, fs::path* pack_root) {
    if (named(path_utf8(path.filename()))) {
        *found = path;
        *pack_root = path.parent_path();
        return Found::One;
    }
    std::error_code ec;
    std::vector<fs::path> level{path};
    for (int depth = 0; depth <= kMaxWrappers && !level.empty(); depth++) {
        std::vector<fs::path> packs, next;
        for (const fs::path& d : level) {
            if (!find_child(d, named).empty()) packs.push_back(d);
            for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_directory(ec) && !named(path_utf8(it->path().filename()))) next.push_back(it->path());
        }
        if (packs.size() > 1) return Found::Several;
        if (packs.size() == 1) {
            *found = find_child(packs[0], named);
            *pack_root = packs[0];
            return Found::One;
        }
        std::sort(next.begin(), next.end());
        level.swap(next);
    }
    return Found::None;
}

// Every file under `dir`, '/' separated and relative to it.
std::vector<std::string> files_under(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) {
            std::string rel = path_utf8(it->path().lexically_relative(dir));
            std::replace(rel.begin(), rel.end(), '\\', '/');
            out.push_back(rel);
        }
    return out;
}

std::string sanitize_id(std::string name) {
    // Strip a .zip or .json extension if present.
    if (name.size() > 4 && lower(name.substr(name.size() - 4)) == ".zip") name.resize(name.size() - 4);
    if (ends_with_json(name)) name.resize(name.size() - 5);
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
            c = '-';
    }
    name = trim(name);
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty()) name = "mod";
    if (!valid_id(name)) name = "mod";
    return name;
}

std::string unique_mod_id(const std::string& data_root, const std::string& want) {
    fs::path root = mods_root(data_root);
    std::error_code ec;
    if (!fs::exists(root / u8_path(want), ec)) return want;
    for (int n = 2; n < 10000; ++n) {
        std::string id = want + "-" + std::to_string(n);
        if (!fs::exists(root / u8_path(id), ec)) return id;
    }
    return want + "-new";
}

std::string stem_of(const fs::path& p) {
    std::string name = path_utf8(p.filename());
    if (name.empty()) name = path_utf8(p);
    return sanitize_id(name);
}

bool ends_with_zip(const std::string& s) {
    return s.size() >= 4 && lower(s.substr(s.size() - 4)) == ".zip";
}

// ---- zip (slim copy of saves.cpp helpers; launcher-safe) --------------------

uint16_t rd16(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const unsigned char* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16) | uint32_t(p[3]) << 24; }

uint32_t crc32_of(const std::string& data) {
    static const auto tbl = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = tbl[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool read_at(std::ifstream& f, uint64_t off, size_t size, std::string& out) {
    out.resize(size);
    f.clear();
    f.seekg(std::streamoff(off));
    if (size) f.read(out.data(), std::streamsize(size));
    return bool(f);
}

struct ZipEntry {
    std::string name;
    uint16_t flags = 0, method = 0;
    uint32_t crc = 0, csize = 0, usize = 0, offset = 0;
};

bool safe_zip_name(std::string& n) {
    std::replace(n.begin(), n.end(), '\\', '/');
    if (n.empty() || n[0] == '/' || n.find(':') != std::string::npos) return false;
    size_t pos = 0;
    while (pos <= n.size()) {
        size_t slash = n.find('/', pos);
        if (slash == std::string::npos) slash = n.size();
        if (slash - pos == 2 && n.compare(pos, 2, "..") == 0) return false;
        pos = slash + 1;
    }
    return true;
}

bool zip_list(const fs::path& zip, std::vector<ZipEntry>& out, std::string* error) {
    std::ifstream f(zip, std::ios::binary);
    if (!f) return set_err(error, "cannot open " + path_utf8(zip)), false;
    f.seekg(0, std::ios::end);
    uint64_t size = uint64_t(f.tellg());
    if (size < 22) return set_err(error, "not a zip file"), false;
    size_t tail = size_t(std::min<uint64_t>(size, 22 + 0xFFFF));
    std::string buf;
    if (!read_at(f, size - tail, tail, buf)) return set_err(error, "cannot read the zip file"), false;
    const auto* b = reinterpret_cast<const unsigned char*>(buf.data());
    size_t eocd = std::string::npos;
    for (size_t i = tail - 22 + 1; i-- > 0;)
        if (rd32(b + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) return set_err(error, "not a zip file"), false;
    uint16_t count = rd16(b + eocd + 10);
    uint32_t cd_size = rd32(b + eocd + 12), cd_off = rd32(b + eocd + 16);
    if (count == 0xFFFF || cd_size == 0xFFFFFFFF || cd_off == 0xFFFFFFFF)
        return set_err(error, "zip64 not supported"), false;
    if (uint64_t(cd_off) + cd_size > size) return set_err(error, "damaged zip file"), false;
    std::string cd;
    if (!read_at(f, cd_off, cd_size, cd)) return set_err(error, "cannot read the zip file"), false;
    const auto* c = reinterpret_cast<const unsigned char*>(cd.data());
    size_t pos = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (pos + 46 > cd.size() || rd32(c + pos) != 0x02014b50) return set_err(error, "damaged zip file"), false;
        ZipEntry e;
        e.flags = rd16(c + pos + 8);
        e.method = rd16(c + pos + 10);
        e.crc = rd32(c + pos + 16);
        e.csize = rd32(c + pos + 20);
        e.usize = rd32(c + pos + 24);
        size_t nlen = rd16(c + pos + 28), xlen = rd16(c + pos + 30), clen = rd16(c + pos + 32);
        e.offset = rd32(c + pos + 42);
        if (pos + 46 + nlen + xlen + clen > cd.size()) return set_err(error, "damaged zip file"), false;
        e.name.assign(cd, pos + 46, nlen);
        pos += 46 + nlen + xlen + clen;
        if (!safe_zip_name(e.name)) return set_err(error, "unsafe path in zip: " + e.name), false;
        if (!e.name.empty() && e.name.back() == '/') continue;
        if (e.name.rfind("__MACOSX/", 0) == 0) continue;
        out.push_back(std::move(e));
    }
    return true;
}

bool zip_inflate(const std::string& in, std::string& out, std::string* error) {
#if defined(NX_HAVE_ZLIB)
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return set_err(error, "zlib failed"), false;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.avail_in = uInt(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = uInt(out.size());
    int r = inflate(&z, Z_FINISH);
    bool ok = r == Z_STREAM_END && z.total_out == out.size();
    inflateEnd(&z);
    if (!ok) set_err(error, "damaged zip file (inflate)");
    return ok;
#else
    (void)in;
    (void)out;
    return set_err(error, "deflated zip needs zlib; use an uncompressed zip or a folder"), false;
#endif
}

// The shortest path prefix ending at .../<named>/ (case-insensitive), at most kMaxWrappers folders deep.
Found zip_find_prefix(const std::vector<ZipEntry>& entries, bool (*named)(const std::string&), std::string* prefix) {
    std::set<std::string> prefixes;
    for (const ZipEntry& e : entries) {
        const std::string& n = e.name;
        size_t start = 0;
        for (int depth = 0; depth <= kMaxWrappers; ++depth) {
            size_t slash = n.find('/', start);
            if (slash == std::string::npos) break;
            if (named(n.substr(start, slash - start))) {
                prefixes.insert(n.substr(0, slash + 1));  // includes the trailing /
                break;
            }
            start = slash + 1;
        }
    }
    if (prefixes.empty()) return Found::None;
    std::string best = *prefixes.begin();
    for (const std::string& p : prefixes)
        if (p.size() < best.size()) best = p;
    size_t same = 0;
    for (const std::string& p : prefixes)
        same += std::count(p.begin(), p.end(), '/') == std::count(best.begin(), best.end(), '/');
    if (same > 1) return Found::Several;  // several packs side by side
    *prefix = best;
    return Found::One;
}

// The files of the pack around `prefix` (the folder that holds romfs/), relative to it.
std::vector<std::string> zip_pack_files(const std::vector<ZipEntry>& entries, const std::string& prefix) {
    size_t slash = prefix.size() >= 2 ? prefix.rfind('/', prefix.size() - 2) : std::string::npos;
    std::string pack = slash == std::string::npos ? std::string() : prefix.substr(0, slash + 1);
    std::vector<std::string> out;
    for (const ZipEntry& e : entries)
        if (e.name.compare(0, pack.size(), pack) == 0) out.push_back(e.name.substr(pack.size()));
    return out;
}

bool zip_read(std::ifstream& f, const ZipEntry& e, uint64_t& total, std::string& plain, std::string* error) {
    if (e.flags & 1) return set_err(error, "encrypted zip entries are not supported"), false;
    if (e.method != 0 && e.method != 8) return set_err(error, "unsupported zip compression in " + e.name), false;
    total += e.usize;
    if (total > kMaxZipBytes) return set_err(error, "zip too large for a mod"), false;
    std::string head, data;
    if (!read_at(f, e.offset, 30, head) || rd32(reinterpret_cast<const unsigned char*>(head.data())) != 0x04034b50)
        return set_err(error, "damaged zip file"), false;
    const auto* h = reinterpret_cast<const unsigned char*>(head.data());
    uint64_t at = uint64_t(e.offset) + 30 + rd16(h + 26) + rd16(h + 28);
    if (!read_at(f, at, e.csize, data)) return set_err(error, "damaged zip file"), false;
    if (e.method == 0) {
        if (e.csize != e.usize) return set_err(error, "damaged zip file"), false;
        plain.swap(data);
    } else {
        plain.assign(e.usize, '\0');
        if (!zip_inflate(data, plain, error)) return false;
    }
    if (crc32_of(plain) != e.crc) return set_err(error, "damaged zip file (CRC mismatch in " + e.name + ")"), false;
    return true;
}

bool write_file(const fs::path& out, const std::string& data, std::string* error) {
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    o.write(data.data(), std::streamsize(data.size()));
    o.close();
    if (!o) return set_err(error, "cannot write " + path_utf8(out)), false;
    return true;
}

bool zip_extract_prefix(const fs::path& zip, const std::vector<ZipEntry>& entries, const std::string& prefix,
                        const fs::path& dest, std::string* error) {
    std::ifstream f(zip, std::ios::binary);
    if (!f) return set_err(error, "cannot open " + path_utf8(zip)), false;
    uint64_t total = 0;
    std::string plain;
    for (const ZipEntry& e : entries) {
        if (e.name.compare(0, prefix.size(), prefix) != 0) continue;
        if (!zip_read(f, e, total, plain, error) || !write_file(dest / u8_path(e.name.substr(prefix.size())), plain, error))
            return false;
    }
    return true;
}

// Loose NookKit patches, (file name, JSON), into staging/romfs/NookKit/<kind>/.
bool write_loose_json(const fs::path& staging, const std::vector<std::pair<std::string, std::string>>& jsons,
                      std::string* error) {
    int n = 0;
    for (const auto& [name, json] : jsons) {
        const char* kind = nookkit_kind(json);
        if (!kind) {
            log("mod_manager: %s is not a NookKit patch (no \"messages\", \"rows\" or \"data\"): skipped", name.c_str());
            continue;
        }
        if (!write_file(staging / "romfs" / "NookKit" / kind / u8_path(name), json, error)) return false;
        n++;
    }
    return n ? true : (set_err(error, "no romfs/ folder and no NookKit patch found"), false);
}

// Fills a new mod folder through `fill` (given its staging folder) and installs it as `want` (or want-2, ...).
template <typename Fill>
bool install_staged(const std::string& data_root, const std::string& want, std::string* out_id, std::string* error,
                    Fill fill) {
    std::string id = unique_mod_id(data_root, want);
    fs::path dst = mods_root(data_root) / u8_path(id);
    fs::path staging = mods_root(data_root) / u8_path(id + ".new");
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "romfs", ec);
    if (!fill(staging)) {
        fs::remove_all(staging, ec);
        return false;
    }
    fs::remove_all(dst, ec);
    fs::rename(staging, dst, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        return set_err(error, "cannot install the mod: " + ec.message()), false;
    }
    if (out_id) *out_id = id;
    log("mod_manager: imported as %s", id.c_str());
    return true;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// A folder: its romfs/ (up to kMaxWrappers folders down); else its NookKit/ (taken as romfs/NookKit/); else the
// NookKit patches (*.json) in it.
bool import_from_folder(const std::string& data_root, const fs::path& src, std::string* out_id, bool* had_exefs,
                        std::string* error) {
    fs::path found, pack;
    Found romfs = find_named_root(src, ieq_romfs, &found, &pack);
    if (romfs == Found::Several) return set_err(error, "several romfs/ folders found; pick one mod folder"), false;
    if (romfs == Found::One) {
        if (had_exefs) *had_exefs = dir_has_exefs(pack) && !exefs_is_nookkit(files_under(pack));
        std::string want = stem_of(pack);
        // atmosphere/contents/<title id>/romfs: named after the folder picked, not the title id
        bool title_id = want.size() == 16 && std::all_of(want.begin(), want.end(), [](char c) { return std::isxdigit(uint8_t(c)); });
        if (ieq_romfs(want) || pack.empty() || title_id) want = stem_of(src);
        return install_staged(data_root, want, out_id, error,
                              [&](const fs::path& staging) { return copy_tree(found, staging / "romfs", error); });
    }
    Found nookkit = find_named_root(src, ieq_nookkit, &found, &pack);
    if (nookkit == Found::Several) return set_err(error, "several NookKit/ folders found; pick one mod folder"), false;
    if (nookkit == Found::One)
        return install_staged(data_root, stem_of(pack.empty() ? src : pack), out_id, error, [&](const fs::path& staging) {
            return copy_tree(found, staging / "romfs" / "NookKit", error);
        });
    std::vector<std::pair<std::string, std::string>> jsons;
    std::error_code ec;
    for (fs::directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && ends_with_json(path_utf8(it->path().filename())))
            jsons.emplace_back(path_utf8(it->path().filename()), read_file(it->path()));
    if (jsons.empty()) return set_err(error, "no romfs/ folder found"), false;
    std::sort(jsons.begin(), jsons.end());
    return install_staged(data_root, stem_of(src), out_id, error,
                          [&](const fs::path& staging) { return write_loose_json(staging, jsons, error); });
}

// A zip: as a folder (romfs/, else NookKit/, else its *.json files).
bool import_from_zip(const std::string& data_root, const fs::path& zip, std::string* out_id, bool* had_exefs,
                     std::string* error) {
    std::vector<ZipEntry> entries;
    if (!zip_list(zip, entries, error)) return false;
    std::string prefix;
    Found romfs = zip_find_prefix(entries, ieq_romfs, &prefix);
    if (romfs == Found::Several) return set_err(error, "several romfs/ folders in the zip; it must hold one mod"), false;
    if (romfs == Found::One) {
        if (had_exefs) {
            std::vector<std::string> files = zip_pack_files(entries, prefix);
            *had_exefs = std::any_of(files.begin(), files.end(), [](const std::string& f) {
                std::string top = lower(f.substr(0, f.find('/')));
                return f.find('/') != std::string::npos && (ieq_exefs(top) || top == "skyline" || top == "atmosphere");
            }) && !exefs_is_nookkit(files);
        }
        return install_staged(data_root, stem_of(zip), out_id, error, [&](const fs::path& staging) {
            return zip_extract_prefix(zip, entries, prefix, staging / "romfs", error);
        });
    }
    Found nookkit = zip_find_prefix(entries, ieq_nookkit, &prefix);
    if (nookkit == Found::Several) return set_err(error, "several NookKit/ folders in the zip; it must hold one mod"), false;
    if (nookkit == Found::One)
        return install_staged(data_root, stem_of(zip), out_id, error, [&](const fs::path& staging) {
            return zip_extract_prefix(zip, entries, prefix, staging / "romfs" / "NookKit", error);
        });
    std::ifstream f(zip, std::ios::binary);
    uint64_t total = 0;
    std::vector<std::pair<std::string, std::string>> jsons;
    for (const ZipEntry& e : entries) {
        if (!ends_with_json(e.name)) continue;
        std::string plain;
        if (!zip_read(f, e, total, plain, error)) return false;
        jsons.emplace_back(e.name.substr(e.name.rfind('/') + 1), std::move(plain));
    }
    if (jsons.empty()) return set_err(error, "no romfs/ folder in the zip"), false;
    return install_staged(data_root, stem_of(zip), out_id, error,
                          [&](const fs::path& staging) { return write_loose_json(staging, jsons, error); });
}

}  // namespace

std::vector<ModInfo> list_mods(const std::string& data_root) {
    std::vector<std::string> enabled = read_enabled(data_root);
    std::set<std::string> enabled_set(enabled.begin(), enabled.end());

    std::vector<ModInfo> out;
    std::error_code ec;
    fs::path root = mods_root(data_root);
    if (!fs::is_directory(root, ec)) return out;

    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string id = path_utf8(it->path().filename());
        if (!valid_id(id) || (id.size() > 4 && id.compare(id.size() - 4, 4, ".new") == 0)) continue;
        fs::path romfs = find_romfs_child(it->path());
        if (romfs.empty()) continue;
        ModInfo m;
        m.id = id;
        m.enabled = enabled_set.count(id) != 0;
        m.managed = is_managed(id);
        // Bundled mods ship their ExeFS for the console; ACNHRecomp has it built in (speedfix60: the recompiled
        // variant), so nothing of theirs is unsupported.
        m.has_exefs = !m.managed && (dir_has_exefs(it->path()) || fs::exists(it->path() / kExefsMark, ec));
        // What the RomFS holds: NookKit's patches, other files (Skyline's plugins are neither: never loaded).
        for (fs::recursive_directory_iterator f(romfs, ec), fend; !ec && f != fend && !(m.has_romfs && m.has_nookkit);
             f.increment(ec)) {
            if (!f->is_regular_file(ec)) continue;
            std::string top = lower(path_utf8(f->path().lexically_relative(romfs)));
            std::replace(top.begin(), top.end(), '\\', '/');
            if (top.rfind("nookkit/", 0) == 0) m.has_nookkit |= ends_with_json(top);
            else if (top.rfind("skyline/", 0) != 0) m.has_romfs = true;
        }
        ec.clear();
        out.push_back(std::move(m));
    }

    // Enabled first (game.mods load order), disabled at the bottom alphabetically.
    std::vector<ModInfo> ordered;
    std::set<std::string> placed;
    for (const std::string& id : enabled) {
        auto it = std::find_if(out.begin(), out.end(), [&](const ModInfo& m) { return m.id == id; });
        if (it != out.end()) {
            ordered.push_back(*it);
            placed.insert(id);
        }
    }
    std::vector<ModInfo> rest;
    for (const ModInfo& m : out)
        if (!placed.count(m.id)) rest.push_back(m);
    std::sort(rest.begin(), rest.end(), [](const ModInfo& a, const ModInfo& b) { return a.id < b.id; });
    ordered.insert(ordered.end(), rest.begin(), rest.end());
    return ordered;
}

bool write_enabled(const std::string& data_root, const std::vector<std::string>& enabled_ids, std::string* error) {
    std::vector<std::string> clean;
    std::set<std::string> seen;
    for (const std::string& id : enabled_ids) {
        if (!valid_id(id) || !seen.insert(id).second) continue;
        clean.push_back(id);
    }
    return write_game_mods_key(data_root, join_mods(clean), error);
}

bool set_enabled(const std::string& data_root, const std::string& id, bool on, std::string* error) {
    if (!valid_id(id)) return set_err(error, "invalid mod id"), false;
    std::vector<std::string> cur = read_enabled(data_root);
    auto it = std::find(cur.begin(), cur.end(), id);
    if (on) {
        if (it == cur.end()) cur.push_back(id);
    } else if (it != cur.end()) {
        cur.erase(it);
    }
    return write_enabled(data_root, cur, error);
}

bool move_enabled(const std::string& data_root, const std::string& id, int delta, std::string* error) {
    if (!valid_id(id) || delta == 0) return set_err(error, "invalid move"), false;
    std::vector<std::string> cur = read_enabled(data_root);
    auto it = std::find(cur.begin(), cur.end(), id);
    if (it == cur.end()) return set_err(error, "mod is not enabled"), false;
    size_t i = size_t(it - cur.begin());
    int ni = int(i) + delta;
    if (ni < 0 || ni >= int(cur.size())) return true;  // already at edge
    std::swap(cur[i], cur[size_t(ni)]);
    return write_enabled(data_root, cur, error);
}

bool import_mod(const std::string& data_root, const std::string& source_path, std::string* out_id, bool* had_exefs,
                std::string* error) {
    if (had_exefs) *had_exefs = false;
    fs::path src = u8_path(source_path);
    std::error_code ec;
    std::string id;
    bool ok = false;
    if (fs::is_regular_file(src, ec) && ends_with_zip(source_path))
        ok = import_from_zip(data_root, src, &id, had_exefs, error);
    else if (fs::is_regular_file(src, ec) && ends_with_json(source_path))  // one loose NookKit patch
        ok = install_staged(data_root, stem_of(src), &id, error, [&](const fs::path& staging) {
            return write_loose_json(staging, {{path_utf8(src.filename()), read_file(src)}}, error);
        });
    else if (fs::is_directory(src, ec))
        ok = import_from_folder(data_root, src, &id, had_exefs, error);
    else
        return set_err(error, "pick a mod folder, a .zip or a NookKit .json"), false;
    if (!ok) return false;
    if (had_exefs && *had_exefs)
        std::ofstream(mods_root(data_root) / u8_path(id) / kExefsMark)
            << "The pack this mod came from had ExeFS or Skyline code, which ACNHRecomp does not run: only its RomFS "
               "was imported.\n";
    if (!set_enabled(data_root, id, true, error)) return false;
    if (out_id) *out_id = id;
    return true;
}

bool delete_mod(const std::string& data_root, const std::string& id, std::string* error) {
    if (!valid_id(id)) return set_err(error, "invalid mod id"), false;
    set_enabled(data_root, id, false, nullptr);
    fs::path dir = mods_root(data_root) / u8_path(id);
    std::error_code ec;
    if (!fs::exists(dir, ec)) return set_err(error, "mod folder not found"), false;
    fs::remove_all(dir, ec);
    if (ec) return set_err(error, "cannot delete mod: " + ec.message()), false;
    log("mod_manager: deleted %s", id.c_str());
    return true;
}

bool rename_mod(const std::string& data_root, const std::string& id, const std::string& name, std::string* new_id,
                std::string* error) {
    if (!valid_id(id)) return set_err(error, "invalid mod id"), false;
    if (is_managed(id)) return set_err(error, "bundled mods keep their name"), false;
    std::string to = trim(name);
    // Same rules as an imported mod's name (no path characters, no trailing dot or space); not ".new", the staging
    // suffix, nor a bundled mod's name.
    if (to.empty()) return set_err(error, "the name is empty"), false;
    to = sanitize_id(to);
    if (to.size() > 4 && lower(to.substr(to.size() - 4)) == ".new") to += "-mod";
    if (is_managed(to)) return set_err(error, "that name belongs to a bundled mod"), false;
    if (new_id) *new_id = to;
    if (to == id) return true;
    fs::path root = mods_root(data_root), from = root / u8_path(id), dst = root / u8_path(to);
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return set_err(error, "mod folder not found"), false;
    bool case_only = lower(to) == lower(id);  // the same folder on Windows and macOS: through a temporary name
    if (!case_only && fs::exists(dst, ec)) return set_err(error, "another mod is already named " + to), false;
    if (case_only) {
        fs::path tmp = root / u8_path(id + ".rename");
        fs::rename(from, tmp, ec);
        if (!ec && (fs::rename(tmp, dst, ec), ec)) {
            std::error_code back;
            fs::rename(tmp, from, back);  // its old name again; ec keeps why
        }
    } else {
        fs::rename(from, dst, ec);
    }
    if (ec) return set_err(error, "cannot rename the mod folder: " + ec.message()), false;
    std::vector<std::string> enabled = read_enabled(data_root);
    auto it = std::find(enabled.begin(), enabled.end(), id);
    if (it != enabled.end()) {
        *it = to;
        if (!write_enabled(data_root, enabled, error)) return false;
    }
    log("mod_manager: renamed %s to %s", id.c_str(), to.c_str());
    return true;
}

std::string mod_folder(const std::string& data_root, const std::string& id) {
    if (!valid_id(id)) return {};
    return path_utf8(mods_root(data_root) / u8_path(id));
}

}  // namespace nx::mod_manager
