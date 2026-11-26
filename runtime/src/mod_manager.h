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
    bool has_exefs = false;  // Atmosphere ExeFS present (or was, in the pack it came from); ignored by ACNHRecomp
    bool has_romfs = false;    // RomFS files replaced as they are (LayeredFS)
    bool has_nookkit = false;  // NookKit JSON patches (romfs/NookKit/)
};

std::vector<ModInfo> list_mods(const std::string& data_root);

// Comma-separated enable order (first wins in LayeredFS). Empty clears game.mods.
bool write_enabled(const std::string& data_root, const std::vector<std::string>& enabled_ids,
                   std::string* error = nullptr);

bool set_enabled(const std::string& data_root, const std::string& id, bool on, std::string* error = nullptr);

// Move `id` one step earlier (toward first / higher priority) or later in game.mods.
bool move_enabled(const std::string& data_root, const std::string& id, int delta, std::string* error = nullptr);

// Folder or .zip with a romfs/ (case-insensitive) within 0–3 nesting levels (atmosphere/contents/<title id>/romfs).
// Copies only the RomFS tree. Without a romfs/: a NookKit/ folder (taken as romfs/NookKit/), else the NookKit JSON
// patches in it; a single .json file is one NookKit patch. Auto-enables the new mod. Sets *had_exefs when
// ExeFS/skyline was detected beside romfs (not imported), unless it is only NookKit's runtime for JSON patches.
bool import_mod(const std::string& data_root, const std::string& source_path, std::string* out_id = nullptr,
                bool* had_exefs = nullptr, std::string* error = nullptr);

bool delete_mod(const std::string& data_root, const std::string& id, std::string* error = nullptr);

// Renames mods/<id> to the folder-safe form of `name` (*new_id), keeping its place in game.mods. Fails when another
// mod has that name; bundled mods (ptbr, speedfix60) keep theirs.
bool rename_mod(const std::string& data_root, const std::string& id, const std::string& name,
                std::string* new_id = nullptr, std::string* error = nullptr);

std::string mod_folder(const std::string& data_root, const std::string& id);

}  // namespace nx::mod_manager
