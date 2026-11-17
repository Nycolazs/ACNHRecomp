// romfs.pak: a RomFS folder in one file, read by the runtime instead of the loose files (runtime/src/hle_fs.cpp,
// "packed RomFS"). Byte for byte what tools/pack_romfs.py writes for the same folder.
//
// Format: "NXRP", u32 version 1, u32 count; per file u16 path length, the UTF-8 path ('/' separated, relative to the
// RomFS), u64 offset, u64 size; then the files' bytes, each at a 16-byte aligned offset, zeros between them and none
// after the last. Little-endian. The files are in the byte order of their paths (Python's sort of the same strings);
// folders are not listed (an empty one is not in the pack), and neither is what a link to a folder holds.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pak.h"

namespace nx::prepare {

struct RomfsPakEntry {
    std::string path;    // in the pack: relative, '/' separated, UTF-8
    std::string source;  // on the disk (UTF-8)
    uint64_t offset = 0, size = 0;
};

struct RomfsPakLayout {
    std::vector<RomfsPakEntry> files;
    uint64_t header_size = 0;  // magic, version, count and the index
    uint64_t size = 0;         // the whole pack (pack_romfs.py --size)
};

// The pack's index and size without writing it. `exclude_root`: names at the RomFS's top left out (a folder that
// holds the ExeFS's files beside the RomFS's, inputs.h Layout::Mixed); empty for pack_romfs.py's exact output.
// `skip_packages`: game packages (inputs.h is_game_package: files, and folders such as yuzu's NCA ones) left out
// wherever they are, never read; no RomFS of the game has one.
bool romfs_pak_layout(const std::string& romfs_dir, RomfsPakLayout& out, std::string& error,
                      const std::vector<std::string>& exclude_root = {}, bool skip_packages = false);

struct RomfsPakOptions : PakOptions {
    std::vector<std::string> exclude_root;
    bool skip_packages = false;
};

// Writes <out_path>.part, then renames it to out_path. The files are streamed, never held whole in memory.
PakResult write_romfs_pak(const std::string& romfs_dir, const std::string& out_path, const RomfsPakOptions& opt = {});

}  // namespace nx::prepare
