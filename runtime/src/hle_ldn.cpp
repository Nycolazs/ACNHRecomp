// nn::ldn, local wireless play (the airport's "local play": visiting other islands nearby).
//
// Off by default, as on a console in airplane mode: Initialize answers ResultDeviceDisabled and the game says local
// play is unavailable (before, it reached the ipc stub, got no session object back and read through a null pointer).
//
// online.enabled or NX_LDN=1: local play between computers (docs/LOCAL_PLAY.md): the API answers as a console with
// wireless on, its network is the link of ldn_link.cpp, and every call is logged with what the game passed
// (configurations as hex).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <mutex>
#include <string>

#include <nx/runtime.h>

#include "config.h"
#include "hle_ldn.h"
#include "kernel.h"
#include "ui/ui.h"

namespace nx {

namespace {

constexpr uint32_t kOk = 0;
constexpr uint32_t kLdnDeviceDisabled = (22u << 9) | 203u;  // ldn ResultDeviceDisabled (wireless off)
constexpr uint32_t kLdnInvalidState = (22u << 9) | 32u;     // ldn ResultInvalidState

// nn::ldn::State
enum State : uint32_t { None = 0, Initialized = 1, AccessPoint = 2, AccessPointCreated = 3, Station = 4, StationConnected = 5, Error = 6 };
const char* state_name(uint32_t s) {
    static const char* n[] = {"None", "Initialized", "AccessPoint", "AccessPointCreated", "Station", "StationConnected", "Error"};
    return s < 7 ? n[s] : "?";
}

// Read once, when the game starts local play, and held until it ends: turning the setting off in the middle of a visit
// would cut the game's sockets and leave the port open at the end.
std::atomic<bool> g_session{false};

bool wanted() {
    static const bool forced = [] {
        const char* e = std::getenv("NX_LDN");
        return e && e[0] == '1';
    }();
    return forced || config_get("online.enabled") == "1";
}

bool enabled() { return g_session; }

std::mutex g_lock;
uint32_t g_state = None;
std::shared_ptr<EventState> g_state_event;  // AttachStateChangeEvent's: signaled at every change of state
uint8_t g_advertise[0x180] = {};
uint64_t g_advertise_size = 0;

// NetworkInfo (0x480): IntentId, then the common part, the LDN part with the nodes and the advertise data.
constexpr size_t kInfoSize = 0x480;
enum { kNiSessionId = 0x10, kNiBssid = 0x20, kNiSsid = 0x26, kNiChannel = 0x48, kNiLinkLevel = 0x4A, kNiNetworkType = 0x4B,
       kNiSecParam = 0x50, kNiSecMode = 0x60, kNiAcceptPolicy = 0x62, kNiNodeMax = 0x66, kNiNodeCount = 0x67,
       kNiNodes = 0x68, kNiAdvSize = 0x26A, kNiAdv = 0x26C, kNiAuthId = 0x478 };
enum { kNodeIp = 0, kNodeMac = 4, kNodeId = 0xA, kNodeConnected = 0xB, kNodeName = 0xC, kNodeVersion = 0x2E };
uint8_t g_info[kInfoSize] = {};
uint8_t g_mac[6] = {};
uint8_t g_accept_policy = 0;
std::atomic<uint32_t> g_own_ip{0};  // 169.254.x.1 while hosting

void random_bytes(uint8_t* p, size_t n) {
    static std::mt19937_64 gen{std::random_device{}()};
    for (size_t i = 0; i < n; i++) p[i] = uint8_t(gen());
}

void put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

void ensure_mac() {
    if (g_mac[0] || g_mac[1]) return;
    random_bytes(g_mac, 6);
    g_mac[0] = (g_mac[0] | 2) & 0xFE;  // locally administered, unicast
}

uint32_t g_disconnect_reason = 0;  // nn::ldn::DisconnectReason: 6 = signal lost

// The network this console hosts, from what CreateNetwork was given. sec_param: CreateNetworkPrivate's.
void make_network(const uint8_t* cfg, const uint8_t* sec, const uint8_t* sec_param, const uint8_t* user) {
    ensure_mac();
    std::memset(g_info, 0, kInfoSize);
    std::memcpy(g_info, cfg, 0x10);  // IntentId
    if (sec_param) {
        std::memcpy(g_info + kNiSecParam, sec_param, 0x10);
        std::memcpy(g_info + kNiSessionId, sec_param + 0x10, 0x10);
    } else {
        random_bytes(g_info + kNiSecParam, 0x10);
        random_bytes(g_info + kNiSessionId, 0x10);
    }
    std::memcpy(g_info + kNiBssid, g_mac, 6);
    uint8_t r[8];
    random_bytes(r, 8);
    char ssid[33];
    std::snprintf(ssid, sizeof ssid, "ACNHRecomp%02x%02x%02x%02x", r[0], r[1], r[2], r[3]);
    g_info[kNiSsid] = uint8_t(std::strlen(ssid));
    std::memcpy(g_info + kNiSsid + 1, ssid, std::strlen(ssid));
    uint16_t channel;
    std::memcpy(&channel, cfg + 0x10, 2);
    put16(g_info + kNiChannel, channel ? channel : 6);
    g_info[kNiLinkLevel] = 3;
    g_info[kNiNetworkType] = 2;  // ldn
    std::memcpy(g_info + kNiSecMode, sec, 2);
    g_info[kNiAcceptPolicy] = g_accept_policy;
    const uint8_t max = cfg[0x12];
    g_info[kNiNodeMax] = max >= 1 && max <= 8 ? max : 8;
    g_info[kNiNodeCount] = 1;
    const uint32_t ip = 0xA9FE0001u | uint32_t(1 + r[4] % 254) << 8;
    g_own_ip = ip;
    uint8_t* node = g_info + kNiNodes;
    put32(node + kNodeIp, ip);
    std::memcpy(node + kNodeMac, g_mac, 6);
    node[kNodeId] = 0;
    node[kNodeConnected] = 1;
    std::memcpy(node + kNodeName, user, 0x20);
    std::memcpy(node + kNodeVersion, cfg + 0x14, 2);
    put16(g_info + kNiAdvSize, uint16_t(g_advertise_size));
    std::memcpy(g_info + kNiAdv, g_advertise, size_t(g_advertise_size));
    std::memcpy(g_info + kNiAuthId, r, 8);
}

std::string hex(const void* p, size_t n) {
    std::string s;
    if (!p) return "(null)";
    const auto* b = static_cast<const uint8_t*>(p);
    char t[4];
    for (size_t i = 0; i < n; i++) {
        std::snprintf(t, sizeof t, "%02x", b[i]);
        s += t;
        if (i % 4 == 3 && i + 1 < n) s += ' ';
    }
    return s;
}

void signal_change() {
    if (g_state_event) {
        g_state_event->signaled = true;
        notify_all();
    }
}

void set_state(uint32_t s) {
    if (g_state == s) return;
    log("ldn: state %s -> %s", state_name(g_state), state_name(s));
    g_state = s;
    signal_change();
}

void disabled(CPU& c) {
    if (!wanted()) return void(c.x[0] = kLdnDeviceDisabled);
    c.x[0] = kOk;
}

void Initialize(CPU& c) {
    if (!wanted()) {  // the game only says nothing and goes back to Orville: show where the setting is
        ui::ui_open_online();
        return void(c.x[0] = kLdnDeviceDisabled);
    }
    g_session = true;
    std::lock_guard lk(g_lock);
    log("ldn: Initialize (%s)", hle_current_name());
    if (g_state == None) set_state(Initialized);
    c.x[0] = kOk;
}

void Finalize(CPU& c) {
    if (enabled()) {  // whatever the setting says by now: the link and the port end with the session
        link_stop();
        vsock_reset();
    }
    std::lock_guard lk(g_lock);
    if (enabled()) log("ldn: Finalize");
    g_session = false;
    g_own_ip = 0;
    g_state = None;
    c.x[0] = kOk;
}

void GetState(CPU& c) {
    std::lock_guard lk(g_lock);
    c.x[0] = enabled() ? g_state : None;
}

// AttachStateChangeEvent(os::SystemEventType*): a system event of ours, signaled at each change of state.
void AttachStateChangeEvent(CPU& c) {
    const uint64_t out = c.x[0];
    auto e = std::make_shared<EventState>();
    auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
    r->state = w->state = e;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    static const uint64_t attach =
        find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[0] = out, c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // auto clear
    if (attach) call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    std::lock_guard lk(g_lock);
    g_state_event = e;
    if (enabled()) log("ldn: AttachStateChangeEvent(%#llx)", (unsigned long long)out);
}

void OpenAccessPoint(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: OpenAccessPoint (state %s)", state_name(g_state));
    if (g_state != Initialized) return void(c.x[0] = kLdnInvalidState);
    set_state(AccessPoint);
    c.x[0] = kOk;
}

void CloseAccessPoint(CPU& c) {
    link_stop();
    std::lock_guard lk(g_lock);
    log("ldn: CloseAccessPoint (state %s)", state_name(g_state));
    g_own_ip = 0;
    set_state(Initialized);
    c.x[0] = kOk;
}

void OpenStation(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: OpenStation (state %s)", state_name(g_state));
    if (g_state != Initialized) return void(c.x[0] = kLdnInvalidState);
    set_state(Station);
    c.x[0] = kOk;
}

void CloseStation(CPU& c) {
    link_stop();
    std::lock_guard lk(g_lock);
    log("ldn: CloseStation (state %s)", state_name(g_state));
    g_own_ip = 0;
    set_state(Initialized);
    c.x[0] = kOk;
}

// CreateNetwork(const NetworkConfig&, const SecurityConfig&, const UserConfig&)
void CreateNetwork(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: CreateNetwork network config %s", hex(reinterpret_cast<void*>(c.x[0]), 0x20).c_str());
    log("ldn:   security config %s", hex(reinterpret_cast<void*>(c.x[1]), 0x44).c_str());
    log("ldn:   user config %s", hex(reinterpret_cast<void*>(c.x[2]), 0x30).c_str());
    if (g_state != AccessPoint) return void(c.x[0] = kLdnInvalidState);
    make_network(reinterpret_cast<const uint8_t*>(c.x[0]), reinterpret_cast<const uint8_t*>(c.x[1]), nullptr,
                 reinterpret_cast<const uint8_t*>(c.x[2]));
    link_host_start();
    set_state(AccessPointCreated);
    c.x[0] = kOk;
}

// CreateNetworkPrivate(network, security, SecurityParameter, user, int count, const AddressEntry*)
void CreateNetworkPrivate(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: CreateNetworkPrivate network %s count %d", hex(reinterpret_cast<void*>(c.x[0]), 0x20).c_str(), int(c.x[4]));
    if (g_state != AccessPoint) return void(c.x[0] = kLdnInvalidState);
    make_network(reinterpret_cast<const uint8_t*>(c.x[0]), reinterpret_cast<const uint8_t*>(c.x[1]),
                 reinterpret_cast<const uint8_t*>(c.x[2]), reinterpret_cast<const uint8_t*>(c.x[3]));
    link_host_start();
    set_state(AccessPointCreated);
    c.x[0] = kOk;
}

void DestroyNetwork(CPU& c) {
    link_stop();
    std::lock_guard lk(g_lock);
    log("ldn: DestroyNetwork (state %s)", state_name(g_state));
    g_own_ip = 0;
    if (g_state == AccessPointCreated) set_state(AccessPoint);
    c.x[0] = kOk;
}

// SetAdvertiseData(const void*, size_t): what the host shows to stations that scan (the island, the player).
void SetAdvertiseData(CPU& c) {
    std::lock_guard lk(g_lock);
    const uint64_t n = c.x[1];
    log("ldn: SetAdvertiseData %llu bytes: %s", (unsigned long long)n,
        hex(reinterpret_cast<void*>(c.x[0]), size_t(std::min<uint64_t>(n, 0x180))).c_str());
    if (n > sizeof g_advertise) return void(c.x[0] = kLdnInvalidState);
    if (n && c.x[0]) std::memcpy(g_advertise, reinterpret_cast<void*>(c.x[0]), n);
    g_advertise_size = n;
    if (g_state == AccessPointCreated) {
        put16(g_info + kNiAdvSize, uint16_t(n));
        std::memset(g_info + kNiAdv, 0, sizeof g_advertise);
        std::memcpy(g_info + kNiAdv, g_advertise, size_t(n));
    }
    c.x[0] = kOk;
}

void SetStationAcceptPolicy(CPU& c) {
    log("ldn: SetStationAcceptPolicy(%u)", uint32_t(c.x[0]));
    std::lock_guard lk(g_lock);
    g_accept_policy = uint8_t(c.x[0]);
    g_info[kNiAcceptPolicy] = g_accept_policy;
    c.x[0] = kOk;
}

// Scan(NetworkInfo* out, int* count, int buffer count, const ScanFilter&, int channel): the hosts ldn_link.cpp finds.
void Scan(CPU& c) {
    log("ldn: %s(out %#llx, count %#llx, max %d, filter %s, channel %d)", hle_current_name(), (unsigned long long)c.x[0],
        (unsigned long long)c.x[1], int(c.x[2]), hex(reinterpret_cast<void*>(c.x[3]), 0x60).c_str(), int(c.x[4]));
    const int found = c.x[0] && c.x[3] ? link_scan(reinterpret_cast<uint8_t*>(c.x[0]), int(c.x[2]),
                                                   reinterpret_cast<const uint8_t*>(c.x[3]), 0)
                                       : 0;
    if (found) log("ldn: Scan found %d", found);
    if (c.x[1]) *reinterpret_cast<int32_t*>(c.x[1]) = found;
    c.x[0] = kOk;
}

// Connect(const NetworkInfo&, const SecurityConfig&, const UserConfig&, int version, ConnectOption)
void Connect(CPU& c) {
    log("ldn: %s(network %s ..., version %d, option %u)", hle_current_name(),
        hex(reinterpret_cast<void*>(c.x[0]), 0x40).c_str(), int(c.x[3]), uint32_t(c.x[4]));
    std::lock_guard lk(g_lock);
    if (g_state != Station) return void(c.x[0] = kLdnInvalidState);
    ensure_mac();
    const auto* net = reinterpret_cast<const uint8_t*>(c.x[0]);
    const auto* user = reinterpret_cast<const uint8_t*>(c.x[2]);
    const uint16_t version = uint16_t(c.x[3]);
    uint8_t info[kInfoSize];
    const int node = link_join(net + kNiBssid, user, g_mac, reinterpret_cast<const uint8_t*>(&version), info);
    if (node < 0) {
        log("ldn: could not join");
        return void(c.x[0] = kLdnInvalidState);
    }
    std::memcpy(g_info, info, kInfoSize);
    uint32_t ip;
    std::memcpy(&ip, g_info + kNiNodes + size_t(node) * 0x40 + kNodeIp, 4);
    g_own_ip = ip;
    g_disconnect_reason = 0;
    log("ldn: joined as node %d, %#x", node, ip);
    set_state(StationConnected);
    c.x[0] = kOk;
}

void Disconnect(CPU& c) {
    link_stop();
    std::lock_guard lk(g_lock);
    log("ldn: Disconnect (state %s)", state_name(g_state));
    g_own_ip = 0;
    if (g_state == StationConnected) set_state(Station);
    c.x[0] = kOk;
}

// GetNetworkInfo(NetworkInfo*): the network this console made or joined (0x480 bytes).
void GetNetworkInfo(CPU& c) {
    log("ldn: GetNetworkInfo(%#llx) in state %s", (unsigned long long)c.x[0], state_name(g_state));
    std::lock_guard lk(g_lock);
    const bool up = g_state == AccessPointCreated || g_state == StationConnected;
    if (c.x[0]) {
        if (up)
            std::memcpy(reinterpret_cast<void*>(c.x[0]), g_info, kInfoSize);
        else
            std::memset(reinterpret_cast<void*>(c.x[0]), 0, kInfoSize);
    }
    c.x[0] = up ? kOk : kLdnInvalidState;
}

void GetNetworkConfig(CPU& c) {
    log("ldn: GetNetworkConfig(%#llx)", (unsigned long long)c.x[0]);
    std::lock_guard lk(g_lock);
    const bool up = g_state == AccessPointCreated || g_state == StationConnected;
    if (c.x[0]) {
        auto* o = reinterpret_cast<uint8_t*>(c.x[0]);
        std::memset(o, 0, 0x20);
        if (up) {
            std::memcpy(o, g_info, 0x10);
            std::memcpy(o + 0x10, g_info + kNiChannel, 2);
            o[0x12] = g_info[kNiNodeMax];
            std::memcpy(o + 0x14, g_info + kNiNodes + kNodeVersion, 2);
        }
    }
    c.x[0] = up ? kOk : kLdnInvalidState;
}

void GetSecurityParameter(CPU& c) {
    log("ldn: GetSecurityParameter(%#llx)", (unsigned long long)c.x[0]);
    std::lock_guard lk(g_lock);
    const bool up = g_state == AccessPointCreated || g_state == StationConnected;
    if (c.x[0]) {
        auto* o = reinterpret_cast<uint8_t*>(c.x[0]);
        std::memset(o, 0, 0x20);
        if (up) {
            std::memcpy(o, g_info + kNiSecParam, 0x10);
            std::memcpy(o + 0x10, g_info + kNiSessionId, 0x10);
        }
    }
    c.x[0] = up ? kOk : kLdnInvalidState;
}

// GetIpv4Address(Ipv4Address*, SubnetMask*): this console's on the network, /24.
void GetIpv4Address(CPU& c) {
    log("ldn: GetIpv4Address in state %s", state_name(g_state));
    if (c.x[0]) *reinterpret_cast<uint32_t*>(c.x[0]) = g_own_ip;
    if (c.x[1]) *reinterpret_cast<uint32_t*>(c.x[1]) = 0xFFFFFF00;
    c.x[0] = kOk;
}

// MakeBroadcastAddress(Ipv4Address, SubnetMask) -> Ipv4Address
void MakeBroadcastAddress(CPU& c) {
    const uint32_t ip = uint32_t(c.x[0]), mask = uint32_t(c.x[1]);
    c.x[0] = (ip & mask) | ~mask;
}

void GetDisconnectReason(CPU& c) {
    log("ldn: GetDisconnectReason");
    c.x[0] = g_disconnect_reason;
}

void Reject(CPU& c) {
    log("ldn: Reject(%#x)", uint32_t(c.x[0]));
    c.x[0] = kOk;
}

}  // namespace

bool ldn_enabled() { return g_session; }

bool ldn_net_up() { return g_own_ip != 0; }
uint32_t ldn_own_ip() { return g_own_ip; }
uint32_t ldn_broadcast_ip() { return g_own_ip | 0xFFu; }

bool ldn_info_valid(uint8_t* info) {
    uint16_t adv;
    std::memcpy(&adv, info + kNiAdvSize, 2);
    const uint8_t max = info[kNiNodeMax], count = info[kNiNodeCount];
    if (info[kNiSsid] > 32 || max < 1 || max > 8 || count < 1 || count > max || adv > sizeof g_advertise) return false;
    info[kNiSsid + 0x21] = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t* node = info + kNiNodes + size_t(i) * 0x40;
        if (!node[kNodeConnected]) {
            std::memset(node, 0, 0x40);
            continue;
        }
        node[kNodeConnected] = 1;
        node[kNodeId] = uint8_t(i);
        node[kNodeName + 0x20] = 0;
    }
    return true;
}

bool ldn_link_info(uint8_t* out) {
    std::lock_guard lk(g_lock);
    if (g_state != AccessPointCreated) return false;
    std::memcpy(out, g_info, kInfoSize);
    return true;
}

int ldn_link_add_node(const uint8_t* name, const uint8_t* mac, const uint8_t* version) {
    std::lock_guard lk(g_lock);
    if (g_state != AccessPointCreated) return -1;
    const int max = g_info[kNiNodeMax];
    for (int i = 1; i < max && i < 8; i++) {
        uint8_t* node = g_info + kNiNodes + size_t(i) * 0x40;
        if (node[kNodeConnected]) continue;
        put32(node + kNodeIp, (g_own_ip & 0xFFFFFF00u) | uint32_t(i + 1));
        std::memcpy(node + kNodeMac, mac, 6);
        node[kNodeId] = uint8_t(i);
        node[kNodeConnected] = 1;
        std::memcpy(node + kNodeName, name, 0x20);
        node[kNodeName + 0x20] = 0;
        std::memcpy(node + kNodeVersion, version, 2);
        g_info[kNiNodeCount]++;
        signal_change();
        return i;
    }
    return -1;
}

void ldn_link_remove_node(int n) {
    std::lock_guard lk(g_lock);
    uint8_t* node = g_info + kNiNodes + size_t(n) * 0x40;
    if (!node[kNodeConnected]) return;
    std::memset(node, 0, 0x40);
    g_info[kNiNodeCount]--;
    signal_change();
}

uint32_t ldn_node_ip(int n) { return (g_own_ip & 0xFFFFFF00u) | uint32_t(n + 1); }

void ldn_link_set_info(const uint8_t* info) {
    std::lock_guard lk(g_lock);
    if (g_state != StationConnected) return;
    std::memcpy(g_info, info, kInfoSize);
    signal_change();
}

void ldn_link_lost() {
    std::lock_guard lk(g_lock);
    if (g_state != StationConnected) return;
    g_own_ip = 0;
    g_disconnect_reason = 6;
    set_state(Error);
}

NX_HLE("_ZN2nn3ldn10InitializeEv", Initialize);
NX_HLE("_ZN2nn3ldn16InitializeSystemEv", Initialize);
NX_HLE("_ZN2nn3ldn16InitializeSystemEi", Initialize);
NX_HLE("_ZN2nn3ldn17InitializeMonitorEv", disabled);
NX_HLE("_ZN2nn3ldn8FinalizeEv", Finalize);
NX_HLE("_ZN2nn3ldn14FinalizeSystemEv", Finalize);
NX_HLE("_ZN2nn3ldn15FinalizeMonitorEv", Finalize);
NX_HLE("_ZN2nn3ldn8GetStateEv", GetState);
NX_HLE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", AttachStateChangeEvent);
NX_HLE("_ZN2nn3ldn15OpenAccessPointEv", OpenAccessPoint);
NX_HLE("_ZN2nn3ldn16CloseAccessPointEv", CloseAccessPoint);
NX_HLE("_ZN2nn3ldn11OpenStationEv", OpenStation);
NX_HLE("_ZN2nn3ldn12CloseStationEv", CloseStation);
NX_HLE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", CreateNetwork);
NX_HLE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_"
       "10UserConfigEiPKNS0_12AddressEntryE",
       CreateNetworkPrivate);
NX_HLE("_ZN2nn3ldn14DestroyNetworkEv", DestroyNetwork);
NX_HLE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", SetAdvertiseData);
NX_HLE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", SetStationAcceptPolicy);
NX_HLE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", Scan);
NX_HLE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", Scan);
NX_HLE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", Connect);
NX_HLE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_"
       "10UserConfigEiNS0_13ConnectOptionE",
       Connect);
NX_HLE("_ZN2nn3ldn10DisconnectEv", Disconnect);
NX_HLE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", GetNetworkInfo);
NX_HLE("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", GetNetworkConfig);
NX_HLE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", GetSecurityParameter);
NX_HLE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", GetIpv4Address);
NX_HLE("_ZN2nn3ldn20MakeBroadcastAddressENS0_11Ipv4AddressENS0_10SubnetMaskE", MakeBroadcastAddress);
NX_HLE("_ZN2nn3ldn19GetDisconnectReasonEv", GetDisconnectReason);
NX_HLE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", Reject);

}  // namespace nx
