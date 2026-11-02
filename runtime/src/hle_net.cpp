// Networking at the API level: the console is offline. nifm reports no network, sockets fail, name resolution
// and SSL fail; the game's online features (EOS, news) switch themselves off. Local wireless (ldn) is off, as on a
// console in airplane mode: before, nn::ldn::Initialize went to the ipc stub, got no session object back and read
// through a null pointer (ACNH: Orville's "local play" crashed the game).
#include <nx/runtime.h>

namespace nx {

namespace {

constexpr uint32_t kNifmNoNetwork = (3400u << 9) | 110u;  // nifm error "network not available"
constexpr uint32_t kGenericFail = (1u << 9) | 110u;

void ok(CPU& c) { c.x[0] = 0; }
void no(CPU& c) { c.x[0] = 0; }        // bool false
void fail(CPU& c) { c.x[0] = kNifmNoNetwork; }
void result_fail(CPU& c) { c.x[0] = kGenericFail; }
void eai_fail(CPU& c) { c.x[0] = 4; }   // EAI_FAIL
void htonl_(CPU& c) { c.x[0] = bswap(uint32_t(c.x[0])); }
void htons_(CPU& c) { c.x[0] = bswap(uint16_t(c.x[0])); }
void show_error(CPU& c) {  // the first argument: a Result or an ErrorCode (by value), or a pointer to an argument
    log("nn::err::%s(%#llx)", hle_current_name(), (unsigned long long)c.x[0]);
}

}  // namespace

NX_HLE("_ZN2nn4nifm10InitializeEv", ok);
NX_HLE("_ZN2nn4nifm20SubmitNetworkRequestEv", ok);
NX_HLE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", ok);
NX_HLE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", no);
NX_HLE("_ZN2nn4nifm20CancelNetworkRequestEv", ok);
NX_HLE("_ZN2nn4nifm18IsNetworkAvailableEv", no);
NX_HLE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", fail);
NX_HLE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", fail);
NX_HLE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", ok);
NX_HLE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", eai_fail);
NX_HLE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", ok);
NX_HLE("_ZN2nn6socket9InetHtonlEj", htonl_);
NX_HLE("_ZN2nn6socket9InetHtonsEt", htons_);
NX_HLE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", result_fail);
NX_HLE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", result_fail);
// Every overload: the error viewer is a library applet, which the runtime does not run (creating one read through
// the null object the ipc stub returned: ACNH crashed when Orville's local play failed with the wireless off).
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
