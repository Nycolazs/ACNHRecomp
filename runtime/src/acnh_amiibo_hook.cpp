// ACNH amiibo: camp gate (main_033F51F8). Nook Stop needs no hook: its NFP manager (main_01D23A50) runs the
// nn::nfp HLE as on the console.
//
// The camp gate asks the game's own controller manager ([host+0x238]+0x570, no null check), not nn::nfp, whether a
// reader is usable. With a virtual tag loaded the gate's ready byte is set first, so the gate returns 1 before it
// dereferences that manager, and a 0 result is forced to 1.
#include <atomic>

#include <nx/runtime.h>

#include "nfp_host.h"

namespace nx {

namespace {

constexpr uint64_t kCampGateReadyOff = 0x1c000 + 0x1d8 + 0xc28;

bool guest_writable(uint64_t addr) {
    if (module_at(addr)) return true;
    const VmInfo v = vm_query(addr);
    return v.state != kMemFree && (v.perm & 2u) != 0;
}

void amiibo_scan_gate_entry(CPU& c) {
    const uint64_t host = c.x[0];
    if (!host || !nfp_host::has_tag()) return;
    const uint64_t ready = host + kCampGateReadyOff;
    if (!guest_writable(ready) || ld<uint8_t>(ready)) return;
    st<uint8_t>(ready, 1);
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) log("nfp: camp amiibo gate ready (host %llx)", (unsigned long long)host);
}

void amiibo_scan_gate_exit(CPU& c) {
    if ((c.x[0] & 1) || !nfp_host::has_tag()) return;
    c.x[0] = 1;
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) log("nfp: camp amiibo gate forced ready (virtual tag loaded)");
}

}  // namespace

NX_HOOK(0x33f51f8, amiibo_scan_gate_entry);
NX_HOOK(0x33f5330, amiibo_scan_gate_exit);

}  // namespace nx
