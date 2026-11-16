// What the prepare/io sources share: UTF-8 paths on every platform, a FILE* opened from one, little-endian reads,
// sizes as text. Plain C++ and stdio, so the library builds for Android as it does for the desktop.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace nx::prepare::detail {

namespace fs = std::filesystem;

// The path of a UTF-8 string, and back (Windows' paths are UTF-16; elsewhere these are the bytes as they are).
inline fs::path u8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
inline std::string str(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

inline FILE* open_file(const fs::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(p.c_str(), m.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }

inline std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// "10.5 GB", "312 KB", "17 bytes": the sizes the messages show (1000-based, as the platforms' file managers).
inline std::string human_size(uint64_t n) {
    char b[32];
    if (n < 1000) {
        std::snprintf(b, sizeof b, "%llu bytes", (unsigned long long)n);
        return b;
    }
    const char* unit[] = {"KB", "MB", "GB", "TB"};
    double v = double(n) / 1000;
    int u = 0;
    while (v >= 1000 && u < 3) v /= 1000, u++;
    std::snprintf(b, sizeof b, v < 100 ? "%.1f %s" : "%.0f %s", v, unit[u]);
    return b;
}

}  // namespace nx::prepare::detail
