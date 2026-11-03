// Virtual NFC / loaded Amiibo state shared by the nn::nfp HLE and the Amiibo importer UI.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace nx {
struct CPU;
struct EventState;
}

namespace nx::nfp_host {

void bind_system_event(nx::CPU& c, uint64_t guest_system_event, int event_slot);
void signal_availability();
void sync_activate_event_after_attach();

// The virtual reader is player 1's controller (npad No1). A DeviceHandle is its npad id, as on yuzu / Ryujinx:
// games match GetNpadId against the npad that asked for the scan (ACNH: DeviceNotFound 0x8073 when none does).
constexpr uint32_t kVirtualNpadId = 0;
constexpr uint64_t kVirtualDeviceHandle = kVirtualNpadId;

constexpr uint32_t kApplicationAreaSize = 0xD8;
constexpr size_t kNfpDataSize = 0x298;
constexpr size_t kRegisterInfoSize = 0x100;
constexpr size_t kRegisterInfoPrivateSize = 0x120;

// Loads a 540-byte (or 532/520) Amiibo .bin dump. Requires retail keys at
// Keys: Settings → Amiibo, or <data_root>/amiibo/key_retail.bin (160 bytes).
bool load_tag_file(const std::string& path, std::string* err = nullptr);
void clear_tag();

bool has_tag();
const std::string& tag_path();

// nn::nfp::DeviceState values used by the HLE.
enum class DeviceState : uint32_t {
    Initialized = 0,
    SearchingForTag = 1,
    TagFound = 2,
    TagRemoved = 3,
    TagMounted = 4,
    Unavailable = 5,
};

DeviceState device_state();
// Creates the SDK nfp/nfc user clients once (the SDK expects them before its own calls).
void ensure_nfc_services_ready(::nx::CPU& c);

void on_initialize();
void on_finalize();
void on_start_detection();
void on_stop_detection();
void on_mount();
void on_unmount();
bool on_open_application_area(uint32_t access_id);

void fill_tag_info(void* out_tag_info_0x58);
void fill_model_info(void* out_model_info_0x40);
void fill_register_info(void* out_register_info_0x100);
void fill_common_info(void* out_common_info_0x40);
void fill_admin_info(void* out_admin_info_0x20);
void fill_nfp_data(void* out_nfp_data_0x298);
void fill_register_info_private(void* out_register_info_private_0x120);
bool tag_has_application_area();
bool read_application_area(void* out, uint32_t size);
void write_application_area(const void* data, uint32_t size);

void signal_activate_if_pending();  // after loading a tag while the game is waiting to scan

// event_slot: 0=activate, 1=deactivate, 2=availability. IPC services (nfp:user / nfc:user) share these slots.
void set_event_slot(int event_slot, std::shared_ptr<EventState> state);

// Presents a tag loaded mid-scan. Call once per frame from the platform loop.
void poll();

}  // namespace nx::nfp_host
