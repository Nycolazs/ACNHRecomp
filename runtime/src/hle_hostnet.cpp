// Real host sockets for Nextendo Online (Dodo / Search). LDN keeps using hle_vsock virtual datagrams.
#include "hle_hostnet.h"

#include "ldn_net.h"
#include "nextendo.h"

#include <cstring>
#include <mutex>
#include <vector>

#include <nx/runtime.h>

#include "hle_ldn.h"
#include "kernel.h"

namespace nx {

namespace {

constexpr int kFdBase = 256;  // above vsock's 3..130
constexpr int kMaxSocks = 256;
enum { kEBADF = 9, kEAGAIN = 11, kEINVAL = 22, kENETDOWN = 50, kECONNREFUSED = 111, kEINPROGRESS = 115 };
enum { kSockStream = 1, kSockDgram = 2, kNonblock = 0x800 };

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

void ret(CPU& c, int r) { c.x[0] = uint64_t(int64_t(r)); }

HostSock* get(int fd) {
    const int i = fd - kFdBase;
    return i >= 0 && i < kMaxSocks && g_socks[i].used ? &g_socks[i] : nullptr;
}

bool read_sockaddr(uint64_t addr, uint64_t len, sockaddr_storage& ss, socklen_t& sslen) {
    if (!addr || len < 8) return false;
    std::memset(&ss, 0, sizeof ss);
    // Switch sockaddr: family u8, port u16 be, addr...
    const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
    uint8_t family = p[1];  // nn::socket::SockAddr layout often has len then family
    // Try both BSD style (family at 0) and nn style (len, family).
    uint16_t port = 0;
    uint32_t ip = 0;
    if (p[0] == 2 || p[1] == 2) {  // AF_INET
        if (p[0] == 2) {
            std::memcpy(&port, p + 2, 2);
            std::memcpy(&ip, p + 4, 4);
        } else {
            std::memcpy(&port, p + 2, 2);
            std::memcpy(&ip, p + 4, 4);
        }
        sockaddr_in* in = reinterpret_cast<sockaddr_in*>(&ss);
        in->sin_family = AF_INET;
        in->sin_port = port;
        in->sin_addr.s_addr = ip;
        sslen = sizeof(sockaddr_in);
        return true;
    }
    return false;
}

void write_sockaddr_in(uint64_t addr, uint64_t len, uint32_t ip_be, uint16_t port_be) {
    if (!addr || len < 8) return;
    uint8_t* p = reinterpret_cast<uint8_t*>(addr);
    std::memset(p, 0, size_t(len));
    p[0] = 8;
    p[1] = 2;  // AF_INET
    std::memcpy(p + 2, &port_be, 2);
    std::memcpy(p + 4, &ip_be, 4);
}

}  // namespace

bool hostnet_active() { return nextendo::online_services_active() && !ldn_enabled(); }

bool hostnet_owns_fd(int fd) {
    std::lock_guard lk(g_mu);
    return get(fd) != nullptr;
}

// Ensure hostnet_active is visible before use in handlers (already defined above).

void hostnet_Socket(CPU& c) {
    if (!hostnet_active()) return ret(c, fail(kENETDOWN));
    net_init();
    const int type = int(c.x[1]) & 0xFF;
    const int host_type = (type == kSockStream) ? SOCK_STREAM : SOCK_DGRAM;
    sock_t s = ::socket(AF_INET, host_type, 0);
    if (s == kBadSock) return ret(c, fail(kEINVAL));
    no_sigpipe(s);
    std::lock_guard lk(g_mu);
    for (int i = 0; i < kMaxSocks; i++) {
        if (g_socks[i].used) continue;
        g_socks[i] = HostSock{};
        g_socks[i].used = true;
        g_socks[i].s = s;
        g_socks[i].type = type;
        return ret(c, i + kFdBase);
    }
    sock_close(s);
    ret(c, fail(kEINVAL));
}

void hostnet_Close(CPU& c) {
    std::lock_guard lk(g_mu);
    HostSock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    sock_close(s->s);
    *s = HostSock{};
    ret(c, 0);
}

void hostnet_Bind(CPU& c) {
    std::lock_guard lk(g_mu);
    HostSock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    sockaddr_storage ss{};
    socklen_t sslen = 0;
    if (!read_sockaddr(c.x[1], c.x[2], ss, sslen)) return ret(c, fail(kEINVAL));
    if (bind(s->s, reinterpret_cast<sockaddr*>(&ss), sslen) != 0) return ret(c, fail(kEINVAL));
    ret(c, 0);
}

void hostnet_Connect(CPU& c) {
    HostSock* s = nullptr;
    {
        std::lock_guard lk(g_mu);
        s = get(int(c.x[0]));
        if (!s) return ret(c, fail(kEBADF));
    }
    sockaddr_storage ss{};
    socklen_t sslen = 0;
    if (!read_sockaddr(c.x[1], c.x[2], ss, sslen)) return ret(c, fail(kEINVAL));
    if (ss.ss_family == AF_INET) {
        auto* in = reinterpret_cast<sockaddr_in*>(&ss);
        std::string host = nextendo::host_for_ip(in->sin_addr.s_addr);
        if (host.empty()) {
            // still fine
        }
    }
    int r = connect(s->s, reinterpret_cast<sockaddr*>(&ss), sslen);
    if (r != 0) {
#ifdef _WIN32
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS) return ret(c, fail(kEINPROGRESS));
#else
        if (errno == EINPROGRESS || errno == EWOULDBLOCK) return ret(c, fail(kEINPROGRESS));
#endif
        return ret(c, fail(kECONNREFUSED));
    }
    ret(c, 0);
}

void hostnet_Send(CPU& c) {
    HostSock* s = nullptr;
    {
        std::lock_guard lk(g_mu);
        s = get(int(c.x[0]));
        if (!s) return ret(c, fail(kEBADF));
    }
    const void* buf = reinterpret_cast<const void*>(c.x[1]);
    size_t n = size_t(c.x[2]);
    int r = int(send(s->s, reinterpret_cast<const char*>(buf), int(n), MSG_NOSIGNAL));
    if (r < 0) return ret(c, fail(kEAGAIN));
    ret(c, r);
}

void hostnet_SendTo(CPU& c) {
    HostSock* s = nullptr;
    {
        std::lock_guard lk(g_mu);
        s = get(int(c.x[0]));
        if (!s) return ret(c, fail(kEBADF));
    }
    const void* buf = reinterpret_cast<const void*>(c.x[1]);
    size_t n = size_t(c.x[2]);
    sockaddr_storage ss{};
    socklen_t sslen = 0;
    if (!read_sockaddr(c.x[4], c.x[5], ss, sslen)) return ret(c, fail(kEINVAL));
    int r = int(sendto(s->s, reinterpret_cast<const char*>(buf), int(n), MSG_NOSIGNAL,
                       reinterpret_cast<sockaddr*>(&ss), sslen));
    if (r < 0) return ret(c, fail(kEAGAIN));
    ret(c, r);
}

void hostnet_Recv(CPU& c) {
    HostSock* s = nullptr;
    {
        std::lock_guard lk(g_mu);
        s = get(int(c.x[0]));
        if (!s) return ret(c, fail(kEBADF));
    }
    void* buf = reinterpret_cast<void*>(c.x[1]);
    size_t n = size_t(c.x[2]);
    int r = int(recv(s->s, reinterpret_cast<char*>(buf), int(n), 0));
    if (r < 0) return ret(c, fail(kEAGAIN));
    ret(c, r);
}

void hostnet_RecvFrom(CPU& c) {
    HostSock* s = nullptr;
    {
        std::lock_guard lk(g_mu);
        s = get(int(c.x[0]));
        if (!s) return ret(c, fail(kEBADF));
    }
    void* buf = reinterpret_cast<void*>(c.x[1]);
    size_t n = size_t(c.x[2]);
    sockaddr_storage ss{};
    socklen_t sslen = sizeof ss;
    int r = int(recvfrom(s->s, reinterpret_cast<char*>(buf), int(n), 0, reinterpret_cast<sockaddr*>(&ss), &sslen));
    if (r < 0) return ret(c, fail(kEAGAIN));
    if (c.x[4] && ss.ss_family == AF_INET) {
        auto* in = reinterpret_cast<sockaddr_in*>(&ss);
        write_sockaddr_in(c.x[4], c.x[5] ? c.x[5] : 16, in->sin_addr.s_addr, in->sin_port);
    }
    ret(c, r);
}

void hostnet_GetAddrInfo(CPU& c) {
    if (!hostnet_active()) {
        c.x[0] = 4;  // EAI_FAIL
        return;
    }
    net_init();
    const char* node = reinterpret_cast<const char*>(c.x[0]);
    const char* service = reinterpret_cast<const char*>(c.x[1]);
    // hints at x2, out at x3
    std::string host = node ? node : "";
    std::string ip;
    if (nextendo::redirect_host(host, ip)) {
        nextendo::remember_sni(inet_addr(ip.c_str()), host);
        // Allocate a minimal AddrInfo chain in guest heap.
        // nn::socket::AddrInfo: next*, canon*, ai_flags, family, socktype, protocol, addrlen, sockaddr*
        // Simplified: write one IPv4 result.
        uint64_t out_ptr = c.x[3];
        if (!out_ptr) {
            c.x[0] = 4;
            return;
        }
        uint64_t info = kernel_alloc(64, 8);
        uint64_t sa = kernel_alloc(16, 8);
        uint8_t* p = reinterpret_cast<uint8_t*>(sa);
        std::memset(p, 0, 16);
        p[0] = 8;
        p[1] = 2;
        uint16_t port = 0;
        if (service) port = htons(uint16_t(std::atoi(service)));
        std::memcpy(p + 2, &port, 2);
        uint32_t addr = inet_addr(ip.c_str());
        std::memcpy(p + 4, &addr, 4);
        uint64_t* info_p = reinterpret_cast<uint64_t*>(info);
        std::memset(info_p, 0, 64);
        // Layout guess matching common Switch SDK: flags, family, socktype, protocol, addrlen, addr, canon, next
        int32_t* i32 = reinterpret_cast<int32_t*>(info);
        i32[0] = 0;   // flags
        i32[1] = 2;   // AF_INET
        i32[2] = 1;   // STREAM (also works for UDP lookups that ignore)
        i32[3] = 0;
        i32[4] = 16;  // addrlen
        st<uint64_t>(info + 24, sa);
        st<uint64_t>(info + 32, 0);
        st<uint64_t>(info + 40, 0);
        st<uint64_t>(out_ptr, info);
        c.x[0] = 0;
        log("hostnet: GetAddrInfo(%s) -> %s (Nextendo redirect)", host.c_str(), ip.c_str());
        return;
    }
    // Real resolve
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(node, service, &hints, &res);
    if (rc != 0 || !res) {
        c.x[0] = 4;
        return;
    }
    uint64_t out_ptr = c.x[3];
    uint64_t info = kernel_alloc(64, 8);
    uint64_t sa = kernel_alloc(16, 8);
    auto* in = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    write_sockaddr_in(sa, 16, in->sin_addr.s_addr, in->sin_port);
    int32_t* i32 = reinterpret_cast<int32_t*>(info);
    std::memset(i32, 0, 64);
    i32[1] = 2;
    i32[2] = res->ai_socktype == SOCK_DGRAM ? 2 : 1;
    i32[4] = 16;
    st<uint64_t>(info + 24, sa);
    st<uint64_t>(out_ptr, info);
    freeaddrinfo(res);
    c.x[0] = 0;
}

void hostnet_FreeAddrInfo(CPU& c) {
    (void)c;
    c.x[0] = 0;
}

void hostnet_GetLastErrno(CPU& c) { c.x[0] = uint64_t(t_errno); }
void hostnet_SetLastErrno(CPU& c) {
    t_errno = int(c.x[0]);
    c.x[0] = 0;
}

void hostnet_Shutdown(CPU& c) {
    std::lock_guard lk(g_mu);
    HostSock* s = get(int(c.x[0]));
    if (!s) return ret(c, fail(kEBADF));
    shutdown(s->s, int(c.x[1]));
    ret(c, 0);
}

void hostnet_SetSockOpt(CPU& c) {
    (void)c;
    ret(c, 0);
}
void hostnet_GetSockOpt(CPU& c) {
    (void)c;
    ret(c, 0);
}
void hostnet_GetSockName(CPU& c) {
    (void)c;
    ret(c, 0);
}
void hostnet_Poll(CPU& c) {
    // Minimal: report writable.
    if (c.x[0] && c.x[1]) {
        // PollFd array — set revents OUT
    }
    c.x[0] = c.x[1];  // all ready
}

void hostnet_Fcntl(CPU& c) {
    HostSock* s = nullptr;
    {
        std::lock_guard lk(g_mu);
        s = get(int(c.x[0]));
        if (!s) return ret(c, fail(kEBADF));
    }
    const int cmd = int(c.x[1]);
    if (cmd == 3) {  // F_GETFL
        ret(c, s->nonblock ? kNonblock : 0);
        return;
    }
    if (cmd == 4) {  // F_SETFL
        s->nonblock = (int(c.x[2]) & kNonblock) != 0;
#ifdef _WIN32
        u_long nb = s->nonblock ? 1 : 0;
        ioctlsocket(s->s, FIONBIO, &nb);
#else
        int fl = fcntl(s->s, F_GETFL, 0);
        fcntl(s->s, F_SETFL, s->nonblock ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
        ret(c, 0);
        return;
    }
    ret(c, 0);
}

hostnet_sock_t hostnet_native_fd(int guest_fd) {
    std::lock_guard lk(g_mu);
    HostSock* s = get(guest_fd);
    if (!s) return
#ifdef _WIN32
        INVALID_SOCKET;
#else
        -1;
#endif
    return hostnet_sock_t(s->s);
}

}  // namespace nx
