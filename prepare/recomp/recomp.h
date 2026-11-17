// Recompile an NSO module to C++ (ACNHRecomp's `python -m recomp`, ported): writes into out_dir
//   funcs.h           NXB (the module's load address) and the declarations of every recompiled function
//   table.cpp         nx::ModuleInfo: guest offset -> function, relocations, symbols, init arrays, segments
//   code_NNN.cpp      the functions, in translation units of about INSNS_PER_FILE instructions
//   code_variant.cpp  with a variant patch: the functions only the patched code has
//   report.txt        coverage: unhandled instructions, unresolved indirect branches
// Files whose text is unchanged are left alone (and keep their time stamps), so a rebuild compiles only what changed.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rc {

// The paths are UTF-8 on every platform (prepare.cpp's; acnh_recomp reads its command line so on Windows).
struct RecompOptions {
    std::string nso;        // the module (exefs/main, sdk, subsdk0)
    std::string out_dir;
    std::string name = "main";
    uint64_t base = 0x4000000000ull;
    unsigned jobs = 0;      // 0: the cores
    std::string hooks;      // file of instruction addresses that call nx_hook first ("" none)
    std::string hooks_text; // or the same list as text
    std::string variant;    // a .pchtxt patch: a second body per changed function ("" none)
    std::vector<uint64_t> only;  // debug: only these functions
    std::atomic<bool>* cancel = nullptr;
    std::function<void(const std::string&)> log;                  // the lines `python -m recomp` prints
    std::function<void(uint64_t done, uint64_t total)> progress;  // functions emitted
};

struct RecompResult {
    size_t functions = 0, files = 0;
    uint64_t handled = 0, unhandled = 0;
};

// Throws std::runtime_error on a bad module or patch.
RecompResult recompile(const RecompOptions& opt);

}  // namespace rc
