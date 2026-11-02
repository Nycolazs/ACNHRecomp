// UPnP for local play's port (ldn_upnp.cpp).
#pragma once
#include <cstdint>
#include <string>

namespace nx {

// What is known about reaching this computer from the internet.
enum class LinkCheck { Idle, Running, NoRouter, Refused, NoPublicAddress, Open, OpenVerified };

struct Igd {
    std::string host, path, service, lan;
    uint16_t port = 0;
};

bool upnp_find(Igd& igd);  // a router that answers: blocks a few seconds
// Maps a free external port to port: Open with the external port and the address to give, or what went wrong.
LinkCheck upnp_open(const Igd& igd, uint16_t port, uint16_t& external, std::string& address);
void upnp_renew(const Igd& igd, uint16_t external, uint16_t port);
void upnp_close(const Igd& igd, uint16_t external);
LinkCheck upnp_check(uint16_t port, std::string& address);  // the whole test; leaves nothing open

// Whether an island answers at a friend's address (host[:port]), in the background.
enum class AddressCheck { Idle, Running, Bad, NoAnswer, Found };
struct AddressResult {
    AddressCheck check = AddressCheck::Idle;
    std::string island;  // its name, when found
};
void address_check_start(const std::string& address);
AddressResult address_check_result();

}  // namespace nx
