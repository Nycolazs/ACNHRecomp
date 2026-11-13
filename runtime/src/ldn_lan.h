// This computer on the local network, for local play (ldn_lan.cpp): shared by the game and the launcher.
#pragma once
#include <cstdint>
#include <string>

namespace nx {

uint32_t lan_bind_ip();               // NX_LDN_IP (host byte order) when set: every socket of local play is bound to it; else 0
uint32_t route_address(uint32_t to);  // this computer's address towards that one (no packet is sent); 0 if none
// This computer's address on its network: NX_LDN_IP, else the default route's. A host that has no station yet answers
// each search with its address towards the asker instead, and keeps the one the first station connected to.
uint32_t local_address();
std::string ip_text(uint32_t ip);
// A private (10/8, 172.16/12, 192.168/16), link-local (169.254/16), CGNAT (100.64/10: Tailscale) or loopback address.
bool lan_private_ip(uint32_t ip);
std::string ldn_island_name(const uint8_t* info);  // the host island's name in a NetworkInfo's advertise data (UTF-8)

// Whether an island answers at a friend's address (host[:port], default 11452), in the background.
enum class AddressCheck { Idle, Running, Bad, NoAnswer, Found };
struct AddressResult {
    AddressCheck check = AddressCheck::Idle;
    std::string island;  // its name, when found
};
void address_check_start(const std::string& address);
AddressResult address_check_result();

}  // namespace nx
