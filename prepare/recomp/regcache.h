// recomp/regcache.py: keeps a function's guest registers (general, vector, NZCV) in C++ locals instead of the CPU
// context, synced around everything that can see the context (calls, syscalls, setjmp, traps, returns).
#pragma once

#include <string>
#include <vector>

namespace rc {

// lines: the function's source lines (the header at header_index); returns the rewritten lines
std::vector<std::string> regcache_apply(std::vector<std::string> lines, size_t header_index);

}  // namespace rc
