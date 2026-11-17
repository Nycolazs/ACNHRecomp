// prepare.h: the stages, on one worker thread. Each stage writes its files as <name>.part and renames them once whole,
// and .import/complete goes before a stage replaces or deletes anything a finished folder holds (uncomplete()), so a
// preparation cut short (cancelled, a crash, a full disk, a rename refused) leaves nothing that is taken for complete;
// run again on the same folder, it keeps what is right and does the rest.
#include "prepare.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "acnh_data.h"
#include "gamelib.h"
#include "io_util.h"
#include "nso.h"
#include "recomp.h"
#include "romfs_pak.h"
#include "spirv_pak.h"

namespace prep {

namespace fs = std::filesystem;
using nx::prepare::detail::str;
using nx::prepare::detail::u8;
namespace np = nx::prepare;

namespace {

const char* kTitle = "01006F8002326000";  // Animal Crossing: New Horizons (the application)

// The hash of recomp/*.py the C++ recompiler matches (prepare/recomp/SOURCE, by CMake): gen/recomp_source.txt, which
// tools/setup_acnh.py checks before it takes generated code from a game folder.
const char* kRecompSource = NX_RECOMP_SOURCE;

// The share of the whole each stage takes, roughly as long as it lasts on a desktop (Compile for one platform).
constexpr double kWeight[] = {0, 0.02, 0.02, 0.18, 0.12, 0.16, 0.48, 0.02, 0};

struct Job {
    std::mutex m;
    std::thread th;
    std::atomic<bool> cancel{false};
    Status st;  // st.log: the lines not yet taken by status()
    bool stage_marked = false;  // the current stage's start is in the log
};
Job& job() {
    static Job j;
    return j;
}

// The error of a stage, or a refusal: thrown, caught at the top of the worker.
struct Failure {
    int rc;
    std::string what;
};
[[noreturn]] void refuse(const std::string& what) { throw Failure{2, what}; }
[[noreturn]] void fail(const std::string& what) { throw Failure{1, what}; }

// ---- files ------------------------------------------------------------------------------------------------------

// A relative path as the folder's indexes write it: UTF-8, '/' separated.
std::string generic(const fs::path& p) {
    const std::u8string g = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(g.data()), g.size());
}

// Whether the file starts like a spirv.pak ("SPK1") with at least one shader in it.
bool is_spirv_pak(const fs::path& p) {
    FILE* f = np::detail::open_file(p, "rb");
    if (!f) return false;
    uint8_t h[8];
    const bool ok = std::fread(h, 1, 8, f) == 8 && std::memcmp(h, "SPK1", 4) == 0 && np::detail::rd32(h + 4) > 1;
    std::fclose(f);
    return ok;
}

bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec) && !ec;
}

// Whether `p` is `dir` or inside it (both made absolute and normal first).
bool within(const fs::path& p, const fs::path& dir) {
    std::error_code ec;
    const fs::path a = fs::weakly_canonical(fs::absolute(p, ec), ec), d = fs::weakly_canonical(fs::absolute(dir, ec), ec);
    auto ai = a.begin(), di = d.begin();
    for (; di != d.end(); ++di, ++ai) {
        if (di->empty()) continue;  // the trailing "" of a path ending in a separator
        if (ai == a.end() || *ai != *di) return false;
    }
    return true;
}

void write_text_atomic(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path part = p.u8string() + u8".part";
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f.flush()) fail("cannot write " + str(part));
    }
    fs::rename(part, p, ec);
    if (ec) fail("cannot write " + str(p) + ": " + ec.message());
}

void write_bytes_atomic(const fs::path& p, const std::vector<uint8_t>& b) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path part = p.u8string() + u8".part";
    FILE* f = np::detail::open_file(part, "wb");
    if (!f) fail("cannot write " + str(part));
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    if (std::fclose(f) != 0 || !ok) {
        fs::remove(part, ec);
        fail("cannot write " + str(part) + " (the disk full?)");
    }
    fs::rename(part, p, ec);
    if (ec) fail("cannot write " + str(p) + ": " + ec.message());
}

// The mark of a folder the preparation made in the game folder and replaces, empties and deletes as it goes (gen/,
// spirv/). It touches no such folder it did not make: in place, the folder of an extraction may be tools/setup_acnh.py's
// data folder, whose gen/ is a developer's NX_GEN_DIR and whose spirv/ its shaders.
const char* const kOwnMark = ".acnhrecomp_prepare";
void mark_own(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir / kOwnMark, ec))
        write_text_atomic(dir / kOwnMark, "made by ACNHRecomp's preparation (prepare/), which deletes or replaces what is in it\n");
}

// A file copied as <to>.part, then renamed; `tick` after each MiB.
void copy_file_atomic(const fs::path& from, const fs::path& to, const std::atomic<bool>& cancel,
                      const std::function<void(uint64_t)>& tick = {}) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    const fs::path part = to.u8string() + u8".part";
    FILE* in = np::detail::open_file(from, "rb");
    if (!in) fail("cannot read " + str(from));
    FILE* out = np::detail::open_file(part, "wb");
    if (!out) {
        std::fclose(in);
        fail("cannot write " + str(part));
    }
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (;;) {
        if (cancel) {
            ok = false;
            break;
        }
        size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if (n == 0) {
            ok = !std::ferror(in);
            break;
        }
        if (std::fwrite(buf.data(), 1, n, out) != n) {
            ok = false;
            break;
        }
        if (tick) tick(n);
    }
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    if (!ok) {
        fs::remove(part, ec);
        if (cancel) throw Failure{4, "cancelled"};
        fail("cannot copy " + str(from) + " to " + str(to) + " (the disk full?)");
    }
    fs::rename(part, to, ec);
    if (ec) fail("cannot write " + str(to) + ": " + ec.message());
}

std::string read_text(const fs::path& p, size_t max = 1 << 20) {
    std::ifstream f(p, std::ios::binary);
    std::string s;
    if (!f) return s;
    s.resize(max);
    f.read(s.data(), std::streamsize(max));
    s.resize(size_t(f.gcount()));
    return s;
}

std::string tail_of(const fs::path& p, size_t max) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return "";
    const auto size = uint64_t(f.tellg());
    const uint64_t from = size > max ? size - max : 0;
    f.seekg(std::streamoff(from));
    std::string s(size_t(size - from), '\0');
    f.read(s.data(), std::streamsize(s.size()));
    return s;
}

// The last non-empty line of a log (the shader tool's progress).
std::string last_line(const fs::path& p) {
    std::string t = tail_of(p, 400);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    const size_t nl = t.find_last_of("\r\n");
    return nl == std::string::npos ? t : t.substr(nl + 1);
}

// The build id game.ini gives, "" when there is none: read before it is written again, it is the main of the folder's
// last preparation, which libraries from before build.txt were built from.
std::string folder_build_id(const fs::path& out) {
    std::istringstream in(read_text(out / "game.ini", 64 << 10));
    std::string id;
    for (std::string ln; std::getline(in, ln);)
        if (ln.rfind("build_id = ", 0) == 0) {
            id = ln.substr(11);
            while (!id.empty() && (id.back() == '\r' || id.back() == ' ')) id.pop_back();
        }
    return id == "unknown" ? "" : id;
}

// Files and bytes under a folder (game packages left out, as the probe and the pack do).
struct Tree {
    std::vector<std::pair<fs::path, uint64_t>> files;  // relative path, size
    uint64_t bytes = 0;
};
Tree tree_of(const fs::path& dir) {
    Tree t;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (np::is_game_package(str(it->path().filename()))) {
            if (it->is_directory(e2)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(e2)) {
            if (it->is_symlink(e2)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(e2)) continue;
        const uint64_t size = it->file_size(e2);
        t.files.push_back({it->path().lexically_relative(dir), size});
        t.bytes += size;
    }
    if (ec) fail("cannot read " + str(dir) + ": " + ec.message());
    std::sort(t.files.begin(), t.files.end());
    return t;
}

// ---- ACNH's data ------------------------------------------------------------------------------------------------

// tmouh's SpeedFix for this build of main (prepare/recomp/acnh/SPEEDFIX.md), "" for a version it was not made for
std::string speedfix_for(const std::string& build_id) {
    const char* file = build_id == "FCD2BB238ABE99E925B4E452DF1F41F1"   ? "speedfix-3.0.2.pchtxt"
                       : build_id == "FF1D1C05670DB6021C85B624A710B963" ? "speedfix-3.0.3.pchtxt"
                                                                        : nullptr;
    if (!file) return "";
    auto it = rc::acnh_data().find(file);
    return it == rc::acnh_data().end() ? "" : it->second;
}

// The hooks of main for this build id ("" when the version is not known: recompiled without hooks, as setup_acnh.py
// does), and the hooks file's name in ACNHRecomp (games/acnh/...).
void hooks_for(const std::string& build_id, std::string& text, std::string& repo_path) {
    std::istringstream in(rc::acnh_data().at("versions.txt"));
    std::string ln;
    while (std::getline(in, ln)) {
        if (ln.empty() || ln[0] == '#') continue;
        std::istringstream ls(ln);
        std::string id, version, hooks;
        ls >> id >> version >> hooks;
        if (id != build_id || hooks.empty() || hooks == "-") continue;
        std::string file = hooks.substr(hooks.rfind('/') + 1);
        auto it = rc::acnh_data().find(file);
        if (it == rc::acnh_data().end()) return;
        text = it->second, repo_path = hooks;
        return;
    }
}

// ---- the worker -------------------------------------------------------------------------------------------------

class Runner {
public:
    explicit Runner(const Options& o) : o_(o), cancel_(job().cancel) {}

    void run() {
        check();
        exefs();
        const bool built = !fs::exists(gen_ / "main" / "table.cpp") && chosen_libraries_built();
        bool variant = o_.variant_60fps;
        if (built) {
            log("every platform chosen has its library built from this main for this ACNHRecomp: kept");
        } else if (skip_recompile_) {
            log("recompile skipped (NX_PREPARE_SKIP_RECOMP): " +
                std::string(fs::exists(gen_ / "main" / "table.cpp") ? "the code in gen/ is built as it is"
                                                                     : "no code in gen/, no library is built"));
        } else {
            int rc = recompile_all(variant, false);
            if (rc != 0 && !cancel_ && variant && variant_applies()) {  // the 60 FPS variant would not build: without it
                log("the 60 FPS variant could not be recompiled (" + err_ + "): main without it");
                variant = false;
                err_.clear();
                rc = recompile_all(false, false);
            }
            if (rc) throw Failure{rc, err_};
        }
        shaders();
        packs();
        if (!built && fs::exists(gen_ / "main" / "table.cpp")) {
            int rc = compile_all(false);
            // Not when the toolchain itself is at fault: without the variant it fails the same way after a second full
            // cycle, or, if the fault was passing, the libraries come out without 60 FPS and are kept so.
            const bool toolchain = err_.find(gl::kToolchainFault) != std::string::npos;
            if (rc != 0 && rc != 4 && !cancel_ && !toolchain && variant && !skip_recompile_ && variant_applies()) {
                log("compiling with the 60 FPS variant failed (" + err_.substr(0, 300) + "): main again without it");
                err_.clear();
                rc = recompile_all(false, true);
                if (rc == 0) rc = compile_all(true);
            }
            if (rc) throw Failure{rc, err_};
        }
        finish();
    }

private:
    // Whether main of this build gets the 60 FPS variant: a version whose hooks and SpeedFix are known (3.0.2, 3.0.3).
    bool variant_applies() const {
        std::string text, path;
        hooks_for(build_id_, text, path);
        return !path.empty() && !speedfix_for(build_id_).empty();
    }

    // ---- status ----
    void stage(Stage s, const std::string& item = "") {
        if (cancel_) throw Failure{4, "cancelled"};
        std::lock_guard<std::mutex> l(job().m);
        if (job().st.stage != s || !job().stage_marked) push(LogLine{s, ""});
        job().stage_marked = true;
        job().st.stage = s;
        job().st.fraction = 0;
        job().st.item = item;
        job().st.overall = overall(s, 0);
    }
    static double overall(Stage s, double f) {
        double before = 0;
        for (int k = 0; k < int(s); k++) before += kWeight[k];
        return std::min(1.0, before + kWeight[int(s)] * std::clamp(f, 0.0, 1.0));
    }
    void progress(double f, const std::string& item = "") {
        std::lock_guard<std::mutex> l(job().m);
        Status& st = job().st;
        st.fraction = std::clamp(f, 0.0, 1.0);
        st.overall = overall(st.stage, f);
        if (!item.empty()) st.item = item;
    }
    static void push(LogLine line) {  // with job().m held
        auto& lg = job().st.log;
        lg.push_back(std::move(line));
        if (lg.size() > 2000) lg.erase(lg.begin(), lg.begin() + long(lg.size() - 2000));
    }
    static void log(const std::string& s) {
        std::lock_guard<std::mutex> l(job().m);
        push(LogLine{job().st.stage, s});
    }

    // ---- Check ----
    void check() {
        stage(Stage::Check, "inputs");
        skip_recompile_ = o_.skip_recompile;
        if (const char* e = std::getenv("NX_PREPARE_SKIP_RECOMP"); e && e[0] == '1') skip_recompile_ = true;
        np::ProbeOptions po = o_.probe;
        po.dlc = o_.dlc;
        po.dlc_index = o_.dlc_index;
        r_ = np::probe(o_.inputs, po);
        {
            std::lock_guard<std::mutex> l(job().m);
            job().st.messages = r_.messages;
        }
        std::string errors;
        for (const np::Message& m : r_.messages) {
            const char* tag = m.severity == np::Severity::Error ? "error: " : m.severity == np::Severity::Warning ? "warning: " : "";
            log(tag + m.text);
            if (m.severity == np::Severity::Error) errors += (errors.empty() ? "" : "\n") + m.text;
        }
        if (!r_.ok) refuse(errors.empty() ? "the game's files cannot be prepared" : errors);
        build_id_ = r_.build_id, version_ = r_.version;
        {
            std::lock_guard<std::mutex> l(job().m);
            job().st.build_id = build_id_, job().st.version = version_;
        }

        // The game folder, and whether the RomFS is used where it is.
        std::error_code ec;
        const fs::path romfs = u8(r_.romfs.path);
        romfs_pak_ = fs::is_regular_file(romfs, ec);  // a game folder prepared before: its romfs.pak
        fs::path out = u8(o_.out);
        if (out.empty()) {
            if (!o_.in_place) refuse("no game folder given: choose where the game folder goes");
            out = romfs.parent_path();
            if (str(out.filename()) == "extract") out = out.parent_path();
        }
        out_ = fs::weakly_canonical(fs::absolute(out, ec), ec);
        {
            std::lock_guard<std::mutex> l(job().m);
            job().st.out = str(out_);
        }
        gen_ = out_ / "gen";
        // What the folder held before: the last preparation's main (game.ini), and whether that one ended.
        previous_build_ = folder_build_id(out_);
        complete_before_ = fs::exists(out_ / ".import" / "complete", ec);
        const bool here = same_file(romfs, out_ / "romfs") || same_file(romfs, out_ / "extract" / "romfs") ||
                          same_file(romfs, out_ / "romfs.pak");
        if (o_.in_place && !here)
            refuse("in place, the RomFS (" + r_.romfs.path + ") must be the game folder's romfs/ (or extract/romfs/): " +
                   "it is not in " + str(out_) +
                   (str(romfs.filename()) != "romfs" && str(romfs.parent_path()) == str(out_)
                        ? " (rename its folder to romfs)" : "") +
                   ". Prepare the game folder somewhere else instead: the RomFS is then packed into its romfs.pak");
        if (here && !o_.in_place) log("the RomFS is in the game folder already: used where it is (in place)");
        in_place_ = here;
        if (romfs_pak_ && !here)
            refuse(r_.romfs.path + " is the romfs.pak of another game folder: pick the extracted RomFS folder instead");
        // the folder's own romfs.pak as the RomFS: only one packed for this main (a change of version cut short leaves
        // the new ExeFS beside the old romfs.pak)
        if (romfs_pak_) {
            const std::string id = made_for("romfs.pak");
            if (id != build_id_)
                refuse(str(out_ / "romfs.pak") + " was packed from the RomFS of " +
                       (id.empty() ? std::string("a preparation that did not end")
                                   : "another version of the game (" + id + ")") +
                       ", not of " + (version_.empty() ? build_id_ : version_) +
                       ": pick the extracted RomFS folder of this version to prepare the game folder");
            // a folder from before the record (made_for fell back to game.ini): recorded now, so a later stage cut
            // short (an add-on added, uncomplete) doesn't leave it refused as "a preparation that did not end"
            std::error_code rec;
            if (id == build_id_ && !fs::exists(out_ / ".import" / "romfs.pak.build_id", rec)) made("romfs.pak");
        }
        if (!romfs_pak_ && !here && within(out_, romfs))
            refuse("the game folder " + str(out_) + " cannot be inside the RomFS (" + r_.romfs.path + ")");
        if (!r_.exefs.empty() && same_file(out_, u8(r_.exefs)))
            refuse("the game folder cannot be the ExeFS folder itself (" + r_.exefs + "): choose another folder");
        if (fs::exists(out_, ec) && !fs::is_directory(out_, ec)) refuse(str(out_) + " is a file, not a folder");
        if (fs::is_directory(out_, ec) && !here && !fs::is_empty(out_, ec) && !fs::exists(out_ / "game.ini", ec) &&
            !fs::exists(out_ / ".import", ec) && !fs::exists(out_ / "exefs", ec))
            refuse(str(out_) + " holds other things: choose an empty folder for the game (or one prepared before)");
        // gen/ and spirv/ are rewritten and deleted by the stages: only those the preparation made (in place too)
        for (const char* d : {"gen", "spirv"})
            if (fs::exists(out_ / d, ec) && !made_here(out_ / d))
                refuse(str(out_ / d) + " was not made by ACNHRecomp's preparation, which would overwrite and delete it " +
                       "(a folder of tools/setup_acnh.py?): move it out of " + str(out_) +
                       ", or prepare the game folder somewhere else");
        fs::create_directories(out_, ec);
        if (ec) refuse("cannot make the game folder " + str(out_) + ": " + ec.message());

        // The platforms, the tools, the space.
        targets_ = o_.platforms;
        if (targets_.empty() && !gl::host_target().empty()) targets_.push_back(gl::host_target());
        if (targets_.empty()) refuse("no platform to build the game's code for: choose one (" + all_targets() + ")");
        for (const auto& t : targets_)
            if (!gl::find_target(t)) refuse("no such platform: " + t + " (" + all_targets() + ")");
        log("platforms: " + join(targets_));
        bool compile = !chosen_libraries_built() || fs::exists(gen_ / "main" / "table.cpp");
        if (skip_recompile_ && !fs::exists(gen_ / "main" / "table.cpp")) compile = false;
        if (compile) {
            if (o_.zig.empty()) refuse("the toolchain (zig) that compiles the game's code is missing");
            if (std::string why = gl::check_toolchain(o_.zig, o_.zig_lib); !why.empty()) refuse(why);
        }
        // The shaders depend on the RomFS and on main (the tool extracts from analysis/main.bin too): kept only when
        // converted for this main.
        const bool converted = fs::exists(out_ / "spirv" / "summary.txt", ec) || is_spirv_pak(out_ / "spirv.pak");
        const std::string shaders_for = converted ? made_for("shaders") : "";
        shaders_done_ = converted && shaders_for == build_id_;
        if (!shaders_done_) {
            if (o_.shader_tool.empty() || !fs::is_regular_file(u8(o_.shader_tool), ec))
                refuse("the shader tool (spirv_harness) is missing" +
                       (o_.shader_tool.empty() ? std::string() : ": " + o_.shader_tool));
            if (romfs_pak_)
                refuse("the shaders are converted from the extracted RomFS: pick its folder (this game folder has "
                       "only romfs.pak, and no shaders converted for this version)");
        }
        if (converted && !shaders_done_)
            log("the shaders were converted for " +
                (shaders_for.empty() ? std::string("a preparation that did not end")
                                     : "another version of the game (" + shaders_for + ")") +
                ": converted again");
        const uint64_t need = space_needed(r_, targets_.size(), in_place_, !shaders_done_, compile).need;
        uint64_t have_already = 0;
        if (fs::is_regular_file(out_ / "romfs.pak", ec)) have_already += fs::file_size(out_ / "romfs.pak", ec);
        const fs::space_info sp = fs::space(out_, ec);
        if (!ec && sp.available != uintmax_t(-1) && sp.available + have_already < need)
            refuse("not enough free space for the game folder: " + np::detail::human_size(need) + " needed, " +
                   np::detail::human_size(sp.available) + " free on " + str(out_));

        // Whether the folder stays complete (it does while only libraries are added; a stage that replaces what it
        // holds removes the mark first, uncomplete(); a folder taking another version's files is not complete until
        // it is done).
        if (complete_before_ && previous_build_ != build_id_) {
            fs::remove(out_ / ".import" / "complete", ec);
            log("the game folder held another version of the game (" + previous_build_ + "): prepared again");
        }
        progress(1);
    }

    // Whether the preparation made this folder of the game folder (gen/, spirv/): its mark, the recompile stage's
    // gen/recomp_source.txt (this preparation's or ACNHRecomp-Importer's), or a folder a preparation finished before
    // (game.ini naming ACNHRecomp, .import/: the Importer's and this one's before the mark).
    bool made_here(const fs::path& dir) const {
        std::error_code ec;
        if (fs::exists(dir / kOwnMark, ec)) return true;
        if (dir.filename() == "gen" && fs::exists(dir / "recomp_source.txt", ec)) return true;
        std::string first = read_text(out_ / "game.ini", 400);
        first = first.substr(0, first.find('\n'));
        return first.find("ACNHRecomp") != std::string::npos && fs::is_directory(out_ / ".import", ec);
    }

    // The build id of the main the folder's romfs.pak or shaders were made for: .import/<what>.build_id, written with
    // them (made()); for a folder from before those records, the main of its last preparation (game.ini) when that one
    // ended (.import/complete). "" when not known: made again.
    std::string made_for(const std::string& what) const {
        std::error_code ec;
        const fs::path rec = out_ / ".import" / (what + ".build_id");
        if (fs::exists(rec, ec)) {
            std::istringstream in(read_text(rec, 200));
            std::string id;
            in >> id;
            return id;
        }
        return complete_before_ ? previous_build_ : "";
    }
    void made(const std::string& what) {
        write_text_atomic(out_ / ".import" / (what + ".build_id"), build_id_ + "\n");
    }
    void unmade(const std::string& what) {
        std::error_code ec;
        fs::remove(out_ / ".import" / (what + ".build_id"), ec);
    }

    // Before a stage replaces or deletes what a finished game folder holds (an add-on, romfs.pak, the shaders): the
    // folder is no longer complete until finish() says so again.
    void uncomplete(const std::string& what) {
        std::error_code ec;
        if (fs::remove(out_ / ".import" / "complete", ec))
            log(what + ": the game folder is taken as complete again once this preparation ends");
    }

    // ---- ExeFS ----
    void exefs() {
        stage(Stage::ExeFS);
        std::error_code ec;
        const fs::path ex = out_ / "exefs", an = out_ / "analysis";
        fs::create_directories(ex, ec);
        fs::create_directories(an, ec);
        size_t k = 0;
        for (const np::ModuleInfo& m : r_.modules) {
            progress(double(k++) / double(r_.modules.size() + 1), m.name);
            if (m.name == "rtld") continue;  // the runtime does not load rtld (HLE): neither in exefs/ nor analysis/
            if (!m.valid) {  // a damaged optional module: the probe said so, the game does not need it
                log("exefs: " + m.name + " left out (damaged)");
                continue;
            }
            const fs::path src = u8(m.path), dst = ex / u8(m.name);
            if (!same_file(src, dst)) copy_file_atomic(src, dst, cancel_);
            std::vector<uint8_t> image;
            np::NsoHeader h;
            np::NsoStatus st = np::nso_file_to_flat(str(dst), image, &h);
            if (!st.ok()) fail("exefs/" + m.name + ": " + st.what);
            // the loader maps it whole and checks its size against the recompiled table's data segment end
            if (image.size() != uint64_t(h.seg[2].mem_offset) + h.seg[2].size)
                fail("exefs/" + m.name + ": its data segment does not end its image");
            write_bytes_atomic(an / u8(m.name + ".bin"), image);
            log("exefs: analysis/" + m.name + ".bin (" + std::to_string(image.size()) + " bytes)");
            if (cancel_) throw Failure{4, "cancelled"};
        }
        const fs::path npdm = u8(r_.npdm);
        if (!same_file(npdm, ex / "main.npdm")) copy_file_atomic(npdm, ex / "main.npdm", cancel_);
        copy_file_atomic(ex / "main.npdm", an / "main.npdm", cancel_);
        progress(1);
    }

    // ---- Recompile ----
    // The game's code as C++ (ACNHRecomp's recompile stage): gen/{sdk,subsdk0,main}, with the profile.txt and
    // recomp_source.txt that tools/setup_acnh.py checks before using it.
    int recompile_all(bool with_variant, bool main_only) {
        stage(Stage::Recompile, "main");
        std::string hooks_text, hooks_path;
        hooks_for(build_id_, hooks_text, hooks_path);
        // The native 60 FPS: main gets a second body for every function the SpeedFix changes (ACNHRecomp plays it
        // when its frame rate is 60; at 30 it runs the game's own code)
        const std::string speedfix = with_variant && !hooks_path.empty() ? speedfix_for(build_id_) : "";
        const fs::path speedfix_file = gen_ / "speedfix_main.pchtxt";
        struct Mod {
            const char* name;
            uint64_t base;
        } mods[] = {{"sdk", 0x4010000000ull}, {"subsdk0", 0x4018000000ull}, {"main", 0x4000000000ull}};
        const double weight[] = {0.08, 0.04, 0.88};  // main is ~90% of the functions
        double before = 0;
        try {
            fs::create_directories(gen_);
            mark_own(gen_);
            std::error_code ec;
            fs::remove_all(gen_ / "split", ec);  // regrouped from the files written now (gamelib split())
            fs::remove(speedfix_file, ec);
            if (!speedfix.empty()) write_text_atomic(speedfix_file, speedfix);
            for (int k = 0; k < 3; k++) {
                const Mod& m = mods[k];
                if (main_only && std::string(m.name) != "main") {
                    before += weight[k];
                    continue;
                }
                rc::RecompOptions ro;
                ro.nso = str(out_ / "exefs" / m.name);
                ro.out_dir = str(gen_ / m.name);
                ro.name = m.name;
                ro.base = m.base;
                ro.cancel = &cancel_;
                const bool main = std::string(m.name) == "main";
                if (main) ro.hooks_text = hooks_text;
                if (main && !speedfix.empty()) ro.variant = str(speedfix_file);
                ro.log = [](const std::string& s) { log(s); };
                ro.progress = [this, before, w = weight[k], name = m.name](uint64_t done, uint64_t total) {
                    progress(before + w * (total ? double(done) / double(total) : 1.0), name);
                };
                progress(before, m.name);
                rc::recompile(ro);
                std::string profile = main && !hooks_path.empty() ? "--hooks " + hooks_path : "none";
                if (main && !ro.variant.empty()) profile += " --variant " + str(speedfix_file);
                write_text_atomic(gen_ / m.name / "profile.txt", profile + "\n");
                before += weight[k];
            }
            write_text_atomic(gen_ / "recomp_source.txt", std::string(kRecompSource) + "\n");
        } catch (const Failure&) {
            throw;
        } catch (const std::exception& e) {
            if (cancel_) return err_ = "cancelled (prepare again to continue where it stopped)", 4;
            err_ = std::string("recompile: ") + e.what();
            return 1;
        }
        return 0;
    }

    // ---- Shaders ----
    // The shaders, from the RomFS and main to SPIR-V (ACNHRecomp's shader stage, its translator built native):
    // spirv/<hash>.spv, results.tsv, summary.txt.
    void shaders() {
        stage(Stage::Shaders);
        std::error_code ec;
        if (shaders_done_) {
            if (!fs::exists(out_ / ".import" / "shaders.build_id", ec)) made("shaders");  // a folder from before it
            log("shaders: converted before, kept");
            return;
        }
        const unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
        const fs::path work = out_ / ".import" / "shaders_extracted", lg = out_ / ".import" / "shaders.log";
        uncomplete("the shaders");
        // what an earlier conversion left (of another version, or cut short) goes: spirv/ (the preparation's, check()),
        // spirv.pak
        unmade("shaders");
        fs::remove_all(work, ec);
        fs::remove_all(out_ / "spirv", ec);
        fs::remove(out_ / "spirv.pak", ec);
        fs::create_directories(out_ / ".import", ec);
        fs::create_directories(out_ / "spirv", ec);
        mark_own(out_ / "spirv");
        // extracted from the RomFS and main, then translated (spirv_harness --prepare)
        const std::vector<std::string> argv = {o_.shader_tool, "--prepare", r_.romfs.path, str(out_ / "analysis" / "main.bin"),
                                               "--out", str(work), "--spirv", str(out_ / "spirv"), "--jobs",
                                               std::to_string(jobs), "--no-val"};
        std::atomic<bool> done{false};
        int rc = -1;
        std::thread t([&] {
            rc = gl::run(argv, str(lg), {}, &cancel_);
            done = true;
        });
        while (!done) {  // the tool's last line as the item
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (std::string ln = last_line(lg); !ln.empty()) progress(0.5, ln.substr(0, 200));
        }
        t.join();
        if (cancel_) throw Failure{4, "cancelled"};
        if (rc != 0) fail("the shader tool failed (" + std::to_string(rc) + "; log: " + str(lg) + "):\n" + tail_of(lg, 1500));
        size_t spv = 0;
        for (const auto& e : fs::directory_iterator(out_ / "spirv", ec))
            spv += e.path().extension() == ".spv";
        if (!fs::exists(out_ / "spirv" / "summary.txt", ec) || spv == 0)
            fail("the shader tool converted no shader (log: " + str(lg) + "): the game would draw nothing");
        fs::remove_all(work, ec);
        made("shaders");
        std::string summary = tail_of(out_ / "spirv" / "summary.txt", 200);
        while (!summary.empty() && (summary.back() == '\n' || summary.back() == '\r')) summary.pop_back();
        log("shaders: " + std::to_string(spv) + " converted; " + summary);
        progress(1);
    }

    // ---- Packs ----
    void packs() {
        stage(Stage::Packs);
        std::error_code ec;
        const double romfs_share = in_place_ ? 0 : 0.85, dlc_share = in_place_ ? 0.9 : 0.1;
        if (!in_place_) romfs_pak(romfs_share);
        add_ons(romfs_share, dlc_share);
        spirv_pak();
        progress(1);
    }

    void romfs_pak(double share) {
        std::error_code ec;
        const fs::path pak = out_ / "romfs.pak";
        np::RomfsPakLayout l;
        std::string e;
        if (!np::romfs_pak_layout(r_.romfs.path, l, e, r_.romfs.exclude_root, true)) fail("romfs.pak: " + e);
        const std::string id = made_for("romfs.pak");
        if (fs::is_regular_file(pak, ec) && fs::file_size(pak, ec) == l.size && pak_count(pak) == l.files.size() &&
            id == build_id_) {
            if (!fs::exists(out_ / ".import" / "romfs.pak.build_id", ec)) made("romfs.pak");
            log("romfs.pak: written before from this RomFS (" + std::to_string(l.files.size()) + " files), kept");
            return;
        }
        progress(0, "romfs.pak");
        uncomplete("romfs.pak");
        unmade("romfs.pak");
        np::RomfsPakOptions po;
        po.exclude_root = r_.romfs.exclude_root;
        po.skip_packages = true;
        po.cancel = &cancel_;
        po.progress = [&](const np::PakProgress& p) {
            progress(share * (p.bytes_total ? double(p.bytes_done) / double(p.bytes_total) : 1), "romfs.pak");
        };
        np::PakResult pr = np::write_romfs_pak(r_.romfs.path, str(pak), po);
        if (pr.cancelled) throw Failure{4, "cancelled"};
        if (!pr.ok) fail("romfs.pak: " + pr.error);
        made("romfs.pak");
        log("romfs.pak: " + std::to_string(pr.files) + " files, " + np::detail::human_size(pr.bytes));
    }

    static uint64_t pak_count(const fs::path& p) {
        FILE* f = np::detail::open_file(p, "rb");
        if (!f) return UINT64_MAX;
        uint8_t h[12];
        const bool ok = std::fread(h, 1, 12, f) == 12 && std::memcmp(h, "NXRP\x01\0\0\0", 8) == 0;
        std::fclose(f);
        return ok ? np::detail::rd32(h + 8) : UINT64_MAX;
    }

    // Add-on content: aoc/<index>/, a copy of each add-on's RomFS (written as aoc/<index>.part/, then renamed).
    void add_ons(double before, double share) {
        std::error_code ec;
        uint64_t total = 0, done = 0;
        for (const np::DlcInfo& d : r_.dlc) total += d.bytes;
        for (const np::DlcInfo& d : r_.dlc) {
            const fs::path src = u8(d.path), dst = out_ / "aoc" / std::to_string(d.index);
            const fs::path part = out_ / "aoc" / (std::to_string(d.index) + ".part");
            const std::string name = "aoc/" + std::to_string(d.index);
            fs::remove_all(part, ec);  // of a preparation cut short
            if (same_file(src, dst)) {
                log(name + ": in the game folder already");
                index_aoc(d.index);
                done += d.bytes;
                continue;
            }
            const Tree from = tree_of(src);
            if (fs::is_directory(dst, ec) && tree_of(dst).files == from.files) {
                log(name + ": copied before (" + std::to_string(from.files.size()) + " files), kept");
                index_aoc(d.index);
                done += d.bytes;
                continue;
            }
            for (const auto& [rel, size] : from.files) {
                copy_file_atomic(src / rel, part / rel, cancel_, [&](uint64_t n) {
                    done += n;
                    progress(before + share * (total ? double(done) / double(total) : 1), name);
                });
            }
            // the folder's copy replaced (removed first: a folder is renamed over no other on Windows)
            uncomplete(name);
            fs::remove_all(dst, ec);
            if (!ec) fs::rename(part, dst, ec);
            if (ec) fail("cannot write " + str(dst) + ": " + ec.message());
            log(name + ": " + std::to_string(from.files.size()) + " files, " + np::detail::human_size(from.bytes) +
                " (add-on " + std::to_string(d.index) + ")");
            index_aoc(d.index);
        }
    }

    // spirv.pak from spirv/, then the .spv files it holds go (results.tsv and summary.txt stay: the record of the
    // conversion). With no .spv left (packed before), spirv.pak is kept as it is.
    void spirv_pak() {
        std::error_code ec;
        const fs::path dir = out_ / "spirv", pak = out_ / "spirv.pak";
        np::SpirvPakLayout l;
        std::string e;
        if (!fs::is_directory(dir, ec) && is_spirv_pak(pak)) return log("spirv.pak: written before, kept");
        if (!np::spirv_pak_layout(str(dir), l, e)) fail("spirv.pak: " + e);
        size_t spv = 0;
        for (const auto& f : l.files) spv += f.key != "results.tsv";
        if (spv == 0) {
            if (!is_spirv_pak(pak)) fail("spirv.pak: no converted shader in " + str(dir));
            log("spirv.pak: written before, kept");
            return;
        }
        progress(0.95, "spirv.pak");
        uncomplete("spirv.pak");
        np::PakOptions po;
        po.cancel = &cancel_;
        np::PakResult pr = np::write_spirv_pak(str(dir), str(pak), po);
        if (pr.cancelled) throw Failure{4, "cancelled"};
        if (!pr.ok) fail("spirv.pak: " + pr.error);
        for (const auto& f : l.files)
            if (f.key != "results.tsv") fs::remove(u8(f.source), ec);
        log("spirv.pak: " + std::to_string(spv) + " shaders, " + np::detail::human_size(pr.bytes));
    }

    // ---- Compile ----
    bool chosen_libraries_built() const {
        if (targets_.empty()) return false;
        for (const auto& t : targets_)
            if (!gl::library_current(str(out_), t, build_id_)) return false;
        return true;
    }

    // The game's code for each chosen platform: game/<platform>/lib<game>_game (gamelib.h), then the recompiled
    // sources go (the player needs the library only). A chosen platform whose library is already built from this
    // main for this runtime is kept, unless every one must be built again (all: main recompiled without the 60 FPS
    // variant, so that the platforms do not end up some with it and some without).
    int compile_all(bool all) {
        stage(Stage::Compile);
        const bool own_cache = o_.cache.empty();
        const std::string cache = own_cache ? str(out_ / ".import" / "zig-cache") : o_.cache;
        const size_t n = targets_.size();
        for (size_t k = 0; k < n; k++) {
            {
                std::lock_guard<std::mutex> l(job().m);
                job().st.platform = int(k + 1), job().st.platforms = int(n);
            }
            if (!all && gl::library_current(str(out_), targets_[k], build_id_)) {  // from an earlier preparation
                log(targets_[k] + ": its library is already built from this game's main, kept");
                continue;
            }
            gl::Options g;
            g.zig = o_.zig, g.zig_lib = o_.zig_lib, g.cache = cache;
            g.gen = str(gen_), g.out = str(out_), g.work = str(out_ / ".import" / ("build-" + targets_[k]));
            g.target = targets_[k];
            g.build_id = build_id_;
            g.jobs = o_.jobs;
            g.cancel = &cancel_;
            g.log = [](const std::string& s) { log(s); };
            const std::string tag = targets_[k] + " (" + std::to_string(k + 1) + "/" + std::to_string(n) + ")";
            g.progress = [this, k, n, tag](uint64_t done, uint64_t total, const std::string& item) {
                progress((double(k) + (total ? double(done) / double(total) : 0)) / double(n), tag + ": " + item);
            };
            progress(double(k) / double(n), tag);
            std::string e;
            int rc = gl::build(g, e);
            if (rc) return err_ = (rc == 4 ? std::string("cancelled") : "compile (" + targets_[k] + "): " + e), rc;
            log(targets_[k] + ": " + gl::find_target(targets_[k])->lib_name + " built");
            std::error_code ec;
            if (own_cache) fs::remove_all(cache, ec);  // its objects, of no use to the next platform
        }
        // A platform not chosen this time whose library was built from another main (the folder prepared again with
        // another update) would crash its player: it goes, and that platform needs preparing again.
        std::error_code ec;
        for (const gl::Target& t : gl::targets()) {
            if (!fs::exists(out_ / "game" / t.name / t.lib_name, ec)) continue;
            std::string id = gl::library_build_id(str(out_), t.name);
            if (id.empty()) id = previous_build_;  // no build.txt (the Importer's 1.0.6 and earlier): the last main
            if (id.empty() || id == build_id_ || build_id_.empty() || id == "unknown") continue;
            fs::remove_all(out_ / "game" / t.name, ec);
            log(std::string(t.name) + ": its library was built from another version of the game (" + id + "), removed");
        }
        fs::remove_all(gen_, ec);  // the libraries are built: the C++ is no longer needed
        if (own_cache) fs::remove_all(cache, ec);
        progress(1);
        return 0;
    }

    // ---- Finish ----
    // .import/index.tsv's lines for the loose files of the game in the folder (the RomFS in place, the add-ons):
    // hle_fs.cpp says one of them gone from the folder in nx.log. "kind \t size \t crc \t path", no CRC here.
    void index_aoc(int index) {
        const std::string base = "aoc/" + std::to_string(index);
        for (const auto& [rel, size] : tree_of(out_ / u8(base)).files)
            index_ += "romfs\t" + std::to_string(size) + "\t-\t" + base + "/" + generic(rel) + "\n";
    }

    void finish() {
        stage(Stage::Finish);
        std::error_code ec;
        if (in_place_ && !romfs_pak_) {
            const fs::path romfs = u8(r_.romfs.path);
            const std::string base = same_file(romfs, out_ / "romfs") ? "romfs" : "extract/romfs";
            for (const auto& [rel, size] : tree_of(romfs).files)
                index_ += "romfs\t" + std::to_string(size) + "\t-\t" + base + "/" + generic(rel) + "\n";
        }
        write_text_atomic(out_ / ".import" / "index.tsv", index_);
        std::string ini = "# ACNHRecomp game folder, prepared by ACNHRecomp from the game's own extracted ExeFS and RomFS\n";
        ini += std::string("title = ") + kTitle + "\n";
        if (!version_.empty()) ini += "version = " + version_ + "\n";
        ini += "build_id = " + build_id_ + "\n";
        write_text_atomic(out_ / "game.ini", ini);
        uint64_t files = in_place_ ? r_.romfs.files : pak_count(out_ / "romfs.pak");
        write_text_atomic(out_ / ".import" / "complete", "ACNHRecomp prepare " + build_id_ + "\n" + std::to_string(files) +
                                                            " romfs files" + (in_place_ ? " (in place)" : " (romfs.pak)") + "\n");
        std::string libs;
        for (const auto& t : targets_)
            if (gl::library_current(str(out_), t, build_id_)) libs += (libs.empty() ? "" : ", ") + t;
        log("done: " + str(out_) + " (" + (version_.empty() ? build_id_ : version_) + "; " +
            (libs.empty() ? std::string("no library") : "the game's code for " + libs) + ")");
        progress(1);
    }

    static std::string join(const std::vector<std::string>& v) {
        std::string s;
        for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
        return s.empty() ? "none" : s;
    }
    static std::string all_targets() {
        std::string s;
        for (const gl::Target& t : gl::targets()) s += (s.empty() ? "" : ", ") + std::string(t.name);
        return s;
    }

    const Options& o_;
    std::atomic<bool>& cancel_;
    np::ProbeResult r_;
    fs::path out_, gen_;
    std::vector<std::string> targets_;
    std::string build_id_, version_, previous_build_, err_, index_;
    bool in_place_ = false, romfs_pak_ = false, shaders_done_ = false, skip_recompile_ = false, complete_before_ = false;
};

void worker(Options o) {
    int rc = 0;
    std::string error;
    try {
        Runner(o).run();
    } catch (const Failure& f) {
        rc = f.rc, error = f.what;
    } catch (const std::exception& e) {
        rc = 1, error = e.what();
    }
    if (rc && job().cancel) rc = 4, error = "cancelled";
    std::lock_guard<std::mutex> l(job().m);
    Status& st = job().st;
    if (rc == 0) st.stage = Stage::Done, st.fraction = 1, st.overall = 1, st.item.clear();
    st.rc = rc;
    st.error = error;
    if (rc) st.log.push_back(LogLine{st.stage, rc == 4 ? std::string("cancelled") : "error: " + error});
    st.running = false;
    st.finished = true;
}

}  // namespace

const char* stage_name(Stage s) {
    switch (s) {
        case Stage::Idle: return "idle";
        case Stage::Check: return "check";
        case Stage::ExeFS: return "exefs";
        case Stage::Recompile: return "recompile";
        case Stage::Shaders: return "shaders";
        case Stage::Packs: return "packs";
        case Stage::Compile: return "compile";
        case Stage::Finish: return "finish";
        case Stage::Done: return "done";
    }
    return "?";
}

bool start(const Options& o) {
    Job& j = job();
    std::lock_guard<std::mutex> l(j.m);
    if (j.st.running) return false;
    if (j.th.joinable()) j.th.join();  // the last one, finished
    j.cancel = false;
    j.st = Status{};
    j.stage_marked = false;
    j.st.running = true;
    j.th = std::thread(worker, o);
    return true;
}

Status status() {
    Job& j = job();
    std::lock_guard<std::mutex> l(j.m);
    Status s = j.st;
    j.st.log.clear();
    return s;
}

void cancel() { job().cancel = true; }

void wait() {
    Job& j = job();
    std::thread t;
    {
        std::lock_guard<std::mutex> l(j.m);
        if (!j.th.joinable()) return;
        t = std::move(j.th);
    }
    t.join();
}

Space space_needed(const np::ProbeResult& r, size_t platforms, bool in_place, bool shaders, bool compile) {
    Space s;
    uint64_t kept = 300ull << 20;  // exefs, analysis, the logs
    if (!in_place) kept += r.romfs.walked ? r.romfs.bytes + r.romfs.files * 64 : 10'600'000'000ull;  // romfs.pak
    for (const np::DlcInfo& d : r.dlc) kept += d.bytes;
    s.need = s.result = kept;
    if (shaders) s.need += 600ull << 20, s.result += 300ull << 20;  // the shader tool's work, then spirv.pak
    // gen/, one platform's objects and zig cache at a time (the cache goes once its library is built), the libraries
    if (compile) s.need += (2500ull << 20) + (800ull << 20) + platforms * (350ull << 20);
    s.result += platforms * (350ull << 20);
    return s;
}

bool parse_platforms(const std::string& list, std::vector<std::string>& out, std::string& bad) {
    out.clear();
    std::istringstream ls(list);
    for (std::string t; std::getline(ls, t, ',');) {
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (t.empty()) continue;
        if (t == "this") {
            if (gl::host_target().empty()) return bad = "this (this device has no platform of its own)", false;
            t = gl::host_target();
        }
        if (t == "all") {
            for (const gl::Target& g : gl::targets())
                if (std::find(out.begin(), out.end(), g.name) == out.end()) out.push_back(g.name);
            continue;
        }
        if (!gl::find_target(t)) return bad = t, false;
        if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    }
    return true;
}

}  // namespace prep
