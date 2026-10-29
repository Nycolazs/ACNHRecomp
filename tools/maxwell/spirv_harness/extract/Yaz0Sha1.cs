// Yaz0 (tools/maxwell/yaz0.py with its C decoder yaz0_fast.c, same results on bad input) and SHA-1 (the 16-hex-digit
// content hashes that name the extracted files). SHA-1 is managed code so the native binary needs no OpenSSL / Android
// crypto provider.

using System;
using System.Buffers.Binary;
using System.Numerics;

namespace SpirvHarness.Extract
{
    static class Yaz0
    {
        public static bool Is(ReadOnlySpan<byte> d) => d.Length >= 4 && d[0] == 'Y' && d[1] == 'a' && d[2] == 'z' && d[3] == '0';

        public static byte[] Decompress(ReadOnlySpan<byte> data)
        {
            if (!Is(data)) throw new PyError("ValueError: not Yaz0");
            if (data.Length < 8) throw new PyError("struct.error: unpack requires a buffer of 4 bytes");
            uint size = BinaryPrimitives.ReadUInt32BigEndian(data.Slice(4));
            byte[] o = new byte[size];
            long n = Decode(data, o);
            if (n != size) throw new PyError($"ValueError: Yaz0: decoded {n} of {size} bytes");
            return o;
        }

        // yaz0_fast.c, line for line.
        static long Decode(ReadOnlySpan<byte> inp, byte[] o)
        {
            long n = inp.Length, size = o.Length, src = 16, dst = 0;
            while (dst < size && src < n)
            {
                byte flags = inp[(int)src++];
                for (int bit = 7; bit >= 0 && dst < size; bit--)
                {
                    if (((flags >> bit) & 1) != 0)
                    {
                        if (src >= n) return dst;
                        o[dst++] = inp[(int)src++];
                    }
                    else
                    {
                        if (src + 1 >= n) return dst;
                        byte b0 = inp[(int)src], b1 = inp[(int)src + 1];
                        src += 2;
                        long dist = (((long)(b0 & 0xF) << 8) | b1) + 1, len = b0 >> 4;
                        if (len == 0)
                        {
                            if (src >= n) return dst;
                            len = inp[(int)src++] + 0x12;
                        }
                        else len += 2;
                        if (dist > dst) return -1;
                        for (; len != 0 && dst < size; len--, dst++) o[dst] = o[dst - dist];
                    }
                }
            }
            return dst;
        }
    }

    static class Sha1
    {
        // The first 8 bytes of the digest as 16 lowercase hex digits (hashlib.sha1(x).hexdigest()[:16]).
        public static string Hex16(ReadOnlySpan<byte> data)
        {
            Span<byte> h = stackalloc byte[20];
            Hash(data, h);
            return Convert.ToHexStringLower(h.Slice(0, 8));
        }

        public static void Hash(ReadOnlySpan<byte> data, Span<byte> digest)
        {
            uint h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
            Span<uint> w = stackalloc uint[80];
            int full = data.Length & ~63;
            for (int off = 0; off < full; off += 64) Block(data.Slice(off, 64), w, ref h0, ref h1, ref h2, ref h3, ref h4);
            Span<byte> tail = stackalloc byte[128];
            tail.Clear();
            int rem = data.Length - full;
            data.Slice(full).CopyTo(tail);
            tail[rem] = 0x80;
            int tlen = rem + 9 <= 64 ? 64 : 128;
            BinaryPrimitives.WriteUInt64BigEndian(tail.Slice(tlen - 8), (ulong)data.Length * 8);
            for (int off = 0; off < tlen; off += 64) Block(tail.Slice(off, 64), w, ref h0, ref h1, ref h2, ref h3, ref h4);
            BinaryPrimitives.WriteUInt32BigEndian(digest, h0);
            BinaryPrimitives.WriteUInt32BigEndian(digest.Slice(4), h1);
            BinaryPrimitives.WriteUInt32BigEndian(digest.Slice(8), h2);
            BinaryPrimitives.WriteUInt32BigEndian(digest.Slice(12), h3);
            BinaryPrimitives.WriteUInt32BigEndian(digest.Slice(16), h4);
        }

        static void Block(ReadOnlySpan<byte> b, Span<uint> w, ref uint h0, ref uint h1, ref uint h2, ref uint h3, ref uint h4)
        {
            for (int i = 0; i < 16; i++) w[i] = BinaryPrimitives.ReadUInt32BigEndian(b.Slice(i * 4));
            for (int i = 16; i < 80; i++) w[i] = BitOperations.RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            uint a = h0, bb = h1, c = h2, d = h3, e = h4;
            for (int i = 0; i < 20; i++)
            {
                uint t = BitOperations.RotateLeft(a, 5) + ((bb & c) | (~bb & d)) + e + 0x5A827999 + w[i];
                e = d; d = c; c = BitOperations.RotateLeft(bb, 30); bb = a; a = t;
            }
            for (int i = 20; i < 40; i++)
            {
                uint t = BitOperations.RotateLeft(a, 5) + (bb ^ c ^ d) + e + 0x6ED9EBA1 + w[i];
                e = d; d = c; c = BitOperations.RotateLeft(bb, 30); bb = a; a = t;
            }
            for (int i = 40; i < 60; i++)
            {
                uint t = BitOperations.RotateLeft(a, 5) + ((bb & c) | (bb & d) | (c & d)) + e + 0x8F1BBCDC + w[i];
                e = d; d = c; c = BitOperations.RotateLeft(bb, 30); bb = a; a = t;
            }
            for (int i = 60; i < 80; i++)
            {
                uint t = BitOperations.RotateLeft(a, 5) + (bb ^ c ^ d) + e + 0xCA62C1D6 + w[i];
                e = d; d = c; c = BitOperations.RotateLeft(bb, 30); bb = a; a = t;
            }
            h0 += a; h1 += bb; h2 += c; h3 += d; h4 += e;
        }
    }
}
