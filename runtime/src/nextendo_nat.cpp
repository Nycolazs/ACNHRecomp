// Dual-vantage NAT keepalive while Nextendo online services are active (Pia hole-punch helper).
#include "nextendo.h"

#include "ldn_net.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#if !defined(NX_LAUNCHER_UI)
#include <nx/runtime.h>
#else
namespace nx {
void log(const char* fmt, ...);
}
#endif

namespace nx::nextendo {

namespace {

std::atomic<bool> g_nat_running{false};

void nat_thread() {
    net_init();
    sock_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) {
        g_nat_running = false;
        return;
    }
    set_timeout(s, 1000);
    const std::string server = server_ip();
    const std::string nat = nat_ip();
    while (g_nat_running.load() && online_services_active()) {
        for (const std::string& ip : {server, nat}) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(1);  // probe port; Nextendo NAT responders accept any
            a.sin_addr.s_addr = inet_addr(ip.c_str());
            const char poke[] = "NX";
            sendto(s, poke, 2, MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&a), sizeof a);
        }
        for (int i = 0; i < 50 && g_nat_running.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    sock_close(s);
    g_nat_running = false;
}

}  // namespace

void nat_keepalive_start() {
    if (!online_services_active()) return;
    if (g_nat_running.exchange(true)) return;
    std::thread(nat_thread).detach();
    log("nextendo: NAT keepalive started");
}

void nat_keepalive_stop() { g_nat_running = false; }

}  // namespace nx::nextendo
