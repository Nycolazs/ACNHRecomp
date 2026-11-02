// UPnP: asks the router for local play's TCP port, so that nobody has to open it by hand, for the game (ldn_link.cpp)
// and the launcher's test. Direct play: only this computer and its router take part, no server.
#include "ldn_upnp.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "ldn_net.h"

namespace nx {

uint32_t local_address() {  // this computer's address towards the internet (no packet is sent)
    const sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    a.sin_addr.s_addr = htonl(0xC0000201);  // 192.0.2.1
    sockaddr_in me{};
    socklen_t len = sizeof me;
    const bool ok = !connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) &&
                    !getsockname(s, reinterpret_cast<sockaddr*>(&me), &len);
    sock_close(s);
    return ok ? ntohl(me.sin_addr.s_addr) : 0;
}

std::string ip_text(uint32_t ip) {
    char b[16];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

// "http://host[:port]/path" against a base: the parts.
void split_url(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
    size_t i = url.find("://");
    i = i == std::string::npos ? 0 : i + 3;
    const size_t j = url.find('/', i);
    std::string h = url.substr(i, j == std::string::npos ? std::string::npos : j - i);
    path = j == std::string::npos ? "/" : url.substr(j);
    port = 80;
    if (const size_t c = h.find(':'); c != std::string::npos) {
        port = uint16_t(std::atoi(h.c_str() + c + 1));
        h.resize(c);
    }
    host = h;
}

// One HTTP exchange with the router: the status line's code (0: none) and the body.
int http(const std::string& host, uint16_t port, const std::string& request, std::string& body) {
    sockaddr_in a{};
    if (!resolve(host, port, a)) return 0;
    const sock_t s = connect_within(a, 2000);
    if (s == kBadSock) return 0;
    set_timeout(s, 3000);
    std::string in;
    if (send_all(s, request.data(), request.size())) {
        char b[4096];
        for (int r; in.size() < (1u << 20) && (r = int(recv(s, b, sizeof b, 0))) > 0;) in.append(b, size_t(r));
    }
    sock_close(s);
    const size_t sp = in.find(' '), end = in.find("\r\n\r\n");
    if (sp == std::string::npos || end == std::string::npos) return 0;
    body = in.substr(end + 4);
    return std::atoi(in.c_str() + sp + 1);
}

// The text between <name> and </name>, from position from.
std::string xml_field(const std::string& x, const char* name, size_t from = 0) {
    const std::string open = std::string("<") + name + ">", close = std::string("</") + name + ">";
    const size_t i = x.find(open, from);
    if (i == std::string::npos) return "";
    const size_t j = x.find(close, i);
    return j == std::string::npos ? "" : x.substr(i + open.size(), j - i - open.size());
}

bool igd_at(const std::string& location, Igd& igd) {
    std::string path, body;
    if (location.rfind("http://", 0) != 0) return false;
    split_url(location, igd.host, igd.port, path);
    if (http(igd.host, igd.port, "GET " + path + " HTTP/1.0\r\nHost: " + igd.host + "\r\nConnection: close\r\n\r\n",
             body) != 200)
        return false;
    for (const char* svc : {":WANIPConnection:", ":WANPPPConnection:"}) {
        for (size_t i = body.find("<serviceType>"); i != std::string::npos; i = body.find("<serviceType>", i + 1)) {
            const std::string type = xml_field(body, "serviceType", i);
            if (type.find(svc) == std::string::npos) continue;
            std::string control = xml_field(body, "controlURL", i);
            if (control.empty()) continue;
            if (control.rfind("http://", 0) == 0) {
                std::string h;
                uint16_t pt;
                split_url(control, h, pt, control);
            } else if (control[0] != '/') {
                control = path.substr(0, path.rfind('/') + 1) + control;
            }
            igd.path = control;
            igd.service = type;
            return true;
        }
    }
    return false;
}

bool find_igd(Igd& igd) {
    const uint32_t lan = local_address();
    if (!lan) return false;
    igd.lan = ip_text(lan);
    const sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) return false;
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(lan);
    bind(s, reinterpret_cast<const sockaddr*>(&me), sizeof me);
    in_addr mif{};
    mif.s_addr = htonl(lan);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&mif), sizeof mif);
    sockaddr_in multicast{}, gateway{};
    multicast.sin_family = gateway.sin_family = AF_INET;
    multicast.sin_port = gateway.sin_port = htons(1900);
    multicast.sin_addr.s_addr = htonl(0xEFFFFFFA);  // 239.255.255.250
    gateway.sin_addr.s_addr = htonl((lan & 0xFFFFFF00u) | 1);  // the usual router, for those that ignore multicast
    for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                           "urn:schemas-upnp-org:device:InternetGatewayDevice:2"}) {
        const std::string m = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                          "MX: 2\r\nST: ") + st + "\r\n\r\n";
        sendto(s, m.data(), int(m.size()), 0, reinterpret_cast<const sockaddr*>(&multicast), sizeof multicast);
        sendto(s, m.data(), int(m.size()), 0, reinterpret_cast<const sockaddr*>(&gateway), sizeof gateway);
    }
    std::vector<std::string> seen;
    bool found = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!found && std::chrono::steady_clock::now() < end) {
        if (!wait_socket(s, false, 300)) continue;
        char b[2048];
        const int r = int(recv(s, b, sizeof b - 1, 0));
        if (r <= 0) continue;
        std::string reply(b, size_t(r));
        std::string lower = reply;
        for (char& c : lower) c = char(std::tolower(uint8_t(c)));
        const size_t i = lower.find("\r\nlocation:");
        if (i == std::string::npos) continue;
        size_t a = i + 11;
        const size_t e = reply.find("\r\n", a);
        while (a < reply.size() && reply[a] == ' ') a++;
        const std::string location = reply.substr(a, e == std::string::npos ? std::string::npos : e - a);
        bool known = false;
        for (const std::string& k : seen) known |= k == location;
        if (known) continue;
        seen.push_back(location);
        found = igd_at(location, igd);
    }
    sock_close(s);
    return found;
}

int soap(const Igd& igd, const char* action, const std::string& args, std::string& reply) {
    const std::string body = std::string("<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:") + action +
                             " xmlns:u=\"" + igd.service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>";
    const std::string req = "POST " + igd.path + " HTTP/1.0\r\nHost: " + igd.host + ":" + std::to_string(igd.port) +
                            "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + igd.service + "#" + action +
                            "\"\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return http(igd.host, igd.port, req, reply);
}

std::string mapping_key(uint16_t external) {
    return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(external) +
           "</NewExternalPort><NewProtocol>TCP</NewProtocol>";
}

bool map_port(const Igd& igd, uint16_t external, uint16_t internal, std::string& reply) {
    constexpr int kLeaseSeconds = 7200;
    return soap(igd, "AddPortMapping", mapping_key(external) + "<NewInternalPort>" + std::to_string(internal) +
                "</NewInternalPort><NewInternalClient>" + igd.lan + "</NewInternalClient><NewEnabled>1</NewEnabled>"
                "<NewPortMappingDescription>ACNHRecomp local play</NewPortMappingDescription><NewLeaseDuration>" +
                std::to_string(kLeaseSeconds) + "</NewLeaseDuration>", reply) == 200;
}

bool not_public(uint32_t ip) {  // the router itself is behind another NAT: no port it opens can be reached
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 22) == (100u << 2 | 1) || !ip;
}

bool upnp_find(Igd& igd) {
    net_init();
    return find_igd(igd);
}

LinkCheck upnp_open(const Igd& igd, uint16_t port, uint16_t& external, std::string& address) {
    std::string reply;
    external = 0;
    for (uint16_t p = port; p < port + 10 && !external; p++) {  // 718: this port is already another computer's or rule's
        if (map_port(igd, p, port, reply)) external = p;
        else if (reply.find("718") == std::string::npos) break;
    }
    if (!external) return LinkCheck::Refused;
    std::string ip;
    if (soap(igd, "GetExternalIPAddress", "", reply) == 200) ip = xml_field(reply, "NewExternalIPAddress");
    uint32_t a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || not_public(a << 24 | b << 16 | c << 8 | d)) {
        upnp_close(igd, external);
        return LinkCheck::NoPublicAddress;
    }
    address = ip + ":" + std::to_string(external);
    return LinkCheck::Open;
}

void upnp_renew(const Igd& igd, uint16_t external, uint16_t port) {
    std::string reply;
    map_port(igd, external, port, reply);
}

void upnp_close(const Igd& igd, uint16_t external) {
    std::string reply;
    soap(igd, "DeletePortMapping", mapping_key(external), reply);
}

// Opens the port for a moment with something listening and tries it from here through the public address (many routers
// do not loop that back: then it stays Open, not OpenVerified), then closes it.
LinkCheck upnp_check(uint16_t port, std::string& address) {
    Igd igd;
    if (!upnp_find(igd)) return LinkCheck::NoRouter;
    uint16_t external = 0;
    const LinkCheck r = upnp_open(igd, port, external, address);
    if (r != LinkCheck::Open) return r;
    bool verified = false;
    sockaddr_in me{}, pub{};
    me.sin_family = AF_INET;
    me.sin_port = htons(port);
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    const sock_t l = socket(AF_INET, SOCK_STREAM, 0);
    if (l != kBadSock && !bind(l, reinterpret_cast<sockaddr*>(&me), sizeof me) && !listen(l, 1) &&
        resolve(address.substr(0, address.find(':')), external, pub)) {
        const sock_t c = connect_within(pub, 2500);
        if (c != kBadSock) {
            verified = true;
            sock_close(c);
        }
    }
    if (l != kBadSock) sock_close(l);
    upnp_close(igd, external);
    return verified ? LinkCheck::OpenVerified : LinkCheck::Open;
}

// ---- a friend's address: asks there as a search does (ldn_discovery.cpp), and reads the island's name

namespace {

std::mutex g_address_lock;
AddressResult g_address;
int g_address_generation = 0;  // only the latest test's answer counts

// The island's name in the advertise data: UTF-16 at +0x1C, 10 characters at most (seen in the game's data), to UTF-8.
std::string island_name(const uint8_t* info) {
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

AddressResult check_address(std::string item) {
    AddressResult r;
    r.check = AddressCheck::Bad;
    net_init();
    uint16_t port = link_port();
    if (const size_t c = item.find(':'); c != std::string::npos) {
        port = uint16_t(std::atoi(item.c_str() + c + 1));
        item.resize(c);
    }
    sockaddr_in a{};
    if (item.empty() || !port || !resolve(item, port, a)) return r;
    r.check = AddressCheck::NoAnswer;
    const sock_t s = connect_within(a, kConnectMs * 2);
    if (s == kBadSock) return r;
    set_timeout(s, kHandshakeMs);
    const uint32_t size = uint32_t(kMagicSize);
    uint8_t h[5 + kMagicSize] = {kProbeFrame};
    std::memcpy(h + 1, &size, 4);
    std::memcpy(h + 5, kProbe, kMagicSize);
    uint8_t type = 0;
    std::vector<uint8_t> f;
    if (send_all(s, h, sizeof h) && read_frame(s, type, f) && type == kInfo && f.size() == kInfoSize) {
        r.check = AddressCheck::Found;
        r.island = island_name(f.data());
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
