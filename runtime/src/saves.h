// Save data management for the settings screen: the device save (ACNH keeps one island per console, see hle_fs.cpp),
// finding the saves of emulators on this computer and of Switch backup tools (JKSV, Checkpoint) on a mounted SD card,
// importing and exporting them, and timestamped backups. Paths are UTF-8. Nothing here throws.
//
// Call these while the game is not running (or restart it after an import or restore): the game writes the save in
// place and keeps files open, so swapping the folder under it would lose or mix writes.
#pragma once

#include <string>
#include <vector>

namespace nx::saves {

std::string save_dir();        // <data root>/user/device_save
bool has_save();               // a complete ACNH save is there
std::string last_modified();   // "2026-10-22 14:03" (local time) of main.dat, "" if none

struct Found {
    std::string label;  // "Ryujinx", "Yuzu (Flatpak)", "Sudachi", "JKSV (SD card)", ...
    std::string path;   // a folder holding main.dat (or a .zip): pass it to import_save
};
// ACNH saves of emulators installed on this computer, of other ACNHRecomp copies, and the newest JKSV / Checkpoint
// backup on mounted volumes (SD cards).
std::vector<Found> detect_emulator_saves();

// main.dat, mainHeader.dat and Villager0/personal.dat, in `dir` or in exactly one folder one or two levels below it
// (a JKSV / Checkpoint title folder), or inside a .zip. why: what is wrong, e.g. "main.dat missing".
bool looks_like_acnh_save(const std::string& dir, std::string* why);
// Replaces the device save with the one in `from` (folder or .zip, as looks_like_acnh_save accepts). Backs up the
// current save first; the new one is staged next to save_dir and renamed in, so a failure never leaves a half save.
bool import_save(const std::string& from, std::string* error);
// Copies the save into `to`/ACNH-<YYYYmmdd-HHMMSS>/ (files at its root: the layout JKSV and Checkpoint restore from),
// or into the zip `to` when it ends with .zip. written: the path written.
bool export_save(const std::string& to, std::string* written, std::string* error);

// <data root>/user/backups/<island name> <8 hex landname.dat hash>/<YYYYmmdd-HHMMSS>[.auto]/, for the island being
// played. Two islands with the same name share a folder (the save holds nothing else that tells them apart). Per
// island, the 10 newest manual backups and the 3 newest automatic ones (automatic: the Island Backup, hle_olsc.cpp)
// are kept, each kind pruned on its own. Returns "<island folder>/<backup>", "" on failure.
std::string backup_now(std::string* error, bool automatic = false);
// Every island's backups as "<island folder>/<backup>": the island being played first, then the others, each newest
// first. A missing or corrupt save, an import or a profile switch never hides a backup.
std::vector<std::string> list_backups();
bool restore_backup(const std::string& name, std::string* error);  // a list_backups name; backs up the current save first
std::string backups_folder();  // <data root>/user/backups/<island folder> of the island being played

}  // namespace nx::saves
