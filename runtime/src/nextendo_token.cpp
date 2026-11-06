// The BAAS id_token the game hands the Nextendo servers (NEX login extra data, island services' Authorization).
// Nextendo's documented contract: an RS256 JWT in the shape of Nintendo's account server tokens whose "nnex" claim
// carries the nex token from GET /api/nex-token. The server checks that nex token (its signature, expiry and the
// pid it proves); a bare pid, the OAuth token or an empty claim is refused.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <ctime>

#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

#include <nx/runtime.h>

#include "nextendo.h"
#include "platform.h"

namespace nx {

bool tls_ready();                                     // hle_ssl.cpp
int tls_random(void*, unsigned char* out, size_t n);  // hle_ssl.cpp

namespace nextendo {

namespace {

constexpr const char* kIssuer = "https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
constexpr const char* kAudience = "ed9e2f05d286f7b8";
constexpr const char* kKeyId = "nextendo-baas-key-1";

std::mutex g_mu;
mbedtls_pk_context g_key;
bool g_key_ready = false;

std::string b64url(const void* data, size_t n) {
    std::string out(4 * ((n + 2) / 3) + 1, '\0');
    size_t len = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &len,
                          static_cast<const unsigned char*>(data), n);
    out.resize(len);
    for (char& ch : out) ch = ch == '+' ? '-' : ch == '/' ? '_' : ch;
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

std::string b64url(const std::string& s) { return b64url(s.data(), s.size()); }

std::string hex(const unsigned char* p, size_t n) {
    std::string out;
    char b[3];
    for (size_t i = 0; i < n; i++) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        out += b;
    }
    return out;
}

std::string random_hex(size_t bytes) {
    unsigned char buf[32];
    tls_random(nullptr, buf, bytes);
    return hex(buf, bytes);
}

std::string json_escape(const std::string& v) {
    std::string out;
    for (char ch : v) {
        if (uint8_t(ch) < 0x20) continue;
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    return out;
}

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    if (in) ss << in.rdbuf();
    return ss.str();
}

// The signing key: NEXTENDO_BAAS_SIGNING_KEY (PEM text, or a path to it), <data root>/nextendo_baas.pem, else a
// fresh 2048-bit key for this run. Games that only forward the token (ACNH) do not need Nextendo's own key: the
// server trusts the nnex proof inside, not this signature.
bool load_key() {
    if (g_key_ready) return true;
    if (!tls_ready()) return false;
    mbedtls_pk_init(&g_key);
    std::string pem;
    if (const char* e = std::getenv("NEXTENDO_BAAS_SIGNING_KEY"); e && e[0])
        pem = std::strstr(e, "BEGIN") ? std::string(e) : read_text(e);
    if (pem.empty()) pem = read_text(plat::data_root() + "/nextendo_baas.pem");
    if (!pem.empty()) {
        if (mbedtls_pk_parse_key(&g_key, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1, nullptr,
                                 0, tls_random, nullptr) == 0 &&
            mbedtls_pk_can_do(&g_key, MBEDTLS_PK_RSA)) {
            log("nextendo: id_token signed with the configured key");
            return g_key_ready = true;
        }
        log("nextendo: the configured id_token key is not an RSA PEM key; using a key generated for this run");
        mbedtls_pk_free(&g_key);
        mbedtls_pk_init(&g_key);
    }
    if (mbedtls_pk_setup(&g_key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0 ||
        mbedtls_rsa_gen_key(mbedtls_pk_rsa(g_key), tls_random, nullptr, 2048, 65537) != 0) {
        mbedtls_pk_free(&g_key);
        return false;
    }
    return g_key_ready = true;
}

}  // namespace

std::string baas_id_token(const std::string& display_version) {
    const Account a = account();
    if (!a.signed_in || a.nex_token.empty()) return {};
    std::lock_guard lk(g_mu);
    if (!load_key()) return {};
    const uint64_t now = uint64_t(std::time(nullptr));
    char sub[17];
    std::snprintf(sub, sizeof sub, "%016llx", (unsigned long long)network_service_account_id());

    const std::string header =
        std::string("{\"alg\":\"RS256\",\"typ\":\"id_token\",\"kid\":\"") + kKeyId + "\",\"jku\":\"" + kIssuer +
        "/1.0.0/certificates\"}";
    std::ostringstream p;
    p << "{\"iss\":\"" << kIssuer << "\",\"aud\":\"" << kAudience << "\",\"sub\":\"" << sub << "\""
      << ",\"typ\":\"id_token\",\"jti\":\"" << random_hex(16) << "\",\"iat\":" << now << ",\"nbf\":" << now
      << ",\"exp\":" << now + 3 * 3600 << ",\"jku\":\"" << kIssuer << "/1.0.0/certificates\""
      << ",\"di\":\"" << random_hex(8) << "\",\"sn\":\"XAW10000000000\",\"bs:did\":\"" << random_hex(8) << "\""
      << ",\"hm\":true,\"nnex\":\"" << json_escape(a.nex_token) << "\"";
    if (!display_version.empty()) p << ",\"tv\":\"" << json_escape(display_version) << "\"";
    p << "}";

    const std::string signing_input = b64url(header) + "." + b64url(p.str());
    unsigned char digest[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(signing_input.data()), signing_input.size(), digest, 0);
    unsigned char sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t sig_len = 0;
    if (mbedtls_pk_sign(&g_key, MBEDTLS_MD_SHA256, digest, sizeof digest, sig, sizeof sig, &sig_len, tls_random,
                        nullptr) != 0)
        return {};
    return signing_input + "." + b64url(sig, sig_len);
}

}  // namespace nextendo
}  // namespace nx
