// nn::socket on the local play network (NX_LDN=1, docs/LOCAL_PLAY.md): the game's own traffic (Pia) is UDP between the
// nodes' real addresses, as with ldn_mitm. Each game socket is a real UDP socket at the same port, whose datagrams a
// thread puts in its queue; a broadcast goes to each other node of the network (a virtual LAN carries no broadcast).
// Errors are the Switch's (Linux numbers), as are the socket options. Stream sockets are not carried yet.
// The sockets listen on every interface, so a datagram is only taken from a node of the network (ldn_peer_ips): Pia
// is not meant to face strangers, and on a public network anyone could otherwise send it packets.
// Outside a local play session every call fails, as on a console with no network (hle_net.cpp).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
// The design follows MHGURecomp (morerokk, MIT): runtime/net/ldn.c and vsock.c, docs/STATUS.md "Local Play over IP".

#include <nx/runtime.h>

#include "hle_hostnet.h"
#include "hle_ldn.h"
#include "kernel.h"
#include "ldn_net.h"

namespace nx {

namespace {

enum { kEBADF = 9, kEAGAIN = 11, kEINVAL = 22, kENOTSOCK = 88, kEDESTADDRREQ = 89, kEMSGSIZE = 90,
       kEPROTONOSUPPORT = 93, kEADDRINUSE = 98, kENETDOWN = 50, kENETUNREACH = 101, kENOTCONN = 107 };
enum { kSockDgram = 2, kNonblock = 0x800, kGetFl = 3, kSetFl = 4 };
enum { kSolSocket = 0xFFFF, kSoRcvTimeo = 0x1006, kSoError = 0x1007, kSoType = 0x1008, kSoNonblock = 0x1100 };
enum { kMsgPeek = 2, kMsgDontWait = 0x80 };
enum { kPollIn = 1, kPollOut = 4, kPollNval = 0x20, kPollRdNorm = 0x40 };

constexpr int kFdBase = 3, kMaxSocks = 128;
constexpr size_t kMaxQueued = 512;

struct Datagram {
    uint32_t ip;
    uint16_t port;
    std::vector<uint8_t> data;
};

struct Sock {
    bool used = false, nonblock = false;
    uint32_t ip = 0, rip = 0;  // bound / connected address
    uint16_t port = 0, rport = 0;
    uint32_t rcv_timeout_ms = 0;
    std::deque<Datagram> queue;
    sock_t real = kBadSock;  // the PC's socket, from the first Bind, Connect or send
    uint32_t gen = 0;        // which real socket its reader thread serves
};

std::mutex g_mu;
std::condition_variable g_cv;
Sock g_socks[kMaxSocks];
uint32_t g_gen = 0;
thread_local int t_errno = 0;

int fail(int e) {
    t_errno = e;
    return -1;
}

void ret(CPU& c, int r) { c.x[0] = uint64_t(int64_t(r)); }

Sock* get(int fd) {
    const int i = fd - kFdBase;
    return i >= 0 && i < kMaxSocks && g_socks[i].used ? &g_socks[i] : nullptr;
}

// SockAddr: length, family, port and address big-endian.
bool read_addr(uint64_t a, uint64_t len, uint32_t& ip, uint16_t& port) {
    if (!a || len < 8) return false;
    const auto* b = reinterpret_cast<const uint8_t*>(a);
    port = uint16_t(b[2] << 8 | b[3]);
    ip = uint32_t(b[4]) << 24 | uint32_t(b[5]) << 16 | uint32_t(b[6]) << 8 | b[7];
    return true;
}

void write_addr(uint64_t a, uint64_t plen, uint32_t ip, uint16_t port) {
    if (!a) return;
    const uint8_t b[16] = {16, 2, uint8_t(port >> 8), uint8_t(port), uint8_t(ip >> 24), uint8_t(ip >> 16),
                           uint8_t(ip >> 8), uint8_t(ip)};
    auto* len = reinterpret_cast<uint32_t*>(plen);
    const uint32_t n = len ? *len : 16;
    std::memcpy(reinterpret_cast<void*>(a), b, n < 16 ? n : 16);
    if (len) *len = 16;
}

void close_real(Sock& s) {  // g_mu held; its reader sees gen change and stops
    // shortcut: the reader may be in recvfrom on this handle when it closes, and a socket opened just after could get the
    // same number and lose one datagram to it; a single poll thread for every socket would avoid that.
    if (s.real != kBadSock) stop_socket(s.real);
    s.gen = 0;
}

void reader(int fd, uint32_t gen, sock_t r) {
    std::vector<uint8_t> buf(0x10000);
    for (;;) {
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        const int n = int(recvfrom(r, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fl));
        uint32_t peers[8];
        const int np = n < 0 ? 0 : ldn_peer_ips(peers);  // outside g_mu, as do_send asks
        std::lock_guard lk(g_mu);
        Sock* s = get(fd);
        if (!s || s->gen != gen) return;
        if (n < 0) continue;  // a timeout, or Windows' WSAECONNRESET from an earlier send
        const uint32_t ip = ntohl(from.sin_addr.s_addr);
        const uint16_t port = ntohs(from.sin_port);
        if (std::find(peers, peers + np, ip) == peers + np) continue;  // not a node of this network
        if (s->rport && (s->rip != ip || s->rport != port)) continue;
        if (s->queue.size() >= kMaxQueued) continue;
        s->queue.push_back({ip, port, std::vector<uint8_t>(buf.begin(), buf.begin() + n)});
        g_cv.notify_all();
    }
}

// The real socket behind a game socket, at its port (0: one the system picks): 0, or the Switch's errno.
int open_real(int fd, Sock& s) {  // g_mu held
    if (s.real != kBadSock) return 0;
    net_init();
    const sock_t r = socket(AF_INET, SOCK_DGRAM, 0);
    if (r == kBadSock) return kENETDOWN;
    int one = 1;
    setsockopt(r, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
    sockaddr_in a = ipv4(lan_bind_ip(), s.port);
    socklen_t len = sizeof a;
    if (bind(r, reinterpret_cast<const sockaddr*>(&a), sizeof a) ||
        getsockname(r, reinterpret_cast<sockaddr*>(&a), &len)) {
        sock_close(r);
        log("vsock: port %u is taken on this computer", s.port);
        return kEADDRINUSE;
    }
    set_timeout(r, 250);  // the reader sees its socket closed even where closing does not wake it
    s.port = ntohs(a.sin_port);
    s.real = r;
    s.gen = ++g_gen;
    std::thread(reader, fd, s.gen, r).detach();
    return 0;
}

void deliver_local(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const void* data, uint32_t len) {  // g_mu held
    const auto* b = static_cast<const uint8_t*>(data);
    for (Sock& s : g_socks)
        if (s.used && s.port == dst_port && s.queue.size() < kMaxQueued && !(s.rport && (s.rip != src_ip || s.rport != src_port)))
            s.queue.push_back({src_ip, src_port, std::vector<uint8_t>(b, b + len)});
    g_cv.notify_all();
}

// The first calls, to see what the game does.
void trace(const char* what, int fd, uint64_t a, uint64_t b) {
    static int n = 0;
    if (n++ < 200) log("vsock: %s(%d, %#llx, %#llx)", what, fd, (unsigned long long)a, (unsigned long long)b);
}

void Socket_(CPU& c) {
    if (hostnet_active()) return hostnet_Socket(c);
    const int type = int(c.x[1]) & 0xFF;
    log("vsock: Socket(family %d, type %d, protocol %d)", int(c.x[0]), int(c.x[1]), int(c.x[2]));
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    if (type != kSockDgram) return ret(c, fail(kEPROTONOSUPPORT));
    std::lock_guard lk(g_mu);
    for (int i = 0; i < kMaxSocks; i++) {
        if (g_socks[i].used) continue;
        g_socks[i] = Sock{};
        g_socks[i].used = true;
        return ret(c, i + kFdBase);
    }
    ret(c, fail(kEINVAL));
}

void Close_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0])) || (hostnet_active() && !ldn_enabled())) return hostnet_Close(c);
    trace("Close", int(c.x[0]), 0, 0);
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    std::lock_guard lk(g_mu);
    Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    close_real(*s);
    *s = Sock{};
    g_cv.notify_all();
    ret(c, 0);
}

void Bind_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0])) || (hostnet_active() && !ldn_enabled())) return hostnet_Bind(c);
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    uint32_t ip;
    uint16_t port;
    if (!read_addr(c.x[1], c.x[2], ip, port)) return ret(c, fail(kEINVAL));
    log("vsock: Bind(%d, %#x:%u)", int(c.x[0]), ip, port);
    std::lock_guard lk(g_mu);
    Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    if (s->real != kBadSock) return ret(c, fail(kEINVAL));
    s->ip = ip;
    s->port = port;
    if (const int e = open_real(int(c.x[0]), *s)) return ret(c, fail(e));
    ret(c, 0);
}

void Connect_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0])) || (hostnet_active() && !ldn_enabled())) return hostnet_Connect(c);
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    uint32_t ip;
    uint16_t port;
    if (!read_addr(c.x[1], c.x[2], ip, port)) return ret(c, fail(kEINVAL));
    log("vsock: Connect(%d, %#x:%u)", int(c.x[0]), ip, port);
    std::lock_guard lk(g_mu);
    Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    s->rip = ip, s->rport = port;
    if (const int e = open_real(int(c.x[0]), *s)) return ret(c, fail(e));
    ret(c, 0);
}

int do_send(int fd, const void* buf, uint64_t len, uint64_t to, uint64_t tolen) {
    if (!ldn_enabled()) return fail(kENETDOWN);
    uint32_t ip = 0;
    uint16_t port = 0;
    sock_t real;
    const bool has_to = read_addr(to, tolen, ip, port);
    if (len > 0xFFE0) return fail(kEMSGSIZE);
    if (!ldn_net_up()) return fail(kENETUNREACH);
    uint32_t to_ips[8];
    const bool all = ip == ldn_broadcast_ip() || ip == 0xFFFFFFFFu;
    const int n = all ? ldn_peer_ips(to_ips) : 1;
    if (!all) to_ips[0] = ip;
    {
        std::lock_guard lk(g_mu);
        Sock* s = get(fd);
        if (!s) return fail(kEBADF);
        if (!has_to) {
            if (!s->rport) return fail(kEDESTADDRREQ);
            ip = s->rip, port = s->rport;
        }
        if (const int e = open_real(fd, *s)) return fail(e);
        real = s->real;
        if (all) deliver_local(ldn_own_ip(), s->port, port, buf, uint32_t(len));  // this node hears its broadcast too
    }
    for (int i = 0; i < n; i++) {
        const sockaddr_in a = ipv4(to_ips[i], port);
        sendto(real, static_cast<const char*>(buf), int(len), 0, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    }
    return int(len);
}

int do_recv(int fd, void* buf, uint64_t len, int flags, uint64_t from, uint64_t fromlen) {
    if (!ldn_enabled()) return fail(kENETDOWN);
    std::unique_lock lk(g_mu);
    Sock* s = get(fd);
    if (!s) return fail(kEBADF);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(s->rcv_timeout_ms);
    for (;;) {
        s = get(fd);
        if (!s) return fail(kEBADF);
        if (!s->queue.empty()) {
            Datagram& d = s->queue.front();
            const size_t n = d.data.size() < len ? d.data.size() : size_t(len);
            if (n) std::memcpy(buf, d.data.data(), n);
            write_addr(from, fromlen, d.ip, d.port);
            if (!(flags & kMsgPeek)) s->queue.pop_front();
            return int(n);
        }
        if (s->nonblock || (flags & kMsgDontWait)) return fail(kEAGAIN);
        if (!s->rcv_timeout_ms)
            g_cv.wait(lk);
        else if (g_cv.wait_until(lk, end) == std::cv_status::timeout)
            return fail(kEAGAIN);
    }
}

void Send_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0]))) return hostnet_Send(c);
    trace("Send", int(c.x[0]), c.x[2], 0);
    ret(c, do_send(int(c.x[0]), reinterpret_cast<const void*>(c.x[1]), c.x[2], 0, 0));
}
void SendTo_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0]))) return hostnet_SendTo(c);
    trace("SendTo", int(c.x[0]), c.x[2], c.x[4]);
    ret(c, do_send(int(c.x[0]), reinterpret_cast<const void*>(c.x[1]), c.x[2], c.x[4], c.x[5]));
}
void Recv_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0]))) return hostnet_Recv(c);
    ret(c, do_recv(int(c.x[0]), reinterpret_cast<void*>(c.x[1]), c.x[2], int(c.x[3]), 0, 0));
}
void RecvFrom_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0]))) return hostnet_RecvFrom(c);
    ret(c, do_recv(int(c.x[0]), reinterpret_cast<void*>(c.x[1]), c.x[2], int(c.x[3]), c.x[4], c.x[5]));
}

// Poll(PollFd*, count, timeout ms): PollFd is the fd, then the events and the returned events (s16 each).
void Poll_(CPU& c) {
    if (hostnet_active() && !ldn_enabled()) return hostnet_Poll(c);
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    auto* fds = reinterpret_cast<uint8_t*>(c.x[0]);
    const uint64_t n = c.x[1];
    const int timeout = int(c.x[2]);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout > 0 ? timeout : 0);
    std::unique_lock lk(g_mu);
    for (;;) {
        int ready = 0;
        for (uint64_t i = 0; i < n; i++) {
            uint8_t* pf = fds + i * 8;
            int32_t fd;
            uint16_t ev, re = 0;
            std::memcpy(&fd, pf, 4);
            std::memcpy(&ev, pf + 4, 2);
            if (fd >= 0) {
                const Sock* s = get(fd);
                if (!s) {
                    re = kPollNval;
                } else {
                    if (!s->queue.empty()) re |= ev & (kPollIn | kPollRdNorm);
                    re |= ev & kPollOut;
                }
            }
            std::memcpy(pf + 6, &re, 2);
            if (re) ready++;
        }
        if (ready || timeout == 0) return ret(c, ready);
        if (timeout < 0)
            g_cv.wait(lk);
        else if (g_cv.wait_until(lk, end) == std::cv_status::timeout)
            return ret(c, 0);
    }
}

void Fcntl_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0]))) return hostnet_Fcntl(c);
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    std::lock_guard lk(g_mu);
    Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    const int cmd = int(c.x[1]);
    if (cmd == kGetFl) return ret(c, s->nonblock ? kNonblock : 0);
    if (cmd == kSetFl)
        s->nonblock = (c.x[2] & kNonblock) != 0;
    else
        log("vsock: Fcntl(%d, cmd %d, %#llx) ignored", int(c.x[0]), cmd, (unsigned long long)c.x[2]);
    ret(c, 0);
}

void SetSockOpt_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0]))) return hostnet_SetSockOpt(c);
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    const int level = int(c.x[1]), name = int(c.x[2]);
    const uint64_t len = c.x[4];
    const auto* val = reinterpret_cast<const uint8_t*>(c.x[3]);
    std::lock_guard lk(g_mu);
    Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    if (level == kSolSocket && name == kSoNonblock && len >= 4) {
        uint32_t v;
        std::memcpy(&v, val, 4);
        s->nonblock = v != 0;
    } else if (level == kSolSocket && name == kSoRcvTimeo && len >= 16) {
        uint64_t t[2];
        std::memcpy(t, val, 16);
        s->rcv_timeout_ms = uint32_t(t[0] * 1000 + t[1] / 1000);
    } else {
        log("vsock: SetSockOpt(%d, level %#x, name %#x, len %llu) accepted", int(c.x[0]), level, name,
            (unsigned long long)len);
    }
    ret(c, 0);
}

void GetSockOpt_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0]))) return hostnet_GetSockOpt(c);
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    const int level = int(c.x[1]), name = int(c.x[2]);
    std::lock_guard lk(g_mu);
    const Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    uint32_t v = 0;
    if (level == kSolSocket && name == kSoType)
        v = kSockDgram;
    else if (level == kSolSocket && name == kSoNonblock)
        v = s->nonblock;
    else if (level == kSolSocket && (name == 0x1001 || name == 0x1002))
        v = 0x10000;  // SO_SNDBUF, SO_RCVBUF
    else if (!(level == kSolSocket && name == kSoError))
        log("vsock: GetSockOpt(%d, level %#x, name %#x) answered 0", int(c.x[0]), level, name);
    auto* plen = reinterpret_cast<uint32_t*>(c.x[4]);
    if (c.x[3] && plen && *plen >= 4) {
        std::memcpy(reinterpret_cast<void*>(c.x[3]), &v, 4);
        *plen = 4;
    }
    ret(c, 0);
}

void GetSockName_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0]))) return hostnet_GetSockName(c);
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    std::lock_guard lk(g_mu);
    const Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    write_addr(c.x[1], c.x[2], s->ip, s->port);
    ret(c, 0);
}

void GetPeerName_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0]))) return hostnet_GetPeerName(c);
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    std::lock_guard lk(g_mu);
    const Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    if (!s->rport) return ret(c, fail(kENOTCONN));
    write_addr(c.x[1], c.x[2], s->rip, s->rport);
    ret(c, 0);
}

void Shutdown_(CPU& c) {
    if (hostnet_owns_fd(int(c.x[0]))) return hostnet_Shutdown(c);
    trace("Shutdown", int(c.x[0]), c.x[1], 0);
    ret(c, ldn_enabled() ? 0 : fail(kENETDOWN));
}

// Not needed so far: named in the log, in case the game comes here.
void missing(CPU& c) {
    log("vsock: %s(%#llx, %#llx, %#llx) not implemented", hle_current_name(), (unsigned long long)c.x[0],
        (unsigned long long)c.x[1], (unsigned long long)c.x[2]);
    ret(c, fail(ldn_enabled() ? kENOTSOCK : kENETDOWN));
}

void GetLastError_(CPU& c) {
    if (hostnet_active() && !ldn_enabled()) return hostnet_GetLastErrno(c);
    c.x[0] = uint64_t(uint32_t(t_errno ? t_errno : (ldn_enabled() ? 0 : kENETDOWN)));
}
void SetLastError_(CPU& c) {
    if (hostnet_active() && !ldn_enabled()) return hostnet_SetLastErrno(c);
    t_errno = int(c.x[0]);
}

}  // namespace

void vsock_reset() {
    std::lock_guard lk(g_mu);
    for (Sock& s : g_socks) {
        close_real(s);
        s = Sock{};
    }
    g_cv.notify_all();
}

NX_HLE("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", Socket_);
NX_HLE("_ZN2nn6socket6SocketEiii", Socket_);
NX_HLE("_ZN2nn6socket12SocketExemptENS0_6FamilyENS0_4TypeENS0_8ProtocolE", Socket_);
NX_HLE("_ZN2nn6socket12SocketExemptEiii", Socket_);
NX_HLE("nnsocketSocket", Socket_);
NX_HLE("_ZN2nn6socket5CloseEi", Close_);
NX_HLE("nnsocketClose", Close_);
NX_HLE("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", Bind_);
NX_HLE("_ZN2nn6socket4BindEiPK8sockaddrj", Bind_);
NX_HLE("nnsocketBind", Bind_);
NX_HLE("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", Connect_);
NX_HLE("_ZN2nn6socket7ConnectEiPK8sockaddrj", Connect_);
NX_HLE("nnsocketConnect", Connect_);
NX_HLE("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", Send_);
NX_HLE("_ZN2nn6socket4SendEiPKvmi", Send_);
NX_HLE("nnsocketSend", Send_);
NX_HLE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", SendTo_);
NX_HLE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", SendTo_);
NX_HLE("nnsocketSendTo", SendTo_);
NX_HLE("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", Recv_);
NX_HLE("_ZN2nn6socket4RecvEiPvmi", Recv_);
NX_HLE("nnsocketRecv", Recv_);
NX_HLE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", RecvFrom_);
NX_HLE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", RecvFrom_);
NX_HLE("nnsocketRecvFrom", RecvFrom_);
NX_HLE("_ZN2nn6socket4PollEPNS0_6PollFdEmi", Poll_);
NX_HLE("_ZN2nn6socket4PollEP6pollfdmi", Poll_);
NX_HLE("nnsocketPoll", Poll_);
NX_HLE("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", Fcntl_);
NX_HLE("_ZN2nn6socket5FcntlEiiz", Fcntl_);
NX_HLE("nnsocketFcntl", Fcntl_);
NX_HLE("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", SetSockOpt_);
NX_HLE("_ZN2nn6socket10SetSockOptEiiiPKvj", SetSockOpt_);
NX_HLE("nnsocketSetSockOpt", SetSockOpt_);
NX_HLE("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", GetSockOpt_);
NX_HLE("_ZN2nn6socket10GetSockOptEiiiPvPj", GetSockOpt_);
NX_HLE("nnsocketGetSockOpt", GetSockOpt_);
NX_HLE("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", GetSockName_);
NX_HLE("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", GetSockName_);
NX_HLE("nnsocketGetSockName", GetSockName_);
NX_HLE("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", GetPeerName_);
NX_HLE("_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", GetPeerName_);
NX_HLE("nnsocketGetPeerName", GetPeerName_);
NX_HLE("_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE", Shutdown_);
NX_HLE("_ZN2nn6socket8ShutdownEii", Shutdown_);
NX_HLE("nnsocketShutdown", Shutdown_);
NX_HLE("_ZN2nn6socket6ListenEii", missing);
NX_HLE("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", missing);
NX_HLE("_ZN2nn6socket6AcceptEiP8sockaddrPj", missing);
NX_HLE("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", missing);
NX_HLE("_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval", missing);
NX_HLE("_ZN2nn6socket7RecvMsgEiPNS0_6MsgHdrENS0_7MsgFlagE", missing);
NX_HLE("_ZN2nn6socket7RecvMsgEiP6msghdri", missing);
NX_HLE("_ZN2nn6socket7SendMsgEiPKNS0_6MsgHdrENS0_7MsgFlagE", missing);
NX_HLE("_ZN2nn6socket7SendMsgEiPK6msghdri", missing);
NX_HLE("_ZN2nn6socket5IoctlEiNS0_12IoctlCommandEPvm", missing);
NX_HLE("_ZN2nn6socket5IoctlEijPvm", missing);
NX_HLE("_ZN2nn6socket12GetLastErrorEv", GetLastError_);
NX_HLE("_ZN2nn6socket12GetLastErrnoEv", GetLastError_);
NX_HLE("nnsocketGetLastErrno", GetLastError_);
NX_HLE("_ZN2nn6socket12SetLastErrorENS0_5ErrnoE", SetLastError_);
NX_HLE("_ZN2nn6socket12SetLastErrnoEi", SetLastError_);
NX_HLE("nnsocketSetLastErrno", SetLastError_);

}  // namespace nx
