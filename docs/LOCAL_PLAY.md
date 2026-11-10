# Local play over the local network

Goal: the airport's **local play** (visiting another island, receiving visitors, up to 8 players) between computers
running ACNHRecomp, and with a Switch running [ldn_mitm](https://github.com/spacemeowx2/ldn_mitm) (spacemeowx2, GPLv2) on
the same network. The runtime speaks ldn_mitm's LAN protocol, reimplemented from its description (no code taken).
Nintendo's online servers are never contacted.

It works in either network mode, next to Nextendo Online: only `online.enabled=1` turns it on. For Dodo Codes and
Search for a friend on Nextendo Network, see [NEXTENDO.md](NEXTENDO.md).

On the Switch, local play is `nn::ldn` (a wireless network between consoles, no router) carrying the game's own
traffic (Pia) over UDP. The game calls both as APIs the runtime implements by name (`runtime/src/hle_ldn.cpp`,
`runtime/src/hle_vsock.cpp`). As with ldn_mitm, the LDN network becomes the local network: each node's address is its
real one, and the game's UDP goes straight between them.

## How to play

In the launcher or the game's settings (F1), page **Online**, turn local play on (in either network mode). It can be
changed while the game runs: it is read when you next talk to Orville, and held until that session ends.

- **On the same network** (or a virtual LAN that forwards broadcasts: ZeroTier, Hamachi): nothing else. The host opens
  the airport, the others ask Orville to travel by Local Play and the island is listed. A Switch with ldn_mitm on works
  the same, as host or visitor (same game version).
- **A virtual LAN without broadcasts** (Tailscale): the visitor types the host's address under "Friend's address"; the
  Online page shows each computer's own address. Over the internet without a virtual LAN: not possible, as with ldn_mitm.

UDP and TCP port 11452 and the game's UDP port must be allowed by the firewall (Windows asks the first time).

## How it works

ldn_mitm's protocol (`ldn_net.h`): a 12-byte header (magic `0x11451400`, type, compressed flag, length, decompressed
length), types Scan, ScanResp, Connect, SyncNetwork; bodies are a `NetworkInfo` (0x480) or a `NodeInfo` (0x40), and may
come compressed (a zero byte, then how many more zeros). This side sends them uncompressed, which ldn_mitm accepts.

| File | What it does |
|---|---|
| `hle_ldn.cpp` | `nn::ldn`, what the game calls: states, the `NetworkInfo`, the setting held for a session |
| `hle_vsock.cpp` | `nn::socket` on that network: each game socket is a real UDP socket at its port; a broadcast goes to each node |
| `ldn_link.cpp` | UDP 11452 (Scan to the broadcast and the given addresses, ScanResp), the host's TCP 11452, a station's connection |
| `ldn_lan.cpp` | This computer's address, and the test of a friend's address. Also used by the launcher |
| `ldn_net.h` | Sockets and the protocol's packets |

Variables: `NX_LDN=1` forces it on, `NX_LDN_JOIN=host[:port],...` (addresses to scan besides the broadcast),
`NX_LDN_IP=a.b.c.d` (the address to use and bind to: a virtual LAN's, or `127.0.0.2` to run a second copy on one PC with
`NX_LDN_JOIN=127.0.0.1`). Lines starting with `ldn:` and `vsock:` in the log show the calls.

Known: one copy per address, as with ldn_mitm (ports 11452 and the game's are fixed); the subnet mask given to the game is
/24. A visitor whose player id is already on the island does not see it (the game's own check, seen with copies of one
save).

The first version (a relay over TCP, UPnP, play over the internet) followed [MHGURecomp](https://github.com/morerokk/MHGURecomp)
by morerokk (MIT), `runtime/net/ldn.c` and `vsock.c`, which proved the approach and its `NetworkInfo` layout.

## Structures (from the calls; sizes per the SDK headers' layout as reimplemented elsewhere)

- `NetworkConfig` 0x20: intent id (local communication id u64, scene id u16), channel u16, node count max u8,
  local communication version u16.
- `SecurityConfig` 0x44: security mode u16, passphrase size u16, passphrase (0x40).
- `UserConfig` 0x30: user name (0x21, UTF-8).
- `ScanFilter` 0x60, `NetworkInfo` 0x480 (network id, common info with the SSID, then the LDN part: nodes (8 x 0x40,
  IPv4, MAC, name), advertise data up to 0x180 bytes).
