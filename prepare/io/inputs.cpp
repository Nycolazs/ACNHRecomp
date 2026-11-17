// What the player gave to prepare the game folder (inputs.h).
#include "inputs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <system_error>

#include "io_util.h"
#include "nso.h"

namespace nx::prepare {

namespace fs = std::filesystem;
using detail::lower;
using detail::str;
using detail::u8;

namespace {

#include "prepare_versions.inc"  // kVersionsTxt: games/acnh/versions.txt, copied in by prepare/io/io.cmake

constexpr uint64_t kAocBase = kAcnhProgramId + 0x1000;  // add-ons: 01006F8002327xxx, xxx the index (hle_fs.cpp aoc/)
constexpr uint64_t kUpdateId = kAcnhProgramId + 0x800;

const char* const kRequiredModules[] = {"main", "sdk", "subsdk0"};
// The RomFS's top-level folders the game and the project read (docs/ACNH.md's shader table, tools/setup_acnh.py,
// tools/setup_60fps.sh, runtime/src/hle_movie.cpp, discord_presence.cpp). Names as the game opens them.
const char* const kRequiredDirs[] = {"Bcsv", "Effect", "Font", "Layout", "Message", "Model", "Movie", "Pack", "Shader",
                                     "System"};
// Files whose name the project knows, each read at boot: missing or empty, the RomFS is not whole.
const char* const kKeyFiles[] = {"Shader/Park_UBER_All.Nin_NX_NVN.szs", "System/GSys/gsys_resource.Nin_NX_NVN.release.szs",
                                 "Pack/StaticParam.pack", "Font/ScalableFont.sarc.zs", "Movie/OpeningMovie.webm"};
// Game packages: never read (no keys, no decryption here). ".xc0": the first part of a split XCI.
const char* const kContainerExts[] = {".nsp", ".xci", ".xc0", ".nsz", ".xcz", ".nca"};

struct Text {
    const char* key;
    Severity severity;
    const char* en;
};
const Text kTexts[] = {
    {"prepare.input.found", Severity::Info, "ExeFS: {} · RomFS: {}"},
    {"prepare.input.missing", Severity::Error, "{} does not exist"},
    {"prepare.input.not_folder", Severity::Error, "{} is a file: pick the folder your emulator extracted into"},
    {"prepare.input.container", Severity::Error,
     "{} is a game package (NSP, XCI, NSZ, XCZ or NCA). ACNHRecomp does not read those: extract the ExeFS and RomFS "
     "with your emulator (Ryujinx: right-click the game, Extract Data; yuzu or Eden: Dump RomFS, and Dump ExeFS in "
     "the debug settings), then pick the folders it wrote"},
    {"prepare.input.container_ignored", Severity::Warning,
     "{} is a game package (NSP, XCI, NSZ, XCZ or NCA): ACNHRecomp never reads those and leaves it where it is. The "
     "game is prepared from the extracted ExeFS and RomFS only (to extract them: Ryujinx, right-click the game, "
     "Extract Data; yuzu or Eden, Dump RomFS, and Dump ExeFS in the debug settings)"},
    {"prepare.input.layout", Severity::Error,
     "{} holds neither an ExeFS (main, sdk, subsdk0, main.npdm) nor a RomFS (Shader, Bcsv, ...), nor exefs and romfs "
     "folders"},
    {"prepare.input.no_exefs", Severity::Error,
     "No ExeFS was found: pick also the folder the ExeFS was extracted into (main, sdk, subsdk0, main.npdm)"},
    {"prepare.input.no_romfs", Severity::Error,
     "No RomFS was found: pick also the folder the RomFS was extracted into (Shader, Bcsv, ...)"},
    {"prepare.input.pick_pair", Severity::Error,
     "Pick one ExeFS folder and one RomFS folder: {} and {} are not one of each"},
    {"prepare.input.too_many", Severity::Error,
     "Pick one folder (holding the ExeFS and the RomFS) or two (the ExeFS, the RomFS); {} were picked"},
    {"prepare.exefs.missing", Severity::Error, "The ExeFS has no {}: extract the ExeFS again"},
    {"prepare.exefs.unreadable", Severity::Error, "ExeFS {} cannot be read"},
    {"prepare.exefs.not_nso", Severity::Error, "ExeFS {} is not one of the game's executables (no NSO header)"},
    {"prepare.exefs.truncated", Severity::Error, "ExeFS {} is cut short (a copy that stopped?): extract the ExeFS again"},
    {"prepare.exefs.bad_hash", Severity::Error,
     "ExeFS {}: its {} segment fails its SHA-256 check (a damaged file): extract the ExeFS again"},
    {"prepare.exefs.corrupt", Severity::Error, "ExeFS {} is damaged ({}): extract the ExeFS again"},
    {"prepare.exefs.optional_bad", Severity::Warning, "ExeFS {} is damaged ({}); the game does not need it"},
    {"prepare.exefs.npdm_missing", Severity::Error, "The ExeFS has no main.npdm: extract the ExeFS again"},
    {"prepare.exefs.npdm_invalid", Severity::Error, "ExeFS main.npdm is not a program description (no META and ACI0)"},
    {"prepare.exefs.other_game", Severity::Error,
     "This ExeFS is of another program ({}), not Animal Crossing: New Horizons (01006F8002326000)"},
    {"prepare.exefs.version", Severity::Info, "Animal Crossing: New Horizons {}"},
    {"prepare.exefs.version_302", Severity::Warning,
     "Version 3.0.2: still accepted for the game folders made from it, but ACNHRecomp is made for update 3.0.3. "
     "Install update 3.0.3 in your emulator and extract the ExeFS and RomFS again"},
    {"prepare.exefs.needs_303", Severity::Error,
     "This ExeFS is version {}: ACNHRecomp needs update 3.0.3. Install update 3.0.3 in your emulator, then extract the "
     "ExeFS and RomFS again (with the update applied)"},
    {"prepare.exefs.unknown_build", Severity::Error,
     "This ExeFS's main (build id {}) is no version of Animal Crossing: New Horizons ACNHRecomp knows: it needs update "
     "3.0.3. Install it in your emulator, then extract the ExeFS and RomFS again"},
    {"prepare.romfs.missing", Severity::Error, "The RomFS folder {} does not exist"},
    {"prepare.romfs.unreadable", Severity::Error, "The RomFS cannot be read: {}"},
    {"prepare.romfs.empty", Severity::Error, "The RomFS folder {} is empty"},
    {"prepare.romfs.skeleton", Severity::Error,
     "The RomFS has its folders but not its files' data ({} of {} files are empty): a \"skeleton\" dump. Dump the "
     "RomFS again, choosing Full"},
    {"prepare.romfs.not_acnh", Severity::Error,
     "{} is not Animal Crossing: New Horizons' RomFS (none of Shader, Bcsv, Message, ...)"},
    {"prepare.romfs.incomplete", Severity::Error, "The RomFS lacks folders the game needs ({}): extract the RomFS again"},
    {"prepare.romfs.key_file", Severity::Warning, "The RomFS has no {} (or it is empty): it may be incomplete"},
    {"prepare.romfs.few_files", Severity::Warning,
     "The RomFS has {} files ({}), a complete one about 103,000 (10.5 GB): it may be incomplete"},
    {"prepare.romfs.summary", Severity::Info, "RomFS: {} files, {}"},
    {"prepare.dlc.missing", Severity::Error, "The add-on folder {} does not exist"},
    {"prepare.dlc.unreadable", Severity::Error, "The add-on folder cannot be read: {}"},
    {"prepare.dlc.empty", Severity::Error, "The add-on folder {} is empty"},
    {"prepare.dlc.skeleton", Severity::Error,
     "The add-on's RomFS in {} has its folders but not its files' data (a \"skeleton\" dump): dump it again, choosing "
     "Full"},
    {"prepare.dlc.is_game", Severity::Error, "{} is the game's own content, not an add-on's"},
    {"prepare.dlc.other_title", Severity::Error,
     "{} is the content of another title ({}), not an add-on of Animal Crossing: New Horizons"},
    {"prepare.dlc.unknown_index", Severity::Warning,
     "Which add-on {} holds cannot be told from its folder's name: it is taken as add-on 1 (Happy Home Paradise, the "
     "game's one paid add-on). If it is another, name its folder after its title (01006F8002327xxx) or choose its "
     "number"},
    {"prepare.dlc.duplicate", Severity::Error, "{} and {} are both add-on {}"},
    {"prepare.dlc.summary", Severity::Info, "Add-on {}: {} files, {}"},
};

std::string fill(const char* tmpl, const std::vector<std::string>& args) {
    std::string out;
    size_t k = 0;
    for (const char* p = tmpl; *p; p++) {
        if (p[0] == '{' && p[1] == '}') {
            out += k < args.size() ? args[k] : std::string();
            k++, p++;
        } else {
            out += *p;
        }
    }
    return out;
}

std::string hex16(uint64_t v) {
    char b[20];
    std::snprintf(b, sizeof b, "%016llX", (unsigned long long)v);
    return b;
}

std::string thousands(uint64_t n) {
    std::string s = std::to_string(n), out;
    for (size_t i = 0; i < s.size(); i++) {
        if (i && (s.size() - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return out;
}

bool has_ext(const std::string& name, const char* const* exts, size_t n) {
    const std::string l = lower(name);
    for (size_t i = 0; i < n; i++) {
        const size_t m = std::strlen(exts[i]);
        if (l.size() > m && l.compare(l.size() - m, m, exts[i]) == 0) return true;
    }
    return false;
}
bool is_container(const std::string& name) { return is_game_package(name); }

// The entry of `dir` named `name` in any case ("exefs", "ExeFS"), of that kind; empty when none.
fs::path child(const fs::path& dir, const std::string& name, bool want_dir) {
    std::error_code ec;
    const fs::path exact = dir / u8(name);
    if (want_dir ? fs::is_directory(exact, ec) : fs::is_regular_file(exact, ec)) return exact;
    const std::string l = lower(name);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (lower(str(it->path().filename())) == l && (want_dir ? it->is_directory(ec) : it->is_regular_file(ec)))
            return it->path();
    return {};
}

bool looks_exefs(const fs::path& dir) { return !child(dir, "main", false).empty() || !child(dir, "main.npdm", false).empty(); }

size_t romfs_dirs_ci(const fs::path& dir) {
    size_t n = 0;
    for (const char* d : kRequiredDirs) n += !child(dir, d, true).empty();
    return n;
}

fs::path romfs_child(const fs::path& dir) {
    fs::path r = child(dir, "romfs", true);
    std::error_code ec;
    if (r.empty() && fs::is_directory(dir / "extract" / "romfs", ec)) r = dir / "extract" / "romfs";
    return r;
}

// Whether the file starts like a packed RomFS ("NXRP", version 1): a game folder prepared before.
bool is_romfs_pak(const fs::path& p) {
    FILE* f = detail::open_file(p, "rb");
    if (!f) return false;
    char h[8] = {};
    const bool ok = std::fread(h, 1, 8, f) == 8 && std::memcmp(h, "NXRP\x01\0\0\0", 8) == 0;
    std::fclose(f);
    return ok;
}

// 16 hex digits, the ids of titles (0100...), in one path component.
std::vector<uint64_t> title_ids(const std::string& component) {
    std::vector<uint64_t> out;
    size_t i = 0;
    while (i < component.size()) {
        size_t j = i;
        while (j < component.size() && std::isxdigit(static_cast<unsigned char>(component[j]))) j++;
        if (j - i == 16 && component.compare(i, 3, "010") == 0)
            out.push_back(std::stoull(component.substr(i, 16), nullptr, 16));
        i = j == i ? i + 1 : j;
    }
    return out;
}

struct Walk {
    uint64_t files = 0, dirs = 0, bytes = 0, empty = 0;
    std::vector<std::string> containers;  // the first few game packages met
    std::string error;
};

// Every file under `dir` (as romfs_pak.h packs them with skip_packages: links to folders not entered, game packages
// left out), but `exclude_root` at its top.
Walk walk(const fs::path& dir, const std::vector<std::string>& exclude_root) {
    Walk w;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& e = *it;
        const std::string name = str(e.path().filename());
        const bool excluded =
            it.depth() == 0 && std::find(exclude_root.begin(), exclude_root.end(), name) != exclude_root.end();
        std::error_code e2;
        if (is_container(name) && !excluded) {  // never read, nor packed (romfs_pak.h: skip_packages)
            if (w.containers.size() < 8) w.containers.push_back(str(e.path()));
            if (e.is_directory(e2)) it.disable_recursion_pending();
            continue;
        }
        if (e.is_directory(e2)) {
            if (excluded || e.is_symlink(e2)) it.disable_recursion_pending();
            if (!excluded) w.dirs++;
            continue;
        }
        if (excluded || !e.is_regular_file(e2)) continue;
        const uint64_t size = e.file_size(e2);
        w.files++, w.bytes += size, w.empty += size == 0;
    }
    if (ec) w.error = str(dir) + ": " + ec.message();
    return w;
}

// The index of a romfs.pak (hle_fs.cpp's Pak): path -> size.
bool read_pak_index(const fs::path& p, std::map<std::string, uint64_t>& files) {
    FILE* f = detail::open_file(p, "rb");
    if (!f) return false;
    std::unique_ptr<FILE, int (*)(FILE*)> hold(f, std::fclose);
    uint8_t h[12];
    if (std::fread(h, 1, 12, f) != 12 || std::memcmp(h, "NXRP\x01\0\0\0", 8) != 0) return false;
    const uint32_t count = detail::rd32(h + 8);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t len[2], tail[16];
        if (std::fread(len, 1, 2, f) != 2) return false;
        std::string path(size_t(len[0] | len[1] << 8), '\0');
        if (std::fread(path.data(), 1, path.size(), f) != path.size() || std::fread(tail, 1, 16, f) != 16) return false;
        files[path] = detail::rd64(tail + 8);
    }
    return true;
}

class Prober {
public:
    Prober(const ProbeOptions& opt, ProbeResult& r) : opt_(opt), r_(r) {}

    void add(const char* key, std::vector<std::string> args = {}, const std::string& path = "") {
        for (const Message& old : r_.messages)  // the same finding met twice (a folder that is both ExeFS and RomFS)
            if (old.key == key && old.path == path && old.args == args) return;
        Message m;
        m.key = key;
        const Text* t = nullptr;
        for (const Text& x : kTexts)
            if (std::strcmp(x.key, key) == 0) t = &x;
        m.severity = t ? t->severity : Severity::Error;
        m.text = fill(t ? t->en : key, args);
        m.args = std::move(args);
        m.path = path;
        r_.messages.push_back(std::move(m));
    }

    // A picked path: a file is refused (a package with the advice to extract it); the packages in a folder are only
    // said (check_containers).
    bool check_pick(const fs::path& p) {
        std::error_code ec;
        if (!fs::exists(p, ec)) return add("prepare.input.missing", {str(p)}, str(p)), false;
        if (!fs::is_directory(p, ec)) {
            if (is_container(str(p.filename()))) return add("prepare.input.container", {str(p.filename())}, str(p)), false;
            return add("prepare.input.not_folder", {str(p)}, str(p)), false;
        }
        check_containers(p);
        return true;
    }

    // The game packages at the top of `dir`: each said (a warning) and left alone, never opened. One lying beside the
    // extracted ExeFS and RomFS (where the player keeps the game) does not stop the preparation.
    void check_containers(const fs::path& dir) {
        if (dir.empty() || !checked_.insert(str(dir)).second) return;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string n = str(it->path().filename());
            if (is_container(n)) add("prepare.input.container_ignored", {n}, str(it->path()));
        }
    }

    void run(const std::vector<std::string>& paths) {
        std::vector<fs::path> game;
        std::vector<std::pair<std::string, int>> dlc;
        for (size_t i = 0; i < opt_.dlc.size(); i++)
            dlc.push_back({opt_.dlc[i], i < opt_.dlc_index.size() ? opt_.dlc_index[i] : -1});
        for (const std::string& s : paths) {
            uint64_t id = 0;
            dlc_index_from_path(s, &id);
            if ((id & ~0xFFFull) == kAocBase)
                dlc.push_back({s, -1});  // names an add-on's title: its content, whatever the order of the picks
            else
                game.push_back(u8(s));
        }
        bool picks_ok = true;
        for (const fs::path& p : game) picks_ok = check_pick(p) && picks_ok;
        if (game.empty()) add("prepare.input.no_exefs"), add("prepare.input.no_romfs");
        if (game.size() > 2) {
            add("prepare.input.too_many", {std::to_string(game.size())});
        } else if (picks_ok && game.size() == 1) {
            resolve_one(game[0]);
        } else if (picks_ok && game.size() == 2) {
            resolve_two(game[0], game[1]);
        }
        if (!exefs_.empty()) check_exefs();
        if (!romfs_.empty() || !pak_.empty()) check_romfs();
        if (!exefs_.empty() && (!romfs_.empty() || !pak_.empty()))
            add("prepare.input.found", {str(exefs_), str(romfs_.empty() ? pak_ : romfs_)});
        for (const auto& [d, index] : dlc) dlc_paths_.push_back({u8(d), index});
        check_dlc();
        r_.ok = r_.count(Severity::Error) == 0;
    }

private:
    // The ExeFS and RomFS of a parent folder; false when it holds neither.
    bool from_parent(const fs::path& d) {
        fs::path e = child(d, "exefs", true), ro = romfs_child(d);
        std::error_code ec;
        fs::path pak;
        if (ro.empty() && fs::is_regular_file(d / "romfs.pak", ec) && is_romfs_pak(d / "romfs.pak")) pak = d / "romfs.pak";
        if (e.empty() && ro.empty() && pak.empty()) return false;
        check_containers(d);
        exefs_ = e, romfs_ = ro, pak_ = pak;
        if (e.empty()) add("prepare.input.no_exefs");
        if (ro.empty() && pak.empty()) add("prepare.input.no_romfs");
        // Add-ons: aoc/<index>/ beside them (a game folder), or yuzu's dump/01006F8002327xxx/romfs beside the game's
        // dump/01006F8002326000/.
        if (fs::path aoc = child(d, "aoc", true); !aoc.empty())
            for (fs::directory_iterator it(aoc, ec), end; !ec && it != end; it.increment(ec)) {
                const std::string n = str(it->path().filename());
                if (it->is_directory(ec) && !n.empty() && n.find_first_not_of("0123456789") == std::string::npos)
                    dlc_paths_.push_back({it->path(), -1});
            }
        const std::vector<uint64_t> ids = title_ids(str(d.filename()));
        if (!ids.empty() && ids[0] == kAcnhProgramId) dump_addons(d.parent_path());
        return true;
    }

    // The add-ons' folders of an emulator's dump folder: 01006F8002327xxx/romfs.
    void dump_addons(const fs::path& dump) {
        std::error_code ec;
        for (fs::directory_iterator it(dump, ec), end; !ec && it != end; it.increment(ec)) {
            const std::vector<uint64_t> ids = title_ids(str(it->path().filename()));
            if (ids.size() == 1 && (ids[0] & ~0xFFFull) == kAocBase && it->is_directory(ec))
                dlc_paths_.push_back({it->path(), -1});
        }
    }

    void resolve_one(const fs::path& d) {
        // yuzu's or Eden's dump folder (or atmosphere/contents): the game's title folder in it.
        if (fs::path t = child(d, hex16(kAcnhProgramId), true); !t.empty() && from_parent(t)) {
            r_.layout = Layout::EmulatorDump;
            check_containers(d);
            return;
        }
        if (from_parent(d)) {
            r_.layout = Layout::Parent;
            return;
        }
        const bool exefs = looks_exefs(d);
        const size_t dirs = romfs_dirs_ci(d);
        if (exefs && dirs) {  // both in one folder: the ExeFS's files are not part of the RomFS
            r_.layout = Layout::Mixed;
            exefs_ = romfs_ = d;
            std::error_code ec;
            for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
                const std::string n = str(it->path().filename()), l = lower(n);
                if (it->is_regular_file(ec) && (l == "main" || l == "main.npdm" || l == "rtld" || l == "sdk" ||
                                                (l.compare(0, 6, "subsdk") == 0 && l.size() <= 7)))
                    r_.romfs.exclude_root.push_back(n);
            }
            return;
        }
        // The ExeFS or the RomFS of a parent folder that holds the other (dump/01006F8002326000/romfs).
        if (exefs || dirs) {
            const fs::path parent = d.parent_path();
            const fs::path other = exefs ? romfs_child(parent) : child(parent, "exefs", true);
            if (!other.empty() && from_parent(parent)) {
                r_.layout = Layout::Parent;
                return;
            }
            r_.layout = Layout::TwoPicks;
            (exefs ? exefs_ : romfs_) = d;
            add(exefs ? "prepare.input.no_romfs" : "prepare.input.no_exefs");
            return;
        }
        add("prepare.input.layout", {str(d)}, str(d));
    }

    void resolve_two(const fs::path& a, const fs::path& b) {
        const bool ea = looks_exefs(a) && !romfs_dirs_ci(a), eb = looks_exefs(b) && !romfs_dirs_ci(b);
        if (ea == eb) {
            add("prepare.input.pick_pair", {str(a), str(b)});
            return;
        }
        r_.layout = Layout::TwoPicks;
        exefs_ = ea ? a : b;
        romfs_ = ea ? b : a;
    }

    void module_problem(const ModuleInfo& m, const NsoStatus& st, bool required) {
        const std::string& n = m.name;
        if (!required) {
            add("prepare.exefs.optional_bad", {n, st.what}, m.path);
            return;
        }
        switch (st.error) {
            case NsoError::Io: add("prepare.exefs.unreadable", {n}, m.path); break;
            case NsoError::NotNso: add("prepare.exefs.not_nso", {n}, m.path); break;
            case NsoError::Truncated: add("prepare.exefs.truncated", {n}, m.path); break;
            case NsoError::Hash: add("prepare.exefs.bad_hash", {n, nso_segment_name(st.segment)}, m.path); break;
            default: add("prepare.exefs.corrupt", {n, st.what}, m.path); break;
        }
    }

    void check_module(const fs::path& p, const std::string& name, bool required) {
        ModuleInfo m;
        m.name = name;
        m.path = str(p);
        m.required = required;
        std::vector<uint8_t> data, image;
        std::error_code ec;
        m.file_size = fs::file_size(p, ec);
        NsoHeader h;
        NsoStatus st;
        if (opt_.verify_nso) {
            if (!read_file(m.path, data)) st = {NsoError::Io, -1, "cannot be read"};
            else st = nso_to_flat(data, image, &h);
        } else {
            st = header_only(m.path, h);
        }
        if (st.ok() || st.error == NsoError::Hash || st.error == NsoError::Truncated || st.error == NsoError::Decompress ||
            st.error == NsoError::BadLayout) {
            m.build_id = build_id_hex(h);
            m.image_size = h.image_size();
        }
        m.valid = st.ok();
        if (!st.ok()) module_problem(m, st, required);
        r_.modules.push_back(m);
    }

    // The header of an NSO, and whether its segments lie inside the file (verify_nso off).
    static NsoStatus header_only(const std::string& path, NsoHeader& h) {
        FILE* f = detail::open_file(u8(path), "rb");
        if (!f) return {NsoError::Io, -1, "cannot be read"};
        uint8_t b[kNsoHeaderSize];
        const size_t n = std::fread(b, 1, sizeof b, f);
        std::fclose(f);
        if (!parse_nso_header(b, n, h)) return {NsoError::NotNso, -1, "not an NSO"};
        std::error_code ec;
        const uint64_t size = fs::file_size(u8(path), ec);
        for (int i = 0; i < 3; i++)
            if (uint64_t(h.seg[i].file_offset) + h.seg[i].file_size > size)
                return {NsoError::Truncated, i, std::string(nso_segment_name(i)) + " segment past the end of the file"};
        return {};
    }

    void check_exefs() {
        r_.exefs = str(exefs_);
        check_containers(exefs_);
        for (const char* m : kRequiredModules) {
            const fs::path p = child(exefs_, m, false);
            if (p.empty()) {
                add("prepare.exefs.missing", {m}, str(exefs_));
                continue;
            }
            check_module(p, m, true);
        }
        for (int k = 1; k <= 9; k++) {  // the other subsdk modules, when the build has them
            const std::string n = "subsdk" + std::to_string(k);
            if (fs::path p = child(exefs_, n, false); !p.empty()) check_module(p, n, false);
        }
        // main.npdm: whose program this is.
        const fs::path np = child(exefs_, "main.npdm", false);
        if (np.empty()) {
            add("prepare.exefs.npdm_missing", {}, str(exefs_));
        } else {
            r_.npdm = str(np);
            std::vector<uint8_t> d;
            const bool read = read_file(r_.npdm, d, 1 << 20);
            const uint32_t aci = d.size() >= 0x80 ? detail::rd32(d.data() + 0x70) : 0;
            if (!read || d.size() < 0x80 || std::memcmp(d.data(), "META", 4) != 0 || uint64_t(aci) + 0x18 > d.size() ||
                std::memcmp(d.data() + aci, "ACI0", 4) != 0) {
                add("prepare.exefs.npdm_invalid", {}, r_.npdm);
            } else {
                r_.program_id = detail::rd64(d.data() + aci + 0x10);
                if (r_.program_id != kAcnhProgramId) add("prepare.exefs.other_game", {hex16(r_.program_id)}, r_.npdm);
            }
        }
        // main's build id: the version.
        for (const ModuleInfo& m : r_.modules)
            if (m.name == "main") r_.build_id = m.build_id;
        if (r_.build_id.empty()) return;
        for (const KnownVersion& v : known_versions(opt_.versions_text))
            if (v.build_id == r_.build_id) r_.version = v.version;
        const std::string main_path = r_.modules.empty() ? std::string() : r_.modules[0].path;
        if (r_.version == kSupportedVersion)
            add("prepare.exefs.version", {r_.version}, main_path);
        else if (r_.version == "3.0.2")
            add("prepare.exefs.version_302", {}, main_path);
        else if (!r_.version.empty())
            add("prepare.exefs.needs_303", {r_.version}, main_path);
        else
            add("prepare.exefs.unknown_build", {r_.build_id}, main_path);
    }

    void check_romfs() {
        RomfsInfo& info = r_.romfs;
        std::error_code ec;
        if (!pak_.empty()) {  // a game folder prepared before: its romfs.pak's index
            info.path = str(pak_);
            std::map<std::string, uint64_t> files;
            if (!read_pak_index(pak_, files)) return add("prepare.romfs.unreadable", {info.path}, info.path);
            std::set<std::string> top;
            for (auto& [p, size] : files) {
                info.files++, info.bytes += size, info.empty_files += size == 0;
                top.insert(p.substr(0, p.find('/')));
            }
            info.walked = true;
            for (const char* d : kRequiredDirs)
                if (!top.count(d)) info.missing_dirs.push_back(d);
            judge_romfs(info.missing_dirs.size() == std::size(kRequiredDirs), [&](const char* k) {
                auto it = files.find(k);
                return it != files.end() && it->second > 0;
            });
            return;
        }
        info.path = str(romfs_);
        if (!fs::is_directory(romfs_, ec)) return add("prepare.romfs.missing", {info.path}, info.path);
        check_containers(romfs_);
        if (fs::directory_iterator(romfs_, ec) == fs::directory_iterator()) return add("prepare.romfs.empty", {info.path}, info.path);
        for (const char* d : kRequiredDirs)
            if (!fs::is_directory(romfs_ / d, ec)) info.missing_dirs.push_back(d);
        const bool none = romfs_dirs_ci(romfs_) == 0;
        auto key_ok = [&](const char* k) {
            std::error_code e2;
            const fs::path p = romfs_ / u8(k);
            return fs::is_regular_file(p, e2) && fs::file_size(p, e2) > 0;
        };
        if (opt_.walk_romfs) {
            Walk w = walk(romfs_, info.exclude_root);
            if (!w.error.empty()) return add("prepare.romfs.unreadable", {w.error}, info.path);
            for (const std::string& c : w.containers) add("prepare.input.container_ignored", {str(u8(c).filename())}, c);
            info.files = w.files, info.dirs = w.dirs, info.bytes = w.bytes, info.empty_files = w.empty;
            info.walked = true;
        } else {
            // Its top only: a skeleton shows in the files the boot needs being empty.
            for (const char* k : kKeyFiles) {
                const fs::path p = romfs_ / u8(k);
                std::error_code e2;
                if (fs::is_regular_file(p, e2)) key_present_++, key_empty_ += fs::file_size(p, e2) == 0;
            }
            info.skeleton = key_present_ && key_empty_ == key_present_;
        }
        judge_romfs(none, key_ok);
    }

    template <class KeyOk>
    void judge_romfs(bool none_of_the_dirs, KeyOk key_ok) {
        RomfsInfo& info = r_.romfs;
        if (info.walked) {
            if (info.files == 0 && info.dirs == 0) return add("prepare.romfs.empty", {info.path}, info.path);
            info.skeleton = info.files == 0 || info.empty_files * 2 > info.files;
        }
        if (info.skeleton)  // counted over the tree, or over the files the boot needs (walk_romfs off)
            return add("prepare.romfs.skeleton", {thousands(info.walked ? info.empty_files : key_empty_),
                                                  thousands(info.walked ? info.files : key_present_)},
                       info.path);
        if (none_of_the_dirs) return add("prepare.romfs.not_acnh", {info.path}, info.path);
        if (!info.missing_dirs.empty()) {
            std::string list;
            for (const std::string& d : info.missing_dirs) list += (list.empty() ? "" : ", ") + d;
            add("prepare.romfs.incomplete", {list}, info.path);
            return;
        }
        for (const char* k : kKeyFiles)
            if (!key_ok(k)) add("prepare.romfs.key_file", {k}, info.path);
        if (!info.walked) return;
        if ((opt_.expect_files && info.files < opt_.expect_files) || (opt_.expect_bytes && info.bytes < opt_.expect_bytes))
            add("prepare.romfs.few_files", {thousands(info.files), detail::human_size(info.bytes)}, info.path);
        add("prepare.romfs.summary", {thousands(info.files), detail::human_size(info.bytes)}, info.path);
    }

    void check_dlc() {
        std::set<std::string> seen;
        std::map<int, std::string> by_index;
        for (const auto& [picked, chosen] : dlc_paths_) {
            std::error_code ec;
            const std::string ps = str(picked);
            const std::string canon = str(fs::weakly_canonical(picked, ec));
            if (!seen.insert(canon.empty() ? ps : canon).second) continue;
            if (!fs::is_directory(picked, ec)) {
                if (fs::exists(picked, ec) && is_container(str(picked.filename())))
                    add("prepare.input.container", {str(picked.filename())}, ps);
                else
                    add("prepare.dlc.missing", {ps}, ps);
                continue;
            }
            check_containers(picked);
            DlcInfo d;
            const fs::path rom = child(picked, "romfs", true);  // dump/01006F8002327001/romfs
            const fs::path dir = rom.empty() ? picked : rom;
            d.path = str(dir);
            d.index = dlc_index_from_path(d.path, &d.title_id);
            if (d.title_id && (d.title_id & ~0xFFFull) != kAocBase) {
                if (d.title_id == kAcnhProgramId || d.title_id == kUpdateId)
                    add("prepare.dlc.is_game", {d.path}, d.path);
                else
                    add("prepare.dlc.other_title", {d.path, hex16(d.title_id)}, d.path);
                continue;
            }
            // The game's RomFS picked as an add-on (the same folder, or a copy: every folder the game needs and its
            // main shader).
            std::error_code e2;
            const bool same = !romfs_.empty() && fs::equivalent(dir, romfs_, e2);
            if (same || (romfs_dirs_ci(dir) == std::size(kRequiredDirs) && fs::is_regular_file(dir / u8(kKeyFiles[0]), e2))) {
                add("prepare.dlc.is_game", {d.path}, d.path);
                continue;
            }
            if (d.index == 0) d.index = -1;  // 01006F8002327000 is no add-on
            if (chosen > 0) d.index = chosen;  // the player's choice wins over the path
            Walk w = walk(dir, {});
            if (!w.error.empty()) {
                add("prepare.dlc.unreadable", {w.error}, d.path);
                continue;
            }
            for (const std::string& c : w.containers) add("prepare.input.container_ignored", {str(u8(c).filename())}, c);
            d.files = w.files, d.bytes = w.bytes;
            if (w.files == 0 && w.dirs == 0) {
                add("prepare.dlc.empty", {d.path}, d.path);
                continue;
            }
            if (w.files == 0 || w.empty * 2 > w.files) {
                add("prepare.dlc.skeleton", {d.path}, d.path);
                continue;
            }
            if (d.index < 0) {  // ACNH has one paid add-on (Happy Home Paradise, 01006F8002327001)
                add("prepare.dlc.unknown_index", {d.path}, d.path);
                d.index = 1;
            }
            if (auto it = by_index.find(d.index); it != by_index.end()) {
                add("prepare.dlc.duplicate", {it->second, d.path, std::to_string(d.index)}, d.path);
                continue;
            } else {
                by_index[d.index] = d.path;
                add("prepare.dlc.summary", {std::to_string(d.index), thousands(d.files), detail::human_size(d.bytes)},
                    d.path);
            }
            r_.dlc.push_back(d);
        }
    }

    const ProbeOptions& opt_;
    ProbeResult& r_;
    fs::path exefs_, romfs_, pak_;
    uint64_t key_present_ = 0, key_empty_ = 0;  // kKeyFiles there, and empty (walk_romfs off)
    std::vector<std::pair<fs::path, int>> dlc_paths_;  // and the index the player chose (-1: from the path)
    std::set<std::string> checked_;  // folders whose top was checked for packages
};

}  // namespace

const char* layout_name(Layout l) {
    switch (l) {
        case Layout::Parent: return "parent";
        case Layout::EmulatorDump: return "emulator dump";
        case Layout::TwoPicks: return "two picks";
        case Layout::Mixed: return "mixed";
        default: return "none";
    }
}

bool ProbeResult::has(const std::string& key) const { return find(key) != nullptr; }

const Message* ProbeResult::find(const std::string& key) const {
    for (const Message& m : messages)
        if (m.key == key) return &m;
    return nullptr;
}

size_t ProbeResult::count(Severity s) const {
    return size_t(std::count_if(messages.begin(), messages.end(), [s](const Message& m) { return m.severity == s; }));
}

std::vector<KnownVersion> known_versions(const std::string& text) {
    const std::string& t = text.empty() ? std::string(kVersionsTxt) : text;
    std::vector<KnownVersion> out;
    size_t at = 0;
    while (at < t.size()) {
        size_t nl = t.find('\n', at);
        if (nl == std::string::npos) nl = t.size();
        std::string line = t.substr(at, nl - at);
        at = nl + 1;
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        char id[64] = {}, version[64] = {};
        if (std::sscanf(line.c_str(), "%63s %63s", id, version) != 2 || std::strlen(id) != 32) continue;
        std::string b = id;
        for (char& c : b) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (b.find_first_not_of("0123456789ABCDEF") != std::string::npos) continue;
        out.push_back({b, version});
    }
    return out;
}

int dlc_index_from_path(const std::string& path, uint64_t* title_id) {
    std::vector<std::string> parts;
    for (const fs::path& c : u8(path))
        if (!str(c).empty() && str(c) != "/" && str(c) != "\\") parts.push_back(str(c));
    if (title_id) *title_id = 0;
    if (parts.empty()) return -1;
    // The folder's own name, or its parent's when it is a romfs/ (dump/01006F8002327001/romfs): a title further up is
    // only where the player keeps things (ACNH [01006F8002326000]/DLC).
    size_t i = parts.size() - 1;
    if (i > 0 && lower(parts[i]) == "romfs" && title_ids(parts[i]).empty()) i--;
    for (uint64_t id : title_ids(parts[i])) {
        if (title_id) *title_id = id;
        return (id & ~0xFFFull) == kAocBase ? int(id & 0xFFF) : -1;
    }
    const std::string& n = parts.back();  // a game folder's aoc/<index>/
    if (parts.size() > 1 && lower(parts[parts.size() - 2]) == "aoc" && !n.empty() && n.size() <= 4 &&
        n.find_first_not_of("0123456789") == std::string::npos)
        return std::stoi(n);
    return -1;
}

bool is_game_package(const std::string& name) {
    return has_ext(name, kContainerExts, sizeof kContainerExts / sizeof *kContainerExts);
}

ProbeResult probe(const std::vector<std::string>& paths, const ProbeOptions& opt) {
    ProbeResult r;
    Prober(opt, r).run(paths);
    return r;
}

}  // namespace nx::prepare
