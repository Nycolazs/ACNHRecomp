// Sockets on the PC hosts, for local play's link (ldn_link.cpp) and its router requests (ldn_upnp.cpp).
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

// ---- the link's protocol

constexpr uint16_t kDefaultPort = 39500;
constexpr char kProbe[] = "ACNHLDN1?", kAnswer[] = "ACNHLDN1!";
constexpr size_t kMagicSize = 9, kInfoSize = 0x480;
constexpr int kMaxNodes = 8;
constexpr uint32_t kMaxFrame = 0x10100;
constexpr int kScanMs = 300, kConnectMs = 1500, kHandshakeMs = 3000;
constexpr uint32_t kJoinSize = 0x28;

enum Frame : uint8_t { kJoin = 1, kWelcome = 2, kInfo = 3, kPacket = 4, kRefused = 5, kProbeFrame = 6 };

inline uint16_t link_port() {
    const char* e = std::getenv("NX_LDN_PORT");
    const int p = e ? std::atoi(e) : 0;
    return p > 0 && p < 65536 ? uint16_t(p) : kDefaultPort;
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

// A frame: its type, the size of what follows (u32), then that.
inline bool read_frame(sock_t s, uint8_t& type, std::vector<uint8_t>& out) {
    uint8_t h[5];
    if (!read_all(s, h, 5)) return false;
    uint32_t size;
    std::memcpy(&size, h + 1, 4);
    if (size > kMaxFrame) return false;
    type = h[0];
    out.resize(size);
    return !size || read_all(s, out.data(), size);
}

bool discovery_address(const uint8_t* bssid, sockaddr_in& out);  // ldn_discovery.cpp: where a scanned network is

}  // namespace nx
