// gamelib.h: the game's library from gen/, compiled and linked with zig as child processes, the biggest files first so
// the longest ones do not end the build alone; funcs.h (every module's declarations, several MB) precompiled once per
// module. Plain C++ and the OS's process calls (posix_spawn, CreateProcessW): it runs on Android as on the desktop.
#include "gamelib.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace gl {

namespace {
// A UTF-8 string as a path (fs::u8path is deprecated in C++20), and a path as UTF-8 with '/' separators: every path
// given to a child process, opened or said goes through these, never path::string() (on Windows, the ANSI code page:
// a folder named "ção" would come out as other bytes, which run_once reads back as UTF-8).
fs::path u8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string u8s(const fs::path& p) {
    auto u = p.generic_u8string();  // std::u8string in C++20
    return std::string(u.begin(), u.end());
}
}  // namespace

namespace {

// ACNHRecomp's runtime headers (runtime/include/nx/cpu.h, module.h, runtime.h), runtime/src/game_abi.cpp and
// cpu_model.c, built in (gamelib_data.inc, made by prepare/prepare.cmake)
struct Embedded {
    const char* path;
    const char* text;
};
const Embedded kRuntime[] = {
#include "gamelib_data.inc"
};

// The functions the game spends its time in, per game (main's build id): "<module> <offset in hex>" lines, from
// ACNHRecomp's CPU profiles of the game played (the Importer's tools/make_hot_list.py; prepare/gamelib/hot/<build
// id>.txt, built in as gamelib_hot.inc by CMake; none so far). Only how fast the code is depends on it, never what it
// does.
struct HotList {
    const char* build_id;
    const char* text;
};
const HotList kHotLists[] = {
#include "gamelib_hot.inc"
    {nullptr, nullptr}};

// module (index in kModules) << 40 | offset
using HotSet = std::unordered_set<uint64_t>;
const char* const kModules[] = {"main", "sdk", "subsdk0"};

int module_index(const std::string& m) {
    for (int i = 0; i < 3; i++)
        if (m == kModules[i]) return i;
    return -1;
}

// One argument of a response file: quoted, forward slashes (Windows takes them; a backslash is an escape there).
std::string rsp_quote(std::string a) {
#ifdef _WIN32
    for (char& ch : a)
        if (ch == '\\') ch = '/';
#endif
    std::string q = "\"";
    for (char ch : a) {
        if (ch == '"' || ch == '\\') q += '\\';
        q += ch;
    }
    return q + "\"";
}

std::string slurp_tail(const std::string& path, size_t max) {
    std::ifstream f(u8p(path), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    return s.size() > max ? s.substr(s.size() - max) : s;
}

// The hot list of a game (its build id, any case) into hot; its id, or "" when there is none.
std::string load_hot(std::string build_id, HotSet& hot) {
    for (char& ch : build_id) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    for (const HotList* l = kHotLists; l->build_id; l++) {
        if (build_id != l->build_id) continue;
        std::istringstream in(l->text);
        std::string m, off;
        while (in >> m >> off) {
            int i = module_index(m);
            if (i >= 0) hot.insert(uint64_t(i) << 40 | std::stoull(off, nullptr, 16));
        }
        return hot.empty() ? "" : l->build_id;
    }
    return "";
}

// gen/<module>/code_*.cpp regrouped into gen/split/<module>/hot_<n>.cpp (the hot list's functions) and cold_<n>.cpp
// (the rest), each about the size of the recompiler's files. The recompiled code is only function definitions, each
// from its "void <module>_<offset>(nx::CPU& NX_RESTRICT c) {" line to the next one, all over funcs.h's declarations,
// so any grouping compiles to the same functions. Done once for every target: "ok" holds the hot list it was made for.
int split(const fs::path& gen, const std::string& m, const HotSet& hot, const std::string& id, std::string& error) {
    std::error_code ec;
    fs::path dir = gen / "split" / m;
    {
        std::ifstream ok(dir / "ok");
        std::string have;
        if (std::getline(ok, have) && have == id) return 0;
    }
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) return error = "cannot write " + u8s(dir) + ": " + ec.message(), 1;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(gen / m, ec))
        if (e.path().extension() == ".cpp" && e.path().filename() != "table.cpp") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    const uint64_t mi = uint64_t(module_index(m)) << 40;
    const std::string head = "void " + m + "_";
    constexpr size_t kPiece = 1400000;
    std::string out[2];  // cold, hot
    int count[2] = {0, 0};
    auto flush = [&](int k) {
        if (out[k].empty()) return true;
        fs::path f = dir / ((k ? "hot_" : "cold_") + std::to_string(count[k]++) + ".cpp");
        std::ofstream w(f, std::ios::binary);
        w << "#include \"funcs.h\"\n\n" << out[k];
        out[k].clear();
        return bool(w.flush());
    };
    for (const fs::path& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in.good() && !in.eof()) return error = "cannot read " + u8s(f), 1;
        int cur = -1;  // before the first function: the #include
        size_t pos = 0;
        while (pos < text.size()) {
            size_t eol = text.find('\n', pos);
            size_t next = eol == std::string::npos ? text.size() : eol + 1;
            if (text.compare(pos, head.size(), head) == 0) {
                uint64_t off = std::strtoull(text.c_str() + pos + head.size(), nullptr, 16);
                cur = hot.count(mi | off) ? 1 : 0;
                if (out[cur].size() >= kPiece && !flush(cur)) return error = "cannot write " + u8s(dir), 1;
            }
            if (cur >= 0) out[cur].append(text, pos, next - pos);
            pos = next;
        }
    }
    if (!flush(0) || !flush(1)) return error = "cannot write " + u8s(dir), 1;
    std::ofstream(dir / "ok") << id << "\n";
    return 0;
}

}  // namespace

namespace {

// Variables that change where a compiler or linker looks (other toolchains installed on the computer set them): the
// toolchain the preparation carries uses its own headers and libraries only.
bool foreign_env(const std::string& kv) {
    static const char* const kDrop[] = {"CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
                                        "OBJCPLUS_INCLUDE_PATH", "LIBRARY_PATH", "INCLUDE", "LIB", "LIBPATH", "CL",
                                        "_CL_", "LINK", "_LINK_", "CCC_OVERRIDE_OPTIONS", "NIX_CFLAGS_COMPILE",
                                        "NIX_LDFLAGS", "ZIG_LIB_DIR", "ZIG_LIBC", "ZIG_GLOBAL_CACHE_DIR",
                                        "ZIG_LOCAL_CACHE_DIR", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET"};
    size_t eq = kv.find('=');
    if (eq == 0 || eq == std::string::npos) return false;  // Windows' "=C:=C:\..." entries
    std::string name = kv.substr(0, eq);
    for (const char* d : kDrop) {
#ifdef _WIN32
        if (_stricmp(name.c_str(), d) == 0) return true;
#else
        if (name == d) return true;
#endif
    }
    return false;
}

void note_failure(const std::string& log, const std::string& what) {
    std::ofstream(u8p(log), std::ios::binary | std::ios::app) << what << "\n";
}

}  // namespace

namespace {

// cancel: polled while the child runs; once set, the child is stopped with every process it started (zig runs a
// `zig clang` per object, which outlived a signal to zig alone): a Job Object on Windows (TerminateJobObject), the
// child's own process group elsewhere (SIGTERM, then SIGKILL, to the group).
int run_once(const std::vector<std::string>& argv, const std::string& log, const std::vector<std::string>& env_extra,
             const std::atomic<bool>* cancel) {
#ifdef _WIN32
    auto widen = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    };
    auto narrow = [](const std::wstring& w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    };
    std::wstring cmd;
    for (const auto& a : argv) {  // CommandLineToArgvW's quoting
        std::wstring w = widen(a);
        cmd += L'"';
        size_t bs = 0;
        for (wchar_t ch : w) {
            if (ch == L'\\') {
                bs++;
                continue;
            }
            if (ch == L'"') cmd.append(bs * 2 + 1, L'\\');
            else cmd.append(bs, L'\\');
            bs = 0;
            cmd += ch;
        }
        cmd.append(bs * 2, L'\\');
        cmd += L"\" ";
    }
    // The child's environment: this process's without the foreign toolchain variables, plus env_extra (built here:
    // the jobs run on several threads, so the process's own environment is never changed).
    std::wstring envb;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* e = block; *e; e += wcslen(e) + 1) {
            std::wstring w(e);
            std::string kv = narrow(w);
            bool replaced = false;
            for (const auto& x : env_extra)
                if (_strnicmp(kv.c_str(), x.c_str(), x.find('=') + 1) == 0) replaced = true;
            if (!replaced && !foreign_env(kv)) envb += w + L'\0';
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& e : env_extra) envb += widen(e) + L'\0';
    envb += L'\0';
    if (cmd.size() >= 32767) {
        note_failure(log, "command line too long (" + std::to_string(cmd.size()) + " characters): " + argv[0]);
        return -1;
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(widen(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    // The child and everything it starts in a job of their own, assigned before the child runs (CREATE_SUSPENDED):
    // TerminateJobObject ends them all, and closing the job's handle (not inherited) ends what is left of it.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li)) CloseHandle(job), job = nullptr;
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (job ? CREATE_SUSPENDED : 0), envb.data(),
                             nullptr, &si, &pi);
    DWORD err = ok ? 0 : GetLastError();
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) {
        if (job) CloseHandle(job);
        note_failure(log, "cannot start " + argv[0] + " (Windows error " + std::to_string(err) + ")");
        return -1;
    }
    if (job) {
        // not assigned (a job of the caller's that takes no nested one, before Windows 8): the child alone is stopped
        if (!AssignProcessToJobObject(job, pi.hProcess)) CloseHandle(job), job = nullptr;
        ResumeThread(pi.hThread);
    }
    if (!cancel) {
        WaitForSingleObject(pi.hProcess, INFINITE);
    } else {
        while (WaitForSingleObject(pi.hProcess, 50) == WAIT_TIMEOUT)
            if (cancel->load()) {
                if (job) TerminateJobObject(job, 1);
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, INFINITE);
                break;
            }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (job) CloseHandle(job);  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: a process the child left running ends with it
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return int(code);
#else
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    std::vector<std::string> envs;
    for (char** e = environ; *e; e++) {
        std::string kv = *e;
        bool replaced = false;
        for (const auto& x : env_extra)
            if (kv.compare(0, x.find('=') + 1, x, 0, x.find('=') + 1) == 0) replaced = true;
        if (!replaced && !foreign_env(kv)) envs.push_back(kv);
    }
    for (const auto& e : env_extra) envs.push_back(e);
    std::vector<char*> envp;
    for (auto& e : envs) envp.push_back(e.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // stdin from /dev/null: in a process group of its own the child is not the terminal's foreground (a read from the
    // terminal would stop it)
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    // With `cancel`: the child leads a process group of its own (Android 9 and up too), which every process it starts
    // joins, so a cancel stops them all.
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    if (cancel) {
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&at, 0);
    }
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0].c_str(), &fa, &at, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (rc != 0) {
        note_failure(log, "cannot start " + argv[0] + ": " + std::strerror(rc));
        return -1;
    }
    int st = 0;
    if (cancel) {
        // Also from here: where posix_spawn returns before the child ran (a fork, bionic), the group exists at once.
        // Once the child has run exec, this fails (EACCES) and changes nothing.
        setpgid(pid, pid);
        // The group, else the child alone (no group: setpgid not run yet). A process group's id is not given to a new
        // process while the group has a member, so -pid names only these, also once the child itself is reaped.
        auto stop = [pid](int sig) {
            if (kill(-pid, sig) != 0) kill(pid, sig);
        };
        int signalled = 0;  // 0 not yet; then SIGTERM, then SIGKILL two seconds later
        auto since = std::chrono::steady_clock::now();
        for (;;) {
            pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid || (w < 0 && errno != EINTR)) break;
            if (cancel->load()) {
                if (!signalled) stop(SIGTERM), signalled = SIGTERM, since = std::chrono::steady_clock::now();
                else if (signalled == SIGTERM && std::chrono::steady_clock::now() - since > std::chrono::seconds(2))
                    stop(SIGKILL), signalled = SIGKILL;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));  // a compile step ends within 10 ms of its child
        }
        if (signalled) kill(-pid, SIGKILL);  // a process of the group that outlived the child (ESRCH: none left)
    } else {
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
    }
    // A step ended by a signal leaves no word of its own (a phone's low-memory killer: SIGKILL): said in its log
    if (WIFSIGNALED(st) && !(cancel && cancel->load()))
        note_failure(log, argv[0] + " stopped by signal " + std::to_string(WTERMSIG(st)) +
                              (WTERMSIG(st) == SIGKILL ? " (killed: out of memory?)" : ""));
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
#endif
}

std::string read_log(const std::string& log) {
    std::ifstream f(u8p(log), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (char& ch : text)
        if (ch == '\\') ch = '/';
    return text;
}

// The toolchain's own header or source "not found" (Windows: an antivirus scanning or holding the files the Importer
// had just unpacked; players saw corecrt.h, vadefs.h, crtdll.c missing at 55% and 99% though the package has them).
bool toolchain_file_missing(const std::string& log) {
    std::string text = read_log(log);
    auto has = [&](const char* a) { return text.find(a) != std::string::npos; };
    return has("zig/lib/") && (has("file not found") || has("FileNotFound"));
}

}  // namespace

std::string toolchain_lib_dir(const std::string& zig, const std::string& zig_lib) {
    fs::path lib = zig_lib.empty() ? u8p(zig).parent_path() / "lib" : u8p(zig_lib);
    auto u = lib.lexically_normal().generic_u8string();  // std::u8string in C++20
    std::string s(u.begin(), u.end());
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// A step that failed in the toolchain's own files: one of them not found, or an error reported in one. Those headers
// compile for every target (CI builds the game's code with them on each system), so an error located in one means the
// file is not what the package holds: an antivirus or a cleaner that emptied or removed a libc header gives errors
// rather than "not found" (players: "zig/lib/libcxx/include/math.h:393:31: error: use of undeclared identifier
// 'FP_NAN'", "reference to unresolved using declaration" for mbstate_t in <cwchar>, errors in crtdefs.h).
// A diagnostic line names the file it is in, so "In file included from" and note lines do not count, and an error in
// the game's code that only mentions a toolchain header in a note is not taken for one.
bool toolchain_broken(const std::string& log, const std::string& lib_dir) {
    if (toolchain_file_missing(log)) return true;
    std::string text = read_log(log), lib = lib_dir + "/";
#ifdef _WIN32
    auto fold = [](std::string v) {
        for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return v;
    };
    text = fold(text), lib = fold(lib);
#endif
    std::istringstream in(text);
    for (std::string ln; std::getline(in, ln);) {
        bool in_toolchain = (!lib_dir.empty() && ln.find(lib) != std::string::npos) || ln.find("zig/lib/") != std::string::npos;
        if (!in_toolchain || ln.rfind("In file included from", 0) == 0 || ln.rfind("in file included from", 0) == 0) continue;
        if (ln.find("error:") != std::string::npos || ln.find("file not found") != std::string::npos || ln.find("FileNotFound") != std::string::npos ||
            ln.find("filenotfound") != std::string::npos)
            return true;
    }
    return false;
}

// sizes.txt: "<size>\t<path>" lines under a "#" comment, paths relative to the zig folder ("lib/std/std.zig", "zig").
std::string check_toolchain(const std::string& zig, const std::string& zig_lib) {
    std::error_code ec;
    const fs::path exe = u8p(zig), lib = u8p(toolchain_lib_dir(zig, zig_lib));
    if (zig.empty() || !fs::is_regular_file(exe, ec)) return "the zig executable is missing (" + zig + ")";
    if (fs::file_size(exe, ec) < (1u << 20)) return "the zig executable " + zig + " is damaged (" + std::to_string(fs::file_size(exe, ec)) + " bytes)";
    if (!fs::is_directory(lib, ec)) return "zig's lib folder is missing (" + u8s(lib) + ")";
    std::ifstream sizes(lib.parent_path() / "sizes.txt");
    if (!sizes) sizes.open(lib / "sizes.txt");
    size_t bad = 0, listed = 0;
    std::string first;
    auto wrong = [&](const std::string& what) {
        if (bad++ < 3) first += (first.empty() ? "" : ", ") + what;
    };
    if (!sizes) {  // no list: what every target needs
        for (const char* f : {"std/std.zig", "compiler_rt.zig", "libcxx/include/cmath", "libcxx/include/__config",
                              "include/stddef.h"})
            if (!fs::is_regular_file(lib / f, ec) || fs::file_size(lib / f, ec) == 0) wrong("lib/" + std::string(f));
    }
    for (std::string ln; sizes && std::getline(sizes, ln);) {
        if (ln.empty() || ln[0] == '#') continue;
        const size_t tab = ln.find('\t');
        if (tab == std::string::npos) continue;
        const uint64_t want = std::strtoull(ln.c_str(), nullptr, 10);
        std::string rel = ln.substr(tab + 1);
        if (!rel.empty() && rel.back() == '\r') rel.pop_back();
        fs::path p;
        if (rel.rfind("lib/", 0) == 0) p = lib / u8p(rel.substr(4));
        else if (rel == u8s(exe.filename())) p = exe;  // Android's libzig.so is not listed: not checked
        else continue;                                       // LICENSE, README.md
        listed++;
        const uint64_t have = fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : UINT64_MAX;
        if (have != want) wrong(rel + (have == UINT64_MAX ? " missing" : " changed"));
    }
    if (!bad) return "";
    return "the toolchain (" + u8s(lib.parent_path()) + ") is damaged: " + std::to_string(bad) +
           (listed ? " of its " + std::to_string(listed) + " files" : " files") + " missing or changed (" + first +
           "). An antivirus or a cleaner may have removed or emptied them: add an exclusion for that folder and put "
           "the toolchain back (tools/fetch_zig.py, or ACNHRecomp's package again)";
}

// A step that failed only because a toolchain file could not be opened is run again, a little later, a few times.
int run(const std::vector<std::string>& argv, const std::string& log, const std::vector<std::string>& env_extra,
        const std::atomic<bool>* cancel) {
    int rc = run_once(argv, log, env_extra, cancel);
    for (int attempt = 1; rc != 0 && attempt <= 3 && toolchain_file_missing(log); attempt++) {
        std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
        if (cancel && cancel->load()) break;
        rc = run_once(argv, log, env_extra, cancel);
    }
    return rc;
}

namespace {

unsigned default_jobs() {
    unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    // the biggest generated files take ~860 MB to compile at -O2 (ACNH main): as many jobs as the memory holds
    uint64_t avail_mb = 0;
#ifdef _WIN32
    MEMORYSTATUSEX ms{sizeof ms};
    // at least half the memory: Windows counts little of it as available once the game's 10 GB went through its file
    // cache and other programs are open (a player's 8-core PC got 1 job), and the biggest files are few
    if (GlobalMemoryStatusEx(&ms)) avail_mb = std::max(ms.ullAvailPhys, ms.ullTotalPhys / 2) >> 20;
#elif defined(__APPLE__)
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    avail_mb = uint64_t(pages) * uint64_t(page) / 2 >> 20;
#else
    std::ifstream f("/proc/meminfo");
    std::string k;
    uint64_t v;
    std::string unit;
    while (f >> k >> v >> unit)
        if (k == "MemAvailable:") avail_mb = v / 1024;
#endif
    if (!avail_mb) return cores;
    uint64_t by_mem = avail_mb > 1000 ? (avail_mb - 1000) / 900 : 1;
    return unsigned(std::max<uint64_t>(1, std::min<uint64_t>(cores, by_mem)));
}

// The compiler flags of an optimisation level (empty: no such level).
//   fast: clang's -O1 (zig turns a plain -O1 into -O2: cc1 takes the last one), instructions selected by FastISel,
//         no machine scheduler and no vectorizers: about a third less compile time than -O2 for this code (the
//         recompiled functions are big, straight and full of memory accesses through the guest's registers), with
//         code as compact.
//   -O2:  what ACNHRecomp's own build uses.  -O0: debugging only (about 8 times the code).
std::vector<std::string> opt_flags(const std::string& opt) {
    if (opt == "fast")
        return {"-O2", "-Xclang", "-O1", "-mllvm", "-fast-isel=true", "-mllvm", "-enable-misched=false",
                "-fno-slp-vectorize", "-fno-vectorize"};
    if (opt == "-O2") return {"-O2"};
    // GlobalISel's Localizer takes minutes on big functions; zig turns on UBSan at -O0, whose handlers no player has
    if (opt == "-O0") return {"-O0", "-fno-global-isel", "-fno-sanitize=all"};
    return {};
}

}  // namespace

const char* abi() { return GL_GAME_ABI; }

const char* const kToolchainFault = "the toolchain files the preparation carries";

// The revision written beside each library (rev.txt), and the first one that made each platform's right (the
// Importer's numbering, kept: a library it built at revision 4 against this runtime is as good as one built here):
//   2: Android's library lists the libraries it needs (before, the player could not load it: "cannot locate symbol")
//   3: main with the native 60 FPS variant (3.0.2, 3.0.3) and ACNHRecomp 0.7's hooks (amiibo camp gate, the camera
//      temporal AA reprojects with on 3.0.3): every platform's library is built again
//   4: the recompiler keeps every jump table target when functions overlap a br (ACNHRecomp 8d0f374; the probable
//      crash shooting a balloon at 60 FPS, ACNHRecomp #58): every platform's library is built again
constexpr int kRevision = 4;
static int first_good_revision(const std::string&) { return 4; }

bool library_current(const std::string& out, const std::string& target, const std::string& build_id) {
    const Target* t = find_target(target);
    if (!t) return false;
    std::error_code ec;
    fs::path dir = u8p(out) / "game" / t->name;
    if (!fs::exists(dir / t->lib_name, ec)) return false;
    int rev = 1;  // libraries from before rev.txt
    std::ifstream(dir / "rev.txt") >> rev;
    if (rev < first_good_revision(t->name)) return false;
    // built against other runtime headers: this ACNHRecomp's player would refuse it (game_folder.cpp)
    std::string built_abi;
    std::ifstream(dir / "abi.txt") >> built_abi;
    if (built_abi != GL_GAME_ABI) return false;
    return build_id.empty() || library_build_id(out, target) == build_id;
}

std::string library_build_id(const std::string& out, const std::string& target) {
    const Target* t = find_target(target);
    std::error_code ec;
    if (!t || !fs::exists(u8p(out) / "game" / t->name / t->lib_name, ec)) return "";
    std::string id;
    std::ifstream(u8p(out) / "game" / t->name / "build.txt") >> id;
    return id;
}

const std::vector<Target>& targets() {
    // The flags of ACNHRecomp's CMakeLists.txt for each platform's game_<module> libraries
    static const std::vector<Target> t = {
        {"linux-x64", "x86_64-linux-gnu.2.28", "libacnh_game.so",
         {"-mcpu=x86_64_v3", "-fPIC", "-fvisibility=hidden", "-ffp-contract=off"},
         {"-shared", "-nostdlib++", "-Wl,--allow-shlib-undefined"},
         {}},
        {"android-arm64", "aarch64-linux-musl", "libacnh_game.so",
         {"-mcpu=cortex_a76", "-fPIC", "-fvisibility=hidden", "-fno-semantic-interposition"},
         // --no-as-needed: the stubs define nothing, so the default --as-needed dropped every DT_NEEDED entry and
         // Android's linker found neither libc++_shared.so's symbols nor the runtime's ("cannot locate symbol")
         {"-shared", "-nostdlib", "-Wl,--allow-shlib-undefined", "-Wl,-z,max-page-size=16384", "-Wl,--no-as-needed"},
         {"libacnh.so", "libc++_shared.so", "libm.so", "libc.so", "libdl.so"}},
        // Windows: a DLL cannot leave the runtime's calls to the loader; game_abi.cpp defines them through the table
        // the player hands over (nx_game_bind). The player is MSVC-built, so the DLL keeps its own C++ runtime.
        {"windows-x64", "x86_64-windows-gnu", "acnh_game.dll",
         {"-mcpu=x86_64_v3", "-ffp-contract=off"},
         {"-shared"},
         {}},
        {"macos-arm64", "aarch64-macos.11.0", "libacnh_game.dylib",
         {"-mcpu=apple_m1", "-fPIC", "-fvisibility=hidden"},
         {"-shared", "-nostdlib++", "-Wl,-undefined,dynamic_lookup"},
         {}},
    };
    return t;
}

const Target* find_target(const std::string& name) {
    for (const Target& t : targets())
        if (name == t.name) return &t;
    return nullptr;
}

std::string host_target() {
#if defined(__ANDROID__) && defined(__aarch64__)
    return "android-arm64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-x64";
#else
    return "";
#endif
}

int build(const Options& o, std::string& error) {
    const Target* t = find_target(o.target);
    if (!t) return error = "no such target: " + o.target, 1;
    auto log = [&](const std::string& s) {
        if (o.log) o.log(s);
    };
    auto cancelled = [&] { return o.cancel && o.cancel->load(); };
    std::error_code ec;
    fs::path work = u8p(o.work), gen = u8p(o.gen);
    fs::create_directories(work / "runtime" / "nx", ec);
    for (const Embedded& e : kRuntime) std::ofstream(work / "runtime" / u8p(e.path), std::ios::binary) << e.text;
    std::vector<std::string> env = {"ZIG_GLOBAL_CACHE_DIR=" + o.cache, "ZIG_LOCAL_CACHE_DIR=" + o.cache};
    if (!o.zig_lib.empty()) env.push_back("ZIG_LIB_DIR=" + o.zig_lib);

    // Each step through run() (which runs one again when a toolchain file could not be opened). One that still fails
    // in the toolchain's own files (toolchain_broken) has it checked against its sizes.txt (check_toolchain), said in
    // the log, and the build's error names the toolchain: nothing here puts its files back.
    const std::string lib_dir = toolchain_lib_dir(o.zig, o.zig_lib);
    std::mutex broken_m;
    std::atomic<bool> still_broken{false};
    auto step = [&](const std::vector<std::string>& argv, const std::string& lg) {
        int rc = run(argv, lg, env, o.cancel);
        if (rc == 0 || cancelled() || !toolchain_broken(lg, lib_dir)) return rc;
        std::lock_guard<std::mutex> l(broken_m);
        if (!still_broken.exchange(true)) {
            std::string why = check_toolchain(o.zig, o.zig_lib);
            log("the toolchain's own files gave an error (" + lib_dir + "): " + (why.empty() ? "its files have their sizes" : why));
        }
        return rc;
    };
    // the error of a step: what the log says, after a word on the toolchain when its own files are at fault
    auto failed = [&](const std::string& what) {
        if (!still_broken) return what;
        // kToolchainFault is matched: prepare.cpp (no second cycle without the 60 FPS variant)
        return std::string(kToolchainFault) + " (" + lib_dir + ") are damaged or blocked: an antivirus or a cleaner " +
               "is probably removing or changing them. Add an exclusion for that folder, put the toolchain back " +
               "(tools/fetch_zig.py, or ACNHRecomp's package again), then prepare the game again with the same files " +
               "and folder.\n" + what;
    };

    std::vector<std::string> base = {o.zig, "c++", "-target", t->triple, "-std=c++20", "-g0", "-w",
                                     "-I" + u8s(work / "runtime")};
    base.insert(base.end(), t->cflags.begin(), t->cflags.end());
    if (opt_flags(o.opt).empty()) return error = "no such optimisation level: " + o.opt, 1;

    // With a hot list for this game (hot/<build id>.txt): its functions at o.opt, the rest at "fast", regrouped into
    // files of each kind once for all targets (split()). Without one: the recompiler's files, all at o.opt.
    HotSet hot;
    const std::string hot_id = o.split ? load_hot(o.build_id, hot) : "";
    struct Src {
        fs::path file;
        std::string module, opt;
        uintmax_t size;
    };
    std::vector<Src> srcs;
    std::vector<std::string> modules;
    for (const char* m : {"main", "sdk", "subsdk0"}) {
        if (!fs::exists(gen / m / "funcs.h", ec)) continue;
        modules.push_back(m);
        if (!hot_id.empty()) {
            if (cancelled()) return error = "cancelled", 4;
            if (int rc = split(gen, m, hot, hot_id, error)) return rc;
            for (const auto& e : fs::directory_iterator(gen / "split" / m, ec))
                if (e.path().extension() == ".cpp")
                    srcs.push_back({e.path(), m, u8s(e.path().filename()).rfind("hot_", 0) == 0 ? o.opt : "fast",
                                    e.file_size(ec)});
            srcs.push_back({gen / m / "table.cpp", m, o.opt, fs::file_size(gen / m / "table.cpp", ec)});
        } else {
            for (const auto& e : fs::directory_iterator(gen / m, ec))
                if (e.path().extension() == ".cpp") srcs.push_back({e.path(), m, o.opt, e.file_size(ec)});
        }
    }
    if (srcs.empty()) return error = "no recompiled code in " + o.gen, 1;
    // the longest first (-O2 takes about 1.5 times "fast"'s time for the same size)
    auto cost = [](const Src& s) { return double(s.size) * (s.opt == "fast" ? 1.0 : 1.5); };
    std::sort(srcs.begin(), srcs.end(), [&](const Src& a, const Src& b) { return cost(a) > cost(b); });

    // funcs.h precompiled per module and level (a PCH holds its optimisation level)
    std::set<std::pair<std::string, std::string>> pchs;
    for (const Src& s : srcs) pchs.insert({s.module, s.opt});
    auto pch_of = [&](const std::string& m, const std::string& opt) { return u8s(work / m / ("funcs" + opt + ".pch")); };
    for (const auto& [m, opt] : pchs) {
        if (cancelled()) return error = "cancelled", 4;
        fs::create_directories(work / m, ec);
        std::vector<std::string> a = base, lv = opt_flags(opt);
        a.insert(a.end(), lv.begin(), lv.end());
        // -fno-pch-timestamp: zig's cache hands back a PCH made by an earlier run when the headers' contents match,
        // but clang then refused it for the headers' times, rewritten since ("has been modified since the precompiled
        // header was built"), as soon as one file had to be compiled again
        a.insert(a.end(), {"-Xclang", "-fno-pch-timestamp", "-I" + u8s(gen / m), "-x", "c++-header",
                           u8s(gen / m / "funcs.h"), "-o", pch_of(m, opt)});
        std::string lg = u8s(work / m / "pch.log");
        if (step(a, lg) != 0) return error = failed("precompiling " + m + "/funcs.h failed:\n" + slurp_tail(lg, 2000)), 1;
    }

    unsigned jobs = o.jobs ? o.jobs : default_jobs();
    log("compiling " + std::to_string(srcs.size()) + " files for " + t->name + " (" +
        (hot_id.empty() ? o.opt : o.opt + " for the " + std::to_string(hot.size()) + " functions of " + hot_id + ", fast for the rest") +
        ", " + std::to_string(jobs) + " jobs)");
    std::atomic<size_t> next{0}, done{0};
    std::mutex mx;
    std::string first_error;
    std::vector<std::string> objects(srcs.size());
    auto worker = [&] {
        for (;;) {
            if (cancelled()) return;
            size_t k = next++;
            if (k >= srcs.size()) return;
            {
                std::lock_guard<std::mutex> l(mx);
                if (!first_error.empty()) return;
            }
            const Src& s = srcs[k];
            fs::path obj = work / s.module / (u8s(s.file.stem()) + ".o");
            objects[k] = u8s(obj);
            std::vector<std::string> a = base, lv = opt_flags(s.opt);
            a.insert(a.end(), lv.begin(), lv.end());
            a.push_back("-I" + u8s(gen / s.module));
            if (s.file.filename() == "table.cpp") a.push_back("-fvisibility=default");  // nx_module_<name>, looked up by the player
            else a.insert(a.end(), {"-include-pch", pch_of(s.module, s.opt)});
            a.insert(a.end(), {"-c", u8s(s.file), "-o", u8s(obj)});
            std::string lg = u8s(obj) + ".log";
            if (step(a, lg) != 0) {
                std::lock_guard<std::mutex> l(mx);
                if (first_error.empty()) first_error = s.module + "/" + u8s(s.file.filename()) + ":\n" + slurp_tail(lg, 2000);
                return;
            }
            fs::remove(u8p(lg), ec);
            size_t d = ++done;
            if (o.progress) o.progress(d, srcs.size() + 1, s.module + "/" + u8s(s.file.filename()));
        }
    };
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < jobs; k++) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    if (cancelled()) return error = "cancelled", 4;
    if (!first_error.empty()) return error = failed("compiling " + first_error), 1;

    // game_abi.cpp: the ABI string and the longjmp the player throws through
    std::vector<std::string> a = base;
    a.insert(a.end(), {"-O2", std::string("-DNX_GAME_ABI=\"") + GL_GAME_ABI + "\"", "-c", u8s(work / "runtime" / "game_abi.cpp"), "-o",
                       u8s(work / "game_abi.o")});
    if (step(a, u8s(work / "abi.log")) != 0) return error = failed("game_abi.cpp: " + slurp_tail(u8s(work / "abi.log"), 2000)), 1;

    // __cpu_model for __builtin_cpu_supports (x86: no libgcc / compiler-rt is linked in)
    bool x86 = std::string(t->triple).rfind("x86_64", 0) == 0;
    if (x86) {
        std::vector<std::string> c = {o.zig, "cc", "-target", t->triple, "-O2", "-c", u8s(work / "runtime" / "cpu_model.c"),
                                      "-o", u8s(work / "cpu_model.o")};
        if (std::string(t->name) != "windows-x64") c.insert(c.begin() + 4, "-fPIC");
        if (step(c, u8s(work / "cpu.log")) != 0) return error = failed("cpu_model.c: " + slurp_tail(u8s(work / "cpu.log"), 2000)), 1;
    }

    // stubs of the libraries the player's process provides, for their DT_NEEDED entries (Android's linker resolves a
    // library's symbols through its own needed libraries)
    std::vector<std::string> link = {o.zig, "c++", "-target", t->triple};
    link.insert(link.end(), t->ldflags.begin(), t->ldflags.end());
    for (const auto& n : t->needed) {
        fs::path stub = work / "stubs" / n;
        fs::create_directories(stub.parent_path(), ec);
        std::ofstream(work / "stubs" / "empty.c") << "\n";
        std::vector<std::string> sa = {o.zig, "cc", "-target", t->triple, "-shared", "-nostdlib", "-Wl,-soname," + n,
                                       u8s(work / "stubs" / "empty.c"), "-o", u8s(stub)};
        if (step(sa, u8s(work / "stubs" / "stub.log")) != 0)
            return error = failed("stub " + n + ": " + slurp_tail(u8s(work / "stubs" / "stub.log"), 2000)), 1;
        link.push_back(u8s(stub));
    }
    if (std::string(t->name) == "windows-x64") {  // what the player looks up (GetProcAddress)
        std::ofstream def(work / "exports.def");
        def << "EXPORTS\n    nx_game_abi\n    nx_game_longjmp\n    nx_game_bind\n";
        for (const auto& m : modules) def << "    nx_module_" << m << "\n";
        link.push_back(u8s(work / "exports.def"));
    }
    fs::path dir = u8p(o.out) / "game" / t->name;
    fs::create_directories(dir, ec);
    fs::path tmp = dir / (std::string(t->lib_name) + ".part");
    link.insert(link.end(), {"-o", u8s(tmp), u8s(work / "game_abi.o")});
    if (x86) link.push_back(u8s(work / "cpu_model.o"));
    for (const auto& obj : objects) link.push_back(obj);
    // Everything after "zig c++" in a response file: 1600 objects are far more than Windows' 32767-character command
    // line (it then refuses to start the linker).
    fs::path rsp = work / "link.rsp";
    {
        std::ofstream r(rsp, std::ios::binary);
        for (size_t k = 2; k < link.size(); k++) r << rsp_quote(link[k]) << "\n";
        if (!r.flush()) return error = "cannot write " + u8s(rsp), 1;
    }
    log("linking " + u8s(dir / t->lib_name));
    if (step({link[0], link[1], "@" + u8s(rsp)}, u8s(work / "link.log")) != 0)
        return error = failed("link: " + slurp_tail(u8s(work / "link.log"), 3000)), 1;
    fs::rename(tmp, dir / t->lib_name, ec);
    if (ec) return error = "cannot write " + u8s(dir / t->lib_name) + ": " + ec.message(), 1;
    std::ofstream(dir / "abi.txt") << GL_GAME_ABI << "\n";
    std::ofstream(dir / "rev.txt") << kRevision << "\n";
    std::ofstream(dir / "build.txt") << o.build_id << "\n";  // main's build id: a re-import of another update rebuilds it
    // lld-link's by-products beside the DLL (its .pdb, an import library): the player needs neither
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".pdb" || e.path().extension() == ".lib") fs::remove(e.path(), ec);
    if (o.progress) o.progress(srcs.size() + 1, srcs.size() + 1, t->lib_name);
    fs::remove_all(work, ec);
    return 0;
}

}  // namespace gl
