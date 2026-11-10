// npns:u, the console's push notifications, for ACNH's best friends on Nextendo.
//
// On a console the island server pushes "a request arrived / was accepted / a best friend was added" through NPNS, and
// the game re-reads its lists when the push arrives. Without NPNS the game only re-reads them when it signs in. This
// service stands in for the push channel: it watches the island server itself and, when the best friends or the
// requests change, delivers the push the server would have sent, so the game updates through its own code path.
//
// How the game reads a push (read from the 3.0.3 code): Receive fills a 0x10a0-byte record whose payload, at +0xa0
// with its length (u16) at +0x6, is base64 text; decoded, it is a MessagePack map {"type", "action", "receiver_user_id",
// "target_user_id"}. Type "friend" with action "add"/"delete" changes a best friend, "sent_friend_request" /
// "received_friend_request" refresh the request lists. The game reads until Receive answers 0x69a (nothing left);
// any other failure aborts it.
//
// Watching the server: the game's own GET requests for its best friends and requests (hle_ssl.cpp hands every request
// line here) are kept and replayed on a separate TLS connection to the same server every 30 s.
// They carry the game's island-server session, which therefore never leaves this process and is never logged.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <mbedtls/base64.h>
#include <mbedtls/ssl.h>

#include <nx/runtime.h>

#include "ipc.h"
#include "ldn_net.h"
#include "nextendo.h"

namespace nx {

bool tls_ready();                                     // hle_ssl.cpp
int tls_random(void*, unsigned char* out, size_t n);  // hle_ssl.cpp
bool nextendo_peer_pinned(const mbedtls_ssl_context* ssl, const std::string& host);  // hle_ssl.cpp

namespace {

constexpr size_t kRecordSize = 0x10a0;
constexpr size_t kPayloadAt = 0xa0, kPayloadMax = kRecordSize - kPayloadAt;
constexpr uint32_t kNothingToReceive = (3u << 9) | 154u;  // 0x69a: what the game's receive loop stops on

struct Watched {
    std::string request;  // the game's GET, as it sent it
    std::set<uint64_t> ids;
    std::string body;
    bool known = false;  // ids and body reflect what the game last read
};

std::mutex g_mu;
std::string g_host;
uint64_t g_me = 0;                       // the player's island user id
std::map<std::string, Watched> g_watch;  // "friends", "send", "receive"
std::deque<std::string> g_queue;         // push payloads not yet received
std::shared_ptr<EventState> g_event = std::make_shared<EventState>();
std::atomic<bool> g_polling{false};

// ---- replaying a request ----------------------------------------------------------------------------------------

int bio_send(void* p, const unsigned char* b, size_t n) {
    const int r = int(send(*static_cast<sock_t*>(p), reinterpret_cast<const char*>(b), int(n), MSG_NOSIGNAL));
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}

int bio_recv(void* p, unsigned char* b, size_t n) {
    const int r = int(recv(*static_cast<sock_t*>(p), reinterpret_cast<char*>(b), int(n), 0));
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;  // 0: closed
}

// The request with "Connection: close" (read to the end) and no Accept-Encoding (a plain body to read).
std::string for_replay(const std::string& req) {
    std::string out;
    size_t i = 0;
    while (i < req.size()) {
        size_t e = req.find("\r\n", i);
        if (e == std::string::npos) break;
        std::string line = req.substr(i, e - i);
        i = e + 2;
        if (line.empty()) break;
        std::string lower = line;
        for (char& ch : lower) ch = char(std::tolower(unsigned(ch)));
        if (lower.rfind("connection:", 0) == 0 || lower.rfind("accept-encoding:", 0) == 0) continue;
        out += line + "\r\n";
    }
    return out + "Connection: close\r\n\r\n";
}

std::string dechunk(const std::string& body) {
    std::string out;
    size_t i = 0;
    while (i < body.size()) {
        const size_t e = body.find("\r\n", i);
        if (e == std::string::npos) break;
        const size_t n = std::strtoul(body.c_str() + i, nullptr, 16);
        if (n == 0) break;
        out += body.substr(e + 2, n);
        i = e + 2 + n + 2;
    }
    return out;
}

// GET on the island server, through Nextendo's address; the body of a 200, or "" on any failure. `status` (may be
// null) gets the HTTP status, 0 when no answer came back.
std::string replay(const std::string& host, const std::string& request, int* status = nullptr) {
    if (status) *status = 0;
    if (!tls_ready()) return {};
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(443);
    a.sin_addr.s_addr = inet_addr(nextendo::server_ip().c_str());
    net_init();
    sock_t s = connect_within(a, 5000);
    if (s == kBadSock) return {};
    set_timeout(s, 10000);
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
    std::string raw;
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) == 0) {
        // Trust as for the game's own connections: the server's key must be one of Nextendo's (hle_ssl.cpp), checked
        // before the request, which carries the game's island-server session, is sent.
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&conf, tls_random, nullptr);
        if (mbedtls_ssl_setup(&ssl, &conf) == 0 && mbedtls_ssl_set_hostname(&ssl, host.c_str()) == 0) {
            mbedtls_ssl_set_bio(&ssl, &s, bio_send, bio_recv, nullptr);
            const std::string req = for_replay(request);
            if (mbedtls_ssl_handshake(&ssl) == 0 && nextendo_peer_pinned(&ssl, host) &&
                mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(req.data()), req.size()) ==
                    int(req.size())) {
                constexpr size_t kMaxResponse = 1 << 20;  // the lists are a few KiB; a server sending more is cut off
                unsigned char buf[4096];
                while (raw.size() < kMaxResponse) {
                    const int r = mbedtls_ssl_read(&ssl, buf, sizeof buf);
                    if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
                    if (r <= 0) break;
                    raw.append(reinterpret_cast<char*>(buf), size_t(r));
                }
            }
        }
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    stop_socket(s);
    if (status && raw.rfind("HTTP/1.", 0) == 0 && raw.size() > 12) *status = std::atoi(raw.c_str() + 9);
    if (raw.rfind("HTTP/1.1 200", 0) != 0) return {};
    const size_t h = raw.find("\r\n\r\n");
    if (h == std::string::npos) return {};
    std::string head = raw.substr(0, h);
    for (char& ch : head) ch = char(std::tolower(unsigned(ch)));
    std::string body = raw.substr(h + 4);
    return head.find("transfer-encoding: chunked") != std::string::npos ? dechunk(body) : body;
}

// The ids in a JSON list response: every number under a key ending in "id" ("id", "user_id", "sender_user_id"...), other
// than the player's own. Whichever names the server uses, a request or a friend appearing or going changes this set.
// A MessagePack body (the island server answers like Nintendo's): every string key ending in "id" followed by an
// unsigned integer.
std::set<uint64_t> msgpack_user_ids(const std::string& body, uint64_t me) {
    std::set<uint64_t> out;
    const auto* b = reinterpret_cast<const uint8_t*>(body.data());
    const size_t n = body.size();
    for (size_t i = 0; i < n; i++) {
        if ((b[i] & 0xe0) != 0xa0) continue;  // fixstr
        const size_t len = b[i] & 0x1f, v = i + 1 + len;
        if (len < 2 || v >= n || b[v - 2] != 'i' || b[v - 1] != 'd') continue;
        const size_t w = b[v] == 0xcf ? 8 : b[v] == 0xce ? 4 : b[v] == 0xcd ? 2 : b[v] == 0xcc ? 1 : 0;
        uint64_t id = b[v] < 0x80 ? b[v] : 0;
        if (w && v + w < n)
            for (size_t k = 1; k <= w; k++) id = (id << 8) | b[v + k];
        if (id && id != me) out.insert(id);
    }
    return out;
}

std::set<uint64_t> user_ids(const std::string& body, uint64_t me) {
    if (!body.empty() && body[0] != '{') return msgpack_user_ids(body, me);
    std::set<uint64_t> out;
    for (size_t q = body.find('"'); q != std::string::npos; q = body.find('"', q + 1)) {
        const size_t e = body.find('"', q + 1);
        if (e == std::string::npos) break;
        const std::string key = body.substr(q + 1, e - q - 1);
        size_t v = e + 1;
        while (v < body.size() && body[v] == ' ') v++;
        if (v >= body.size() || body[v] != ':') {
            q = e;
            continue;
        }
        v++;
        while (v < body.size() && (body[v] == ' ' || body[v] == '"')) v++;
        if (key.size() >= 2 && key.compare(key.size() - 2, 2, "id") == 0)
            if (const uint64_t id = std::strtoull(body.c_str() + v, nullptr, 10); id && id != me) out.insert(id);
        q = e;
    }
    return out;
}

// ---- pushes -----------------------------------------------------------------------------------------------------

// The payload is MessagePack (the game reads it with the reader it uses for island-server replies):
// {type, action, receiver_user_id, target_user_id}.
void push(const char* type, const char* action, uint64_t target) {
    std::string m(1, char(0x84));  // map of 4
    auto str = [&m](const char* s) {
        const size_t n = std::strlen(s);  // all keys and values here are under 32 bytes: fixstr
        m += char(0xa0 | n);
        m.append(s, n);
    };
    auto u64 = [&m](uint64_t v) {
        m += char(0xcf);
        for (int i = 7; i >= 0; i--) m += char(uint8_t(v >> (i * 8)));
    };
    str("type"), str(type);
    str("action"), str(action);
    str("receiver_user_id"), u64(g_me);
    str("target_user_id"), u64(target);
    g_queue.emplace_back(std::move(m));
    g_event->signaled = true;
    log("npns: %s %s", type, action);
}

void poll_once() {
    std::string host;
    std::map<std::string, std::string> requests;
    {
        std::lock_guard lk(g_mu);
        host = g_host;
        for (const auto& [k, w] : g_watch) requests[k] = w.request;
    }
    bool pushed = false;
    for (const auto& [key, req] : requests) {
        const std::string body = replay(host, req);
        if (body.empty()) continue;  // offline, or the game's session expired: it brings a new one when it signs in
        std::lock_guard lk(g_mu);
        const std::set<uint64_t> now = user_ids(body, g_me);
        Watched& w = g_watch[key];
        if (w.request != req) continue;  // the game read this list again meanwhile: its copy is newer
        if (w.known && g_me) {  // a push names its receiver: wait until the game has shown who the player is
            const char* type = key == "friends" ? "friend" : key == "send" ? "sent_friend_request" : "received_friend_request";
            bool changed = false;
            for (uint64_t id : now)
                if (!w.ids.count(id)) push(type, "add", id), changed = true;
            for (uint64_t id : w.ids)
                if (!now.count(id)) push(type, "delete", id), changed = true;
            if (!changed && body != w.body) push(type, body.size() >= w.body.size() ? "add" : "delete", 0), changed = true;
            pushed |= changed;
        }
        w.ids = now;
        w.body = body;
        w.known = true;
    }
    if (pushed) notify_all();
}

void poller() {
    while (g_polling) {
        if (nextendo::online_services_active()) poll_once();
        for (int i = 0; i < 30 && g_polling; i++) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ---- npns:u -----------------------------------------------------------------------------------------------------

uint32_t receive(IpcCtx& c) {
    const Buffer out = c.out_buf(0);
    std::lock_guard lk(g_mu);
    if (g_queue.empty()) {
        g_event->signaled = false;
        return kNothingToReceive;
    }
    const std::string json = std::move(g_queue.front());
    g_queue.pop_front();
    if (g_queue.empty()) g_event->signaled = false;
    if (!out.addr || out.size < kRecordSize) {
        log("npns: receive buffer of %llu bytes, %zu expected", (unsigned long long)out.size, kRecordSize);
        return kNothingToReceive;
    }
    unsigned char b64[kPayloadMax];
    size_t len = 0;
    if (mbedtls_base64_encode(b64, sizeof b64, &len, reinterpret_cast<const unsigned char*>(json.data()),
                              json.size()) != 0)
        return kNothingToReceive;
    auto* rec = reinterpret_cast<uint8_t*>(out.addr);  // the header the game filled (+0x0, +0x4) stays
    std::memset(rec + 0x6, 0, kRecordSize - 0x6);
    const uint16_t n = uint16_t(len);
    std::memcpy(rec + 0x6, &n, 2);
    std::memcpy(rec + kPayloadAt, b64, len);
    return 0;
}

std::shared_ptr<Service> make_npns() {
    auto s = std::make_shared<Service>("npns:u");
    auto ok = [](IpcCtx&) { return 0u; };
    for (uint32_t cmd : {1u, 2u, 8u, 26u}) s->cmds[cmd] = ok;  // ListenAll / ListenTo / ListenToByName / ...MyApplicationId
    s->cmds[3] = receive;  // Receive
    s->cmds[4] = receive;  // ReceiveRaw
    s->cmds[5] = [](IpcCtx& c) {  // GetReceiveEvent
        auto r = std::make_shared<KEvent>();
        r->state = g_event;
        c.out_copy.push_back(handle_add(r));
        return 0u;
    };
    s->cmds[7] = [](IpcCtx& c) {  // GetStateChangeEvent: the state never changes here
        auto r = std::make_shared<KEvent>();
        r->state = std::make_shared<EventState>();
        c.out_copy.push_back(handle_add(r));
        return 0u;
    };
    s->cmds[21] = [](IpcCtx& c) {  // CreateToken(Uid, ApplicationId) -> token: the island server stores it
        const Buffer out = c.out_buf(0);
        if (out.addr && out.size) {
            unsigned char raw[12];
            tls_ready();
            tls_random(nullptr, raw, sizeof raw);
            char tok[40] = "nxrecomp-";
            for (int i = 0; i < 12; i++) std::snprintf(tok + 9 + i * 2, 3, "%02x", raw[i]);
            std::memset(reinterpret_cast<void*>(out.addr), 0, size_t(out.size));
            std::memcpy(reinterpret_cast<void*>(out.addr), tok, std::min<size_t>(std::strlen(tok), size_t(out.size) - 1));
        }
        return 0u;
    };
    s->cmds[25] = [](IpcCtx& c) { c.push<uint8_t>(1); return 0u; };  // QueryIsTokenValid
    if (!g_polling.exchange(true)) std::thread(poller).detach();
    return s;
}

ServiceRegistrar r_npns_u("npns:u", make_npns);

}  // namespace

std::string nextendo::game_server_get(const std::string& host, const std::string& request, int* status) {
    return replay(host, request, status);
}

// hle_ssl.cpp: a request the game sent. The best-friend lists are kept for replay; the profile URL names the player.
void npns_observe_request(const std::string& host, const uint8_t* p, size_t n) {
    if (host.find(".acbaa.") == std::string::npos || n < 16) return;
    const std::string req(reinterpret_cast<const char*>(p), n);
    const std::string line = req.substr(0, req.find("\r\n"));
    const char* key = line.rfind("GET /api/v1/friends ", 0) == 0                          ? "friends"
                      : line.rfind("GET /api/v1/friend_requests?type=send ", 0) == 0    ? "send"
                      : line.rfind("GET /api/v1/friend_requests?type=receive ", 0) == 0 ? "receive"
                                                                                        : nullptr;
    // A request the player sends: the server confirms it with a push to the sender (the game shows "processing"
    // until then). Sent shortly after, once the server has stored it.
    static const char kSend[] = "POST /api/v1/friend_requests?target_user_id=";
    if (line.rfind(kSend, 0) == 0) {
        const uint64_t target = std::strtoull(line.c_str() + sizeof kSend - 1, nullptr, 10);
        std::thread([target] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            {
                std::lock_guard lk(g_mu);
                if (!g_me) return;
                push("sent_friend_request", "add", target);
            }
            notify_all();
        }).detach();
    }
    std::lock_guard lk(g_mu);
    g_host = host;
    if (line.rfind("GET /api/v1/users/", 0) == 0) g_me = std::strtoull(line.c_str() + 18, nullptr, 10);
    if (!key || req.find("\r\n\r\n") == std::string::npos) return;
    Watched& w = g_watch[key];
    w.request = req.substr(0, req.find("\r\n\r\n") + 4);
    w.known = false;  // the game is reading it now: the next replay is the baseline, not a change
}

}  // namespace nx
