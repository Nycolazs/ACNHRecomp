// Nextendo mode, account session, DNS redirect classification, BAAS helpers.
#include "nextendo.h"

#include "nextendo_http.h"
#include "ldn_net.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#if !defined(NX_LAUNCHER_UI)
#include <nx/runtime.h>
#include "config.h"
#include "platform.h"
#else
namespace nx {
void log(const char* fmt, ...);
}
#endif

#if defined(_WIN32)
#include <bcrypt.h>
#include <shellapi.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#endif

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

#if defined(NX_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

namespace nx::nextendo {

// Defined below; used from sign_in_start.
bool fetch_nex_token();

namespace {

std::mutex g_mu;
Account g_account;
std::string g_game_version;
std::string g_last_error;
std::string g_data_root_override;
std::string g_mode_override;
std::string g_client_id_override;
std::atomic<bool> g_sign_in_busy{false};
std::unordered_map<uint32_t, std::string> g_ip_host;

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string json_get_string(const std::string& json, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t i = json.find(pat);
    if (i == std::string::npos) return {};
    i = json.find(':', i + pat.size());
    if (i == std::string::npos) return {};
    i = json.find_first_not_of(" \t\r\n", i + 1);
    if (i == std::string::npos) return {};
    if (json[i] == '"') {
        size_t j = i + 1;
        std::string out;
        while (j < json.size() && json[j] != '"') {
            if (json[j] == '\\' && j + 1 < json.size()) {
                out += json[j + 1];
                j += 2;
                continue;
            }
            out += json[j++];
        }
        return out;
    }
    size_t j = json.find_first_of(",}\r\n ", i);
    return trim(json.substr(i, j == std::string::npos ? std::string::npos : j - i));
}

uint64_t json_get_u64(const std::string& json, const char* key) {
    return std::strtoull(json_get_string(json, key).c_str(), nullptr, 10);
}

std::string data_root() {
    if (!g_data_root_override.empty()) return g_data_root_override;
#if !defined(NX_LAUNCHER_UI)
    return plat::data_root();
#else
    return ".";
#endif
}

std::string account_path() { return data_root() + "/nextendo_account.json"; }

std::string env_or(const char* name, const std::string& fallback) {
    if (const char* e = std::getenv(name); e && e[0]) return e;
    return fallback;
}

std::string b64url(const uint8_t* data, size_t n) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((n * 4 + 2) / 3);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < n) v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < n) v |= uint32_t(data[i + 2]);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        if (i + 1 < n) out += k[(v >> 6) & 63];
        if (i + 2 < n) out += k[v & 63];
    }
    return out;
}

std::string random_token(size_t bytes) {
    std::vector<uint8_t> buf(bytes);
#if defined(_WIN32)
    BCryptGenRandom(nullptr, buf.data(), ULONG(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    std::ifstream ur("/dev/urandom", std::ios::binary);
    if (ur) ur.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
#endif
    return b64url(buf.data(), buf.size());
}

std::string sha256_b64url(const std::string& s) {
    uint8_t dig[32] = {};
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
            BCryptHashData(hash, (PUCHAR)s.data(), ULONG(s.size()), 0);
            BCryptFinishHash(hash, dig, 32, 0);
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
#elif defined(__APPLE__)
    CC_SHA256(s.data(), CC_LONG(s.size()), dig);
#else
    // Linux: openssl dgst when available
    const std::string in_p = data_root() + "/.nx_pkce_in";
    const std::string out_p = data_root() + "/.nx_pkce_out";
    {
        std::ofstream f(in_p, std::ios::binary);
        if (f) f.write(s.data(), std::streamsize(s.size()));
    }
    std::string cmd = "openssl dgst -sha256 -binary -out \"" + out_p + "\" \"" + in_p + "\" 2>/dev/null";
    (void)std::system(cmd.c_str());
    {
        std::ifstream out(out_p, std::ios::binary);
        if (out) out.read(reinterpret_cast<char*>(dig), 32);
    }
#endif
    return b64url(dig, 32);
}

std::string percent_encode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += char(c);
        else {
            char b[8];
            std::snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        }
    }
    return out;
}

bool open_browser(const std::string& url) {
#if defined(NX_HAVE_SDL3)
    return SDL_OpenURL(url.c_str());
#elif defined(_WIN32)
    return (uintptr_t)ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
#else
#if defined(__APPLE__)
    std::string cmd = "open \"" + url + "\"";
#else
    std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
#endif
    return std::system(cmd.c_str()) == 0;
#endif
}

void set_error(const std::string& e) {
    std::lock_guard lk(g_mu);
    g_last_error = e;
}

// Bind loopback, invoke on_ready(port), then accept one OAuth redirect.
bool oauth_listen(const std::string& expect_state, std::string& code_out, int timeout_sec,
                  const std::function<void(uint16_t)>& on_ready) {
    net_init();
    sock_t listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd == kBadSock) return false;
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&one), sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    uint16_t port = 47823;
    bool bound = false;
    for (int t = 0; t < 40; t++) {
        addr.sin_port = htons(uint16_t(port + t));
        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
            port = uint16_t(port + t);
            bound = true;
            break;
        }
    }
    if (!bound || listen(listen_fd, 1) != 0) {
        sock_close(listen_fd);
        return false;
    }
    on_ready(port);
    set_timeout(listen_fd, timeout_sec * 1000);
    sock_t client = accept(listen_fd, nullptr, nullptr);
    sock_close(listen_fd);
    if (client == kBadSock) return false;
    char buf[4096];
    int n = int(recv(client, buf, sizeof buf - 1, 0));
    if (n <= 0) {
        sock_close(client);
        return false;
    }
    buf[n] = 0;
    const char* ok =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
        "<!doctype html><html><body><p>Signed in to Nextendo. You can close this window and return to "
        "ACNHRecomp.</p></body></html>";
    send(client, ok, int(std::strlen(ok)), MSG_NOSIGNAL);
    sock_close(client);
    std::string req(buf);
    size_t q = req.find('?');
    if (q == std::string::npos) return false;
    size_t sp = req.find(' ', q);
    std::string query = req.substr(q + 1, sp == std::string::npos ? std::string::npos : sp - q - 1);
    auto percent_decode = [](std::string v) {
        std::string out;
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i] == '%' && i + 2 < v.size()) {
                unsigned int x = 0;
                if (std::sscanf(v.c_str() + i + 1, "%02x", &x) == 1 ||
                    std::sscanf(v.c_str() + i + 1, "%02X", &x) == 1) {
                    out += char(x);
                    i += 2;
                    continue;
                }
            }
            out += v[i] == '+' ? ' ' : v[i];
        }
        return out;
    };
    std::string code, state;
    for (size_t i = 0; i < query.size();) {
        size_t amp = query.find('&', i);
        std::string pair = query.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
        size_t eq = pair.find('=');
        std::string k = pair.substr(0, eq);
        std::string v = eq == std::string::npos ? "" : percent_decode(pair.substr(eq + 1));
        if (k == "code") code = v;
        if (k == "state") state = v;
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    if (code.empty()) {
        set_error("OAuth callback missing authorization code.");
        return false;
    }
    if (state != expect_state) {
        set_error("OAuth state mismatch (try Sign in again).");
        return false;
    }
    code_out = code;
    return true;
}

bool exchange_token(const std::string& form, Account& out) {
    auto r = http_request("POST", api_base() + "/oauth/token",
                          {{"X-Nextendo-Client-Id", client_id()}, {"Accept", "application/json"}}, form,
                          "application/x-www-form-urlencoded");
    if (r.status != 200) {
        std::string err = r.error.empty() ? ("token exchange failed (HTTP " + std::to_string(r.status) + ")") : r.error;
        if (!r.body.empty()) {
            std::string snip = r.body.size() > 180 ? r.body.substr(0, 180) + "…" : r.body;
            err += ": " + snip;
        }
        set_error(err);
        return false;
    }
    out.access_token = json_get_string(r.body, "access_token");
    out.refresh_token = json_get_string(r.body, "refresh_token");
    uint64_t expires = json_get_u64(r.body, "expires_in");
    out.access_expires_unix =
        uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                     .count()) +
        (expires ? expires : 3600);
    out.signed_in = !out.access_token.empty();
    if (!out.signed_in) {
        set_error("token response missing access_token");
        return false;
    }
    auto ui = http_request("GET", api_base() + "/api/oauth/userinfo",
                           {{"Authorization", "Bearer " + out.access_token},
                            {"X-Nextendo-Client-Id", client_id()},
                            {"Accept", "application/json"}});
    if (ui.status == 200) {
        out.username = json_get_string(ui.body, "username");
        if (out.username.empty()) out.username = json_get_string(ui.body, "name");
        out.friend_code = json_get_string(ui.body, "friend_code");
        out.pid = json_get_u64(ui.body, "pid");
        if (!out.pid) out.pid = json_get_u64(ui.body, "id");
    }
    return true;
}

}  // namespace

void set_data_root(const std::string& path) {
    std::lock_guard lk(g_mu);
    g_data_root_override = path;
}

void set_mode_override(const std::string& mode) {
    std::lock_guard lk(g_mu);
    g_mode_override = mode;
}

void set_client_id_override(const std::string& id) {
    std::lock_guard lk(g_mu);
    g_client_id_override = id;
}

Mode mode() {
    {
        std::lock_guard lk(g_mu);
        if (g_mode_override == "local") return Mode::Local;
        if (g_mode_override == "nextendo") return Mode::Nextendo;
    }
#if !defined(NX_LAUNCHER_UI)
    if (config_get("online.mode", "nextendo") == "local") return Mode::Local;
#endif
    return Mode::Nextendo;
}

bool is_nextendo() { return mode() == Mode::Nextendo; }
bool is_local() { return mode() == Mode::Local; }

void set_game_version(const std::string& version) {
    std::lock_guard lk(g_mu);
    g_game_version = version;
}

std::string game_version_string() {
    {
        std::lock_guard lk(g_mu);
        if (!g_game_version.empty()) return g_game_version;
    }
    std::ifstream in(data_root() + "/game.ini");
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim(line.substr(0, eq)) == "version") return trim(line.substr(eq + 1));
    }
    return {};
}

bool version_ok_for_nextendo() {
    std::string v = game_version_string();
    if (v.empty()) return true;
    while (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(v.begin());
    return v == "3.0.3";
}

bool online_services_active() { return is_nextendo() && signed_in() && version_ok_for_nextendo(); }

bool ldn_wanted() {
    if (const char* e = std::getenv("NX_LDN"); e && e[0] == '1') return true;
#if defined(NX_LAUNCHER_UI)
    return false;
#else
    return is_local() && config_get("online.enabled", "0") == "1";
#endif
}

void load_account() {
    std::ifstream in(account_path());
    if (!in) return;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string j = ss.str();
    Account a;
    a.access_token = json_get_string(j, "access_token");
    a.refresh_token = json_get_string(j, "refresh_token");
    a.username = json_get_string(j, "username");
    a.friend_code = json_get_string(j, "friend_code");
    a.nex_token = json_get_string(j, "nex_token");
    a.pid = json_get_u64(j, "pid");
    a.access_expires_unix = json_get_u64(j, "access_expires_unix");
    a.signed_in = !a.access_token.empty();
    std::lock_guard lk(g_mu);
    g_account = std::move(a);
}

bool save_account() {
    Account a = account();
    std::ostringstream j;
    j << "{\n"
      << "  \"access_token\": \"" << a.access_token << "\",\n"
      << "  \"refresh_token\": \"" << a.refresh_token << "\",\n"
      << "  \"username\": \"" << a.username << "\",\n"
      << "  \"friend_code\": \"" << a.friend_code << "\",\n"
      << "  \"nex_token\": \"" << a.nex_token << "\",\n"
      << "  \"pid\": " << a.pid << ",\n"
      << "  \"access_expires_unix\": " << a.access_expires_unix << "\n"
      << "}\n";
#if !defined(NX_LAUNCHER_UI)
    plat::make_dirs(data_root());
#else
    std::error_code ec;
    std::filesystem::create_directories(data_root(), ec);
#endif
    std::ofstream out(account_path(), std::ios::binary);
    if (!out) return false;
    out << j.str();
    return true;
}

void clear_account() {
    {
        std::lock_guard lk(g_mu);
        g_account = Account{};
    }
    std::error_code ec;
    std::filesystem::remove(account_path(), ec);
}

Account account() {
    std::lock_guard lk(g_mu);
    return g_account;
}

void set_account(Account a) {
    std::lock_guard lk(g_mu);
    g_account = std::move(a);
}

bool signed_in() {
    std::lock_guard lk(g_mu);
    return g_account.signed_in && !g_account.access_token.empty();
}

// ACNHRecomp's registered public PKCE app on Nextendo Network.
constexpr const char* kDefaultClientId = "nxc_t63scKVq2hF1";

std::string client_id() {
    if (const char* e = std::getenv("NEXTENDO_CLIENT_ID"); e && e[0]) return e;
    {
        std::lock_guard lk(g_mu);
        if (!g_client_id_override.empty()) return g_client_id_override;
    }
#if !defined(NX_LAUNCHER_UI)
    {
        std::string from_cfg = config_get("online.nextendo.client_id", "");
        if (!from_cfg.empty()) return from_cfg;
    }
#endif
    return kDefaultClientId;
}

std::string api_base() {
    // Account + OAuth live on nextendo.network (api.nextendo.network does not resolve).
    std::string b = env_or("NEXTENDO_API", "https://nextendo.network");
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}

std::string auth_base() {
    std::string b = env_or("NEXTENDO_AUTH", api_base());
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}

std::string server_ip() { return env_or("NEXTENDO_SERVER_IP", "51.178.29.194"); }
std::string nat_ip() { return env_or("NEXTENDO_NAT_IP", "164.132.111.120"); }

bool sign_in_busy() { return g_sign_in_busy.load(); }

std::string last_error() {
    std::lock_guard lk(g_mu);
    return g_last_error;
}

void clear_error() {
    std::lock_guard lk(g_mu);
    g_last_error.clear();
}

void sign_out() {
    clear_account();
    clear_error();
}

void sign_in_start() {
    if (g_sign_in_busy.exchange(true)) return;
    clear_error();
    std::thread([] {
        struct Done {
            ~Done() { g_sign_in_busy = false; }
        } done;
        const std::string cid = client_id();
        if (cid.empty()) {
            set_error("Nextendo OAuth client id is empty.");
            return;
        }
        const std::string verifier = random_token(32);
        const std::string challenge = sha256_b64url(verifier);
        const std::string state = random_token(16);
        std::string code;
        std::string redirect_uri;
        if (!oauth_listen(state, code, 180, [&](uint16_t port) {
                redirect_uri = "http://127.0.0.1:" + std::to_string(port) + "/callback";
                const std::string url =
                    auth_base() + "/oauth/authorize?client_id=" + percent_encode(cid) +
                    "&redirect_uri=" + percent_encode(redirect_uri) + "&response_type=code&scope=" +
                    percent_encode("identity friends presence game.matchmaking") + "&state=" + percent_encode(state) +
                    "&code_challenge=" + percent_encode(challenge) + "&code_challenge_method=S256";
                if (!open_browser(url)) set_error("Could not open a browser for Nextendo sign-in.");
            })) {
            if (last_error().empty()) set_error("Sign-in timed out or was cancelled.");
            return;
        }
        if (code.empty()) return;
        const std::string form =
            "grant_type=authorization_code&client_id=" + percent_encode(cid) + "&code=" + percent_encode(code) +
            "&redirect_uri=" + percent_encode(redirect_uri) + "&code_verifier=" + percent_encode(verifier);
        Account a;
        if (!exchange_token(form, a)) return;
        set_account(a);
        save_account();
        fetch_nex_token();
        log("nextendo: signed in as %s", a.username.c_str());
    }).detach();
}

bool refresh_session() {
    Account a = account();
    if (a.refresh_token.empty()) return signed_in();
    const std::string form = "grant_type=refresh_token&client_id=" + percent_encode(client_id()) +
                             "&refresh_token=" + percent_encode(a.refresh_token);
    Account n;
    if (!exchange_token(form, n)) return false;
    if (n.refresh_token.empty()) n.refresh_token = a.refresh_token;
    set_account(n);
    save_account();
    return true;
}

bool fetch_nex_token() {
    Account a = account();
    if (a.access_token.empty()) return false;
    auto r = http_request("GET", api_base() + "/api/nex-token",
                          {{"Authorization", "Bearer " + a.access_token},
                           {"X-Nextendo-Client-Id", client_id()},
                           {"Accept", "application/json"}});
    if (r.status != 200) return false;
    a.nex_token = json_get_string(r.body, "token");
    if (a.nex_token.empty()) a.nex_token = json_get_string(r.body, "nex_token");
    set_account(a);
    save_account();
    return !a.nex_token.empty();
}

bool fetch_friends(std::vector<Friend>& out) {
    out.clear();
    Account a = account();
    if (a.access_token.empty()) return false;
    auto r = http_request("GET", api_base() + "/api/friends",
                          {{"Authorization", "Bearer " + a.access_token},
                           {"X-Nextendo-Client-Id", client_id()},
                           {"Accept", "application/json"}});
    if (r.status != 200) return false;
    size_t i = 0;
    while ((i = r.body.find('{', i)) != std::string::npos) {
        size_t j = r.body.find('}', i);
        if (j == std::string::npos) break;
        std::string obj = r.body.substr(i, j - i + 1);
        Friend f;
        f.pid = json_get_u64(obj, "pid");
        f.name = json_get_string(obj, "name");
        if (f.name.empty()) f.name = json_get_string(obj, "username");
        f.friend_code = json_get_string(obj, "friend_code");
        f.status = int(json_get_u64(obj, "status"));
        if (f.pid || !f.name.empty()) out.push_back(f);
        i = j + 1;
    }
    return true;
}

bool fetch_online_status(std::string& message) {
    message.clear();
    Account a = account();
    if (a.access_token.empty()) {
        message = "Not signed in to Nextendo.";
        return false;
    }
    auto r = http_request("GET", api_base() + "/api/online-status",
                          {{"Authorization", "Bearer " + a.access_token},
                           {"X-Nextendo-Client-Id", client_id()},
                           {"Accept", "application/json"}});
    if (r.status != 200) {
        message = r.error.empty() ? "Could not reach Nextendo." : r.error;
        return false;
    }
    message = json_get_string(r.body, "message");
    if (message.empty()) message = json_get_string(r.body, "reason");
    std::string ok = json_get_string(r.body, "ok");
    if (ok == "false" || ok == "0") return false;
    return true;
}

bool publish_presence(int status, uint64_t app_id) {
    Account a = account();
    if (a.access_token.empty()) return false;
    char body[128];
    std::snprintf(body, sizeof body, "{\"status\":%d,\"app_id\":%llu}", status, (unsigned long long)app_id);
    auto r = http_request("POST", api_base() + "/api/presence",
                          {{"Authorization", "Bearer " + a.access_token},
                           {"X-Nextendo-Client-Id", client_id()},
                           {"Accept", "application/json"}},
                          body, "application/json");
    return r.status >= 200 && r.status < 300;
}

static bool in_domain(const std::string& host, const char* domain) {
    if (host == domain) return true;
    const size_t n = std::strlen(domain);
    return host.size() > n && host.compare(host.size() - n, n, domain) == 0 && host[host.size() - n - 1] == '.';
}

RedirectTarget classify_host(const std::string& host_in) {
    std::string host = host_in;
    while (!host.empty() && host.back() == '.') host.pop_back();
    for (char& c : host) c = char(std::tolower(unsigned(c)));
    if (host.rfind("nncs2-", 0) == 0 && in_domain(host, "n.n.srv.nintendo.net")) return RedirectTarget::Nat;
    if (host == "g2b309e01-lp1.s.n.srv.nintendo.net") return RedirectTarget::Nat;
    static const char* domains[] = {"nintendo.net", "nintendo.com", "nintendowifi.net", "nintendo.co.jp",
                                    "demonware.net"};
    for (const char* d : domains)
        if (in_domain(host, d)) return RedirectTarget::Server;
    return RedirectTarget::None;
}

bool redirect_host(const std::string& host, std::string& out_ip) {
    out_ip.clear();
    if (!online_services_active()) return false;
    switch (classify_host(host)) {
    case RedirectTarget::Server:
        out_ip = server_ip();
        return true;
    case RedirectTarget::Nat:
        out_ip = nat_ip();
        return true;
    default:
        return false;
    }
}

void remember_sni(uint32_t ip_be, const std::string& host) {
    std::lock_guard lk(g_mu);
    g_ip_host[ip_be] = host;
}

std::string host_for_ip(uint32_t ip_be) {
    std::lock_guard lk(g_mu);
    auto it = g_ip_host.find(ip_be);
    return it == g_ip_host.end() ? std::string() : it->second;
}

uint64_t network_service_account_id() {
    Account a = account();
    return a.pid ? a.pid : 0x1234ABCD5678EF90ull;
}

std::string sign_baas_id_token(uint64_t nsaid, uint64_t title_id, uint32_t title_version) {
    (void)title_version;
    Account a = account();
    if (!a.nex_token.empty()) return a.nex_token;
    (void)std::getenv("NEXTENDO_BAAS_SIGNING_KEY");
    char payload[256];
    std::snprintf(payload, sizeof payload,
                  "{\"sub\":\"%016llx\",\"typ\":\"id_token\",\"nintendo\":{\"ai\":\"%016llx\"}}",
                  (unsigned long long)nsaid, (unsigned long long)title_id);
    const char* header = "{\"alg\":\"none\",\"typ\":\"JWT\"}";
    return b64url(reinterpret_cast<const uint8_t*>(header), std::strlen(header)) + "." +
           b64url(reinterpret_cast<const uint8_t*>(payload), std::strlen(payload)) + ".";
}

}  // namespace nx::nextendo
