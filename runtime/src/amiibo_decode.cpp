#include "amiibo_decode.h"

#include <cstring>

namespace nx::amiibo {

namespace {

constexpr size_t kTagHeaderOffset = 0x010;
constexpr size_t kTagUidOffset = 0x000;
constexpr size_t kTagModelOffset = 0x054;
constexpr size_t kAmiiboIdEntrySize = 0x02C;

constexpr size_t kOffUid = 0x1D4;
constexpr size_t kOffModel = 0x1DC;
void copy_clear_fields(const uint8_t* tag, uint8_t* plain) {
    // Keep the NFP internal layout, but copy only fields that are clear in the raw dump.
    memcpy(plain + 0x000, tag + 0x008, 0x008);
    memcpy(plain + 0x008, tag + 0x080, 0x020);
    memcpy(plain + 0x028, tag + kTagHeaderOffset, 0x004);
    memcpy(plain + 0x1B4, tag + 0x034, 0x020);
    memcpy(plain + kOffUid, tag + kTagUidOffset, 0x008);
    memcpy(plain + kOffModel, tag + kTagModelOffset, kAmiiboIdEntrySize);
    // HMACs are preserved as stored; this keyless reader does not authenticate them.
}

void parse_model_info(const uint8_t* model, DecodedTag& out) {
    out.model.character_id = uint16_t(uint16_t(model[0]) << 8) | model[1];
    out.model.character_variant = model[2];
    out.model.amiibo_type = model[3];
    out.model.model_number = uint16_t(uint16_t(model[4]) << 8) | model[5];
    out.model.series = model[6];
    out.model.constant_value = model[7];
}

}  // namespace

bool decoded_tag_valid(const DecodedTag& tag) {
    return tag.uid_length == 7 && tag.model.character_id != 0;
}

bool decode_tag_dump(const uint8_t* tag, size_t tag_size, DecodedTag& out, std::string* err) {
    if (err) err->clear();
    if (!tag || tag_size < kMinTagSize) return false;
    if (tag[kTagHeaderOffset] != 0xA5 || tag[kTagUidOffset] != 0x04) return false;

    DecodedTag decoded{};
    copy_clear_fields(tag, decoded.plain.data());

    const uint8_t* model = tag + kTagModelOffset;
    parse_model_info(model, decoded);
    if (decoded.model.character_id == 0) return false;

    memcpy(decoded.uid, tag + kTagUidOffset, 7);
    decoded.uid_length = 7;
    decoded.application_area_id = 0;
    decoded.app_area_initialized = false;

    if (!decoded_tag_valid(decoded)) return false;
    out = decoded;
    return true;
}

}  // namespace nx::amiibo
