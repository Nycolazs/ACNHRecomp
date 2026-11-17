// The command line as UTF-8 on every platform, as the preparation takes its paths. On Windows main's argv is in the
// ANSI code page: a folder named "ção" would come as other bytes (or "?" outside that code page), so the arguments are
// read again from the UTF-16 command line (GetCommandLineW, split by CommandLineToArgvW). Elsewhere argv as it is.
// Header only: acnh_prepare and acnh_recomp include it.
#pragma once

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW (shell32)
#endif

namespace nx::prepare::detail {

inline std::vector<std::string> utf8_args(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    int n = 0;
    if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 0; i < n; i++) {
            const int wl = int(wcslen(w[i]));
            const int len = wl ? WideCharToMultiByte(CP_UTF8, 0, w[i], wl, nullptr, 0, nullptr, nullptr) : 0;
            std::string s(size_t(len > 0 ? len : 0), '\0');
            if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w[i], wl, s.data(), len, nullptr, nullptr);
            out.push_back(std::move(s));
        }
        LocalFree(w);
        if (!out.empty()) return out;
    }
#endif
    for (int i = 0; i < argc; i++) out.push_back(argv[i]);
    return out;
}

}  // namespace nx::prepare::detail
