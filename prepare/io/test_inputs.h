// Synthetic game inputs for the preparation's tests (prepare_io_test, prepare_e2e_test): no game data. NSOs built here
// (LZ4 literal runs and a match, the SHA-256 of each segment), a fake main.npdm, a small RomFS with every folder the
// game needs and the files the probe looks for, the ExeFS the emulators extract.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "inputs.h"
#include "sha256.h"

namespace nx::prepare::test {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

inline void write_file(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
inline void write_text(const fs::path& p, const std::string& t) { write_file(p, Bytes(t.begin(), t.end())); }
inline Bytes read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
inline Bytes pattern(size_t n, uint32_t seed) {
    Bytes b(n);
    for (size_t i = 0; i < n; i++) seed = seed * 1103515245u + 12345u, b[i] = uint8_t(seed >> 16);
    return b;
}
inline void put32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (8 * i));
}
inline void put64(Bytes& b, size_t at, uint64_t v) {
    for (int i = 0; i < 8; i++) b[at + i] = uint8_t(v >> (8 * i));
}

// ---- synthetic NSOs ---------------------------------------------------------------------------------------------

// An LZ4 block of literals only: one sequence, no match (valid for every decoder: the last sequence has no match).
inline Bytes lz4_literals(const Bytes& d) {
    Bytes o;
    const size_t n = d.size();
    o.push_back(uint8_t((n >= 15 ? 15 : n) << 4));
    if (n >= 15) {
        size_t r = n - 15;
        while (r >= 255) o.push_back(255), r -= 255;
        o.push_back(uint8_t(r));
    }
    o.insert(o.end(), d.begin(), d.end());
    return o;
}

// 8 literals, a 32-byte match at offset 8 (overlapping, extended length), then 16 literals: what lz4_text() gives.
inline Bytes text_plain() {
    Bytes t = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
    for (int k = 0; k < 32; k++) t.push_back(t[k % 8]);
    Bytes tail = pattern(16, 7);
    t.insert(t.end(), tail.begin(), tail.end());
    return t;
}
inline Bytes lz4_text() {
    Bytes o = {uint8_t(8 << 4 | 15), 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 8, 0, uint8_t(32 - 4 - 15)};
    o.push_back(uint8_t(15 << 4));
    o.push_back(1);  // 15 + 1 literals
    Bytes tail = pattern(16, 7);
    o.insert(o.end(), tail.begin(), tail.end());
    return o;
}

struct NsoParts {
    Bytes text = text_plain(), ro = pattern(300, 11), data = pattern(100, 13);
};

inline Bytes hex_id(const std::string& hex) {
    Bytes b(32, 0);
    for (size_t i = 0; i + 1 < hex.size() && i / 2 < 32; i += 2) b[i / 2] = uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16));
    return b;
}

// text and ro LZ4-compressed, data plain, the three hashed (flags 0x3B), at 0, 0x1000 and 0x2000.
inline Bytes make_nso(const std::string& build_id_hex, const NsoParts& p = {}) {
    const Bytes* seg[3] = {&p.text, &p.ro, &p.data};
    const Bytes ctext = lz4_text(), cro = lz4_literals(p.ro);
    const Bytes* file[3] = {&ctext, &cro, &p.data};
    Bytes n(0x100, 0);
    std::memcpy(n.data(), "NSO0", 4);
    put32(n, 0xC, 0x3B);
    uint32_t mem = 0, off = 0x100;
    for (int i = 0; i < 3; i++) {
        put32(n, 0x10 + 0x10 * i, off);
        put32(n, 0x14 + 0x10 * i, mem);
        put32(n, 0x18 + 0x10 * i, uint32_t(seg[i]->size()));
        put32(n, 0x60 + 4 * i, uint32_t(file[i]->size()));
        uint8_t d[32];
        nx::Sha256 h;
        h.update(seg[i]->data(), seg[i]->size());
        h.final(d);
        std::memcpy(n.data() + 0xA0 + 0x20 * i, d, 32);
        off += uint32_t(file[i]->size());
        mem = (mem + uint32_t(seg[i]->size()) + 0xFFF) & ~0xFFFu;
    }
    put32(n, 0x3C, 0x40);  // bss
    const Bytes id = hex_id(build_id_hex);
    std::memcpy(n.data() + 0x40, id.data(), 32);
    for (int i = 0; i < 3; i++) n.insert(n.end(), file[i]->begin(), file[i]->end());
    return n;
}

inline Bytes make_npdm(uint64_t program_id) {
    Bytes b(0xC0, 0);
    std::memcpy(b.data(), "META", 4);
    put32(b, 0x70, 0x80);
    put32(b, 0x74, 0x40);
    std::memcpy(b.data() + 0x80, "ACI0", 4);
    put64(b, 0x90, program_id);
    return b;
}

inline void make_exefs(const fs::path& d, const std::string& main_id) {
    write_file(d / "main", make_nso(main_id));
    write_file(d / "sdk", make_nso("5D4B0A6C3B2A19080706050403020100"));
    write_file(d / "subsdk0", make_nso("0123456789ABCDEF0123456789ABCDEF"));
    write_file(d / "rtld", make_nso("AA000000000000000000000000000000"));
    write_file(d / "main.npdm", make_npdm(kAcnhProgramId));
}

// A small RomFS with every folder the game needs and the files the probe looks for.
inline const char* const kDirs[] = {"Bcsv", "Effect", "Font", "Layout", "Message", "Model", "Movie", "Pack", "Shader", "System"};
inline const char* const kKeys[] = {"Shader/Park_UBER_All.Nin_NX_NVN.szs", "System/GSys/gsys_resource.Nin_NX_NVN.release.szs",
                              "Pack/StaticParam.pack", "Font/ScalableFont.sarc.zs", "Movie/OpeningMovie.webm"};
inline int make_romfs(const fs::path& d, bool skeleton = false, const char* skip_dir = nullptr) {
    int n = 0;
    uint32_t seed = 1;
    for (const char* dir : kDirs) {
        if (skip_dir && std::strcmp(dir, skip_dir) == 0) continue;
        for (int k = 0; k < 3; k++, n++)
            write_file(d / dir / ("file" + std::to_string(k) + ".bin"), skeleton ? Bytes() : pattern(100 + k * 37, seed++));
    }
    for (const char* k : kKeys) {
        if (skip_dir && std::strncmp(k, skip_dir, std::strlen(skip_dir)) == 0) continue;
        write_file(d / k, skeleton ? Bytes() : pattern(64, seed++)), n++;
    }
    return n;
}

}  // namespace nx::prepare::test
