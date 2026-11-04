#include "mod_manager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

size_t count_files(const fs::path& root) {
    size_t n = 0;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return 0;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) ++n;
    }
    return n;
}

fs::path find_romfs_child(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        if (ieq_romfs(path_utf8(it->path().filename()))) return it->path();
    }
    return {};
}

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

// Find romfs/ in `path` itself, or one/two levels below (Atmosphere / wrapper folders).
bool find_romfs_root(const fs::path& path, fs::path* romfs, fs::path* pack_root, bool* had_exefs, std::string* error) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return set_err(error, "folder not found"), false;

    auto try_at = [&](const fs::path& pack) -> bool {
        fs::path r = find_romfs_child(pack);
        if (r.empty()) return false;
        *romfs = r;
        *pack_root = pack;
        if (had_exefs) *had_exefs = dir_has_exefs(pack);
        return true;
    };

    if (try_at(path)) return true;
    // path itself named romfs
    if (ieq_romfs(path_utf8(path.filename()))) {
        *romfs = path;
        *pack_root = path.parent_path();
        if (had_exefs) *had_exefs = dir_has_exefs(*pack_root);
        return true;
    }

    std::vector<fs::path> level1;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) level1.push_back(it->path());
    std::sort(level1.begin(), level1.end());

    std::vector<fs::path> found_packs;
    for (const fs::path& a : level1) {
        if (try_at(a)) found_packs.push_back(a);
    }
    if (found_packs.size() == 1) {
        try_at(found_packs[0]);
        return true;
    }
    if (found_packs.size() > 1) return set_err(error, "several romfs/ folders found; pick one mod folder"), false;

    for (const fs::path& a : level1) {
        std::vector<fs::path> level2;
        for (fs::directory_iterator it(a, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec)) level2.push_back(it->path());
        for (const fs::path& b : level2) {
            if (try_at(b)) found_packs.push_back(b);
        }
    }
    if (found_packs.size() == 1) {
        try_at(found_packs[0]);
        return true;
    }
    if (found_packs.size() > 1) return set_err(error, "several romfs/ folders found; pick one mod folder"), false;
    return set_err(error, "no romfs/ folder found"), false;
}

std::string sanitize_id(std::string name) {
    // Strip .zip extension if present.
    if (name.size() > 4 && lower(name.substr(name.size() - 4)) == ".zip") name.resize(name.size() - 4);
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

// Find the shortest path prefix ending at .../romfs/ (case-insensitive).
bool zip_find_romfs_prefix(const std::vector<ZipEntry>& entries, std::string* prefix, bool* had_exefs,
                          std::string* error) {
    std::set<std::string> prefixes;
    for (const ZipEntry& e : entries) {
        std::string n = e.name;
        std::replace(n.begin(), n.end(), '\\', '/');
        // Walk components looking for romfs
        size_t start = 0;
        int depth = 0;
        while (start < n.size()) {
            size_t slash = n.find('/', start);
            std::string part = n.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (ieq_romfs(part) && slash != std::string::npos) {
                std::string p = n.substr(0, slash + 1);  // includes trailing /
                // Depth of folders before romfs (0–2 wrappers)
                if (depth <= 2) prefixes.insert(p);
                break;
            }
            if (slash == std::string::npos) break;
            ++depth;
            start = slash + 1;
            if (depth > 3) break;
        }
    }
    if (prefixes.empty()) return set_err(error, "no romfs/ folder in the zip"), false;
    // Prefer the shortest prefix; if several distinct packs at same depth, fail.
    std::string best = *prefixes.begin();
    for (const std::string& p : prefixes)
        if (p.size() < best.size()) best = p;
    std::set<std::string> same_len;
    for (const std::string& p : prefixes)
        if (p.size() == best.size()) same_len.insert(p);
    if (same_len.size() > 1) return set_err(error, "several romfs/ folders in the zip; it must hold one mod"), false;
    *prefix = best;

    if (had_exefs) {
        *had_exefs = false;
        size_t slash = best.size() >= 2 ? best.rfind('/', best.size() - 2) : std::string::npos;
        std::string pack_prefix = slash == std::string::npos ? std::string() : best.substr(0, slash + 1);
        for (const ZipEntry& e : entries) {
            std::string n = e.name;
            if (!pack_prefix.empty() && n.compare(0, pack_prefix.size(), pack_prefix) != 0) continue;
            std::string rest = pack_prefix.empty() ? n : n.substr(pack_prefix.size());
            size_t s = rest.find('/');
            std::string top = s == std::string::npos ? rest : rest.substr(0, s);
            if (ieq_exefs(top) || lower(top) == "skyline" || lower(top) == "atmosphere") {
                *had_exefs = true;
                break;
            }
        }
    }
    return true;
}

bool zip_extract_prefix(const fs::path& zip, const std::vector<ZipEntry>& entries, const std::string& prefix,
                        const fs::path& dest, std::string* error) {
    std::ifstream f(zip, std::ios::binary);
    if (!f) return set_err(error, "cannot open " + path_utf8(zip)), false;
    uint64_t total = 0;
    std::string head, data, plain;
    for (const ZipEntry& e : entries) {
        if (e.name.compare(0, prefix.size(), prefix) != 0) continue;
        if (e.flags & 1) return set_err(error, "encrypted zip entries are not supported"), false;
        if (e.method != 0 && e.method != 8) return set_err(error, "unsupported zip compression in " + e.name), false;
        total += e.usize;
        if (total > kMaxZipBytes) return set_err(error, "zip too large for a mod"), false;
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
        fs::path out = dest / u8_path(e.name.substr(prefix.size()));
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        o.write(plain.data(), std::streamsize(plain.size()));
        o.close();
        if (!o) return set_err(error, "cannot write " + path_utf8(out)), false;
    }
    return true;
}

bool import_from_folder(const std::string& data_root, const fs::path& src, std::string* out_id, bool* had_exefs,
                        std::string* error) {
    fs::path romfs, pack;
    bool exefs = false;
    if (!find_romfs_root(src, &romfs, &pack, &exefs, error)) return false;
    if (had_exefs) *had_exefs = exefs;

    std::string want = stem_of(pack.empty() ? src : pack);
    if (ieq_romfs(want)) want = stem_of(src);
    std::string id = unique_mod_id(data_root, want);

    fs::path dst = mods_root(data_root) / u8_path(id);
    fs::path staging = mods_root(data_root) / u8_path(id + ".new");
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "romfs", ec);
    if (!copy_tree(romfs, staging / "romfs", error)) {
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
    log("mod_manager: imported folder as %s", id.c_str());
    return true;
}

bool import_from_zip(const std::string& data_root, const fs::path& zip, std::string* out_id, bool* had_exefs,
                     std::string* error) {
    std::vector<ZipEntry> entries;
    if (!zip_list(zip, entries, error)) return false;
    std::string prefix;
    bool exefs = false;
    if (!zip_find_romfs_prefix(entries, &prefix, &exefs, error)) return false;
    if (had_exefs) *had_exefs = exefs;

    std::string id = unique_mod_id(data_root, stem_of(zip));
    fs::path dst = mods_root(data_root) / u8_path(id);
    fs::path staging = mods_root(data_root) / u8_path(id + ".new");
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "romfs", ec);
    if (!zip_extract_prefix(zip, entries, prefix, staging / "romfs", error)) {
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
    log("mod_manager: imported zip as %s", id.c_str());
    return true;
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
        m.has_exefs = dir_has_exefs(it->path());
        m.file_count = count_files(romfs);
        out.push_back(std::move(m));
    }

    // Enabled first, in game.mods order; then the rest alphabetically.
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
    else if (fs::is_directory(src, ec))
        ok = import_from_folder(data_root, src, &id, had_exefs, error);
    else
        return set_err(error, "pick a mod folder or a .zip"), false;
    if (!ok) return false;
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

std::string mod_folder(const std::string& data_root, const std::string& id) {
    if (!valid_id(id)) return {};
    return path_utf8(mods_root(data_root) / u8_path(id));
}

}  // namespace nx::mod_manager
