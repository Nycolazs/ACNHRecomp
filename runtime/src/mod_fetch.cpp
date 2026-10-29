// The two mods every player gets without doing anything (computers; config.h mods_fetch): the pt-BR fan translation
// (mods/ptbr) and the parameter files of tmouh's 60 FPS SpeedFix (mods/speedfix60, with the code the importer compiled
// into the game). They are made from the game's files, so they are not in this repository or its releases: the
// project hosts them on MediaFire, and the first start downloads them into the game folder.
//
// MediaFire's link is a page; the file's address in it changes, so it is read from the page each time. curl does the
// transfers (part of Windows 10+, macOS and nearly every Linux), tar (bsdtar: Windows, macOS) or unzip / python3
// (Linux) the extraction, into mods/.fetch first and then moved into place. A download that does not have the
// SHA-256 below is thrown away. A failed try waits a day before the next (mods/.fetch_failed); NX_MOD_FETCH=0 or
// game.mod_fetch = off: never.
#include "platform.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__ANDROID__)
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

#include <nx/runtime.h>

#include "config.h"

namespace nx {

#if !defined(__ANDROID__)
namespace {

namespace fs = std::filesystem;

std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct ModFile {
    const char* name;  // the folder in mods/ (and the zip's top folder)
    const char* page;  // MediaFire's page for the file
    const char* sha256;
};
const ModFile kMods[] = {
    {"ptbr", "https://www.mediafire.com/file/15cu4orbaozmwkk/ACNHRecomp-ptbr-mod.zip/file",
     "0d5cb40ef5beb5705ada5a9fff157be9346bcbc20491106a790679b581fa49f6"},
    {"speedfix60", "https://www.mediafire.com/file/v0orwholavilef2/ACNHRecomp-speedfix60-mod.zip/file",
     "524bcfc32e7b42af32bf86706ac294283d93cd86a46aa7bc7dbe077acaa16761"},
};

// SHA-256 (FIPS 180-4) of a file, as lowercase hex; empty if it cannot be read.
std::string sha256_file(const fs::path& p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto block = [&](const uint8_t* b) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(b[4 * i]) << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = bb, bb = a, a = t1 + t2;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    };
    FILE* f = plat::fopen_utf8(u8(p), "rb");
    if (!f) return "";
    std::vector<uint8_t> buf(1 << 16);
    uint64_t total = 0;
    size_t n, have = 0;  // bytes of a partial block at the start of buf
    while ((n = std::fread(buf.data() + have, 1, buf.size() - have, f)) > 0) {
        total += n, have += n;
        size_t k = 0;
        for (; k + 64 <= have; k += 64) block(buf.data() + k);
        std::memmove(buf.data(), buf.data() + k, have - k);
        have -= k;
    }
    std::fclose(f);
    uint8_t tail[128] = {};
    std::memcpy(tail, buf.data(), have);
    tail[have] = 0x80;
    size_t len = have < 56 ? 64 : 128;
    for (int i = 0; i < 8; i++) tail[len - 1 - i] = uint8_t((total * 8) >> (8 * i));
    block(tail);
    if (len == 128) block(tail + 64);
    std::string hex;
    char b[9];
    for (uint32_t v : h) std::snprintf(b, sizeof b, "%08x", v), hex += b;
    return hex;
}

// Runs a program (looked up in PATH) without a console window; its exit code, or -1 if it could not start.
int run(const std::vector<std::string>& argv) {
#if defined(_WIN32)
    std::wstring cmd;
    for (const std::string& a : argv) {
        int n = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, w.data(), n);
        if (!cmd.empty()) cmd += L' ';
        cmd += L'"' + w + L'"';  // no argument here holds a quote
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return int(code);
#else
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t child = 0;
    int rc = posix_spawnp(&child, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::string read_file(const fs::path& p) {
    std::string s;
    if (FILE* f = plat::fopen_utf8(u8(p), "rb")) {
        char b[65536];
        size_t n;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
        std::fclose(f);
    }
    return s;
}

// The file's address on MediaFire's page (https://downloadNNN.mediafire.com/...), or empty.
std::string direct_link(const std::string& page) {
    for (size_t at = page.find("https://download"); at != std::string::npos; at = page.find("https://download", at + 1)) {
        size_t end = page.find_first_of("\"' <>", at);
        std::string url = page.substr(at, end == std::string::npos ? std::string::npos : end - at);
        size_t host = url.find(".mediafire.com/");
        if (host != std::string::npos && host < 40) return url;
    }
    return "";
}

bool curl(const std::vector<std::string>& args) {
    std::vector<std::string> a = {"curl", "-sSfL", "--connect-timeout", "20", "--max-time", "300", "-A",
                                  "Mozilla/5.0 ACNHRecomp"};
    a.insert(a.end(), args.begin(), args.end());
    return run(a) == 0;
}

bool unzip(const fs::path& zip, const fs::path& dir) {
    const std::string z = u8(zip), d = u8(dir);
#if defined(__linux__)
    if (run({"unzip", "-q", "-o", z, "-d", d}) == 0) return true;
    if (run({"bsdtar", "-xf", z, "-C", d}) == 0) return true;
    return run({"python3", "-m", "zipfile", "-e", z, d}) == 0;
#else
    return run({"tar", "-xf", z, "-C", d}) == 0;  // bsdtar reads zip (Windows 10+, macOS)
#endif
}

bool fetch(const ModFile& m, const fs::path& mods) {
    std::error_code ec;
    const fs::path work = mods / ".fetch", page = work / "page.html", zip = work / (std::string(m.name) + ".zip");
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    bool ok = false;
    std::string why;
    if (!curl({"-o", u8(page), m.page})) {
        why = "the page did not download (no internet, or curl missing)";
    } else if (std::string url = direct_link(read_file(page)); url.empty()) {
        why = "no download link on the page";
    } else if (!curl({"-o", u8(zip), url})) {
        why = "the file did not download";
    } else if (std::string sum = sha256_file(zip); sum != m.sha256) {
        why = "the file is not the expected one (SHA-256 " + sum + ")";
    } else if (!unzip(zip, work) || !fs::is_directory(work / m.name / "romfs", ec)) {
        why = "the zip did not extract";
    } else {
        fs::remove_all(mods / m.name, ec);
        fs::rename(work / m.name, mods / m.name, ec);
        ok = !ec && fs::is_directory(mods / m.name / "romfs", ec);
        if (!ok) why = "it could not be moved into mods/";
    }
    fs::remove_all(work, ec);
    log("mods: %s %s%s%s", m.name, ok ? "downloaded and installed" : "not installed: ", ok ? "" : why.c_str(),
        ok ? "" : " (tried again tomorrow)");
    return ok;
}

}  // namespace

void mods_fetch() {
    if (const char* e = std::getenv("NX_MOD_FETCH"); e && e[0] == '0') return;
    if (config_get("game.mod_fetch", "on") == "off") return;
    std::error_code ec;
    const fs::path mods = fs::u8path(plat::data_root()) / "mods";
    std::vector<const ModFile*> missing;
    for (const ModFile& m : kMods)
        if (!fs::is_directory(mods / m.name / "romfs", ec)) missing.push_back(&m);
    if (missing.empty()) return;
    const fs::path failed = mods / ".fetch_failed";
    long last = std::atol(read_file(failed).c_str());
    const long now = long(std::time(nullptr));
    if (last > 0 && now - last < 24 * 3600) return;
    fs::create_directories(mods, ec);
    bool all = true;
    for (const ModFile* m : missing) all = fetch(*m, mods) && all;
    if (all) {
        fs::remove(failed, ec);
    } else if (FILE* f = plat::fopen_utf8(u8(failed), "wb")) {
        std::fprintf(f, "%ld\n", now);
        std::fclose(f);
    }
}
#else
void mods_fetch() {}  // Android: the folder is copied to the phone from a computer that has them
#endif

}  // namespace nx
