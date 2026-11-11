// Save data management for the settings screen: see saves.h.
//
// Where other programs keep an ACNH save (title id 01006F8002326000, a device save: one per console, user id zero):
//   yuzu and forks (suyu, sudachi, citron, eden, torzu): <emu>/nand/user/save/0000000000000000/<user id, 32 hex>/
//     01006F8002326000/ (user id all zeros for a device save), or the newer <emu>/nand/user/save/device/
//     01006F8002326000/0/ that these emulators prefer when present (src/core/file_sys/savedata_factory.cpp).
//   Ryujinx / Ryubing (LibHac): <Ryujinx>/bis/user/save/<save data id, 16 hex>/0/ (committed; 1/ is the working copy).
//     The id -> title map is in bis/system/save/8000000000000000/0/imkvdb.arc; we look inside each folder instead,
//     which needs no parser and finds the save even when that index is stale.
//   JKSV: sdmc:/JKSV/<title name>/<backup>/ (folders, or .zip with the files at their root).
//   Checkpoint: sdmc:/switch/Checkpoint/saves/0x01006F8002326000 <title name>/<backup>/.
// The save itself: main.dat + mainHeader.dat (the island), Villager0..7/personal.dat + personalHeader.dat (residents),
// landname.dat, photo_studio_island.dat, postbox.dat, profile.dat... (encrypted; copied as they are).
#include "saves.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

#if defined(NX_HAVE_ZLIB)
#include <zlib.h>
#endif

#include <nx/runtime.h>

#include "platform.h"
#include "save_storage.h"

namespace fs = std::filesystem;

namespace nx::saves {

namespace {

constexpr const char* kTitle = "01006F8002326000";
constexpr size_t kKeepBackups = 10;
constexpr uint64_t kMaxZipBytes = 1ull << 30;  // a save is ~10-40 MB: anything past 1 GiB is not one (or a zip bomb)

// One operation at a time: the settings screen may run them on a worker thread.
std::mutex g_lock;

fs::path u8(const std::string& s) { return fs::u8path(s); }

std::string utf8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string lower(std::string s) {
    for (char& ch : s)
        if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
    return s;
}

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool ends_with_zip(const std::string& s) { return s.size() >= 4 && lower(s.substr(s.size() - 4)) == ".zip"; }

void set(std::string* out, const std::string& v) {
    if (out) *out = v;
}

fs::path user_dir() { return save_storage::user_dir(plat::data_root()); }
fs::path device_dir() { return save_storage::device_dir(plat::data_root()); }
fs::path backups_root() { return user_dir() / "backups"; }

// The folder name for a save's backups: its island name, made safe for every file system ("Unnamed island" before
// the island has one). Two islands of the same name share a folder.
std::string island_folder(const fs::path& save) {
    std::string n = save_storage::island_display_name(save);
    for (char& ch : n)
        if (static_cast<unsigned char>(ch) < 0x20 || std::strchr("<>:\"/\\|?*", ch)) ch = '_';
    while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
    while (!n.empty() && n.front() == ' ') n.erase(0, 1);
    if (n.empty()) return "Unnamed island";
    static const char* const kReserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
                                            "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
                                            "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* r : kReserved)
        if (lower(n) == r) return n + "_";
    return n;
}


bool flat_save(const fs::path& d, std::string* why) { return save_storage::flat_save(d, why); }
bool copy_tree(const fs::path& src, const fs::path& dst, std::string* error) {
    return save_storage::copy_tree(src, dst, error);
}
bool non_empty_dir(const fs::path& d) { return save_storage::non_empty_dir(d); }
void recover() { save_storage::recover(plat::data_root()); }
bool swap_in(const fs::path& staging, std::string* error) {
    return save_storage::swap_in(plat::data_root(), staging, error);
}

std::string stamp(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, fmt, &tm);
    return buf;
}

// A name that does not exist yet in `dir`: base, base-2, base-3... (two exports or backups in the same second).
fs::path unique_in(const fs::path& dir, const std::string& base, const std::string& ext = "") {
    std::error_code ec;
    fs::path p = dir / u8(base + ext);
    for (int n = 2; fs::exists(p, ec) && n < 1000; ++n) p = dir / u8(base + "-" + std::to_string(n) + ext);
    return p;
}

// ---- recognising a save ----

std::vector<fs::path> subdirs(const fs::path& d) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) v.push_back(it->path());
    std::sort(v.begin(), v.end());
    return v;
}

// The island's identity: FNV-1a over its landname.dat.
uint32_t landname_hash(const fs::path& save) {
    uint32_t h = 2166136261u;
    std::ifstream f(save / "landname.dat", std::ios::binary);
    for (char c; f.get(c);) h = (h ^ uint8_t(c)) * 16777619u;
    return h;
}

std::string hex8(uint32_t v) {
    char b[9];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

// user/backups/<island> <8 hex landname hash><8 hex unix time>/: the backups of the save's island. The hash ties the
// folder to the island (a rename starts a new one); the time, set when the folder is made, tells apart two islands
// with the same name and hash. A folder from before these names (user/backups/<island>/) is renamed to one.
fs::path island_dir_for(const fs::path& save) {
    const std::string head = island_folder(save) + " " + hex8(landname_hash(save));
    for (const fs::path& p : subdirs(backups_root())) {
        const std::string n = utf8(p.filename());
        if (n.size() == head.size() + 8 && n.compare(0, head.size(), head) == 0) return p;
    }
    const fs::path made = backups_root() / u8(head + hex8(uint32_t(std::time(nullptr))));
    std::error_code ec;
    const fs::path legacy = backups_root() / u8(island_folder(save));
    if (is_dir(legacy)) fs::rename(legacy, made, ec);  // on failure the old folder stays, and a new one is made
    return made;
}

fs::path backups_dir() { return island_dir_for(device_dir()); }

// The save in `d` itself, else the only one one or two levels down (JKSV / Checkpoint keep a folder per backup).
// A Ryujinx save folder holds 0/ (committed) and maybe 1/ (working copy): 0 wins.
bool find_root(const fs::path& d, fs::path* root, std::string* why) {
    std::string own_why;
    if (flat_save(d, &own_why)) return *root = d, true;
    if (!is_dir(d)) return set(why, own_why), false;
    std::vector<fs::path> found;
    for (const fs::path& a : subdirs(d)) {
        if (flat_save(a, nullptr)) {
            found.push_back(a);
            continue;
        }
        for (const fs::path& b : subdirs(a))
            if (flat_save(b, nullptr)) found.push_back(b);
    }
    if (found.size() == 2 && found[0].filename() == "0" && found[1].filename() == "1" &&
        found[0].parent_path() == found[1].parent_path())
        found.pop_back();
    if (found.empty()) return set(why, own_why), false;
    if (found.size() > 1)
        return set(why, std::to_string(found.size()) + " saves in this folder: choose one of them"), false;
    *root = found[0];
    return true;
}

// ---- zip ----

uint16_t rd16(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const unsigned char* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16) | uint32_t(p[3]) << 24; }
void wr16(std::string& s, uint32_t v) { s.push_back(char(v)), s.push_back(char(v >> 8)); }
void wr32(std::string& s, uint32_t v) { wr16(s, v & 0xFFFF), wr16(s, v >> 16); }

uint32_t crc32_of(const std::string& data) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
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
    std::string name;  // '/' separated, checked: relative, no ".."
    uint16_t flags = 0, method = 0;
    uint32_t crc = 0, csize = 0, usize = 0, offset = 0;
};

// Rejects what could write outside the destination: absolute paths, drive letters, "..".
bool safe_name(std::string& n) {
    std::replace(n.begin(), n.end(), '\\', '/');
    if (n.empty() || n[0] == '/' || n.find(':') != std::string::npos) return false;
    size_t pos = 0;
    while (pos <= n.size()) {
        size_t slash = n.find('/', pos);
        if (slash == std::string::npos) slash = n.size();
        if (n.compare(pos, slash - pos, "..") == 0 && slash - pos == 2) return false;
        pos = slash + 1;
    }
    return true;
}

// The central directory (no zip64: a save never needs it). Directory entries and macOS's __MACOSX/ are left out.
bool zip_list(const fs::path& zip, std::vector<ZipEntry>& out, std::string* error) {
    std::ifstream f(zip, std::ios::binary);
    if (!f) return set(error, "cannot open " + utf8(zip)), false;
    f.seekg(0, std::ios::end);
    uint64_t size = uint64_t(f.tellg());
    if (size < 22) return set(error, "not a zip file"), false;
    size_t tail = size_t(std::min<uint64_t>(size, 22 + 0xFFFF));
    std::string buf;
    if (!read_at(f, size - tail, tail, buf)) return set(error, "cannot read the zip file"), false;
    const auto* b = reinterpret_cast<const unsigned char*>(buf.data());
    size_t eocd = std::string::npos;
    for (size_t i = tail - 22 + 1; i-- > 0;)
        if (rd32(b + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) return set(error, "not a zip file"), false;
    uint16_t count = rd16(b + eocd + 10);
    uint32_t cd_size = rd32(b + eocd + 12), cd_off = rd32(b + eocd + 16);
    if (count == 0xFFFF || cd_size == 0xFFFFFFFF || cd_off == 0xFFFFFFFF)
        return set(error, "zip64 not supported"), false;
    if (uint64_t(cd_off) + cd_size > size) return set(error, "damaged zip file"), false;
    std::string cd;
    if (!read_at(f, cd_off, cd_size, cd)) return set(error, "cannot read the zip file"), false;
    const auto* c = reinterpret_cast<const unsigned char*>(cd.data());
    size_t pos = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (pos + 46 > cd.size() || rd32(c + pos) != 0x02014b50) return set(error, "damaged zip file"), false;
        ZipEntry e;
        e.flags = rd16(c + pos + 8);
        e.method = rd16(c + pos + 10);
        e.crc = rd32(c + pos + 16);
        e.csize = rd32(c + pos + 20);
        e.usize = rd32(c + pos + 24);
        size_t nlen = rd16(c + pos + 28), xlen = rd16(c + pos + 30), clen = rd16(c + pos + 32);
        e.offset = rd32(c + pos + 42);
        if (pos + 46 + nlen + xlen + clen > cd.size()) return set(error, "damaged zip file"), false;
        e.name.assign(cd, pos + 46, nlen);
        pos += 46 + nlen + xlen + clen;
        if (!safe_name(e.name)) return set(error, "unsafe path in zip: " + e.name), false;
        if (e.name.back() == '/' || e.name.rfind("__MACOSX/", 0) == 0) continue;
        out.push_back(std::move(e));
    }
    return true;
}

// The folder prefix ("" or "a/" or "a/b/") of the one save in the zip.
bool zip_find_root(const std::vector<ZipEntry>& entries, std::string* prefix, std::string* why) {
    std::set<std::string> names;
    for (const ZipEntry& e : entries) names.insert(e.name);
    std::vector<std::string> found;
    bool any_main = false;
    for (const std::string& n : names) {
        if (n.size() < 8 || n.compare(n.size() - 8, 8, "main.dat") != 0) continue;
        std::string p = n.substr(0, n.size() - 8);
        if (!p.empty() && p.back() != '/') continue;
        if (std::count(p.begin(), p.end(), '/') > 2) continue;
        any_main = true;
        if (names.count(p + "mainHeader.dat") && names.count(p + "Villager0/personal.dat")) found.push_back(p);
    }
    if (found.empty()) {
        if (!any_main) return set(why, "main.dat missing"), false;
        return set(why, "mainHeader.dat or Villager0/personal.dat missing"), false;
    }
    if (found.size() > 1) return set(why, std::to_string(found.size()) + " saves in this zip: it must hold one"), false;
    *prefix = found[0];
    return true;
}

bool zip_inflate(const std::string& in, std::string& out, std::string* error) {
#if defined(NX_HAVE_ZLIB)
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return set(error, "zlib failed"), false;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.avail_in = uInt(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = uInt(out.size());
    int r = inflate(&z, Z_FINISH);
    bool ok = r == Z_STREAM_END && z.total_out == out.size();
    inflateEnd(&z);
    if (!ok) set(error, "damaged zip file (inflate)");
    return ok;
#else
    (void)in, (void)out;
    return set(error, "zip not supported in this build"), false;
#endif
}

// The entries under `prefix`, written below `dest` without it.
bool zip_extract(const fs::path& zip, const std::vector<ZipEntry>& entries, const std::string& prefix,
                 const fs::path& dest, std::string* error) {
    std::ifstream f(zip, std::ios::binary);
    if (!f) return set(error, "cannot open " + utf8(zip)), false;
    uint64_t total = 0;
    std::string head, data, plain;
    for (const ZipEntry& e : entries) {
        if (e.name.compare(0, prefix.size(), prefix) != 0) continue;
        if (e.flags & 1) return set(error, "encrypted zip entries are not supported"), false;
        if (e.method != 0 && e.method != 8) return set(error, "unsupported zip compression in " + e.name), false;
        total += e.usize;
        if (total > kMaxZipBytes) return set(error, "zip too large for a save"), false;
        if (!read_at(f, e.offset, 30, head) || rd32(reinterpret_cast<const unsigned char*>(head.data())) != 0x04034b50)
            return set(error, "damaged zip file"), false;
        const auto* h = reinterpret_cast<const unsigned char*>(head.data());
        uint64_t at = uint64_t(e.offset) + 30 + rd16(h + 26) + rd16(h + 28);
        if (!read_at(f, at, e.csize, data)) return set(error, "damaged zip file"), false;
        if (e.method == 0) {
            if (e.csize != e.usize) return set(error, "damaged zip file"), false;
            plain.swap(data);
        } else {
            plain.assign(e.usize, '\0');
            if (!zip_inflate(data, plain, error)) return false;
        }
        if (crc32_of(plain) != e.crc) return set(error, "damaged zip file (CRC mismatch in " + e.name + ")"), false;
        fs::path out = dest / u8(e.name.substr(prefix.size()));
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        o.write(plain.data(), std::streamsize(plain.size()));
        o.close();
        if (!o) return set(error, "cannot write " + utf8(out)), false;
    }
    return true;
}

// Stored (uncompressed) entries: the save files are encrypted, deflate would gain nothing.
bool zip_write(const fs::path& src, const fs::path& zip, std::string* error) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    if (ec) return set(error, "cannot read " + utf8(src) + ": " + ec.message()), false;
    std::sort(files.begin(), files.end());

    std::tm tm{};
    std::time_t t = std::time(nullptr);
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    uint16_t dos_time = uint16_t(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    uint16_t dos_date = uint16_t((std::max(tm.tm_year - 80, 0)) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);

    std::ofstream o(zip, std::ios::binary | std::ios::trunc);
    if (!o) return set(error, "cannot write " + utf8(zip)), false;
    std::string central, head, data;
    uint64_t offset = 0;
    for (const fs::path& p : files) {
        auto rel = p.lexically_relative(src).generic_u8string();
        std::string name(rel.begin(), rel.end());
        std::ifstream in(p, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (!in && !in.eof()) return set(error, "cannot read " + utf8(p)), false;
        if (data.size() >= 0xFFFFFFFFull || offset >= 0xFFFFFFFFull)
            return set(error, "save too large for a zip"), false;
        uint32_t crc = crc32_of(data);
        head.clear();
        wr32(head, 0x04034b50), wr16(head, 20), wr16(head, 1 << 11), wr16(head, 0);  // version, UTF-8 names, stored
        wr16(head, dos_time), wr16(head, dos_date), wr32(head, crc), wr32(head, uint32_t(data.size()));
        wr32(head, uint32_t(data.size())), wr16(head, uint32_t(name.size())), wr16(head, 0);
        wr32(central, 0x02014b50), wr16(central, 20), wr16(central, 20), wr16(central, 1 << 11), wr16(central, 0);
        wr16(central, dos_time), wr16(central, dos_date), wr32(central, crc), wr32(central, uint32_t(data.size()));
        wr32(central, uint32_t(data.size())), wr16(central, uint32_t(name.size())), wr16(central, 0), wr16(central, 0);
        wr16(central, 0), wr16(central, 0), wr32(central, 0), wr32(central, uint32_t(offset));
        central += name;
        o.write(head.data(), std::streamsize(head.size()));
        o.write(name.data(), std::streamsize(name.size()));
        o.write(data.data(), std::streamsize(data.size()));
        offset += head.size() + name.size() + data.size();
    }
    if (files.size() > 0xFFFF || offset > 0xFFFFFFFFull) return set(error, "save too large for a zip"), false;
    std::string end;
    wr32(end, 0x06054b50), wr16(end, 0), wr16(end, 0), wr16(end, uint32_t(files.size()));
    wr16(end, uint32_t(files.size())), wr32(end, uint32_t(central.size())), wr32(end, uint32_t(offset)), wr16(end, 0);
    o.write(central.data(), std::streamsize(central.size()));
    o.write(end.data(), std::streamsize(end.size()));
    o.close();
    if (!o) return set(error, "cannot write " + utf8(zip)), false;
    return true;
}

// YYYYmmdd-HHMMSS[-n]; ".tmp" folders are backups still being written (or abandoned by a crash).
bool backup_name(const std::string& n) {
    bool ok = n.size() >= 15 && n[8] == '-' && n.find(".tmp") == std::string::npos;
    for (size_t i = 0; ok && i < 15; ++i) ok = i == 8 || (n[i] >= '0' && n[i] <= '9');
    return ok;
}

// Backups from before the per-island folders sit in user/backups/ itself: each moves into its island's folder.
void migrate_flat_backups() {
    std::error_code ec;
    std::vector<fs::path> flat;
    for (fs::directory_iterator it(backups_root(), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && backup_name(utf8(it->path().filename()))) flat.push_back(it->path());
    for (const fs::path& p : flat) {
        const fs::path dest = island_dir_for(p) / p.filename();
        fs::create_directories(dest.parent_path(), ec);
        if (!fs::exists(dest, ec)) fs::rename(p, dest, ec);
        if (ec) log("saves: cannot move the backup %s into its island folder", utf8(p).c_str());
    }
}

std::vector<std::string> backups_sorted() {
    migrate_flat_backups();
    std::vector<std::string> v;
    std::error_code ec;
    for (fs::directory_iterator it(backups_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        std::string n = utf8(it->path().filename());
        if (it->is_directory(ec) && backup_name(n)) v.push_back(n);
    }
    // Newest first. Same-second names: "x-2" sorts after "x" and is newer, which descending order also gives,
    // but "x-10" vs "x-9" does not: compare the suffix as a number.
    auto key = [](const std::string& n) {
        return std::make_pair(n.substr(0, 15), n.size() > 16 ? std::atoi(n.c_str() + 16) : 0);
    };
    std::sort(v.begin(), v.end(), [&](const std::string& a, const std::string& b) { return key(a) > key(b); });
    return v;
}

std::string backup_locked(std::string* error) {
    fs::path dir = device_dir();
    if (!non_empty_dir(dir)) return set(error, "no save to back up"), std::string();
    std::error_code ec;
    const fs::path bdir = backups_dir();  // fixed for this backup: the folder's time part is set when it is made
    fs::create_directories(bdir, ec);
    // Same second as an existing backup: a suffix past the highest one, not the first free name (pruning may have
    // freed the bare name, and reusing it would sort the newest backup as the oldest and prune it at once).
    std::string base = stamp("%Y%m%d-%H%M%S");
    int next = 0;
    for (const std::string& n : backups_sorted())
        if (n.compare(0, base.size(), base) == 0) next = std::max(next, n.size() > 16 ? std::atoi(n.c_str() + 16) : 1);
    fs::path final_path = unique_in(bdir, next ? base + "-" + std::to_string(next + 1) : base);
    fs::path tmp = u8(utf8(final_path) + ".tmp");
    fs::remove_all(tmp, ec);
    if (!copy_tree(dir, tmp, error)) {
        fs::remove_all(tmp, ec);
        return std::string();
    }
    fs::rename(tmp, final_path, ec);
    if (ec) {
        fs::remove_all(tmp, ec);
        return set(error, "cannot finish the backup: " + ec.message()), std::string();
    }
    std::string name = utf8(final_path.filename());
    std::vector<std::string> all = backups_sorted();
    for (size_t i = kKeepBackups; i < all.size(); ++i) fs::remove_all(bdir / u8(all[i]), ec);
    for (fs::directory_iterator it(bdir, ec), end; !ec && it != end; it.increment(ec))
        if (utf8(it->path().filename()).find(".tmp") != std::string::npos) fs::remove_all(it->path(), ec);
    log("saves: backup %s", name.c_str());
    return name;
}

// Stages `from` (folder or zip) next to device_save, backs the current save up, swaps the staged one in.
bool import_locked(const fs::path& from, std::string* error) {
    recover();
    fs::path staging = u8(utf8(device_dir()) + ".new");
    std::error_code ec;
    fs::create_directories(user_dir(), ec);
    fs::remove_all(staging, ec);
    if (is_file(from)) {
        std::vector<ZipEntry> entries;
        std::string prefix;
        if (!zip_list(from, entries, error) || !zip_find_root(entries, &prefix, error)) return false;
        if (!zip_extract(from, entries, prefix, staging, error)) return fs::remove_all(staging, ec), false;
    } else {
        fs::path root;
        if (!find_root(from, &root, error)) return false;
        // Importing the save onto itself would copy it into its own staging copy and then delete it.
        if (fs::equivalent(root, device_dir(), ec)) return set(error, "this is the current save"), false;
        if (!copy_tree(root, staging, error)) return fs::remove_all(staging, ec), false;
    }
    if (!flat_save(staging, error)) return fs::remove_all(staging, ec), false;
    if (non_empty_dir(device_dir())) {
        std::string berr;
        if (backup_locked(&berr).empty()) {
            fs::remove_all(staging, ec);
            return set(error, "cannot back up the current save: " + berr), false;
        }
    }
    if (!swap_in(staging, error)) return fs::remove_all(staging, ec), false;
    log("saves: imported %s", utf8(from).c_str());
    return true;
}

// ---- detection ----

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

fs::path home() {
#if defined(_WIN32)
    std::string h = env("USERPROFILE");
#else
    std::string h = env("HOME");
#endif
    return h.empty() ? fs::path() : u8(h);
}

struct Scan {
    std::vector<Found> out;
    std::set<std::string> seen;  // canonical paths: a ~/.local/share symlink into a Flatpak folder is found twice
    fs::path own;                // our own device_save, never offered

    void add(const std::string& label, const fs::path& p) {
        std::error_code ec;
        fs::path c = fs::weakly_canonical(p, ec);
        if (ec) c = p;
        if (!own.empty() && fs::equivalent(c, own, ec)) return;
        if (!seen.insert(utf8(c)).second) return;
        out.push_back({label, utf8(p)});
    }
};

// <emu>/nand/user/save: the newer device/ location first (it wins in the emulator), then the zero user, then any.
void scan_yuzu(Scan& s, const fs::path& root, const std::string& label) {
    fs::path save = root / "nand" / "user" / "save";
    if (!is_dir(save)) return;
    const std::string titles[] = {kTitle, lower(kTitle)};
    for (const std::string& t : titles)
        if (flat_save(save / "device" / t / "0", nullptr)) s.add(label, save / "device" / t / "0");
    fs::path zero_user = save / "0000000000000000" / "00000000000000000000000000000000";
    for (const std::string& t : titles)
        if (flat_save(zero_user / t, nullptr)) s.add(label, zero_user / t);
    for (const fs::path& user : subdirs(save / "0000000000000000"))
        for (const std::string& t : titles)
            if (flat_save(user / t, nullptr)) s.add(label, user / t);
}

// <Ryujinx>/bis/user/save/<id>/0 (committed) or /1.
void scan_ryujinx(Scan& s, const fs::path& root, const std::string& label) {
    fs::path save = root / "bis" / "user" / "save";
    for (const fs::path& d : subdirs(save)) {
        if (d.filename().u8string().size() != 16) continue;
        if (flat_save(d / "0", nullptr))
            s.add(label, d / "0");
        else if (flat_save(d / "1", nullptr))
            s.add(label, d / "1");
    }
}

fs::file_time_type mtime(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? fs::file_time_type::min() : t;
}

bool zip_is_save(const fs::path& zip) {
    std::vector<ZipEntry> entries;
    std::string prefix;
    return zip_list(zip, entries, nullptr) && zip_find_root(entries, &prefix, nullptr);
}

// The newest save (folder or zip) one or two levels below a JKSV / Checkpoint title folder.
void add_newest_backup(Scan& s, const fs::path& title_dir, const std::string& label) {
    fs::path best;
    fs::file_time_type best_time = fs::file_time_type::min();
    auto consider = [&](const fs::path& p) {
        std::error_code ec;
        fs::file_time_type t;
        if (is_dir(p) && flat_save(p, nullptr))
            t = mtime(p / "main.dat");
        else if (fs::is_regular_file(p, ec) && ends_with_zip(utf8(p.filename())) && zip_is_save(p))
            t = mtime(p);
        else
            return false;
        if (best.empty() || t > best_time) best = p, best_time = t;
        return true;
    };
    std::error_code ec;
    for (fs::directory_iterator it(title_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (consider(it->path()) || !is_dir(it->path())) continue;
        std::error_code e2;
        for (fs::directory_iterator jt(it->path(), e2), jend; !e2 && jt != jend; jt.increment(e2)) consider(jt->path());
    }
    if (!best.empty()) s.add(label, best);
}

// sdmc:/JKSV/<title name>/ and sdmc:/switch/Checkpoint/saves/0x<title id> <title name>/ on a mounted volume.
void scan_volume(Scan& s, const fs::path& v, const std::string& name) {
    for (const fs::path& t : subdirs(v / "JKSV")) {
        std::string n = lower(utf8(t.filename()));
        if (n.find("animal crossing") != std::string::npos || n.find(lower(kTitle)) != std::string::npos)
            add_newest_backup(s, t, "JKSV (" + name + ")");
    }
    for (const fs::path& t : subdirs(v / "switch" / "Checkpoint" / "saves"))
        if (lower(utf8(t.filename())).rfind("0x" + lower(kTitle), 0) == 0)
            add_newest_backup(s, t, "Checkpoint (" + name + ")");
}

std::vector<fs::path> volumes() {
    std::vector<fs::path> v;
#if defined(_WIN32)
    for (char d = 'D'; d <= 'Z'; ++d) {
        fs::path p = std::string(1, d) + ":\\";
        if (is_dir(p)) v.push_back(p);
    }
#elif defined(__APPLE__)
    v = subdirs("/Volumes");
#else
    std::string user = env("USER");
    if (!user.empty()) {
        for (const fs::path& p : subdirs(u8("/media/" + user))) v.push_back(p);
        for (const fs::path& p : subdirs(u8("/run/media/" + user))) v.push_back(p);
    }
    for (const char* base : {"/media", "/mnt"})
        for (const fs::path& p : subdirs(base))
            if (utf8(p.filename()) != user) v.push_back(p);
#endif
    return v;
}

struct Emu {
    const char* folder;
    const char* label;
    bool ryujinx;
};
const Emu kEmus[] = {
    {"yuzu", "Yuzu", false},       {"suyu", "Suyu", false},   {"sudachi", "Sudachi", false},
    {"citron", "Citron", false},   {"eden", "Eden", false},   {"torzu", "Torzu", false},
    {"Ryujinx", "Ryujinx", true},
};

void scan_emu_base(Scan& s, const fs::path& base, const std::string& suffix) {
    for (const Emu& e : kEmus) {
        fs::path root = base / e.folder;
        if (!is_dir(root)) continue;
        std::string label = std::string(e.label) + suffix;
        if (e.ryujinx)
            scan_ryujinx(s, root, label);
        else
            scan_yuzu(s, root, label);
    }
}

// Other copies of this project: <somewhere>/<folder with "acnh" in its name>/private/acnh/user/device_save.
void scan_recomps(Scan& s) {
    fs::path h = home();
    if (h.empty()) return;
    for (const char* sub : {"", "Downloads", "Desktop", "Documents", "Games", "Applications", "Projects"})
        for (const fs::path& d : subdirs(*sub ? h / sub : h)) {
            if (lower(utf8(d.filename())).find("acnh") == std::string::npos) continue;
            fs::path p = d / "private" / "acnh" / "user" / "device_save";
            if (flat_save(p, nullptr)) s.add("ACNHRecomp (" + utf8(d.filename()) + ")", p);
        }
}

}  // namespace

std::string save_dir() {
    try {
        return utf8(device_dir());
    } catch (...) {
        return std::string();
    }
}

bool has_save() {
    try {
        std::lock_guard lk(g_lock);
        recover();
        return flat_save(device_dir(), nullptr);
    } catch (...) {
        return false;
    }
}

std::string last_modified() {
    try {
        return save_storage::last_modified(device_dir());
    } catch (...) {
        return std::string();
    }
}

std::vector<Found> detect_emulator_saves() {
    try {
        Scan s;
        std::error_code ec;
        s.own = fs::weakly_canonical(device_dir(), ec);
        if (ec) s.own.clear();
        fs::path h = home();
#if defined(_WIN32)
        if (std::string a = env("APPDATA"); !a.empty()) scan_emu_base(s, u8(a), "");
#elif defined(__APPLE__)
        if (!h.empty()) scan_emu_base(s, h / "Library" / "Application Support", "");
#else
        // yuzu and forks use XDG_DATA_HOME, Ryujinx XDG_CONFIG_HOME (its ApplicationData folder): look in both.
        std::string data = env("XDG_DATA_HOME"), config = env("XDG_CONFIG_HOME");
        if (data.empty() && !h.empty()) data = utf8(h / ".local" / "share");
        if (config.empty() && !h.empty()) config = utf8(h / ".config");
        if (!data.empty()) scan_emu_base(s, u8(data), "");
        if (!config.empty()) scan_emu_base(s, u8(config), "");
        // Flatpak apps keep theirs in ~/.var/app/<app id>/{data,config}/ (org.yuzu_emu.yuzu, org.ryujinx.Ryujinx,
        // io.github.ryubing.Ryujinx...): every app is checked, so app ids we do not know are found too.
        if (!h.empty())
            for (const fs::path& app : subdirs(h / ".var" / "app")) {
                scan_emu_base(s, app / "data", " (Flatpak)");
                scan_emu_base(s, app / "config", " (Flatpak)");
            }
#endif
        scan_recomps(s);
        for (const fs::path& v : volumes())
            scan_volume(s, v, v.filename().empty() ? utf8(v) : utf8(v.filename()));
        return s.out;
    } catch (...) {
        return {};
    }
}

bool looks_like_acnh_save(const std::string& dir, std::string* why) {
    try {
        fs::path p = u8(dir);
        if (is_file(p)) {
            std::vector<ZipEntry> entries;
            std::string prefix;
            return zip_list(p, entries, why) && zip_find_root(entries, &prefix, why);
        }
        fs::path root;
        return find_root(p, &root, why);
    } catch (...) {
        set(why, "cannot read this folder");
        return false;
    }
}

bool import_save(const std::string& from, std::string* error) {
    try {
        std::lock_guard lk(g_lock);
        return import_locked(u8(from), error);
    } catch (...) {
        set(error, "unexpected error");
        return false;
    }
}

bool export_save(const std::string& to, std::string* written, std::string* error) {
    try {
        std::lock_guard lk(g_lock);
        recover();
        fs::path src = device_dir();
        if (!flat_save(src, error)) return set(error, "no save to export"), false;
        std::error_code ec;
        fs::path dest = u8(to);
        if (ends_with_zip(to)) {
            if (dest.has_parent_path() && !is_dir(dest.parent_path()))
                return set(error, "folder not found: " + utf8(dest.parent_path())), false;
            fs::path tmp = u8(to + ".tmp");
            if (!zip_write(src, tmp, error)) return fs::remove(tmp, ec), false;
            fs::remove(dest, ec);  // rename does not replace an existing file on Windows
            fs::rename(tmp, dest, ec);
            if (ec) return fs::remove(tmp, ec), set(error, "cannot write " + to), false;
        } else {
            fs::create_directories(dest, ec);
            if (!is_dir(dest)) return set(error, "cannot create " + to), false;
            fs::path final_path = unique_in(dest, "ACNH-" + stamp("%Y%m%d-%H%M%S"));
            fs::path tmp = u8(utf8(final_path) + ".partial");
            fs::remove_all(tmp, ec);
            if (!copy_tree(src, tmp, error)) return fs::remove_all(tmp, ec), false;
            fs::rename(tmp, final_path, ec);
            if (ec) return fs::remove_all(tmp, ec), set(error, "cannot finish " + utf8(final_path)), false;
            dest = final_path;
        }
        set(written, utf8(dest));
        log("saves: exported to %s", utf8(dest).c_str());
        return true;
    } catch (...) {
        set(error, "unexpected error");
        return false;
    }
}

std::string backup_now(std::string* error) {
    try {
        std::lock_guard lk(g_lock);
        recover();
        return backup_locked(error);
    } catch (...) {
        set(error, "unexpected error");
        return std::string();
    }
}

std::string backups_folder() { return utf8(backups_dir()); }

std::vector<std::string> list_backups() {
    try {
        std::lock_guard lk(g_lock);
        return backups_sorted();
    } catch (...) {
        return {};
    }
}

bool restore_backup(const std::string& name, std::string* error) {
    try {
        std::lock_guard lk(g_lock);
        if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos)
            return set(error, "invalid backup name"), false;
        fs::path p = backups_dir() / u8(name);
        if (!is_dir(p)) return set(error, "backup not found: " + name), false;
        // The backup is staged before the current save is backed up, so the pruning that follows that backup cannot
        // delete the one being restored.
        return import_locked(p, error);
    } catch (...) {
        set(error, "unexpected error");
        return false;
    }
}

}  // namespace nx::saves
