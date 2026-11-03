#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nx::amiibo {

constexpr size_t kTagSize = 540;

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
    std::vector<uint8_t> plain;  // 532-byte internal plain layout (amiitool)
    AmiiboModelInfo model{};
    uint8_t uid[10]{};
    uint8_t uid_length = 0;
    uint32_t application_area_id = 0;
    bool app_area_initialized = false;
    std::vector<uint8_t> application_area;  // kApplicationAreaSize
};

// Decrypts a retail NTAG215 dump using key_retail.bin. Returns false if keys are missing or HMAC check fails.
bool decode_tag_dump(const uint8_t* tag, size_t tag_size, DecodedTag& out, std::string* err);

// True when decrypted plain layout is complete enough for nn::nfp read paths.
bool decoded_tag_valid(const DecodedTag& tag);

std::string keys_path();
bool master_keys_available();
// Copies a 160-byte retail key file (key_retail.bin, keys.bin, unfixed+locked) into keys_path() for later runs.
bool install_master_keys_file(const std::string& src_path, std::string* err);

}  // namespace nx::amiibo
