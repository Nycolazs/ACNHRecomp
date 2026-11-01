#include "save_storage.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "save_log.h"

namespace fs = std::filesystem;

namespace nx::save_storage {

namespace {

void set_err(std::string* out, const std::string& v) {
    if (out) *out = v;
}

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool remove_all_quiet(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    return !ec;
}

}  // namespace

fs::path u8_path(const std::string& s) { return fs::u8path(s); }

std::string path_utf8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path user_dir(const std::string& data_root) { return u8_path(data_root) / "user"; }

fs::path device_dir(const std::string& data_root) { return user_dir(data_root) / "device_save"; }

fs::path device_size_file(const std::string& data_root) { return user_dir(data_root) / "device_save.size"; }

bool flat_save(const fs::path& d, std::string* why) {
    if (!is_dir(d)) return set_err(why, "folder not found"), false;
    if (!is_file(d / "main.dat")) return set_err(why, "main.dat missing"), false;
    if (!is_file(d / "mainHeader.dat")) return set_err(why, "mainHeader.dat missing"), false;
    if (!is_file(d / "Villager0" / "personal.dat")) return set_err(why, "Villager0/personal.dat missing"), false;
    return true;
}

bool non_empty_dir(const fs::path& d) {
    std::error_code ec;
    return is_dir(d) && !fs::is_empty(d, ec) && !ec;
}

void recover(const std::string& data_root) {
    fs::path dir = device_dir(data_root), old = u8_path(path_utf8(dir) + ".old");
    std::error_code ec;
    if (!fs::exists(dir, ec) && is_dir(old)) {
        fs::rename(old, dir, ec);
        log("save_storage: restored %s after an interrupted swap", path_utf8(dir).c_str());
    }
}

bool swap_in(const std::string& data_root, const fs::path& staging, std::string* error) {
    fs::path dir = device_dir(data_root), old = u8_path(path_utf8(dir) + ".old");
    std::error_code ec;
    fs::remove_all(old, ec);
    bool had = fs::exists(dir, ec);
    if (had) {
        fs::rename(dir, old, ec);
        if (ec) return set_err(error, "cannot move the current save aside: " + ec.message()), false;
    }
    fs::rename(staging, dir, ec);
    if (ec) {
        std::error_code e2;
        if (had) fs::rename(old, dir, e2);
        return set_err(error, "cannot move the new save in: " + ec.message()), false;
    }
    fs::remove_all(old, ec);
    return true;
}

bool copy_tree(const fs::path& src, const fs::path& dst, std::string* error) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return set_err(error, "cannot create " + path_utf8(dst) + ": " + ec.message()), false;
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        fs::path to = dst / it->path().lexically_relative(src);
        std::error_code e2;
        if (it->is_symlink(e2)) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(e2)) {
            fs::create_directories(to, e2);
        } else if (it->is_regular_file(e2)) {
            fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, e2);
        }
        if (e2) return set_err(error, "cannot copy " + path_utf8(it->path()) + ": " + e2.message()), false;
    }
    if (ec) return set_err(error, "cannot read " + path_utf8(src) + ": " + ec.message()), false;
    return true;
}

bool copy_file_if_exists(const fs::path& src, const fs::path& dst, std::string* error) {
    std::error_code ec;
    if (!is_file(src)) return true;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) return set_err(error, "cannot copy " + path_utf8(src) + ": " + ec.message()), false;
    return true;
}

std::string island_display_name(const fs::path& device_save) {
    fs::path land = device_save / "landname.dat";
    if (!is_file(land)) return std::string();
    std::ifstream f(land, std::ios::binary);
    if (!f) return std::string();
    uint8_t b[20];
    f.read(reinterpret_cast<char*>(b), sizeof b);
    size_t n = size_t(f.gcount()) / 2;
    std::string out;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = b[2 * i] | uint32_t(b[2 * i + 1]) << 8;
        if (!c) break;
        if (c < 0x80)
            out += char(c);
        else if (c < 0x800)
            out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
        else
            out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
    return out;
}

std::string last_modified(const fs::path& device_save) {
    std::error_code ec;
    auto ft = fs::last_write_time(device_save / "main.dat", ec);
    if (ec) return std::string();
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t t = std::chrono::system_clock::to_time_t(sys);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

bool park_active_into_profile(const std::string& data_root, const fs::path& profile_dir, std::string* error) {
    recover(data_root);
    fs::path active = device_dir(data_root);
    if (!fs::exists(active)) return true;
    if (!non_empty_dir(active) && !flat_save(active, nullptr)) {
        remove_all_quiet(active);
        return true;
    }
    fs::path staging = profile_dir / "device_save.new";
    fs::path dest = profile_dir / "device_save";
    std::error_code ec;
    remove_all_quiet(staging);
    if (!copy_tree(active, staging, error)) return remove_all_quiet(staging), false;
    remove_all_quiet(dest);
    fs::rename(staging, dest, ec);
    if (ec) {
        remove_all_quiet(staging);
        return set_err(error, "cannot park save into profile: " + ec.message()), false;
    }
    remove_all_quiet(profile_dir / "device_save.size");
    copy_file_if_exists(device_size_file(data_root), profile_dir / "device_save.size", error);
    return true;
}

bool promote_profile_into_active(const std::string& data_root, const fs::path& profile_dir, bool empty_profile,
                                 std::string* error) {
    recover(data_root);
    std::error_code ec;
    if (empty_profile) {
        remove_all_quiet(device_dir(data_root));
        remove_all_quiet(device_size_file(data_root));
        fs::create_directories(device_dir(data_root), ec);
        if (ec) return set_err(error, "cannot prepare empty save slot: " + ec.message()), false;
        return true;
    }
    fs::path src = profile_dir / "device_save";
    if (!flat_save(src, error)) return false;
    fs::path staging = u8_path(path_utf8(user_dir(data_root)) + "/device_save.new");
    remove_all_quiet(staging);
    if (!copy_tree(src, staging, error)) return remove_all_quiet(staging), false;
    if (!swap_in(data_root, staging, error)) return remove_all_quiet(staging), false;
    remove_all_quiet(device_size_file(data_root));
    copy_file_if_exists(profile_dir / "device_save.size", device_size_file(data_root), error);
    return true;
}

}  // namespace nx::save_storage
