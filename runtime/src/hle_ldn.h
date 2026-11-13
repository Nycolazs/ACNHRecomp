// What nn::ldn (hle_ldn.cpp), nn::socket on its network (hle_vsock.cpp) and the link (ldn_link.cpp) share.
#pragma once
#include <cstdint>
#include <string>

#include "ldn_lan.h"

namespace nx {

bool ldn_enabled();          // the game is in a local play session (online.enabled or NX_LDN=1 when it started)
std::string ldn_visited_island();  // the host's island while this console is a guest there, else empty
bool ldn_net_up();           // a network exists: this console made or joined it
uint32_t ldn_own_ip();       // 0 without a network (host byte order, as every address here)
uint32_t ldn_broadcast_ip();
int ldn_peer_ips(uint32_t* out);  // the other nodes' addresses (8 at most): where a broadcast goes

void vsock_reset();  // the session is over: no socket stays open for the next one

// The link between consoles, ldn_mitm's protocol (ldn_link.cpp).
void link_host_start();
void link_stop();           // host or station: every connection ends (no lock of hle_ldn.cpp held)
void link_push_info();      // host: the NetworkInfo to every station
int link_scan(uint8_t* out, int max, const uint8_t* filter);  // NetworkInfos of hosts nearby: how many
// Joins the host of the scanned network with this BSSID, as this node (its address filled in here): the node it got,
// and the NetworkInfo; -1 on failure.
int link_join(const uint8_t* bssid, uint8_t* node, uint8_t* info);

// What the settings screen shows.
struct LinkStatus {
    int players = 0;       // connected to this host, itself excluded
    bool hosting = false;  // the port is really open now
};
LinkStatus link_status();

// What the link asks hle_ldn.cpp.
bool ldn_link_info(uint8_t* out);           // the hosted network's NetworkInfo
// host: a station's NodeInfo (0x40) in a free slot: its number, or -1. own_ip: this computer's address on the station's
// connection, which becomes the host's node address when it is the first station (0: keep it).
int ldn_link_add_node(const uint8_t* node, uint32_t own_ip);
void ldn_link_remove_node(int node);
void ldn_link_set_info(const uint8_t* info);  // station: the host's new NetworkInfo
void ldn_link_lost();                         // station: the host is gone
// A NetworkInfo from the network: whether its sizes and counts make sense (strings get terminated).
bool ldn_info_valid(uint8_t* info);

}  // namespace nx
