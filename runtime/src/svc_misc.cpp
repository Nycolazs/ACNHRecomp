// Small services that only need to answer "yes, fine": parental controls, add-on content, logging, power.
#include <cstdlib>
#include "config.h"
#include "ipc.h"

namespace nx {

namespace {

std::shared_ptr<Service> make(const char* name, std::function<void(Service&)> setup = {}) {
    auto s = std::make_shared<Service>(name);
    if (setup) setup(*s);
    return s;
}

std::shared_ptr<Service> parental_control() {
    return make("IParentalControlService", [](Service& s) {
        s.cmds[1403] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // IsRestrictionEnabled
        s.cmds[1001] = [](IpcCtx&) { return 0u; };                          // CheckFreeCommunicationPermission
    });
}

void pctl(Service& s) {
    s.cmds[0] = [](IpcCtx& c) { c.push_object(parental_control()); return 0u; };  // CreateService
    s.cmds[1] = [](IpcCtx& c) { c.push_object(parental_control()); return 0u; };  // CreateServiceWithoutInitialize
}

ServiceRegistrar r_pctl("pctl", [] { return make("pctl", pctl); });
ServiceRegistrar r_pctl_s("pctl:s", [] { return make("pctl:s", pctl); });
ServiceRegistrar r_pctl_a("pctl:a", [] { return make("pctl:a", pctl); });
ServiceRegistrar r_pctl_r("pctl:r", [] { return make("pctl:r", pctl); });

ServiceRegistrar r_lm("lm", [] {
    return make("lm", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) {  // OpenLogger
            c.push_object(make("ILogger", [](Service& l) {
                // Log(packet): a 0x10-byte header, then chunks {key u8, size u8, bytes}; the text runs go to our log.
                l.cmds[0] = [](IpcCtx& c) {
                    const Buffer in = c.in_buf(0);
                    const auto* p = reinterpret_cast<const uint8_t*>(in.addr);
                    std::string text;
                    for (uint64_t i = 0x10; p && i < in.size; i++)
                        text += (p[i] >= 0x20 && p[i] < 0x7f) ? char(p[i]) : ' ';
                    if (!text.empty()) log("lm: %s", text.c_str());
                    return 0u;
                };
            }));
            return 0u;
        };
    });
});

ServiceRegistrar r_apm("apm", [] {
    return make("apm", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(make("ISession")); return 0u; };  // OpenSession
        s.cmds[1] = [](IpcCtx& c) {  // GetPerformanceMode: boost (docked), normal with handheld
            c.push<uint32_t>(config_is_handheld() ? 0 : 1);
            return 0u;
        };
    });
});

ServiceRegistrar r_aoc("aoc:u", [] {
    return make("aoc:u", [](Service& s) {
        s.cmds[2] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // CountAddOnContent (old)
        s.cmds[3] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // ListAddOnContent (old)
        s.cmds[7] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };
        s.cmds[8] = [](IpcCtx& c) {                                       // GetAddOnContentListChangedEvent
            auto e = std::make_shared<KEvent>();
            e->state = std::make_shared<EventState>();
            c.out_copy.push_back(handle_add(e));
            return 0u;
        };
    });
});

}  // namespace

}  // namespace nx
