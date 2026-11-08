// The Online page's "Server status" check: is each part of Nextendo that ACNH relies on answering right now.
#include <cstdlib>
#include <string>

#include "nextendo.h"
#include "nextendo_http.h"

namespace nx::nextendo {

ServiceStatus check_service_status() {
    ServiceStatus s;
    // Nextendo's own service.
    s.health = http_request("GET", api_base() + "/api/health", {{"Accept", "application/json"}}).status;
    // The account service behind sign-in, friends and presence (it can fail on its own: 502 while the rest is up).
    const Account a = account();
    if (!a.access_token.empty())
        s.account = http_request("GET", api_base() + "/api/me",
                                 {{"Authorization", "Bearer " + a.access_token},
                                  {"X-Nextendo-Client-Id", client_id()},
                                  {"Accept", "application/json"}})
                        .status;
    // The game servers: the island services and NEX share the address the game's Nintendo names go to. Any HTTP answer
    // from the island services' name means that machine is up.
    const char* host = "api.hac.lp1.acbaa.srv.nintendo.net";
    game_server_get(host, std::string("GET / HTTP/1.1\r\nHost: ") + host + "\r\nAccept: */*\r\n\r\n", &s.game);
    // How many play ACNH online right now: {"counts":{"<title id>":N,...}}.
    const auto counts = http_request("GET", api_base() + "/api/online-counts", {{"Accept", "application/json"}});
    if (counts.status == 200) {
        const std::string key = "\"01006f8002326000\":";
        const size_t i = counts.body.find(key);
        if (i != std::string::npos) s.players = std::atoi(counts.body.c_str() + i + key.size());
    }
    return s;
}

}  // namespace nx::nextendo
