// The C++ recompiler (prepare/recomp/) is a port of recomp/*.py and must give the same code. SOURCE records the
// version of the Python it was ported from, as tools/setup_acnh.py's recomp_hash() computes it: sha256 over
// "name\0content\0" of recomp/*.py in name order. A change to the Python without the same change here fails this test.
//   recomp_source_test <repository root> <SOURCE file>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "sha256.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: recomp_source_test <repository root> <SOURCE file>\n");
        return 2;
    }
    const fs::path dir = fs::path(argv[1]) / "recomp";
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 3 && n.compare(n.size() - 3, 3, ".py") == 0) names.push_back(n);
    }
    if (ec || names.empty()) {
        std::fprintf(stderr, "FAIL no recomp/*.py in %s\n", dir.string().c_str());
        return 1;
    }
    std::sort(names.begin(), names.end());  // byte order, as Python's sorted() on these (ASCII) names
    nx::Sha256 h;
    for (const std::string& n : names) {
        std::ifstream f(dir / n, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!f.good() && !f.eof()) {
            std::fprintf(stderr, "FAIL cannot read %s\n", (dir / n).string().c_str());
            return 1;
        }
        h.update(n.data(), n.size());
        h.update("", 1);
        h.update(text.data(), text.size());
        h.update("", 1);
    }
    const std::string now = h.hex();

    std::ifstream sf(argv[2], std::ios::binary);
    std::string recorded;
    sf >> recorded;
    if (now != recorded) {
        std::printf("FAIL recomp/*.py is %s, prepare/recomp/SOURCE says %s: port the change of the Python recompiler "
                    "to prepare/recomp/ (the same generated code), then write the new hash into SOURCE\n",
                    now.c_str(), recorded.empty() ? "nothing" : recorded.c_str());
        return 1;
    }
    std::printf("ok   recomp/*.py (%zu files) is %s, as prepare/recomp/SOURCE\n", names.size(), now.c_str());
    return 0;
}
