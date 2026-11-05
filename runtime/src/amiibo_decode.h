#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace nx::amiibo {

constexpr size_t kTagSize = 540;
constexpr size_t kMinTagSize = 0x208;
constexpr size_t kPlainLayoutSize = 0x218;
constexpr size_t kApplicationAreaDataSize = 0xD8;

#pragma pack(push, 1)
struct AmiiboModelInfo {
    uint16_t character_id = 0;
    uint8_t character_variant = 0;
    uint8_t amiibo_type = 0;
    uint16_t model_number = 0;
    uint8_t series = 0;
    uint8_t constant_value = 0;
    uint8_t padding[4]{};
};
#pragma pack(pop)

struct DecodedTag {
    std::array<uint8_t, kPlainLayoutSize> plain{};  // Encrypted settings and app data are zeroed.
    AmiiboModelInfo model{};
    uint8_t uid[10]{};
    uint8_t uid_length = 0;
    uint32_t application_area_id = 0;
    bool app_area_initialized = false;
    std::array<uint8_t, kApplicationAreaDataSize> application_area{};
};

// Reads the unencrypted fields from a raw NTAG215 dump. Encrypted settings and application data are zeroed.
bool decode_tag_dump(const uint8_t* tag, size_t tag_size, DecodedTag& out, std::string* err);

// True when the clear tag fields are complete enough for nn::nfp read paths.
bool decoded_tag_valid(const DecodedTag& tag);

}  // namespace nx::amiibo
