// nn::hid::system NFC (and settings NFC flag): ACNH registers NFC handlers through these, not only nn::nfc.
//
// Env: NX_HID_NFC_TRACE=1 logs pad enumeration / discovery.
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <nx/runtime.h>

#include "nfp_host.h"

namespace nx {

namespace {

using nfp_host::kVirtualNpadId;
constexpr uint32_t kVirtualAbstractedPadId = 0;
// nn::hid Result — invalid argument when npad has no NFC.
constexpr uint32_t kHidInvalidArgument = 124 | (10u << 9);

void ok(CPU& c) { c.x[0] = 0; }

void fail(CPU& c, uint32_t r) { c.x[0] = r; }

bool hid_nfc_trace() {
    static int on = -1;
    if (on < 0) on = std::getenv("NX_HID_NFC_TRACE") && std::getenv("NX_HID_NFC_TRACE")[0] == '1' ? 1 : 0;
    return on != 0;
}

void bind_os_event(CPU& c, int slot) {
    const uint64_t ev = c.x[1];
    nfp_host::bind_system_event(c, ev, slot);
    if (slot == 2) nfp_host::signal_availability();
    ok(c);
}

void SettingsIsNfcEnabled(CPU& c) { c.x[0] = 1; }

void SettingsSetNfcEnabled(CPU& c) { ok(c); }

void GetAbstractedPadId(CPU& c) {
    const uint64_t out = c.x[0];
    const uint32_t npad = uint32_t(c.x[1]);
    if (npad != kVirtualNpadId) {
        if (hid_nfc_trace()) log("hid: GetAbstractedPadIdForNpadWithNfc npad=%u -> invalid", npad);
        fail(c, kHidInvalidArgument);
        return;
    }
    if (out) st<uint32_t>(out, kVirtualAbstractedPadId);
    if (hid_nfc_trace()) log("hid: GetAbstractedPadIdForNpadWithNfc npad=0 -> abstracted %u", kVirtualAbstractedPadId);
    ok(c);
}

// Returns pad count in x0 (not nn::Result). See sdk_0052CDB0.
void GetNpadsWithNfc(CPU& c) {
    nfp_host::ensure_nfc_services_ready(c);
    const uint64_t out = c.x[0];
    const int max = int(c.x[1]);
    int count = 0;
    if (max > 0 && out) {
        st<uint32_t>(out, kVirtualNpadId);
        count = 1;
    }
    c.x[0] = uint64_t(uint32_t(count));
    static std::atomic<bool> once{false};
    if (!once.exchange(true) || hid_nfc_trace()) log("hid: GetNpadsWithNfc max=%d -> count %d", max, count);
}

// detail: int* outCount, unsigned int* outPads, int max — Result in x0.
void DetailGetNpadsWithNfc(CPU& c) {
    const uint64_t count_out = c.x[0];
    const uint64_t pads_out = c.x[1];
    const int max = int(c.x[2]);
    const int count = max > 0 ? 1 : 0;
    if (count_out) st<int32_t>(count_out, count);
    if (count > 0 && pads_out) st<uint32_t>(pads_out, kVirtualNpadId);
    if (hid_nfc_trace()) log("hid: detail GetNpadsWithNfc max=%d -> count %d", max, count);
    ok(c);
}

void StartNfcDiscovery(CPU& c) {
    static std::atomic<bool> once{false};
    if (!once.exchange(true) || hid_nfc_trace()) log("hid: StartNfcDiscovery (virtual NFC)");
    ok(c);
}

void StopNfcDiscovery(CPU& c) {
    nfp_host::on_stop_detection();
    if (hid_nfc_trace()) log("hid: StopNfcDiscovery");
    ok(c);
}

void ActivateNfc(CPU& c) { ok(c); }

void DeactivateNfc(CPU& c) { ok(c); }

void ActivateNfcApplet(CPU& c) { ok(c); }

void DeactivateNfcApplet(CPU& c) { ok(c); }

void DetailActivateNfc(CPU& c) {
    if (c.x[0]) st<uint32_t>(c.x[0], kVirtualNpadId);
    ok(c);
}

void DetailDeactivateNfc(CPU& c) {
    if (c.x[0]) st<uint32_t>(c.x[0], kVirtualNpadId);
    ok(c);
}

void IsNfcActivatedNpad(CPU& c) { c.x[0] = 1; }

void DetailIsNfcActivated(CPU& c) {
    const uint64_t out = c.x[0];
    if (out) st<uint8_t>(out, 1);
    ok(c);
}

void CheckNfcDevicePower(CPU& c) { ok(c); }

void GetNfcInfo(CPU& c) {
    const uint64_t out = c.x[0];
    if (out) {
        std::memset(reinterpret_cast<void*>(out), 0, 0x40);
        if (nfp_host::has_tag()) st<uint8_t>(out, 1);
    }
    ok(c);
}

void SetNfcEvent(CPU& c) { bind_os_event(c, 2); }

void BindNfcActivate(CPU& c) { bind_os_event(c, 0); }

void BindNfcDeviceUpdate(CPU& c) { bind_os_event(c, 2); }

void DetailBindNfcActivate(CPU& c) { bind_os_event(c, 0); }

void DetailBindNfcDeviceUpdate(CPU& c) { bind_os_event(c, 2); }

void SendNfcRawData(CPU& c) { ok(c); }

void GetMcuVersionForNfc(CPU& c) { ok(c); }

}  // namespace

NX_HLE("_ZN2nn8settings6system12IsNfcEnabledEv", SettingsIsNfcEnabled);
NX_HLE("_ZN2nn8settings6system13SetNfcEnabledEb", SettingsSetNfcEnabled);

NX_HLE("_ZN2nn3hid6system32GetAbstractedPadIdForNpadWithNfcEPNS1_15AbstractedPadIdEj", GetAbstractedPadId);
NX_HLE("_ZN2nn3hid6system6detail32GetAbstractedPadIdForNpadWithNfcEPNS1_15AbstractedPadIdEj", GetAbstractedPadId);
NX_HLE("_ZN2nn3hid6system15GetNpadsWithNfcEPji", GetNpadsWithNfc);
NX_HLE("_ZN2nn3hid6system6detail15GetNpadsWithNfcEPiPji", DetailGetNpadsWithNfc);

NX_HLE("_ZN2nn3hid6system17StartNfcDiscoveryERKNS1_21NfcDiscoveryParameterENS1_15AbstractedPadIdE", StartNfcDiscovery);
NX_HLE("_ZN2nn3hid6system6detail17StartNfcDiscoveryERKNS1_21NfcDiscoveryParameterENS1_15AbstractedPadIdE",
       StartNfcDiscovery);
NX_HLE("_ZN2nn3hid6system16StopNfcDiscoveryENS1_15AbstractedPadIdE", StopNfcDiscovery);
NX_HLE("_ZN2nn3hid6system6detail16StopNfcDiscoveryENS1_15AbstractedPadIdE", StopNfcDiscovery);

NX_HLE("_ZN2nn3hid6system11ActivateNfcEj", ActivateNfc);
NX_HLE("_ZN2nn3hid6system11ActivateNfcEjNS_6applet20AppletResourceUserIdE", ActivateNfcApplet);
NX_HLE("_ZN2nn3hid6system6detail11ActivateNfcERjNS_6applet20AppletResourceUserIdE", DetailActivateNfc);
NX_HLE("_ZN2nn3hid6system13DeactivateNfcEj", DeactivateNfc);
NX_HLE("_ZN2nn3hid6system13DeactivateNfcEjNS_6applet20AppletResourceUserIdE", DeactivateNfcApplet);
NX_HLE("_ZN2nn3hid6system6detail13DeactivateNfcERjNS_6applet20AppletResourceUserIdE", DetailDeactivateNfc);

NX_HLE("_ZN2nn3hid6system14IsNfcActivatedEj", IsNfcActivatedNpad);
NX_HLE("_ZN2nn3hid6system6detail14IsNfcActivatedEPbRj", DetailIsNfcActivated);

NX_HLE("_ZN2nn3hid6system19CheckNfcDevicePowerENS1_15AbstractedPadIdE", CheckNfcDevicePower);
NX_HLE("_ZN2nn3hid6system6detail19CheckNfcDevicePowerENS1_15AbstractedPadIdE", CheckNfcDevicePower);

NX_HLE("_ZN2nn3hid6system10GetNfcInfoEPNS1_7NfcInfoENS1_15AbstractedPadIdE", GetNfcInfo);
NX_HLE("_ZN2nn3hid6system6detail10GetNfcInfoEPNS1_7NfcInfoENS1_15AbstractedPadIdE", GetNfcInfo);

NX_HLE("_ZN2nn3hid6system11SetNfcEventEPNS_2os15SystemEventTypeES4_NS1_15AbstractedPadIdE", SetNfcEvent);
NX_HLE("_ZN2nn3hid6system6detail11SetNfcEventEPNS_2os15SystemEventTypeES5_NS1_15AbstractedPadIdE", SetNfcEvent);

NX_HLE("_ZN2nn3hid6system20BindNfcActivateEventEjPNS_2os15SystemEventTypeENS2_14EventClearModeE", BindNfcActivate);
NX_HLE("_ZN2nn3hid6system6detail20BindNfcActivateEventERjPNS_2os15SystemEventTypeENS4_14EventClearModeE",
       DetailBindNfcActivate);
NX_HLE("_ZN2nn3hid6system24BindNfcDeviceUpdateEventEPNS_2os15SystemEventTypeENS2_14EventClearModeE",
       BindNfcDeviceUpdate);
NX_HLE("_ZN2nn3hid6system6detail24BindNfcDeviceUpdateEventEPNS_2os15SystemEventTypeENS3_14EventClearModeE",
       DetailBindNfcDeviceUpdate);

NX_HLE("_ZN2nn3hid6system14SendNfcRawDataERKNS1_20NfcPassThruParameterENS1_15AbstractedPadIdE", SendNfcRawData);
NX_HLE("_ZN2nn3hid6system6detail14SendNfcRawDataERKNS1_20NfcPassThruParameterENS1_15AbstractedPadIdE", SendNfcRawData);
NX_HLE("_ZN2nn3hid6system19GetMcuVersionForNfcEPNS1_20McuVersionDataForNfcENS1_15AbstractedPadIdE", GetMcuVersionForNfc);
NX_HLE("_ZN2nn3hid6system6detail19GetMcuVersionForNfcEPNS1_20McuVersionDataForNfcENS1_15AbstractedPadIdE",
       GetMcuVersionForNfc);

}  // namespace nx
