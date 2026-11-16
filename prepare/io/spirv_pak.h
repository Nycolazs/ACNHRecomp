// spirv.pak: a spirv folder's .spv files in one file, read by the runtime instead of the folder
// (runtime/src/nvn/nvn_shaders.cpp, spirv_pak()). Byte for byte what tools/pack_spirv.py writes for the same folder.
//
// Format: "SPK1" (u32 0x314B5053) and the count (u32); per file 16 bytes of key (the name without .spv, zero padded),
// u64 offset, u64 size; then the files, one after the other with no padding. Little-endian. The .spv files whose key
// is at most 16 characters, in the byte order of their names, then the folder's results.tsv when it has one, under
// the key "results.tsv" (without it the storage buffers of translated shaders cannot be bound).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pak.h"

namespace nx::prepare {

struct SpirvPakEntry {
    std::string key;     // in the pack (at most 16 bytes)
    std::string source;  // on the disk (UTF-8)
    uint64_t offset = 0, size = 0;
};

struct SpirvPakLayout {
    std::vector<SpirvPakEntry> files;
    uint64_t size = 0;
    bool results_tsv = false;
};

bool spirv_pak_layout(const std::string& spirv_dir, SpirvPakLayout& out, std::string& error);

// Writes <out_path>.part, then renames it to out_path. The files are streamed, never held whole in memory.
PakResult write_spirv_pak(const std::string& spirv_dir, const std::string& out_path, const PakOptions& opt = {});

}  // namespace nx::prepare
