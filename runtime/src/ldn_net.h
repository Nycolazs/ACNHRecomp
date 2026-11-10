// Sockets on the PC hosts, for local play (ldn_link.cpp, hle_vsock.cpp) and the launcher's address test (ldn_lan.cpp).
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
constexpr sock_t kBadSock = INVALID_SOCKET;
inline void sock_close(sock_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
constexpr sock_t kBadSock = -1;
inline void sock_close(sock_t s) { close(s); }
#endif

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0  // Windows has no SIGPIPE; macOS: no_sigpipe
#endif

namespace nx {

inline void net_init() {
#ifdef _WIN32
    static const bool done = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)done;
#endif
}

// A send to a player who just left must not end the game with SIGPIPE: MSG_NOSIGNAL here, this on macOS.
inline void no_sigpipe(sock_t s) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)s;
#endif
}

inline bool send_all(sock_t s, const void* p, size_t n) {
    const char* b = static_cast<const char*>(p);
    while (n) {
        const int r = int(send(s, b, int(n), MSG_NOSIGNAL));
        if (r <= 0) return false;
        b += r, n -= size_t(r);
    }
    return true;
}

// Receive timeout on a socket; 0: none.
inline void set_timeout(sock_t s, int ms) {
#ifdef _WIN32
    const DWORD v = DWORD(ms);
#else
    const timeval v{ms / 1000, (ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof v);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof v);
}

inline void set_nonblocking(sock_t s, bool on) {
#ifdef _WIN32
    u_long v = on;
    ioctlsocket(s, FIONBIO, &v);
#else
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
#endif
}

// Whether a socket gets ready within ms, to read or to write; a failed connect counts. poll on POSIX: select's set has
// no room for a descriptor past FD_SETSIZE, and the game keeps many files open.
inline bool wait_socket(sock_t s, bool write, int ms) {
#ifdef _WIN32
    fd_set set, err;
    FD_ZERO(&set);
    FD_ZERO(&err);
    FD_SET(s, &set);
    FD_SET(s, &err);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return select(0, write ? nullptr : &set, write ? &set : nullptr, &err, &tv) > 0;
#else
    pollfd p{s, short(write ? POLLOUT : POLLIN), 0};
    return poll(&p, 1, ms) > 0;
#endif
}

// connect() that gives up after ms, as an unreachable address would take long.
inline sock_t connect_within(const sockaddr_in& a, int ms) {
    const sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSock) return kBadSock;
    no_sigpipe(s);
    set_nonblocking(s, true);
    connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    int err = 1;
    socklen_t len = sizeof err;
    if (!wait_socket(s, true, ms) || getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) || err) {
        sock_close(s);
        return kBadSock;
    }
    set_nonblocking(s, false);
    return s;
}

inline bool resolve(const std::string& host, uint16_t port, sockaddr_in& out) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) || !res) return false;
    out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out.sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

inline void stop_socket(sock_t& s) {
    if (s == kBadSock) return;
#ifdef _WIN32
    shutdown(s, SD_BOTH);
#else
    shutdown(s, SHUT_RDWR);
#endif
    sock_close(s);
    s = kBadSock;
}

// ---- ldn_mitm's LAN protocol (github.com/spacemeowx2/ldn_mitm, lan_protocol.cpp), so that a Switch running ldn_mitm,
// or anything else that speaks it, plays with this one. UDP on port 11452 finds networks (Scan, answered by the host
// with ScanResp and its NetworkInfo); a station keeps one TCP connection to the host's port 11452 (Connect with its
// NodeInfo, then SyncNetwork with the NetworkInfo at every change). The game's own traffic does not go through it:
// nodes have their real addresses and send each other UDP directly.

constexpr uint16_t kLanPort = 11452;
constexpr uint32_t kLanMagic = 0x11451400;
constexpr size_t kInfoSize = 0x480, kNodeSize = 0x40, kLanHeaderSize = 12, kLanMaxPacket = 2048;
constexpr int kMaxNodes = 8;
constexpr int kScanMs = 500, kConnectMs = 1500, kHandshakeMs = 3000;

enum LanType : uint8_t { kScan = 0, kScanResp = 1, kConnect = 2, kSyncNetwork = 3 };

// Header: magic u32, type u8, compressed u8, length u16 (of what follows), decompressed length u16, 2 reserved.
// Sent uncompressed, which ldn_mitm accepts.
inline std::vector<uint8_t> lan_packet(uint8_t type, const void* data, size_t n) {
    std::vector<uint8_t> p(kLanHeaderSize + n);
    const uint16_t len = uint16_t(n);
    std::memcpy(&p[0], &kLanMagic, 4);
    p[4] = type;
    std::memcpy(&p[6], &len, 2);
    if (n) std::memcpy(&p[kLanHeaderSize], data, n);
    return p;
}

// ldn_mitm's compression: a zero byte is followed by how many more zeros come after it.
inline bool lan_decompress(const uint8_t* in, size_t n, size_t want, std::vector<uint8_t>& out) {
    out.clear();
    for (size_t i = 0; i < n;) {
        const uint8_t c = in[i++];
        out.push_back(c);
        if (c) continue;
        if (i == n) return false;
        out.insert(out.end(), in[i++], uint8_t(0));
        if (out.size() > kLanMaxPacket) return false;
    }
    return out.size() == want;
}

// A whole packet (a datagram, or header and body read from TCP): its type and its body, decompressed.
inline bool lan_parse(const uint8_t* p, size_t n, uint8_t& type, std::vector<uint8_t>& body) {
    if (n < kLanHeaderSize) return false;
    uint32_t magic;
    uint16_t len, raw_len;
    std::memcpy(&magic, p, 4);
    std::memcpy(&len, p + 6, 2);
    std::memcpy(&raw_len, p + 8, 2);
    if (magic != kLanMagic || kLanHeaderSize + len > n) return false;
    type = p[4];
    if (p[5]) return lan_decompress(p + kLanHeaderSize, len, raw_len, body);
    body.assign(p + kLanHeaderSize, p + kLanHeaderSize + len);
    return true;
}

inline bool read_all(sock_t s, void* p, size_t n) {
    char* b = static_cast<char*>(p);
    while (n) {
        const int r = int(recv(s, b, int(n), 0));
        if (r <= 0) return false;
        b += r, n -= size_t(r);
    }
    return true;
}

// One packet from a TCP connection.
inline bool lan_read(sock_t s, uint8_t& type, std::vector<uint8_t>& body) {
    uint8_t p[kLanMaxPacket];
    if (!read_all(s, p, kLanHeaderSize)) return false;
    uint16_t len;
    std::memcpy(&len, p + 6, 2);
    if (kLanHeaderSize + len > sizeof p || !read_all(s, p + kLanHeaderSize, len)) return false;
    return lan_parse(p, kLanHeaderSize + len, type, body);
}

inline sockaddr_in ipv4(uint32_t ip, uint16_t port) {  // host byte order in, a socket address out
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    return a;
}

}  // namespace nx
