// Launcher save profiles: offline copies under user/profiles/, active island in user/device_save.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace nx::save_profiles {

struct ProfileInfo {
    std::string id;
    std::string island_name;  // from landname.dat, empty when none yet
    std::string last_modified;
    bool has_save = false;
};

bool ensure_migrated(const std::string& data_root, std::string* error = nullptr);

std::vector<ProfileInfo> list_profiles(const std::string& data_root);
std::string read_active_id(const std::string& data_root);
void write_active_id(const std::string& data_root, const std::string& id);

// Creates user/profiles/<id>/ with no device_save yet. Returns the new id.
std::string create_empty_profile(const std::string& data_root, std::string* error = nullptr);

using BackupHook = std::function<bool(std::string* error)>;
bool activate_profile(const std::string& data_root, const std::string& id, std::string* error = nullptr,
                      BackupHook backup = {});

bool delete_profile(const std::string& data_root, const std::string& id, std::string* error = nullptr);
// Replaces the profile's device save from a folder (or JKSV/Checkpoint layout under it).
bool import_into_profile(const std::string& data_root, const std::string& id, const std::string& from,
                         std::string* error = nullptr);

}  // namespace nx::save_profiles
