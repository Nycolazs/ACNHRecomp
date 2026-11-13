// ldn_mitm's LAN packet format (runtime/src/ldn_net.h): header, zero-run compression, bounds. Exit 0 when all pass.
#include <cstdio>
#include <vector>

#include "ldn_net.h"

using namespace nx;

static int g_failed = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_failed++;                                           \
        }                                                         \
    } while (0)

// A packet as ldn_mitm sends it compressed: header with the compressed flag and both lengths, then the body.
static std::vector<uint8_t> compressed(uint8_t type, const std::vector<uint8_t>& body, uint16_t raw_len) {
    std::vector<uint8_t> p = lan_packet(type, body.data(), body.size());
    p[5] = 1;
    std::memcpy(&p[8], &raw_len, 2);
    return p;
}

int main() {
    uint8_t type = 0xFF;
    std::vector<uint8_t> body;

    // The header: magic, type, uncompressed, length.
    const uint8_t data[3] = {1, 2, 3};
    std::vector<uint8_t> p = lan_packet(kSyncNetwork, data, 3);
    CHECK(p.size() == kLanHeaderSize + 3);
    CHECK(p[0] == 0x00 && p[1] == 0x14 && p[2] == 0x45 && p[3] == 0x11);  // 0x11451400, little-endian
    CHECK(p[4] == kSyncNetwork && p[5] == 0 && p[6] == 3 && p[7] == 0);
    CHECK(lan_parse(p.data(), p.size(), type, body) && type == kSyncNetwork && body == std::vector<uint8_t>(data, data + 3));

    // A Scan has no body.
    p = lan_packet(kScan, nullptr, 0);
    CHECK(p.size() == kLanHeaderSize && lan_parse(p.data(), p.size(), type, body) && type == kScan && body.empty());

    // Bad magic, short header, length past the end.
    p = lan_packet(kScan, data, 3);
    p[0] ^= 1;
    CHECK(!lan_parse(p.data(), p.size(), type, body));
    p = lan_packet(kScan, data, 3);
    CHECK(!lan_parse(p.data(), kLanHeaderSize - 1, type, body));
    CHECK(!lan_parse(p.data(), p.size() - 1, type, body));

    // Compression: a zero byte, then how many more zeros follow it.
    std::vector<uint8_t> out;
    const uint8_t z[] = {5, 0, 3, 7, 0, 0};  // 5, four zeros, 7, one zero
    CHECK(lan_decompress(z, sizeof z, 7, out) && out == (std::vector<uint8_t>{5, 0, 0, 0, 0, 7, 0}));
    CHECK(!lan_decompress(z, sizeof z, 8, out));        // not the announced size
    CHECK(!lan_decompress(z, sizeof z - 1, 6, out));    // a zero with its count cut off
    const uint8_t big[] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    CHECK(!lan_decompress(big, sizeof big, 9 * 256, out));  // past kLanMaxPacket
    p = compressed(kScanResp, std::vector<uint8_t>(z, z + sizeof z), 7);
    CHECK(lan_parse(p.data(), p.size(), type, body) && type == kScanResp && body.size() == 7);

    // A NetworkInfo-sized body survives a round trip.
    std::vector<uint8_t> info(kInfoSize);
    for (size_t i = 0; i < info.size(); i++) info[i] = uint8_t(i * 7);
    p = lan_packet(kScanResp, info.data(), info.size());
    CHECK(p.size() <= kLanMaxPacket && lan_parse(p.data(), p.size(), type, body) && body == info);

    if (!g_failed) std::printf("ldn_net: all passed\n");
    return g_failed ? 1 : 0;
}
