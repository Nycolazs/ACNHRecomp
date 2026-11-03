// Retail Amiibo .bin (NTAG215) decryption. Keys are not distributed; place key_retail.bin (160 bytes) under
// <data root>/amiibo/ (unfixed-info.bin + locked-secret.bin concatenated), same as amiitool / emulators.
#include "amiibo_decode.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "platform.h"

namespace nx::amiibo {

namespace {

constexpr size_t kPlainSize = 0x218;
constexpr size_t kHmacPosData = 0x008;
constexpr size_t kHmacPosTag = 0x1B4;

constexpr size_t kOffAppArea = 0xDC;
constexpr size_t kOffAppAreaId = 0xB6;
constexpr size_t kOffUid = 0x1D4;
constexpr size_t kOffModel = 0x1DC;
constexpr size_t kOffSettings = 0x2C;
constexpr size_t kAppAreaBytes = 0xD8;

// 80-byte retail key block (unfixed-info / locked-secret), same layout as amiitool.
#pragma pack(push, 1)
struct MasterKeys {
    uint8_t hmac_key[16];
    char type_string[14];
    uint8_t unused{};
    uint8_t magic_bytes_size;
    uint8_t magic_bytes[16];
    uint8_t xor_pad[32];
};
static_assert(sizeof(MasterKeys) == 80);
struct DerivedKeys {
    uint8_t aes_key[16];
    uint8_t aes_iv[16];
    uint8_t hmac_key[16];
};
#pragma pack(pop)

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

struct HashCtx {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
};

bool hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len, uint8_t out[32]) {
    HashCtx ctx;
    if (BCryptOpenAlgorithmProvider(&ctx.alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
        return false;
    DWORD obj = 0, cb = 0;
    BCryptGetProperty(ctx.alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj, sizeof(obj), &cb, 0);
    std::vector<uint8_t> objbuf(obj);
    if (BCryptCreateHash(ctx.alg, &ctx.hash, objbuf.data(), obj, (PUCHAR)key, ULONG(key_len), 0) < 0) return false;
    BCryptHashData(ctx.hash, (PUCHAR)data, ULONG(len), 0);
    BCryptFinishHash(ctx.hash, out, 32, 0);
    BCryptDestroyHash(ctx.hash);
    BCryptCloseAlgorithmProvider(ctx.alg, 0);
    return true;
}

void drbg(const uint8_t* hmac_key, size_t hmac_key_len, const uint8_t* seed, size_t seed_len, uint8_t* out,
          size_t out_len) {
    uint8_t buf[512];
    size_t prep = 0;
    uint16_t counter = 0;
    size_t done = 0;
    while (done < out_len) {
        size_t pos = 0;
        buf[pos++] = uint8_t(counter >> 8);
        buf[pos++] = uint8_t(counter);
        memcpy(buf + pos, seed, seed_len);
        pos += seed_len;
        uint8_t block[32];
        hmac_sha256(hmac_key, hmac_key_len, buf, pos, block);
        size_t take = std::min(out_len - done, size_t(32));
        memcpy(out + done, block, take);
        done += take;
        counter++;
    }
}

// AES-128-CTR via ECB keystream (works on all Windows SDKs; BCRYPT_CHAIN_MODE_CTR is not always in headers).
bool aes128_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key_handle = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0) <
        0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    DWORD obj = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj, sizeof(obj), &cb, 0);
    std::vector<uint8_t> objbuf(obj);
    if (BCryptGenerateSymmetricKey(alg, &key_handle, objbuf.data(), obj, (PUCHAR)key, 16, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    uint8_t counter[16];
    memcpy(counter, iv, 16);
    bool ok = true;
    for (size_t off = 0; off < len && ok; ) {
        uint8_t keystream[16];
        ULONG ks = 0;
        ok = BCryptEncrypt(key_handle, counter, 16, nullptr, nullptr, 0, keystream, 16, &ks, 0) >= 0 && ks == 16;
        size_t n = std::min(len - off, size_t(16));
        for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ keystream[i];
        off += n;
        for (int i = 15; i >= 0; i--) {
            if (++counter[i] != 0) break;
        }
    }
    BCryptDestroyKey(key_handle);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

#define NX_AMIIBO_HAVE_CRYPTO 1

#elif defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#define NX_AMIIBO_HAVE_CRYPTO 1

bool hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len, uint8_t out[32]) {
    CCHmac(kCCHmacAlgSHA256, key, key_len, data, len, out);
    return true;
}

void drbg(const uint8_t* hmac_key, size_t hmac_key_len, const uint8_t* seed, size_t seed_len, uint8_t* out,
          size_t out_len) {
    uint8_t buf[512];
    uint16_t counter = 0;
    size_t done = 0;
    while (done < out_len) {
        size_t pos = 0;
        buf[pos++] = uint8_t(counter >> 8);
        buf[pos++] = uint8_t(counter);
        memcpy(buf + pos, seed, seed_len);
        pos += seed_len;
        uint8_t block[32];
        hmac_sha256(hmac_key, hmac_key_len, buf, pos, block);
        size_t take = std::min(out_len - done, size_t(32));
        memcpy(out + done, block, take);
        done += take;
        counter++;
    }
}

bool aes128_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len) {
    std::vector<uint8_t> ctr(16);
    memcpy(ctr.data(), iv, 16);
    size_t nc_off = 0;
    uint8_t stream_block[16];
    CCCryptorRef cryptor = nullptr;
    CCCryptorCreateWithMode(kCCDecrypt, kCCModeCTR, kCCAlgorithmAES, ccNoPadding, ctr.data(), key, 16, nullptr, 0, 0,
                            0, &cryptor);
    if (!cryptor) return false;
    size_t moved = 0;
    CCCryptorUpdate(cryptor, in, len, out, len, &moved);
    CCCryptorRelease(cryptor);
    return true;
}

#else
#define NX_AMIIBO_HAVE_CRYPTO 0
#endif

void calc_seed(const uint8_t* dump, uint8_t* key) {
    memcpy(key + 0x00, dump + 0x029, 0x02);
    memset(key + 0x02, 0x00, 0x0E);
    memcpy(key + 0x10, dump + 0x1D4, 0x08);
    memcpy(key + 0x18, dump + 0x1D4, 0x08);
    memcpy(key + 0x20, dump + 0x1E8, 0x20);
}

void prepare_seed(const MasterKeys& base, const uint8_t* base_seed, uint8_t* output, size_t* output_size) {
    uint8_t* start = output;
    // Same as amiitool memccpy(typeString, '\0', 14): no extra byte when the string fills all 14 chars.
    size_t i = 0;
    for (; i < sizeof(base.type_string); i++) {
        output[i] = base.type_string[i];
        if (base.type_string[i] == '\0') {
            i++;
            break;
        }
    }
    output += i;
    size_t leading = 16 - base.magic_bytes_size;
    memcpy(output, base_seed, leading);
    output += leading;
    memcpy(output, base.magic_bytes, base.magic_bytes_size);
    output += base.magic_bytes_size;
    memcpy(output, base_seed + 0x10, 16);
    output += 16;
    for (size_t i = 0; i < 32; i++) output[i] = base_seed[i + 32] ^ base.xor_pad[i];
    output += 32;
    *output_size = size_t(output - start);
}

void derive_keys(const MasterKeys& master, const uint8_t* internal, DerivedKeys& out) {
    uint8_t seed[64];
    calc_seed(internal, seed);
    uint8_t prepared[512];
    size_t prepared_size = 0;
    prepare_seed(master, seed, prepared, &prepared_size);
    drbg(master.hmac_key, sizeof(master.hmac_key), prepared, prepared_size, (uint8_t*)&out, sizeof(out));
}

void tag_to_internal(const uint8_t* tag, uint8_t* internal) {
    memcpy(internal + 0x000, tag + 0x008, 0x008);
    memcpy(internal + 0x008, tag + 0x080, 0x020);
    memcpy(internal + 0x028, tag + 0x010, 0x024);
    memcpy(internal + 0x04C, tag + 0x0A0, 0x168);
    memcpy(internal + 0x1B4, tag + 0x034, 0x020);
    memcpy(internal + 0x1D4, tag + 0x000, 0x008);
    memcpy(internal + 0x1DC, tag + 0x054, 0x02C);
}

void cipher(const DerivedKeys& keys, const uint8_t* in, uint8_t* out) {
    aes128_ctr(keys.aes_key, keys.aes_iv, in + 0x02C, out + 0x02C, 0x188);
    memcpy(out + 0x000, in + 0x000, 0x008);
    memcpy(out + 0x028, in + 0x028, 0x004);
    memcpy(out + 0x1D4, in + 0x1D4, 0x034);
}

constexpr size_t kMasterKeysFileBytes = 160;

bool keys_blob_valid(const uint8_t* blob, size_t size) {
    if (size != kMasterKeysFileBytes) return false;
    // magic_bytes_size lives at byte 31 and 111 in each 80-byte block (amiitool on-disk layout).
    return blob[31] <= 16 && blob[111] <= 16;
}

void keys_blob_to_master(const uint8_t* blob, MasterKeys& data, MasterKeys& tag) {
    memcpy(&data, blob, sizeof(data));
    memcpy(&tag, blob + sizeof(data), sizeof(tag));
}

bool load_master_keys(MasterKeys& data, MasterKeys& tag) {
    std::string path = keys_path();
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return false;
    uint8_t blob[kMasterKeysFileBytes];
    size_t n = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    if (n != sizeof(blob) || !keys_blob_valid(blob, n)) return false;
    keys_blob_to_master(blob, data, tag);
    return true;
}

void parse_plain(const uint8_t* plain, DecodedTag& out) {
    out.plain.assign(plain, plain + kPlainSize);
    const uint8_t* model = plain + kOffModel;
    // NTAG model block is big-endian; do not memcpy into host uint16_t fields.
    out.model.character_id = uint16_t(uint16_t(model[0]) << 8) | model[1];
    out.model.character_variant = model[2];
    out.model.amiibo_type = model[3];
    out.model.model_number = uint16_t(uint16_t(model[4]) << 8) | model[5];
    out.model.series = model[6];
    out.model.constant_value = model[7];
    memcpy(out.uid, plain + kOffUid, 7);
    out.uid_length = 7;
    out.application_area_id = (uint32_t(plain[kOffAppAreaId + 0]) << 24) | (uint32_t(plain[kOffAppAreaId + 1]) << 16) |
                              (uint32_t(plain[kOffAppAreaId + 2]) << 8) | uint32_t(plain[kOffAppAreaId + 3]);
    const uint8_t settings = plain[kOffSettings];
    out.app_area_initialized = (settings & (1u << 5)) != 0;  // appdata_initialized bit
    out.application_area.assign(plain + kOffAppArea, plain + kOffAppArea + kAppAreaBytes);
}

}  // namespace

bool decoded_tag_valid(const DecodedTag& tag) {
    if (tag.plain.size() < kPlainSize) return false;
    if (tag.uid_length < 7) return false;
    if (tag.model.character_id == 0) return false;
    return true;
}

std::string keys_path() { return plat::data_root() + "/amiibo/key_retail.bin"; }

bool master_keys_available() {
    MasterKeys data{}, tag{};
    return load_master_keys(data, tag);
}

bool install_master_keys_file(const std::string& src_path, std::string* err) {
    FILE* f = plat::fopen_utf8(src_path, "rb");
    if (!f) {
        if (err) *err = "Could not read the selected key file.";
        return false;
    }
    uint8_t blob[kMasterKeysFileBytes];
    size_t n = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    if (n != sizeof(blob)) {
        if (err)
            *err = "Key file must be exactly 160 bytes (got " + std::to_string(n) +
                   "). Use key_retail.bin or unfixed-info.bin + locked-secret.bin combined.";
        return false;
    }
    if (!keys_blob_valid(blob, n)) {
        if (err) *err = "This file does not look like a valid retail Amiibo key blob.";
        return false;
    }
    const std::string dest = keys_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dest).parent_path(), ec);
    FILE* out = plat::fopen_utf8(dest, "wb");
    if (!out) {
        if (err) *err = "Could not write keys to " + dest + ".";
        return false;
    }
    bool ok = fwrite(blob, 1, sizeof(blob), out) == sizeof(blob);
    fclose(out);
    if (!ok) {
        if (err) *err = "Could not save keys (disk full or permission denied).";
        return false;
    }
    return true;
}

bool decode_tag_dump(const uint8_t* tag, size_t tag_size, DecodedTag& out, std::string* err) {
    if (tag_size < 532) {
        if (err) *err = "File is too small for an Amiibo dump (expected at least 532 bytes).";
        return false;
    }
#if !NX_AMIIBO_HAVE_CRYPTO
    (void)tag;
    (void)out;
    if (err) *err = "Amiibo decryption is not available on this platform build.";
    return false;
#else
    MasterKeys data{}, tag_keys{};
    if (!load_master_keys(data, tag_keys)) {
        if (err)
            *err = "Amiibo keys are not set up. In Settings → Amiibo, choose your key file (160 bytes), or place "
                   "key_retail.bin in " +
                   plat::data_root() + "/amiibo/.";
        return false;
    }
    uint8_t internal[kPlainSize]{};
    tag_to_internal(tag, internal);
    DerivedKeys data_keys{}, tkeys{};
    derive_keys(data, internal, data_keys);
    derive_keys(tag_keys, internal, tkeys);
    uint8_t plain[kPlainSize]{};
    cipher(data_keys, internal, plain);
    // amiitool order: tag HMAC is written into plain first; data HMAC is over plain[0x029, 0x1DF) including it.
    hmac_sha256(tkeys.hmac_key, 16, plain + 0x1D4, 0x34, plain + kHmacPosTag);
    hmac_sha256(data_keys.hmac_key, 16, plain + 0x029, 0x1DF, plain + kHmacPosData);
    if (memcmp(plain + kHmacPosData, internal + kHmacPosData, 32) != 0 ||
        memcmp(plain + kHmacPosTag, internal + kHmacPosTag, 32) != 0) {
        if (err) *err = "Amiibo keys or dump data appear invalid (HMAC check failed).";
        return false;
    }
    parse_plain(plain, out);
    if (!decoded_tag_valid(out)) {
        if (err) *err = "Decrypted Amiibo data is incomplete or invalid.";
        return false;
    }
    return true;
#endif
}

}  // namespace nx::amiibo
