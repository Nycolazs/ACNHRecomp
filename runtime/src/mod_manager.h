// Launcher mod manager: RomFS LayeredFS packs under mods/<id>/romfs/, enablement via config.ini game.mods.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nx::mod_manager {

struct ModInfo {
    std::string id;          // folder name under mods/
    bool enabled = false;    // listed in game.mods
    bool managed = false;    // ptbr / speedfix60 (bundled / settings-linked)
    bool has_exefs = false;  // Atmosphere ExeFS present; ignored by ACNHRecomp
    size_t file_count = 0;
};

std::vector<ModInfo> list_mods(const std::string& data_root);

// Comma-separated enable order (first wins in LayeredFS). Empty clears game.mods.
bool write_enabled(const std::string& data_root, const std::vector<std::string>& enabled_ids,
                   std::string* error = nullptr);

bool set_enabled(const std::string& data_root, const std::string& id, bool on, std::string* error = nullptr);

// Move `id` one step earlier (toward first / higher priority) or later in game.mods.
bool move_enabled(const std::string& data_root, const std::string& id, int delta, std::string* error = nullptr);

// Folder or .zip with a romfs/ (case-insensitive) within 0–2 nesting levels. Copies only the RomFS tree.
// Auto-enables the new mod. Sets *had_exefs when ExeFS/skyline was detected beside romfs (not imported).
bool import_mod(const std::string& data_root, const std::string& source_path, std::string* out_id = nullptr,
                bool* had_exefs = nullptr, std::string* error = nullptr);

bool delete_mod(const std::string& data_root, const std::string& id, std::string* error = nullptr);

std::string mod_folder(const std::string& data_root, const std::string& id);

}  // namespace nx::mod_manager
