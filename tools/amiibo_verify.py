#!/usr/bin/env python3
"""Verify retail Amiibo .bin + key_retail.bin (160 bytes). Same HMAC layout as amiitool / runtime amiibo_decode."""
from __future__ import annotations

import hashlib
import hmac
import struct
import sys
from pathlib import Path

PLAIN_SIZE = 0x218
HMAC_DATA = 0x008
HMAC_TAG = 0x1B4


def drbg(hmac_key: bytes, seed: bytes, out_len: int) -> bytes:
    out = bytearray()
    counter = 0
    while len(out) < out_len:
        block = hmac.new(hmac_key, struct.pack(">H", counter) + seed, hashlib.sha256).digest()
        out.extend(block)
        counter += 1
    return bytes(out[:out_len])


def calc_seed(dump_internal: bytes) -> bytes:
    key = bytearray(48)
    key[0:2] = dump_internal[0x029:0x02B]
    key[0x10:0x18] = dump_internal[0x1D4:0x1DC]
    key[0x18:0x20] = dump_internal[0x1D4:0x1DC]
    key[0x20:0x40] = dump_internal[0x1E8:0x208]
    return bytes(key)


def prepare_seed(master: bytes, base_seed: bytes) -> bytes:
    hmac_key = master[0:16]
    type_string = master[16:30]
    magic_size = master[31]
    magic = master[32:48]
    xor_pad = master[48:80]
    i = type_string.find(b"\x00")
    if i < 0:
        i = 14
    else:
        i += 1
    out = bytearray()
    out.extend(type_string[:i])
    leading = 16 - magic_size
    out.extend(base_seed[:leading])
    out.extend(magic[:magic_size])
    out.extend(base_seed[0x10:0x20])
    for j in range(32):
        out.append(base_seed[0x20 + j] ^ xor_pad[j])
    return bytes(out)


def derive_keys(master: bytes, internal: bytes) -> tuple[bytes, bytes, bytes]:
    seed = calc_seed(internal)
    prepared = prepare_seed(master, seed)
    material = drbg(master[0:16], prepared, 48)
    return material[0:16], material[16:32], material[32:48]


def tag_to_internal(tag: bytes) -> bytearray:
    internal = bytearray(PLAIN_SIZE)
    internal[0x000:0x008] = tag[0x008:0x010]
    internal[0x008:0x028] = tag[0x080:0x0A0]
    internal[0x028:0x04C] = tag[0x010:0x034]
    internal[0x04C:0x1B4] = tag[0x0A0:0x208]
    internal[0x1B4:0x1D4] = tag[0x034:0x054]
    internal[0x1D4:0x1DC] = tag[0x000:0x008]
    internal[0x1DC:0x208] = tag[0x054:0x080]
    return internal


def aes128_ctr(key: bytes, iv: bytes, data: bytes) -> bytes:
    from Crypto.Cipher import AES

    ctr = int.from_bytes(iv, "big")
    out = bytearray()
    for off in range(0, len(data), 16):
        ks = AES.new(key, AES.MODE_ECB).encrypt(ctr.to_bytes(16, "big"))
        chunk = data[off : off + 16]
        out.extend(bytes(a ^ b for a, b in zip(chunk, ks)))
        ctr += 1
    return bytes(out)


def decrypt_tag(tag: bytes, keys160: bytes) -> tuple[bytearray, dict]:
    data_master, tag_master = keys160[0:80], keys160[80:160]
    internal = tag_to_internal(tag)
    d_aes, d_iv, d_hmac = derive_keys(data_master, internal)
    t_aes, t_iv, t_hmac = derive_keys(tag_master, internal)
    plain = bytearray(internal)
    plain[0x02C:0x1B4] = aes128_ctr(d_aes, d_iv, internal[0x02C:0x1B4])
    plain[0x000:0x008] = internal[0x000:0x008]
    plain[0x028:0x02C] = internal[0x028:0x02C]
    plain[0x1D4:0x208] = internal[0x1D4:0x208]
    plain[HMAC_TAG : HMAC_TAG + 32] = hmac.new(t_hmac, plain[0x1D4:0x208], hashlib.sha256).digest()
    plain[HMAC_DATA : HMAC_DATA + 32] = hmac.new(d_hmac, plain[0x029:0x208], hashlib.sha256).digest()
    if plain[HMAC_DATA : HMAC_DATA + 32] != internal[HMAC_DATA : HMAC_DATA + 32]:
        raise ValueError("data HMAC mismatch (wrong keys or corrupt dump)")
    if plain[HMAC_TAG : HMAC_TAG + 32] != internal[HMAC_TAG : HMAC_TAG + 32]:
        raise ValueError("tag HMAC mismatch (wrong keys or corrupt dump)")
    model = plain[0x1DC:0x1E4]
    char_id = (model[0] << 8) | model[1]
    info = {
        "character_id": char_id,
        "variant": model[2],
        "series": model[6],
        "model_number": struct.unpack(">H", model[4:6])[0],
        "access_id": int.from_bytes(plain[0xB6:0xBA], "big"),
        "app_initialized": bool(plain[0x2C] & (1 << 5)),
    }
    return plain, info


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <key_retail.bin> <amiibo.bin>", file=sys.stderr)
        return 2
    keys = Path(sys.argv[1]).read_bytes()
    tag = Path(sys.argv[2]).read_bytes()
    if len(keys) != 160:
        print(f"Expected 160-byte keys, got {len(keys)}", file=sys.stderr)
        return 1
    if len(tag) < 532:
        print(f"Expected >=532-byte tag dump, got {len(tag)}", file=sys.stderr)
        return 1
    try:
        from Crypto.Cipher import AES  # noqa: F401
    except ImportError:
        print("Install pycryptodome: pip install pycryptodome", file=sys.stderr)
        return 1
    plain, info = decrypt_tag(tag, keys)
    print("OK: HMAC verified")
    print(
        f"  character_id=0x{info['character_id']:04X} variant={info['variant']} "
        f"series={info['series']} model_number={info['model_number']}"
    )
    print(f"  access_id=0x{info['access_id']:08X} app_area_initialized={info['app_initialized']}")
    print(f"  uid={plain[0x1D4:0x1DB].hex()} length=7")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
