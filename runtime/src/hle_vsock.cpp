// nn::socket on the local play network (NX_LDN=1, docs/LOCAL_PLAY.md): the game's own traffic (Pia) is UDP between
// addresses of the LDN subnet, which only exist here. Sockets are a table of queues; datagrams go through hle_ldn.cpp.
// Errors are the Switch's (Linux numbers), as are the socket options. Stream sockets are not carried yet.
// Outside a local play session every call fails, as on a console with no network (hle_net.cpp).
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
// The design follows MHGURecomp (morerokk, MIT): runtime/net/ldn.c and vsock.c, docs/STATUS.md "Local Play over IP".

#include <nx/runtime.h>

#include "hle_ldn.h"
#include "kernel.h"

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
constexpr uint16_t kFirstEphemeral = 49152;

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
};

std::mutex g_mu;
std::condition_variable g_cv;
Sock g_socks[kMaxSocks];
uint16_t g_next_port = kFirstEphemeral;
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

bool port_used(uint16_t port) {
    for (const Sock& s : g_socks)
        if (s.used && s.port == port) return true;
    return false;
}

uint16_t ephemeral() {
    for (int i = 0; i < 16384; i++) {
        const uint16_t p = g_next_port;
        g_next_port = g_next_port == 0xFFFF ? kFirstEphemeral : uint16_t(g_next_port + 1);
        if (!port_used(p)) return p;
    }
    return 0;
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

// The first calls, to see what the game does.
void trace(const char* what, int fd, uint64_t a, uint64_t b) {
    static int n = 0;
    if (n++ < 200) log("vsock: %s(%d, %#llx, %#llx)", what, fd, (unsigned long long)a, (unsigned long long)b);
}

void Socket_(CPU& c) {
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
    trace("Close", int(c.x[0]), 0, 0);
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    std::lock_guard lk(g_mu);
    Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    *s = Sock{};
    g_cv.notify_all();
    ret(c, 0);
}

void Bind_(CPU& c) {
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    uint32_t ip;
    uint16_t port;
    if (!read_addr(c.x[1], c.x[2], ip, port)) return ret(c, fail(kEINVAL));
    log("vsock: Bind(%d, %#x:%u)", int(c.x[0]), ip, port);
    std::lock_guard lk(g_mu);
    Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    if (port && port_used(port)) return ret(c, fail(kEADDRINUSE));
    s->ip = ip;
    s->port = port ? port : ephemeral();
    ret(c, 0);
}

void Connect_(CPU& c) {
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    uint32_t ip;
    uint16_t port;
    if (!read_addr(c.x[1], c.x[2], ip, port)) return ret(c, fail(kEINVAL));
    log("vsock: Connect(%d, %#x:%u)", int(c.x[0]), ip, port);
    std::lock_guard lk(g_mu);
    Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    s->rip = ip, s->rport = port;
    if (!s->port) s->port = ephemeral();
    ret(c, 0);
}

int do_send(int fd, const void* buf, uint64_t len, uint64_t to, uint64_t tolen) {
    if (!ldn_enabled()) return fail(kENETDOWN);
    uint32_t ip = 0, src_ip;
    uint16_t port = 0, src_port;
    const bool has_to = read_addr(to, tolen, ip, port);
    {
        std::lock_guard lk(g_mu);
        Sock* s = get(fd);
        if (!s) return fail(kEBADF);
        if (!has_to) {
            if (!s->rport) return fail(kEDESTADDRREQ);
            ip = s->rip, port = s->rport;
        }
        if (!s->port) s->port = ephemeral();
        src_ip = s->ip ? s->ip : ldn_own_ip();
        src_port = s->port;
    }
    if (len > 0xFFE0) return fail(kEMSGSIZE);
    if (!ldn_net_up()) return fail(kENETUNREACH);
    ldn_send(src_ip, src_port, ip, port, buf, uint32_t(len));
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
    trace("Send", int(c.x[0]), c.x[2], 0);
    ret(c, do_send(int(c.x[0]), reinterpret_cast<const void*>(c.x[1]), c.x[2], 0, 0));
}
void SendTo_(CPU& c) {
    trace("SendTo", int(c.x[0]), c.x[2], c.x[4]);
    ret(c, do_send(int(c.x[0]), reinterpret_cast<const void*>(c.x[1]), c.x[2], c.x[4], c.x[5]));
}
void Recv_(CPU& c) { ret(c, do_recv(int(c.x[0]), reinterpret_cast<void*>(c.x[1]), c.x[2], int(c.x[3]), 0, 0)); }
void RecvFrom_(CPU& c) {
    ret(c, do_recv(int(c.x[0]), reinterpret_cast<void*>(c.x[1]), c.x[2], int(c.x[3]), c.x[4], c.x[5]));
}

// Poll(PollFd*, count, timeout ms): PollFd is the fd, then the events and the returned events (s16 each).
void Poll_(CPU& c) {
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
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    std::lock_guard lk(g_mu);
    const Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    write_addr(c.x[1], c.x[2], s->ip, s->port);
    ret(c, 0);
}

void GetPeerName_(CPU& c) {
    if (!ldn_enabled()) return ret(c, fail(kENETDOWN));
    std::lock_guard lk(g_mu);
    const Sock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    if (!s->rport) return ret(c, fail(kENOTCONN));
    write_addr(c.x[1], c.x[2], s->rip, s->rport);
    ret(c, 0);
}

void Shutdown_(CPU& c) {
    trace("Shutdown", int(c.x[0]), c.x[1], 0);
    ret(c, ldn_enabled() ? 0 : fail(kENETDOWN));
}

// Not needed so far: named in the log, in case the game comes here.
void missing(CPU& c) {
    log("vsock: %s(%#llx, %#llx, %#llx) not implemented", hle_current_name(), (unsigned long long)c.x[0],
        (unsigned long long)c.x[1], (unsigned long long)c.x[2]);
    ret(c, fail(ldn_enabled() ? kENOTSOCK : kENETDOWN));
}

void GetLastError_(CPU& c) { c.x[0] = uint64_t(uint32_t(t_errno ? t_errno : (ldn_enabled() ? 0 : kENETDOWN))); }
void SetLastError_(CPU& c) { t_errno = int(c.x[0]); }

}  // namespace

void vsock_reset() {
    std::lock_guard lk(g_mu);
    for (Sock& s : g_socks) s = Sock{};
    g_cv.notify_all();
}

void vsock_deliver(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void* data,
                   uint32_t len) {
    std::lock_guard lk(g_mu);
    const bool all = dst_ip == ldn_broadcast_ip() || dst_ip == 0xFFFFFFFFu;
    const auto* b = static_cast<const uint8_t*>(data);
    for (Sock& s : g_socks) {
        if (!s.used || s.port != dst_port) continue;
        if (s.ip && s.ip != dst_ip && !all) continue;
        if (s.rport && (s.rip != src_ip || s.rport != src_port)) continue;
        if (s.queue.size() >= kMaxQueued) continue;
        s.queue.push_back({src_ip, src_port, std::vector<uint8_t>(b, b + len)});
        if (!all) break;
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
