// NSO reading (nso.h). lz4_decompress and nso_to_flat come from ACNHRecomp-Importer's src/nximport.cpp (MIT, same
// author), without anything of its containers or keys.
#include "nso.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "io_util.h"
#include "sha256.h"

namespace nx::prepare {

using detail::rd32;

namespace {

NsoStatus fail(NsoError e, int segment, const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return {e, segment, b};
}

}  // namespace

const char* nso_segment_name(int segment) {
    static const char* names[] = {"text", "ro", "data"};
    return segment >= 0 && segment < 3 ? names[segment] : "file";
}

uint64_t NsoHeader::image_size() const {
    uint64_t end = 0;
    for (const NsoSegment& s : seg) end = std::max<uint64_t>(end, uint64_t(s.mem_offset) + s.size);
    return end;
}

bool parse_nso_header(const uint8_t* data, size_t size, NsoHeader& out) {
    if (size < kNsoHeaderSize || std::memcmp(data, "NSO0", 4) != 0) return false;
    out.flags = rd32(data + 0xC);
    for (int i = 0; i < 3; i++) {
        const uint8_t* s = data + 0x10 + 0x10 * i;
        NsoSegment& g = out.seg[i];
        g.file_offset = rd32(s);
        g.mem_offset = rd32(s + 4);
        g.size = rd32(s + 8);
        g.file_size = rd32(data + 0x60 + 4 * i);
        g.compressed = (out.flags >> i) & 1;
        g.hashed = (out.flags >> (3 + i)) & 1;
        std::memcpy(g.sha256, data + 0xA0 + 0x20 * i, 32);
    }
    out.bss_size = rd32(data + 0x3C);
    std::memcpy(out.module_id, data + 0x40, 32);
    return true;
}

std::string build_id_hex(const NsoHeader& h) {
    static const char* x = "0123456789ABCDEF";
    std::string s;
    for (int k = 0; k < 16; k++) s += x[h.module_id[k] >> 4], s += x[h.module_id[k] & 15];
    return s;
}

bool lz4_decompress(const uint8_t* src, size_t n, uint8_t* dst, size_t cap) {
    const uint8_t *ip = src, *iend = src + n;
    uint8_t *op = dst, *oend = dst + cap;
    while (ip < iend) {
        uint8_t token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (size_t(iend - ip) < lit || size_t(oend - op) < lit) return false;
        std::memcpy(op, ip, lit);
        op += lit, ip += lit;
        if (ip >= iend) break;  // the last sequence has only literals
        if (iend - ip < 2) return false;
        size_t offset = size_t(ip[0] | ip[1] << 8);
        ip += 2;
        if (offset == 0 || offset > size_t(op - dst)) return false;
        size_t ml = token & 15;
        if (ml == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                ml += b;
            } while (b == 255);
        }
        ml += 4;
        if (size_t(oend - op) < ml) return false;
        const uint8_t* m = op - offset;
        for (size_t k = 0; k < ml; k++) op[k] = m[k];  // byte by byte: a match may overlap what it writes
        op += ml;
    }
    return op == oend;
}

NsoStatus nso_to_flat(const uint8_t* nso, size_t size, std::vector<uint8_t>& img, NsoHeader* header) {
    NsoHeader h;
    if (!parse_nso_header(nso, size, h)) return fail(NsoError::NotNso, -1, "not an NSO (no NSO0 header)");
    if (header) *header = h;
    const uint64_t end = h.image_size();
    if (end > (1ull << 30)) return fail(NsoError::BadLayout, -1, "implausible image size 0x%llx", (unsigned long long)end);
    img.assign(size_t(end), 0);
    for (int i = 0; i < 3; i++) {
        const NsoSegment& s = h.seg[i];
        if (uint64_t(s.file_offset) + s.file_size > size)
            return fail(NsoError::Truncated, i, "%s segment ends at 0x%llx, past the end of the file (0x%zx bytes)",
                        nso_segment_name(i), (unsigned long long)(uint64_t(s.file_offset) + s.file_size), size);
        if (s.compressed) {
            if (!lz4_decompress(nso + s.file_offset, s.file_size, img.data() + s.mem_offset, s.size))
                return fail(NsoError::Decompress, i, "%s segment does not decompress to 0x%x bytes", nso_segment_name(i),
                            s.size);
        } else {
            if (s.file_size != s.size)
                return fail(NsoError::BadLayout, i, "%s segment: 0x%x bytes in the file for 0x%x in memory",
                            nso_segment_name(i), s.file_size, s.size);
            std::memcpy(img.data() + s.mem_offset, nso + s.file_offset, s.size);
        }
        if (s.hashed) {
            uint8_t d[32];
            Sha256 sha;
            sha.update(img.data() + s.mem_offset, s.size);
            sha.final(d);
            if (std::memcmp(d, s.sha256, 32) != 0)
                return fail(NsoError::Hash, i, "%s segment fails its SHA-256 check", nso_segment_name(i));
        }
    }
    return {};
}

bool read_file(const std::string& path, std::vector<uint8_t>& out, uint64_t max_size) {
    out.clear();
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(detail::u8(path), ec);
    if (!ec && size <= max_size) out.reserve(size_t(size));
    FILE* f = detail::open_file(detail::u8(path), "rb");
    if (!f) return false;
    uint8_t buf[1 << 16];
    bool ok = true;
    for (;;) {
        size_t n = std::fread(buf, 1, sizeof buf, f);
        if (n == 0) {
            ok = !std::ferror(f);
            break;
        }
        if (out.size() + n > max_size) {
            ok = false;
            break;
        }
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return ok;
}

NsoStatus nso_file_to_flat(const std::string& path, std::vector<uint8_t>& image, NsoHeader* header) {
    std::vector<uint8_t> nso;
    if (!read_file(path, nso)) return fail(NsoError::Io, -1, "cannot be read");
    return nso_to_flat(nso, image, header);
}

}  // namespace nx::prepare
