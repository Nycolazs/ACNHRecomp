// nfp:user and nfc:user — SDK nn::nfp / nn::nfc talk to the system through these, not main-module HLE.
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "ipc.h"
#include "kernel.h"
#include "nfp_host.h"

namespace nx {

namespace {

constexpr uint32_t kOk = 0;
constexpr uint32_t kDeviceNotFound = 115 | (64u << 9);
constexpr uint32_t kWrongDeviceState = 115 | (73u << 9);
constexpr uint32_t kWrongArgument = 115 | (10u << 9);
constexpr uint32_t kAppAreaMissing = 115 | (128u << 9);
constexpr uint64_t kDeviceHandle = nfp_host::kVirtualDeviceHandle;

bool g_nfp_ipc_init = false;
bool g_nfc_ipc_init = false;

bool nfp_ipc_log() {
    static int on = -1;
    if (on < 0) on = std::getenv("NX_NFP_TRACE") && std::getenv("NX_NFP_TRACE")[0] == '1' ? 1 : 0;
    return on != 0;
}

bool handle_ok(uint64_t h) { return h == kDeviceHandle || uint32_t(h) == uint32_t(kDeviceHandle); }

// Commands before Initialize act on an implicitly initialized device (as the nn::nfp HLE).
void ensure_nfp_ipc_init() {
    if (g_nfp_ipc_init) return;
    g_nfp_ipc_init = true;
    g_nfc_ipc_init = true;
    nfp_host::on_initialize();
}

uint32_t write_device_list(IpcCtx& c) {
    Buffer out = c.out_buf(0);
    if (!out.addr || out.size < 8) return kWrongArgument;
    std::memset(reinterpret_cast<void*>(out.addr), 0, out.size);
    st<uint64_t>(out.addr, kDeviceHandle);
    c.push<uint32_t>(1);
    return kOk;
}

uint32_t push_event(IpcCtx& c, int slot) {
    auto state = std::make_shared<EventState>();
    auto ev = std::make_shared<KEvent>();
    ev->state = state;
    nfp_host::set_event_slot(slot, state);
    c.out_copy.push_back(handle_add(ev));
    if (slot == 0) nfp_host::sync_activate_event_after_attach();
    if (slot == 2) nfp_host::signal_availability();
    return kOk;
}

uint32_t device_state_result() {
    if (!g_nfp_ipc_init) return uint32_t(nfp_host::DeviceState::Unavailable);
    return uint32_t(nfp_host::device_state());
}

std::shared_ptr<Service> make_nfp_iuser();

void mount_nfp_iuser(Service& s) {
    s.cmds[0] = [](IpcCtx& c) {  // Initialize
        ensure_nfp_ipc_init();
        if (nfp_ipc_log()) log("nfp-ipc: Initialize");
        return kOk;
    };
    s.cmds[1] = [](IpcCtx& c) {  // Finalize
        g_nfp_ipc_init = false;
        nfp_host::on_finalize();
        if (nfp_ipc_log()) log("nfp-ipc: Finalize");
        return kOk;
    };
    s.cmds[2] = [](IpcCtx& c) {  // ListDevices
        ensure_nfp_ipc_init();
        const uint32_t r = write_device_list(c);
        if (nfp_ipc_log()) log("nfp-ipc: ListDevices");
        return r;
    };
    s.cmds[3] = [](IpcCtx& c) {  // StartDetection(device, protocol)
        ensure_nfp_ipc_init();
        const uint64_t dev = c.arg<uint64_t>(0);
        if (!handle_ok(dev)) {
            return kDeviceNotFound;
        }
        nfp_host::on_start_detection();
        if (nfp_ipc_log()) log("nfp-ipc: StartDetection -> state %u", unsigned(nfp_host::device_state()));
        return kOk;
    };
    s.cmds[4] = [](IpcCtx& c) {  // StopDetection
        ensure_nfp_ipc_init();
        const uint64_t dev = c.arg<uint64_t>(0);
        if (!handle_ok(dev)) return kDeviceNotFound;
        nfp_host::on_stop_detection();
        return kOk;
    };
    s.cmds[5] = [](IpcCtx& c) {  // Mount
        ensure_nfp_ipc_init();
        const uint64_t dev = c.arg<uint64_t>(0);
        const uint32_t model = c.arg<uint32_t>(8);
        const uint32_t mount_target = c.arg<uint32_t>(12);
        if (!handle_ok(dev)) return kDeviceNotFound;
        if (model != 0) return kWrongArgument;
        if ((mount_target & 3u) == 0) return kWrongArgument;
        if (!nfp_host::has_tag()) {
            return kWrongDeviceState;
        }
        if (nfp_host::device_state() != nfp_host::DeviceState::TagFound) {
            return kWrongDeviceState;
        }
        nfp_host::on_mount();
        if (nfp_ipc_log()) log("nfp-ipc: Mount");
        return kOk;
    };
    s.cmds[6] = [](IpcCtx& c) {  // Unmount
        ensure_nfp_ipc_init();
        const uint64_t dev = c.arg<uint64_t>(0);
        if (!handle_ok(dev)) return kDeviceNotFound;
        if (nfp_host::device_state() != nfp_host::DeviceState::TagMounted) return kWrongDeviceState;
        nfp_host::on_unmount();
        return kOk;
    };
    s.cmds[7] = [](IpcCtx& c) {  // OpenApplicationArea
        ensure_nfp_ipc_init();
        const uint64_t dev = c.arg<uint64_t>(0);
        const uint32_t access = c.arg<uint32_t>(8);
        if (!handle_ok(dev)) return kDeviceNotFound;
        if (nfp_host::device_state() != nfp_host::DeviceState::TagMounted) return kWrongDeviceState;
        if (!nfp_host::on_open_application_area(access)) {
            return kAppAreaMissing;
        }
        return kOk;
    };
    s.cmds[8] = [](IpcCtx& c) {  // GetApplicationArea
        ensure_nfp_ipc_init();
        const uint64_t dev = c.arg<uint64_t>(0);
        if (!handle_ok(dev)) return kDeviceNotFound;
        Buffer out = c.out_buf(0);
        if (!out.addr) return kWrongDeviceState;
        const uint32_t size = uint32_t(std::min<uint64_t>(out.size, nfp_host::kApplicationAreaSize));
        if (!nfp_host::read_application_area(reinterpret_cast<void*>(out.addr), size)) return kWrongDeviceState;
        c.push<uint32_t>(size);
        return kOk;
    };
    s.cmds[9] = [](IpcCtx&) { return kOk; };  // SetApplicationArea
    s.cmds[10] = [](IpcCtx&) { return kOk; }; // Flush
    s.cmds[11] = [](IpcCtx&) { return kOk; }; // Restore
    s.cmds[12] = [](IpcCtx&) { return kAppAreaMissing; };  // CreateApplicationArea
    s.cmds[13] = [](IpcCtx& c) {  // GetTagInfo
        ensure_nfp_ipc_init();
        const uint64_t dev = c.arg<uint64_t>(0);
        if (!handle_ok(dev)) return kDeviceNotFound;
        auto st = nfp_host::device_state();
        if (st != nfp_host::DeviceState::TagFound && st != nfp_host::DeviceState::TagMounted) {
            return kWrongDeviceState;
        }
        Buffer out = c.out_buf(0);
        if (out.addr) nfp_host::fill_tag_info(reinterpret_cast<void*>(out.addr));
        return kOk;
    };
    s.cmds[14] = [](IpcCtx& c) {  // GetRegisterInfo
        ensure_nfp_ipc_init();
        if (!handle_ok(c.arg<uint64_t>(0))) return kDeviceNotFound;
        if (nfp_host::device_state() != nfp_host::DeviceState::TagMounted) return kWrongDeviceState;
        Buffer out = c.out_buf(0);
        if (out.addr) nfp_host::fill_register_info(reinterpret_cast<void*>(out.addr));
        return kOk;
    };
    s.cmds[15] = [](IpcCtx& c) {  // GetCommonInfo
        ensure_nfp_ipc_init();
        if (!handle_ok(c.arg<uint64_t>(0))) return kDeviceNotFound;
        if (nfp_host::device_state() != nfp_host::DeviceState::TagMounted) return kWrongDeviceState;
        Buffer out = c.out_buf(0);
        if (out.addr) nfp_host::fill_common_info(reinterpret_cast<void*>(out.addr));
        return kOk;
    };
    s.cmds[16] = [](IpcCtx& c) {  // GetModelInfo
        ensure_nfp_ipc_init();
        if (!handle_ok(c.arg<uint64_t>(0))) return kDeviceNotFound;
        if (nfp_host::device_state() != nfp_host::DeviceState::TagMounted) return kWrongDeviceState;
        Buffer out = c.out_buf(0);
        if (out.addr) nfp_host::fill_model_info(reinterpret_cast<void*>(out.addr));
        if (nfp_ipc_log()) log("nfp-ipc: GetModelInfo");
        return kOk;
    };
    s.cmds[17] = [](IpcCtx& c) {  // AttachActivateEvent
        ensure_nfp_ipc_init();
        if (!handle_ok(c.arg<uint64_t>(0))) return kDeviceNotFound;
        return push_event(c, 0);
    };
    s.cmds[18] = [](IpcCtx& c) {  // AttachDeactivateEvent
        ensure_nfp_ipc_init();
        if (!handle_ok(c.arg<uint64_t>(0))) return kDeviceNotFound;
        return push_event(c, 1);
    };
    s.cmds[19] = [](IpcCtx& c) {  // GetState
        c.push<uint32_t>(g_nfp_ipc_init ? 1u : 0u);
        return kOk;
    };
    s.cmds[20] = [](IpcCtx& c) {  // GetDeviceState
        ensure_nfp_ipc_init();
        const uint64_t dev = c.arg<uint64_t>(0);
        if (!handle_ok(dev)) {
            c.push<uint32_t>(uint32_t(nfp_host::DeviceState::Unavailable));
            return kDeviceNotFound;
        }
        c.push<uint32_t>(device_state_result());
        return kOk;
    };
    s.cmds[21] = [](IpcCtx& c) {  // GetNpadId
        ensure_nfp_ipc_init();
        if (!handle_ok(c.arg<uint64_t>(0))) return kDeviceNotFound;
        c.push<uint32_t>(nfp_host::kVirtualNpadId);
        return kOk;
    };
    s.cmds[22] = [](IpcCtx& c) {  // GetApplicationAreaSize
        c.push<uint32_t>(nfp_host::kApplicationAreaSize);
        return kOk;
    };
    s.cmds[23] = [](IpcCtx& c) { return push_event(c, 2); };  // AttachAvailabilityChangeEvent
}

std::shared_ptr<Service> make_nfp_iuser() {
    auto s = std::make_shared<Service>("nfp:IUser");
    mount_nfp_iuser(*s);
    return s;
}

void mount_nfc_iuser(Service& s) {
    s.cmds[400] = [](IpcCtx& c) {  // Initialize
        g_nfc_ipc_init = true;
        g_nfp_ipc_init = true;
        nfp_host::on_initialize();
        if (nfp_ipc_log()) log("nfp-ipc: nfc Initialize");
        return kOk;
    };
    s.cmds[401] = [](IpcCtx& c) {  // Finalize
        g_nfc_ipc_init = false;
        return kOk;
    };
    s.cmds[402] = [](IpcCtx& c) {  // GetState
        c.push<uint32_t>(g_nfc_ipc_init ? 1u : 0u);
        return kOk;
    };
    s.cmds[403] = [](IpcCtx& c) {  // IsNfcEnabled
        c.push<uint8_t>(1);
        return kOk;
    };
    s.cmds[404] = [](IpcCtx& c) {  // ListDevices
        ensure_nfp_ipc_init();
        const uint32_t r = write_device_list(c);
        return r;
    };
    s.cmds[405] = [](IpcCtx& c) {  // GetDeviceState
        ensure_nfp_ipc_init();
        if (!handle_ok(c.arg<uint64_t>(0))) return kDeviceNotFound;
        c.push<uint32_t>(device_state_result());
        return kOk;
    };
    s.cmds[406] = [](IpcCtx& c) {  // GetNpadId
        if (!g_nfc_ipc_init) return kDeviceNotFound;
        if (!handle_ok(c.arg<uint64_t>(0))) return kDeviceNotFound;
        c.push<uint32_t>(nfp_host::kVirtualNpadId);
        return kOk;
    };
    s.cmds[407] = [](IpcCtx& c) { return push_event(c, 2); };  // GetAvailabilityChangeEventHandle
    s.cmds[408] = [](IpcCtx& c) {  // StartDetection
        ensure_nfp_ipc_init();
        if (!handle_ok(c.arg<uint64_t>(0))) return kDeviceNotFound;
        nfp_host::on_start_detection();
        if (nfp_ipc_log()) log("nfp-ipc: nfc StartDetection");
        return kOk;
    };
    s.cmds[409] = [](IpcCtx& c) {  // StopDetection
        if (!g_nfc_ipc_init) return kDeviceNotFound;
        nfp_host::on_stop_detection();
        return kOk;
    };
    s.cmds[410] = [](IpcCtx& c) {  // GetTagInfo
        if (!g_nfc_ipc_init) return kDeviceNotFound;
        if (!handle_ok(c.arg<uint64_t>(0))) return kDeviceNotFound;
        auto st = nfp_host::device_state();
        if (st != nfp_host::DeviceState::TagFound && st != nfp_host::DeviceState::TagMounted)
            return kWrongDeviceState;
        Buffer out = c.out_buf(0);
        if (out.addr) nfp_host::fill_tag_info(reinterpret_cast<void*>(out.addr));
        return kOk;
    };
    s.cmds[411] = [](IpcCtx& c) { return push_event(c, 0); };  // GetActivateEventHandle
    s.cmds[412] = [](IpcCtx& c) { return push_event(c, 1); };  // GetDeactivateEventHandle
}

std::shared_ptr<Service> make_nfc_iuser() {
    auto s = std::make_shared<Service>("nfc:IUser");
    mount_nfc_iuser(*s);
    return s;
}

std::shared_ptr<Service> make_nfp_user_manager() {
    auto s = std::make_shared<Service>("nfp:user");
    s->cmds[0] = [](IpcCtx& c) {
        log("nfp:user CreateUserInterface");
        ensure_nfp_ipc_init();
        c.push_object(make_nfp_iuser());
        return kOk;
    };
    return s;
}

std::shared_ptr<Service> make_nfc_user_manager() {
    auto s = std::make_shared<Service>("nfc:user");
    s->cmds[0] = [](IpcCtx& c) {
        log("nfc:user CreateUserInterface");
        ensure_nfp_ipc_init();
        c.push_object(make_nfc_iuser());
        return kOk;
    };
    return s;
}

}  // namespace

ServiceRegistrar r_nfp_user("nfp:user", [] { return make_nfp_user_manager(); });
ServiceRegistrar r_nfc_user("nfc:user", [] { return make_nfc_user_manager(); });

}  // namespace nx
