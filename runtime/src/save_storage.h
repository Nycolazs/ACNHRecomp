// Shared ACNH device save filesystem helpers (launcher + runtime). Paths are UTF-8. Nothing here throws.
#pragma once

#include <filesystem>
#include <string>

namespace nx::save_storage {

std::filesystem::path u8_path(const std::string& s);
std::string path_utf8(const std::filesystem::path& p);

std::filesystem::path user_dir(const std::string& data_root);
std::filesystem::path device_dir(const std::string& data_root);
std::filesystem::path device_size_file(const std::string& data_root);

bool flat_save(const std::filesystem::path& d, std::string* why = nullptr);
// The save folder in `path` itself, or the only one one or two levels below (JKSV / Checkpoint layouts).
bool find_acnh_save_root(const std::filesystem::path& path, std::filesystem::path* root, std::string* why = nullptr);
bool non_empty_dir(const std::filesystem::path& d);

void recover(const std::string& data_root);
// Replaces user/device_save with the folder `staging` (a sibling path on the same volume).
bool swap_in(const std::string& data_root, const std::filesystem::path& staging, std::string* error);

bool copy_tree(const std::filesystem::path& src, const std::filesystem::path& dst, std::string* error);
bool copy_file_if_exists(const std::filesystem::path& src, const std::filesystem::path& dst, std::string* error);

std::string island_display_name(const std::filesystem::path& device_save);
std::string last_modified(const std::filesystem::path& device_save);

// Copies the live user/device_save tree into profile_dir/device_save (+ device_save.size).
bool park_active_into_profile(const std::string& data_root, const std::filesystem::path& profile_dir,
                              std::string* error);
// Promotes profile_dir/device_save into user/device_save, or prepares an empty slot when empty_profile.
bool promote_profile_into_active(const std::string& data_root, const std::filesystem::path& profile_dir,
                                 bool empty_profile, std::string* error);

}  // namespace nx::save_storage
