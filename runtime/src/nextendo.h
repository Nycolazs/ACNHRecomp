// Nextendo Network client: mode, account session, hostname redirect, and helpers the HLE / launcher share.
// Clean-room against public Nextendo docs and server behaviour — no GPL / PolyForm client code.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nx::nextendo {

enum class Mode { Nextendo, Local };

struct Account {
    bool signed_in = false;
    uint64_t pid = 0;  // Network ID — never log or show in UI
    std::string username;
    std::string friend_code;
    std::string access_token;
    std::string refresh_token;
    std::string nex_token;  // from /api/nex-token: the game login proof (nnex claim of the BAAS id_token)
    uint64_t access_expires_unix = 0;
    uint64_t nex_expires_unix = 0;
    uint64_t nsa = 0;  // baasUserID from /api/me "nsa": what a Switch on this account announces
};

struct Friend {
    uint64_t pid = 0;
    uint64_t nsa = 0;  // 0 when the server has none for this friend
    std::string name;
    std::string friend_code;
    int status = 0;  // 0 offline, 1 online, 2 playing online
    std::string app_id;  // title being played, 16 hex digits, or ""
    bool favorite = false;
};

// ---- mode / version -------------------------------------------------------------------------------------------

void set_data_root(const std::string& path);  // launcher: build data folder (account file lives here)
void set_mode_override(const std::string& mode);  // "nextendo" / "local" / "" to clear (launcher Ini)
void set_client_id_override(const std::string& id);  // launcher before OAuth

Mode mode();  // online.mode: nextendo (default) or local
bool is_nextendo();
bool is_local();
bool version_ok_for_nextendo();  // game build reports 3.0.3
std::string game_version_string();  // "" if unknown
void set_game_version(const std::string& version);  // launcher / runtime once known

// Online services (Dodo / Search) may run when Nextendo mode, signed in, and on 3.0.3.
bool online_services_active();
// LDN peer path (docs/LOCAL_PLAY.md) when online.enabled, in either mode.
bool ldn_wanted();

// ---- account persistence --------------------------------------------------------------------------------------

void load_account();   // <data root>/nextendo_account.json
bool save_account();
void clear_account();
Account account();
void set_account(Account a);
bool signed_in();

// ---- OAuth / API ----------------------------------------------------------------------------------------------

std::string client_id();   // env / override / config / built-in ACNHRecomp app id
std::string api_base();    // NEXTENDO_API or https://nextendo.network
std::string auth_base();   // NEXTENDO_AUTH or api_base(); browser /oauth/authorize host
std::string server_ip();   // NEXTENDO_SERVER_IP or 51.178.29.194
std::string nat_ip();      // NEXTENDO_NAT_IP or 164.132.111.120

// Browser PKCE sign-in on a background thread; poll sign_in_busy / last_error.
void sign_in_start();
void sign_out();
bool sign_in_busy();
std::string last_error();
void clear_error();

// Refresh the access token (1 h) and the nex token (24 h) when close to expiry; fetch the baasUserID once.
// Blocking HTTP: call from a worker thread. False when the account cannot go online (last_error() says why).
bool refresh_session();
bool fetch_nex_token();
bool ensure_online_identity();
bool fetch_friends(std::vector<Friend>& out);
// Friend list cached by a background refresh (at most every 20 s); never blocks the game.
std::vector<Friend> friends_cached();
void friends_refresh_async();
void friends_refresh_blocking();  // now, from a thread that may wait (EnsureFriendListAvailable)

// Presence (POST /api/presence): published at least every 60 s while online services are active, or the server
// shows the player offline after 90 s. playing: inside an online session (nn::friends::DeclareOpenOnlinePlaySession).
void presence_start();
void presence_stop();
void presence_set_playing(bool playing);

// ---- DNS redirect ---------------------------------------------------------------------------------------------

enum class RedirectTarget { None, Server, Nat };
RedirectTarget classify_host(const std::string& host);
// When online_services_active(), Nintendo hosts resolve to server/nat IPs; else None and out_ip empty.
bool redirect_host(const std::string& host, std::string& out_ip);
void remember_sni(uint32_t ip_be, const std::string& host);  // for TLS SNI recovery
std::string host_for_ip(uint32_t ip_be);

// ---- BAAS id_token (runtime only: nextendo_token.cpp) ---------------------------------------------------------

// The id_token the game forwards at NEX login and to the island services: RS256 JWT, BAAS claim shape, carrying
// the nex token in "nnex". Signed with NEXTENDO_BAAS_SIGNING_KEY (PEM text or path) or <data root>/nextendo_baas.pem,
// else a key generated for this run. Empty when not signed in.
std::string baas_id_token(const std::string& display_version);

// Network Service Account id presented to the game: the baasUserID when known, else the pid.
uint64_t network_service_account_id();

constexpr uint64_t kAcnhTitleId = 0x01006F8002326000ull;

}  // namespace nx::nextendo
