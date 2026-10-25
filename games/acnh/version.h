#pragma once
// Which update of main the build was made from. Two are supported, 3.0.2 and 3.0.3 (docs/ACNH.md, "Updates"): 3.0.3
// moved most of the code between about main+280c000 and main+3e02000 by 0x10 and left the rest, and the data, where
// they were. Code offsets the runtime uses therefore come in pairs.
#include <cstdint>

namespace nx::acnh {

bool is_303();  // main is 3.0.3 (else 3.0.2); valid once the modules are loaded
inline uint64_t ofs(uint64_t v302, uint64_t v303) { return is_303() ? v303 : v302; }

}  // namespace nx::acnh
