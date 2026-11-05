// Texts of the settings screen (Dear ImGui) in the languages it offers. This is the menu's own language, not the
// game's: the game's language is game.language in config.ini (config.cpp), read once at boot by the am/set services.
#pragma once

#include <string>
#include <vector>

namespace nx::ui {

struct UiLanguage {
    const char* code;         // "en", "pt-BR", "zh-TW", ...
    const char* native_name;  // the language's name in itself, so a player can find theirs whatever the menu shows
};

// In display order, English first.
const std::vector<UiLanguage>& ui_languages();

// The best match among ui_languages() for the OS's preferred languages; "en" if none. With SDL3 this queries the OS,
// which SDL says is not thread safe: call it from the main thread.
std::string ui_detect_system_language();

// "auto" or "" = ui_detect_system_language(). An unknown code falls back to the closest offered language, else "en".
void ui_set_language(const std::string& code);

// The code in use.
const std::string& ui_language();

// The translated UTF-8 text; falls back to English, then to the key itself (so a missing key shows up on screen).
// The pointer stays valid for the whole run. Safe to call from any thread.
const char* tr(const char* key);

// Every menu string of `code` (its native name included), for the font atlas. `code` null visits every language.
void ui_visit_texts(const char* code, void (*fn)(const char* utf8, void* ctx), void* ctx);

// tr() with the first "{}" replaced by arg.
std::string trf(const char* key, const std::string& arg);

}  // namespace nx::ui
