// Nextendo mode, account session, DNS redirect classification, BAAS helpers.
#include "nextendo.h"

#include "nextendo_http.h"
#include "ldn_net.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
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
bool ensure_online_identity();

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

// End of the JSON value (object, array or string) starting at i: one past its closing character, npos if cut.
size_t json_value_end(const std::string& j, size_t i) {
    if (i >= j.size()) return std::string::npos;
    if (j[i] == '"') {
        for (size_t k = i + 1; k < j.size(); k++) {
            if (j[k] == '\\')
                k++;
            else if (j[k] == '"')
                return k + 1;
        }
        return std::string::npos;
    }
    int depth = 0;
    for (size_t k = i; k < j.size(); k++) {
        const char ch = j[k];
        if (ch == '"') {
            k = json_value_end(j, k);
            if (k == std::string::npos) return k;
            k--;
        } else if (ch == '{' || ch == '[') {
            depth++;
        } else if (ch == '}' || ch == ']') {
            if (--depth == 0) return k + 1;
        }
    }
    return std::string::npos;
}

// The raw value of a member of an object at its top level (nested objects are skipped), "" if absent.
std::string json_member(const std::string& obj, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    int depth = 0;
    for (size_t k = 0; k < obj.size(); k++) {
        const char ch = obj[k];
        if (ch == '{' || ch == '[') {
            depth++;
        } else if (ch == '}' || ch == ']') {
            depth--;
        } else if (ch == '"') {
            if (depth == 1 && obj.compare(k, pat.size(), pat) == 0) {
                size_t v = obj.find(':', k + pat.size());
                if (v == std::string::npos) return {};
                v = obj.find_first_not_of(" \t\r\n", v + 1);
                if (v == std::string::npos) return {};
                const bool nested = obj[v] == '{' || obj[v] == '[' || obj[v] == '"';
                const size_t e = nested ? json_value_end(obj, v) : obj.find_first_of(",}]", v);
                return e == std::string::npos ? std::string() : obj.substr(v, e - v);
            }
            const size_t e = json_value_end(obj, k);
            if (e == std::string::npos) return {};
            k = e - 1;
        }
    }
    return {};
}

// A member as text: strings unquoted (simple escapes dropped), numbers and literals as written.
std::string json_str(const std::string& obj, const char* key) {
    std::string v = trim(json_member(obj, key));
    if (v.size() >= 2 && v.front() == '"') {
        std::string out;
        for (size_t k = 1; k + 1 < v.size(); k++) {
            if (v[k] == '\\' && k + 2 < v.size()) k++;
            out += v[k];
        }
        return out;
    }
    return v == "null" ? std::string() : v;
}

uint64_t json_u64(const std::string& obj, const char* key) {
    return std::strtoull(json_str(obj, key).c_str(), nullptr, 10);
}

// The objects of an array value.
std::vector<std::string> json_objects(const std::string& arr) {
    std::vector<std::string> out;
    for (size_t k = 1; k < arr.size();) {
        k = arr.find('{', k);
        if (k == std::string::npos) break;
        const size_t e = json_value_end(arr, k);
        if (e == std::string::npos) break;
        out.push_back(arr.substr(k, e - k));
        k = e;
    }
    return out;
}

uint64_t parse_hex16(const std::string& v) {
    if (v.size() != 16 || v.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return 0;
    return std::strtoull(v.c_str(), nullptr, 16);
}

uint64_t now_unix() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
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
    auto r = http_request("POST", api_base() + "/api/oauth/token",
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
    out.access_token = json_str(r.body, "access_token");
    // Each refresh rotates the refresh token (the old one stops working a minute later): always keep the new one.
    if (std::string rt = json_str(r.body, "refresh_token"); !rt.empty()) out.refresh_token = rt;
    const uint64_t expires = json_u64(r.body, "expires_in");
    out.access_expires_unix = now_unix() + (expires ? expires : 3600);
    out.signed_in = !out.access_token.empty();
    if (!out.signed_in) {
        set_error("token response missing access_token");
        return false;
    }
    auto ui = http_request("GET", api_base() + "/api/oauth/userinfo",
                           {{"Authorization", "Bearer " + out.access_token},
                            {"X-Nextendo-Client-Id", client_id()},
                            {"Accept", "application/json"}});
    if (ui.status == 200) {  // fills what it has; a refresh keeps the identity already known
        std::string name = json_str(ui.body, "username");
        if (name.empty()) name = json_str(ui.body, "name");
        if (!name.empty()) out.username = name;
        if (std::string fc = json_str(ui.body, "friend_code"); !fc.empty()) out.friend_code = fc;
        if (uint64_t pid = json_u64(ui.body, "pid")) out.pid = pid;
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
    a.nex_expires_unix = json_get_u64(j, "nex_expires_unix");
    a.nsa = parse_hex16(json_get_string(j, "nsa"));
    a.signed_in = !a.access_token.empty();
    std::lock_guard lk(g_mu);
    g_account = std::move(a);
}

bool save_account() {
    Account a = account();
    char nsa_hex[17] = {};
    if (a.nsa) std::snprintf(nsa_hex, sizeof nsa_hex, "%016llx", (unsigned long long)a.nsa);
    std::ostringstream j;
    j << "{\n"
      << "  \"access_token\": \"" << a.access_token << "\",\n"
      << "  \"refresh_token\": \"" << a.refresh_token << "\",\n"
      << "  \"username\": \"" << a.username << "\",\n"
      << "  \"friend_code\": \"" << a.friend_code << "\",\n"
      << "  \"nex_token\": \"" << a.nex_token << "\",\n"
      << "  \"pid\": " << a.pid << ",\n"
      << "  \"access_expires_unix\": " << a.access_expires_unix << ",\n"
      << "  \"nex_expires_unix\": " << a.nex_expires_unix << ",\n"
      << "  \"nsa\": \"" << nsa_hex << "\"\n"
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

// Every account request carries the bearer token, so an override may only name Nextendo over https, or this machine
// (a developer's test server). Anything else falls back to the real server, loudly.
static std::string checked_base(const char* var, const std::string& fallback) {
    std::string b = env_or(var, fallback);
    while (!b.empty() && b.back() == '/') b.pop_back();
    if (b == fallback) return b;
    std::string host;
    bool https = false;
    if (b.rfind("https://", 0) == 0) {
        host = b.substr(8), https = true;
    } else if (b.rfind("http://", 0) == 0) {
        host = b.substr(7);
    }
    host = host.substr(0, host.find_first_of(":/"));
    for (char& ch : host) ch = char(std::tolower(unsigned(ch)));
    const bool loopback = host == "127.0.0.1" || host == "localhost" || host == "[::1]";
    const bool nextendo = host == "nextendo.network" ||
                          (host.size() > 17 && host.compare(host.size() - 17, 17, ".nextendo.network") == 0);
    if (loopback || (https && nextendo)) return b;
    log("nextendo: ignoring %s=%s (only https://*.nextendo.network or loopback); using %s", var, b.c_str(),
        fallback.c_str());
    return fallback;
}

std::string api_base() { return checked_base("NEXTENDO_API", "https://nextendo.network"); }

std::string auth_base() { return checked_base("NEXTENDO_AUTH", api_base()); }

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
                    auth_base() + "/api/oauth/authorize?client_id=" + percent_encode(cid) +
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
        ensure_online_identity();
        log("nextendo: signed in as %s", account().username.c_str());
    }).detach();
}

bool refresh_session() {
    Account a = account();
    if (a.refresh_token.empty()) return signed_in();
    const std::string form = "grant_type=refresh_token&client_id=" + percent_encode(client_id()) +
                             "&refresh_token=" + percent_encode(a.refresh_token);
    Account n = a;  // identity, nex token and nsa survive a refresh
    if (!exchange_token(form, n)) return false;
    set_account(n);
    save_account();
    return true;
}

namespace {

std::map<std::string, std::string> bearer(const std::string& token) {
    return {{"Authorization", "Bearer " + token}, {"X-Nextendo-Client-Id", client_id()}, {"Accept", "application/json"}};
}

// GET with the access token; one refresh and retry on 401.
HttpResponse api_get(const std::string& path) {
    auto r = http_request("GET", api_base() + path, bearer(account().access_token));
    if (r.status == 401 && refresh_session()) r = http_request("GET", api_base() + path, bearer(account().access_token));
    return r;
}

}  // namespace

// GET /api/nex-token (scope game.matchmaking): {nex_token, pid, friend_code, username, expires_in}; 24 h.
bool fetch_nex_token() {
    if (account().access_token.empty()) return false;
    auto r = api_get("/api/nex-token");
    if (r.status != 200) {
        set_error(r.status == 403 ? "Nextendo refused a game token (approve the game.matchmaking permission, or check "
                                    "the account: confirmed e-mail, linked Discord)."
                                  : "Could not get a Nextendo game token (HTTP " + std::to_string(r.status) + ").");
        return false;
    }
    Account a = account();
    a.nex_token = json_str(r.body, "nex_token");
    if (a.nex_token.empty()) a.nex_token = json_str(r.body, "token");
    if (uint64_t pid = json_u64(r.body, "pid")) a.pid = pid;
    if (std::string fc = json_str(r.body, "friend_code"); !fc.empty()) a.friend_code = fc;
    if (std::string u = json_str(r.body, "username"); !u.empty()) a.username = u;
    const uint64_t ttl = json_u64(r.body, "expires_in");
    a.nex_expires_unix = now_unix() + (ttl ? ttl : 24 * 3600);
    set_account(a);
    save_account();
    return !a.nex_token.empty();
}

// GET /api/me: "nsa" is the account's baasUserID (16 hex digits), the id a Switch on this account announces. ACNH
// compares the id each player announces with its friend list, so emulator and Switch must use the same one.
static bool fetch_nsa() {
    auto r = api_get("/api/me");
    if (r.status != 200) return false;
    const uint64_t nsa = parse_hex16(json_str(r.body, "nsa"));
    if (!nsa) return false;
    Account a = account();
    a.nsa = nsa;
    set_account(a);
    save_account();
    return true;
}

bool ensure_online_identity() {
    if (!signed_in()) return false;
    const uint64_t now = now_unix();
    Account a = account();
    if (a.access_expires_unix && a.access_expires_unix < now + 120 && !refresh_session()) return false;
    a = account();
    if ((a.nex_token.empty() || a.nex_expires_unix < now + 600) && !fetch_nex_token()) return false;
    if (!account().nsa) fetch_nsa();  // optional: without it the pid stands in, as on servers without the field
    return true;
}

// GET /api/friends: {"friends":[{pid, name, nsa, favorite, presence:{status, app_id, ...}}, ...]}.
bool fetch_friends(std::vector<Friend>& out) {
    out.clear();
    if (account().access_token.empty()) return false;
    auto r = api_get("/api/friends");
    log("nextendo: GET /api/friends -> HTTP %d", r.status);  // DIAG
    if (r.status != 200) return false;
    for (const std::string& o : json_objects(json_member(r.body, "friends"))) {
        Friend f;
        f.pid = json_u64(o, "pid");
        f.nsa = parse_hex16(json_str(o, "nsa"));
        f.name = json_str(o, "name");
        if (f.name.empty()) f.name = json_str(o, "username");
        f.friend_code = json_str(o, "friend_code");
        f.favorite = json_str(o, "favorite") == "true";
        const std::string pres = json_member(o, "presence");
        if (!pres.empty()) {
            f.status = int(json_u64(pres, "status"));
            f.app_id = json_str(pres, "app_id");
        }
        if (f.pid || f.nsa) out.push_back(std::move(f));
    }
    return true;
}

namespace {
std::mutex g_friends_mu;
std::vector<Friend> g_friends;
std::chrono::steady_clock::time_point g_friends_at{};
std::atomic<bool> g_friends_busy{false};
}  // namespace

std::vector<Friend> friends_cached() {
    bool cold;
    {
        std::lock_guard lk(g_friends_mu);
        cold = !g_friends_at.time_since_epoch().count();
    }
    // The first read waits for the list: a game that reads it once at start would otherwise keep an empty one.
    if (cold) friends_refresh_blocking();
    friends_refresh_async();
    std::lock_guard lk(g_friends_mu);
    return g_friends;
}

void friends_refresh_async() {
    {
        std::lock_guard lk(g_friends_mu);
        if (g_friends_at.time_since_epoch().count() &&
            std::chrono::steady_clock::now() - g_friends_at < std::chrono::seconds(20))
            return;
    }
    if (g_friends_busy.exchange(true)) return;
    std::thread([] {
        std::vector<Friend> list;
        const bool ok = fetch_friends(list);
        {
            std::lock_guard lk(g_friends_mu);
            if (ok) g_friends = std::move(list);
            g_friends_at = std::chrono::steady_clock::now();  // a failure also waits 20 s before the next try
        }
        g_friends_busy = false;
    }).detach();
}

void friends_refresh_blocking() {
    std::vector<Friend> list;
    const bool ok = fetch_friends(list);
    std::lock_guard lk(g_friends_mu);
    if (ok) g_friends = std::move(list);
    g_friends_at = std::chrono::steady_clock::now();  // a failure also waits for the next 20 s refresh
}

// ---- presence -------------------------------------------------------------------------------------------------

namespace {
std::atomic<bool> g_presence_running{false}, g_playing{false};
std::mutex g_presence_mu;
std::condition_variable g_presence_cv;

// POST /api/presence {status, app_field, app_id, app_detail}: 1 online, 2 in an online session.
bool post_presence(int status) {
    char body[160];
    std::snprintf(body, sizeof body, "{\"status\":%d,\"app_field\":\"\",\"app_id\":\"%016llX\",\"app_detail\":\"\"}",
                  status, (unsigned long long)kAcnhTitleId);
    auto r = http_request("POST", api_base() + "/api/presence", bearer(account().access_token), body,
                          "application/json");
    if (r.status == 401 && refresh_session())
        r = http_request("POST", api_base() + "/api/presence", bearer(account().access_token), body,
                         "application/json");
    return r.status >= 200 && r.status < 300;
}

void presence_thread() {
    while (g_presence_running) {
        if (online_services_active()) {
            ensure_online_identity();  // keeps the access and nex tokens fresh during long sessions
            post_presence(g_playing ? 2 : 1);
        }
        std::unique_lock lk(g_presence_mu);
        g_presence_cv.wait_for(lk, std::chrono::seconds(60), [] { return !g_presence_running.load(); });
    }
}
}  // namespace

void presence_start() {
    if (g_presence_running.exchange(true)) return;
    std::thread(presence_thread).detach();
}

void presence_stop() {
    g_presence_running = false;
    g_presence_cv.notify_all();
}

void presence_set_playing(bool playing) {
    if (g_playing.exchange(playing) == playing) return;
    g_presence_cv.notify_all();  // publish the change now, not at the next minute
    if (g_presence_running) std::thread([playing] {
        if (online_services_active()) post_presence(playing ? 2 : 1);
    }).detach();
}

// ---- DNS redirect ---------------------------------------------------------------------------------------------

static bool in_domain(const std::string& host, const char* domain) {
    if (host == domain) return true;
    const size_t n = std::strlen(domain);
    return host.size() > n && host.compare(host.size() - n, n, domain) == 0 && host[host.size() - n - 1] == '.';
}

// Most specific first: the NAT-check names go to the second vantage point (it must be a distinct address, or the
// game's NAT check never completes), then every Nintendo domain to the main server so no name leaks to Nintendo.
RedirectTarget classify_host(const std::string& host_in) {
    std::string host = host_in;
    while (!host.empty() && host.back() == '.') host.pop_back();
    for (char& c : host) c = char(std::tolower(unsigned(c)));
    if (host.rfind("nncs2-", 0) == 0 && in_domain(host, "n.n.srv.nintendo.net")) return RedirectTarget::Nat;
    if (host == "g2b309e01-lp1.s.n.srv.nintendo.net" || host == "g2df33d01-lp1.p.srv.nintendo.net")
        return RedirectTarget::Nat;
    static const char* domains[] = {"nintendo.net", "nintendo.com", "nintendowifi.net", "nintendo.co.jp"};
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
    // Several names share the server address; the first stays, as a connection by IP is rare and any of them routes.
    g_ip_host.emplace(ip_be, host);
}

std::string host_for_ip(uint32_t ip_be) {
    std::lock_guard lk(g_mu);
    auto it = g_ip_host.find(ip_be);
    return it == g_ip_host.end() ? std::string() : it->second;
}

uint64_t network_service_account_id() {
    Account a = account();
    if (a.nsa) return a.nsa;
    return a.pid ? a.pid : 0x1234ABCD5678EF90ull;
}

}  // namespace nx::nextendo
