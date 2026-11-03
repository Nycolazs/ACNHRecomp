// nn::friends: empty / not-linked offline; when signed into Nextendo, friend list comes from the account API.
#include <cstring>
#include <vector>

#include <nx/runtime.h>

#include "kernel.h"
#include "nextendo.h"

namespace nx {

namespace {

constexpr uint32_t kOk = 0;
constexpr uint32_t kNotLinked = 121 | (4 << 9);  // friends: network service account not available

void ok(CPU& c) { c.x[0] = kOk; }

void empty_list(CPU& c) {
    if (c.x[0]) st<int32_t>(c.x[0], 0);
    c.x[0] = kOk;
}

void GetPlayHistoryRegistrationKey(CPU& c) {
    if (c.x[0]) std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x40);
    c.x[0] = kOk;
}

void not_linked(CPU& c) { c.x[0] = kNotLinked; }

void AsyncGetSystemEvent(CPU& c) {
    auto e = std::make_shared<EventState>();
    e->signaled = true;
    auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
    r->state = w->state = e;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    static const uint64_t attach =
        find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[0] = saved[1], c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;
    if (attach) call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    c.x[0] = kOk;
}

void GetFriendList(CPU& c) {
    if (!nextendo::online_services_active()) return empty_list(c);
    std::vector<nextendo::Friend> friends;
    nextendo::fetch_friends(friends);
    // (int* count, Friend* out, const Uid&, int offset, int count, const FriendFilter&)
    int offset = int(c.x[3]);
    int max = int(c.x[4]);
    if (max < 0) max = 0;
    int written = 0;
    uint64_t out = c.x[1];
    // Friend structure is large; write NetworkServiceAccountId at a common offset (0) and name if layout allows.
    // Minimal: report count only so the game does not crash; detailed layout filled as we learn offsets.
    for (int i = offset; i < int(friends.size()) && written < max; i++, written++) {
        if (out) {
            // Zero a conservative Friend size (0x200) then store NSAID (u64) at +0.
            std::memset(reinterpret_cast<void*>(out + uint64_t(written) * 0x200), 0, 0x200);
            st<uint64_t>(out + uint64_t(written) * 0x200, friends[size_t(i)].pid);
        }
    }
    if (c.x[0]) st<int32_t>(c.x[0], written);
    c.x[0] = kOk;
}

void EnsureFriendListAvailable(CPU& c) {
    if (nextendo::online_services_active()) {
        ok(c);
        return;
    }
    not_linked(c);
}

void DeclareOpen(CPU& c) {
    if (nextendo::online_services_active()) nextendo::publish_presence(2, 0x01006F8002326000ull);
    ok(c);
}

void DeclareClose(CPU& c) {
    if (nextendo::online_services_active()) nextendo::publish_presence(1, 0);
    ok(c);
}

}  // namespace

NX_HLE("_ZN2nn7friends10InitializeEv", ok);
NX_HLE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", GetFriendList);
NX_HLE("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii", empty_list);
NX_HLE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb",
       GetPlayHistoryRegistrationKey);
NX_HLE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_",
       ok);
NX_HLE("_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE", DeclareOpen);
NX_HLE("_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE", DeclareClose);
NX_HLE("_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", ok);
NX_HLE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", EnsureFriendListAvailable);
NX_HLE("_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", AsyncGetSystemEvent);
NX_HLE("_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_",
       EnsureFriendListAvailable);
NX_HLE("_ZN2nn7friends25StartSendingFriendRequestERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_",
       EnsureFriendListAvailable);

}  // namespace nx
