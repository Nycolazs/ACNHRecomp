// This computer on the local network, for local play: its address, and the test of a friend's address (one ldn_mitm
// Scan sent there over UDP). Shared by the game and the launcher.
#include "ldn_lan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "ldn_net.h"

namespace nx {

uint32_t lan_bind_ip() {
    static const uint32_t ip = [] {
        const char* e = std::getenv("NX_LDN_IP");
        in_addr a{};
        return e && inet_pton(AF_INET, e, &a) == 1 ? ntohl(a.s_addr) : 0u;
    }();
    return ip;
}

uint32_t route_address(uint32_t to) {
    net_init();
    const sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) return 0;
    const sockaddr_in a = ipv4(to, 9);
    sockaddr_in me{};
    socklen_t len = sizeof me;
    const bool ok = !connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) &&
                    !getsockname(s, reinterpret_cast<sockaddr*>(&me), &len);
    sock_close(s);
    return ok ? ntohl(me.sin_addr.s_addr) : 0;
}

uint32_t local_address() {
    if (const uint32_t ip = lan_bind_ip()) return ip;
    if (const uint32_t ip = route_address(0xC0000201)) return ip;  // towards 192.0.2.1: the default route's interface
    // A LAN with no route out: the first address of this computer's name that is not loopback.
    char name[256];
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    uint32_t ip = 0x7F000001;
    if (!gethostname(name, sizeof name) && !getaddrinfo(name, nullptr, &hints, &res)) {
        for (const addrinfo* r = res; r; r = r->ai_next) {
            const uint32_t v = ntohl(reinterpret_cast<const sockaddr_in*>(r->ai_addr)->sin_addr.s_addr);
            if (v >> 24 != 127) {
                ip = v;
                break;
            }
        }
        freeaddrinfo(res);
    }
    return ip;
}

std::string ip_text(uint32_t ip) {
    char b[16];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

bool lan_private_ip(uint32_t ip) {
    return ip >> 24 == 10 || ip >> 24 == 127 || ip >> 20 == 0xAC1 || ip >> 16 == 0xC0A8 || ip >> 16 == 0xA9FE ||
           ip >> 22 == (100u << 2 | 1);
}

// The island's name in the advertise data: UTF-16 at +0x1C, 10 characters at most (seen in the game's data), to UTF-8.
std::string ldn_island_name(const uint8_t* info) {
    constexpr size_t kAdv = 0x26C, kName = 0x1C;
    std::string out;
    for (size_t i = 0; i < 10; i++) {
        uint16_t c;
        std::memcpy(&c, info + kAdv + kName + i * 2, 2);
        if (!c) break;
        if (c < 0x80) {
            if (c >= 0x20) out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0xD800 || c > 0xDFFF) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

namespace {

std::mutex g_address_lock;
AddressResult g_address;
int g_address_generation = 0;  // only the latest test's answer counts

AddressResult check_address(std::string item) {
    AddressResult r;
    r.check = AddressCheck::Bad;
    net_init();
    uint16_t port = kLanPort;
    if (const size_t c = item.find(':'); c != std::string::npos) {
        port = uint16_t(std::atoi(item.c_str() + c + 1));
        item.resize(c);
    }
    sockaddr_in a{};
    if (item.empty() || !port || !resolve(item, port, a)) return r;
    r.check = AddressCheck::NoAnswer;
    const sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) return r;
    const std::vector<uint8_t> scan = lan_packet(kScan, nullptr, 0);
    for (int attempt = 0; attempt < 3 && r.check != AddressCheck::Found; attempt++) {  // UDP: a lost one is tried again
        sendto(s, reinterpret_cast<const char*>(scan.data()), int(scan.size()), 0, reinterpret_cast<const sockaddr*>(&a),
               sizeof a);
        uint8_t in[kLanMaxPacket];
        std::vector<uint8_t> body;
        uint8_t type;
        while (wait_socket(s, false, kScanMs)) {
            const int n = int(recv(s, reinterpret_cast<char*>(in), int(sizeof in), 0));
            if (n > 0 && lan_parse(in, size_t(n), type, body) && type == kScanResp && body.size() == kInfoSize) {
                r.check = AddressCheck::Found;
                r.island = ldn_island_name(body.data());
                break;
            }
        }
    }
    sock_close(s);
    return r;
}

}  // namespace

void address_check_start(const std::string& address) {
    int generation;
    {
        std::lock_guard lk(g_address_lock);
        generation = ++g_address_generation;
        g_address = {};
        g_address.check = address.empty() ? AddressCheck::Idle : AddressCheck::Running;
    }
    if (address.empty()) return;
    std::thread([address, generation] {
        const AddressResult r = check_address(address);
        std::lock_guard lk(g_address_lock);
        if (generation == g_address_generation) g_address = r;
    }).detach();
}

AddressResult address_check_result() {
    std::lock_guard lk(g_address_lock);
    return g_address;
}

}  // namespace nx
