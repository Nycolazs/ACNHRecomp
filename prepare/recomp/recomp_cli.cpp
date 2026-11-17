// acnh_recomp: the recompiler from the command line, with python -m recomp's arguments (for comparing the two).
//   acnh_recomp <module.nso> <out dir> --name main --base 0x4000000000 [--jobs N] [--hooks <file>] [--variant <file.pchtxt>]
//               [--only 0xADDR ...]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "args_utf8.h"  // prepare/io/
#include "recomp.h"

int main(int argc_, char** argv_) {
    // UTF-8, as RecompOptions takes its paths (Windows' argv is in the ANSI code page)
    const std::vector<std::string> argv = nx::prepare::detail::utf8_args(argc_, argv_);
    const int argc = int(argv.size());
    rc::RecompOptions o;
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
            return argv[++i];
        };
        if (a == "--name") o.name = val();
        else if (a == "--base") o.base = std::strtoull(val().c_str(), nullptr, 0);
        else if (a == "--jobs") o.jobs = unsigned(std::atoi(val().c_str()));
        else if (a == "--hooks") o.hooks = val();
        else if (a == "--variant") o.variant = val();
        else if (a == "--only") {
            while (i + 1 < argc && argv[i + 1][0] != '-') o.only.push_back(std::strtoull(argv[++i].c_str(), nullptr, 16));
        } else if (pos == 0) o.nso = a, pos++;
        else if (pos == 1) o.out_dir = a, pos++;
        else {
            std::fprintf(stderr, "unexpected argument %s\n", a.c_str());
            return 2;
        }
    }
    if (pos != 2) {
        std::fprintf(stderr, "usage: acnh_recomp <module.nso> <out dir> --name main --base 0x4000000000 [--jobs N] [--hooks f] [--variant f.pchtxt]\n");
        return 2;
    }
    o.log = [](const std::string& s) { std::printf("%s\n", s.c_str()), std::fflush(stdout); };
    try {
        rc::recompile(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
