// A stand-in for the shader tool (tools/maxwell/spirv_harness) in the preparation's tests: it takes the arguments the
// Shaders stage gives the real one and writes what the real one leaves, with no game in it:
//   fake_shader_tool --prepare <romfs dir> <main.bin> --out <work dir> --spirv <dir> --jobs N --no-val
// writes <dir>/0123456789abcdef.spv and fedcba9876543210.spv (a SPIR-V header each), results.tsv and summary.txt.
// NX_FAKE_SHADER_SLEEP=<seconds>: takes that long, a line every 100 ms (the cancel test). A RomFS holding
// Shader/fake_fail: fails (exit 3).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

static int bad(const char* why) {
    std::fprintf(stderr, "fake_shader_tool: %s\n", why);
    return 2;
}

int main(int argc, char** argv) {
    if (argc != 11 || std::strcmp(argv[1], "--prepare") != 0 || std::strcmp(argv[4], "--out") != 0 ||
        std::strcmp(argv[6], "--spirv") != 0 || std::strcmp(argv[8], "--jobs") != 0 || std::atoi(argv[9]) < 1 ||
        std::strcmp(argv[10], "--no-val") != 0)
        return bad("usage: --prepare <romfs> <main.bin> --out <dir> --spirv <dir> --jobs N --no-val");
    const fs::path romfs = argv[2], main_bin = argv[3], work = argv[5], spirv = argv[7];
    std::error_code ec;
    if (!fs::is_directory(romfs / "Shader", ec)) return bad("the RomFS has no Shader folder");
    if (!fs::is_regular_file(main_bin, ec) || fs::file_size(main_bin, ec) == 0) return bad("no main.bin");
    if (fs::exists(romfs / "Shader" / "fake_fail", ec)) {
        std::puts("Shader/fake_fail: failing as asked");
        return 3;
    }
    fs::create_directories(work, ec);
    fs::create_directories(spirv, ec);
    std::printf("extracted 2 shaders from %s\n", romfs.string().c_str());
    std::fflush(stdout);
    if (const char* s = std::getenv("NX_FAKE_SHADER_SLEEP")) {
        const int tenths = std::atoi(s) * 10;
        for (int k = 0; k < tenths; k++) {
            std::printf("translating %d/%d\n", k, tenths);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    for (const char* key : {"0123456789abcdef", "fedcba9876543210"}) {
        std::ofstream f(spirv / (std::string(key) + ".spv"), std::ios::binary);
        const unsigned char header[20] = {0x03, 0x02, 0x23, 0x07, 0, 0, 1, 0, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0};
        f.write(reinterpret_cast<const char*>(header), sizeof header);
        f << key;
    }
    std::ofstream(spirv / "results.tsv") << "0123456789abcdef\tok\nfedcba9876543210\tok\n";
    std::ofstream(spirv / "summary.txt") << "2 shaders, 2 translated, 0 failed (fake_shader_tool)\n";
    std::puts("translated 2/2");
    return 0;
}
