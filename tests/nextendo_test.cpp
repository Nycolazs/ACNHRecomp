// nextendo.cpp's account client against a loopback stand-in for the Nextendo API: nex token and baasUserID fetch,
// friend list parsing (nested presence objects), the endpoint allowlist and the DNS redirect table. Needs curl on
// PATH, like the client itself.
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "ldn_net.h"
#include "nextendo.h"

namespace nx {
void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}
}  // namespace nx

using namespace nx;

static int g_failures = 0;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            g_failures++;                                                \
        }                                                                \
    } while (0)

static const char* kNexToken =
    R"({"nex_token":"nx2.abc.def","pid":1800000042,"friend_code":"SW-1111-2222-3333","username":"Tester","expires_in":86400})";
static const char* kMe = R"({"account":{"pid":1800000042,"username":"Tester"},"nsa":"0123456789abcdef"})";
static const char* kFriends = R"({"friends":[
  {"pid":1800000001,"name":"Alpha","nsa":"aaaaaaaaaaaaaaaa","favorite":true,
   "presence":{"status":2,"app_id":"01006F8002326000","app_field":"","jeu":"Animal Crossing"}},
  {"pid":1800000002,"name":"Be\"ta","presence":{"status":0}},
  {"pid":1800000003,"username":"Gamma","nsa":"bad"}
]})";

static void serve(sock_t listener, std::atomic<bool>* stop) {
    while (!*stop) {
        if (!wait_socket(listener, false, 100)) continue;
        sock_t c = accept(listener, nullptr, nullptr);
        if (c == kBadSock) continue;
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            int n = int(recv(c, buf, sizeof buf, 0));
            if (n <= 0) break;
            req.append(buf, size_t(n));
        }
        const bool authed = req.find("Authorization: Bearer access-1") != std::string::npos;
        std::string body = "{}";
        int status = 404;
        if (!authed) {
            status = 401;
        } else if (req.rfind("GET /api/nex-token ", 0) == 0) {
            status = 200, body = kNexToken;
        } else if (req.rfind("GET /api/me ", 0) == 0) {
            status = 200, body = kMe;
        } else if (req.rfind("GET /api/friends ", 0) == 0) {
            status = 200, body = kFriends;
        }
        std::string resp = "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\n" +
                           "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        send_all(c, resp.data(), resp.size());
        sock_close(c);
    }
}

int main() {
    net_init();
    sock_t l = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t al = sizeof a;
    getsockname(l, reinterpret_cast<sockaddr*>(&a), &al);
    listen(l, 8);
    std::atomic<bool> stop{false};
    std::thread server(serve, l, &stop);

    const std::string base = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port));
#ifdef _WIN32
    _putenv_s("NEXTENDO_API", base.c_str());
#else
    setenv("NEXTENDO_API", base.c_str(), 1);
#endif
    const auto dir = std::filesystem::temp_directory_path() / "nx_nextendo_test";
    std::filesystem::create_directories(dir);
    nextendo::set_data_root(dir.string());
    nextendo::set_mode_override("nextendo");
    nextendo::set_game_version("3.0.3");

    CHECK(nextendo::api_base() == base);  // loopback override accepted

    nextendo::Account acc;
    acc.signed_in = true;
    acc.access_token = "access-1";
    acc.refresh_token = "refresh-1";
    acc.access_expires_unix = 4102444800ull;  // 2100: no refresh needed
    nextendo::set_account(acc);
    CHECK(nextendo::online_services_active());

    CHECK(nextendo::ensure_online_identity());
    const nextendo::Account got = nextendo::account();
    CHECK(got.nex_token == "nx2.abc.def");
    CHECK(got.pid == 1800000042ull);
    CHECK(got.username == "Tester");
    CHECK(got.nsa == 0x0123456789abcdefull);
    CHECK(got.nex_expires_unix > 0);
    CHECK(nextendo::network_service_account_id() == 0x0123456789abcdefull);

    // Persisted and read back.
    nextendo::set_account({});
    nextendo::load_account();
    CHECK(nextendo::account().nsa == 0x0123456789abcdefull);
    CHECK(nextendo::account().nex_token == "nx2.abc.def");

    std::vector<nextendo::Friend> fr;
    CHECK(nextendo::fetch_friends(fr));
    CHECK(fr.size() == 3);
    if (fr.size() == 3) {
        CHECK(fr[0].nsa == 0xaaaaaaaaaaaaaaaaull && fr[0].status == 2 && fr[0].favorite);
        CHECK(fr[0].app_id == "01006F8002326000");
        CHECK(fr[0].name == "Alpha");
        CHECK(fr[1].name == "Be\"ta" && fr[1].status == 0 && fr[1].nsa == 0 && !fr[1].favorite);
        CHECK(fr[2].name == "Gamma" && fr[2].nsa == 0);  // malformed nsa ignored
    }

    using RT = nextendo::RedirectTarget;
    CHECK(nextendo::classify_host("nncs2-lp1.n.n.srv.nintendo.net") == RT::Nat);
    CHECK(nextendo::classify_host("g2df33d01-lp1.p.srv.nintendo.net") == RT::Nat);
    CHECK(nextendo::classify_host("api.hac.lp1.acbaa.srv.nintendo.net.") == RT::Server);
    CHECK(nextendo::classify_host("nintendo.net.evil.com") == RT::None);
    CHECK(nextendo::classify_host("example.com") == RT::None);

    stop = true;
    server.join();
    sock_close(l);
    std::filesystem::remove_all(dir);

    // An override to a foreign host is refused (it would receive the bearer token).
#ifdef _WIN32
    _putenv_s("NEXTENDO_API", "http://evil.example");
#else
    setenv("NEXTENDO_API", "http://evil.example", 1);
#endif
    CHECK(nextendo::api_base() == "https://nextendo.network");

    std::printf(g_failures ? "nextendo_test: %d failure(s)\n" : "nextendo_test: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
