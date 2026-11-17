// romfs.pak writer (romfs_pak.h), tools/pack_romfs.py in C++.
#include "romfs_pak.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "inputs.h"
#include "io_util.h"

namespace nx::prepare {

namespace fs = std::filesystem;
using detail::str;
using detail::u8;

namespace {

void put16(std::string& s, uint16_t v) { s += char(v & 0xFF), s += char(v >> 8); }
void put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s += char((v >> (8 * i)) & 0xFF);
}
void put64(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; i++) s += char((v >> (8 * i)) & 0xFF);
}

// The index as it goes into the file.
std::string index_bytes(const RomfsPakLayout& l) {
    std::string h = "NXRP";
    put32(h, 1);
    put32(h, uint32_t(l.files.size()));
    for (const RomfsPakEntry& e : l.files) {
        put16(h, uint16_t(e.path.size()));
        h += e.path;
        put64(h, e.offset);
        put64(h, e.size);
    }
    return h;
}

}  // namespace

bool romfs_pak_layout(const std::string& romfs_dir, RomfsPakLayout& out, std::string& error,
                      const std::vector<std::string>& exclude_root, bool skip_packages) {
    out = {};
    std::error_code ec;
    const fs::path root = u8(romfs_dir);
    if (!fs::is_directory(root, ec)) return error = romfs_dir + ": not a folder", false;
    // os.walk(root) as pack_romfs.py runs it: every file, links to files included (their target's size), links to
    // folders listed but not entered.
    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
    if (ec) return error = romfs_dir + ": " + ec.message(), false;
    for (; it != end; it.increment(ec)) {
        if (ec) return error = romfs_dir + ": " + ec.message(), false;
        const fs::directory_entry& e = *it;
        const bool link = e.is_symlink(ec);
        if (skip_packages && is_game_package(str(e.path().filename()))) {
            if (e.is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (e.is_directory(ec)) {
            if (link) it.disable_recursion_pending();
            if (it.depth() == 0 && std::find(exclude_root.begin(), exclude_root.end(), str(e.path().filename())) !=
                                       exclude_root.end())
                it.disable_recursion_pending();
            continue;
        }
        const std::u8string g = e.path().lexically_relative(root).generic_u8string();
        const std::string rel(reinterpret_cast<const char*>(g.data()), g.size());
        if (it.depth() == 0 && std::find(exclude_root.begin(), exclude_root.end(), rel) != exclude_root.end()) continue;
        if (!e.is_regular_file(ec))
            return error = str(e.path()) + ": neither a file nor a folder (a broken link?)", false;
        const uint64_t size = e.file_size(ec);
        if (ec) return error = str(e.path()) + ": " + ec.message(), false;
        if (rel.size() > 0xFFFF) return error = str(e.path()) + ": path too long for the pack", false;
        out.files.push_back({rel, str(e.path()), 0, size});
    }
    if (out.files.size() > 0xFFFFFFFFu) return error = romfs_dir + ": too many files for the pack", false;
    std::sort(out.files.begin(), out.files.end(),
              [](const RomfsPakEntry& a, const RomfsPakEntry& b) { return a.path < b.path; });  // bytes = code points
    uint64_t h = 12;
    for (const RomfsPakEntry& e : out.files) h += 2 + e.path.size() + 16;
    out.header_size = h;
    uint64_t at = (h + 15) & ~uint64_t(15);
    for (RomfsPakEntry& e : out.files) {
        e.offset = at;
        at = (at + e.size + 15) & ~uint64_t(15);
    }
    out.size = out.files.empty() ? h : out.files.back().offset + out.files.back().size;  // no padding after the last
    return true;
}

PakResult write_romfs_pak(const std::string& romfs_dir, const std::string& out_path, const RomfsPakOptions& opt) {
    PakResult r;
    RomfsPakLayout l;
    if (!romfs_pak_layout(romfs_dir, l, r.error, opt.exclude_root, opt.skip_packages)) return r;
    detail::PakOut out(out_path);
    if (!out.open(r.error)) return r;
    const std::string index = index_bytes(l);
    if (!out.write(index.data(), index.size(), r.error)) return r;
    detail::ProgressPacer pace(opt, l.files.size(), l.size);
    uint64_t done = 0;
    for (const RomfsPakEntry& e : l.files) {
        if (opt.cancel && opt.cancel->load()) return r.cancelled = true, r;
        if (!out.zeros(e.offset - out.written(), r.error)) return r;
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
