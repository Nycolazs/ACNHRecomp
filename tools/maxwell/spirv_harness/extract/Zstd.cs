// Zstandard decoder (RFC 8878), decode only, no dictionaries: the RomFS's .zs files (Layout/, Model/). .NET 10 has no
// zstd and a NuGet package would be one more thing to fetch; this is the format as the RFC describes it, checked
// against the reference decoder's output on every .zs file of the game. Content checksums are not verified.

using System;
using System.Buffers.Binary;
using System.Numerics;

namespace SpirvHarness.Extract
{
    sealed class ZstdException : Exception
    {
        public ZstdException(string m) : base("zstd: " + m) { }
    }

    sealed class Zstd
    {
        // Decodes every frame of `src` (skippable frames are skipped), concatenated, like `zstd -dc`.
        public static byte[] Decompress(ReadOnlySpan<byte> src)
        {
            // 8 bytes of slack: the bit readers load 8 bytes at a time and mask off what is past their stream.
            byte[] input = new byte[src.Length + 16];
            src.CopyTo(input);
            return new Zstd(input, src.Length).Run();
        }

        readonly byte[] _in;
        readonly int _len;
        byte[] _out;
        int _n;

        // State carried from block to block inside a frame (and reset per frame).
        Fse _ll, _of, _ml;
        Huf _huf;
        readonly int[] _rep = new int[3];
        readonly byte[] _lits = new byte[(1 << 17) + 64];

        Zstd(byte[] input, int len)
        {
            _in = input;
            _len = len;
            _out = new byte[Math.Max(64, len * 3)];
        }

        static ZstdException Bad(string m) => new ZstdException(m);

        uint U32(int p) => p + 4 <= _len ? BinaryPrimitives.ReadUInt32LittleEndian(_in.AsSpan(p)) : throw Bad("truncated");

        void Reserve(int k)
        {
            if (_n + k <= _out.Length) return;
            long want = Math.Max((long)_out.Length * 2, (long)_n + k);
            if (want > int.MaxValue - 64) throw Bad("output too large");
            Array.Resize(ref _out, (int)want);
        }

        byte[] Run()
        {
            int p = 0;
            while (p < _len)
            {
                uint magic = U32(p);
                if ((magic & 0xFFFFFFF0) == 0x184D2A50)
                {
                    long skip = U32(p + 4);
                    if (p + 8 + skip > _len) throw Bad("truncated skippable frame");
                    p += 8 + (int)skip;
                    continue;
                }
                if (magic != 0xFD2FB528) throw Bad("bad magic");
                p = Frame(p + 4);
            }
            Array.Resize(ref _out, _n);
            return _out;
        }

        int Frame(int p)
        {
            if (p >= _len) throw Bad("truncated frame header");
            int fhd = _in[p++];
            int fcsFlag = fhd >> 6, dictFlag = fhd & 3;
            bool single = (fhd & 0x20) != 0, checksum = (fhd & 4) != 0;
            if ((fhd & 8) != 0) throw Bad("reserved bit set");
            if (!single) p++;                                               // window descriptor
            p += dictFlag == 0 ? 0 : dictFlag == 1 ? 1 : dictFlag == 2 ? 2 : 4;
            int fcsSize = fcsFlag == 0 ? (single ? 1 : 0) : fcsFlag == 1 ? 2 : fcsFlag == 2 ? 4 : 8;
            if (p + fcsSize > _len) throw Bad("truncated frame header");
            ulong fcs = 0;
            for (int i = 0; i < fcsSize; i++) fcs |= (ulong)_in[p + i] << (8 * i);
            if (fcsSize == 2) fcs += 256;
            p += fcsSize;
            if (fcsSize > 0 && fcs < int.MaxValue) Reserve((int)fcs);

            _ll = _of = _ml = null;
            _huf = null;
            _rep[0] = 1; _rep[1] = 4; _rep[2] = 8;
            while (true)
            {
                if (p + 3 > _len) throw Bad("truncated block header");
                int bh = _in[p] | _in[p + 1] << 8 | _in[p + 2] << 16;
                p += 3;
                bool last = (bh & 1) != 0;
                int type = (bh >> 1) & 3, size = bh >> 3;
                switch (type)
                {
                    case 0:
                        if (p + size > _len) throw Bad("truncated raw block");
                        Reserve(size);
                        Buffer.BlockCopy(_in, p, _out, _n, size);
                        _n += size;
                        p += size;
                        break;
                    case 1:
                        if (p >= _len) throw Bad("truncated RLE block");
                        Reserve(size);
                        _out.AsSpan(_n, size).Fill(_in[p]);
                        _n += size;
                        p += 1;
                        break;
                    case 2:
                        if (p + size > _len) throw Bad("truncated block");
                        Block(p, p + size);
                        p += size;
                        break;
                    default:
                        throw Bad("reserved block type");
                }
                if (last) break;
            }
            if (checksum) p += 4;
            if (p > _len) throw Bad("truncated checksum");
            return p;
        }

        // ---- compressed block ----------------------------------------------------------------------------------

        void Block(int p, int end)
        {
            // Literals section.
            int b0 = _in[p];
            int ltype = b0 & 3, sf = (b0 >> 2) & 3;
            byte[] lsrc;
            int loff, nlit;
            if (ltype < 2)
            {
                int hs;
                if ((sf & 1) == 0) { nlit = b0 >> 3; hs = 1; }
                else if (sf == 1) { nlit = (b0 >> 4) | (_in[p + 1] << 4); hs = 2; }
                else { nlit = (b0 >> 4) | (_in[p + 1] << 4) | (_in[p + 2] << 12); hs = 3; }
                p += hs;
                if (ltype == 0)
                {
                    if (p + nlit > end) throw Bad("truncated raw literals");
                    lsrc = _in; loff = p; p += nlit;
                }
                else
                {
                    if (p >= end) throw Bad("truncated RLE literals");
                    if (nlit > _lits.Length) throw Bad("too many literals");
                    _lits.AsSpan(0, nlit).Fill(_in[p]);
                    lsrc = _lits; loff = 0; p += 1;
                }
            }
            else
            {
                int csize, hs;
                bool four = sf != 0;
                if (sf < 2)
                {
                    int c = _in[p] | _in[p + 1] << 8 | _in[p + 2] << 16;
                    nlit = (c >> 4) & 0x3FF; csize = (c >> 14) & 0x3FF; hs = 3;
                }
                else if (sf == 2)
                {
                    uint c = BinaryPrimitives.ReadUInt32LittleEndian(_in.AsSpan(p));
                    nlit = (int)((c >> 4) & 0x3FFF); csize = (int)((c >> 18) & 0x3FFF); hs = 4;
                }
                else
                {
                    ulong c = BinaryPrimitives.ReadUInt64LittleEndian(_in.AsSpan(p)) & 0xFFFFFFFFFF;
                    nlit = (int)((c >> 4) & 0x3FFFF); csize = (int)((c >> 22) & 0x3FFFF); hs = 5;
                }
                p += hs;
                if (p + csize > end) throw Bad("truncated compressed literals");
                if (nlit > _lits.Length - 8) throw Bad("too many literals");
                int q = p, qend = p + csize;
                if (ltype == 2) q += ReadHuf(q, qend);
                else if (_huf == null) throw Bad("treeless literals without a table");
                if (!four) DecodeHufStream(q, qend, 0, nlit);
                else
                {
                    if (q + 6 > qend) throw Bad("truncated jump table");
                    int s1 = _in[q] | _in[q + 1] << 8, s2 = _in[q + 2] | _in[q + 3] << 8, s3 = _in[q + 4] | _in[q + 5] << 8;
                    int a = q + 6, seg = (nlit + 3) / 4;
                    if (a + s1 + s2 + s3 > qend || seg * 3 > nlit) throw Bad("bad jump table");
                    DecodeHufStream(a, a + s1, 0, seg);
                    DecodeHufStream(a + s1, a + s1 + s2, seg, seg);
                    DecodeHufStream(a + s1 + s2, a + s1 + s2 + s3, 2 * seg, seg);
                    DecodeHufStream(a + s1 + s2 + s3, qend, 3 * seg, nlit - 3 * seg);
                }
                lsrc = _lits; loff = 0;
                p = qend;
            }

            // Sequences section.
            if (p >= end) throw Bad("truncated sequences header");
            int nseq = _in[p++];
            if (nseq >= 128)
            {
                if (nseq < 255) { nseq = ((nseq - 128) << 8) + _in[p++]; }
                else { nseq = _in[p] + (_in[p + 1] << 8) + 0x7F00; p += 2; }
            }
            Reserve(nlit);
            if (nseq == 0)
            {
                Buffer.BlockCopy(lsrc, loff, _out, _n, nlit);
                _n += nlit;
                return;
            }
            if (p >= end) throw Bad("truncated sequences header");
            int modes = _in[p++];
            _ll = Table((modes >> 6) & 3, ref p, end, _ll, LlDefault, 6, 35, 9);
            _of = Table((modes >> 4) & 3, ref p, end, _of, OfDefault, 5, 31, 8);
            _ml = Table((modes >> 2) & 3, ref p, end, _ml, MlDefault, 6, 52, 9);

            var br = new BackBits(_in, p, end);
            int lls = (int)br.Read(_ll.Log), ofs = (int)br.Read(_of.Log), mls = (int)br.Read(_ml.Log);
            int lit = loff, litEnd = loff + nlit;
            Fse ll = _ll, of = _of, ml = _ml;
            for (int i = 0; i < nseq; i++)
            {
                int ofc = of.Sym[ofs], llc = ll.Sym[lls], mlc = ml.Sym[mls];
                if (ofc > 31) throw Bad("bad offset code");
                long ofv = (1L << ofc) + (long)br.Read(ofc);
                int mlen = MlBase[mlc] + (int)br.Read(MlBits[mlc]);
                int llen = LlBase[llc] + (int)br.Read(LlBits[llc]);
                int offset;
                if (ofv > 3)
                {
                    offset = (int)Math.Min(ofv - 3, int.MaxValue);
                    _rep[2] = _rep[1]; _rep[1] = _rep[0]; _rep[0] = offset;
                }
                else
                {
                    int idx = (int)ofv - 1 + (llen == 0 ? 1 : 0);
                    if (idx == 0) offset = _rep[0];
                    else
                    {
                        offset = idx == 3 ? _rep[0] - 1 : _rep[idx];
                        if (idx > 1) _rep[2] = _rep[1];
                        _rep[1] = _rep[0];
                        _rep[0] = offset;
                    }
                }
                if (i != nseq - 1)
                {
                    lls = ll.Base[lls] + (int)br.Read(ll.Bits[lls]);
                    mls = ml.Base[mls] + (int)br.Read(ml.Bits[mls]);
                    ofs = of.Base[ofs] + (int)br.Read(of.Bits[ofs]);
                }
                // Execute: literals, then the match.
                if (llen > litEnd - lit) throw Bad("literals overrun");
                Reserve(llen + mlen);
                Buffer.BlockCopy(lsrc, lit, _out, _n, llen);
                _n += llen;
                lit += llen;
                if (offset <= 0 || offset > _n) throw Bad("offset past the start of the output");
                int from = _n - offset;
                if (offset >= mlen)
                {
                    Buffer.BlockCopy(_out, from, _out, _n, mlen);
                    _n += mlen;
                }
                else
                {
                    byte[] o = _out;
                    int to = _n;
                    for (int k = 0; k < mlen; k++) o[to + k] = o[from + k];
                    _n += mlen;
                }
            }
            int rest = litEnd - lit;
            Reserve(rest);
            Buffer.BlockCopy(lsrc, lit, _out, _n, rest);
            _n += rest;
        }

        Fse Table(int mode, ref int p, int end, Fse prev, short[] def, int defLog, int maxSym, int maxLog)
        {
            switch (mode)
            {
                case 0:
                    return def == LlDefault ? LlPre : def == OfDefault ? OfPre : MlPre;
                case 1:
                    if (p >= end) throw Bad("truncated RLE table");
                    {
                        int s = _in[p++];
                        if (s > maxSym) throw Bad("bad RLE symbol");
                        return new Fse { Log = 0, Sym = new[] { (byte)s }, Bits = new byte[1], Base = new ushort[1] };
                    }
                case 2:
                    {
                        short[] norm = new short[256];
                        p += ReadNCount(_in, p, end, norm, maxSym, out int ms, out int log);
                        if (log > maxLog) throw Bad("table log too large");
                        return Fse.Build(norm, ms, log);
                    }
                default:
                    return prev ?? throw Bad("repeat mode without a previous table");
            }
        }

        // ---- FSE -----------------------------------------------------------------------------------------------

        sealed class Fse
        {
            public int Log;
            public byte[] Sym, Bits;
            public ushort[] Base;

            public static Fse Build(short[] norm, int maxSym, int log)
            {
                int size = 1 << log, high = size - 1;
                var t = new Fse { Log = log, Sym = new byte[size], Bits = new byte[size], Base = new ushort[size] };
                int[] next = new int[maxSym + 1];
                for (int s = 0; s <= maxSym; s++)
                {
                    if (norm[s] == -1) { t.Sym[high--] = (byte)s; next[s] = 1; }
                    else next[s] = norm[s];
                }
                int step = (size >> 1) + (size >> 3) + 3, mask = size - 1, pos = 0;
                for (int s = 0; s <= maxSym; s++)
                    for (int i = 0; i < norm[s]; i++)
                    {
                        t.Sym[pos] = (byte)s;
                        do pos = (pos + step) & mask; while (pos > high);
                    }
                if (pos != 0) throw Bad("bad FSE table");
                for (int u = 0; u < size; u++)
                {
                    int s = t.Sym[u];
                    int ns = next[s]++;
                    int nb = log - (31 - BitOperations.LeadingZeroCount((uint)ns));
                    t.Bits[u] = (byte)nb;
                    t.Base[u] = (ushort)((ns << nb) - size);
                }
                return t;
            }
        }

        // FSE table description (normalized counts), forward little-endian bits. Returns the bytes it takes.
        static int ReadNCount(byte[] s, int start, int end, short[] norm, int maxSymAllowed, out int maxSym, out int log)
        {
            long bit = 0;
            uint Peek(int n)
            {
                ulong v = 0;
                long bi = start + (bit >> 3);
                for (int k = 0; k < 8; k++)
                    if (bi + k < end) v |= (ulong)s[bi + k] << (8 * k);
                return (uint)((v >> (int)(bit & 7)) & ((1UL << n) - 1));
            }
            if (start >= end) throw Bad("truncated FSE table");
            log = (int)Peek(4) + 5;
            bit += 4;
            if (log > 15) throw Bad("FSE table log too large");
            int remaining = (1 << log) + 1, threshold = 1 << log, nbBits = log + 1, sym = 0;
            bool prev0 = false;
            while (remaining > 1 && sym <= maxSymAllowed)
            {
                if (prev0)
                {
                    int n0 = sym;
                    while (true)
                    {
                        uint r = Peek(2);
                        bit += 2;
                        if (r == 3) { n0 += 3; continue; }
                        n0 += (int)r;
                        break;
                    }
                    if (n0 > maxSymAllowed) throw Bad("FSE table: too many symbols");
                    while (sym < n0) norm[sym++] = 0;
                }
                int max = (2 * threshold - 1) - remaining;
                uint v = Peek(nbBits);
                int count;
                if ((v & (uint)(threshold - 1)) < (uint)max)
                {
                    count = (int)(v & (uint)(threshold - 1));
                    bit += nbBits - 1;
                }
                else
                {
                    count = (int)(v & (uint)(2 * threshold - 1));
                    if (count >= threshold) count -= max;
                    bit += nbBits;
                }
                count--;
                remaining -= Math.Abs(count);
                norm[sym++] = (short)count;
                prev0 = count == 0;
                while (remaining < threshold) { nbBits--; threshold >>= 1; }
            }
            if (remaining != 1) throw Bad("bad FSE table");
            maxSym = sym - 1;
            int used = (int)((bit + 7) >> 3);
            if (start + used > end) throw Bad("truncated FSE table");
            return used;
        }

        // Backward bit stream: read from the end, the highest set bit of the last byte marks where the data starts.
        // Past the start it reads zeros (Overflow then says so).
        struct BackBits
        {
            readonly byte[] _d;
            readonly int _a;
            long _pos;   // bits left

            public BackBits(byte[] d, int a, int b)
            {
                if (b <= a || d[b - 1] == 0) throw Bad("bad bit stream end");
                _d = d;
                _a = a;
                _pos = (long)(b - 1 - a) * 8 + (31 - BitOperations.LeadingZeroCount(d[b - 1]));
            }

            public bool Overflow => _pos < 0;

            public ulong Peek(int n)
            {
                long lo = _pos - n;
                if (lo >= 0)
                {
                    ulong w = BinaryPrimitives.ReadUInt64LittleEndian(_d.AsSpan(_a + (int)(lo >> 3), 8));
                    return (w >> (int)(lo & 7)) & ((1UL << n) - 1);
                }
                if (_pos <= 0) return 0;
                ulong x = BinaryPrimitives.ReadUInt64LittleEndian(_d.AsSpan(_a, 8)) & ((1UL << (int)_pos) - 1);
                return x << (int)-lo;
            }

            public void Skip(int n) => _pos -= n;

            public ulong Read(int n)
            {
                if (n == 0) return 0;
                ulong v = Peek(n);
                _pos -= n;
                return v;
            }
        }

        // ---- Huffman literals ----------------------------------------------------------------------------------

        sealed class Huf
        {
            public int Log;
            public byte[] Sym, Len;
        }

        int ReadHuf(int p, int end)
        {
            if (p >= end) throw Bad("truncated Huffman tree");
            int h = _in[p];
            byte[] w = new byte[256];
            int nw, used;
            if (h < 128)
            {
                int a = p + 1, b = p + 1 + h;
                if (b > end) throw Bad("truncated Huffman tree");
                short[] norm = new short[256];
                int nc = ReadNCount(_in, a, b, norm, 255, out int ms, out int log);
                if (log > 6) throw Bad("Huffman weight table log too large");
                Fse t = Fse.Build(norm, ms, log);
                var br = new BackBits(_in, a + nc, b);
                int s1 = (int)br.Read(log), s2 = (int)br.Read(log);
                nw = 0;
                while (true)
                {
                    if (nw > 253) throw Bad("too many Huffman weights");
                    w[nw++] = t.Sym[s1];
                    s1 = t.Base[s1] + (int)br.Read(t.Bits[s1]);
                    if (br.Overflow) { w[nw++] = t.Sym[s2]; break; }
                    w[nw++] = t.Sym[s2];
                    s2 = t.Base[s2] + (int)br.Read(t.Bits[s2]);
                    if (br.Overflow) { w[nw++] = t.Sym[s1]; break; }
                }
                used = 1 + h;
            }
            else
            {
                nw = h - 127;
                int nb = (nw + 1) / 2;
                if (p + 1 + nb > end) throw Bad("truncated Huffman weights");
                for (int i = 0; i < nw; i++)
                {
                    int x = _in[p + 1 + i / 2];
                    w[i] = (byte)((i & 1) == 0 ? x >> 4 : x & 15);
                }
                used = 1 + nb;
            }
            long total = 0;
            for (int i = 0; i < nw; i++)
            {
                if (w[i] > 11) throw Bad("Huffman weight too large");
                if (w[i] > 0) total += 1L << (w[i] - 1);
            }
            if (total == 0) throw Bad("empty Huffman tree");
            int maxBits = 64 - BitOperations.LeadingZeroCount((ulong)total);   // highbit(total) + 1
            if (maxBits > 11) throw Bad("Huffman tree too deep");
            long rest = (1L << maxBits) - total;
            if ((rest & (rest - 1)) != 0) throw Bad("bad Huffman tree");
            w[nw] = (byte)(64 - BitOperations.LeadingZeroCount((ulong)rest));
            int nsym = nw + 1;
            int[] rank = new int[13];
            for (int i = 0; i < nsym; i++) rank[w[i]]++;
            int next = 0;
            for (int k = 1; k <= maxBits; k++)
            {
                int cur = next;
                next += rank[k] << (k - 1);
                rank[k] = cur;
            }
            var t2 = new Huf { Log = maxBits, Sym = new byte[1 << maxBits], Len = new byte[1 << maxBits] };
            for (int s = 0; s < nsym; s++)
            {
                int k = w[s];
                if (k == 0) continue;
                int len = 1 << (k - 1), at = rank[k];
                for (int j = 0; j < len; j++) { t2.Sym[at + j] = (byte)s; t2.Len[at + j] = (byte)(maxBits + 1 - k); }
                rank[k] += len;
            }
            _huf = t2;
            return used;
        }

        void DecodeHufStream(int a, int b, int dst, int count)
        {
            Huf h = _huf;
            var br = new BackBits(_in, a, b);
            int log = h.Log;
            byte[] sym = h.Sym, len = h.Len, o = _lits;
            for (int i = 0; i < count; i++)
            {
                int v = (int)br.Peek(log);
                o[dst + i] = sym[v];
                br.Skip(len[v]);
            }
        }

        // ---- predefined tables and code -> value maps ----------------------------------------------------------

        static readonly short[] LlDefault = { 4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1 };
        static readonly short[] MlDefault = { 1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1 };
        static readonly short[] OfDefault = { 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1 };
        static readonly Fse LlPre = Fse.Build(LlDefault, LlDefault.Length - 1, 6);
        static readonly Fse MlPre = Fse.Build(MlDefault, MlDefault.Length - 1, 6);
        static readonly Fse OfPre = Fse.Build(OfDefault, OfDefault.Length - 1, 5);

        static readonly int[] LlBase = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
        static readonly byte[] LlBits = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
        static readonly int[] MlBase = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539 };
        static readonly byte[] MlBits = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    }
}
