// Networking at the API level. Offline by default; Nextendo mode reports a network and resolves Nintendo
// hostnames to Nextendo (docs/NEXTENDO.md). Local wireless (ldn) is separate (hle_ldn.cpp).
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <string>

#include <nx/runtime.h>

#include "game_clock.h"
#include "hle_hostnet.h"
#include "nextendo.h"
#include "nfp_host.h"

namespace nx {

namespace {

constexpr uint32_t kNifmNoNetwork = (3400u << 9) | 110u;  // nifm error "network not available"
constexpr uint32_t kGenericFail = (1u << 9) | 110u;
constexpr uint32_t kOk = 0;

bool net_up() { return nextendo::online_services_active(); }

void ok(CPU& c) { c.x[0] = 0; }
void no(CPU& c) { c.x[0] = 0; }
void fail(CPU& c) { c.x[0] = kNifmNoNetwork; }
void result_fail(CPU& c) { c.x[0] = kGenericFail; }
void eai_fail(CPU& c) { c.x[0] = 4; }  // EAI_FAIL
void htonl_(CPU& c) { c.x[0] = bswap(uint32_t(c.x[0])); }
void htons_(CPU& c) { c.x[0] = bswap(uint16_t(c.x[0])); }
uint32_t htonl_u32(uint32_t v) { return bswap(v); }  // the guest and every host here are little-endian

void IsNetworkAvailable(CPU& c) {
    const bool up = net_up();
    static bool last = !up;
    if (up != last) {  // each change, with why: the gate the game sees before anything online
        last = up;
        nx_time_refresh_shared_clocks();  // the network clock follows the online state (svc_time.cpp)
        log("nifm: network %s (Nextendo mode %d, signed in %d, version %s ok %d)", up ? "available" : "unavailable",
            int(nextendo::is_nextendo()), int(nextendo::signed_in()), nextendo::game_version_string().c_str(),
            int(nextendo::version_ok_for_nextendo()));
    }
    c.x[0] = up ? 1 : 0;
}

// nn::account checks these before any network service account call (CheckInternetAvailability): without them the
// SDK's own nifm client, talking to the stub service, says no request was accepted (2124-0059).
void GetClientId(CPU& c) { c.x[0] = 1; }
void IsAnyInternetRequestAccepted(CPU& c) { c.x[0] = net_up() ? 1 : 0; }

// GetCurrentProxySetting(ProxySetting*): no proxy. ProxySetting is {bool enabled, u16 port, char server[0x64],
// bool auth, char user[0x20], char password[0x20]} (0xAA bytes): all zero means disabled. The game's static-service
// login stops at a failure here.
void GetCurrentProxySetting(CPU& c) {
    if (!net_up()) return fail(c);
    if (c.x[0]) std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0xAA);
    c.x[0] = kOk;
}

void HandleNetworkRequestError(CPU& c) {
    if (net_up()) {
        c.x[0] = kOk;
        return;
    }
    fail(c);
}

void show_error(CPU& c) {
    const uint64_t code = c.x[0];
    if (code <= 0xffffffffull && (code & 0x1ff) == 115) {
        log("nn::err::%s(%#llx) nfp error, virtual amiibo %s (lr %s)", hle_current_name(), (unsigned long long)code,
            nfp_host::has_tag() ? "loaded" : "not loaded", describe(c.x[30]).c_str());
        return;
    }
    // ShowError(const ErrorResultVariant&): the argument is its address; log its words to read the Result or code.
    if (std::strstr(hle_current_name(), "ErrorResultVariant") && code > 0xffffffffull) {
        log("nn::err::%s variant %08x %08x %08x %08x (lr %s)", hle_current_name(), ld<uint32_t>(code),
            ld<uint32_t>(code + 4), ld<uint32_t>(code + 8), ld<uint32_t>(code + 12), describe(c.x[30]).c_str());
        return;
    }
    // Online errors (2306-xxxx, 2618-xxxx): the Nextendo wiki's error pages explain them by code.
    log("nn::err::%s(%#llx)%s", hle_current_name(), (unsigned long long)code,
        nextendo::is_nextendo() ? (nextendo::signed_in() ? " [Nextendo signed in]" : " [Nextendo not signed in]") : "");
}

void net_trace(const char* what, const char* arg) {
    static std::atomic<int> n{0};
    if (n++ < 200) log("net: %s(%s) online %d", what, arg ? arg : "null", int(hostnet_active()));
}

void GetAddrInfo_(CPU& c) {
    net_trace(hle_current_name(), reinterpret_cast<const char*>(c.x[0]));
    if (hostnet_active()) return hostnet_GetAddrInfo(c);
    eai_fail(c);
}

void FreeAddrInfo_(CPU& c) {
    if (hostnet_active()) return hostnet_FreeAddrInfo(c);
    ok(c);
}

void GetHostByName_(CPU& c) {
    net_trace(hle_current_name(), reinterpret_cast<const char*>(c.x[0]));
    if (hostnet_active()) return hostnet_GetHostByName(c);
    c.x[0] = 0;
}

// nn::nsd::ResolveEx(Fqdn* out, const Fqdn& in): environment substitution, "%" becomes the retail environment
// "lp1" (api.hac.%.acbaa... -> api.hac.lp1.acbaa...). The DNS redirect happens at resolution.
void ResolveEx_(CPU& c) {
    const char* in = reinterpret_cast<const char*>(c.x[1]);
    char* out = reinterpret_cast<char*>(c.x[0]);
    net_trace("nsd::ResolveEx", in);
    if (!net_up() || !in || !out) return result_fail(c);
    std::string fqdn;
    for (const char* p = in; *p && fqdn.size() < 0xF0; p++) fqdn += *p == '%' ? std::string("lp1") : std::string(1, *p);
    std::snprintf(out, 0x100, "%s", fqdn.c_str());
    c.x[0] = kOk;
}

// nifm::GetCurrentPrimaryIpAddress(in_addr*) / GetCurrentIpConfigInfo(ip, mask, gateway, dns1, dns2): Pia puts the
// local address in its NAT candidates.
void GetCurrentPrimaryIpAddress(CPU& c) {
    const uint32_t ip = net_up() ? hostnet_local_ip() : 0;
    if (!ip) return fail(c);
    if (c.x[0]) st<uint32_t>(c.x[0], ip);
    c.x[0] = kOk;
}

void GetCurrentIpConfigInfo(CPU& c) {
    const uint32_t ip = net_up() ? hostnet_local_ip() : 0;
    if (!ip) return fail(c);
    const uint32_t mask = htonl_u32(0xFFFFFF00u), gw = (ip & mask) | htonl_u32(1);
    const uint64_t outs[5] = {c.x[0], c.x[1], c.x[2], c.x[3], c.x[4]};
    const uint32_t vals[5] = {ip, mask, gw, gw, gw};
    for (int i = 0; i < 5; i++)
        if (outs[i]) st<uint32_t>(outs[i], vals[i]);
    c.x[0] = kOk;
}

}  // namespace

NX_HLE("_ZN2nn4nifm10InitializeEv", ok);
NX_HLE("_ZN2nn4nifm20SubmitNetworkRequestEv", ok);
NX_HLE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", ok);
NX_HLE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", no);
NX_HLE("_ZN2nn4nifm20CancelNetworkRequestEv", ok);
NX_HLE("_ZN2nn4nifm18IsNetworkAvailableEv", IsNetworkAvailable);
NX_HLE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", HandleNetworkRequestError);
NX_HLE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", GetCurrentProxySetting);
NX_HLE("_ZN2nn4nifm19SetLocalNetworkModeEb", ok);
NX_HLE("_ZN2nn4nifm11GetClientIdEv", GetClientId);
NX_HLE("_ZN2nn4nifm30IsAnyForegroundRequestAcceptedEv", IsAnyInternetRequestAccepted);
NX_HLE("_ZN2nn4nifm28IsAnyInternetRequestAcceptedENS0_8ClientIdE", IsAnyInternetRequestAccepted);
NX_HLE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", GetCurrentPrimaryIpAddress);
NX_HLE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", GetCurrentPrimaryIpAddress);
NX_HLE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", GetCurrentIpConfigInfo);
NX_HLE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", GetCurrentIpConfigInfo);
NX_HLE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", ok);
NX_HLE("_ZN2nn6socket10InitializeEPvmmi", ok);
NX_HLE("_ZN2nn6socket8FinalizeEv", ok);
NX_HLE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", GetAddrInfo_);
NX_HLE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", GetAddrInfo_);
NX_HLE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", FreeAddrInfo_);
NX_HLE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", FreeAddrInfo_);
NX_HLE("nnsocketGetHostByName", GetHostByName_);
NX_HLE("nnsocketGetHostByNameWithOptions", GetHostByName_);
NX_HLE("nnsocketResolverSetOption", ok);
NX_HLE("nnsocketResolverGetOption", ok);
NX_HLE("_ZN2nn6socket9InetHtonlEj", htonl_);
NX_HLE("_ZN2nn6socket9InetHtonsEt", htons_);
NX_HLE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", ResolveEx_);
NX_HLE("_ZN2nn3err9ShowErrorENS_6ResultE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS_6ResultERKNS0_15ResultBacktraceE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS_6ResultERKNS0_12ErrorContextE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeERKNS0_12ErrorContextE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS_6ResultERKNS0_16CallerIdentifierE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS_6ResultERKNS0_15ResultBacktraceERKNS0_16CallerIdentifierE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS_6ResultERKNS0_12ErrorContextERKNS0_16CallerIdentifierE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeERKNS0_16CallerIdentifierE", show_error);
NX_HLE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeERKNS0_12ErrorContextERKNS0_16CallerIdentifierE", show_error);
NX_HLE("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", show_error);
NX_HLE("_ZN2nn3err15ShowSystemErrorERKNS0_14SystemErrorArgE", show_error);

}  // namespace nx
