// Real host sockets for Nextendo Online (NEX, Pia, HTTPS). LDN keeps using hle_vsock virtual datagrams.
// The guest sees the Switch's ABI: BSD socket option numbers, Linux errno values, sockaddr {len, family, port be,
// addr be}, pollfd {int fd; s16 events; s16 revents}. Everything below translates between that and the host.
#include "hle_hostnet.h"

#include "ldn_net.h"
#include "nextendo.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "hle_ldn.h"
#include "kernel.h"

namespace nx {

namespace {

constexpr int kFdBase = 256;  // above vsock's 3..130
constexpr int kMaxSocks = 256;

// Switch errno (Linux numbering).
enum {
    kEPIPE = 32, kEBADF = 9, kEAGAIN = 11, kEINVAL = 22, kEMSGSIZE = 90, kEPROTONOSUPPORT = 93,
    kEADDRINUSE = 98, kEADDRNOTAVAIL = 99, kENETDOWN = 50, kENETUNREACH = 101, kECONNABORTED = 103,
    kECONNRESET = 104, kEISCONN = 106, kENOTCONN = 107, kETIMEDOUT = 110, kECONNREFUSED = 111,
    kEHOSTUNREACH = 113, kEALREADY = 114, kEINPROGRESS = 115, kENOTSOCK = 88,
};
enum { kSockStream = 1, kSockDgram = 2, kNonblock = 0x800, kGetFl = 3, kSetFl = 4 };
// Switch socket options (BSD numbering) and SO_NONBLOCK, Nintendo's own.
enum { kSolSocket = 0xFFFF, kIpprotoIp = 0, kIpprotoTcp = 6 };
enum {
    kSoReuseAddr = 0x4, kSoKeepAlive = 0x8, kSoBroadcast = 0x20, kSoLinger = 0x80, kSoReusePort = 0x200,
    kSoSndBuf = 0x1001, kSoRcvBuf = 0x1002, kSoSndTimeo = 0x1005, kSoRcvTimeo = 0x1006, kSoError = 0x1007,
    kSoType = 0x1008, kSoNonblock = 0x1100, kTcpNoDelay = 1,
};
enum { kMsgPeek = 2, kMsgWaitAll = 0x40, kMsgDontWait = 0x80 };
enum { kPollIn = 1, kPollPri = 2, kPollOut = 4, kPollErr = 8, kPollHup = 0x10, kPollNval = 0x20, kPollRdNorm = 0x40,
       kPollWrNorm = 0x100 };

struct HostSock {
    bool used = false;
    bool nonblock = false;
    sock_t s = kBadSock;
    int type = 0;
};

std::mutex g_mu;
HostSock g_socks[kMaxSocks];
thread_local int t_errno = 0;

int fail(int e) {
    t_errno = e;
    return -1;
}

void ret(CPU& c, int64_t r) { c.x[0] = uint64_t(r); }

// The host's last socket error as a Switch errno.
int host_errno() {
#ifdef _WIN32
    switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK: return kEAGAIN;
    case WSAEINPROGRESS: return kEINPROGRESS;
    case WSAEALREADY: return kEALREADY;
    case WSAECONNREFUSED: return kECONNREFUSED;
    case WSAECONNRESET: return kECONNRESET;
    case WSAECONNABORTED: return kECONNABORTED;
    case WSAETIMEDOUT: return kETIMEDOUT;
    case WSAENOTCONN: return kENOTCONN;
    case WSAEISCONN: return kEISCONN;
    case WSAEADDRINUSE: return kEADDRINUSE;
    case WSAEADDRNOTAVAIL: return kEADDRNOTAVAIL;
    case WSAENETDOWN: return kENETDOWN;
    case WSAENETUNREACH: return kENETUNREACH;
    case WSAEHOSTUNREACH: return kEHOSTUNREACH;
    case WSAEMSGSIZE: return kEMSGSIZE;
    case WSAESHUTDOWN: return kEPIPE;
    case WSAENOTSOCK: return kENOTSOCK;
    default: return kEINVAL;
    }
#else
    switch (errno) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return kEAGAIN;
    case EINPROGRESS: return kEINPROGRESS;
    case EALREADY: return kEALREADY;
    case ECONNREFUSED: return kECONNREFUSED;
    case ECONNRESET: return kECONNRESET;
    case ECONNABORTED: return kECONNABORTED;
    case ETIMEDOUT: return kETIMEDOUT;
    case ENOTCONN: return kENOTCONN;
    case EISCONN: return kEISCONN;
    case EADDRINUSE: return kEADDRINUSE;
    case EADDRNOTAVAIL: return kEADDRNOTAVAIL;
    case ENETDOWN: return kENETDOWN;
    case ENETUNREACH: return kENETUNREACH;
    case EHOSTUNREACH: return kEHOSTUNREACH;
    case EMSGSIZE: return kEMSGSIZE;
    case EPIPE: return kEPIPE;
    case ENOTSOCK: return kENOTSOCK;
    default: return kEINVAL;
    }
#endif
}

int host_fail() { return fail(host_errno()); }

// The host socket behind a guest fd (copied out under the lock), or kBadSock.
sock_t lookup(int fd, HostSock* out = nullptr) {
    std::lock_guard lk(g_mu);
    const int i = fd - kFdBase;
    if (i < 0 || i >= kMaxSocks || !g_socks[i].used) return kBadSock;
    if (out) *out = g_socks[i];
    return g_socks[i].s;
}

// Switch SockAddr: length, family, port and address big-endian (both already network order in memory).
bool read_sockaddr(uint64_t addr, uint64_t len, sockaddr_in& in) {
    if (!addr || len < 8) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
    if (p[1] != 2) return false;  // AF_INET only: the Switch has no IPv6
    std::memset(&in, 0, sizeof in);
    in.sin_family = AF_INET;
    std::memcpy(&in.sin_port, p + 2, 2);
    std::memcpy(&in.sin_addr.s_addr, p + 4, 4);
    return true;
}

// Writes a SockAddr; plen (u32*, may be null) holds the room on entry and the written size on return.
void write_sockaddr(uint64_t addr, uint64_t plen, const sockaddr_in& in) {
    if (!addr) return;
    uint8_t b[16] = {16, 2};
    std::memcpy(b + 2, &in.sin_port, 2);
    std::memcpy(b + 4, &in.sin_addr.s_addr, 4);
    auto* len = reinterpret_cast<uint32_t*>(plen);
    const uint32_t room = len ? *len : 16;
    std::memcpy(reinterpret_cast<void*>(addr), b, room < 16 ? room : 16);
    if (len) *len = 16;
}

// Guest memory for resolver results. kernel_alloc never frees and rounds to pages, so one arena is cut into slots.
// shortcut: a slot is reused after kSlots lookups, so a result held past that many resolves is overwritten; give
// FreeAddrInfo a real allocator if a game ever keeps results that long.
constexpr size_t kSlotSize = 256, kSlots = 64;
uint64_t resolver_slot() {
    static uint64_t base = 0;
    static size_t next = 0;
    static std::mutex mu;
    std::lock_guard lk(mu);
    if (!base) base = kernel_alloc(kSlotSize * kSlots, 0x1000);
    const uint64_t a = base + (next++ % kSlots) * kSlotSize;
    std::memset(reinterpret_cast<void*>(a), 0, kSlotSize);
    return a;
}

// IPv4 for a hostname, with the Nextendo redirect applied. False: unknown host.
bool resolve_host(const std::string& host, uint32_t& ip_be) {
    std::string redirected;
    if (nextendo::redirect_host(host, redirected)) {
        ip_be = inet_addr(redirected.c_str());
        nextendo::remember_sni(ip_be, host);
        log("hostnet: %s -> %s (Nextendo)", host.c_str(), redirected.c_str());
        return true;
    }
    net_init();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    ip_be = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    nextendo::remember_sni(ip_be, host);
    return true;
}

int map_level(int level) {
    switch (level) {
    case kSolSocket: return SOL_SOCKET;
    case kIpprotoTcp: return IPPROTO_TCP;
    default: return IPPROTO_IP;
    }
}

int map_option(int level, int name) {
    if (level == kIpprotoTcp) return name == kTcpNoDelay ? TCP_NODELAY : -1;
    if (level != kSolSocket) return -1;
    switch (name) {
    case kSoReuseAddr: return SO_REUSEADDR;
    case kSoKeepAlive: return SO_KEEPALIVE;
    case kSoBroadcast: return SO_BROADCAST;
    case kSoSndBuf: return SO_SNDBUF;
    case kSoRcvBuf: return SO_RCVBUF;
    case kSoError: return SO_ERROR;
#if defined(SO_REUSEPORT)
    case kSoReusePort: return SO_REUSEPORT;
#endif
    default: return -1;
    }
}

void set_host_timeout(sock_t s, int opt, const uint8_t* val, uint64_t len) {
    if (len < 16) return;
    int64_t t[2];
    std::memcpy(t, val, 16);  // timeval {s64 sec, s64 usec}
#ifdef _WIN32
    const DWORD v = DWORD(t[0] * 1000 + t[1] / 1000);
#else
    const timeval v{time_t(t[0]), suseconds_t(t[1])};
#endif
    setsockopt(s, SOL_SOCKET, opt, reinterpret_cast<const char*>(&v), sizeof v);
}

}  // namespace

bool hostnet_active() { return nextendo::online_services_active() && !ldn_enabled(); }

bool hostnet_owns_fd(int fd) { return lookup(fd) != kBadSock; }

void hostnet_Socket(CPU& c) {
    if (!hostnet_active()) return ret(c, fail(kENETDOWN));
    net_init();
    const int type = int(c.x[1]) & 0xFF;
    if (type != kSockStream && type != kSockDgram) return ret(c, fail(kEPROTONOSUPPORT));
    sock_t s = ::socket(AF_INET, type == kSockStream ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (s == kBadSock) return ret(c, host_fail());
    no_sigpipe(s);
#ifdef _WIN32
    if (type == kSockDgram) {
        // Windows reports an ICMP "port unreachable" from an earlier datagram as WSAECONNRESET on the next recvfrom;
        // BSD (the Switch) does not, and Pia takes the error as fatal.
        BOOL off = FALSE;
        DWORD got = 0;
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
    }
#endif
    std::lock_guard lk(g_mu);
    for (int i = 0; i < kMaxSocks; i++) {
        if (g_socks[i].used) continue;
        g_socks[i] = HostSock{true, false, s, type};
        return ret(c, i + kFdBase);
    }
    sock_close(s);
    ret(c, fail(kEINVAL));
}

namespace {

// Pia probes the NAT-check servers from a UDP socket bound to port P, closes it, and at once binds a new socket to P
// for the session. On a console the socket lives in the firmware and the router's mapping survives; here closing the
// host socket drops the mapping, so the external port the NAT check reported (and matchmaking gave the other
// player) is dead when they reach for it, and the host turns the visit down. A closed, bound, unconnected UDP socket
// is therefore kept a few seconds and handed back when its port is bound again.
struct Parked {
    sock_t s;
    uint16_t port;  // host order
    std::chrono::steady_clock::time_point until;
};
std::vector<Parked> g_parked;  // under g_mu

void reap_parked() {
    const auto now = std::chrono::steady_clock::now();
    for (size_t k = 0; k < g_parked.size();)
        if (g_parked[k].until <= now) {
            sock_close(g_parked[k].s);
            g_parked.erase(g_parked.begin() + std::ptrdiff_t(k));
        } else {
            k++;
        }
}

bool park(const HostSock& hs) {
    if (hs.type != kSockDgram) return false;
    sockaddr_in me{}, peer{};
    socklen_t l = sizeof me, pl = sizeof peer;
    if (getsockname(hs.s, reinterpret_cast<sockaddr*>(&me), &l) != 0 || !me.sin_port) return false;
    if (getpeername(hs.s, reinterpret_cast<sockaddr*>(&peer), &pl) == 0) return false;  // connected: a real teardown
    reap_parked();
    const uint16_t port = ntohs(me.sin_port);
    for (Parked& p : g_parked)
        if (p.port == port) {
            sock_close(p.s);
            p.s = hs.s;
            p.until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            return true;
        }
    if (g_parked.size() >= 16) return false;
    g_parked.push_back({hs.s, port, std::chrono::steady_clock::now() + std::chrono::seconds(3)});
    return true;
}

// The parked socket for this port, taken out of the list; kBadSock if none.
sock_t unpark(uint16_t port) {
    reap_parked();
    for (size_t k = 0; k < g_parked.size(); k++)
        if (g_parked[k].port == port) {
            const sock_t s = g_parked[k].s;
            g_parked.erase(g_parked.begin() + std::ptrdiff_t(k));
            return s;
        }
    return kBadSock;
}

}  // namespace

void hostnet_Close(CPU& c) {
    std::lock_guard lk(g_mu);
    const int i = int(c.x[0]) - kFdBase;
    if (i < 0 || i >= kMaxSocks || !g_socks[i].used) return ret(c, fail(kEBADF));
    if (!park(g_socks[i])) sock_close(g_socks[i].s);
    g_socks[i] = HostSock{};
    ret(c, 0);
}

void hostnet_Bind(CPU& c) {
    sock_t s = lookup(int(c.x[0]));
    if (s == kBadSock) return ret(c, fail(kEBADF));
    sockaddr_in in;
    if (!read_sockaddr(c.x[1], c.x[2], in)) return ret(c, fail(kEINVAL));
    {
        std::lock_guard lk(g_mu);
        const int i = int(c.x[0]) - kFdBase;
        if (in.sin_port && g_socks[i].type == kSockDgram) {
            const sock_t kept = unpark(ntohs(in.sin_port));
            if (kept != kBadSock) {  // same socket, same router mapping; the fresh one goes
                sock_close(g_socks[i].s);
                g_socks[i].s = kept;
                set_nonblocking(kept, g_socks[i].nonblock);
                return ret(c, 0);
            }
        }
    }
    if (bind(s, reinterpret_cast<sockaddr*>(&in), sizeof in) != 0) return ret(c, host_fail());
    ret(c, 0);
}

void hostnet_Connect(CPU& c) {
    sock_t s = lookup(int(c.x[0]));
    if (s == kBadSock) return ret(c, fail(kEBADF));
    sockaddr_in in;
    if (!read_sockaddr(c.x[1], c.x[2], in)) return ret(c, fail(kEINVAL));
    if (connect(s, reinterpret_cast<sockaddr*>(&in), sizeof in) == 0) return ret(c, 0);
    int e = host_errno();
    if (e == kEAGAIN) e = kEINPROGRESS;  // Windows: a non-blocking connect reports WSAEWOULDBLOCK
    ret(c, fail(e));
}

namespace {

// A blocking socket asked for MSG_DONTWAIT: the host flag does not exist on Windows, so look first.
bool would_block(sock_t s, const HostSock& hs, int flags, bool write) {
    return (flags & kMsgDontWait) && !hs.nonblock && !wait_socket(s, write, 0);
}

int host_flags(int flags) {
    int f = 0;
    if (flags & kMsgPeek) f |= MSG_PEEK;
    if (flags & kMsgWaitAll) f |= MSG_WAITALL;
    return f;
}

int64_t do_send(int fd, uint64_t buf, uint64_t n, int flags, uint64_t to, uint64_t tolen) {
    HostSock hs;
    sock_t s = lookup(fd, &hs);
    if (s == kBadSock) return fail(kEBADF);
    if (would_block(s, hs, flags, true)) return fail(kEAGAIN);
    sockaddr_in in;
    const bool has_to = to && read_sockaddr(to, tolen, in);
    const int r = has_to ? int(sendto(s, reinterpret_cast<const char*>(buf), int(n), MSG_NOSIGNAL,
                                      reinterpret_cast<sockaddr*>(&in), sizeof in))
                         : int(send(s, reinterpret_cast<const char*>(buf), int(n), MSG_NOSIGNAL));
    return r < 0 ? host_fail() : r;
}

int64_t do_recv(int fd, uint64_t buf, uint64_t n, int flags, uint64_t from, uint64_t fromlen) {
    HostSock hs;
    sock_t s = lookup(fd, &hs);
    if (s == kBadSock) return fail(kEBADF);
    if (would_block(s, hs, flags, false)) return fail(kEAGAIN);
    sockaddr_in in{};
    socklen_t inlen = sizeof in;
    const int r = int(recvfrom(s, reinterpret_cast<char*>(buf), int(n), host_flags(flags),
                               reinterpret_cast<sockaddr*>(&in), &inlen));
#ifdef _WIN32
    // A datagram bigger than the buffer: Windows fails, the Switch (BSD) truncates.
    if (r < 0 && WSAGetLastError() == WSAEMSGSIZE) return int64_t(n);
#endif
    if (r < 0) return host_fail();
    if (from && inlen >= sizeof(sockaddr_in) && in.sin_family == AF_INET) write_sockaddr(from, fromlen, in);
    return r;
}

}  // namespace

void hostnet_Send(CPU& c) { ret(c, do_send(int(c.x[0]), c.x[1], c.x[2], int(c.x[3]), 0, 0)); }
void hostnet_SendTo(CPU& c) { ret(c, do_send(int(c.x[0]), c.x[1], c.x[2], int(c.x[3]), c.x[4], c.x[5])); }
void hostnet_Recv(CPU& c) { ret(c, do_recv(int(c.x[0]), c.x[1], c.x[2], int(c.x[3]), 0, 0)); }
void hostnet_RecvFrom(CPU& c) { ret(c, do_recv(int(c.x[0]), c.x[1], c.x[2], int(c.x[3]), c.x[4], c.x[5])); }

// GetAddrInfo(node, service, hints, AddrInfo** out): one IPv4 answer.
void hostnet_GetAddrInfo(CPU& c) {
    constexpr uint64_t kEaiNoName = 8, kEaiFail = 4;
    const char* node = reinterpret_cast<const char*>(c.x[0]);
    const char* service = reinterpret_cast<const char*>(c.x[1]);
    const uint64_t hints = c.x[2], out = c.x[3];
    if (!hostnet_active() || !out) {
        c.x[0] = kEaiFail;
        return;
    }
    uint32_t ip = 0;
    if (node && node[0] && !resolve_host(node, ip)) {
        c.x[0] = kEaiNoName;
        return;
    }
    const uint16_t port = htons(uint16_t(service ? std::atoi(service) : 0));
    int32_t socktype = kSockStream, protocol = 0;
    if (hints) {
        socktype = ld<int32_t>(hints + 8) ? ld<int32_t>(hints + 8) : kSockStream;
        protocol = ld<int32_t>(hints + 12);
    }
    const uint64_t info = resolver_slot();
    const uint64_t sa = info + 64;
    sockaddr_in in{};
    in.sin_family = AF_INET;
    in.sin_port = port;
    in.sin_addr.s_addr = ip;
    write_sockaddr(sa, 0, in);
    st<int32_t>(info + 0, 0);  // ai_flags
    st<int32_t>(info + 4, 2);  // AF_INET
    st<int32_t>(info + 8, socktype);
    st<int32_t>(info + 12, protocol);
    st<uint32_t>(info + 16, 16);  // ai_addrlen
    // shortcut: whether ai_addr or ai_canonname comes first is not confirmed for this SDK; both slots point at the
    // address (a canonical name of junk is harmless), so either order works. Write ai_canonname properly if a game
    // ever reads it.
    st<uint64_t>(info + 24, sa);
    st<uint64_t>(info + 32, sa);
    st<uint64_t>(info + 40, 0);  // ai_next
    st<uint64_t>(out, info);
    c.x[0] = 0;
}

void hostnet_FreeAddrInfo(CPU& c) { c.x[0] = 0; }

// GetHostByName(name[, options, count]) -> hostent* (BSD layout with 64-bit pointers), null when unknown.
void hostnet_GetHostByName(CPU& c) {
    const char* name = reinterpret_cast<const char*>(c.x[0]);
    uint32_t ip = 0;
    if (!hostnet_active() || !name || !resolve_host(name, ip)) {
        c.x[0] = 0;
        return;
    }
    const uint64_t h = resolver_slot();
    const uint64_t addr = h + 64, list = h + 72, aliases = h + 88, hname = h + 96;
    st<uint32_t>(addr, ip);
    st<uint64_t>(list, addr);
    st<uint64_t>(list + 8, 0);
    st<uint64_t>(aliases, 0);
    std::strncpy(reinterpret_cast<char*>(hname), name, kSlotSize - 97);
    st<uint64_t>(h + 0, hname);    // h_name
    st<uint64_t>(h + 8, aliases);  // h_aliases
    st<int32_t>(h + 16, 2);        // h_addrtype AF_INET
    st<int32_t>(h + 20, 4);        // h_length
    st<uint64_t>(h + 24, list);    // h_addr_list
    c.x[0] = h;
}

void hostnet_GetLastErrno(CPU& c) { c.x[0] = uint64_t(uint32_t(t_errno)); }
void hostnet_SetLastErrno(CPU& c) {
    t_errno = int(c.x[0]);
    c.x[0] = 0;
}

void hostnet_Shutdown(CPU& c) {
    sock_t s = lookup(int(c.x[0]));
    if (s == kBadSock) return ret(c, fail(kEBADF));
    // SHUT_RD/WR/RDWR are 0/1/2 on the Switch and on every host (SD_RECEIVE/SEND/BOTH on Windows).
    if (shutdown(s, int(c.x[1])) != 0) return ret(c, host_fail());
    ret(c, 0);
}

void hostnet_SetSockOpt(CPU& c) {
    const int fd = int(c.x[0]), level = int(c.x[1]), name = int(c.x[2]);
    const auto* val = reinterpret_cast<const uint8_t*>(c.x[3]);
    const uint64_t len = c.x[4];
    sock_t s = lookup(fd);
    if (s == kBadSock) return ret(c, fail(kEBADF));
    if (level == kSolSocket && name == kSoNonblock && len >= 4) {
        uint32_t v;
        std::memcpy(&v, val, 4);
        set_nonblocking(s, v != 0);
        std::lock_guard lk(g_mu);
        g_socks[fd - kFdBase].nonblock = v != 0;
        return ret(c, 0);
    }
    if (level == kSolSocket && (name == kSoRcvTimeo || name == kSoSndTimeo)) {
        set_host_timeout(s, name == kSoRcvTimeo ? SO_RCVTIMEO : SO_SNDTIMEO, val, len);
        return ret(c, 0);
    }
    if (level == kSolSocket && name == kSoLinger && len >= 8) {
        int32_t v[2];
        std::memcpy(v, val, 8);
        linger l{};
        l.l_onoff = decltype(l.l_onoff)(v[0]);
        l.l_linger = decltype(l.l_linger)(v[1]);
        setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l);
        return ret(c, 0);
    }
    const int opt = map_option(level, name);
    if (opt >= 0 && len >= 4) {
        int32_t v;
        std::memcpy(&v, val, 4);
        setsockopt(s, map_level(level), opt, reinterpret_cast<const char*>(&v), sizeof v);
    }
    ret(c, 0);  // options the host lacks (IP TTL/TOS, ...) are accepted, as they only tune the link
}

void hostnet_GetSockOpt(CPU& c) {
    const int fd = int(c.x[0]), level = int(c.x[1]), name = int(c.x[2]);
    auto* plen = reinterpret_cast<uint32_t*>(c.x[4]);
    HostSock hs;
    sock_t s = lookup(fd, &hs);
    if (s == kBadSock) return ret(c, fail(kEBADF));
    int32_t v = 0;
    if (level == kSolSocket && name == kSoType) {
        v = hs.type;
    } else if (level == kSolSocket && name == kSoNonblock) {
        v = hs.nonblock;
    } else if (level == kSolSocket && name == kSoError) {
        int e = 0;
        socklen_t l = sizeof e;
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &l);
        if (e) {
#ifdef _WIN32
            WSASetLastError(e);
#else
            errno = e;
#endif
            v = host_errno();
        }
    } else if (const int opt = map_option(level, name); opt >= 0) {
        socklen_t l = sizeof v;
        getsockopt(s, map_level(level), opt, reinterpret_cast<char*>(&v), &l);
    }
    if (c.x[3] && plen && *plen >= 4) {
        std::memcpy(reinterpret_cast<void*>(c.x[3]), &v, 4);
        *plen = 4;
    }
    ret(c, 0);
}

void hostnet_GetSockName(CPU& c) {
    sock_t s = lookup(int(c.x[0]));
    if (s == kBadSock) return ret(c, fail(kEBADF));
    sockaddr_in in{};
    socklen_t l = sizeof in;
    if (getsockname(s, reinterpret_cast<sockaddr*>(&in), &l) != 0) return ret(c, host_fail());
    write_sockaddr(c.x[1], c.x[2], in);
    ret(c, 0);
}

void hostnet_GetPeerName(CPU& c) {
    sock_t s = lookup(int(c.x[0]));
    if (s == kBadSock) return ret(c, fail(kEBADF));
    sockaddr_in in{};
    socklen_t l = sizeof in;
    if (getpeername(s, reinterpret_cast<sockaddr*>(&in), &l) != 0) return ret(c, host_fail());
    write_sockaddr(c.x[1], c.x[2], in);
    ret(c, 0);
}

// Poll(PollFd*, count, timeout ms).
void hostnet_Poll(CPU& c) {
    auto* fds = reinterpret_cast<uint8_t*>(c.x[0]);
    const uint64_t n = c.x[1];
    const int timeout = int(c.x[2]);
    // poll(nullptr, 0, ms) is a sleep (libcurl waits for its resolver thread this way): returning at once turned that
    // wait into a busy loop that starved the resolver until curl's timeout (CURLE_OPERATION_TIMEDOUT, ens 2219-1028).
    if (n == 0) {
        if (timeout > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
        return ret(c, 0);
    }
    std::vector<pollfd> host(n);
    for (uint64_t i = 0; i < n; i++) {
        int32_t fd;
        uint16_t ev;
        std::memcpy(&fd, fds + i * 8, 4);
        std::memcpy(&ev, fds + i * 8 + 4, 2);
        sock_t s = fd >= 0 ? lookup(fd) : kBadSock;
        host[i].fd = s;
        host[i].events = 0;
        if (ev & (kPollIn | kPollRdNorm)) host[i].events |= POLLRDNORM;
        if (ev & (kPollOut | kPollWrNorm)) host[i].events |= POLLWRNORM;
        host[i].revents = 0;
    }
    // Unknown descriptors: poll/WSAPoll skip a negative fd, reported as invalid below.
#ifdef _WIN32
    const int r = n ? WSAPoll(host.data(), ULONG(n), timeout) : 0;
#else
    const int r = n ? ::poll(host.data(), nfds_t(n), timeout) : 0;
#endif
    if (r < 0) return ret(c, host_fail());
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        int32_t fd;
        uint16_t ev, re = 0;
        std::memcpy(&fd, fds + i * 8, 4);
        std::memcpy(&ev, fds + i * 8 + 4, 2);
        const short h = host[i].revents;
        if (fd >= 0 && host[i].fd == kBadSock) re = kPollNval;
        if (h & POLLRDNORM) re |= ev & (kPollIn | kPollRdNorm);
        if (h & POLLWRNORM) re |= ev & (kPollOut | kPollWrNorm);
        if (h & POLLERR) re |= kPollErr;
        if (h & POLLHUP) re |= kPollHup | (ev & (kPollIn | kPollRdNorm));  // a closed peer reads as 0 bytes
        if (h & POLLNVAL) re |= kPollNval;
        std::memcpy(fds + i * 8 + 6, &re, 2);
        if (re) ready++;
    }
    ret(c, ready);
}

void hostnet_Fcntl(CPU& c) {
    const int fd = int(c.x[0]), cmd = int(c.x[1]);
    HostSock hs;
    sock_t s = lookup(fd, &hs);
    if (s == kBadSock) return ret(c, fail(kEBADF));
    if (cmd == kGetFl) return ret(c, hs.nonblock ? kNonblock : 0);
    if (cmd == kSetFl) {
        const bool nb = (int(c.x[2]) & kNonblock) != 0;
        set_nonblocking(s, nb);
        std::lock_guard lk(g_mu);
        g_socks[fd - kFdBase].nonblock = nb;
    }
    ret(c, 0);
}

hostnet_sock_t hostnet_native_fd(int guest_fd) { return hostnet_sock_t(lookup(guest_fd)); }

bool hostnet_is_nonblocking(int guest_fd) {
    HostSock hs;
    return lookup(guest_fd, &hs) != kBadSock && hs.nonblock;
}

uint32_t hostnet_local_ip() {
    // The interface the host would use to reach Nextendo: connect a UDP socket (sends nothing) and read its address.
    net_init();
    sock_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) return 0;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(443);
    to.sin_addr.s_addr = inet_addr(nextendo::server_ip().c_str());
    sockaddr_in me{};
    socklen_t l = sizeof me;
    uint32_t ip = 0;
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0 &&
        getsockname(s, reinterpret_cast<sockaddr*>(&me), &l) == 0)
        ip = me.sin_addr.s_addr;
    sock_close(s);
    return ip;
}

}  // namespace nx
