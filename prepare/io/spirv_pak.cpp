// spirv.pak writer (spirv_pak.h), tools/pack_spirv.py in C++.
#include "spirv_pak.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "io_util.h"

namespace nx::prepare {

namespace fs = std::filesystem;
using detail::str;
using detail::u8;

namespace {

// Characters (code points) of a UTF-8 string: pack_spirv.py's len() of the name.
size_t code_points(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

}  // namespace

bool spirv_pak_layout(const std::string& spirv_dir, SpirvPakLayout& out, std::string& error) {
    out = {};
    std::error_code ec;
    const fs::path dir = u8(spirv_dir);
    if (!fs::is_directory(dir, ec)) return error = spirv_dir + ": not a folder", false;
    std::vector<std::string> names;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const std::string n = str(it->path().filename());
        if (n.size() >= 4 && n.compare(n.size() - 4, 4, ".spv") == 0 && code_points(n) - 4 <= 16) names.push_back(n);
    }
    if (ec) return error = spirv_dir + ": " + ec.message(), false;
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) out.files.push_back({n.substr(0, std::min<size_t>(n.size() - 4, 16)), n});
    if (fs::is_regular_file(dir / "results.tsv", ec)) {
        out.files.push_back({"results.tsv", "results.tsv"});
        out.results_tsv = true;
    }
    if (out.files.size() > 0xFFFFFFFFu) return error = spirv_dir + ": too many files for the pack", false;
    uint64_t at = 8 + 32 * uint64_t(out.files.size());
    for (SpirvPakEntry& e : out.files) {
        const fs::path p = dir / u8(e.source);
        e.source = str(p);
        if (!fs::is_regular_file(p, ec)) return error = e.source + ": not a file", false;
        e.size = fs::file_size(p, ec);
        if (ec) return error = e.source + ": " + ec.message(), false;
        e.offset = at;
        at += e.size;
    }
    out.size = at;
    return true;
}

PakResult write_spirv_pak(const std::string& spirv_dir, const std::string& out_path, const PakOptions& opt) {
    PakResult r;
    SpirvPakLayout l;
    if (!spirv_pak_layout(spirv_dir, l, r.error)) return r;
    std::string index;
    auto put32 = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) index += char((v >> (8 * i)) & 0xFF);
    };
    auto put64 = [&](uint64_t v) {
        for (int i = 0; i < 8; i++) index += char((v >> (8 * i)) & 0xFF);
    };
    put32(0x314B5053);  // "SPK1"
    put32(uint32_t(l.files.size()));
    for (const SpirvPakEntry& e : l.files) {
        index += e.key;
        index.append(16 - e.key.size(), '\0');
        put64(e.offset);
        put64(e.size);
    }
    detail::PakOut out(out_path);
    if (!out.open(r.error) || !out.write(index.data(), index.size(), r.error)) return r;
    detail::ProgressPacer pace(opt, l.files.size(), l.size);
    uint64_t done = 0;
    for (const SpirvPakEntry& e : l.files) {
        if (opt.cancel && opt.cancel->load()) return r.cancelled = true, r;
        if (!detail::copy_file_into(e.source, e.size, out, opt.cancel,
                                    [&](uint64_t) { pace.update(done, out.written()); }, r.cancelled, r.error))
            return r;
        pace.update(++done, out.written());
    }
    if (out.written() != l.size) return r.error = "internal error: pack size", r;
    if (!out.finish(r.error)) return r;
    pace.update(done, l.size, true);
    r.ok = true;
    r.files = l.files.size();
    r.bytes = l.size;
    return r;
}

}  // namespace nx::prepare
