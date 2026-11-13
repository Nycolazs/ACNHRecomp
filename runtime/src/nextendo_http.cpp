// HTTPS via curl on PATH (Windows 10+, macOS, Linux) — same pattern as launcher/desktop.cpp.
#include "nextendo_http.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace nx::nextendo {

namespace {

namespace fs = std::filesystem;

std::string temp_path(const char* prefix) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    char name[80];
    std::snprintf(name, sizeof name, "%s%016llx", prefix, (unsigned long long)gen());
#if defined(_WIN32)
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    return std::string(dir) + name;
#else
    const char* t = std::getenv("TMPDIR");  // per-user on macOS; else the shared /tmp (the files are 0600 there)
    return std::string(t && *t ? t : "/tmp") + "/" + name;
#endif
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Request files hold the bearer token: owner-only on POSIX (/tmp is shared); %TEMP% is per-user on Windows.
bool write_file(const std::string& path, const std::string& data) {
#if !defined(_WIN32)
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    const bool ok = write(fd, data.data(), data.size()) == ssize_t(data.size());
    close(fd);
    return ok;
#else
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), std::streamsize(data.size()));
    return true;
#endif
}

// Last HTTP status from a curl -D dump (follows -L redirects).
int status_from_headers(const std::string& headers) {
    int status = 0;
    size_t i = 0;
    while (i < headers.size()) {
        size_t line_end = headers.find('\n', i);
        std::string line = headers.substr(i, line_end == std::string::npos ? std::string::npos : line_end - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("HTTP/", 0) == 0) {
            size_t sp = line.find(' ');
            if (sp != std::string::npos) status = std::atoi(line.c_str() + sp + 1);
        }
        if (line_end == std::string::npos) break;
        i = line_end + 1;
    }
    return status;
}

int run_curl(const std::vector<std::string>& argv) {
#if defined(_WIN32)
    // CreateProcessW directly — do not wrap in cmd.exe (it expands %VAR% and breaks -w / URLs).
    std::wstring cmd;
    for (const std::string& a : argv) {
        int n = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, w.data(), n);
        if (!cmd.empty()) cmd += L' ';
        cmd += L'"';
        for (wchar_t c : w) {
            if (c == L'"') cmd += L'\\';
            cmd += c;
        }
        cmd += L'"';
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
#else
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t child = 0;
    int rc = posix_spawnp(&child, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

}  // namespace

HttpResponse http_request(const std::string& method, const std::string& url,
                          const std::map<std::string, std::string>& headers, const std::string& body,
                          const std::string& content_type) {
    HttpResponse r;
    const std::string body_path = temp_path("nx_http_body_");
    const std::string out_path = temp_path("nx_http_out_");
    const std::string hdr_path = temp_path("nx_http_hdr_");
    std::error_code ec;
    if (!body.empty() && !write_file(body_path, body)) {
        r.error = "could not write request body";
        return r;
    }
    // The response (its access, refresh and NEX tokens) and its headers: created here, owner-only, so curl only
    // writes into them; made by curl they would take the umask (0644 in a shared /tmp).
    if (!write_file(out_path, "") || !write_file(hdr_path, "")) {
        fs::remove(out_path, ec), fs::remove(hdr_path, ec), fs::remove(body_path, ec);
        r.error = "could not create the response files";
        return r;
    }

    // Use -D for status (not -w %{http_code}): cmd-free CreateProcess still avoids % pitfalls if PATH wraps curl.
    std::vector<std::string> argv = {"curl", "-sS", "-L", "--connect-timeout", "15", "--max-time", "45",
                                     "-D", hdr_path, "-o", out_path, "-X", method};
    // Headers go through a file (-H @file): on the command line the bearer token would be readable by any process
    // that lists command lines.
    std::string header_lines;
    for (const auto& [k, v] : headers) header_lines += k + ": " + v + "\n";
    if (!content_type.empty()) header_lines += "Content-Type: " + content_type + "\n";
    const std::string req_hdr_path = temp_path("nx_http_req_");
    if (!header_lines.empty()) {
        if (!write_file(req_hdr_path, header_lines)) {
            r.error = "could not write request headers";
            return r;
        }
        argv.push_back("-H");
        argv.push_back("@" + req_hdr_path);
    }
    if (!body.empty()) {
        argv.push_back("--data-binary");
        argv.push_back("@" + body_path);
    }
    argv.push_back(url);

    const int exit_code = run_curl(argv);
    r.body = read_file(out_path);
    r.status = status_from_headers(read_file(hdr_path));
    fs::remove(body_path, ec);
    fs::remove(out_path, ec);
    fs::remove(hdr_path, ec);
    fs::remove(req_hdr_path, ec);
    if (exit_code < 0) {
        r.error = "curl could not start (is curl on PATH?)";
        return r;
    }
    if (exit_code != 0 && r.status == 0) {
        r.error = "curl failed (exit " + std::to_string(exit_code) + ")";
        return r;
    }
    if (r.status == 0) r.error = "no response from Nextendo";
    return r;
}

}  // namespace nx::nextendo
