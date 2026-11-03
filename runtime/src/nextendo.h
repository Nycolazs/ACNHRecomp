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
    std::string nex_token;  // from /api/nex-token; for NEX login
    uint64_t access_expires_unix = 0;
};

struct Friend {
    uint64_t pid = 0;
    std::string name;
    std::string friend_code;
    int status = 0;  // 0 offline, 1 online, 2 playing
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
// LDN peer path (docs/LOCAL_PLAY.md) when Local mode and online.enabled.
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

// Refresh access token if needed; fetch nex-token and friends. False on hard failure.
bool refresh_session();
bool fetch_nex_token();
bool fetch_friends(std::vector<Friend>& out);
bool fetch_online_status(std::string& message);  // human-readable gate reason when blocked
bool publish_presence(int status, uint64_t app_id);

// ---- DNS redirect ---------------------------------------------------------------------------------------------

enum class RedirectTarget { None, Server, Nat };
RedirectTarget classify_host(const std::string& host);
// When online_services_active(), Nintendo hosts resolve to server/nat IPs; else None and out_ip empty.
bool redirect_host(const std::string& host, std::string& out_ip);
void remember_sni(uint32_t ip_be, const std::string& host);  // for TLS SNI recovery
std::string host_for_ip(uint32_t ip_be);

// ---- BAAS id_token (optional local signing) -------------------------------------------------------------------

// Signs a BAAS-style id_token with NEXTENDO_BAAS_SIGNING_KEY / nextendo_baas.pem when present.
// Empty if no key: game may still use nex_token path depending on server.
std::string sign_baas_id_token(uint64_t nsaid, uint64_t title_id, uint32_t title_version);

// Network Service Account id presented to the game (from Nextendo pid).
uint64_t network_service_account_id();

// ---- NAT / P2P ------------------------------------------------------------------------------------------------

void nat_keepalive_start();
void nat_keepalive_stop();

}  // namespace nx::nextendo
