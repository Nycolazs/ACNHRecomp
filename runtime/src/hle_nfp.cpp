// nn::nfp (Amiibo) HLE: virtual NFC on player 1. Load a .bin in Settings; when the game scans, present the tag.
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <nx/runtime.h>

#include "nfp_host.h"

namespace nx {

namespace {

constexpr uint32_t kNfcDeviceNotFound = 115 | (64u << 9);
constexpr uint32_t kWrongDeviceState = 115 | (73u << 9);
constexpr uint32_t kWrongArgument = 115 | (10u << 9);
constexpr uint32_t kAppAreaMissing = 115 | (128u << 9);

bool nfp_trace() {
    static int on = -1;
    if (on < 0) on = std::getenv("NX_NFP_TRACE") && std::getenv("NX_NFP_TRACE")[0] == '1' ? 1 : 0;
    return on != 0;
}

void ok(CPU& c) { c.x[0] = 0; }
void fail(CPU& c, uint32_t code) { c.x[0] = code; }

bool g_initialized = false;

// Calls before Initialize act on an implicitly initialized device.
void ensure_nfp_init() {
    if (!g_initialized) {
        g_initialized = true;
        nfp_host::on_initialize();
    }
}

bool need_mounted(CPU& c) {
    if (nfp_host::device_state() == nfp_host::DeviceState::TagMounted) return true;
    fail(c, kWrongDeviceState);
    return false;
}

bool need_found_or_mounted(CPU& c) {
    auto s = nfp_host::device_state();
    if (s == nfp_host::DeviceState::TagFound || s == nfp_host::DeviceState::TagMounted) return true;
    fail(c, kWrongDeviceState);
    return false;
}

void Initialize(CPU& c) {
    nfp_host::ensure_nfc_services_ready(c);
    g_initialized = true;
    nfp_host::on_initialize();
    if (nfp_trace()) log("nfp: Initialize");
    ok(c);
}

void Finalize(CPU& c) {
    g_initialized = false;
    nfp_host::on_finalize();
    ok(c);
}

void InitializeSystem(CPU& c) {
    g_initialized = true;
    nfp_host::on_initialize();
    ok(c);
}

void FinalizeSystem(CPU& c) { ok(c); }

void GetState(CPU& c) { c.x[0] = g_initialized ? 1 : 0; }

void ListDevices(CPU& c) {
    nfp_host::ensure_nfc_services_ready(c);
    ensure_nfp_init();
    uint64_t out = c.x[0];
    int max = int(c.x[2]);
    if (c.x[1]) st<int32_t>(c.x[1], max > 0 ? 1 : 0);
    if (out && max > 0) st<uint64_t>(out, nfp_host::kVirtualDeviceHandle);
    static std::atomic<bool> once{false};
    if (!once.exchange(true) || nfp_trace())
        log("nfp: ListDevices -> 1 device (npad %u, tag=%d)", nfp_host::kVirtualNpadId, nfp_host::has_tag() ? 1 : 0);
    ok(c);
}

// SDK returns DeviceState enum in x0 (not nn::Result); 5 = Unavailable when service is down.
void GetDeviceState(CPU& c) {
    if (!g_initialized && nfp_host::has_tag()) ensure_nfp_init();
    if (!g_initialized) {
        c.x[0] = uint32_t(nfp_host::DeviceState::Unavailable);
        if (nfp_trace()) log("nfp: GetDeviceState -> Unavailable (not initialized)");
        return;
    }
    const auto st = nfp_host::device_state();
    c.x[0] = uint32_t(st);
    static std::atomic<uint32_t> last{~0u};
    if (last.exchange(uint32_t(st)) != uint32_t(st) && nfp_trace()) log("nfp: GetDeviceState -> %u", unsigned(st));
}

void GetDeviceStateOut(CPU& c) {
    uint64_t out = c.x[0];
    if (out) st<uint32_t>(out, uint32_t(nfp_host::device_state()));
    ok(c);
}

void AttachAvailabilityChangeEvent(CPU& c) {
    const uint64_t ev = c.x[0];
    nfp_host::bind_system_event(c, ev, 2);
    nfp_host::signal_availability();
    if (nfp_trace()) log("nfp: AttachAvailabilityChangeEvent (signaled)");
    ok(c);
}

void AttachActivateEvent(CPU& c) {
    const uint64_t ev = c.x[0];
    nfp_host::bind_system_event(c, ev, 0);
    nfp_host::sync_activate_event_after_attach();
    if (nfp_trace()) log("nfp: AttachActivateEvent");
    ok(c);
}

void AttachDeactivateEvent(CPU& c) {
    const uint64_t ev = c.x[0];
    nfp_host::bind_system_event(c, ev, 1);
    ok(c);
}

// GetNpadId(u32* out, const DeviceHandle&)
void GetNpadId(CPU& c) {
    ensure_nfp_init();
    const uint64_t out = c.x[0];
    const uint64_t handle = c.x[1] ? ld<uint64_t>(c.x[1]) : ~0ull;
    if (handle != nfp_host::kVirtualDeviceHandle) {
        if (nfp_trace()) log("nfp: GetNpadId handle %#llx -> DeviceNotFound", (unsigned long long)handle);
        return fail(c, kNfcDeviceNotFound);
    }
    if (out) st<uint32_t>(out, nfp_host::kVirtualNpadId);
    if (nfp_trace()) log("nfp: GetNpadId -> npad %u", nfp_host::kVirtualNpadId);
    ok(c);
}

void StartDetection(CPU& c) {
    ensure_nfp_init();
    nfp_host::on_start_detection();
    if (nfp_trace())
        log("nfp: StartDetection -> state %u (tag=%d)", unsigned(nfp_host::device_state()),
            nfp_host::has_tag() ? 1 : 0);
    ok(c);
}

void StopDetection(CPU& c) {
    nfp_host::on_stop_detection();
    if (nfp_trace()) log("nfp: StopDetection -> state %u", unsigned(nfp_host::device_state()));
    ok(c);
}

void GetTagInfo(CPU& c) {
    ensure_nfp_init();
    if (!need_found_or_mounted(c)) return;
    if (!nfp_host::has_tag()) {
        log("nfp: GetTagInfo but no virtual tag loaded (Settings -> Amiibo)");
        return fail(c, kWrongDeviceState);
    }
    uint64_t out = c.x[0];
    if (out) nfp_host::fill_tag_info(reinterpret_cast<void*>(out));
    if (nfp_trace()) log("nfp: GetTagInfo ok");
    ok(c);
}

bool mount_from_tag_found(CPU& c, uint32_t model_type) {
    if (model_type != 0) {
        fail(c, kWrongArgument);
        return false;
    }
    if (!nfp_host::has_tag()) {
        log("nfp: Mount but no virtual tag loaded");
        fail(c, kWrongDeviceState);
        return false;
    }
    auto st = nfp_host::device_state();
    if (st != nfp_host::DeviceState::TagFound) {
        log("nfp: Mount rejected (state %u)", unsigned(st));
        fail(c, kWrongDeviceState);
        return false;
    }
    nfp_host::on_mount();
    if (nfp_trace()) log("nfp: Mount -> state %u", unsigned(nfp_host::device_state()));
    ok(c);
    return true;
}

void MountFull(CPU& c) {
    ensure_nfp_init();
    const uint32_t mount_target = uint32_t(c.x[2]);
    if ((mount_target & 3u) == 0) return fail(c, kWrongArgument);
    mount_from_tag_found(c, uint32_t(c.x[1]));
}

void MountShort(CPU& c) {
    ensure_nfp_init();
    mount_from_tag_found(c, uint32_t(c.x[1]));
}

void Unmount(CPU& c) {
    if (!need_mounted(c)) return;
    nfp_host::on_unmount();
    ok(c);
}

void GetModelInfo(CPU& c) {
    if (!need_mounted(c)) {
        log("nfp: GetModelInfo rejected (state %u)", unsigned(nfp_host::device_state()));
        return;
    }
    uint64_t out = c.x[0];
    if (out) nfp_host::fill_model_info(reinterpret_cast<void*>(out));
    log("nfp: amiibo read (%s)", nfp_host::tag_path().c_str());
    ok(c);
}

void GetRegisterInfo(CPU& c) {
    if (!need_mounted(c)) return;
    uint64_t out = c.x[0];
    if (out) nfp_host::fill_register_info(reinterpret_cast<void*>(out));
    ok(c);
}

void GetRegisterInfoPrivate(CPU& c) {
    if (!need_mounted(c)) return;
    uint64_t out = c.x[0];
    if (out) nfp_host::fill_register_info_private(reinterpret_cast<void*>(out));
    ok(c);
}

void GetAll(CPU& c) {
    if (!need_mounted(c)) return;
    uint64_t out = c.x[0];
    if (out) nfp_host::fill_nfp_data(reinterpret_cast<void*>(out));
    if (nfp_trace()) log("nfp: GetAll");
    ok(c);
}

void SetAll(CPU& c) {
    if (!need_mounted(c)) return;
    ok(c);
}

void GetCommonInfo(CPU& c) {
    if (!need_mounted(c)) return;
    uint64_t out = c.x[0];
    if (out) nfp_host::fill_common_info(reinterpret_cast<void*>(out));
    ok(c);
}

void GetAdminInfo(CPU& c) {
    if (!need_mounted(c)) return;
    uint64_t out = c.x[0];
    if (out) nfp_host::fill_admin_info(reinterpret_cast<void*>(out));
    ok(c);
}

void ExistsApplicationArea(CPU& c) {
    ensure_nfp_init();
    uint64_t out = c.x[0];
    bool exists = nfp_host::tag_has_application_area() &&
                  (nfp_host::device_state() == nfp_host::DeviceState::TagFound ||
                   nfp_host::device_state() == nfp_host::DeviceState::TagMounted);
    if (out) st<uint8_t>(out, exists ? 1 : 0);
    if (nfp_trace()) log("nfp: ExistsApplicationArea -> %d", exists ? 1 : 0);
    ok(c);
}

void OpenApplicationArea(CPU& c) {
    if (!need_mounted(c)) return;
    uint32_t access = uint32_t(c.x[1]);
    if (!nfp_host::on_open_application_area(access)) {
        log("nfp: OpenApplicationArea failed (access_id=0x%08X)", access);
        return fail(c, kAppAreaMissing);
    }
    if (nfp_trace()) log("nfp: OpenApplicationArea 0x%08X", access);
    ok(c);
}

void GetApplicationArea(CPU& c) {
    if (!need_mounted(c)) return;
    uint64_t out = c.x[0];
    uint32_t size = uint32_t(c.x[1]);
    if (!out || !nfp_host::read_application_area(reinterpret_cast<void*>(out), size)) return fail(c, kWrongDeviceState);
    ok(c);
}

// GetApplicationArea(void* out, size_t* out_size, DeviceHandle const&, size_t size)
void GetApplicationAreaOutSize(CPU& c) {
    if (!need_mounted(c)) return;
    uint64_t out = c.x[0];
    uint64_t out_size = c.x[1];
    uint32_t size = uint32_t(c.x[3]);
    if (!out) return fail(c, kWrongDeviceState);
    if (!nfp_host::read_application_area(reinterpret_cast<void*>(out), size)) return fail(c, kWrongDeviceState);
    if (out_size) st<uint64_t>(out_size, size);
    ok(c);
}

void SetApplicationArea(CPU& c) {
    if (!need_mounted(c)) return;
    nfp_host::write_application_area(reinterpret_cast<const void*>(c.x[0]), uint32_t(c.x[1]));
    ok(c);
}

// SetApplicationArea(DeviceHandle, void const*, size_t size)
void SetApplicationAreaSize(CPU& c) {
    if (!need_mounted(c)) return;
    nfp_host::write_application_area(reinterpret_cast<const void*>(c.x[1]), uint32_t(c.x[2]));
    ok(c);
}

void Flush(CPU& c) {
    if (!need_mounted(c)) return;
    ok(c);
}

void Restore(CPU& c) {
    if (!need_mounted(c)) return;
    ok(c);
}

void GetApplicationAreaSize(CPU& c) {
    uint64_t out = c.x[0];
    if (out) st<uint32_t>(out, nfp_host::kApplicationAreaSize);
    ok(c);
}

// detail::CheckError(const Result&) maps a service result to the public one; the HLE results already are.
void DetailCheckError(CPU& c) { c.x[0] = c.x[0] ? ld<uint32_t>(c.x[0]) : 0; }

}  // namespace

NX_HLE("_ZN2nn3nfp10InitializeEv", Initialize);
NX_HLE("_ZN2nn3nfp8FinalizeEv", Finalize);
NX_HLE("_ZN2nn3nfp16InitializeSystemEv", InitializeSystem);
NX_HLE("_ZN2nn3nfp14FinalizeSystemEv", FinalizeSystem);
NX_HLE("_ZN2nn3nfp8GetStateEv", GetState);
NX_HLE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", ListDevices);
NX_HLE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", GetDeviceState);
NX_HLE("_ZN2nn3nfp14GetDeviceStateEPNS0_11DeviceStateERKNS0_12DeviceHandleE", GetDeviceStateOut);
NX_HLE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", AttachAvailabilityChangeEvent);
NX_HLE("_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", AttachActivateEvent);
NX_HLE("_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", AttachDeactivateEvent);
NX_HLE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", GetNpadId);
NX_HLE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", StartDetection);
NX_HLE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", StopDetection);
NX_HLE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", GetTagInfo);
NX_HLE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", MountFull);
NX_HLE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeE", MountShort);
NX_HLE("_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE", Unmount);
NX_HLE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", GetModelInfo);
NX_HLE("_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE", GetRegisterInfo);
NX_HLE("_ZN2nn3nfp15GetRegisterInfoEPNS0_19RegisterInfoPrivateERKNS0_12DeviceHandleE", GetRegisterInfoPrivate);
NX_HLE("_ZN2nn3nfp6GetAllEPNS0_7NfpDataERKNS0_12DeviceHandleE", GetAll);
NX_HLE("_ZN2nn3nfp6SetAllERKNS0_12DeviceHandleERKNS0_7NfpDataE", SetAll);
NX_HLE("_ZN2nn3nfp13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE", GetCommonInfo);
NX_HLE("_ZN2nn3nfp12GetAdminInfoEPNS0_9AdminInfoERKNS0_12DeviceHandleE", GetAdminInfo);
NX_HLE("_ZN2nn3nfp21ExistsApplicationAreaEPbRKNS0_12DeviceHandleE", ExistsApplicationArea);
NX_HLE("_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", OpenApplicationArea);
NX_HLE("_ZN2nn3nfp18GetApplicationAreaEPvjRKNS0_12DeviceHandleE", GetApplicationArea);
NX_HLE("_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", GetApplicationAreaOutSize);
NX_HLE("_ZN2nn3nfp18SetApplicationAreaEPKvjRKNS0_12DeviceHandleE", SetApplicationArea);
NX_HLE("_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm", SetApplicationAreaSize);
NX_HLE("_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE", Flush);
NX_HLE("_ZN2nn3nfp7RestoreERKNS0_12DeviceHandleE", Restore);
NX_HLE("_ZN2nn3nfp21GetApplicationAreaSizeEPjRKNS0_12DeviceHandleE", GetApplicationAreaSize);

// nn::nfc — same virtual device; ACNH calls these through SDK vtables (not main PLT).
void NfcIsNfcEnabled(CPU& c) { c.x[0] = 1; }

void NfcStartDetectionProtocol(CPU& c) { StartDetection(c); }

void NfcStartDetectionShort(CPU& c) { StartDetection(c); }

NX_HLE("_ZN2nn3nfc10InitializeEv", Initialize);
NX_HLE("_ZN2nn3nfc8FinalizeEv", Finalize);
NX_HLE("_ZN2nn3nfc16InitializeSystemEv", InitializeSystem);
NX_HLE("_ZN2nn3nfc14FinalizeSystemEv", FinalizeSystem);
NX_HLE("_ZN2nn3nfc8GetStateEv", GetState);
NX_HLE("_ZN2nn3nfc11ListDevicesEPNS0_12DeviceHandleEPii", ListDevices);
NX_HLE("_ZN2nn3nfc14GetDeviceStateERKNS0_12DeviceHandleE", GetDeviceState);
NX_HLE("_ZN2nn3nfc29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", AttachAvailabilityChangeEvent);
NX_HLE("_ZN2nn3nfc19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", AttachActivateEvent);
NX_HLE("_ZN2nn3nfc21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", AttachDeactivateEvent);
NX_HLE("_ZN2nn3nfc9GetNpadIdEPjRKNS0_12DeviceHandleE", GetNpadId);
NX_HLE("_ZN2nn3nfc14StartDetectionERKNS0_12DeviceHandleENS0_11NfcProtocolE", NfcStartDetectionProtocol);
NX_HLE("_ZN2nn3nfc14StartDetectionERKNS0_12DeviceHandleE", NfcStartDetectionShort);
NX_HLE("_ZN2nn3nfc13StopDetectionERKNS0_12DeviceHandleE", StopDetection);
NX_HLE("_ZN2nn3nfc10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", GetTagInfo);
NX_HLE("_ZN2nn3nfc12IsNfcEnabledEv", NfcIsNfcEnabled);

NX_HLE("_ZN2nn3nfp6detail10CheckErrorERKNS_6ResultE", DetailCheckError);
NX_HLE("_ZN2nn3nfc6detail10CheckErrorERKNS_6ResultE", DetailCheckError);

}  // namespace nx
