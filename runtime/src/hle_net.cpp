// Networking at the API level. Offline by default; Nextendo mode reports a network and resolves Nintendo
// hostnames to Nextendo (docs/NEXTENDO.md). Local wireless (ldn) is separate (hle_ldn.cpp).
#include <cstdio>
#include <string>

#include <nx/runtime.h>

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
void yes(CPU& c) { c.x[0] = 1; }
void fail(CPU& c) { c.x[0] = kNifmNoNetwork; }
void result_fail(CPU& c) { c.x[0] = kGenericFail; }
void eai_fail(CPU& c) { c.x[0] = 4; }  // EAI_FAIL
void htonl_(CPU& c) { c.x[0] = bswap(uint32_t(c.x[0])); }
void htons_(CPU& c) { c.x[0] = bswap(uint16_t(c.x[0])); }

void IsNetworkAvailable(CPU& c) { c.x[0] = net_up() ? 1 : 0; }

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
    // Nextendo: ask account status for a clearer toast when online fails.
    if (nextendo::is_nextendo() && nextendo::signed_in()) {
        std::string msg;
        if (!nextendo::fetch_online_status(msg) && !msg.empty())
            log("nn::err::%s(%#llx) nextendo: %s", hle_current_name(), (unsigned long long)code, msg.c_str());
        else
            log("nn::err::%s(%#llx)", hle_current_name(), (unsigned long long)code);
        return;
    }
    log("nn::err::%s(%#llx)", hle_current_name(), (unsigned long long)code);
}

void GetAddrInfo_(CPU& c) {
    if (hostnet_active()) return hostnet_GetAddrInfo(c);
    eai_fail(c);
}

void FreeAddrInfo_(CPU& c) {
    if (hostnet_active()) return hostnet_FreeAddrInfo(c);
    ok(c);
}

void ResolveEx_(CPU& c) {
    // nn::nsd::ResolveEx(Fqdn* out, const Fqdn& in): copy with Nextendo redirect applied to the hostname text.
    if (!net_up()) {
        result_fail(c);
        return;
    }
    const char* in = reinterpret_cast<const char*>(c.x[1]);
    char* out = reinterpret_cast<char*>(c.x[0]);
    if (!in || !out) {
        result_fail(c);
        return;
    }
    std::string host(in);
    std::string ip;
    if (nextendo::redirect_host(host, ip)) {
        // Keep the FQDN string the game expects; DNS redirect happens in GetAddrInfo.
        std::snprintf(out, 0x100, "%s", host.c_str());
        c.x[0] = kOk;
        return;
    }
    std::snprintf(out, 0x100, "%s", host.c_str());
    c.x[0] = kOk;
}

// ---- SSL: create succeeds in Nextendo mode; handshake/read/write are stubbed until sockets are associated ----
// Full Schannel/OpenSSL bridging is phased; Create must not fail or the game never reaches NEX.
void SslContextCreate(CPU& c) {
    if (!net_up()) {
        result_fail(c);
        return;
    }
    // this in x0; version in w1 — leave object as-is, report success
    c.x[0] = kOk;
}

void SslOk(CPU& c) {
    if (!net_up()) {
        result_fail(c);
        return;
    }
    c.x[0] = kOk;
}

void SslInitialize(CPU& c) { ok(c); }

}  // namespace

NX_HLE("_ZN2nn4nifm10InitializeEv", ok);
NX_HLE("_ZN2nn4nifm20SubmitNetworkRequestEv", ok);
NX_HLE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", ok);
NX_HLE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", no);
NX_HLE("_ZN2nn4nifm20CancelNetworkRequestEv", ok);
NX_HLE("_ZN2nn4nifm18IsNetworkAvailableEv", IsNetworkAvailable);
NX_HLE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", HandleNetworkRequestError);
NX_HLE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", fail);
NX_HLE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", ok);
NX_HLE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", GetAddrInfo_);
NX_HLE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", FreeAddrInfo_);
NX_HLE("_ZN2nn6socket9InetHtonlEj", htonl_);
NX_HLE("_ZN2nn6socket9InetHtonsEt", htons_);
NX_HLE("_ZN2nn3ssl10InitializeEv", SslInitialize);
NX_HLE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", SslContextCreate);
NX_HLE("_ZN2nn3ssl7Context7DestroyEv", SslOk);
NX_HLE("_ZN2nn3ssl7Context15ImportServerPkiENS1_12CertificateFormatEPKhj", SslOk);
NX_HLE("_ZN2nn3ssl7Context15ImportClientPkiEPKhjS3_j", SslOk);
NX_HLE("_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", SslOk);
NX_HLE("_ZN2nn3ssl10Connection7DestroyEv", SslOk);
NX_HLE("_ZN2nn3ssl10Connection19SetSocketDescriptorEi", SslOk);
NX_HLE("_ZN2nn3ssl10Connection18SetSocketDescriptorEib", SslOk);
NX_HLE("_ZN2nn3ssl10Connection11SetHostNameEPKc", SslOk);
NX_HLE("_ZN2nn3ssl10Connection11DoHandshakeEv", SslOk);
NX_HLE("_ZN2nn3ssl10Connection4ReadEPvmPm", SslOk);
NX_HLE("_ZN2nn3ssl10Connection5WriteEPKvmPm", SslOk);
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
