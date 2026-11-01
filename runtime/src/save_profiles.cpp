#include "save_profiles.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "save_log.h"
#include "save_storage.h"

namespace fs = std::filesystem;

namespace nx::save_profiles {

namespace {

constexpr const char* kDefaultId = "default";
constexpr const char* kActiveFile = "active.txt";

using nx::save_storage::device_dir;
using nx::save_storage::flat_save;
using nx::save_storage::island_display_name;
using nx::save_storage::last_modified;
using nx::save_storage::non_empty_dir;
using nx::save_storage::park_active_into_profile;
using nx::save_storage::path_utf8;
using nx::save_storage::promote_profile_into_active;
using nx::save_storage::recover;
using nx::save_storage::u8_path;
using nx::save_storage::user_dir;

fs::path profiles_root(const std::string& data_root) { return user_dir(data_root) / "profiles"; }

fs::path profile_dir(const std::string& data_root, const std::string& id) {
    return profiles_root(data_root) / u8_path(id);
}

void set_err(std::string* out, const std::string& v) {
    if (out) *out = v;
}

bool valid_id(const std::string& id) {
    if (id.empty() || id == "." || id == "..") return false;
    for (char ch : id) {
        if (ch == '/' || ch == '\\' || ch == ':') return false;
    }
    return true;
}

std::vector<fs::path> profile_subdirs(const fs::path& root) {
    std::vector<fs::path> v;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return v;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) v.push_back(it->path());
    std::sort(v.begin(), v.end());
    return v;
}

std::string next_profile_id(const std::string& data_root) {
    fs::path root = profiles_root(data_root);
    for (int n = 2; n < 10000; ++n) {
        std::string id = std::string("island-") + std::to_string(n);
        std::error_code ec;
        if (!fs::exists(profile_dir(data_root, id), ec)) return id;
    }
    return "island-" + std::to_string(std::time(nullptr));
}

}  // namespace

bool ensure_migrated(const std::string& data_root, std::string* error) {
    std::error_code ec;
    fs::path proot = profiles_root(data_root);
    fs::create_directories(proot, ec);
    if (ec) return set_err(error, "cannot create profiles folder: " + ec.message()), false;

    bool have_profiles = !profile_subdirs(proot).empty();
    fs::path active = device_dir(data_root);

    if (!have_profiles) {
        fs::path def = profile_dir(data_root, kDefaultId);
        fs::create_directories(def, ec);
        if (flat_save(active, nullptr)) {
            if (!park_active_into_profile(data_root, def, error)) return false;
            log("save_profiles: migrated active save to profile %s", kDefaultId);
        }
    }

    if (read_active_id(data_root).empty()) write_active_id(data_root, kDefaultId);
    return true;
}

std::vector<ProfileInfo> list_profiles(const std::string& data_root) {
    std::vector<ProfileInfo> out;
    for (const fs::path& d : profile_subdirs(profiles_root(data_root))) {
        ProfileInfo p;
        p.id = path_utf8(d.filename());
        if (!valid_id(p.id)) continue;
        fs::path save = d / "device_save";
        p.has_save = flat_save(save, nullptr);
        if (p.has_save) {
            p.island_name = island_display_name(save);
            p.last_modified = last_modified(save);
        }
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const ProfileInfo& a, const ProfileInfo& b) {
        if (a.id == kDefaultId) return true;
        if (b.id == kDefaultId) return false;
        return a.id < b.id;
    });
    return out;
}

std::string read_active_id(const std::string& data_root) {
    fs::path f = profiles_root(data_root) / kActiveFile;
    std::ifstream in(f);
    std::string id;
    if (in >> id) {
        while (!id.empty() && (id.back() == '\r' || id.back() == '\n' || id.back() == ' ')) id.pop_back();
    }
    return valid_id(id) ? id : std::string();
}

void write_active_id(const std::string& data_root, const std::string& id) {
    if (!valid_id(id)) return;
    std::error_code ec;
    fs::create_directories(profiles_root(data_root), ec);
    std::ofstream out(profiles_root(data_root) / kActiveFile, std::ios::trunc);
    out << id << '\n';
}

std::string create_empty_profile(const std::string& data_root, std::string* error) {
    ensure_migrated(data_root, error);
    std::string id = next_profile_id(data_root);
    std::error_code ec;
    fs::create_directories(profile_dir(data_root, id), ec);
    if (ec) return set_err(error, "cannot create profile: " + ec.message()), std::string();
    log("save_profiles: created empty profile %s", id.c_str());
    return id;
}

bool activate_profile(const std::string& data_root, const std::string& id, std::string* error, BackupHook backup) {
    if (!valid_id(id)) return set_err(error, "invalid profile"), false;
    if (!ensure_migrated(data_root, error)) return false;

    fs::path target = profile_dir(data_root, id);
    std::error_code ec;
    if (!fs::is_directory(target, ec)) return set_err(error, "profile not found: " + id), false;

    std::string prev = read_active_id(data_root);
    if (prev.empty()) prev = kDefaultId;
    if (prev == id) return true;

    recover(data_root);
    if (backup && non_empty_dir(device_dir(data_root)) && flat_save(device_dir(data_root), nullptr)) {
        std::string berr;
        if (!backup(&berr)) return set_err(error, "backup before switch failed: " + berr), false;
    }

    if (prev != id) {
        fs::path prev_dir = profile_dir(data_root, prev);
        if (fs::is_directory(prev_dir, ec))
            if (!park_active_into_profile(data_root, prev_dir, error)) return false;
    }

    fs::path save = target / "device_save";
    bool empty = !flat_save(save, nullptr);
    if (!promote_profile_into_active(data_root, target, empty, error)) return false;

    write_active_id(data_root, id);
    log("save_profiles: activated profile %s", id.c_str());
    return true;
}

}  // namespace nx::save_profiles
