// The link between consoles for local play (NX_LDN=1, docs/LOCAL_PLAY.md): what the wireless network is on a Switch.
// A host answers searches on UDP (NX_LDN_PORT, default 39500) with its NetworkInfo and listens on TCP on the same
// port. A station keeps one TCP connection to the host, which gives it a node, sends the NetworkInfo at every change
// and relays the game's datagrams between the players. A search asks this computer and the LAN's broadcast on UDP, and
// every address of online.join / NX_LDN_JOIN (host[:port], comma separated: the internet, a virtual LAN) on TCP, so that
// only that TCP port has to be reachable from outside. Direct play, no server: the host asks its router for that port
// with UPnP (NX_LDN_UPNP=0: not); the settings screen shows the address to give to friends (link_status).
// The design follows MHGURecomp (morerokk, MIT): runtime/net/ldn.c and vsock.c, docs/STATUS.md "Local Play over IP".
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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
#include "ldn_upnp.h"

namespace nx {

namespace {

enum Role { kNoRole, kHost, kStation };

struct PacketHeader {
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
};

struct Peer {
    sock_t fd = kBadSock;
    std::mutex tx;
    std::thread reader;
    std::atomic<bool> alive{false};
    uint32_t ip = 0;
};

std::atomic<Role> g_role{kNoRole};
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_listening{false};  // host: its port is really open (g_role says it hosts even when it could not)
sock_t g_listen = kBadSock;
std::thread g_accept_thread;
std::mutex g_peers_lock;
Peer g_peers[kMaxNodes];  // host: [node]; [0] unused. station: [0] is the link to the host.

// A frame: its type, the size of what follows (u32), then the two parts of it.
bool send_frame(Peer& peer, uint8_t type, const void* a, uint32_t alen, const void* b = nullptr, uint32_t blen = 0) {
    std::vector<uint8_t> f(5 + alen + blen);
    const uint32_t size = alen + blen;
    f[0] = type;
    std::memcpy(&f[1], &size, 4);
    if (alen) std::memcpy(&f[5], a, alen);
    if (blen) std::memcpy(&f[5 + alen], b, blen);
    std::lock_guard lk(peer.tx);
    return peer.fd != kBadSock && send_all(peer.fd, f.data(), f.size());
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

// Host: to the host's own sockets and/or the stations. from: the sending node, 0 = the host.
void route(int from, const PacketHeader& h, const void* data, uint32_t len) {
    const bool all = h.dst_ip == ldn_broadcast_ip() || h.dst_ip == 0xFFFFFFFFu;
    if (all || h.dst_ip == ldn_own_ip()) vsock_deliver(h.src_ip, h.src_port, h.dst_ip, h.dst_port, data, len);
    for (int i = 1; i < kMaxNodes; i++) {
        Peer& p = g_peers[i];
        if (i == from || !p.alive || !(all || p.ip == h.dst_ip)) continue;
        send_frame(p, kPacket, &h, sizeof h, data, len);
    }
}

void host_reader(int node) {
    Peer& p = g_peers[node];
    uint8_t type;
    std::vector<uint8_t> f;
    while (read_frame(p.fd, type, f)) {
        if (type != kPacket || f.size() < sizeof(PacketHeader)) continue;
        PacketHeader h;
        std::memcpy(&h, f.data(), sizeof h);
        h.src_ip = p.ip;  // whatever it claims
        route(node, h, f.data() + sizeof h, uint32_t(f.size() - sizeof h));
    }
    peer_ended(p);
    if (!g_stopping) {
        log("ldn: node %d left", node);
        ldn_link_remove_node(node);
        link_push_info();
    }
}

void host_accept() {
    while (!g_stopping) {
        const sock_t fd = accept(g_listen, nullptr, nullptr);
        if (fd == kBadSock) return;
        no_sigpipe(fd);
        no_delay(fd);
        set_timeout(fd, kHandshakeMs);  // nobody holds the accept loop for long
        uint8_t type = 0;
        std::vector<uint8_t> f;
        int node = -1;
        const bool got = read_frame(fd, type, f);
        if (got && type == kProbeFrame && f.size() == kMagicSize && !std::memcmp(f.data(), kProbe, kMagicSize)) {
            uint8_t info[kInfoSize];
            if (ldn_link_info(info)) {
                const uint32_t size = uint32_t(kInfoSize);
                uint8_t h[5] = {kInfo};
                std::memcpy(h + 1, &size, 4);
                if (send_all(fd, h, 5)) send_all(fd, info, kInfoSize);
            }
            sock_close(fd);
            continue;
        }
        if (got && type == kJoin && f.size() == kJoinSize) {
            std::lock_guard lk(g_peers_lock);
            node = ldn_link_add_node(f.data(), f.data() + 0x20, f.data() + 0x26);
        }
        if (node < 1 || node >= kMaxNodes) {
            const uint8_t h[5] = {kRefused, 0, 0, 0, 0};
            send_all(fd, h, 5);
            sock_close(fd);
            continue;
        }
        Peer& p = g_peers[node];
        if (p.reader.joinable()) p.reader.join();
        p.fd = fd;
        p.ip = ldn_node_ip(node);
        p.alive = true;
        set_timeout(fd, 0);
        const uint32_t id = uint32_t(node);
        send_frame(p, kWelcome, &id, 4);
        log("ldn: node %d joined", node);
        p.reader = std::thread(host_reader, node);
        link_push_info();
    }
}

void station_reader() {
    Peer& p = g_peers[0];
    uint8_t type;
    std::vector<uint8_t> f;
    while (read_frame(p.fd, type, f)) {
        if (type == kInfo && f.size() == kInfoSize) {
            if (ldn_info_valid(f.data())) ldn_link_set_info(f.data());
        } else if (type == kPacket && f.size() >= sizeof(PacketHeader)) {
            PacketHeader h;
            std::memcpy(&h, f.data(), sizeof h);
            const uint32_t n = uint32_t(f.size() - sizeof h);
            vsock_deliver(h.src_ip, h.src_port, h.dst_ip, h.dst_port, f.data() + sizeof h, n);
        }
    }
    peer_ended(p);
    if (!g_stopping) {
        log("ldn: the link to the host ended");
        ldn_link_lost();
    }
}

std::mutex g_upnp_lock;
std::condition_variable g_upnp_cv;
std::thread g_upnp_thread;
bool g_upnp_stop = false;

std::mutex g_status_lock;
LinkStatus g_status;
std::atomic<bool> g_checking{false};

void set_status(LinkCheck check, const std::string& address = "") {
    std::lock_guard lk(g_status_lock);
    g_status.check = check;
    g_status.address = address;
}

void upnp_thread() {
    const uint16_t port = link_port();
    while (g_checking)  // a test of the same port would close what this opens
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    set_status(LinkCheck::Running);
    Igd igd;
    if (!upnp_find(igd)) {
        log("ldn: upnp: no router answered");
        set_status(LinkCheck::NoRouter);
        return;
    }
    uint16_t external = 0;
    std::string address;
    const LinkCheck r = upnp_open(igd, port, external, address);
    if (r != LinkCheck::Open) {
        log("ldn: upnp: the port was not opened (%d)", int(r));
        set_status(r);
        return;
    }
    log("ldn: upnp: port %u opened", external);  // the address itself not in the log, which gets shared
    set_status(LinkCheck::Open, address);
    std::unique_lock lk(g_upnp_lock);
    while (!g_upnp_cv.wait_for(lk, std::chrono::minutes(30), [] { return g_upnp_stop; })) {
        lk.unlock();
        upnp_renew(igd, external, port);  // before its lease ends
        lk.lock();
    }
    lk.unlock();
    upnp_close(igd, external);
    log("ldn: upnp: port %u closed", external);
}

void check_thread() {
    set_status(LinkCheck::Running);
    std::string address;
    const LinkCheck r = upnp_check(link_port(), address);
    set_status(r, address);
    g_checking = false;
}

void upnp_start() {
    if (const char* e = std::getenv("NX_LDN_UPNP"); e && e[0] == '0') return;
    g_upnp_stop = false;
    g_upnp_thread = std::thread(upnp_thread);
}

void upnp_stop() {
    if (!g_upnp_thread.joinable()) return;
    {
        std::lock_guard lk(g_upnp_lock);
        g_upnp_stop = true;
    }
    g_upnp_cv.notify_all();
    g_upnp_thread.join();
}

}  // namespace

LinkStatus link_status() {
    std::lock_guard lk(g_status_lock);
    LinkStatus s = g_status;
    s.players = 0;
    s.hosting = g_listening;
    for (int i = 1; i < kMaxNodes; i++) s.players += g_peers[i].alive ? 1 : 0;
    return s;
}

void link_check() {
    if (g_role != kNoRole || g_checking.exchange(true)) return;  // hosting: that thread already keeps it up to date
    std::thread(check_thread).detach();
}

void link_push_info() {
    uint8_t info[kInfoSize];
    if (!ldn_link_info(info)) return;
    for (int i = 1; i < kMaxNodes; i++)
        if (g_peers[i].alive) send_frame(g_peers[i], kInfo, info, uint32_t(kInfoSize));
}

void link_host_start() {
    net_init();
    g_stopping = false;
    g_role = kHost;
    const uint16_t port = link_port();
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
#ifndef _WIN32
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (g_listen == kBadSock || bind(g_listen, reinterpret_cast<sockaddr*>(&a), sizeof a) || listen(g_listen, 8) ||
        !discovery_answer_start()) {
        log("ldn: cannot listen on port %u: this island will not be found", port);
        stop_socket(g_listen);
        return;
    }
    log("ldn: hosting on port %u", port);
    g_listening = true;
    g_accept_thread = std::thread(host_accept);
    upnp_start();
}

void link_stop() {
    upnp_stop();
    g_stopping = true;
    g_listening = false;
    discovery_answer_stop();
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
    g_role = kNoRole;
    g_stopping = false;
}

int link_join(const uint8_t* bssid, const uint8_t* name, const uint8_t* mac, const uint8_t* version, uint8_t* info) {
    sockaddr_in addr{};
    if (!discovery_address(bssid, addr)) return -1;
    g_stopping = false;
    Peer& p = g_peers[0];
    if (p.reader.joinable()) p.reader.join();
    const sock_t fd = connect_within(addr, kConnectMs * 2);
    if (fd == kBadSock) return -1;
    no_delay(fd);
    set_timeout(fd, kHandshakeMs);
    p.fd = fd;
    uint8_t join[0x28];
    std::memcpy(join, name, 0x20);
    std::memcpy(join + 0x20, mac, 6);
    std::memcpy(join + 0x26, version, 2);
    int node = -1;
    uint8_t type = 0;
    std::vector<uint8_t> f;
    if (send_frame(p, kJoin, join, sizeof join)) {
        // The answer, then the first NetworkInfo: nothing else comes before the reader starts.
        if (read_frame(fd, type, f) && type == kWelcome && f.size() == 4) {
            std::memcpy(&node, f.data(), 4);
            while (node >= 1 && node < kMaxNodes && read_frame(fd, type, f)) {
                if (type == kInfo && f.size() == kInfoSize && ldn_info_valid(f.data())) {
                    std::memcpy(info, f.data(), kInfoSize);
                    set_timeout(fd, 0);
                    g_role = kStation;
                    p.alive = true;
                    p.reader = std::thread(station_reader);
                    return node;
                }
            }
        }
    }
    {
        std::lock_guard lk(p.tx);
        stop_socket(p.fd);
    }
    return -1;
}

void ldn_send(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void* data, uint32_t len) {
    const PacketHeader h{src_ip, dst_ip, src_port, dst_port};
    if (g_role == kHost) return route(0, h, data, len);
    if (g_role != kStation) return;
    const bool all = dst_ip == ldn_broadcast_ip() || dst_ip == 0xFFFFFFFFu;
    if (all || dst_ip == ldn_own_ip()) vsock_deliver(src_ip, src_port, dst_ip, dst_port, data, len);  // (a broadcast too)
    if (dst_ip != ldn_own_ip()) send_frame(g_peers[0], kPacket, &h, sizeof h, data, len);
}

}  // namespace nx
