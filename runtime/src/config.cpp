// <data root>/config.ini: see config.h.
#include "config.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

std::mutex g_lock;
std::vector<ConfigEntry> g_entries;  // file order
std::atomic<uint64_t> g_generation{1};
bool g_loaded = false;

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string path() { return plat::data_root() + "/config.ini"; }

ConfigEntry* find(const std::string& key) {
    for (auto& e : g_entries)
        if (e.key == key) return &e;
    return nullptr;
}

std::set<std::string> g_env_from_config;  // the variables config_apply set (under g_lock): not overrides

void set_env_default(const char* name, const std::string& value) {
    if (std::getenv(name)) return;  // set by hand or by a preset: it wins
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 0);
#endif
    g_env_from_config.insert(name);
}

void unset_env(const char* name) {
    g_env_from_config.insert(name);  // config_apply's doing: not an override either
#if defined(_WIN32)
    _putenv_s(name, "");  // an empty value removes the variable
#else
    unsetenv(name);
#endif
}

// Settings that become a runtime switch. `map` turns the setting's value into the variable's ("" = leave unset).
struct Mapping {
    const char* key;
    const char* env;
    std::string (*map)(const std::string&);
    bool restart;
};

std::string same(const std::string& v) { return v; }
std::string on_off(const std::string& v) { return v == "1" || v == "on" || v == "true" ? "1" : ""; }
std::string on_or_zero(const std::string& v) { return v.empty() ? "" : on_off(v).empty() ? "0" : "1"; }  // off says so
std::string off_is_zero(const std::string& v) { return v == "0" || v == "off" || v == "false" ? "0" : ""; }
std::string auto_empty(const std::string& v) { return v == "auto" ? "" : v; }
std::string off_empty(const std::string& v) { return v == "off" ? "" : v; }
std::string game_empty(const std::string& v) { return v == "game" ? "" : v; }

// pt-BR is not a console language: the fan translation replaces the European Spanish files, so the console
// language becomes "es" and the translation's files are layered over the RomFS (hle_fs.cpp, game.mods).
std::string game_language(const std::string& v) {
    if (v.empty() || v == "auto") return "";
    if (v == "pt-BR") return "es";
    return v;
}
std::string handheld(const std::string& v) { return v == "handheld" ? "1" : "0"; }
std::string fsr(const std::string& v) { return v == "fsr" ? "1" : "0"; }
std::string fps_cap(const std::string& v) { return v == "30" ? "30" : ""; }
// "60native": the game's logic steps at 60 Hz too (games/acnh/native60.cpp, docs/NATIVE60.md).
// "60": tmouh's SpeedFix (tools/setup_60fps.sh) rescales the game for 60 updates a second; without it, 60 presents a
// second would run the game twice as fast, so "60" stays at 30 until it is installed.
bool speedfix_installed() {
    std::error_code ec;
    return std::filesystem::is_directory(std::filesystem::u8path(plat::data_root() + "/mods/speedfix60/romfs"), ec);
}
std::string speedfix(const std::string& v) { return v == "60" && speedfix_installed() ? "1" : ""; }
std::string interval(const std::string& v) { return (v == "60" && speedfix_installed()) || v == "60native" ? "1" : ""; }
std::string native60(const std::string& v) { return v == "60native" ? "1" : ""; }
// lsfg-vk is a Vulkan layer of its own. "off" says so: frame generation is on by default on Android.
std::string framegen(const std::string& v) { return v == "builtin" ? "1" : v.empty() ? "" : "0"; }
// The screen's 0..100% sharpness -> FSR's RCAS attenuation in stops (0 sharpest, 2 softest).
std::string fsr_sharpness(const std::string& v) {
    if (v.empty()) return "";
    char b[16];
    std::snprintf(b, sizeof b, "%.2f", (100 - std::clamp(std::atoi(v.c_str()), 0, 100)) / 50.0);
    return b;
}

// video.aspect and video.resolution are read live (nvn_ultrawide.cpp); NX_ULTRAWIDE / NX_RENDER_HEIGHT override them.
const Mapping kMappings[] = {
    {"game.language", "NX_LANGUAGE", game_language, true},
    {"video.fullscreen", "NX_FULLSCREEN", on_off, false},
    {"video.show_fps", "NX_SHOW_FPS", on_off, false},
    {"video.window", "NX_RESOLUTION", auto_empty, true},
    {"video.mode", "NX_HANDHELD", handheld, true},
    {"video.upscaler", "NX_FSR", fsr, true},
    {"video.fsr_sharpness_pct", "NX_FSR_SHARP", fsr_sharpness, true},
    {"video.antialiasing", "NX_AA", off_empty, true},  // fxaa (nvn_present.cpp), ssaa15 / ssaa2 (nvn_ultrawide.cpp)
    {"video.anisotropy", "NX_ANISOTROPY", game_empty, true},  // 2 .. 16 (nvn_sampler.cpp)
    {"video.gpu", "NX_GPU", auto_empty, true},
    {"video.frame_rate", "NX_PRESENT_INTERVAL", interval, true},
    {"video.frame_rate", "NX_NATIVE60", native60, true},
    {"video.frame_rate", "NX_SPEEDFIX", speedfix, true},
    {"video.frame_cap", "NX_FPS_CAP", fps_cap, true},
    {"video.frame_gen", "NX_FRAMEGEN", framegen, true},
    {"video.async_shaders", "NX_ASYNC_PIPELINES", on_or_zero, true},  // on by default on Android
    {"video.vsync", "NX_VSYNC", off_is_zero, true},
    {"video.shader_prepare", "NX_PIPE_PREWARM", off_is_zero, true},
    {"audio.reverb", "NX_AUDIO_REVERB", off_is_zero, false},  // also read live (hle_audio.cpp)
    {"video.root_clocks", "NX_ROOT_CLOCKS", on_off, true},  // Android, rooted phones (perf_hint.cpp)
};

// video.preset: a named group of the settings that cost the most (measured on an M1, intro scene, GPU time per frame:
// TV mode 1080p 14.9 ms, handheld 720p 9.4 ms, TV mode at 540p 6.8 ms; the audio reverb ~1 CPU core). Choosing one on the
// settings screen writes its keys; in config.ini a preset fills only the keys the file leaves out, so a hand-set
// key still wins. "custom" (or none): nothing.
struct Preset {
    const char* name;
    std::vector<std::pair<const char*, const char*>> keys;
};
const std::vector<Preset>& presets() {
    static const std::vector<Preset> p = {
        {"quality", {{"video.mode", "docked"}, {"video.resolution", "native"}, {"video.upscaler", "linear"},
                     {"audio.reverb", "1"}}},
        {"balanced", {{"video.mode", "handheld"}, {"video.resolution", "native"}, {"video.upscaler", "fsr"},
                      {"audio.reverb", "1"}}},
        // Weak PCs (dual/quad-core CPUs, Intel UHD/Iris, old APUs, 4-6 GB): TV mode drawn at 540 rows (half of 1080)
        // and upscaled with FSR 1, no frame generation, the CPU-rendered reverb bypassed. TV mode at 540 measured
        // cheaper on the GPU than handheld mode at the same 540 rows (6.8 vs 8.2 ms; handheld's 720p native: 9.4 ms).
        {"low", {{"video.mode", "docked"}, {"video.resolution", "540"}, {"video.upscaler", "fsr"},
                 {"video.frame_gen", "off"}, {"audio.reverb", "0"}}},
    };
    return p;
}
const Preset* preset_named(const std::string& name) {
    for (const Preset& p : presets())
        if (name == p.name) return &p;
    return nullptr;
}

// "auto" (or no setting): the system's locale picks among the game's languages; Portuguese gets the pt-BR fan
// translation when it is installed (the game itself has no Portuguese), else English.
std::string system_game_language() {
    const char* vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    std::string loc;
    for (const char* v : vars)
        if (const char* e = std::getenv(v); e && *e && std::string(e) != "C" && std::string(e) != "POSIX") {
            loc = e;
            break;
        }
#if defined(__APPLE__)
    if (loc.empty()) loc = "en_US";
#endif
    std::string lang = loc.substr(0, loc.find_first_of("_-.@")), region;
    size_t us = loc.find_first_of("_-");
    if (us != std::string::npos) region = loc.substr(us + 1, 2);
    for (char& c : lang) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (lang == "pt") {
        std::error_code ec;
        bool ptbr = std::filesystem::is_directory(std::filesystem::u8path(plat::data_root() + "/mods/ptbr/romfs"), ec);
        return ptbr ? "pt-BR" : "en-US";
    }
    if (lang == "en") return region == "US" || region.empty() ? "en-US" : "en-GB";
    if (lang == "fr") return region == "CA" ? "fr-CA" : "fr";
    if (lang == "es") return region == "ES" || region.empty() ? "es" : "es-419";
    if (lang == "zh") return region == "TW" || region == "HK" || region == "MO" ? "zh-TW" : "zh-CN";
    for (const char* l : {"de", "it", "nl", "ru", "ja", "ko"})
        if (lang == l) return l;
    return "en-US";
}

}  // namespace

void config_load() {
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lk(g_lock);
    g_entries.clear();
    g_loaded = true;
    FILE* f = plat::fopen_utf8(path(), "rb");
    if (!f) return;
    std::string section, line;
    char buf[1024];
    while (std::fgets(buf, sizeof buf, f)) {
        line = trim(buf);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (!section.empty() && k.find('.') == std::string::npos) k = section + "." + k;
        if (ConfigEntry* e = find(k))
            e->value = v;
        else
            g_entries.push_back({k, v});
    }
    std::fclose(f);
    // A preset named in the file supplies the keys the file leaves out (in memory: written back only if changed).
    if (ConfigEntry* e = find("video.preset"))
        if (const Preset* p = preset_named(e->value))
            for (auto& [k, v] : p->keys)
                if (!find(k)) g_entries.push_back({k, v});
}

std::vector<std::pair<std::string, std::string>> config_preset(const std::string& name) {
    std::vector<std::pair<std::string, std::string>> out;
    if (const Preset* p = preset_named(name))
        for (auto& [k, v] : p->keys) out.push_back({k, v});
    return out;
}

void config_apply() {
    if (!g_loaded) config_load();
    mods_fetch();  // before the language and the frame rate look for them
    // Game language "auto": from the system, unless a language file or NX_LANGUAGE says otherwise. Only applied:
    // the entry keeps "auto", so the choice stays automatic.
    std::string game_lang = config_get("game.language", "auto");
    {
        std::error_code ec;
        bool file = std::filesystem::exists(std::filesystem::u8path(plat::data_root() + "/language.txt"), ec);
        if ((game_lang.empty() || game_lang == "auto") && !file && !std::getenv("NX_LANGUAGE")) {
            game_lang = system_game_language();
            log("config: game language from the system: %s", game_lang.c_str());
        }
    }
    std::lock_guard lk(g_lock);
    for (const Mapping& m : kMappings) {
        ConfigEntry* e = find(m.key);
        std::string value = std::string(m.key) == "game.language" ? game_lang : e ? e->value : std::string();
        // No frame rate chosen: 60 when the SpeedFix is there (the loader keeps 30 if the game's code lacks it).
        if (value.empty() && std::string(m.key) == "video.frame_rate" && speedfix_installed()) value = "60";
        if (value.empty()) continue;
        std::string v = m.map(value);
        if (std::string(m.env) == "NX_HANDHELD") {
            if (v == "0") unset_env("NX_HANDHELD");
            else if (v == "1") set_env_default("NX_HANDHELD", "1");
        } else if (std::string(m.env) == "NX_FSR") {
            if (v == "0") unset_env("NX_FSR");
            else if (v == "1") set_env_default("NX_FSR", "1");
        } else if (!v.empty()) {
            set_env_default(m.env, v);
        }
    }
    // The pt-BR translation is a mod: enable it with the language (game.mods can list more, comma separated).
    std::string mods;
    if (ConfigEntry* e = find("game.mods")) mods = e->value;
    if (game_lang == "pt-BR")
        mods = mods.empty() ? "ptbr" : (mods.find("ptbr") == std::string::npos ? mods + ",ptbr" : mods);
    if (const char* sf = std::getenv("NX_SPEEDFIX"); sf && sf[0] == '1')  // its parameter files go with its code
        mods = mods.empty() ? "speedfix60" : mods + ",speedfix60";
    if (!mods.empty()) set_env_default("NX_MODS", mods);
    log("config: %zu settings from %s", g_entries.size(), path().c_str());
}

bool config_save() {
    std::lock_guard lk(g_lock);
    // Grouped by section, sections in first-seen order.
    std::vector<std::string> sections;
    for (const auto& e : g_entries) {
        std::string s = e.key.substr(0, e.key.find('.'));
        if (std::find(sections.begin(), sections.end(), s) == sections.end()) sections.push_back(s);
    }
    std::string tmp = path() + ".tmp";
    FILE* f = plat::fopen_utf8(tmp, "wb");
    if (!f) return false;
    std::fprintf(f, "# ACNHRecomp settings (edited by the settings screen; F1 or Minus+Plus in game)\n");
    for (const auto& s : sections) {
        std::fprintf(f, "\n[%s]\n", s.c_str());
        for (const auto& e : g_entries)
            if (e.key.compare(0, s.size() + 1, s + ".") == 0)
                std::fprintf(f, "%s = %s\n", e.key.substr(s.size() + 1).c_str(), e.value.c_str());
    }
    bool ok = std::fclose(f) == 0;
    if (ok) {  // std::filesystem::rename replaces the old file on Windows too (std::rename fails there if it exists)
        std::error_code ec;
        std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path()), ec);
        ok = !ec;
    }
    return ok;
}

std::string config_get(const std::string& key, const std::string& fallback) {
    if (!g_loaded) config_load();
    std::lock_guard lk(g_lock);
    ConfigEntry* e = find(key);
    return e ? e->value : fallback;
}

int config_get_int(const std::string& key, int fallback) {
    std::string v = config_get(key);
    return v.empty() ? fallback : std::atoi(v.c_str());
}

float config_get_float(const std::string& key, float fallback) {
    std::string v = config_get(key);
    return v.empty() ? fallback : float(std::atof(v.c_str()));
}

bool config_get_bool(const std::string& key, bool fallback) {
    std::string v = config_get(key);
    if (v.empty()) return fallback;
    return v == "1" || v == "on" || v == "true" || v == "yes";
}

uint64_t config_generation() { return g_generation.load(std::memory_order_acquire); }

const char* config_env_override(const char* name) {
    const char* e = std::getenv(name);
    if (!e || !*e) return nullptr;
    std::lock_guard lk(g_lock);
    return g_env_from_config.count(name) ? nullptr : e;
}

bool config_show_fps() {
    static std::atomic<uint64_t> s_last_gen{0};
    static std::atomic<bool> s_cached{false};
    uint64_t gen = config_generation();
    if (s_last_gen.load(std::memory_order_relaxed) != gen) {
        bool val = false;
        std::string v = config_get("video.show_fps");
        if (!v.empty()) {
            val = (v == "1" || v == "on" || v == "true" || v == "yes");
        } else {
            const char* env = std::getenv("NX_SHOW_FPS");
            val = env && (std::string(env) == "1" || std::string(env) == "true" || std::string(env) == "on");
        }
        s_cached.store(val, std::memory_order_relaxed);
        s_last_gen.store(gen, std::memory_order_relaxed);
    }
    return s_cached.load(std::memory_order_relaxed);
}

bool config_is_handheld() {
    if (const char* e = std::getenv("NX_HANDHELD")) {
        return *e != '\0' && *e != '0' && std::string(e) != "false" && std::string(e) != "off";
    }
    std::string m = config_get("video.mode");
    return m == "handheld";
}

void config_set(const std::string& key, const std::string& value) {
    // The generation goes up once the value is in (below): a reader that sees the new one reads the new value.
    struct Bump {
        ~Bump() { g_generation.fetch_add(1, std::memory_order_acq_rel); }
    } bump;
    std::lock_guard lk(g_lock);
    if (ConfigEntry* e = find(key))
        e->value = value;
    else
        g_entries.push_back({key, value});
}

std::vector<ConfigEntry> config_all() {
    std::lock_guard lk(g_lock);
    return g_entries;
}

bool config_needs_restart(const std::string& key) {
    for (const Mapping& m : kMappings)
        if (key == m.key) return m.restart;
    return key == "game.mods" || key == "controls.glyphs" || key == "input.layout" || key == "video.preset";  // icons: read at boot
}

}  // namespace nx
