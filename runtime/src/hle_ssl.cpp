// nn::ssl for Nextendo Online: TLS client connections run here with Mbed TLS over the host sockets of
// hle_hostnet.cpp, instead of the console's ssl service. The game reaches it through the C API (libcurl:
// nnsslConnection*) and a few C++ calls (NEX: nn::ssl::Context). Server certificates are not verified: the
// Nextendo servers present certificates for Nintendo's hostnames that no host trust store accepts, and the DNS
// redirect (nextendo.cpp) already decides who the game talks to.
//
// Object layouts, read from the SDK: nn::ssl::Context holds its context id at +0; nn::ssl::Connection holds its
// connection id at +0 and its context id at +8. The SDK's own getters read those fields, so Create writes them.
// The C wrappers pass their arguments through unchanged to the C++ methods, so one handler serves both.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <psa/crypto.h>

#include <nx/runtime.h>

#include "hle_hostnet.h"
#include "ldn_net.h"
#include "nextendo.h"

namespace nx {

// One RNG for every connection and the id_token signer (nextendo_token.cpp); Mbed TLS is built without its
// threading layer, so calls are serialized here.
namespace {

std::mutex g_rng_mu;
mbedtls_entropy_context g_entropy;
mbedtls_ctr_drbg_context g_drbg;

}  // namespace

int tls_random(void*, unsigned char* out, size_t n) {
    std::lock_guard lk(g_rng_mu);
    return mbedtls_ctr_drbg_random(&g_drbg, out, n);
}

bool tls_ready() {
    static const bool ok = [] {
        if (psa_crypto_init() != PSA_SUCCESS) return false;  // TLS 1.3 needs PSA
        mbedtls_entropy_init(&g_entropy);
        mbedtls_ctr_drbg_init(&g_drbg);
        static const char pers[] = "nx-ssl";
        return mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                     reinterpret_cast<const unsigned char*>(pers), sizeof pers - 1) == 0;
    }();
    return ok;
}

namespace {

constexpr uint32_t kModule = 123;
constexpr uint32_t res(uint32_t desc) { return (desc << 9) | kModule; }
constexpr uint32_t kOk = 0;
constexpr uint32_t kNoSocket = res(103);
constexpr uint32_t kInvalidSocket = res(106);
constexpr uint32_t kBadObject = 0xe27b;  // what the SDK's C wrappers return for a null object
constexpr uint32_t kWouldBlock = res(204);
constexpr uint32_t kTimeout = res(205);
constexpr uint32_t kConnectionReset = res(209);
constexpr uint32_t kConnectionAbort = res(210);

enum IoMode : uint32_t { kBlocking = 1, kNonBlocking = 2 };
enum OptionType : uint32_t { kDoNotCloseSocket = 0, kGetServerCertChain = 1, kSkipDefaultVerify = 2, kEnableAlpn = 3 };
enum AlpnState : uint32_t { kAlpnNoSupport = 0, kAlpnNegotiated = 1, kAlpnNoOverlap = 2 };
enum PollEvent : uint32_t { kPollRead = 1, kPollWrite = 2, kPollExcept = 4 };

struct Conn {
    int fd = -1;
    std::string host;
    uint32_t io_mode = kBlocking;
    bool alpn_requested = false;
    uint64_t cert_buf = 0;
    uint32_t cert_buf_size = 0;
    bool configured = false, handshaken = false;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    std::vector<uint8_t> peeked;  // plaintext Peek took off the TLS layer, handed out first by Read
    const char* alpn_list[2] = {"http/1.1", nullptr};

    Conn() {
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_init(&ssl);
    }
    ~Conn() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    }
};

std::mutex g_mu;
std::map<uint64_t, std::shared_ptr<Conn>> g_conns;
std::atomic<uint64_t> g_next_id{1};

std::shared_ptr<Conn> conn_of(uint64_t obj) {
    if (!obj) return nullptr;
    std::lock_guard lk(g_mu);
    auto it = g_conns.find(ld<uint64_t>(obj));
    return it == g_conns.end() ? nullptr : it->second;
}

bool is_ip_literal(const std::string& h) {
    return !h.empty() && h.find_first_not_of("0123456789.") == std::string::npos;
}

// ---- transport: the guest fd's host socket --------------------------------------------------------------------

int bio_send(void* p, const unsigned char* buf, size_t len) {
    Conn* c = static_cast<Conn*>(p);
    const sock_t s = sock_t(hostnet_native_fd(c->fd));
    if (s == kBadSock) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    for (;;) {
        const int r = int(send(s, reinterpret_cast<const char*>(buf), int(len), MSG_NOSIGNAL));
        if (r >= 0) return r;
#ifdef _WIN32
        const bool again = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool again = errno == EAGAIN || errno == EWOULDBLOCK;
#endif
        if (!again) return MBEDTLS_ERR_NET_SEND_FAILED;
        if (c->io_mode == kNonBlocking) return MBEDTLS_ERR_SSL_WANT_WRITE;
        wait_socket(s, true, 1000);  // a game-side O_NONBLOCK socket under a blocking TLS connection
    }
}

int bio_recv(void* p, unsigned char* buf, size_t len) {
    Conn* c = static_cast<Conn*>(p);
    const sock_t s = sock_t(hostnet_native_fd(c->fd));
    if (s == kBadSock) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    for (;;) {
        if (c->io_mode == kNonBlocking && !wait_socket(s, false, 0)) return MBEDTLS_ERR_SSL_WANT_READ;
        const int r = int(recv(s, reinterpret_cast<char*>(buf), int(len), 0));
        if (r > 0) return r;
        if (r == 0) return MBEDTLS_ERR_NET_CONN_RESET;
#ifdef _WIN32
        const int e = WSAGetLastError();
        const bool again = e == WSAEWOULDBLOCK, timeout = e == WSAETIMEDOUT;
#else
        const bool again = errno == EAGAIN || errno == EWOULDBLOCK, timeout = false;
#endif
        if (timeout) return MBEDTLS_ERR_SSL_TIMEOUT;
        if (!again) return MBEDTLS_ERR_NET_RECV_FAILED;
        if (c->io_mode == kNonBlocking) return MBEDTLS_ERR_SSL_WANT_READ;
        wait_socket(s, false, 1000);
    }
}

uint32_t map_error(int e) {
    switch (e) {
    case MBEDTLS_ERR_SSL_WANT_READ:
    case MBEDTLS_ERR_SSL_WANT_WRITE: return kWouldBlock;
    case MBEDTLS_ERR_SSL_TIMEOUT: return kTimeout;
    case MBEDTLS_ERR_NET_CONN_RESET: return kConnectionReset;
    case MBEDTLS_ERR_NET_INVALID_CONTEXT: return kInvalidSocket;
    default: return kConnectionAbort;
    }
}

void log_tls_error(const char* what, const Conn& c, int e) {
    char msg[128];
    mbedtls_strerror(e, msg, sizeof msg);
    log("ssl: %s %s failed: %s (-0x%04x)", what, c.host.c_str(), msg, unsigned(-e));
}

// First handshake call: the TLS configuration, built once the host name and socket are known.
bool configure(Conn& c) {
    if (c.configured) return true;
    if (!tls_ready()) return false;
    if (mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return false;
    mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&c.conf, tls_random, nullptr);
    // ALPN: http/1.1 only, whatever the game offered. The island services answer HTTP/1.1, and NEX's WebSocket is
    // an HTTP/1.1 upgrade; offering h2 lets a server pick a protocol the game's client then reads as garbage.
    if (c.alpn_requested) mbedtls_ssl_conf_alpn_protocols(&c.conf, c.alpn_list);
    if (mbedtls_ssl_setup(&c.ssl, &c.conf) != 0) return false;
    // SNI routes the connection at Nextendo's proxy: the name the game set, else the one its address was resolved
    // from (a connection opened by IP).
    std::string sni = c.host;
    if (sni.empty() || is_ip_literal(sni)) {
        const sock_t s = sock_t(hostnet_native_fd(c.fd));
        sockaddr_in peer{};
        socklen_t l = sizeof peer;
        if (s != kBadSock && getpeername(s, reinterpret_cast<sockaddr*>(&peer), &l) == 0)
            sni = nextendo::host_for_ip(peer.sin_addr.s_addr);
    }
    if (!sni.empty() && !is_ip_literal(sni)) mbedtls_ssl_set_hostname(&c.ssl, sni.c_str());
    mbedtls_ssl_set_bio(&c.ssl, &c, bio_send, bio_recv, nullptr);
    if (c.host.empty()) c.host = sni;
    c.configured = true;
    return true;
}

uint32_t handshake(Conn& c) {
    if (c.handshaken) return kOk;
    if (c.fd < 0 || hostnet_native_fd(c.fd) == hostnet_sock_t(kBadSock)) return kNoSocket;
    if (!configure(c)) return kConnectionAbort;
    const int r = mbedtls_ssl_handshake(&c.ssl);
    if (r != 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) log_tls_error("handshake", c, r);
        return map_error(r);
    }
    c.handshaken = true;
    log("ssl: connected to %s (%s)", c.host.c_str(), mbedtls_ssl_get_version(&c.ssl));
    return kOk;
}

// Plaintext from the TLS layer; 0 at the peer's close_notify.
int tls_read(Conn& c, uint8_t* buf, size_t n) {
    for (;;) {
        const int r = mbedtls_ssl_read(&c.ssl, buf, n);
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;  // TLS 1.3 housekeeping, not data
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return r;
    }
}

// DIAG: the HTTP request line and response status line crossing a connection (never headers: they carry tokens).
void http_trace(const Conn& k, const char* dir, const uint8_t* p, size_t n) {
    static std::atomic<int> count{0};
    if (count >= 2000 || n < 8) return;
    static const char* const kStarts[] = {"GET ", "POST ", "PUT ", "DELETE ", "PATCH ", "HTTP/1."};
    for (const char* st : kStarts)
        if (std::strncmp(reinterpret_cast<const char*>(p), st, std::strlen(st)) == 0) {
            size_t e = 0;
            while (e < n && e < 200 && p[e] != 0x0d && p[e] != 0x0a) e++;
            count++;
            log("ssl: %s %s %.*s", k.host.c_str(), dir, int(e), reinterpret_cast<const char*>(p));
            return;
        }
}

// ---- nn::ssl::Context -----------------------------------------------------------------------------------------

void Initialize(CPU& c) { c.x[0] = tls_ready() ? kOk : kConnectionAbort; }
void Ok(CPU& c) { c.x[0] = kOk; }

// Context::Create(this, SslVersion)
void ContextCreate(CPU& c) {
    if (!c.x[0]) return void(c.x[0] = kBadObject);
    if (!nextendo::online_services_active()) return void(c.x[0] = kNoSocket);  // offline: as before, no TLS at all
    st<uint64_t>(c.x[0], g_next_id++);
    c.x[0] = kOk;
}

void ContextDestroy(CPU& c) {
    if (c.x[0]) st<uint64_t>(c.x[0], 0);
    c.x[0] = kOk;
}

// Context::GetContextId(this, u64*)
void ContextGetId(CPU& c) {
    if (!c.x[0] || !c.x[1]) return void(c.x[0] = kBadObject);
    st<uint64_t>(c.x[1], ld<uint64_t>(c.x[0]));
    c.x[0] = kOk;
}

// ImportServerPki(this, u64* id, ...), ImportClientPki(this, u64* id, ...), ImportCrl(this, u64* id, ...): kept by
// id only; nothing is verified and no server asks for a client certificate.
void ContextImport(CPU& c) {
    if (c.x[1]) st<uint64_t>(c.x[1], g_next_id++);
    c.x[0] = kOk;
}

// ---- nn::ssl::Connection --------------------------------------------------------------------------------------

// Connection::Create(this, Context*)
void ConnectionCreate(CPU& c) {
    const uint64_t obj = c.x[0], ctx = c.x[1];
    if (!obj || !ctx) return void(c.x[0] = kBadObject);
    const uint64_t id = g_next_id++;
    {
        std::lock_guard lk(g_mu);
        g_conns[id] = std::make_shared<Conn>();
    }
    st<uint64_t>(obj, id);
    st<uint64_t>(obj + 8, ld<uint64_t>(ctx));
    c.x[0] = kOk;
}

// shortcut: the socket is not closed here even without DoNotCloseSocket; the game closes its own descriptor
// (libcurl and NEX both do). Close it here if a title is seen leaking sockets.
void ConnectionDestroy(CPU& c) {
    const uint64_t obj = c.x[0];
    if (!obj) return void(c.x[0] = kBadObject);
    std::shared_ptr<Conn> conn;
    {
        std::lock_guard lk(g_mu);
        auto it = g_conns.find(ld<uint64_t>(obj));
        if (it != g_conns.end()) {
            conn = it->second;
            g_conns.erase(it);
        }
    }
    if (conn && conn->handshaken) mbedtls_ssl_close_notify(&conn->ssl);
    st<uint64_t>(obj, 0);
    c.x[0] = kOk;
}

#define WITH_CONN                                    \
    std::shared_ptr<Conn> conn = conn_of(c.x[0]);    \
    if (!conn) return void(c.x[0] = kBadObject);     \
    Conn& k = *conn

void SetSocketDescriptor(CPU& c) {
    WITH_CONN;
    if (!hostnet_owns_fd(int(c.x[1]))) return void(c.x[0] = kInvalidSocket);
    k.fd = int(c.x[1]);
    c.x[0] = kOk;
}

// SetHostName(this, const char*, u32 length)
void SetHostName(CPU& c) {
    WITH_CONN;
    const char* p = reinterpret_cast<const char*>(c.x[1]);
    k.host = p ? std::string(p, strnlen(p, size_t(c.x[2]))) : std::string();
    c.x[0] = kOk;
}

void SetIoMode(CPU& c) {
    WITH_CONN;
    k.io_mode = uint32_t(c.x[1]) == kNonBlocking ? kNonBlocking : kBlocking;
    c.x[0] = kOk;
}

// SetServerCertBuffer(this, char*, u32)
void SetServerCertBuffer(CPU& c) {
    WITH_CONN;
    k.cert_buf = c.x[1];
    k.cert_buf_size = uint32_t(c.x[2]);
    c.x[0] = kOk;
}

// SetNextAlpnProto(this, const u8* wire, u32): remembered as "the game wants ALPN"; see configure().
void SetNextAlpnProto(CPU& c) {
    WITH_CONN;
    k.alpn_requested = c.x[1] && c.x[2];
    c.x[0] = kOk;
}

// SetOption(this, OptionType, bool)
void SetOption(CPU& c) {
    WITH_CONN;
    if (uint32_t(c.x[1]) == kEnableAlpn && !(c.x[2] & 1)) k.alpn_requested = false;
    c.x[0] = kOk;
}

// Verify option, session cache mode: accepted (nothing is verified, every session is new).
void ConnectionAccept(CPU& c) {
    WITH_CONN;
    (void)k;
    c.x[0] = kOk;
}

void DoHandshake(CPU& c) {
    WITH_CONN;
    c.x[0] = handshake(k);
}

// DoHandshake(this, u32* cert_size, u32* cert_count): the server's leaf certificate into the buffer the game gave.
void DoHandshakeWithCertBuffer(CPU& c) {
    WITH_CONN;
    const uint64_t out_size = c.x[1], out_count = c.x[2];
    const uint32_t r = handshake(k);
    uint32_t size = 0, count = 0;
    if (r == kOk && k.cert_buf) {
        const mbedtls_x509_crt* crt = mbedtls_ssl_get_peer_cert(&k.ssl);
        if (crt && crt->raw.len <= k.cert_buf_size) {
            std::memcpy(reinterpret_cast<void*>(k.cert_buf), crt->raw.p, crt->raw.len);
            size = uint32_t(crt->raw.len);
            count = 1;
        }
    }
    if (out_size) st<uint32_t>(out_size, size);
    if (out_count) st<uint32_t>(out_count, count);
    c.x[0] = r;
}

// Read(this, char*, int* read, u32 size)
void Read(CPU& c) {
    WITH_CONN;
    auto* buf = reinterpret_cast<uint8_t*>(c.x[1]);
    const uint64_t out = c.x[2];
    const size_t n = size_t(uint32_t(c.x[3]));
    if (const uint32_t h = handshake(k); h != kOk) return void(c.x[0] = h);
    int got;
    if (!k.peeked.empty()) {
        got = int(std::min(n, k.peeked.size()));
        std::memcpy(buf, k.peeked.data(), size_t(got));
        k.peeked.erase(k.peeked.begin(), k.peeked.begin() + got);
    } else {
        got = tls_read(k, buf, n);
        if (got < 0) {
            if (got != MBEDTLS_ERR_SSL_WANT_READ && got != MBEDTLS_ERR_SSL_WANT_WRITE) log_tls_error("read", k, got);
            return void(c.x[0] = map_error(got));
        }
    }
    if (got > 0) http_trace(k, "<-", buf, size_t(got));
    if (out) st<int32_t>(out, got);
    c.x[0] = kOk;
}

// Peek(this, char*, int* read, u32 size): read, and keep the bytes for the next Read.
void Peek(CPU& c) {
    WITH_CONN;
    auto* buf = reinterpret_cast<uint8_t*>(c.x[1]);
    const uint64_t out = c.x[2];
    const size_t n = size_t(uint32_t(c.x[3]));
    if (const uint32_t h = handshake(k); h != kOk) return void(c.x[0] = h);
    if (k.peeked.size() < n) {
        std::vector<uint8_t> more(n - k.peeked.size());
        const int got = tls_read(k, more.data(), more.size());
        if (got < 0 && k.peeked.empty()) return void(c.x[0] = map_error(got));
        if (got > 0) k.peeked.insert(k.peeked.end(), more.begin(), more.begin() + got);
    }
    const size_t m = std::min(n, k.peeked.size());
    std::memcpy(buf, k.peeked.data(), m);
    if (out) st<int32_t>(out, int32_t(m));
    c.x[0] = kOk;
}

// Write(this, const char*, int* written, u32 size)
void Write(CPU& c) {
    WITH_CONN;
    const auto* buf = reinterpret_cast<const uint8_t*>(c.x[1]);
    const uint64_t out = c.x[2];
    const size_t n = size_t(uint32_t(c.x[3]));
    if (const uint32_t h = handshake(k); h != kOk) return void(c.x[0] = h);
    http_trace(k, "->", buf, n);
    const int r = mbedtls_ssl_write(&k.ssl, buf, n);
    if (r < 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) log_tls_error("write", k, r);
        return void(c.x[0] = map_error(r));
    }
    if (out) st<int32_t>(out, r);
    c.x[0] = kOk;
}

// Pending(this, int*): decrypted bytes ready without touching the socket.
void Pending(CPU& c) {
    WITH_CONN;
    const size_t n = k.peeked.size() + (k.handshaken ? mbedtls_ssl_get_bytes_avail(&k.ssl) : 0);
    if (c.x[1]) st<int32_t>(c.x[1], int32_t(n));
    c.x[0] = kOk;
}

// Poll(this, PollEvent* out, PollEvent* in, u32 timeout ms)
void Poll(CPU& c) {
    WITH_CONN;
    const uint64_t out = c.x[1];
    const uint32_t want = c.x[2] ? ld<uint32_t>(c.x[2]) : 0;
    const int timeout = int(uint32_t(c.x[3]));
    uint32_t got = 0;
    const sock_t s = sock_t(hostnet_native_fd(k.fd));
    if (s == kBadSock) return void(c.x[0] = kNoSocket);
    if (want & kPollRead) {
        const bool buffered = !k.peeked.empty() || (k.handshaken && mbedtls_ssl_get_bytes_avail(&k.ssl));
        if (buffered || wait_socket(s, false, timeout)) got |= kPollRead;
    }
    if (want & kPollWrite) got |= kPollWrite;
    if (out) st<uint32_t>(out, got);
    c.x[0] = got ? kOk : kTimeout;
}

// GetVerifyCertError(this, Result*): nothing was verified, nothing failed.
void GetVerifyCertError(CPU& c) {
    WITH_CONN;
    (void)k;
    if (c.x[1]) st<uint32_t>(c.x[1], 0);
    c.x[0] = kOk;
}

// GetConnectionId(this, u64*)
void GetConnectionId(CPU& c) {
    if (!c.x[0] || !c.x[1]) return void(c.x[0] = kBadObject);
    st<uint64_t>(c.x[1], ld<uint64_t>(c.x[0]));
    c.x[0] = kOk;
}

// GetNextAlpnProto(this, AlpnProtoState*, u8* out, u32* out_len, u32 max)
void GetNextAlpnProto(CPU& c) {
    WITH_CONN;
    const char* p = k.handshaken ? mbedtls_ssl_get_alpn_protocol(&k.ssl) : nullptr;
    const uint32_t max = uint32_t(c.x[4]);
    uint32_t state = p ? kAlpnNegotiated : (k.alpn_requested ? kAlpnNoOverlap : kAlpnNoSupport), len = 0;
    if (p && c.x[2]) {
        len = std::min<uint32_t>(uint32_t(std::strlen(p)), max);
        std::memcpy(reinterpret_cast<void*>(c.x[2]), p, len);
    }
    if (c.x[1]) st<uint32_t>(c.x[1], state);
    if (c.x[3]) st<uint32_t>(c.x[3], len);
    c.x[0] = kOk;
}

#undef WITH_CONN

}  // namespace

// Library
NX_HLE("nnsslInitialize", Initialize);
NX_HLE("nnsslFinalize", Ok);
NX_HLE("_ZN2nn3ssl10InitializeEv", Initialize);
NX_HLE("_ZN2nn3ssl10InitializeEj", Initialize);
NX_HLE("_ZN2nn3ssl8FinalizeEv", Ok);
// Context
NX_HLE("nnsslContextCreate", ContextCreate);
NX_HLE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ContextCreate);
NX_HLE("nnsslContextDestroy", ContextDestroy);
NX_HLE("_ZN2nn3ssl7Context7DestroyEv", ContextDestroy);
NX_HLE("nnsslContextGetContextId", ContextGetId);
NX_HLE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ContextImport);
NX_HLE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ContextImport);
NX_HLE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ContextImport);
// Connection
NX_HLE("nnsslConnectionCreate", ConnectionCreate);
NX_HLE("_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", ConnectionCreate);
NX_HLE("nnsslConnectionDestroy", ConnectionDestroy);
NX_HLE("_ZN2nn3ssl10Connection7DestroyEv", ConnectionDestroy);
NX_HLE("nnsslConnectionSetSocketDescriptor", SetSocketDescriptor);
NX_HLE("_ZN2nn3ssl10Connection19SetSocketDescriptorEi", SetSocketDescriptor);
NX_HLE("nnsslConnectionSetHostName", SetHostName);
NX_HLE("_ZN2nn3ssl10Connection11SetHostNameEPKcj", SetHostName);
NX_HLE("nnsslConnectionSetVerifyOption", ConnectionAccept);
NX_HLE("nnsslConnectionSetSessionCacheMode", ConnectionAccept);
NX_HLE("_ZN2nn3ssl10Connection19SetSessionCacheModeENS1_16SessionCacheModeE", ConnectionAccept);
NX_HLE("nnsslConnectionSetIoMode", SetIoMode);
NX_HLE("nnsslConnectionSetServerCertBuffer", SetServerCertBuffer);
NX_HLE("nnsslConnectionSetNextAlpnProto", SetNextAlpnProto);
NX_HLE("nnsslConnectionSetOption", SetOption);
NX_HLE("nnsslConnectionDoHandshake", DoHandshake);
NX_HLE("nnsslConnectionDoHandshakeWithCertBuffer", DoHandshakeWithCertBuffer);
NX_HLE("nnsslConnectionRead", Read);
NX_HLE("nnsslConnectionWrite", Write);
NX_HLE("nnsslConnectionPeek", Peek);
NX_HLE("nnsslConnectionPending", Pending);
NX_HLE("nnsslConnectionPoll", Poll);
NX_HLE("nnsslConnectionGetVerifyCertError", GetVerifyCertError);
NX_HLE("nnsslConnectionGetConnectionId", GetConnectionId);
NX_HLE("nnsslConnectionGetNextAlpnProto", GetNextAlpnProto);

}  // namespace nx
