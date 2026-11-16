// acnh_prepare: the game folder prepared from a terminal (prepare.h), the stages' progress printed as they go.
//   acnh_prepare --exefs DIR --romfs DIR [--dlc DIR[:index]]... --out DIR [options]
//   acnh_prepare GAME [--out DIR] ...   (GAME: one folder holding exefs/ and romfs/, or an emulator's dump folder)
// Exit code 0 when the folder is ready; 2 refused (the files, the folder, the space, the tools), 1 a stage failed,
// 4 cancelled (Ctrl+C), 64 a wrong command line.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include "args_utf8.h"
#include "gamelib.h"
#include "io_util.h"
#include "prepare.h"

namespace fs = std::filesystem;
using nx::prepare::detail::str;
using nx::prepare::detail::u8;

namespace {

const char* kUsage =
    "usage: acnh_prepare --exefs DIR --romfs DIR [--dlc DIR[:index]]... --out DIR [options]\n"
    "       acnh_prepare GAME [--out DIR] [options]\n"
    "\n"
    "Prepares the game folder ACNHRecomp plays, from the ExeFS and RomFS you extracted from your own copy of Animal\n"
    "Crossing: New Horizons with update 3.0.3 (Ryujinx: right-click the game, Extract Data; yuzu or Eden: Dump RomFS,\n"
    "and Dump ExeFS in the debug settings). No keys, and no NSP, XCI or NCA file is ever read.\n"
    "\n"
    "  GAME                one folder holding exefs/ and romfs/ (or an emulator's dump folder); two folders: the\n"
    "                      ExeFS and the RomFS, in either order\n"
    "  --exefs DIR         the extracted ExeFS (main, sdk, subsdk0, main.npdm)\n"
    "  --romfs DIR         the extracted RomFS (Shader/, Bcsv/, ...)\n"
    "  --dlc DIR[:index]   add-on content's RomFS (Happy Home Paradise: index 1, the default when its folder's name\n"
    "                      does not say); repeatable\n"
    "  --out DIR           the game folder (an empty folder, or one prepared before)\n"
    "  --platforms LIST    this (default), all, or a list of linux-x64, windows-x64, macos-arm64, android-arm64\n"
    "  --no-60fps          main without the native 60 FPS variant (tmouh's SpeedFix)\n"
    "  --in-place          the RomFS stays loose where it is, as the game folder's romfs/ (no romfs.pak, no copy):\n"
    "                      --out is then the folder holding it (the default)\n"
    "  --zig PATH          the zig executable (default: tools/zig/zig beside acnh_prepare)\n"
    "  --zig-lib DIR       zig's lib folder, when it is not beside the executable\n"
    "  --shader-tool PATH  spirv_harness (default: tools/shaders/spirv_harness beside acnh_prepare)\n"
    "  --cache DIR         zig's cache (default: <out>/.import/zig-cache, removed when done)\n"
    "  --jobs N            compile jobs (default: the cores, as many as the memory holds)\n"
    "\n"
    "Prepared again on the same folder, what is already right is kept (another platform builds that one only).\n";

std::atomic<bool> g_interrupted{false};
extern "C" void on_signal(int) { g_interrupted = true; }

// The folder acnh_prepare is in (UTF-8), "" when unknown.
std::string own_dir() {
#if defined(_WIN32)
    static wchar_t w[32768];
    DWORD n = GetModuleFileNameW(nullptr, w, 32768);
    if (n == 0 || n >= 32768) return "";
    return str(fs::path(std::wstring(w, n)).parent_path());
#elif defined(__APPLE__)
    char b[4096];
    uint32_t size = sizeof b;
    if (_NSGetExecutablePath(b, &size) != 0) return "";
    std::error_code ec;
    return str(fs::weakly_canonical(fs::path(b), ec).parent_path());
#else
    std::error_code ec;
    return str(fs::read_symlink("/proc/self/exe", ec).parent_path());
#endif
}

// The first of `names` (relative to the executable's folder) that is a file, else "".
std::string beside(const std::vector<std::string>& names) {
    const std::string dir = own_dir();
    if (dir.empty()) return "";
    std::error_code ec;
    for (const auto& n : names) {
        const fs::path p = u8(dir) / u8(n);
        if (fs::is_regular_file(p, ec)) return str(p);
    }
    return "";
}

[[noreturn]] void usage_error(const std::string& what) {
    std::fprintf(stderr, "acnh_prepare: %s\n\n%s", what.c_str(), kUsage);
    std::exit(64);
}

std::string percent(double f) {
    char b[16];
    std::snprintf(b, sizeof b, "%3d%%", int(f * 100 + 0.5));
    return b;
}

}  // namespace

int main(int argc_, char** argv_) {
    // UTF-8, the preparation's paths (Windows' argv is in the ANSI code page)
    const std::vector<std::string> argv = nx::prepare::detail::utf8_args(argc_, argv_);
    const int argc = int(argv.size());
    prep::Options o;
    std::string platforms = "this";
    std::vector<std::string> exefs, romfs;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage_error(a + " needs a value");
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            std::fputs(kUsage, stdout);
            return 0;
        } else if (a == "--exefs") {
            exefs.push_back(value());
        } else if (a == "--romfs") {
            romfs.push_back(value());
        } else if (a == "--dlc") {
            // DIR or DIR:index (the last ':' followed by digits only: a Windows drive's "C:\" is no index)
            std::string v = value();
            int index = -1;
            const size_t c = v.rfind(':');
            if (c != std::string::npos && c + 1 < v.size() && v.find_first_not_of("0123456789", c + 1) == std::string::npos) {
                index = std::atoi(v.c_str() + c + 1);
                v.resize(c);
                if (index < 1 || index > 0xFFF) usage_error("an add-on's index is 1 to 4095: " + std::to_string(index));
            }
            o.dlc.push_back(v);
            o.dlc_index.push_back(index);
        } else if (a == "--out") {
            o.out = value();
        } else if (a == "--platforms") {
            platforms = value();
        } else if (a == "--no-60fps") {
            o.variant_60fps = false;
        } else if (a == "--in-place") {
            o.in_place = true;
        } else if (a == "--zig") {
            o.zig = value();
        } else if (a == "--zig-lib") {
            o.zig_lib = value();
        } else if (a == "--shader-tool") {
            o.shader_tool = value();
        } else if (a == "--cache") {
            o.cache = value();
        } else if (a == "--jobs") {
            const int n = std::atoi(value().c_str());
            if (n < 1 || n > 1024) usage_error("--jobs takes 1 to 1024");
            o.jobs = unsigned(n);
        } else if (!a.empty() && a[0] == '-') {
            usage_error("unknown option " + a);
        } else {
            o.inputs.push_back(a);
        }
    }
    if (exefs.size() > 1 || romfs.size() > 1) usage_error("one --exefs and one --romfs");
    if (!exefs.empty() || !romfs.empty()) {
        if (!o.inputs.empty()) usage_error("give either GAME or --exefs and --romfs");
        if (exefs.empty() || romfs.empty()) usage_error("--exefs and --romfs go together");
        o.inputs = {exefs[0], romfs[0]};
    }
    if (o.inputs.empty()) usage_error("no game files given");
    if (o.inputs.size() > 2) usage_error("one folder (holding exefs/ and romfs/) or two (the ExeFS, the RomFS)");
    if (o.out.empty() && !o.in_place) usage_error("--out DIR: where the game folder goes (or --in-place)");
    std::string bad;
    if (!prep::parse_platforms(platforms, o.platforms, bad)) usage_error("no such platform: " + bad);
#ifdef _WIN32
    const char* zig_exe = "tools/zig/zig.exe";
    const char* tool_exe = "tools/shaders/spirv_harness.exe";
#else
    const char* zig_exe = "tools/zig/zig";
    const char* tool_exe = "tools/shaders/spirv_harness";
#endif
    // tools/zig/zig, zig/zig; in ACNHRecomp.app (acnh_prepare in Contents/MacOS) Contents/Resources/tools/zig/zig
    if (o.zig.empty()) o.zig = beside({zig_exe, std::string(zig_exe).substr(6), std::string("../Resources/") + zig_exe});
    if (o.shader_tool.empty())
        o.shader_tool = beside({tool_exe, std::string(tool_exe).substr(14), std::string("../Resources/") + tool_exe});

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    if (!prep::start(o)) {
        std::fprintf(stderr, "acnh_prepare: a preparation is running already\n");
        return 1;
    }
    const bool tty = isatty(fileno(stdout));
    prep::Stage shown = prep::Stage::Idle;
    int last_pct = -1;
    std::string last_item;
    bool line_open = false;  // a progress line written with \r, not ended yet
    auto end_line = [&] {
        if (line_open) std::fputs("\n", stdout), line_open = false;
    };
    prep::Status st;
    for (bool cancel_sent = false;;) {
        if (g_interrupted && !cancel_sent) {
            prep::cancel();
            cancel_sent = true;
            end_line();
            std::puts("cancelling...");
        }
        st = prep::status();
        for (const prep::LogLine& l : st.log) {
            end_line();
            if (!l.text.empty()) {
                std::printf("  %s\n", l.text.c_str());
            } else if (l.stage != shown) {  // a stage begins
                std::printf("[%s]\n", prep::stage_name(l.stage));
                shown = l.stage, last_pct = -1;
            }
        }
        const int pct = int(st.fraction * 100);
        if (!st.finished && (pct != last_pct || st.item != last_item)) {
            const std::string text = percent(st.fraction) + "  " + st.item.substr(0, 100);
            if (tty) {
                std::printf("\r\033[K%s", text.c_str());
                line_open = true;
            } else if (pct / 10 != last_pct / 10) {
                std::printf("%s\n", text.c_str());
            }
            last_pct = pct, last_item = st.item;
        }
        std::fflush(stdout);
        if (st.finished) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(tty ? 100 : 250));
    }
    prep::wait();
    end_line();
    if (st.rc == 0) {
        std::printf("ready: %s (abi %s)\n", st.out.c_str(), gl::abi());
        return 0;
    }
    std::fprintf(stderr, "acnh_prepare: %s\n", st.rc == 4 ? "cancelled" : st.error.c_str());
    return st.rc;
}
