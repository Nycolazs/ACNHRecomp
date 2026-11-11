// The save's link to an online account, cleared (save_unlink.h).
//
// ACNH's save files are pairs: <name>Header.dat (0x300 bytes) and <name>.dat, AES-128-CTR encrypted with a key and
// counter derived from the header's 128 words at 0x100 (sead's xorshift128 seeded from them). The decrypted data is
// split into regions, each led by the Murmur3-32 hash of the bytes after it; the game rejects a file whose hashes do
// not match. Layout facts for 3.0.x (file sizes, hash regions) are the public ones the save editors use.
//
// The link itself, as found in saves linked under three accounts (Nextendo, Rustendo, none): personal.dat holds a
// 0x58-byte record at 0x66690 {u64; char[32] account string; zeros; u64 network service account id @+0x50}, all zero
// when the resident was never linked; main.dat keeps a copy of each resident's record in its last region. Unlinking
// zeroes those records, rehashes and re-encrypts with the same header.
#include "save_unlink.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include <mbedtls/aes.h>

#include <nx/runtime.h>

#include "platform.h"
#include "save_storage.h"
#include "saves.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace nx::save_unlink {

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;
struct Region {
    uint32_t at, size;  // hash at `at`, hashed bytes [at + 4, at + 4 + size)
};

constexpr uint32_t kMajor = 0xA0002, kMinor = 0xA0028;  // 3.0.0 to 3.0.3
constexpr size_t kMainSize = 0x9B0E90, kPersonalSize = 0x74A40, kHeaderSize = 0x300;
constexpr Region kMain[] = {{0x000110, 0x1e339c}, {0x1e34b0, 0x3d089c}, {0x5b3e60, 0x037acc}, {0x5eb930, 0x03ce5c},
                            {0x6288a0, 0x037acc}, {0x660370, 0x03ce5c}, {0x69d2e0, 0x037acc}, {0x6d4db0, 0x03ce5c},
                            {0x711d20, 0x037acc}, {0x7497f0, 0x03ce5c}, {0x786760, 0x037acc}, {0x7be230, 0x03ce5c},
                            {0x7fb1a0, 0x037acc}, {0x832c70, 0x03ce5c}, {0x86fbe0, 0x037acc}, {0x8a76b0, 0x03ce5c},
                            {0x8e4620, 0x037acc}, {0x91c0f0, 0x03ce5c}, {0x958f50, 0x057f3c}};
constexpr Region kPersonal[] = {{0x00110, 0x37acc}, {0x37be0, 0x3ce5c}};
constexpr size_t kRecord = 0x66690, kRecordSize = 0x58, kSignature = 0x28;  // signature: the u64 and the string

std::mutex g_mu;
std::string g_last;  // under g_mu

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24); }

uint32_t murmur3(const uint8_t* p, size_t n) {  // 32-bit, seed 0; n is a multiple of 4 for every region
    uint32_t h = 0;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t k = rd32(p + i) * 0xcc9e2d51u;
        k = (k << 15 | k >> 17) * 0x1b873593u;
        h ^= k;
        h = (h << 13 | h >> 19) * 5 + 0xe6546b64u;
    }
    h ^= uint32_t(n);
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

// sead::Random (xorshift128), seeded the way the game seeds it.
struct Random {
    uint32_t s[4];
    explicit Random(uint32_t seed) {
        uint32_t v = seed;
        for (uint32_t i = 0; i < 4; i++) s[i] = v = 0x6C078965u * (v ^ (v >> 30)) + i + 1;
    }
    uint32_t next() {
        uint32_t t = s[0];
        s[0] = s[1], s[1] = s[2], s[2] = s[3];
        t ^= t << 11;
        t ^= t >> 8;
        return s[3] = t ^ s[3] ^ (s[3] >> 19);
    }
};

// The key (index 0) or the counter (index 2) from the header's words.
std::array<uint8_t, 16> param(const uint8_t* words, uint32_t index) {
    auto w = [&](uint32_t i) { return rd32(words + 4 * i); };
    Random r(w(w(index) & 0x7F));
    const uint32_t rolls = ((w(w(index + 1) & 0x7F) & 0x7F) & 0xF) + 1;
    for (uint32_t i = 0; i < rolls * 2; i++) r.next();  // each roll draws 64 bits
    std::array<uint8_t, 16> out{};
    for (auto& b : out) b = uint8_t(r.next() >> 24);
    return out;
}

// AES-128-CTR in place (the same call encrypts and decrypts).
bool crypt(const Bytes& header, Bytes& data) {
    const uint8_t* words = header.data() + 0x100;
    std::array<uint8_t, 16> key = param(words, 0), ctr = param(words, 2), stream{};
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    size_t off = 0;
    bool ok = mbedtls_aes_setkey_enc(&aes, key.data(), 128) == 0 &&
              mbedtls_aes_crypt_ctr(&aes, data.size(), &off, ctr.data(), stream.data(), data.data(), data.data()) == 0;
    mbedtls_aes_free(&aes);
    return ok;
}

bool read_file(const fs::path& p, Bytes& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool write_file(const fs::path& p, const Bytes& data) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size())) || !f.flush()) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

template <size_t N>
bool hashes_ok(const Bytes& d, const Region (&regions)[N]) {
    for (const Region& r : regions)
        if (rd32(d.data() + r.at) != murmur3(d.data() + r.at + 4, r.size)) return false;
    return true;
}

template <size_t N>
void rehash(Bytes& d, const Region (&regions)[N]) {
    for (const Region& r : regions) wr32(d.data() + r.at, murmur3(d.data() + r.at + 4, r.size));
}

struct Pair {
    fs::path header_path, data_path;
    Bytes header, data;
};

// Read and decrypt one pair; "" when it is a 3.0.x file with every hash right, else why not.
template <size_t N>
std::string open_pair(Pair& p, const fs::path& dir, const char* name, size_t size, const Region (&regions)[N]) {
    p.header_path = dir / (std::string(name) + "Header.dat");
    p.data_path = dir / (std::string(name) + ".dat");
    if (!read_file(p.header_path, p.header) || !read_file(p.data_path, p.data)) return std::string(name) + ": cannot read";
    if (p.header.size() != kHeaderSize || p.data.size() != size || rd32(p.header.data()) != kMajor ||
        rd32(p.header.data() + 4) != kMinor)
        return std::string(name) + ": not a 3.0.x save";
    if (!crypt(p.header, p.data)) return std::string(name) + ": cannot decrypt";
    if (!hashes_ok(p.data, regions)) return std::string(name) + ": damaged or not recognised";
    return "";
}

// The save as it was, before any change: <user>/backups/save-link-fix/<island> <date time>. Kept apart from the
// timestamped backups (saves.cpp lists and prunes only those), so it is never pruned. Returns its path, "" on failure.
std::string backup_unaltered(const fs::path& dir, std::string& error) {
    std::string island = save_storage::island_display_name(dir);
    for (char& ch : island)
        if (static_cast<unsigned char>(ch) < 0x20 || std::strchr("<>:\"/\\|?*", ch)) ch = '_';
    while (!island.empty() && (island.back() == ' ' || island.back() == '.')) island.pop_back();
    if (island.empty()) island = "Unnamed island";
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char when[32];
    std::strftime(when, sizeof when, "%Y-%m-%d %H-%M-%S", &tm);
    const fs::path root = save_storage::user_dir(plat::data_root()) / "backups" / "save-link-fix";
    fs::path dst = root / fs::u8path(island + " " + when);
    std::error_code ec;
    for (int n = 2; fs::exists(dst, ec) && n < 100; n++) dst = root / fs::u8path(island + " " + when + " (" + std::to_string(n) + ")");
    fs::path tmp = dst;
    tmp += ".tmp";
    fs::create_directories(root, ec);
    fs::remove_all(tmp, ec);
    if (!save_storage::copy_tree(dir, tmp, &error)) return fs::remove_all(tmp, ec), std::string();
    fs::rename(tmp, dst, ec);
    if (ec) return fs::remove_all(tmp, ec), error = ec.message(), std::string();
    return save_storage::path_utf8(dst);
}

bool all_zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}

}  // namespace

bool unlink(const std::string& device_save, std::string& message) {
    const fs::path dir = fs::u8path(device_save);
    Pair main;
    if (std::string why = open_pair(main, dir, "main", kMainSize, kMain); !why.empty()) return message = why, false;

    std::vector<Pair> residents;
    for (int v = 0; v < 8; v++) {
        const fs::path vd = dir / ("Villager" + std::to_string(v));
        std::error_code ec;
        if (!fs::is_regular_file(vd / "personal.dat", ec)) continue;
        Pair p;
        if (std::string why = open_pair(p, vd, "personal", kPersonalSize, kPersonal); !why.empty())
            return message = "Villager" + std::to_string(v) + "/" + why, false;
        residents.push_back(std::move(p));
    }
    if (residents.empty()) return message = "no residents in this save", false;

    // Zero each linked resident's record, and its copy in main.dat's last region.
    int unlinked = 0, copies = 0;
    std::vector<bool> changed(residents.size(), false);
    const Region& last = kMain[std::size(kMain) - 1];
    uint8_t* const lo = main.data.data() + last.at + 4;
    uint8_t* const hi = lo + last.size;
    for (size_t k = 0; k < residents.size(); k++) {
        uint8_t* rec = residents[k].data.data() + kRecord;
        if (all_zero(rec, kRecordSize)) continue;
        if (!all_zero(rec, kSignature)) {
            for (uint8_t* at = lo; at + kRecordSize <= hi; at++) {
                at = std::search(at, hi - kRecordSize + kSignature, rec, rec + kSignature);
                if (at + kRecordSize > hi) break;
                std::memset(at, 0, kRecordSize);
                copies++;
            }
        }
        std::memset(rec, 0, kRecordSize);
        changed[k] = true;
        unlinked++;
    }
    if (!unlinked) return message = "This save is not linked to an online account: nothing to fix.", true;

    std::string err;
    const std::string backup = backup_unaltered(dir, err);
    if (backup.empty()) return message = "backup failed, nothing changed: " + err, false;
    log("save_unlink: unaltered save kept in %s", backup.c_str());

    // Rehash and encrypt again under the same headers (same key and counter): only the data files change.
    auto store = [](Pair& p, bool personal) {
        if (personal)
            rehash(p.data, kPersonal);
        else
            rehash(p.data, kMain);
        return crypt(p.header, p.data) && write_file(p.data_path, p.data);
    };
    for (size_t k = 0; k < residents.size(); k++)
        if (changed[k] && !store(residents[k], true)) return message = "could not write a resident's file", false;
    if (copies && !store(main, false)) return message = "could not write main.dat", false;
    message = "Unlinked " + std::to_string(unlinked) + (unlinked == 1 ? " resident" : " residents") +
              " from the old online account. The unaltered save is in backups/save-link-fix/" +
              save_storage::path_utf8(fs::u8path(backup).filename()) + ".";
    return true;
}

bool request_restart() {
#if defined(__ANDROID__)
    return false;
#elif defined(_WIN32)
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (!n || n >= std::size(exe)) return false;
    SetEnvironmentVariableW(L"NX_UNLINK_SAVE", L"1");
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // Inherit handles: a log redirected to a file keeps going to it.
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        SetEnvironmentVariableW(L"NX_UNLINK_SAVE", nullptr);
        return false;
    }
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    log("save_unlink: restarting to fix the save");
    plat::exit_process(0);
#else
    char exe[4096] = {};
#if defined(__APPLE__)
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0) return false;
#else
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return false;
#endif
    setenv("NX_UNLINK_SAVE", "1", 1);
    log("save_unlink: restarting to fix the save");
    char* argv[] = {exe, nullptr};
    execv(exe, argv);
    unsetenv("NX_UNLINK_SAVE");
    return false;
#endif
}

void run_if_requested() {
    const char* e = std::getenv("NX_UNLINK_SAVE");
    if (!e || std::strcmp(e, "1") != 0) return;
#if defined(_WIN32)
    SetEnvironmentVariableW(L"NX_UNLINK_SAVE", nullptr);
    _putenv_s("NX_UNLINK_SAVE", "");
#else
    unsetenv("NX_UNLINK_SAVE");
#endif
    std::string msg;
    const bool ok = unlink(saves::save_dir(), msg);
    log("save_unlink: %s%s", ok ? "" : "not fixed: ", msg.c_str());
    std::lock_guard lk(g_mu);
    g_last = ok ? msg : "Not fixed: " + msg;
}

std::string last_result() {
    std::lock_guard lk(g_mu);
    return g_last;
}

}  // namespace nx::save_unlink
