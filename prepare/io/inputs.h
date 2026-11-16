// What the player gave to prepare the game folder: the ExeFS and RomFS they extracted from their own copy of Animal
// Crossing: New Horizons with update 3.0.3 (an emulator's "Extract ExeFS / RomFS" or "Dump RomFS"), and optionally
// the RomFS of add-on content. probe() finds them in the folders picked and says everything that is wrong, before
// anything is written.
//
// ACNHRecomp never reads NSP, XCI, NSZ, XCZ or NCA files and never uses console keys: such a file picked is refused
// with the advice to extract the ExeFS and RomFS with the emulator; one lying in a folder picked (beside the extracted
// ExeFS and RomFS, where players keep the game) is a warning, left alone, never opened and never packed. Nothing here
// decrypts.
//
// Layouts accepted:
//   Parent        one folder holding exefs/ and romfs/ (any case; or extract/romfs/), and aoc/<index>/ for add-ons;
//                 also yuzu's, Eden's and their forks' dump folder of the game, <dump>/01006F8002326000/ (Dump RomFS
//                 writes its romfs/, the "Dump ExeFS" setting its exefs/).
//   EmulatorDump  yuzu's or Eden's dump folder itself (or atmosphere/contents/), holding 01006F8002326000/ and the
//                 add-ons' 01006F8002327xxx/romfs/.
//   TwoPicks      an ExeFS folder (main, sdk, subsdk0, main.npdm at its top) and a RomFS folder (Shader/, Bcsv/...
//                 at its top), in either order: Ryujinx's "Extract Data > ExeFS / RomFS" write into the folder chosen.
//                 A pick of one of them whose parent holds the other (dump/01006F8002326000/romfs) finds it too.
//   Mixed         one folder holding both (Ryujinx's ExeFS and RomFS extracted into the same folder): the ExeFS's
//                 files are left out of the RomFS (RomfsInfo::exclude_root).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nx::prepare {

constexpr uint64_t kAcnhProgramId = 0x01006F8002326000ull;
constexpr const char* kSupportedVersion = "3.0.3";

enum class Severity { Info, Warning, Error };  // Warning: preparing still works, the player is told something

// One finding. `key` is stable (for translating later; the launcher's i18n uses "{}" for each argument, in order);
// `text` is the English sentence with `args` filled in.
struct Message {
    Severity severity = Severity::Info;
    std::string key;
    std::vector<std::string> args;
    std::string text;
    std::string path;  // the file or folder it is about, "" none
};

enum class Layout { None, Parent, EmulatorDump, TwoPicks, Mixed };
const char* layout_name(Layout l);

struct ModuleInfo {
    std::string name;  // "main", "sdk", "subsdk0", ... as the ExeFS names them (analysis/<name>.bin)
    std::string path;
    bool required = false;  // main, sdk, subsdk0: the game's code
    bool valid = false;     // an NSO whose segments decompress and match their SHA-256 (or header only, verify_nso off)
    std::string build_id;   // 32 upper-case hex digits
    uint64_t file_size = 0;
    uint64_t image_size = 0;  // its analysis/<name>.bin
};

struct RomfsInfo {
    std::string path;
    uint64_t files = 0, dirs = 0, bytes = 0;
    uint64_t empty_files = 0;  // 0 bytes
    bool walked = false;       // the counts are of the whole tree (ProbeOptions::walk_romfs)
    bool skeleton = false;     // folders without the files' data ("Skeleton" dump mode)
    std::vector<std::string> missing_dirs;    // top-level folders the game reads that are not there
    std::vector<std::string> exclude_root;    // Layout::Mixed: the ExeFS's files at its top, not RomFS
};

struct DlcInfo {
    std::string path;   // the add-on's RomFS folder
    // aoc/<index>/ in the game folder: the player's choice (ProbeOptions::dlc_index), else from the path, else 1 (ACNH
    // has one paid add-on, Happy Home Paradise) with the warning prepare.dlc.unknown_index
    int index = -1;
    uint64_t title_id = 0;  // from the path, 0 when it names none
    uint64_t files = 0, bytes = 0;
};

struct ProbeResult {
    bool ok = false;  // no Error: the game folder can be prepared from this
    Layout layout = Layout::None;
    std::string exefs;  // the ExeFS folder
    std::vector<ModuleInfo> modules;  // main, sdk, subsdk0 first, then the other subsdk* (and rtld) found
    std::string npdm;                 // main.npdm
    uint64_t program_id = 0;          // its ACI0's
    std::string build_id;             // main's
    std::string version;              // main's, from games/acnh/versions.txt; "" when not listed
    RomfsInfo romfs;
    std::vector<DlcInfo> dlc;
    std::vector<Message> messages;

    bool has(const std::string& key) const;
    const Message* find(const std::string& key) const;
    size_t count(Severity s) const;
};

struct ProbeOptions {
    std::vector<std::string> dlc;  // folders the player picked as add-on content (one RomFS each)
    std::vector<int> dlc_index;    // the index the player chose for dlc[i] (1-4095; -1 or none: from the path)
    bool verify_nso = true;        // decompress the modules and check their SHA-256 (else the headers only)
    bool walk_romfs = true;        // count every RomFS file (seconds on a phone's shared storage); else its top only
    // A complete RomFS of 3.0.x has about 103,000 files and 10.5 GB (docs/ACNH.md): fewer is said, as a warning (a
    // copy that stopped half way). 0 turns the check off.
    uint64_t expect_files = 93000;
    uint64_t expect_bytes = 9400000000ull;
    std::string versions_text;  // games/acnh/versions.txt's text; "" the copy built into the library
};

// `paths`: what the player picked, one folder (a parent, a dump folder, or one holding everything) or two (the ExeFS
// and the RomFS, in either order). A path that names an add-on's title (01006F8002327xxx) counts as add-on content.
ProbeResult probe(const std::vector<std::string>& paths, const ProbeOptions& opt = {});

// versions.txt's lines: build id -> version. `text` "" = the built-in copy.
struct KnownVersion {
    std::string build_id, version;
};
std::vector<KnownVersion> known_versions(const std::string& text = "");

// Whether a file's name is a game package's (.nsp, .xci, .xc0, .nsz, .xcz, .nca, any case): never read.
bool is_game_package(const std::string& name);

// The add-on index a path names: the last three hex digits of a 01006F8002327xxx title id in the folder's name (or
// its parent's, for <title>/romfs), else <...>/aoc/<n>; -1 when none. `title_id` gets the title found (0 none).
int dlc_index_from_path(const std::string& path, uint64_t* title_id = nullptr);

}  // namespace nx::prepare
