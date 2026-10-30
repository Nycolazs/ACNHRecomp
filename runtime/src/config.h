// Player settings: <data root>/config.ini, edited by the settings screen (ui/) or by hand.
//
// Every setting is a "section.key = value" line. At startup (config_apply) each one that maps to a runtime switch
// becomes that switch's NX_* environment variable, unless the variable is already set (the environment wins, so
// scripts and launch presets keep working). Settings the runtime reads live (volume, glyph style, ...) are read with
// config_get whenever needed. The file is rewritten by config_save, keeping unknown keys.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nx {

struct ConfigEntry {
    std::string key;    // "video.aspect"
    std::string value;  // "21:9"
};

void config_load();                    // reads <data root>/config.ini (once; later calls re-read)
void mods_fetch();  // mod_fetch.cpp: the pt-BR translation and the SpeedFix files the package carries, into mods/
void config_apply();                   // settings -> NX_* environment defaults (call once, early in startup)
bool config_save();                    // writes the file back
std::string config_get(const std::string& key, const std::string& fallback = "");
int config_get_int(const std::string& key, int fallback);
float config_get_float(const std::string& key, float fallback);
bool config_get_bool(const std::string& key, bool fallback);
void config_set(const std::string& key, const std::string& value);
std::vector<ConfigEntry> config_all();
uint64_t config_generation();          // bumped by every config_set: readers cache by it
// The variable's value when it was set before the game (config.txt, a script, a test): it wins over the setting.
// nullptr when unset or set by config_apply from config.ini (then the setting itself is read, live).
const char* config_env_override(const char* name);
bool config_show_fps();                 // whether the on-screen FPS counter is shown (video.show_fps / NX_SHOW_FPS)
bool config_is_handheld();               // whether handheld mode (1280x720) is active (video.mode / NX_HANDHELD)
// The keys and values of a video.preset ("quality", "balanced", "low"); empty for "custom" or an unknown name.
std::vector<std::pair<std::string, std::string>> config_preset(const std::string& name);
// Whether changing this key takes effect only after a restart (the screen says so).
bool config_needs_restart(const std::string& key);

}  // namespace nx
