// Finding islands: a host answers searches on UDP on its LAN with its NetworkInfo; a search asks this computer and the
// LAN's broadcast on UDP, and each address of online.join / NX_LDN_JOIN on TCP (the one port a host opens to the internet;
// ldn_link.cpp answers that one). Remembers where each network it found is, for link_join.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nx/runtime.h>

#include "config.h"
#include "hle_ldn.h"
#include "ldn_net.h"

namespace nx {

namespace {

struct Found {
    uint8_t bssid[6];
    sockaddr_in addr;
};

std::mutex g_found_lock;
std::vector<Found> g_found;
sock_t g_answer = kBadSock;
std::thread g_answer_thread;
std::atomic<bool> g_answering_stop{false};

void host_answer() {
    char in[32];
    std::vector<uint8_t> out(kMagicSize + kInfoSize);
    while (!g_answering_stop) {
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        const int r = int(recvfrom(g_answer, in, sizeof in, 0, reinterpret_cast<sockaddr*>(&from), &fl));
        if (r < 0) {
            if (g_answering_stop) return;
            continue;  // Windows: an earlier answer's port closed (WSAECONNRESET)
        }
        if (r != int(kMagicSize) || std::memcmp(in, kProbe, kMagicSize)) continue;
        std::memcpy(out.data(), kAnswer, kMagicSize);
        if (!ldn_link_info(out.data() + kMagicSize)) continue;
        sendto(g_answer, reinterpret_cast<const char*>(out.data()), int(out.size()), 0,
               reinterpret_cast<const sockaddr*>(&from), sizeof from);
    }
}


std::vector<sockaddr_in> lan_targets() {
    std::vector<sockaddr_in> v;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(link_port());
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    v.push_back(a);
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    v.push_back(a);
    return v;
}


std::vector<sockaddr_in> join_targets() {
    std::vector<sockaddr_in> v;
    const char* e = std::getenv("NX_LDN_JOIN");
    const std::string list = e ? e : config_get("online.join");
    if (list.empty()) return v;
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string item = list.substr(i, j - i);
        uint16_t p = link_port();
        if (const size_t c = item.find(':'); c != std::string::npos) {
            p = uint16_t(std::atoi(item.c_str() + c + 1));
            item.resize(c);
        }
        sockaddr_in a{};
        if (!item.empty() && resolve(item, p, a)) v.push_back(a);
        i = j + 1;
    }
    return v;
}


}  // namespace

bool discovery_answer_start() {
    g_answering_stop = false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(link_port());
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    g_answer = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_answer == kBadSock || bind(g_answer, reinterpret_cast<sockaddr*>(&a), sizeof a)) {
        stop_socket(g_answer);
        return false;
    }
    g_answer_thread = std::thread(host_answer);
    return true;
}

void discovery_answer_stop() {
    g_answering_stop = true;
    stop_socket(g_answer);
    if (g_answer_thread.joinable()) g_answer_thread.join();
}

bool discovery_address(const uint8_t* bssid, sockaddr_in& out) {
    std::lock_guard lk(g_found_lock);
    for (const Found& f : g_found)
        if (!std::memcmp(f.bssid, bssid, 6)) {
            out = f.addr;
            return true;
        }
    return false;
}

static void add_found(uint8_t* info, const sockaddr_in& from, uint8_t* out, int max, const uint8_t* filter, int& count) {
    if (std::memcmp(info, filter, 8) || !ldn_info_valid(info)) return;  // another game's, or nonsense
    bool seen = false;
    for (const Found& f : g_found) seen |= !std::memcmp(f.bssid, info + 0x20, 6);
    if (seen || count >= max) return;
    Found f;
    std::memcpy(f.bssid, info + 0x20, 6);
    f.addr = from;
    g_found.push_back(f);
    std::memcpy(out + size_t(count) * kInfoSize, info, kInfoSize);
    count++;
}


int link_scan(uint8_t* out, int max, const uint8_t* filter, int max_results_size) {
    (void)max_results_size;
    net_init();
    const sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) return 0;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
    for (const sockaddr_in& a : lan_targets())
        sendto(s, kProbe, int(kMagicSize), 0, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    int count = 0;
    std::vector<uint8_t> in(kMagicSize + kInfoSize + 16);
    std::lock_guard lk(g_found_lock);
    g_found.clear();
    for (;;) {
        if (!wait_socket(s, false, kScanMs)) break;
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        const int r = int(recvfrom(s, reinterpret_cast<char*>(in.data()), int(in.size()), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fl));
        if (r != int(kMagicSize + kInfoSize) || std::memcmp(in.data(), kAnswer, kMagicSize)) continue;
        add_found(in.data() + kMagicSize, from, out, max, filter, count);
    }
    sock_close(s);
    for (const sockaddr_in& a : join_targets()) {  // given addresses: TCP, the one port a host has to open
        const sock_t c = connect_within(a, kConnectMs);
        if (c == kBadSock) continue;
        set_timeout(c, kHandshakeMs);
        const uint32_t size = uint32_t(kMagicSize);
        uint8_t h[5 + kMagicSize] = {kProbeFrame};
        std::memcpy(h + 1, &size, 4);
        std::memcpy(h + 5, kProbe, kMagicSize);
        uint8_t type = 0;
        std::vector<uint8_t> f;
        if (send_all(c, h, sizeof h) && read_frame(c, type, f) && type == kInfo && f.size() == kInfoSize)
            add_found(f.data(), a, out, max, filter, count);
        sock_close(c);
    }
    return count;
}


}  // namespace nx
