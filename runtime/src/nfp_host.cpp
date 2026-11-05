#include "nfp_host.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#include <nx/runtime.h>

#include "kernel.h"
#include "kernel_state.h"
#include "platform.h"

namespace nx::nfp_host {

using nx::call;
using nx::EventState;
using nx::find_symbol;
using nx::handle_add;
using nx::KEvent;
using nx::kstate::g_k;
using nx::notify_all;

namespace {

std::mutex g_lock;
DeviceState g_state = DeviceState::Initialized;
amiibo::DecodedTag g_decoded{};
std::string g_path;
bool g_app_area_open = false;
uint32_t g_open_access_id = 0;

std::shared_ptr<EventState> g_activate_state;
std::shared_ptr<EventState> g_deactivate_state;
std::shared_ptr<EventState> g_availability_state;

bool g_tag_present_pending = false;

void signal_state(const std::shared_ptr<EventState>& state) {
    if (!state) return;
    std::lock_guard lk(g_k);
    state->signaled = true;
    notify_all();
}

bool nfp_trace_enabled() {
    static int on = -1;
    if (on < 0) on = std::getenv("NX_NFP_TRACE") && std::getenv("NX_NFP_TRACE")[0] == '1' ? 1 : 0;
    return on != 0;
}

void log_state(const char* reason) {
    if (!nfp_trace_enabled()) return;
    log("nfp: state %u (%s) tag=%d", unsigned(g_state), reason, amiibo::decoded_tag_valid(g_decoded) ? 1 : 0);
}

bool tag_data_ready_locked() { return amiibo::decoded_tag_valid(g_decoded); }

void promote_tag_found_locked() {
    if (g_state != DeviceState::SearchingForTag || !tag_data_ready_locked()) return;
    g_state = DeviceState::TagFound;
    g_tag_present_pending = false;
    log_state("tag found");
    signal_state(g_activate_state);
    signal_state(g_availability_state);
}

// A tag loaded while the game is already searching is presented on the next poll().
void begin_tag_present_wait_locked() {
    g_tag_present_pending = tag_data_ready_locked() && g_state == DeviceState::SearchingForTag;
}

void write_be16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

void write_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

// nn::util::Date in guest memory: u16 year (native LE), u8 month, u8 day; plain stores year as BE u16.
void write_guest_date(uint8_t* dest, const uint8_t* src) {
    const uint16_t year = uint16_t(uint16_t(src[0]) << 8) | src[1];
    st<uint16_t>(reinterpret_cast<uint64_t>(dest), year);
    dest[2] = src[2];
    dest[3] = src[3];
}

}  // namespace

void ensure_nfc_services_ready(nx::CPU& c) {
    static std::once_flag clients_once;
    std::call_once(clients_once, [&] {
        const uint64_t saved[8] = {c.x[0], c.x[1], c.x[2], c.x[3], c.x[4], c.x[5], c.x[6], c.x[7]};
        for (const char* sym : {"_ZN2nn3nfp6client19CreateUserInterfaceEv",
                                "_ZN2nn3nfc6client19CreateUserInterfaceEv"}) {
            const uint64_t fn = find_symbol(sym);
            if (!fn) continue;
            call(c, fn);
            if (nfp_trace_enabled()) log("nfp: %s (guest x0=%llx)", sym, (unsigned long long)c.x[0]);
        }
        for (int i = 0; i < 8; i++) c.x[i] = saved[i];
    });
}

bool load_tag_file(const std::string& path, std::string* err) {
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) {
        if (err) *err = "Could not open file.";
        return false;
    }
    std::array<uint8_t, amiibo::kTagSize> buf{};
    size_t n = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (n < amiibo::kMinTagSize) {
        if (err) *err = "Not a valid Amiibo dump size.";
        return false;
    }
    amiibo::DecodedTag decoded;
    std::string why;
    if (!amiibo::decode_tag_dump(buf.data(), n, decoded, &why)) {
        if (err) *err = why;
        return false;
    }
    std::lock_guard lk(g_lock);
    g_decoded = decoded;
    g_path = path;
    g_app_area_open = false;
    begin_tag_present_wait_locked();
    log("nfp: loaded amiibo %s (char 0x%04X app_id 0x%08X app_init=%d)", path.c_str(), g_decoded.model.character_id,
        g_decoded.application_area_id, g_decoded.app_area_initialized ? 1 : 0);
    return true;
}

void clear_tag() {
    std::lock_guard lk(g_lock);
    const bool was_present =
        g_state == DeviceState::TagFound || g_state == DeviceState::TagMounted;
    g_decoded = {};
    g_path.clear();
    g_app_area_open = false;
    g_tag_present_pending = false;
    if (was_present) {
        g_state = DeviceState::SearchingForTag;
        log_state("tag cleared while scanning");
        signal_state(g_deactivate_state);
    }
}

bool has_tag() {
    std::lock_guard lk(g_lock);
    return amiibo::decoded_tag_valid(g_decoded);
}

const std::string& tag_path() {
    static thread_local std::string p;
    std::lock_guard lk(g_lock);
    p = g_path;
    return p;
}

DeviceState device_state() {
    std::lock_guard lk(g_lock);
    return g_state;
}

void on_initialize() {
    std::lock_guard lk(g_lock);
    if (g_state == DeviceState::Unavailable) g_state = DeviceState::Initialized;
}

void on_finalize() {
    std::lock_guard lk(g_lock);
    g_state = DeviceState::Initialized;
    g_app_area_open = false;
    g_tag_present_pending = false;
}

void on_start_detection() {
    std::lock_guard lk(g_lock);
    g_state = DeviceState::SearchingForTag;
    g_app_area_open = false;
    log_state("start detection");
    promote_tag_found_locked();
}

// As on the console (yuzu NfcDevice::StopDetection): a mounted tag is unmounted, the device goes back to Initialized.
void on_stop_detection() {
    std::lock_guard lk(g_lock);
    g_tag_present_pending = false;
    g_state = DeviceState::Initialized;
    g_app_area_open = false;
}

void on_mount() {
    std::lock_guard lk(g_lock);
    if (g_state == DeviceState::TagFound && tag_data_ready_locked()) {
        g_state = DeviceState::TagMounted;
        g_app_area_open = false;
        log_state("mounted");
    }
}

void on_unmount() {
    std::lock_guard lk(g_lock);
    if (g_state == DeviceState::TagMounted) g_state = DeviceState::TagFound;
    g_app_area_open = false;
}

bool on_open_application_area(uint32_t access_id) {
    std::lock_guard lk(g_lock);
    if (g_state != DeviceState::TagMounted || !tag_data_ready_locked()) return false;
    if (g_decoded.app_area_initialized && g_decoded.application_area_id != 0 && access_id != 0 &&
        access_id != g_decoded.application_area_id)
        log("nfp: amiibo access_id 0x%08X, game requested 0x%08X (opening anyway)",
            g_decoded.application_area_id, access_id);
    g_app_area_open = true;
    g_open_access_id = access_id;
    return true;
}

void fill_tag_info(void* out) {
    if (!out) return;
    std::memset(out, 0, 0x58);
    std::lock_guard lk(g_lock);
    if (!tag_data_ready_locked()) return;
    auto* p = static_cast<uint8_t*>(out);
    // nn::nfp::TagInfo (yuzu / Ryujinx): u8 uuid[10], u8 uuid_length, reserved[0x15], u32 protocol, u32 tag_type.
    // ACNH (main_01D23A50) reads uuid_length at +0xA and formats that many uuid bytes.
    const uint8_t len = g_decoded.uid_length;
    std::memcpy(p, g_decoded.uid, len);
    p[0xA] = len;
    st<uint32_t>(reinterpret_cast<uint64_t>(p + 0x20), 1u);  // NfcProtocol TypeA
    st<uint32_t>(reinterpret_cast<uint64_t>(p + 0x24), 2u);  // TagType Type2 (NTAG215)
}

void fill_model_info(void* out) {
    if (!out) return;
    std::memset(out, 0, 0x40);
    std::lock_guard lk(g_lock);
    if (!tag_data_ready_locked()) return;
    const amiibo::AmiiboModelInfo& m = g_decoded.model;
    auto* p = static_cast<uint8_t*>(out);
    // nn::nfp::ModelInfo (Switch SDK / Ryujinx): u16 characterId, u8 variant, u8 series, u16 modelNumber, u8 type.
    // CharacterId ushort is byte-swapped so guest LE memory matches tag order (e.g. 0x0267 -> bytes 02 67).
    const uint16_t character_id_field = uint16_t((m.character_id >> 8) | (m.character_id << 8));
    st<uint16_t>(reinterpret_cast<uint64_t>(p), character_id_field);
    p[2] = m.character_variant;
    p[3] = m.series;
    st<uint16_t>(reinterpret_cast<uint64_t>(p + 4), m.model_number);
    p[6] = m.amiibo_type;
    if (nfp_trace_enabled())
        log("nfp: ModelInfo char=0x%04X series=%u model=%u type=%u", m.character_id, m.series, m.model_number,
            m.amiibo_type);
}

void fill_register_info(void* out) {
    if (!out) return;
    std::memset(out, 0, kRegisterInfoSize);
    std::lock_guard lk(g_lock);
    if (!tag_data_ready_locked()) return;
    const uint8_t* pl = g_decoded.plain.data();
    auto* p = static_cast<uint8_t*>(out);
    // nn::nfp::RegisterInfo: CharInfo 0x60, Date firstWriteDate, ArrayString name, fontRegion.
    std::memcpy(p, pl + 0x30, 0x60);
    write_guest_date(p + 0x60, pl + 0x54);
    std::memcpy(p + 0x64, pl + 0x70, 0x14);
    p[0x78] = pl[0x84];
}

void fill_register_info_private(void* out) {
    if (!out) return;
    std::memset(out, 0, kRegisterInfoPrivateSize);
    std::lock_guard lk(g_lock);
    if (!tag_data_ready_locked()) return;
    const uint8_t* pl = g_decoded.plain.data();
    auto* p = static_cast<uint8_t*>(out);
    std::memcpy(p, pl + 0x30, 0x60);
    write_guest_date(p + 0x60, pl + 0x54);
    std::memcpy(p + 0x64, pl + 0x70, 0x14);
    p[0x78] = pl[0x84];
    std::memcpy(p + kRegisterInfoSize, pl + 0x90, kRegisterInfoPrivateSize - kRegisterInfoSize);
}

void fill_common_info(void* out) {
    if (!out) return;
    std::memset(out, 0, 0x40);
    std::lock_guard lk(g_lock);
    if (!tag_data_ready_locked()) return;
    const uint8_t* pl = g_decoded.plain.data();
    auto* p = static_cast<uint8_t*>(out);
    // nn::nfp::CommonInfo: last_write_date, write_counter u16, version, pad, application_area_size.
    write_guest_date(p, pl + 0x58);
    st<uint16_t>(reinterpret_cast<uint64_t>(p + 4), uint16_t(uint16_t(pl[0x29]) << 8) | pl[0x2A]);
    st<uint16_t>(reinterpret_cast<uint64_t>(p + 6), 1);  // version (Ryujinx VirtualAmiibo default)
    st<uint32_t>(reinterpret_cast<uint64_t>(p + 8), kApplicationAreaSize);
}

bool tag_has_application_area() {
    std::lock_guard lk(g_lock);
    return tag_data_ready_locked() && g_decoded.app_area_initialized;
}

void fill_admin_info(void* out) {
    if (!out) return;
    std::memset(out, 0, 0x20);
    std::lock_guard lk(g_lock);
    if (!tag_data_ready_locked()) return;
    auto* p = static_cast<uint8_t*>(out);
    uint64_t program_id = 0;
    const uint8_t* pid = g_decoded.plain.data() + 0xAC;
    for (int i = 0; i < 8; i++) program_id = (program_id << 8) | pid[i];
    st<uint64_t>(reinterpret_cast<uint64_t>(p), program_id);
    st<uint32_t>(reinterpret_cast<uint64_t>(p + 8), g_decoded.application_area_id);
    uint8_t flags = uint8_t((g_decoded.plain[0x2C] >> 4) & 0x0F);
    if (flags == 0) flags = 0x02;
    if (g_decoded.app_area_initialized) flags |= 0x01;
    p[0x0E] = flags;
    p[0x0F] = 0x02;
    p[0x10] = g_decoded.plain[0x2E];
}

void fill_nfp_data(void* out) {
    if (!out) return;
    std::memset(out, 0, kNfpDataSize);
    std::lock_guard lk(g_lock);
    if (!tag_data_ready_locked()) return;
    const uint8_t* pl = g_decoded.plain.data();
    auto* nd = static_cast<uint8_t*>(out);
    nd[0] = 0xA5;
    write_be16(nd + 0x2, uint16_t(uint16_t(pl[0x29]) << 8) | pl[0x2A]);
    write_be32(nd + 0x4, uint32_t(uint32_t(pl[0x2B]) << 24) | (uint32_t(pl[0x2C] & 0x0F) << 20));
    write_guest_date(nd + 0x40, pl + 0x58);
    write_be16(nd + 0x44, uint16_t(uint16_t(pl[0x29]) << 8) | pl[0x2A]);
    nd[0x46] = 0;
    nd[0x47] = 0;
    st<uint32_t>(reinterpret_cast<uint64_t>(nd + 0x48), kApplicationAreaSize);
    std::memcpy(nd + 0x80, pl + 0x30, 0x60);
    write_guest_date(nd + 0xE8, pl + 0x54);
    std::memcpy(nd + 0xEC, pl + 0x70, 0x14);
    nd[0x102] = pl[0x84];
    std::memcpy(nd + 0x180, pl + 0xAC, 8);
    write_be32(nd + 0x188, g_decoded.application_area_id);
    uint8_t admin_flags = uint8_t((pl[0x2C] >> 4) & 0x0F);
    if (admin_flags == 0) admin_flags = 0x02;
    if (g_decoded.app_area_initialized) admin_flags |= 0x01;
    nd[0x18E] = admin_flags;
    nd[0x18F] = 0x02;
    nd[0x190] = pl[0x2E];
    std::memcpy(nd + 0x1C0, g_decoded.application_area.data(), kApplicationAreaSize);
}

bool read_application_area(void* out, uint32_t size) {
    if (!out || !size) return false;
    std::lock_guard lk(g_lock);
    if (g_state != DeviceState::TagMounted) return false;
    if (!g_app_area_open) g_app_area_open = true;
    if (size > kApplicationAreaSize) size = kApplicationAreaSize;
    std::memcpy(out, g_decoded.application_area.data(), size);
    return true;
}

void write_application_area(const void* data, uint32_t size) {
    if (!data || !size) return;
    std::lock_guard lk(g_lock);
    if (!g_app_area_open) return;
    if (size > kApplicationAreaSize) size = kApplicationAreaSize;
    std::memcpy(g_decoded.application_area.data(), data, size);
}

void bind_system_event(CPU& c, uint64_t guest_out, int event_slot) {
    if (!guest_out) return;
    auto state = std::make_shared<EventState>();
    auto read_ev = std::make_shared<KEvent>();
    auto write_ev = std::make_shared<KEvent>();
    read_ev->state = write_ev->state = state;
    const uint32_t hr = handle_add(read_ev);
    const uint32_t hw = handle_add(write_ev);
    static const uint64_t attach =
        find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    if (!attach) return;
    const uint64_t saved[8] = {c.x[0], c.x[1], c.x[2], c.x[3], c.x[4], c.x[5], c.x[6], c.x[7]};
    c.x[0] = guest_out;
    c.x[1] = hr;
    c.x[2] = 1;
    c.x[3] = hw;
    c.x[4] = 1;
    c.x[5] = 0;  // ManualClear (same as nn::os::CreateSystemEvent for NFP)
    call(c, attach);
    for (int i = 0; i < 8; i++) c.x[i] = saved[i];
    set_event_slot(event_slot, std::move(state));
}

void signal_activate_if_pending() {
    std::lock_guard lk(g_lock);
    begin_tag_present_wait_locked();
}

void signal_availability() { signal_state(g_availability_state); }

void set_event_slot(int event_slot, std::shared_ptr<EventState> state) {
    if (event_slot == 0)
        g_activate_state = std::move(state);
    else if (event_slot == 1)
        g_deactivate_state = std::move(state);
    else
        g_availability_state = std::move(state);
}

void sync_activate_event_after_attach() {
    std::lock_guard lk(g_lock);
    if (g_state == DeviceState::TagFound || g_state == DeviceState::TagMounted) signal_state(g_activate_state);
}

void poll() {
    std::lock_guard lk(g_lock);
    if (g_tag_present_pending) promote_tag_found_locked();
}

}  // namespace nx::nfp_host
