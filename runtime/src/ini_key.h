#pragma once
// The full key of a config.ini line, shared by the game's reader (config.cpp) and the launcher's (launcher/files.cpp).
#include <string>

namespace nx {

// `k` as read under `[section]`. Saving writes input.key.move_up as "key.move_up" under [input]; any other key with a
// dot is already a full key (video.frame_rate = 60 appended at the end of the file, as tools/setup_60fps.sh says).
// [key] and [key2] are what an older save made of input.key.* after reading it wrong: *legacy is set, and the caller
// keeps such a value only for a key nothing else set (the same file's [input] has the newer one).
inline std::string ini_full_key(const std::string& section, const std::string& k, bool* legacy) {
    *legacy = false;
    if (section.empty()) return k;
    if (section == "key" || section == "key2") {
        *legacy = true;
        return "input." + section + "." + k;
    }
    if (k.find('.') == std::string::npos) return section + "." + k;
    if (section == "input" && (k.rfind("key.", 0) == 0 || k.rfind("key2.", 0) == 0)) return "input." + k;
    return k;
}

}  // namespace nx
