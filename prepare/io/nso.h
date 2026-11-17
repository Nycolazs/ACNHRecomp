// The ExeFS's executables (NSO) as the game folder needs them: the header, the build id, LZ4 block decompression, the
// SHA-256 of each segment, and the flat image ACNHRecomp's loader reads as analysis/<module>.bin (runtime/src/loader.cpp,
// tools/nso2bin.py). An NSO of an extracted ExeFS is plain: nothing here decrypts.
//
// Format: "NSO0", u32 version, u32 reserved, u32 flags (bits 0-2: text/ro/data LZ4-compressed, bits 3-5: their SHA-256
// is to be checked); per segment at 0x10 + 0x10 * i {file offset, memory offset, decompressed size, (name offset,
// name size, bss size)}; the module id at 0x40 (32 bytes; the build id is its first 16); the file sizes of the
// segments at 0x60 (u32 each); the SHA-256 of each decompressed segment at 0xA0 (32 bytes each). Little-endian.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nx::prepare {

constexpr size_t kNsoHeaderSize = 0x100;

struct NsoSegment {
    uint32_t file_offset = 0;
    uint32_t mem_offset = 0;
    uint32_t size = 0;       // decompressed
    uint32_t file_size = 0;  // in the file (compressed, when compressed)
    bool compressed = false;
    bool hashed = false;     // the flags ask for its SHA-256 to be checked (every retail NSO does)
    uint8_t sha256[32] = {};
};

struct NsoHeader {
    uint32_t flags = 0;
    NsoSegment seg[3];  // text, ro, data
    uint32_t bss_size = 0;
    uint8_t module_id[32] = {};
    // The flat image's size: the end of the segment that ends last (the data segment in every NSO).
    uint64_t image_size() const;
};

enum class NsoError {
    None,
    NotNso,     // no "NSO0", or shorter than its header
    Truncated,  // a segment lies past the end of the file
    BadLayout,  // implausible sizes, or a plain segment whose file size is not its size
    Decompress, // a segment's LZ4 data does not give its size
    Hash,       // a segment's SHA-256 is not the one in the header
    Io,         // the file could not be read
};

struct NsoStatus {
    NsoError error = NsoError::None;
    int segment = -1;  // 0 text, 1 ro, 2 data; -1 when about the whole file
    std::string what;  // English, for logs
    bool ok() const { return error == NsoError::None; }
};

const char* nso_segment_name(int segment);  // "text", "ro", "data"

// The header of `data` (at least kNsoHeaderSize bytes); false when it is not an NSO.
bool parse_nso_header(const uint8_t* data, size_t size, NsoHeader& out);
// The build id: the module id's first 16 bytes in upper-case hex, as games/acnh/versions.txt lists them.
std::string build_id_hex(const NsoHeader& h);

// An LZ4 block (no frame) into exactly `cap` bytes: false when the data is malformed or gives another size.
bool lz4_decompress(const uint8_t* src, size_t n, uint8_t* dst, size_t cap);

// The flat image: text, ro and data at their memory offsets, zeros between them, bss not included; its size is
// NsoHeader::image_size(). Byte for byte what tools/nso2bin.py writes. Each segment's SHA-256 is checked when the
// flags ask for it.
NsoStatus nso_to_flat(const uint8_t* nso, size_t size, std::vector<uint8_t>& image, NsoHeader* header = nullptr);
inline NsoStatus nso_to_flat(const std::vector<uint8_t>& nso, std::vector<uint8_t>& image, NsoHeader* header = nullptr) {
    return nso_to_flat(nso.data(), nso.size(), image, header);
}

// The whole file (UTF-8 path). false when it cannot be read.
bool read_file(const std::string& path, std::vector<uint8_t>& out, uint64_t max_size = 1ull << 30);
// nso_to_flat of a file.
NsoStatus nso_file_to_flat(const std::string& path, std::vector<uint8_t>& image, NsoHeader* header = nullptr);

}  // namespace nx::prepare
