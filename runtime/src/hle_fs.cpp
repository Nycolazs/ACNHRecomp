// nn::fs at the API level, on host files. The game imports these by name (main), so no fsp-srv IPC is involved:
//   rom:   -> private/extract/romfs (the RomFS with the update applied)
//   save:  -> private/user/save/<mount>
//   cache: -> private/user/cache<index>
//   add-on content (MountAddOnContent) -> private/aoc/<index>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <nx/runtime.h>

#include "platform.h"
#include "users.h"

namespace fs = std::filesystem;

namespace nx {

void discord_presence_file(const std::string& guest_path);  // discord_presence.cpp: room and island models

namespace {

// NX_TRACE_FS=1: every file the game opens (and the ones it looks for and does not find).
bool trace_fs() {
    static const bool on = g_trace || std::getenv("NX_TRACE_FS") != nullptr;
    return on;
}

// The last opens, newest last: a crash report that says what was being loaded (log_recent_files, read by the crash
// handlers; try_lock there, a handler must not wait for a thread that holds the lock).
constexpr size_t kRecentOps = 24;
std::mutex g_recent_lock;
struct RecentOp {
    int64_t thread = 0;
    uint64_t seq = 0;
    char text[200] = {0};
} g_recent[kRecentOps];
uint64_t g_recent_seq = 0;

void note_open(const char* what, const std::string& guest_path, const char* result) {
    if (result[0] == 'o' && result[1] == 'k' && !result[2]) discord_presence_file(guest_path);
    std::lock_guard lk(g_recent_lock);
    RecentOp& r = g_recent[g_recent_seq % kRecentOps];
    r.thread = plat::current_thread_id();
    r.seq = ++g_recent_seq;
    std::snprintf(r.text, sizeof r.text, "%s %s: %s", what, guest_path.c_str(), result);
}

constexpr uint32_t kOk = 0;
constexpr uint32_t kPathNotFound = 0x202;       // fs 1
constexpr uint32_t kPathAlreadyExists = 0x402;  // fs 2
constexpr uint32_t kTargetNotFound = 0x2EE202;  // fs 6001-ish: generic "not mounted"

std::mutex g_fs;
std::map<std::string, fs::path> g_mounts;
// Open files: shared so a read in progress survives a close from another thread. Reads run outside g_fs (which
// only guards the tables): one global lock made every streaming thread (Bink animations, FMOD banks) wait behind
// any big read, felt as hitches.
struct File {
    FILE* f;
    std::string path;
    uint64_t written = 0;  // bytes the game wrote through this handle (logged at the close: saves)
    int64_t base = 0, size = -1;  // a file inside the packed RomFS: its bytes in the pack (size -1: a file of its own)
    bool owns = true;             // the packed RomFS's FILE is shared by all its files
    File(FILE* file, std::string p) : f(file), path(std::move(p)) {}
    File(FILE* pack, std::string p, int64_t at, int64_t n) : f(pack), path(std::move(p)), base(at), size(n), owns(false) {}
    File(const File&) = delete;  // owns the FILE: one instance only
    File& operator=(const File&) = delete;
    ~File() {
        if (owns) std::fclose(f);
    }
};
std::map<uint64_t, std::shared_ptr<File>> g_files;

// Files the runtime adds or replaces (mods): key = path under the mount point, '/' separated, lower case.
std::map<std::string, std::string> g_virtual;

std::string virtual_key(const fs::path& p) {
    auto g = p.lexically_normal().generic_u8string();
    std::string s(g.begin(), g.end());
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// The virtual file whose key the resolved path ends with, or nullptr.
const std::string* find_virtual(const fs::path& resolved) {
    if (g_virtual.empty() || resolved.empty()) return nullptr;
    std::string k = virtual_key(resolved);
    for (auto& [key, data] : g_virtual)
        if (k.size() >= key.size() + 1 && k.compare(k.size() - key.size(), key.size(), key) == 0 &&
            k[k.size() - key.size() - 1] == '/')
            return &data;
    return nullptr;
}
struct DirEntry {
    std::string name;
    bool dir = false;
    int64_t size = 0;
};
struct Dir {
    std::vector<DirEntry> entries;
    size_t pos = 0;
};

// The packed RomFS (tools/pack_romfs.py): <data root>/romfs.pak, when there, serves every RomFS file from one file
// instead of the ~100,000 under extract/romfs. On a phone's memory card (exFAT, 128 KiB clusters) the loose files
// took twice their size and each open was a slow lookup. Format: "NXRP", u32 version 1, u32 count, then per file
// u16 path length, the path ('/' separated, relative to the RomFS), u64 offset, u64 size; the data after.
struct Pak {
    FILE* f = nullptr;
    std::unordered_map<std::string, std::pair<int64_t, int64_t>> files;  // path -> (offset, size)
    std::unordered_map<std::string, std::vector<DirEntry>> dirs;        // directory ("" the root) -> its entries
};
const Pak* pak() {
    static const Pak* p = []() -> const Pak* {
        FILE* f = plat::fopen_utf8(plat::data_root() + "/romfs.pak", "rb");
        if (!f) return nullptr;
        auto* k = new Pak;
        k->f = f;
        char magic[4];
        uint32_t version = 0, count = 0;
        if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "NXRP", 4) != 0 || std::fread(&version, 4, 1, f) != 1 ||
            version != 1 || std::fread(&count, 4, 1, f) != 1) {
            log("fs: romfs.pak: not a packed RomFS");
            return nullptr;
        }
        std::set<std::string> seen_dirs;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t len = 0;
            int64_t off = 0, size = 0;
            std::string path;
            if (std::fread(&len, 2, 1, f) != 1) return nullptr;
            path.resize(len);
            if (std::fread(path.data(), 1, len, f) != len || std::fread(&off, 8, 1, f) != 1 || std::fread(&size, 8, 1, f) != 1)
                return nullptr;
            k->files[path] = {off, size};
            // Its parent directories, each listed once in its own parent.
            std::string child = path;
            int64_t child_size = size;
            bool child_dir = false;
            for (;;) {
                size_t slash = child.rfind('/');
                std::string parent = slash == std::string::npos ? "" : child.substr(0, slash);
                std::string name = slash == std::string::npos ? child : child.substr(slash + 1);
                k->dirs[parent].push_back({name, child_dir, child_dir ? 0 : child_size});
                if (parent.empty() || !seen_dirs.insert(parent).second) break;
                child = parent, child_dir = true;
            }
        }
        log("fs: packed RomFS: %u files from romfs.pak", count);
        return k;
    }();
    return p;
}
std::map<uint64_t, Dir> g_dirs;
uint64_t g_next = 1;

fs::path data_root() { return fs::u8path(plat::data_root()); }

std::string utf8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string cstr(uint64_t p) { return p ? std::string(reinterpret_cast<const char*>(p)) : std::string(); }

}  // namespace
std::string glyph_override(const std::string& romfs_path);  // glyphs.cpp
namespace {

// Mods layered over the RomFS (LayeredFS): NX_MODS="ptbr,other" (config.ini game.mods / game.language = pt-BR).
// A file the game opens on a RomFS mount is taken from <data root>/mods/<mod>/romfs/<same path> when a mod has it,
// the first listed mod winning. Only files are layered (a mod replaces what the game already has).
const std::vector<fs::path>& mod_roots() {
    static const std::vector<fs::path> roots = [] {
        std::vector<fs::path> v;
        const char* e = std::getenv("NX_MODS");
        std::string list = e ? e : "";
        size_t pos = 0;
        while (pos < list.size()) {
            size_t comma = list.find(',', pos);
            std::string name = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? list.size() : comma + 1;
            while (!name.empty() && name.back() == ' ') name.pop_back();
            while (!name.empty() && name.front() == ' ') name.erase(0, 1);
            if (name.empty() || name.find("..") != std::string::npos) continue;
            fs::path root = data_root() / "mods" / fs::u8path(name) / "romfs";
            std::error_code ec;
            if (fs::is_directory(root, ec)) {
                v.push_back(root);
                log("fs: mod %s layered over the RomFS", name.c_str());
            } else {
                log("fs: mod %s not installed (%s missing)", name.c_str(), root.string().c_str());
            }
        }
        return v;
    }();
    return roots;
}

// The RomFS with the update applied, loose: <data root>/romfs (the game folder ACNHRecomp-Importer prepares) or
// <data root>/extract/romfs (a setup's data folder).
fs::path romfs_root() {
    static const fs::path p = [] {
        std::error_code ec;
        fs::path r = data_root() / "romfs";
        return fs::is_directory(r, ec) ? r : data_root() / "extract" / "romfs";
    }();
    return p;
}

// "mount:/a/b" -> host path; empty when the mount is unknown.
fs::path resolve(const std::string& guest) {
    auto colon = guest.find(':');
    if (colon == std::string::npos) return {};
    auto it = g_mounts.find(guest.substr(0, colon));
    if (it == g_mounts.end()) return {};
    std::string rest = guest.substr(colon + 1);
    while (!rest.empty() && (rest[0] == '/' || rest[0] == '\\')) rest.erase(0, 1);
    static const fs::path romfs = romfs_root();
    // The button icons of the chosen controller style (glyphs.cpp: a redrawn copy of the game's icon font).
    if (it->second == romfs && !rest.empty())
        if (std::string g = glyph_override(rest); !g.empty()) return fs::u8path(g);
    if (!mod_roots().empty() && it->second == romfs && !rest.empty()) {
        std::error_code ec;
        for (const fs::path& root : mod_roots()) {
            fs::path p = root / fs::u8path(rest);
            if (fs::is_regular_file(p, ec)) return p;
        }
    }
    return it->second / fs::u8path(rest);
}

// The path inside the packed RomFS of a resolved host path, when it is one of the RomFS's own (not a mod's or an
// override's) and the pack is there; else nullptr.
const Pak* in_pak(const fs::path& p, std::string& rel) {
    const Pak* k = pak();
    if (!k) return nullptr;
    // '/' throughout: on Windows the joined paths have '\' separators, the pack's names '/'.
    auto slashes = [](std::string v) {
        std::replace(v.begin(), v.end(), '\\', '/');
        return v;
    };
    static const std::string romfs = slashes(utf8(romfs_root()));
    std::string s = slashes(utf8(p));
    if (s.compare(0, romfs.size(), romfs) != 0 || (s.size() > romfs.size() && s[romfs.size()] != '/')) return nullptr;
    rel = s.size() > romfs.size() ? s.substr(romfs.size() + 1) : "";
    while (!rel.empty() && rel.back() == '/') rel.pop_back();
    return k;
}

void mount(const std::string& name, const fs::path& p) {
    fs::create_directories(p);
    g_mounts[name] = p;
    log("fs: mounted %s: -> %s", name.c_str(), p.string().c_str());
}

// ---- mounts ---------------------------------------------------------------------------------------------------

void QueryMountRomCacheSize(CPU& c) {
    st<uint64_t>(c.x[0], 0x1000);
    c.x[0] = kOk;
}

void MountRom(CPU& c) {
    std::lock_guard lk(g_fs);
    mount(cstr(c.x[0]), romfs_root());
    c.x[0] = kOk;
}

// The save folder of the account::Uid at a guest address: user/save for the first user, user/save_<uid> for the others.
fs::path save_dir_of(uint64_t uid_addr) {
    uint64_t lo = ld<uint64_t>(uid_addr), hi = ld<uint64_t>(uid_addr + 8);
    user_opened(lo, hi);
    return data_root() / "user" / user_save_dir(lo, hi);
}

void EnsureSaveData(CPU& c) {
    std::error_code ec;
    fs::path p = save_dir_of(c.x[0]);
    fs::create_directories(p, ec);
    log("fs: EnsureSaveData -> %s", p.string().c_str());
    c.x[0] = kOk;
}

void MountSaveData(CPU& c) {
    std::lock_guard lk(g_fs);
    std::string name = cstr(c.x[0]);
    mount(name, save_dir_of(c.x[1]));
    c.x[0] = kOk;
}

void Unmount(CPU& c) {
    std::lock_guard lk(g_fs);
    g_mounts.erase(cstr(c.x[0]));
    c.x[0] = kOk;
}

// Saves are logged (rare, small): what the game created, wrote, renamed and committed, and each failure.
void Commit(CPU& c) {  // (const char* mount[, const CommitOption&])
    log("fs: save: Commit(%s)", cstr(c.x[0]).c_str());
    c.x[0] = kOk;
}

void CommitMultiple(CPU& c) {  // (const char* const* mounts, int count)
    std::string names;
    for (int k = 0; k < int(c.x[1]) && k < 8; k++) names += (k ? ", " : "") + cstr(ld<uint64_t>(c.x[0] + 8ull * k));
    log("fs: save: Commit(%s)", names.c_str());
    c.x[0] = kOk;
}

// ---- device save data (ACNH: the island is one save for the whole console, not per user) ----
// user/device_save. Its size: what the game asked for last (ExtendDeviceSaveData), kept in user/device_save.size; until
// then a size larger than any game asks for, so the game never needs to extend it. Commits write nothing extra:
// files are written in place (a crash in the middle of a save can leave it half written, unlike the console's
// journal; back up user/device_save to be safe).
fs::path device_save_dir() { return data_root() / "user" / "device_save"; }

void MountDeviceSaveData(CPU& c) {  // Result (const char* name)
    std::lock_guard lk(g_fs);
    mount(cstr(c.x[0]), device_save_dir());
    c.x[0] = kOk;
}

void GetDeviceSaveDataSize(CPU& c) {  // Result (int64_t* size, int64_t* journal size)
    int64_t size = 0x40000000, journal = 0x40000000;
    if (FILE* f = plat::fopen_utf8(utf8(data_root() / "user" / "device_save.size"), "rb")) {
        long long a = 0, b = 0;
        if (std::fscanf(f, "%lld %lld", &a, &b) == 2) size = a, journal = b;
        std::fclose(f);
    }
    if (c.x[0]) st<int64_t>(c.x[0], size);
    if (c.x[1]) st<int64_t>(c.x[1], journal);
    c.x[0] = kOk;
}

void ExtendDeviceSaveData(CPU& c) {  // Result (int64_t size, int64_t journal size)
    std::error_code ec;
    fs::create_directories(data_root() / "user", ec);
    if (FILE* f = plat::fopen_utf8(utf8(data_root() / "user" / "device_save.size"), "wb")) {
        std::fprintf(f, "%lld %lld\n", (long long)int64_t(c.x[0]), (long long)int64_t(c.x[1]));
        std::fclose(f);
    }
    log("fs: ExtendDeviceSaveData(%#llx, %#llx)", (unsigned long long)c.x[0], (unsigned long long)c.x[1]);
    c.x[0] = kOk;
}

void GetSaveDataRestoreFlag(CPU& c) {  // Result (bool*, const char* mount): never restored
    if (c.x[0]) st<uint8_t>(c.x[0], 0);
    c.x[0] = kOk;
}

void SetPriorityOnCurrentThread(CPU&) {}
void EnableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }
void DisableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }

// ---- files ----------------------------------------------------------------------------------------------------

void CreateFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    int64_t size = int64_t(c.x[1]);
    if (p.empty() || !fs::exists(p.parent_path())) {
        log("fs: save: CreateFile(%s, %lld): no parent directory", cstr(c.x[0]).c_str(), (long long)size);
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), "wb");
    if (f) {
        if (size > 0) {
            plat::fseek64(f, size - 1, SEEK_SET);
            fputc(0, f);
        }
        fclose(f);
    }
    log("fs: save: CreateFile(%s, %lld)%s", cstr(c.x[0]).c_str(), (long long)size, f ? "" : " failed on the host");
    c.x[0] = f ? kOk : kPathNotFound;
}

void DeleteFile(CPU& c) {
    std::lock_guard lk(g_fs);
    std::string guest = cstr(c.x[0]);
    fs::path p = resolve(guest);
    std::error_code ec;
    bool ok = !p.empty() && fs::remove(p, ec);
    log("fs: save: DeleteFile(%s)%s", guest.c_str(), ok ? "" : ": not found");
    c.x[0] = ok ? kOk : kPathNotFound;
}

void CreateDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    if (p.empty()) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    std::error_code ec;
    bool ok = fs::create_directory(p, ec);
    log("fs: save: CreateDirectory(%s)%s", cstr(c.x[0]).c_str(), ok ? "" : ": failed");
    c.x[0] = ok ? kOk : kPathNotFound;
}

void GetEntryType(CPU& c) {  // (DirectoryEntryType* out, path)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    if (find_virtual(p)) {
        st<uint32_t>(c.x[0], 1);
        c.x[0] = kOk;
        return;
    }
    std::string rel;
    if (const Pak* k = in_pak(p, rel)) {
        bool file = k->files.count(rel) != 0, dir = rel.empty() || k->dirs.count(rel) != 0;
        if (file || dir) st<uint32_t>(c.x[0], dir ? 0 : 1);
        c.x[0] = file || dir ? kOk : kPathNotFound;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::exists(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    st<uint32_t>(c.x[0], fs::is_directory(p) ? 0 : 1);
    c.x[0] = kOk;
}

void OpenFile(CPU& c) {  // (FileHandle* out, path, int mode: 1 read, 2 write, 4 append)
    std::lock_guard lk(g_fs);
    std::string guest = cstr(c.x[1]);
    fs::path p = resolve(guest);
    int mode = int(c.x[2]);
    if (const std::string* v = find_virtual(p)) {  // served from memory, through a temporary file
        FILE* f = std::tmpfile();
        if (!f) {
            c.x[0] = kPathNotFound;
            return;
        }
        std::fwrite(v->data(), 1, v->size(), f);
        std::fflush(f);
        uint64_t h = g_next++;
        g_files[h] = std::make_shared<File>(f, guest);
        st<uint64_t>(c.x[0], h);
        note_open("OpenFile", guest, "virtual");
        if (trace_fs()) log("fs: OpenFile(%s) -> virtual, %zu bytes", guest.c_str(), v->size());
        c.x[0] = kOk;
        return;
    }
    std::string rel;
    if (const Pak* k = in_pak(p, rel)) {
        auto it = k->files.find(rel);
        if (it == k->files.end() || (mode & 2)) {
            note_open("OpenFile", guest, "not found");
            if (trace_fs()) log("fs: OpenFile(%s) not in romfs.pak", guest.c_str());
            c.x[0] = kPathNotFound;
            return;
        }
        uint64_t h = g_next++;
        g_files[h] = std::make_shared<File>(k->f, guest, it->second.first, it->second.second);
        st<uint64_t>(c.x[0], h);
        note_open("OpenFile", guest, "ok");
        if (trace_fs()) log("fs: OpenFile(%s, %d) -> %llu (packed)", guest.c_str(), mode, (unsigned long long)h);
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) {
        note_open("OpenFile", guest, p.empty() ? "unknown mount" : "not found");
        if (trace_fs() || (mode & 2)) log("fs: OpenFile(%s, %d) not found", guest.c_str(), mode);
        c.x[0] = kPathNotFound;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), (mode & 2) ? "r+b" : "rb");
    if (!f) {
        note_open("OpenFile", guest, "the host could not open it");
        if (trace_fs()) log("fs: OpenFile(%s) failed to open on the host (errno %d)", guest.c_str(), errno);
        c.x[0] = kPathNotFound;
        return;
    }
    uint64_t h = g_next++;
    g_files[h] = std::make_shared<File>(f, guest);
    st<uint64_t>(c.x[0], h);
    note_open("OpenFile", guest, "ok");
    if (trace_fs()) log("fs: OpenFile(%s, %d) -> %llu", guest.c_str(), mode, (unsigned long long)h);
    c.x[0] = kOk;
}

std::shared_ptr<File> open_file(uint64_t h) {
    std::lock_guard lk(g_fs);
    auto it = g_files.find(h);
    return it == g_files.end() ? nullptr : it->second;
}

void CloseFile(CPU& c) {
    std::lock_guard lk(g_fs);
    auto it = g_files.find(c.x[0]);
    if (it == g_files.end()) return;
    if (it->second->written)
        log("fs: save: wrote %llu bytes to %s", (unsigned long long)it->second->written, it->second->path.c_str());
    g_files.erase(it);
}

void GetFileSize(CPU& c) {  // (s64* out, FileHandle)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    if (file->size >= 0) {  // packed
        st<int64_t>(c.x[0], file->size);
        c.x[0] = kOk;
        return;
    }
    std::lock_guard lk(g_fs);  // the FILE position is shared with writers
    FILE* f = file->f;
    int64_t cur = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_END);
    st<int64_t>(c.x[0], plat::ftell64(f));
    if (g_trace) log("fs: size %s = %lld", file->path.c_str(), (long long)ld<int64_t>(c.x[0]));
    plat::fseek64(f, cur, SEEK_SET);
    c.x[0] = kOk;
}

uint64_t read_at(const File& file, int64_t off, uint64_t buf, uint64_t size) {
    uint64_t t0 = plat::monotonic_ns();
    if (file.size >= 0) {  // packed: within the file's bytes of the pack
        if (off < 0 || off >= file.size) return 0;
        size = std::min<uint64_t>(size, uint64_t(file.size - off));
        off += file.base;
    }
    uint64_t n = plat::read_at(file.f, off, reinterpret_cast<void*>(buf), size);
    uint64_t ms = (plat::monotonic_ns() - t0) / 1'000'000;
    if (g_trace) log("fs: read %s at %lld size %llu -> %llu", file.path.c_str(), (long long)off, (unsigned long long)size,
                     (unsigned long long)n);
    if (trace_fs() && file.path.find(".bwav") != std::string::npos) {  // streamed audio: is it being consumed?
        static std::mutex m;
        static std::map<std::string, uint64_t> reads;
        std::lock_guard lk(m);
        uint64_t k = ++reads[file.path];
        if (k <= 3 || k % 50 == 0) log("fs: stream read #%llu %s at %lld", (unsigned long long)k, file.path.c_str(), (long long)off);
    }
    if (ms >= 30)
        log("fs: slow read %s: %llu bytes at %lld took %llu ms", file.path.c_str(), (unsigned long long)size,
            (long long)off, (unsigned long long)ms);
    return n;
}

void ReadFileOut(CPU& c) {  // (size_t* out, FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    st<uint64_t>(c.x[0], read_at(*file, int64_t(c.x[2]), c.x[3], c.x[4]));
    c.x[0] = kOk;
}

void ReadFile(CPU& c) {  // (FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    read_at(*file, int64_t(c.x[1]), c.x[2], c.x[3]);
    c.x[0] = kOk;
}

void WriteFile(CPU& c) {  // (FileHandle, s64 off, const void* buf, size_t size, const WriteOption&)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    std::lock_guard lk(g_fs);
    plat::fseek64(file->f, int64_t(c.x[1]), SEEK_SET);
    size_t n = fwrite(reinterpret_cast<const void*>(c.x[2]), 1, c.x[3], file->f);
    fflush(file->f);  // positional reads bypass the FILE buffer (saves only: rare and small)
    file->written += n;
    if (n != c.x[3])
        log("fs: save: WriteFile(%s) wrote %zu of %llu bytes (errno %d)", file->path.c_str(), n,
            (unsigned long long)c.x[3], errno);
    c.x[0] = kOk;
}

void FlushFile(CPU& c) {
    if (auto file = open_file(c.x[0])) {
        std::lock_guard lk(g_fs);
        fflush(file->f);
    }
    c.x[0] = kOk;
}

// ---- directories ----------------------------------------------------------------------------------------------

void OpenDirectory(CPU& c) {  // (DirectoryHandle* out, path, int mode: 1 dirs, 2 files)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    int mode = int(c.x[2]);
    std::string rel;
    if (const Pak* k = in_pak(p, rel)) {
        auto it = k->dirs.find(rel);
        note_open("OpenDirectory", cstr(c.x[1]), it != k->dirs.end() ? "ok" : "not found");
        if (it == k->dirs.end()) {
            c.x[0] = kPathNotFound;
            return;
        }
        Dir d;
        for (const DirEntry& e : it->second)
            if ((e.dir && (mode & 1)) || (!e.dir && (mode & 2))) d.entries.push_back(e);
        uint64_t h = g_next++;
        g_dirs[h] = std::move(d);
        st<uint64_t>(c.x[0], h);
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    bool found = !p.empty() && fs::is_directory(p, ec);
    note_open("OpenDirectory", cstr(c.x[1]), found ? "ok" : "not found");
    if (trace_fs()) log("fs: OpenDirectory(%s, %d)%s", cstr(c.x[1]).c_str(), mode, found ? "" : " not found");
    if (!found) {
        c.x[0] = kPathNotFound;
        return;
    }
    Dir d;
    for (auto& e : fs::directory_iterator(p, ec)) {
        bool dir = e.is_directory();
        auto u8 = e.path().filename().u8string();
        if ((dir && (mode & 1)) || (!dir && (mode & 2)))
            d.entries.push_back({std::string(u8.begin(), u8.end()), dir, dir ? 0 : int64_t(e.file_size(ec))});
    }
    uint64_t h = g_next++;
    g_dirs[h] = std::move(d);
    st<uint64_t>(c.x[0], h);
    c.x[0] = kOk;
}

void ReadDirectory(CPU& c) {  // (s64* out_count, DirectoryEntry* entries, DirectoryHandle, s64 max)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[2]);
    if (it == g_dirs.end()) {
        c.x[0] = kTargetNotFound;
        return;
    }
    Dir& d = it->second;
    int64_t n = 0, max = int64_t(c.x[3]);
    for (; n < max && d.pos < d.entries.size(); n++, d.pos++) {
        const auto& e = d.entries[d.pos];
        uint64_t out = c.x[1] + uint64_t(n) * 0x310;
        std::memset(reinterpret_cast<void*>(out), 0, 0x310);
        std::memcpy(reinterpret_cast<void*>(out), e.name.c_str(), std::min<size_t>(e.name.size(), 0x300));
        st<int8_t>(out + 0x304, e.dir ? 0 : 1);
        st<int64_t>(out + 0x308, e.size);
    }
    st<int64_t>(c.x[0], n);
    c.x[0] = kOk;
}

void GetDirectoryEntryCount(CPU& c) {  // (s64* out, DirectoryHandle)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[1]);
    st<int64_t>(c.x[0], it == g_dirs.end() ? 0 : int64_t(it->second.entries.size()));
    c.x[0] = kOk;
}

void CloseDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    g_dirs.erase(c.x[0]);
}

// ---- cache storage --------------------------------------------------------------------------------------------

void OpenCacheStorageList(CPU& c) {
    st<uint64_t>(c.x[0], 1);
    c.x[0] = kOk;
}
void ReadCacheStorageList(CPU& c) {  // (int* out, CacheStorageInfo*, handle, int)
    st<int32_t>(c.x[0], 0);
    c.x[0] = kOk;
}
void CloseCacheStorageList(CPU& c) {}
void CreateCacheStorage(CPU& c) { c.x[0] = kOk; }
void DeleteCacheStorage(CPU& c) { c.x[0] = kOk; }
void MountCacheStorage(CPU& c) {  // (name, int index)
    std::lock_guard lk(g_fs);
    mount(cstr(c.x[0]), data_root() / "user" / ("cache" + std::to_string(int(c.x[1]))));
    c.x[0] = kOk;
}

// ---- add-on content (DLC): aoc/<index>/, from the game folder (one RomFS folder per add-on) -------------------------
// nn::aoc answers with the indexes found there, nn::fs::MountAddOnContent mounts one of them. Without the folder the
// game sees no add-on content, as on a console that never bought any.
std::vector<int> aoc_indexes() {
    std::vector<int> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(data_root() / "aoc", ec)) {
        if (!e.is_directory()) continue;
        std::string n = utf8(e.path().filename());
        if (!n.empty() && n.find_first_not_of("0123456789") == std::string::npos) out.push_back(std::stoi(n));
    }
    std::sort(out.begin(), out.end());
    return out;
}

void CountAddOnContent(CPU& c) {  // int ()
    static bool once = [] {
        auto v = aoc_indexes();
        std::string list;
        for (int i : v) list += " " + std::to_string(i);
        log("aoc: %zu add-on content(s):%s", v.size(), list.c_str());
        return true;
    }();
    (void)once;
    c.x[0] = uint64_t(aoc_indexes().size());
}

void ListAddOnContent(CPU& c) {  // int (int* out, int offset, int count)
    auto v = aoc_indexes();
    int off = int(c.x[1]), n = int(c.x[2]), k = 0;
    for (int i = std::max(off, 0); i < int(v.size()) && k < n; i++, k++) st<int32_t>(c.x[0] + 4 * k, v[i]);
    c.x[0] = uint64_t(k);
}

void QueryMountAddOnContentCacheSize(CPU& c) {  // Result (size_t*, int index)
    st<uint64_t>(c.x[0], 0x1000);
    c.x[0] = kOk;
}

void MountAddOnContent(CPU& c) {  // Result (const char* name, int index, void* cache, size_t)
    std::lock_guard lk(g_fs);
    fs::path p = data_root() / "aoc" / std::to_string(int(c.x[1]));
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        log("aoc: MountAddOnContent(%d): not imported", int(c.x[1]));
        c.x[0] = kTargetNotFound;
        return;
    }
    g_mounts[cstr(c.x[0])] = p;
    log("fs: mounted %s: -> %s (add-on %d)", cstr(c.x[0]).c_str(), p.string().c_str(), int(c.x[1]));
    c.x[0] = kOk;
}

// ---- SDK 16 additions (Super Mario Bros. Wonder) ---------------------------------------------------------------
// MountRom(name) without a cache buffer, and ReadFile(size_t* out, FileHandle, s64, void*, size_t, const ReadOption&)
// are the older calls with one argument less / more: the same functions serve them.

void IsSaveDataExisting(CPU& c) { c.x[0] = 1; }  // (const account::Uid&) -> bool: EnsureSaveData made it

void SetFileSize(CPU& c) {  // (FileHandle, s64 size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    std::lock_guard lk(g_fs);
    std::fflush(file->f);
#if defined(_WIN32)
    bool ok = _chsize_s(_fileno(file->f), int64_t(c.x[1])) == 0;
#else
    bool ok = ftruncate(fileno(file->f), off_t(c.x[1])) == 0;
#endif
    c.x[0] = ok ? kOk : kPathNotFound;
}

void CleanDirectoryRecursively(CPU& c) {  // the directory's contents go, the directory stays
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    if (p.empty() || !fs::is_directory(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    for (auto& e : fs::directory_iterator(p, ec)) fs::remove_all(e.path(), ec);
    c.x[0] = ec ? kPathNotFound : kOk;
}

void RenameDirectory(CPU& c) {  // (const char* from, const char* to)
    std::lock_guard lk(g_fs);
    fs::path a = resolve(cstr(c.x[0])), b = resolve(cstr(c.x[1]));
    std::error_code ec;
    if (a.empty() || b.empty() || !fs::is_directory(a, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(b, ec)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    fs::rename(a, b, ec);
    log("fs: save: RenameDirectory(%s, %s)%s", cstr(c.x[0]).c_str(), cstr(c.x[1]).c_str(), ec ? ": failed" : "");
    c.x[0] = ec ? kPathNotFound : kOk;
}

void DeleteDirectoryRecursively(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    if (p.empty() || !fs::is_directory(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    fs::remove_all(p, ec);
    c.x[0] = ec ? kPathNotFound : kOk;
}

}  // namespace

NX_HLE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", QueryMountRomCacheSize);
NX_HLE("_ZN2nn2fs19MountDeviceSaveDataEPKc", MountDeviceSaveData);
NX_HLE("_ZN2nn2fs21GetDeviceSaveDataSizeEPlS1_", GetDeviceSaveDataSize);
NX_HLE("_ZN2nn2fs20ExtendDeviceSaveDataEll", ExtendDeviceSaveData);
NX_HLE("_ZN2nn2fs22GetSaveDataRestoreFlagEPbPKc", GetSaveDataRestoreFlag);
NX_HLE("_ZN2nn2fs6CommitEPKPKci", CommitMultiple);
NX_HLE("_ZN2nn2fs6CommitEPKcRKNS0_12CommitOptionE", Commit);
NX_HLE("_ZN2nn2fs25CleanDirectoryRecursivelyEPKc", CleanDirectoryRecursively);
NX_HLE("_ZN2nn2fs15RenameDirectoryEPKcS2_", RenameDirectory);
NX_HLE("_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE", SetPriorityOnCurrentThread);
NX_HLE("_ZN2nn2fs8MountRomEPKcPvm", MountRom);
NX_HLE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", EnsureSaveData);
NX_HLE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", MountSaveData);
NX_HLE("_ZN2nn2fs7UnmountEPKc", Unmount);
NX_HLE("_ZN2nn2fs10DeleteFileEPKc", DeleteFile);
NX_HLE("_ZN2nn2fs10CreateFileEPKcl", CreateFile);
NX_HLE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", OpenFile);
NX_HLE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", GetFileSize);
NX_HLE("_ZN2nn2fs15CreateDirectoryEPKc", CreateDirectory);
NX_HLE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", GetEntryType);
NX_HLE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", CloseFile);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", ReadFileOut);
NX_HLE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", ReadFile);
NX_HLE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", WriteFile);
NX_HLE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", FlushFile);
NX_HLE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", EnableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs26DisableGlobalFileDataCacheEv", DisableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs6CommitEPKc", Commit);
NX_HLE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", OpenDirectory);
NX_HLE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", ReadDirectory);
NX_HLE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", CloseDirectory);
NX_HLE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", GetDirectoryEntryCount);
NX_HLE("_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", OpenCacheStorageList);
NX_HLE("_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", ReadCacheStorageList);
NX_HLE("_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", CloseCacheStorageList);
NX_HLE("_ZN2nn2fs18CreateCacheStorageEill", CreateCacheStorage);
NX_HLE("_ZN2nn2fs18DeleteCacheStorageEi", DeleteCacheStorage);
NX_HLE("_ZN2nn2fs17MountCacheStorageEPKci", MountCacheStorage);
NX_HLE("_ZN2nn2fs8MountRomEPKc", MountRom);
NX_HLE("_ZN2nn3aoc17CountAddOnContentEv", CountAddOnContent);
NX_HLE("_ZN2nn3aoc16ListAddOnContentEPiii", ListAddOnContent);
NX_HLE("_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi", QueryMountAddOnContentCacheSize);
NX_HLE("_ZN2nn2fs17MountAddOnContentEPKciPvm", MountAddOnContent);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", ReadFileOut);
NX_HLE("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", IsSaveDataExisting);
NX_HLE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", SetFileSize);
NX_HLE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", DeleteDirectoryRecursively);

void log_recent_files() {
    std::unique_lock lk(g_recent_lock, std::try_to_lock);
    if (!lk.owns_lock()) return;
    log("  last file opens (newest last; thread = host thread id):");
    for (uint64_t k = 0; k < kRecentOps; k++) {
        const RecentOp& r = g_recent[(g_recent_seq + k) % kRecentOps];
        if (r.seq) log("    #%llu thread %lld  %s", (unsigned long long)r.seq, (long long)r.thread, r.text);
    }
}

void add_virtual_file(const std::string& path, std::string data) {
    std::lock_guard lk(g_fs);
    g_virtual[virtual_key(fs::u8path(path))] = std::move(data);
}

std::string read_game_file(const std::string& relative_to_romfs) {
    FILE* f = plat::fopen_utf8(utf8(romfs_root() / fs::u8path(relative_to_romfs)), "rb");
    if (!f) return {};
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

}  // namespace nx
