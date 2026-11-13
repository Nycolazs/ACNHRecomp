// olsc:u — Island Backup (nn::olsc, "save data backup"), kept on this computer.
//
// Nextendo's cloud saves are reserved to its official applications (and limited to Splatoon 3, 5 MB), so the backup
// the game makes is a local one: <data root>/user/backups/<island folder>/<stamp>.auto/, listed and restored by the
// settings screen with the others (saves.cpp: the 3 newest automatic ones of each island are kept, apart from the
// player's own, so automatic backups never push those out). ACNH keeps one island per console, so each
// island (save profile) has one Island Backup, whatever user the game names; only the island being played is backed up.
//
// Like the Switch, the system side makes the backup, not the game: when the player has turned Island Backup on (the
// game sets the system setting, cmd 14), the save is backed up if main.dat changed since the last Island Backup: at
// boot (olsc Initialize, before the game writes), back at the title screen after Save and quit (cmd 13), and when the
// window is closed (olsc_backup_on_exit, from the SDL quit event on Windows, Linux and macOS; Android has no such
// event, so there it is only at boot and at the title screen).
//
// A restore the game asks for (cmd 26) swaps the save under the running game, like the settings screen's restore: the
// settings screen then opens and only offers to quit, so the game cannot write the old island over the restored one.
//
// Local state, per island (user/backups/<island folder>/island_backup.txt): the setting (each save has its own Island
// Backup option, which the game sends at boot), the game's 0x400-byte custom block (island name and the like, shown in
// the restore list), and the Island Backup's name, time, custom block and main.dat stamp.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nx/runtime.h>

#include "ipc.h"
#include "kernel.h"
#include "platform.h"
#include "saves.h"
#include "ui/ui.h"

namespace nx {
namespace {

namespace fs = std::filesystem;

// Module 179 is olsc (shown as 2179-xxxx). The descriptions are this runtime's own.
constexpr uint32_t olsc_error(uint32_t desc) { return 179u | (desc << 9); }
const uint32_t kNoSave = olsc_error(903);     // nothing to back up / the backup could not be written
const uint32_t kBadBackup = olsc_error(904);  // the backup could not be restored
const uint32_t kNotDone = olsc_error(905);
const uint32_t kBusy = olsc_error(906);       // a backup or restore is already running
const uint32_t kNoCache = olsc_error(1200);   // GetSaveDataBackupInfoCache with no backup

constexpr size_t kInfoSize = 0x410;  // backup info: +0 id, +8 posix time, +0x10 custom block
constexpr size_t kCustomSize = 0x400;
constexpr int kMaxWaitSeconds = 120;  // a backup copies 10-40 MB on this disk

// ---- local state ----------------------------------------------------------------------------------------------

struct State {
    int setting = 0;      // 0 never set, 1 on, 2 off
    std::string custom;   // base64 of the custom block the game set last
    std::string backup;   // its name as saves::list_backups gives it ("<island folder>/<stamp>.auto"), "" if none
    int64_t time = 0;     // posix time of the Island Backup
    std::string backup_custom;  // base64 of the custom block when it was made
    std::string stamp;    // main.dat size and write time when it was made
};

std::mutex g_state_lock;

fs::path state_path() { return fs::u8path(saves::backups_folder()) / "island_backup.txt"; }

State load_state() {
    State s;
    std::ifstream in(state_path());
    for (std::string line; std::getline(in, line);) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "setting") s.setting = std::atoi(v.c_str());
        else if (k == "custom") s.custom = v;
        else if (k == "backup") s.backup = v;
        else if (k == "time") s.time = std::atoll(v.c_str());
        else if (k == "backup_custom") s.backup_custom = v;
        else if (k == "stamp") s.stamp = v;
    }
    return s;
}

void save_state(const State& s) {
    const fs::path p = state_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".new";  // not ".tmp": saves.cpp deletes those there as abandoned backups
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "setting=" << s.setting << "\ncustom=" << s.custom << "\nbackup=" << s.backup << "\ntime=" << s.time
            << "\nbackup_custom=" << s.backup_custom << "\nstamp=" << s.stamp << "\n";
        if (!out) return;
    }
    fs::rename(tmp, p, ec);
    if (ec) log("olsc: cannot write %s", p.string().c_str());
}

void update_state(const std::function<void(State&)>& f) {
    std::lock_guard lk(g_state_lock);
    State s = load_state();
    f(s);
    save_state(s);
}

State get_state() {
    std::lock_guard lk(g_state_lock);
    return load_state();
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const std::vector<uint8_t>& in) {
    std::string out;
    out.reserve(4 * ((in.size() + 2) / 3));
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t n = uint32_t(in[i]) << 16 | (i + 1 < in.size() ? uint32_t(in[i + 1]) << 8 : 0) |
                           (i + 2 < in.size() ? in[i + 2] : 0);
        out += kB64[n >> 18 & 63];
        out += kB64[n >> 12 & 63];
        out += i + 1 < in.size() ? kB64[n >> 6 & 63] : '=';
        out += i + 2 < in.size() ? kB64[n & 63] : '=';
    }
    return out;
}

// Empty on anything that is not padded base64.
std::vector<uint8_t> b64_decode(const std::string& in) {
    if (in.empty() || in.size() % 4) return {};
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        uint32_t n = 0;
        int pad = 0;
        for (size_t k = 0; k < 4; k++) {
            const char ch = in[i + k];
            const char* p = ch ? std::strchr(kB64, ch) : nullptr;
            if (ch == '=' && i + 4 == in.size() && k >= 2) {
                pad++;
            } else if (!p || pad) {
                return {};
            }
            n = n << 6 | uint32_t(p ? p - kB64 : 0);
        }
        out.push_back(uint8_t(n >> 16));
        if (pad < 2) out.push_back(uint8_t(n >> 8));
        if (pad < 1) out.push_back(uint8_t(n));
    }
    return out;
}

// ---- the Island Backup ----------------------------------------------------------------------------------------

struct Backup {
    uint64_t id = 0;
    int64_t time = 0;
    std::vector<uint8_t> custom;
};

// The Island Backup, if its folder is still there (the settings screen's pruning or the player may have removed it).
bool current_backup(Backup& out) {
    const State s = get_state();
    if (s.backup.empty()) return false;
    const auto all = saves::list_backups();
    if (std::find(all.begin(), all.end(), s.backup) == all.end()) return false;
    out.id = uint64_t(s.time);
    out.time = s.time;
    out.custom = b64_decode(s.backup_custom);
    return true;
}

// Size and write time of main.dat: changes whenever the game saves.
std::string save_stamp() {
    const fs::path main = fs::u8path(saves::save_dir()) / "main.dat";
    std::error_code ec;
    const auto size = fs::file_size(main, ec);
    if (ec) return {};
    const auto t = fs::last_write_time(main, ec);
    if (ec) return {};
    // libc++ (Android, macOS) keeps file times in __int128, which to_string has no overload for; 64 bits are plenty here
    return std::to_string(size) + ":" + std::to_string(static_cast<long long>(t.time_since_epoch().count()));
}

std::atomic<bool> g_transfer{false};  // one backup or restore at a time

uint32_t make_backup() {
    if (!saves::has_save()) return kNoSave;
    const std::string stamp = save_stamp();
    std::string err;
    const std::string name = saves::backup_now(&err, true);
    if (name.empty()) {
        log("olsc: cannot back up the island: %s", err.c_str());
        return kNoSave;
    }
    const int64_t now = int64_t(std::time(nullptr));
    update_state([&](State& s) {
        s.backup = name;
        s.time = now;
        s.backup_custom = s.custom;
        s.stamp = stamp;
    });
    log("olsc: Island Backup saved as %s", name.c_str());
    return 0;
}

uint32_t restore() {
    const State s = get_state();
    std::string err;
    if (s.backup.empty() || !saves::restore_backup(s.backup, &err)) {  // backs up the current save before swapping
        log("olsc: cannot restore the Island Backup %s: %s", s.backup.c_str(), err.c_str());
        return kBadBackup;
    }
    update_state([](State& st) { st.stamp = save_stamp(); });  // the restored save is the backup: nothing new
    log("olsc: restored the Island Backup %s", s.backup.c_str());
    ui::ui_must_quit();
    return 0;
}

uint32_t transfer(uint32_t (*work)()) {
    if (g_transfer.exchange(true)) return kBusy;
    const uint32_t rc = work();
    g_transfer = false;
    return rc;
}

bool backup_due() {
    const State s = get_state();
    return s.setting == 1 && !g_transfer.load() && saves::has_save() && save_stamp() != s.stamp;
}

void backup_if_changed() {
    if (!backup_due()) return;
    std::thread([] {
        if (const uint32_t rc = transfer(make_backup)) log("olsc: automatic Island Backup failed (%#x)", rc);
    }).detach();
}

void write_info(uint64_t addr, const Backup& b) {
    std::memset(reinterpret_cast<void*>(addr), 0, kInfoSize);
    std::memcpy(reinterpret_cast<void*>(addr), &b.id, 8);
    std::memcpy(reinterpret_cast<void*>(addr + 8), &b.time, 8);
    std::memcpy(reinterpret_cast<void*>(addr + 0x10), b.custom.data(), std::min(b.custom.size(), kCustomSize));
}

// ---- IPC ------------------------------------------------------------------------------------------------------

// Logs every olsc call (few per session): ACNH aborts on a failed result, so the log must say which call it was.
std::shared_ptr<Service> traced(std::shared_ptr<Service> s) {
    for (auto& [id, fn] : s->cmds) {
        fn = [name = s->name, id = id, inner = fn](IpcCtx& ctx) {
            const uint32_t rc = inner(ctx);
            log("olsc: %s cmd %u -> %#x", name.c_str(), id, rc);
            return rc;
        };
    }
    return s;
}

// An async object (IAsyncResult and its subclasses): `work` runs on its own thread.
struct Async {
    const char* name = "";
    std::atomic<uint32_t> result{~0u};
    std::shared_ptr<EventState> event = std::make_shared<EventState>();
    std::mutex m;
    std::condition_variable cv;
    bool done() const { return result.load() != ~0u; }
    // ACNH aborts on a "not done" result, so a result asked for early waits for the worker. The IPC lock is held
    // meanwhile, so the game stalls for the copy at worst.
    bool wait() {
        if (!done()) log("olsc: %s asked before it finished, waiting", name);
        std::unique_lock lk(m);
        return cv.wait_for(lk, std::chrono::seconds(kMaxWaitSeconds), [this] { return done(); });
    }
};

std::shared_ptr<Service> make_async(const char* name, std::shared_ptr<Async> a, std::function<uint32_t()> work) {
    auto s = std::make_shared<Service>(name);
    a->name = name;
    std::thread([a, work] {
        const uint32_t rc = work();
        {
            std::lock_guard lk(a->m);
            a->result = rc;
        }
        a->cv.notify_all();
        a->event->signaled = true;
        notify_all();
    }).detach();
    // cmd 3 is GetSystemEvent: ACNH calls it first and attaches the handle (AttachReadableHandleToSystemEvent).
    // 0-2 are not seen yet; Cancel / GetResult / HasDone is a guess, and the trace log shows which ACNH uses.
    s->cmds[3] = [a](IpcCtx& ctx) {
        auto e = std::make_shared<KEvent>();
        e->state = a->event;
        ctx.out_copy.push_back(handle_add(e));
        return 0u;
    };
    s->cmds[0] = [](IpcCtx&) { return 0u; };  // Cancel: the work runs to its end
    s->cmds[1] = [a](IpcCtx&) { return a->wait() ? a->result.load() : kNotDone; };  // GetResult
    s->cmds[2] = [a](IpcCtx& ctx) {  // HasDone
        ctx.push<uint8_t>(a->done());
        return 0u;
    };
    return s;
}

// Newness / info-cache refreshes: nothing to fetch for a local backup.
std::shared_ptr<Service> make_done_async() {
    return make_async("olsc:IAsyncResult", std::make_shared<Async>(), [] { return 0u; });
}

std::shared_ptr<Service> make_transfer_async(uint32_t (*work)()) {
    auto s = make_async("olsc:IAsyncTransferSaveDataBackup", std::make_shared<Async>(), [work] { return transfer(work); });
    s->cmds[100] = [](IpcCtx& ctx) {  // GetProgress: no progress reported
        for (int i = 0; i < 0x30 / 4; i++) ctx.push<uint32_t>(0);
        return 0u;
    };
    return s;
}

std::shared_ptr<Service> make_list_async() {
    auto a = std::make_shared<Async>();
    auto items = std::make_shared<std::vector<Backup>>();
    auto s = make_async("olsc:IAsyncListDownloadableSaveDataBackupInfo", a, [items] {
        Backup b;
        if (current_backup(b)) items->push_back(b);
        return 0u;
    });
    s->cmds[100] = [a, items](IpcCtx& ctx) {  // GetCount
        if (!a->wait()) return kNotDone;
        ctx.push<uint32_t>(uint32_t(items->size()));
        return 0u;
    };
    s->cmds[101] = [a, items](IpcCtx& ctx) {  // List(offset) -> infos
        if (!a->wait()) return kNotDone;
        const uint32_t from = ctx.arg<uint32_t>(0);
        const Buffer out = ctx.out_buf(0);
        uint32_t n = 0;
        for (size_t k = from; k < items->size() && (n + 1) * kInfoSize <= out.size; k++, n++)
            write_info(out.addr + n * kInfoSize, (*items)[k]);
        ctx.push<uint32_t>(n);
        return 0u;
    };
    return s;
}

std::shared_ptr<Service> make_olsc(const char* name) {
    auto s = std::make_shared<Service>(name);
    s->cmds[0] = [](IpcCtx&) {  // Initialize
        backup_if_changed();
        return 0u;
    };
    s->cmds[10] = [](IpcCtx& ctx) {           // UpdateDataNewnessAsync
        ctx.push_object(traced(make_done_async()));
        return 0u;
    };
    s->cmds[13] = [](IpcCtx& ctx) {  // GetSaveDataBackupSetting(uid) -> u8: asked at the title screen
        backup_if_changed();
        ctx.push<uint8_t>(uint8_t(get_state().setting));
        return 0u;
    };
    s->cmds[14] = [](IpcCtx& ctx) {  // SetSaveDataBackupSettingEnabled(bool, uid)
        const int setting = ctx.arg<uint8_t>(0) ? 1 : 2;
        update_state([setting](State& st) { st.setting = setting; });
        return 0u;
    };
    s->cmds[15] = [](IpcCtx& ctx) {  // SetCustomData(uid, buffer)
        const Buffer in = ctx.in_buf(0);
        std::vector<uint8_t> bytes;
        if (in.addr) {
            const auto* p = reinterpret_cast<const uint8_t*>(in.addr);
            bytes.assign(p, p + std::min<uint64_t>(in.size, kCustomSize));
        }
        const std::string b64 = b64_encode(bytes);
        update_state([&](State& st) { st.custom = b64; });
        return 0u;
    };
    s->cmds[16] = [](IpcCtx&) {  // DeleteSaveDataBackupSetting(uid)
        update_state([](State& st) { st.setting = 0; });
        return 0u;
    };
    s->cmds[18] = [](IpcCtx& ctx) {  // GetSaveDataBackupInfoCache(uid) -> info
        Backup b;
        if (!current_backup(b)) return kNoCache;
        const Buffer out = ctx.out_buf(0);
        if (out.addr && out.size >= kInfoSize) write_info(out.addr, b);
        return 0u;
    };
    s->cmds[19] = [](IpcCtx& ctx) {  // UpdateSaveDataBackupInfoCacheAsync(uid)
        ctx.push_object(traced(make_done_async()));
        return 0u;
    };
    s->cmds[25] = [](IpcCtx& ctx) {  // ListDownloadableSaveDataBackupInfoAsync
        ctx.push_object(traced(make_list_async()));
        return 0u;
    };
    s->cmds[26] = [](IpcCtx& ctx) {  // DownloadSaveDataBackupAsync(uid, backup id): there is only one backup
        ctx.push_object(traced(make_transfer_async(restore)));
        return 0u;
    };
    s->cmds[27] = [](IpcCtx& ctx) {  // UploadSaveDataBackupAsync(uid)
        ctx.push_object(traced(make_transfer_async(make_backup)));
        return 0u;
    };
    return s;
}

}  // namespace

// The window is closing: the Island Backup now, on this thread, if the save changed. A main.dat written in the last
// 3 s may be a save still in progress, which would make a torn backup: the next boot backs it up instead.
void olsc_backup_on_exit() {
    try {
        if (!backup_due()) return;
        const fs::path main = fs::u8path(saves::save_dir()) / "main.dat";
        std::error_code ec;
        const auto t = fs::last_write_time(main, ec);
        if (ec || fs::file_time_type::clock::now() - t < std::chrono::seconds(3)) return;
        if (const uint32_t rc = transfer(make_backup)) log("olsc: Island Backup at exit failed (%#x)", rc);
    } catch (...) {
    }
}

static ServiceRegistrar r_olsc_u("olsc:u", [] { return traced(make_olsc("olsc:u")); });

}  // namespace nx
