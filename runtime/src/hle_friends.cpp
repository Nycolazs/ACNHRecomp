// nn::friends: empty / not-linked offline; when signed into Nextendo, the friend list comes from the account API
// (nextendo.cpp caches it). ACNH matches the network service account id each player announces against these
// entries (best friends, letters, visits), so an entry carries the id that friend announces: their baasUserID ("nsa")
// when both accounts have one (linked to a console), else the pid (friend_id below).
#include <cstring>
#include <vector>

#include <nx/runtime.h>

#include "kernel.h"
#include "nextendo.h"

namespace nx {

namespace {

constexpr uint32_t kOk = 0;
constexpr uint32_t kNotLinked = 121 | (4 << 9);  // friends: network service account not available

// nn::friends::Friend (0x200): Uid +0x00, NetworkServiceAccountId +0x10, Nickname +0x18 (0x21 bytes), UserPresence
// +0x40 {Uid +0x40, last online +0x50, status +0x58, same presence group +0x5C, app key/value storage +0x60 (0xC0)},
// favourite +0x120, new +0x121, valid +0x128.
constexpr uint64_t kFriendSize = 0x200;
enum PresenceStatus : uint32_t { kOffline = 0, kOnline = 1, kOnlinePlay = 2 };

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

// The id this friend's game announces, which ACNH matches against: their baasUserID only when ours is known too (both
// sides are then linked to a console and announce it); otherwise every Nextendo client announces its pid.
uint64_t friend_id(const nextendo::Friend& f) { return f.nsa && nextendo::account().nsa ? f.nsa : f.pid; }

bool same_game(const nextendo::Friend& f) {
    return f.status != kOffline && std::strtoull(f.app_id.c_str(), nullptr, 16) == nextendo::kAcnhTitleId;
}

void write_friend(uint64_t out, uint64_t uid, const nextendo::Friend& f) {
    std::memset(reinterpret_cast<void*>(out), 0, kFriendSize);
    if (uid) std::memcpy(reinterpret_cast<void*>(out), reinterpret_cast<const void*>(uid), 16);
    st<uint64_t>(out + 0x10, friend_id(f));
    std::strncpy(reinterpret_cast<char*>(out + 0x18), f.name.c_str(), 0x20);
    if (uid) std::memcpy(reinterpret_cast<void*>(out + 0x40), reinterpret_cast<const void*>(uid), 16);
    const uint32_t status = f.status == 2 ? kOnlinePlay : f.status == 1 ? kOnline : kOffline;
    st<uint32_t>(out + 0x58, status);
    st<uint8_t>(out + 0x5C, same_game(f) ? 1 : 0);
    st<uint8_t>(out + 0x120, f.favorite ? 1 : 0);
    st<uint8_t>(out + 0x128, 1);
}

// FriendFilter: presence filter u32 (0 any, 1 online, 2 online play, 3 either), favourite only, same app presence
// only, same app played only, any app played only (bools), presence group id u64.
bool passes(uint64_t filter, const nextendo::Friend& f) {
    if (!filter) return true;
    const uint32_t presence = ld<uint32_t>(filter);
    if (presence == 1 && f.status != 1) return false;
    if (presence == 2 && f.status != 2) return false;
    if (presence == 3 && f.status == 0) return false;
    if (ld<uint8_t>(filter + 4) && !f.favorite) return false;
    if (ld<uint8_t>(filter + 5) && !same_game(f)) return false;
    return true;
}

// GetFriendList(int* count, Friend* out, const Uid&, int offset, int max, const FriendFilter&)
void GetFriendList(CPU& c) {
    if (!nextendo::online_services_active()) return empty_list(c);
    const uint64_t out_count = c.x[0], out = c.x[1], uid = c.x[2], filter = c.x[5];
    const int offset = int(c.x[3]) < 0 ? 0 : int(c.x[3]);
    const int max = int(c.x[4]) < 0 ? 0 : int(c.x[4]);
    std::vector<nextendo::Friend> list;
    for (const auto& f : nextendo::friends_cached())
        if (passes(filter, f)) list.push_back(f);
    int written = 0;
    for (int i = offset; i < int(list.size()) && written < max; i++, written++)
        if (out) write_friend(out + uint64_t(written) * kFriendSize, uid, list[size_t(i)]);
    if (out_count) st<int32_t>(out_count, written);
    c.x[0] = kOk;
}

// UpdateFriendInfo(Friend* out, const Uid&, const NetworkServiceAccountId* ids, int count): the entries for those
// ids, in order; an id that is not (or no longer) a friend comes back not valid.
void UpdateFriendInfo(CPU& c) {
    const uint64_t out = c.x[0], uid = c.x[1], ids = c.x[2];
    const int count = int(c.x[3]);
    const auto list = nextendo::online_services_active() ? nextendo::friends_cached() : std::vector<nextendo::Friend>{};
    for (int i = 0; out && ids && i < count; i++) {
        const uint64_t id = ld<uint64_t>(ids + uint64_t(i) * 8);
        const uint64_t dst = out + uint64_t(i) * kFriendSize;
        std::memset(reinterpret_cast<void*>(dst), 0, kFriendSize);
        for (const auto& f : list)
            if (friend_id(f) == id) write_friend(dst, uid, f);
    }
    c.x[0] = kOk;
}

// EnsureFriendListAvailable(AsyncContext*, const Uid&): fetches the list now, so the GetFriendList after the
// context's event (signaled at once, AsyncGetSystemEvent) sees it.
void EnsureFriendListAvailable(CPU& c) {
    if (!nextendo::online_services_active()) return not_linked(c);
    nextendo::friends_refresh_blocking();
    ok(c);
}

// ShowUserDetailInfo / StartSendingFriendRequest open system applets the runtime does not run. Friend requests go
// through the Nextendo site or app; report success so the game carries on.
void online_ok(CPU& c) {
    if (!nextendo::online_services_active()) return not_linked(c);
    ok(c);
}

void DeclareOpen(CPU& c) {
    nextendo::presence_set_playing(true);
    ok(c);
}

void DeclareClose(CPU& c) {
    nextendo::presence_set_playing(false);
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
NX_HLE("_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi",
       UpdateFriendInfo);
NX_HLE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", EnsureFriendListAvailable);
NX_HLE("_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", AsyncGetSystemEvent);
NX_HLE("_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_",
       online_ok);
NX_HLE("_ZN2nn7friends25StartSendingFriendRequestERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_",
       online_ok);

}  // namespace nx
