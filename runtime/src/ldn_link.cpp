// The link between consoles for local play (docs/LOCAL_PLAY.md): what the wireless network is on a Switch, carried as
// ldn_mitm carries it (github.com/spacemeowx2/ldn_mitm, its protocol in ldn_net.h), so that this game plays with a
// Switch running ldn_mitm on the same network as well as with other computers running this project.
//
// UDP port 11452, open while a network is searched or hosted: a host answers Scan with its NetworkInfo. A search sends
// Scan to the LAN's broadcast and to each address of online.join / NX_LDN_JOIN (host[:port], comma separated: a virtual
// LAN that carries no broadcast), again to those that have not answered. Each step is in the log ("ldn: Scan to",
// "ldn: Scan from", "ldn: TCP"). A station keeps one TCP connection to the host's port 11452: Connect with its NodeInfo,
// then the host sends SyncNetwork with the NetworkInfo at every change. The game's own packets do not go through here:
// every node has its real address and hle_vsock.cpp sends UDP to the others directly.
#include <algorithm>
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
// A virtual LAN may lose a search's first datagrams or answer late (a relay, a tunnel set up on first use): online.join's
// addresses that have not answered are asked again every kScanMs, for kScanRounds rounds at most.
constexpr int kScanRounds = 3;

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
    uint16_t port;
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

std::string why(int err, int ms) {  // connect_within's error, for the log
    return err == -1 ? "no answer in " + std::to_string(ms) + " ms" : "error " + std::to_string(err);
}

// Whether this kind of event from this address is logged (n: how many so far): the first 3, then one in 100, as a
// station searches about every second; 64 addresses at most, so that a flood from many does not fill the log. The UDP
// reader and the TCP accept loop both ask (a lock).
enum Seen { kSeenAnswered, kSeenClosed, kSeenForeign, kSeenLate, kSeenNotSent, kSeenTcpForeign, kSeenTcpRefused };
bool worth_logging(uint32_t ip, Seen kind, uint32_t* n = nullptr) {
    static std::mutex lock;
    std::lock_guard lk(lock);
    struct Entry {
        uint32_t ip, n;
        Seen kind;
    };
    static Entry seen[32];
    static uint32_t added = 0;
    for (Entry& e : seen)
        if (e.n && e.ip == ip && e.kind == kind) {
            if (n) *n = e.n + 1;
            return ++e.n <= 3 || e.n % 100 == 0;
        }
    if (added >= 64) return false;
    seen[added++ % 32] = {ip, 1, kind};
    if (n) *n = 1;
    return true;
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
            if (!lan_private_ip(from_ip)) {
                if (worth_logging(from_ip, kSeenForeign))
                    log("ldn: Scan from %s ignored: not a local network's address", addr_text(from).c_str());
                continue;
            }
            uint8_t info[kInfoSize];
            if (!ldn_link_info(info)) {
                // A station hears its own broadcast: only another computer's search is worth a line.
                if (worth_logging(from_ip, kSeenClosed) && route_address(from_ip) != from_ip)
                    log("ldn: Scan from %s not answered: no island is open here", addr_text(from).c_str());
                continue;
            }
            // Before anyone joins, the host's node holds this computer's address towards the asker: on a virtual LAN
            // (Tailscale, ZeroTier) that is the virtual one, where the game's UDP must go, not the default route's.
            if (!lan_bind_ip() && info[kNiNodeCount] == 1)
                if (const uint32_t own = route_address(from_ip)) std::memcpy(info + kNiNodes + kNodeIp, &own, 4);
            const std::vector<uint8_t> p = lan_packet(kScanResp, info, kInfoSize);
            const bool sent = sendto(g_udp, reinterpret_cast<const char*>(p.data()), int(p.size()), 0,
                                     reinterpret_cast<const sockaddr*>(&from), sizeof from) == int(p.size());
            const int err = sent ? 0 : sock_error();
            uint32_t n = 0;
            if (!sent) {
                if (worth_logging(from_ip, kSeenNotSent))
                    log("ldn: Scan from %s: the answer was not sent (error %d)", addr_text(from).c_str(), err);
            } else if (worth_logging(from_ip, kSeenAnswered, &n))
                log("ldn: Scan from %s answered, host node address %s (%u so far)", addr_text(from).c_str(),
                    ip_text(get32(info + kNiNodes + kNodeIp)).c_str(), n);
        } else if (type == kScanResp && body.size() == kInfoSize) {
            {
                std::lock_guard lk(g_found_lock);
                if (g_scanning && g_found.size() >= 64) continue;  // a flood of answers: the search keeps its first ones
                if (g_scanning) {
                    Found f;
                    std::memcpy(f.info, body.data(), kInfoSize);
                    f.from = from_ip;
                    f.port = ntohs(from.sin_port);
                    g_found.push_back(f);
                    continue;
                }
            }
            if (worth_logging(from_ip, kSeenLate))
                log("ldn: an answer from %s came after the search had ended", addr_text(from).c_str());
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
        log("ldn: cannot use UDP port %u (error %d): no island will be found or find this one", kLanPort, sock_error());
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

// online.join / NX_LDN_JOIN's addresses; bad: the items that are not one, for the log.
std::vector<sockaddr_in> join_targets(std::string& bad) {
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
        else if (j > i) bad += (bad.empty() ? "\"" : ", \"") + list.substr(i, j - i) + "\"";
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
        if (fd == kBadSock) {
            if (!g_stopping) log("ldn: TCP port %u stopped taking connections (error %d)", kLanPort, sock_error());
            return;
        }
        const std::string who = addr_text(peer);
        // Only from a local network, as the Scan answers (above): a station joins the game's network and gets its
        // packets, so the internet must not reach this port even with the firewall open.
        if (!lan_private_ip(ntohl(peer.sin_addr.s_addr))) {
            if (worth_logging(ntohl(peer.sin_addr.s_addr), kSeenTcpForeign))
                log("ldn: TCP connection from %s refused: not a local network's address", who.c_str());
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
        const bool connect = lan_read(fd, type, body) && type == kConnect && body.size() == kNodeSize;
        if (connect) node = ldn_link_add_node(body.data(), lan_bind_ip() ? 0 : own);
        if (node < 1 || node >= kMaxNodes) {  // ldn_mitm closes the connection when it is full
            if (worth_logging(ntohl(peer.sin_addr.s_addr), kSeenTcpRefused)) {
                if (connect) log("ldn: TCP connection from %s refused: the island is full or closed", who.c_str());
                else log("ldn: TCP connection from %s: no Connect within %d ms", who.c_str(), kHandshakeMs);
            }
            sock_close(fd);
            continue;
        }
        Peer& p = g_peers[node];
        if (p.reader.joinable()) p.reader.join();
        p.fd = fd;
        p.alive = true;
        set_timeout(fd, 0);
        log("ldn: node %d joined from %s (this host's address on it %s)", node, who.c_str(), ip_text(own).c_str());
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
    if (!udp_open()) {  // it said why
        stop_socket(g_listen);
        return;
    }
    if (g_listen == kBadSock || bind(g_listen, reinterpret_cast<const sockaddr*>(&a), sizeof a) || listen(g_listen, 8)) {
        log("ldn: cannot listen on TCP port %u (error %d): this island will not be found", kLanPort, sock_error());
        stop_socket(g_listen);
        return;
    }
    log("ldn: hosting on port %u of %s", kLanPort, lan_bind_ip() ? ip_text(lan_bind_ip()).c_str() : "every address");
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
    std::string bad, sent;  // for the log: where the Scan went, from which local address
    const std::vector<sockaddr_in> join = join_targets(bad);
    const auto send_scan = [&](const sockaddr_in& a, bool describe) {
        const bool ok = sendto(g_udp, reinterpret_cast<const char*>(scan.data()), int(scan.size()), 0,
                               reinterpret_cast<const sockaddr*>(&a), sizeof a) == int(scan.size());
        const int err = ok ? 0 : sock_error();
        if (!describe) return;
        sent += (sent.empty() ? "" : ", ") + addr_text(a);
        if (a.sin_addr.s_addr != INADDR_BROADCAST)
            sent += " from " + ip_text(lan_bind_ip() ? lan_bind_ip() : route_address(ntohl(a.sin_addr.s_addr)));
        if (!ok) sent += " (error " + std::to_string(err) + ")";
    };
    // The addresses not heard from yet: an answer from that address, or naming it as its host's node (a host with
    // several networks answers from another of its addresses, its node the one towards the asker). Another island
    // answering the broadcast on the same Wi-Fi doesn't count for the friend asked by address.
    const auto unanswered = [&] {
        std::vector<sockaddr_in> v;
        std::lock_guard lk(g_found_lock);
        for (const sockaddr_in& a : join) {
            const uint32_t t = ntohl(a.sin_addr.s_addr);
            bool heard = false;
            for (const Found& f : g_found) heard |= f.from == t || get32(f.info + kNiNodes + kNodeIp) == t;
            if (!heard) v.push_back(a);
        }
        return v;
    };
    for (const sockaddr_in& a : join) send_scan(a, true);
    send_scan(ipv4(0xFFFFFFFFu, kLanPort), true);
    std::this_thread::sleep_for(std::chrono::milliseconds(kScanMs));
    int rounds = 1;
    for (std::vector<sockaddr_in> v; rounds < kScanRounds && !(v = unanswered()).empty(); rounds++) {
        for (const sockaddr_in& a : v) send_scan(a, false);
        for (int ms = 0; ms < kScanMs && !unanswered().empty(); ms += 25)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    int count = 0;
    std::string answers;
    {
        std::lock_guard lk(g_found_lock);
        g_scanning = false;
        std::vector<Found> kept;
        for (size_t i = 0; i < g_found.size(); i++) {
            Found& f = g_found[i];
            bool seen = false;  // its answer to another round, or to the broadcast
            for (size_t j = 0; j < i; j++) seen |= !std::memcmp(g_found[j].info + kNiBssid, f.info + kNiBssid, 6);
            if (seen) continue;
            const bool ours = !std::memcmp(f.info, filter, 8) && ldn_info_valid(f.info);  // not another game's, nonsense
            answers += (answers.empty() ? "" : ", ") + addr_text(ipv4(f.from, f.port));
            answers += ours ? " \"" + ldn_island_name(f.info) + "\"" : " (another game's)";
            if (!ours || count >= max) continue;
            kept.push_back(f);
            std::memcpy(out + size_t(count++) * kInfoSize, f.info, kInfoSize);
        }
        g_found = kept;  // for link_join
    }
    log("ldn: Scan to %s%s%s, %d round%s: %s", sent.c_str(), bad.empty() ? "" : "; not an address: ", bad.c_str(),
        rounds, rounds > 1 ? "s" : "", answers.empty() ? "no answer" : ("answers from " + answers).c_str());
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
    int err = 0;
    sock_t fd = connect_within(ipv4(host, kLanPort), kConnectMs, &err);
    if (fd == kBadSock)
        log("ldn: TCP to the host's node address %s failed (%s)", addr_text(ipv4(host, kLanPort)).c_str(),
            why(err, kConnectMs).c_str());
    if (fd == kBadSock && f.from != host) {
        fd = connect_within(ipv4(used = f.from, kLanPort), kConnectMs, &err);
        if (fd == kBadSock)
            log("ldn: TCP to %s, where its answer came from, failed (%s)", addr_text(ipv4(f.from, kLanPort)).c_str(),
                why(err, kConnectMs).c_str());
    }
    if (fd == kBadSock) return -1;
    const uint32_t own = lan_bind_ip() ? lan_bind_ip() : route_address(used);
    log("ldn: TCP connected to the host at %s, this computer %s", addr_text(ipv4(used, kLanPort)).c_str(),
        ip_text(own).c_str());
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
        log("ldn: the host took no node with this computer's address (closed, or no SyncNetwork with it in %d ms)",
            kHandshakeMs);
    } else {
        log("ldn: Connect not sent to the host (error %d)", sock_error());
    }
    std::lock_guard lk(p.tx);
    stop_socket(p.fd);
    return -1;
}

}  // namespace nx
