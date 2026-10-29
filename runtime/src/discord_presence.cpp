// Discord Rich Presence: "Playing <application name>", the time since the game started, where the player is (from the
// room and island models the game loads: discord_presence_file, called by hle_fs.cpp), and the season and time of day
// from the host clock (the island's clock). It talks to the Discord client over its local IPC (\\.\pipe\discord-ipc-N
// on Windows, a Unix socket discord-ipc-N in $XDG_RUNTIME_DIR, $TMPDIR or /tmp elsewhere): a frame is
// {u32 opcode, u32 length} and JSON.
//
// config.ini, [discord]: client_id (the Application ID of a Discord application; nothing without it: its name is what
// Discord shows), island (the island's name; else the save's), hemisphere (north / south), large_image (default "acnh")
// and the small images winter, spring, summer, autumn (defaults: those names): Rich Presence art assets of that
// application, or image URLs. trace = 1 logs the models that set the place to <data root>/discord_trace.txt. The texts
// are in the settings screen's language (ui/i18n.cpp, "discord.*"). Discord started after the game is picked up within
// 20 s.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <nx/runtime.h>

#include "config.h"
#include "ui/i18n.h"
#include "platform.h"

namespace nx {

using ui::tr;
using ui::trf;

namespace {

#if defined(_WIN32)
using Handle = HANDLE;
const Handle kNone = INVALID_HANDLE_VALUE;
Handle ipc_open(int n) {
    std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(n);
    return CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}
bool ipc_write(Handle h, const void* p, uint32_t n) {
    DWORD done = 0;
    return WriteFile(h, p, n, &done, nullptr) && done == n;
}
bool ipc_read(Handle h, void* p, uint32_t n) {
    for (uint32_t got = 0; got < n;) {
        DWORD r = 0;
        if (!ReadFile(h, static_cast<char*>(p) + got, n - got, &r, nullptr) || !r) return false;
        got += r;
    }
    return true;
}
// Bytes waiting to be read; -1 when Discord has closed the pipe.
long ipc_available(Handle h) {
    DWORD avail = 0;
    return PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) ? long(avail) : -1;
}
void ipc_close(Handle h) { CloseHandle(h); }
int process_id() { return int(GetCurrentProcessId()); }
#else
using Handle = int;
const Handle kNone = -1;
Handle ipc_open(int n) {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (!dir) dir = std::getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::snprintf(a.sun_path, sizeof a.sun_path, "%s/discord-ipc-%d", dir, n);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) return fd;
    if (fd >= 0) close(fd);
    return kNone;
}
bool ipc_write(Handle h, const void* p, uint32_t n) { return write(h, p, n) == ssize_t(n); }
bool ipc_read(Handle h, void* p, uint32_t n) {
    for (uint32_t got = 0; got < n;) {
        ssize_t r = read(h, static_cast<char*>(p) + got, n - got);
        if (r <= 0) return false;
        got += uint32_t(r);
    }
    return true;
}
long ipc_available(Handle h) {
    int avail = 0;
    if (ioctl(h, FIONREAD, &avail) != 0) return -1;
    if (avail == 0) {  // nothing to read, or closed: a peek tells them apart
        char c;
        if (recv(h, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0) return -1;
    }
    return avail;
}
void ipc_close(Handle h) { close(h); }
int process_id() { return int(getpid()); }
#endif

bool send_frame(Handle h, uint32_t opcode, const std::string& json) {
    uint32_t head[2] = {opcode, uint32_t(json.size())};
    return ipc_write(h, head, sizeof head) && ipc_write(h, json.data(), uint32_t(json.size()));
}
bool read_frame(Handle h, std::string& json) {
    uint32_t head[2];
    if (!ipc_read(h, head, sizeof head) || head[1] > (1u << 20)) return false;
    json.resize(head[1]);
    return ipc_read(h, json.data(), head[1]);
}

std::string escaped(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (uint8_t(c) >= 0x20) out += c;
    }
    return out;
}

// ---- where the player is ---------------------------------------------------------------------------------------

std::mutex g_place_lock;
const char* g_place = nullptr;  // the place's text key; null until a model says
bool g_place_trace = false;

// Model name prefixes (romfs Model/<name>.Nin_NX_NVN.zs) and the place each one means (a text key); the first match
// wins. A room's model is loaded on the way in, the buoys around an island when its field is.
struct Place {
    const char* prefix;
    const char* where;
};
constexpr Place kPlaces[] = {
    {"FldBuoyMain", "discord.island"},
    {"FldBuoyKappei", "discord.kappn"},
    {"FldBuoyMystery", "discord.mystery_island"},
    {"FldBuoyWherearen", "discord.paradise"},
    {"Wherearen", "discord.paradise"},
    {"IdrMuseumCafe", "discord.roost"},
    {"IdrMuseum", "discord.museum"},
    {"IdrMarket", "discord.shop"},
    {"IdrTailor", "discord.tailor"},
    {"IdrOffice", "discord.office"},
    {"IdrAirPort", "discord.airport"},
    {"IdrHotel", "discord.paradise_office"},
    {"IdrDream", "discord.dream"},
    {"IdrCampSite", "discord.campsite"},
};

const char* place() {
    std::lock_guard lk(g_place_lock);
    return g_place;
}

// ---- season and time of day ------------------------------------------------------------------------------------

struct Moment {
    const char* season;  // the small image's config key / asset name, and its text key "discord.<season>"
    const char* day;     // a text key
};

Moment moment(bool south) {
    std::time_t t = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    static const char* const kSeasons[] = {"winter", "spring", "summer", "autumn"};
    int s = (lt.tm_mon + 1) % 12 / 3;  // Dec-Feb 0, Mar-May 1, Jun-Aug 2, Sep-Nov 3
    if (south) s = (s + 2) % 4;
    int h = lt.tm_hour;
    return {kSeasons[s], h >= 5 && h < 12    ? "discord.morning"
                         : h >= 12 && h < 17 ? "discord.afternoon"
                         : h >= 17 && h < 21 ? "discord.evening"
                                             : "discord.night"};
}

// ---- the presence ----------------------------------------------------------------------------------------------

struct Settings {
    std::string client_id, island, large_image;
    bool south = false;
};

// The island's name: discord.island, else the save's landname.dat (UTF-16, up to 10 characters; there before the first
// save is not), read again once a minute.
std::string island_name(const Settings& s) {
    if (!s.island.empty()) return s.island;
    static std::string name;
    static auto next = std::chrono::steady_clock::time_point{};
    if (std::chrono::steady_clock::now() < next) return name;
    next = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    if (FILE* f = plat::fopen_utf8(plat::data_root() + "/user/device_save/landname.dat", "rb")) {
        uint8_t b[20];
        size_t n = std::fread(b, 1, sizeof b, f) / 2;
        std::fclose(f);
        std::string out;
        for (size_t i = 0; i < n; i++) {
            uint32_t c = b[2 * i] | uint32_t(b[2 * i + 1]) << 8;
            if (!c) break;
            if (c < 0x80) out += char(c);
            else if (c < 0x800) out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
            else out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
        }
        name = out;
    }
    return name;
}

std::string activity_json(const Settings& s, int64_t start) {
    const Moment m = moment(s.south);
    const std::string island = island_name(s);
    const char* at = place();
    std::string where = !at ? std::string()
                        : std::strcmp(at, "discord.island") == 0 && !island.empty() ? trf("discord.island_named", island)
                                                                                     : std::string(tr(at));
    const std::string season = tr((std::string("discord.") + m.season).c_str());
    std::string j = "{\"timestamps\":{\"start\":" + std::to_string(start) + "}";
    if (!where.empty()) j += ",\"details\":\"" + escaped(where) + "\"";
    j += ",\"state\":\"" + escaped(season + " · " + tr(m.day)) + "\"";
    j += ",\"assets\":{\"large_image\":\"" + escaped(s.large_image) + "\"";
    j += ",\"large_text\":\"" +
         escaped(island.empty() ? std::string("Animal Crossing: New Horizons") : trf("discord.island_title", island)) +
         "\"";
    j += ",\"small_image\":\"" + escaped(config_get(std::string("discord.") + m.season, m.season)) +
         "\",\"small_text\":\"" + escaped(season) + "\"}}";
    return j;
}

void presence_thread(Settings s) {
    const int64_t start = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string pid = std::to_string(process_id());
    bool told = false;
    uint64_t nonce = 0;
    for (;;) {
        Handle h = kNone;
        for (int n = 0; n < 10 && h == kNone; n++) h = ipc_open(n);
        std::string reply;
        if (h != kNone && send_frame(h, 0, "{\"v\":1,\"client_id\":\"" + escaped(s.client_id) + "\"}") &&
            read_frame(h, reply)) {
            if (!told) log("discord: rich presence on"), told = true;
            std::string sent;
            for (;;) {  // the activity again whenever it changes; Discord's replies read as they come
                std::string a = activity_json(s, start);
                if (a != sent) {
                    std::string cmd = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + std::to_string(++nonce) +
                                      "\",\"args\":{\"pid\":" + pid + ",\"activity\":" + a + "}}";
                    if (!send_frame(h, 1, cmd)) break;
                    sent = a;
                }
                long avail = ipc_available(h);
                if (avail < 0) break;
                if (avail > 0 && !read_frame(h, reply)) break;
                if (avail > 0 && reply.find("\"evt\":\"ERROR\"") != std::string::npos)
                    log("discord: %s", reply.substr(0, 300).c_str());
                if (!avail) std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
        if (h != kNone) ipc_close(h);
        std::this_thread::sleep_for(std::chrono::seconds(20));
    }
}

std::atomic<bool> g_on{false};

}  // namespace

// A file the game opened (hle_fs.cpp): a room or island model sets the place.
void discord_presence_file(const std::string& guest_path) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    size_t at = guest_path.find("/Model/");
    if (at == std::string::npos) return;
    std::string name = guest_path.substr(at + 7);
    if (name.find("_Tex_") != std::string::npos || name.find("Obj") != std::string::npos) return;
    if (size_t dot = name.find('.'); dot != std::string::npos) name.resize(dot);
    for (const Place& p : kPlaces) {
        if (name.rfind(p.prefix, 0) != 0) continue;
        std::lock_guard lk(g_place_lock);
        if (g_place_trace && g_place != p.where)
            if (FILE* f = plat::fopen_utf8(plat::data_root() + "/discord_trace.txt", "ab")) {
                std::fprintf(f, "%s -> %s\n", name.c_str(), p.where);
                std::fclose(f);
            }
        g_place = p.where;
        return;
    }
}

void discord_presence_start() {
    Settings s;
    s.client_id = config_get("discord.client_id");
    if (s.client_id.empty()) return;
    s.island = config_get("discord.island");  // else the save's landname.dat (island_name)
    s.large_image = config_get("discord.large_image", "acnh");
    std::string hemi = config_get("discord.hemisphere", "north");
    s.south = hemi == "south" || hemi == "sud";
    g_place_trace = config_get_bool("discord.trace", false);
    g_on = true;
    std::thread(presence_thread, s).detach();
}

}  // namespace nx
