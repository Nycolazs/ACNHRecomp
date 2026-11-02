# Local play over the local network

Goal (2026-10-26): the airport's **local play** (visiting another island, receiving visitors, up to 8 players)
between computers running ACNHRecomp on the same network. A virtual LAN (ZeroTier, Hamachi) counts as the same
network; Tailscale does not forward broadcasts, so a peer's address can also be typed in. Nintendo's online servers
are never contacted: only machines running this project play together, each with its own copy of the game.

On the Switch, local play is `nn::ldn` (a wireless network between consoles, no router) carrying the game's own
traffic (Pia) over UDP sockets with addresses in the LDN subnet. The game calls both as APIs the runtime implements
by name (`runtime/src/hle_ldn.cpp`, `runtime/src/hle_net.cpp`), so both can be carried over the local network: the
LDN network becomes UDP broadcasts (discovery) and a session between hosts, and the game's sockets on LDN addresses
become real UDP sockets towards the peers' real addresses.

## How to play

In the launcher or the game's settings (F1), page **Online**, turn it on. It can be changed while the game runs: it is
read when you next talk to Orville, and held until that session ends.

- **On the same network**: nothing else. The host opens the airport (Local Play > invite), the others ask Orville to
  travel by Local Play and the island is listed.
- **Over the internet**: the host copies the address the page shows and sends it to friends, who paste it under
  "Friend's address" (the page tests it and names the island), then talk to Orville.

The host's port opens by itself when the island opens, and closes when it closes. Only the host has to be reachable: a
visitor can be anywhere, a phone's hotspot included. A connection without a public address (carrier-grade NAT) can visit
but not host; the page says so, and a virtual LAN (ZeroTier, Hamachi, Tailscale) with the host's address there works.

**Play with people you trust.** Your address is your connection's, so it stays hidden until you ask and never goes to the
log. The game's own network code was written for consoles and is not meant to face strangers.

## How it works

Direct between players: no server of ours, only each computer and its router. Files (`runtime/src/`):

| File | What it does |
|---|---|
| `hle_ldn.cpp` | `nn::ldn`, what the game calls: states, the `NetworkInfo`, the setting held for a session |
| `hle_vsock.cpp` | `nn::socket` on that network: the game's UDP (Pia, one socket on port 12345) as queues |
| `ldn_discovery.cpp` | Finding islands: UDP answers on the LAN, TCP to the addresses given |
| `ldn_link.cpp` | The host's TCP port (default 39500), each visitor's one connection, the relay of the game's packets |
| `ldn_upnp.cpp` | The router: opens, renews and closes the port, the test. Also used by the launcher |
| `ldn_net.h` | Sockets and the link's frames |

The host relays everything between visitors. Packets all go over TCP so far: fine on the connections tried (LAN, 5G), and
a lost packet stalls those behind it on a bad one. Direct UDP is the next thing to try if that shows up.

Variables: `NX_LDN=1` forces it on, `NX_LDN_PORT`, `NX_LDN_JOIN=host[:port],...`, `NX_LDN_UPNP=0`. Lines starting with
`ldn:` and `vsock:` in the log show the calls.

Tested on Windows: 6 instances on one PC, a desktop and a laptop on one LAN, and a laptop on a 5G hotspot joining a desktop
for 30 minutes (Livebox 7). Tested between Windows and Linux (Bazzite, NVIDIA), each hosting in turn, through the router's
public address. Not tested yet: macOS, two hosts behind one router at once. Known: a visitor whose player id is
already on the island does not see it (the game's own check, seen with copies of one save); some routers only answer UPnP
once it has been turned off and on again.

The design follows [MHGURecomp](https://github.com/morerokk/MHGURecomp) by morerokk (MIT): `runtime/net/ldn.c` and
`vsock.c`, `docs/STATUS.md` "Local Play over IP", which proved this approach and its `NetworkInfo` layout.

## Structures (from the calls; sizes per the SDK headers' layout as reimplemented elsewhere)

- `NetworkConfig` 0x20: intent id (local communication id u64, scene id u16), channel u16, node count max u8,
  local communication version u16.
- `SecurityConfig` 0x44: security mode u16, passphrase size u16, passphrase (0x40).
- `UserConfig` 0x30: user name (0x21, UTF-8).
- `ScanFilter` 0x60, `NetworkInfo` 0x480 (network id, common info with the SSID, then the LDN part: nodes (8 x 0x40,
  IPv4, MAC, name), advertise data up to 0x180 bytes).
