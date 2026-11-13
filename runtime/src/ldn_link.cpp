// The link between consoles for local play (docs/LOCAL_PLAY.md): what the wireless network is on a Switch, carried as
// ldn_mitm carries it (github.com/spacemeowx2/ldn_mitm, its protocol in ldn_net.h), so that this game plays with a
// Switch running ldn_mitm on the same network as well as with other computers running this project.
//
// UDP port 11452, open while a network is searched or hosted: a host answers Scan with its NetworkInfo. A search sends
// Scan to the LAN's broadcast and to each address of online.join / NX_LDN_JOIN (host[:port], comma separated: a virtual
// LAN that carries no broadcast). A station keeps one TCP connection to the host's port 11452: Connect with its NodeInfo,
// then the host sends SyncNetwork with the NetworkInfo at every change. The game's own packets do not go through here:
// every node has its real address and hle_vsock.cpp sends UDP to the others directly.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
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

constexpr size_t kNiBssid = 0x20, kNiNodeCount = 0x67, kNiNodes = 0x68, kNodeIp = 0, kNodeMac = 4;

struct Peer {
    sock_t fd = kBadSock;
    std::mutex tx;
    std::thread reader;
    std::atomic<bool> alive{false};
};

std::atomic<bool> g_stopping{false};
std::atomic<bool> g_listening{false};
sock_t g_listen = kBadSock;
std::thread g_accept_thread;
Peer g_peers[kMaxNodes];  // host: [node]; [0] unused. station: [0] is the link to the host.

// The UDP socket on port 11452 and its reader.
std::mutex g_udp_lock;
sock_t g_udp = kBadSock;
std::thread g_udp_thread;
std::atomic<bool> g_udp_stop{false};

struct Found {
    uint8_t info[kInfoSize];
    uint32_t from;  // where the answer came from: the host's address on a virtual LAN when its node says another
};
std::mutex g_found_lock;
std::vector<Found> g_found;
bool g_scanning = false;

uint32_t get32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool send_packet(Peer& p, uint8_t type, const void* data, size_t n) {
    const std::vector<uint8_t> f = lan_packet(type, data, n);
    std::lock_guard lk(p.tx);
    return p.fd != kBadSock && send_all(p.fd, f.data(), f.size());
}

void no_delay(sock_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}

// The connection ended: its socket with it (a later node on this slot, or the next join, brings its own).
void peer_ended(Peer& p) {
    p.alive = false;
    std::lock_guard lk(p.tx);
    stop_socket(p.fd);
}

void udp_reader() {
    std::vector<uint8_t> in(kLanMaxPacket), body;
    while (!g_udp_stop) {
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        const int n = int(recvfrom(g_udp, reinterpret_cast<char*>(in.data()), int(in.size()), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fl));
        uint8_t type;
        if (n <= 0 || !lan_parse(in.data(), size_t(n), type, body)) continue;  // a timeout, or Windows' WSAECONNRESET
        const uint32_t from_ip = ntohl(from.sin_addr.s_addr);
        if (type == kScan) {
            // Only a local network answers: a 12-byte Scan gets a NetworkInfo back, which a stranger could aim elsewhere.
            if (!lan_private_ip(from_ip)) continue;
            uint8_t info[kInfoSize];
            if (!ldn_link_info(info)) continue;
            // Before anyone joins, the host's node holds this computer's address towards the asker: on a virtual LAN
            // (Tailscale, ZeroTier) that is the virtual one, where the game's UDP must go, not the default route's.
            if (!lan_bind_ip() && info[kNiNodeCount] == 1)
                if (const uint32_t own = route_address(from_ip)) std::memcpy(info + kNiNodes + kNodeIp, &own, 4);
            const std::vector<uint8_t> p = lan_packet(kScanResp, info, kInfoSize);
            sendto(g_udp, reinterpret_cast<const char*>(p.data()), int(p.size()), 0,
                   reinterpret_cast<const sockaddr*>(&from), sizeof from);
        } else if (type == kScanResp && body.size() == kInfoSize) {
            std::lock_guard lk(g_found_lock);
            if (!g_scanning) continue;
            Found f;
            std::memcpy(f.info, body.data(), kInfoSize);
            f.from = from_ip;
            g_found.push_back(f);
        }
    }
}

bool udp_open() {
    std::lock_guard lk(g_udp_lock);
    if (g_udp != kBadSock) return true;
    net_init();
    g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    if (g_udp != kBadSock) {
        setsockopt(g_udp, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
#ifndef _WIN32  // on Windows it would let another program take the port over
        setsockopt(g_udp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);  // as ldn_mitm
#endif
    }
    const sockaddr_in a = ipv4(lan_bind_ip(), kLanPort);
    if (g_udp == kBadSock || bind(g_udp, reinterpret_cast<const sockaddr*>(&a), sizeof a)) {
        log("ldn: cannot use UDP port %u: no island will be found or find this one", kLanPort);
        stop_socket(g_udp);
        return false;
    }
    set_timeout(g_udp, 250);  // the reader sees g_udp_stop even where closing a socket does not wake it
    g_udp_stop = false;
    g_udp_thread = std::thread(udp_reader);
    return true;
}

void udp_close() {
    std::lock_guard lk(g_udp_lock);
    g_udp_stop = true;
    if (g_udp_thread.joinable()) g_udp_thread.join();
    stop_socket(g_udp);
}

std::vector<sockaddr_in> join_targets() {
    std::vector<sockaddr_in> v;
    const char* e = std::getenv("NX_LDN_JOIN");
    const std::string list = e ? e : config_get("online.join");
    for (size_t i = 0; i < list.size();) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string item = list.substr(i, j - i);
        uint16_t p = kLanPort;
        if (const size_t c = item.find(':'); c != std::string::npos) {
            p = uint16_t(std::atoi(item.c_str() + c + 1));
            item.resize(c);
        }
        sockaddr_in a{};
        if (!item.empty() && p && resolve(item, p, a)) v.push_back(a);
        i = j + 1;
    }
    return v;
}

void host_reader(int node) {
    Peer& p = g_peers[node];
    uint8_t type;
    std::vector<uint8_t> body;
    while (lan_read(p.fd, type, body)) {}  // a station sends nothing after Connect: this only sees it leave
    peer_ended(p);
    if (!g_stopping) {
        log("ldn: node %d left", node);
        ldn_link_remove_node(node);
        link_push_info();
    }
}

void host_accept() {
    while (!g_stopping) {
        sockaddr_in peer{};
        socklen_t pl = sizeof peer;
        const sock_t fd = accept(g_listen, reinterpret_cast<sockaddr*>(&peer), &pl);
        if (fd == kBadSock) return;
        // Only from a local network, as the Scan answers (above): a station joins the game's network and gets its
        // packets, so the internet must not reach this port even with the firewall open.
        if (!lan_private_ip(ntohl(peer.sin_addr.s_addr))) {
            sock_close(fd);
            continue;
        }
        no_sigpipe(fd);
        no_delay(fd);
        set_timeout(fd, kHandshakeMs);  // nobody holds the accept loop for long
        uint8_t type = 0;
        std::vector<uint8_t> body;
        int node = -1;
        // This computer's address on the connection: the host's node takes it when this is the first station.
        sockaddr_in me{};
        socklen_t ml = sizeof me;
        const uint32_t own = getsockname(fd, reinterpret_cast<sockaddr*>(&me), &ml) ? 0 : ntohl(me.sin_addr.s_addr);
        if (lan_read(fd, type, body) && type == kConnect && body.size() == kNodeSize)
            node = ldn_link_add_node(body.data(), lan_bind_ip() ? 0 : own);
        if (node < 1 || node >= kMaxNodes) {  // ldn_mitm closes the connection when it is full
            sock_close(fd);
            continue;
        }
        Peer& p = g_peers[node];
        if (p.reader.joinable()) p.reader.join();
        p.fd = fd;
        p.alive = true;
        set_timeout(fd, 0);
        log("ldn: node %d joined", node);
        p.reader = std::thread(host_reader, node);
        link_push_info();
    }
}

void station_reader() {
    Peer& p = g_peers[0];
    uint8_t type;
    std::vector<uint8_t> body;
    while (lan_read(p.fd, type, body))
        if (type == kSyncNetwork && body.size() == kInfoSize && ldn_info_valid(body.data())) ldn_link_set_info(body.data());
    peer_ended(p);
    if (!g_stopping) {
        log("ldn: the link to the host ended");
        ldn_link_lost();
    }
}

}  // namespace

LinkStatus link_status() {
    LinkStatus s;
    s.hosting = g_listening;
    for (int i = 1; i < kMaxNodes; i++) s.players += g_peers[i].alive ? 1 : 0;
    return s;
}

void link_push_info() {
    uint8_t info[kInfoSize];
    if (!ldn_link_info(info)) return;
    for (int i = 1; i < kMaxNodes; i++)
        if (g_peers[i].alive) send_packet(g_peers[i], kSyncNetwork, info, kInfoSize);
}

void link_host_start() {
    net_init();
    g_stopping = false;
    const sockaddr_in a = ipv4(lan_bind_ip(), kLanPort);
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
#ifndef _WIN32
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (!udp_open() || g_listen == kBadSock || bind(g_listen, reinterpret_cast<const sockaddr*>(&a), sizeof a) ||
        listen(g_listen, 8)) {
        log("ldn: cannot listen on TCP port %u: this island will not be found", kLanPort);
        stop_socket(g_listen);
        return;
    }
    log("ldn: hosting on port %u", kLanPort);
    g_listening = true;
    g_accept_thread = std::thread(host_accept);
}

void link_stop() {
    g_stopping = true;
    g_listening = false;
    udp_close();
    stop_socket(g_listen);
    if (g_accept_thread.joinable()) g_accept_thread.join();
    for (Peer& p : g_peers) {
        p.alive = false;
        {
            std::lock_guard lk(p.tx);
            stop_socket(p.fd);
        }
        if (p.reader.joinable()) p.reader.join();
    }
    g_stopping = false;
}

int link_scan(uint8_t* out, int max, const uint8_t* filter) {
    if (!udp_open()) return 0;
    {
        std::lock_guard lk(g_found_lock);
        g_found.clear();
        g_scanning = true;
    }
    const std::vector<uint8_t> scan = lan_packet(kScan, nullptr, 0);
    std::vector<sockaddr_in> to = join_targets();
    to.push_back(ipv4(0xFFFFFFFFu, kLanPort));
    for (const sockaddr_in& a : to)
        sendto(g_udp, reinterpret_cast<const char*>(scan.data()), int(scan.size()), 0,
               reinterpret_cast<const sockaddr*>(&a), sizeof a);
    std::this_thread::sleep_for(std::chrono::milliseconds(kScanMs));
    std::lock_guard lk(g_found_lock);
    g_scanning = false;
    int count = 0;
    std::vector<Found> kept;
    for (Found& f : g_found) {
        if (std::memcmp(f.info, filter, 8) || !ldn_info_valid(f.info)) continue;  // another game's, or nonsense
        bool seen = false;
        for (const Found& k : kept) seen |= !std::memcmp(k.info + kNiBssid, f.info + kNiBssid, 6);
        if (seen || count >= max) continue;
        kept.push_back(f);
        std::memcpy(out + size_t(count++) * kInfoSize, f.info, kInfoSize);
    }
    g_found = kept;  // for link_join
    return count;
}

int link_join(const uint8_t* bssid, uint8_t* node, uint8_t* info) {
    Found f;
    {
        std::lock_guard lk(g_found_lock);
        size_t i = 0;
        while (i < g_found.size() && std::memcmp(g_found[i].info + kNiBssid, bssid, 6)) i++;
        if (i == g_found.size()) return -1;
        f = g_found[i];
    }
    const uint32_t host = get32(f.info + kNiNodes + kNodeIp);
    g_stopping = false;
    Peer& p = g_peers[0];
    if (p.reader.joinable()) p.reader.join();
    // The host's node address, as ldn_mitm connects; else where its answer came from (a virtual LAN, NAT).
    uint32_t used = host;
    sock_t fd = connect_within(ipv4(host, kLanPort), kConnectMs);
    if (fd == kBadSock && f.from != host) fd = connect_within(ipv4(used = f.from, kLanPort), kConnectMs);
    if (fd == kBadSock) return -1;
    const uint32_t own = lan_bind_ip() ? lan_bind_ip() : route_address(used);
    std::memcpy(node + kNodeIp, &own, 4);
    no_delay(fd);
    set_timeout(fd, kHandshakeMs);
    p.fd = fd;
    uint8_t type = 0;
    std::vector<uint8_t> body;
    if (send_packet(p, kConnect, node, kNodeSize)) {
        while (lan_read(fd, type, body)) {  // the first SyncNetwork: nothing else comes before the reader starts
            if (type != kSyncNetwork || body.size() != kInfoSize || !ldn_info_valid(body.data())) continue;
            for (int i = 1; i < kMaxNodes; i++) {
                const uint8_t* n = body.data() + kNiNodes + size_t(i) * kNodeSize;
                if (std::memcmp(n + kNodeMac, node + kNodeMac, 6) || get32(n + kNodeIp) != own) continue;
                std::memcpy(info, body.data(), kInfoSize);
                set_timeout(fd, 0);
                p.alive = true;
                p.reader = std::thread(station_reader);
                return i;
            }
        }
    }
    std::lock_guard lk(p.tx);
    stop_socket(p.fd);
    return -1;
}

}  // namespace nx
