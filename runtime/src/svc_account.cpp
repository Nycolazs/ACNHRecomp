// acc:u0 (accounts) and the storage objects am hands out (launch parameters).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "ipc.h"
#include "nextendo.h"
#include "users.h"

namespace nx {

extern bool g_trace_net;  // loader.cpp (DIAG)

// IStorage / IStorageAccessor over a byte blob (am's storage objects).
namespace {
struct StorageService : Service {
    std::shared_ptr<std::vector<uint8_t>> blob;
    StorageService() : Service("IStorage") {}
};
}  // namespace

std::shared_ptr<Service> make_storage(std::shared_ptr<std::vector<uint8_t>> blob) {
    auto s = std::make_shared<StorageService>();
    s->blob = blob;
    s->cmds[0] = [blob](IpcCtx& c) {  // Open -> IStorageAccessor
        auto a = std::make_shared<Service>("IStorageAccessor");
        a->cmds[0] = [blob](IpcCtx& c) { c.push<int64_t>(int64_t(blob->size())); return 0u; };  // GetSize
        a->cmds[10] = [blob](IpcCtx& c) {                                                       // Write(offset)
            int64_t off = c.arg<int64_t>(0);
            Buffer in = c.in_buf(0);
            if (off < 0 || uint64_t(off) > blob->size()) return 0x3E880u;
            size_t n = std::min<size_t>(in.size, blob->size() - size_t(off));
            std::memcpy(blob->data() + off, reinterpret_cast<void*>(in.addr), n);
            return 0u;
        };
        a->cmds[11] = [blob](IpcCtx& c) {  // Read(offset)
            int64_t off = c.arg<int64_t>(0);
            Buffer out = c.out_buf(0);
            if (off < 0 || uint64_t(off) > blob->size()) return 0x3E880u;  // out of bounds
            size_t n = std::min<size_t>(out.size, blob->size() - size_t(off));
            std::memcpy(reinterpret_cast<void*>(out.addr), blob->data() + off, n);
            return 0u;
        };
        c.push_object(a);
        return 0u;
    };
    return s;
}

std::shared_ptr<Service> make_storage(std::vector<uint8_t> data) {
    return make_storage(std::make_shared<std::vector<uint8_t>>(std::move(data)));
}

std::shared_ptr<std::vector<uint8_t>> storage_blob(const std::shared_ptr<Service>& s) {
    auto p = std::dynamic_pointer_cast<StorageService>(s);
    return p ? p->blob : nullptr;
}

std::shared_ptr<Service> preselected_user_storage() {
    UserInfo u = user_boot();
    std::vector<uint8_t> d(0x88, 0);
    uint32_t magic = 0xC79497CA, selected = 1;
    std::memcpy(d.data(), &magic, 4);
    std::memcpy(d.data() + 4, &selected, 4);
    std::memcpy(d.data() + 8, &u.lo, 8);
    std::memcpy(d.data() + 16, &u.hi, 8);
    log("account: preselected user %s", u.nickname.c_str());
    return make_storage(std::move(d));
}

namespace {

void push_uid(IpcCtx& c, const UserInfo& u) {
    c.push<uint64_t>(u.lo);
    c.push<uint64_t>(u.hi);
}

void write_uid_list(IpcCtx& c) {
    Buffer out = c.out_buf(0);
    if (out.addr && out.size >= 16) {
        std::memset(reinterpret_cast<void*>(out.addr), 0, out.size);
        size_t i = 0;
        for (auto& u : users_all()) {
            if ((i + 1) * 16 > out.size) break;
            st<uint64_t>(out.addr + i * 16, u.lo);
            st<uint64_t>(out.addr + i * 16 + 8, u.hi);
            i++;
        }
    }
}

std::shared_ptr<Service> profile(UserInfo user) {
    auto s = std::make_shared<Service>("IProfile");
    auto base = [user](IpcCtx& c) {  // ProfileBase {Uid; u64 last_edit; char nickname[0x20]}
        push_uid(c, user);
        c.push<uint64_t>(0);
        char name[0x20] = {};
        std::memcpy(name, user.nickname.c_str(), std::min<size_t>(user.nickname.size(), 0x1F));
        for (char ch : name) c.push<char>(ch);
    };
    s->cmds[0] = [base](IpcCtx& c) {  // Get(out UserData buffer) -> ProfileBase
        Buffer out = c.out_buf(0);
        if (out.addr) std::memset(reinterpret_cast<void*>(out.addr), 0, out.size);
        base(c);
        return 0u;
    };
    s->cmds[1] = [base](IpcCtx& c) { base(c); return 0u; };            // GetBase
    s->cmds[10] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // GetImageSize: no picture
    s->cmds[11] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // LoadImage
    return s;
}

// IAsyncContext: a request running on a host thread. GetSystemEvent hands out an event signaled when it ends.
std::shared_ptr<Service> async_context(std::function<uint32_t()> work) {
    struct State {
        std::shared_ptr<EventState> event = std::make_shared<EventState>();
        std::atomic<bool> done{false};
        std::atomic<uint32_t> result{0};
    };
    auto st = std::make_shared<State>();
    std::thread([st, work = std::move(work)] {
        st->result = work();
        st->done = true;
        st->event->signaled = true;
        notify_all();
    }).detach();
    auto s = std::make_shared<Service>("IAsyncContext");
    s->cmds[0] = [st](IpcCtx& c) {  // GetSystemEvent
        auto r = std::make_shared<KEvent>();
        r->state = st->event;
        c.out_copy.push_back(handle_add(r));
        return 0u;
    };
    s->cmds[1] = [](IpcCtx&) { return 0u; };  // Cancel: the request finishes on its own
    s->cmds[2] = [st](IpcCtx& c) { c.push<uint8_t>(st->done ? 1 : 0); return 0u; };  // HasDone
    s->cmds[3] = [st](IpcCtx&) { return st->done ? st->result.load() : 0u; };       // GetResult
    return s;
}

// account: a failure to get the id token. shortcut: the exact Result a console returns here was not looked up; the
// game only needs a failure to show its network error. Replace with the real code if it ever shows a wrong message.
constexpr uint32_t kNsaUnavailable = (280u << 9) | 124u;

std::shared_ptr<Service> manager_for_application() {
    auto s = std::make_shared<Service>("IManagerForApplication");
    // CheckAvailability: always available, as before; offline, nifm already keeps the game off the network.
    s->cmds[0] = [](IpcCtx&) { return 0u; };
    s->cmds[1] = [](IpcCtx& c) {  // GetAccountId (NetworkServiceAccountId)
        c.push<uint64_t>(nextendo::network_service_account_id());
        return 0u;
    };
    // EnsureIdTokenCacheAsync -> IAsyncContext: renews the account and nex tokens with Nextendo.
    s->cmds[2] = [](IpcCtx& c) {
        log("account: EnsureIdTokenCacheAsync");
        {  // DIAG: trace the online calls that follow, 60 s
            static std::atomic<bool> armed{false};
            if (!armed.exchange(true)) {
                g_trace_net = true;
                std::thread([] {
                    std::this_thread::sleep_for(std::chrono::seconds(60));
                    g_trace_net = false;
                }).detach();
            }
        }
        c.push_object(async_context([] {
            if (!nextendo::online_services_active()) return kNsaUnavailable;
            if (!nextendo::ensure_online_identity()) {
                log("account: Nextendo token renewal failed: %s", nextendo::last_error().c_str());
                return kNsaUnavailable;
            }
            return 0u;
        }));
        return 0u;
    };
    // LoadIdTokenCache(out buffer) -> u32 size: the BAAS id_token (nextendo_token.cpp), as a C string. cmd 3 is the
    // old number, cmd 4 the current one (ACNH 3.0.3 uses 4).
    s->cmds[3] = [](IpcCtx& c) {
        if (!nextendo::online_services_active()) return kNsaUnavailable;
        const std::string tok = nextendo::baas_id_token(nextendo::game_version_string());
        log("account: LoadIdTokenCache -> %zu-byte id_token", tok.size());
        if (tok.empty()) return kNsaUnavailable;
        Buffer out = c.out_buf(0);
        if (!out.addr || out.size <= tok.size()) {
            log("account: id_token (%zu bytes) does not fit the game's buffer (%llu)", tok.size(),
                (unsigned long long)out.size);
            return kNsaUnavailable;
        }
        std::memset(reinterpret_cast<void*>(out.addr), 0, out.size);
        std::memcpy(reinterpret_cast<void*>(out.addr), tok.data(), tok.size());
        c.push<uint32_t>(uint32_t(tok.size()));
        return 0u;
    };
    s->cmds[4] = s->cmds[3];
    return s;
}

void account(Service& s) {
    s.cmds[0] = [](IpcCtx& c) { c.push<int32_t>(int32_t(users_all().size())); return 0u; };  // GetUserCount
    s.cmds[1] = [](IpcCtx& c) {                                                               // GetUserExistence(uid)
        c.push<uint8_t>(user_find(c.arg<uint64_t>(0), c.arg<uint64_t>(8)));
        return 0u;
    };
    s.cmds[2] = [](IpcCtx& c) { write_uid_list(c); return 0u; };  // ListAllUsers
    s.cmds[3] = [](IpcCtx& c) { write_uid_list(c); return 0u; };  // ListOpenUsers
    s.cmds[4] = [](IpcCtx& c) { push_uid(c, user_last_opened()); return 0u; };  // GetLastOpenedUser
    s.cmds[5] = [](IpcCtx& c) {                                                  // GetProfile(uid)
        UserInfo u;
        if (!user_find(c.arg<uint64_t>(0), c.arg<uint64_t>(8), &u)) {
            log("account: GetProfile of an unknown user");
            return 0x4E2u;  // account 0x?: user not found
        }
        c.push_object(profile(u));
        return 0u;
    };
    s.cmds[50] = [](IpcCtx& c) { c.push<uint8_t>(1); return 0u; };  // IsUserRegistrationRequestPermitted
    s.cmds[51] = [](IpcCtx& c) { push_uid(c, user_boot()); return 0u; };  // TrySelectUserWithoutInteraction
    s.cmds[100] = [](IpcCtx&) { return 0u; };                       // InitializeApplicationInfo (old)
    s.cmds[101] = [](IpcCtx& c) { c.push_object(manager_for_application()); return 0u; };
    s.cmds[140] = [](IpcCtx&) { return 0u; };  // InitializeApplicationInfo
    s.cmds[141] = [](IpcCtx& c) { write_uid_list(c); return 0u; };  // ListQualifiedUsers (SDK 16)
    s.cmds[160] = [](IpcCtx&) { return 0u; };                       // InitializeApplicationInfoV2 (SDK 16)
    s.cmds[150] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // IsUserAccountSwitchLocked
}

ServiceRegistrar r_acc_u0("acc:u0", [] { auto s = std::make_shared<Service>("acc:u0"); account(*s); return s; });
ServiceRegistrar r_acc_u1("acc:u1", [] { auto s = std::make_shared<Service>("acc:u1"); account(*s); return s; });

}  // namespace

}  // namespace nx
