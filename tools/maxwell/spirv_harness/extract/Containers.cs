// Switch shader containers, ported from tools/maxwell/containers.py (the parts extract_shaders.py runs: open_blob, the
// SARC / FSHA / BNSH / VFXB / BAHS / BYML / sample-pair parsers, describe and extract --unique). Same ids, hashes,
// manifest lines and log text; docs/nvn/SHADERS.md has the layouts.
//
// Reads follow Python's semantics on purpose, since what is extracted depends on them: struct reads past the end
// raise (a negative offset counts from the end, as struct.unpack_from does), slices clamp, and any error drops the
// whole file, as the Python's uncaught exception does.

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace SpirvHarness.Extract
{
    // An error the Python would raise (struct.error, ValueError, IndexError...): the file is not extracted.
    sealed class PyError : Exception
    {
        public PyError(string m) : base(m) { }
    }

    static class Py
    {
        // u64 values are offsets; anything this large is past any file (and adding a few of them cannot overflow).
        const long Huge = 1L << 60;

        static int At(byte[] d, long o, int n)
        {
            if (o < 0)
            {
                if (o + d.Length < 0) throw new PyError($"struct.error: offset {o} out of range for {d.Length}-byte buffer");
                o += d.Length;
            }
            if (o + n > d.Length) throw new PyError($"struct.error: unpack_from requires a buffer of at least {o + n} bytes");
            return (int)o;
        }

        public static uint U16(byte[] d, long o) => BinaryPrimitives.ReadUInt16LittleEndian(d.AsSpan(At(d, o, 2)));
        public static uint U32(byte[] d, long o) => BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(At(d, o, 4)));
        public static int S32(byte[] d, long o) => BinaryPrimitives.ReadInt32LittleEndian(d.AsSpan(At(d, o, 4)));

        public static long U64(byte[] d, long o)
        {
            ulong v = BinaryPrimitives.ReadUInt64LittleEndian(d.AsSpan(At(d, o, 8)));
            return v > Huge ? Huge : (long)v;
        }

        public static byte Byte(byte[] d, long i)
        {
            if (i < 0) i += d.Length;
            if (i < 0 || i >= d.Length) throw new PyError("IndexError: index out of range");
            return d[i];
        }

        static long Clamp(long x, long len) => x < 0 ? Math.Max(0, x + len) : Math.Min(x, len);

        // d[a:b]
        public static (int Start, int Length) SliceRange(byte[] d, long a, long b)
        {
            long s = Clamp(a, d.Length), e = Clamp(b, d.Length);
            return ((int)s, (int)Math.Max(0, e - s));
        }

        public static ReadOnlySpan<byte> Slice(byte[] d, long a, long b)
        {
            var (s, n) = SliceRange(d, a, b);
            return new ReadOnlySpan<byte>(d, s, n);
        }

        public static bool Eq(ReadOnlySpan<byte> s, string ascii)
        {
            if (s.Length != ascii.Length) return false;
            for (int i = 0; i < s.Length; i++) if (s[i] != ascii[i]) return false;
            return true;
        }

        public static bool Any(ReadOnlySpan<byte> s) => s.IndexOfAnyExcept((byte)0) >= 0;

        // bytes.isalnum(): non-empty, ASCII letters and digits only.
        public static bool IsAlnum(ReadOnlySpan<byte> s)
        {
            if (s.Length == 0) return false;
            foreach (byte b in s)
                if (!(b >= '0' && b <= '9' || b >= 'a' && b <= 'z' || b >= 'A' && b <= 'Z')) return false;
            return true;
        }

        public static string Utf8(ReadOnlySpan<byte> s) => Encoding.UTF8.GetString(s);

        // nn::util BinString: u16 length, then the characters.
        public static string BinStr(byte[] d, long o)
        {
            long n = U16(d, o);
            return Utf8(Slice(d, o + 2, o + 2 + n));
        }

        public static string CStr(byte[] d, long o)
        {
            var s = Slice(d, o, o + 256);
            int end = s.IndexOf((byte)0);
            if (end < 0) throw new PyError("ValueError: subsection not found");
            return Utf8(s.Slice(0, end));
        }

        // f"{x:#x}"
        public static string H(long x) => x < 0 ? "-0x" + (-x).ToString("x") : "0x" + x.ToString("x");
    }

    sealed class Stage
    {
        public string Id, Kind, Hash;
        public long CodeOff, CodeSize, CtrlOff, CtrlSize, Start, Length, ConstOff, ConstSize;
        public byte[] Data;
        public List<string> Problems;

        public ReadOnlySpan<byte> Code => Py.Slice(Data, CodeOff, CodeOff + CodeSize);
        public ReadOnlySpan<byte> Ctrl => Py.Slice(Data, CtrlOff, CtrlOff + CtrlSize);
    }

    sealed class Container
    {
        public string Kind, Path;
        public long Off, Size;
        public List<(string Name, List<Stage> Stages)> Programs = new();
        public int Variations, Named;
        public List<string> Info = new();
        public List<Container> Children = new();

        public IEnumerable<Container> Walk()
        {
            yield return this;
            foreach (Container c in Children)
                foreach (Container x in c.Walk()) yield return x;
        }

        public IEnumerable<Stage> AllStages() => Walk().SelectMany(x => x.Programs.SelectMany(p => p.Stages));
    }

    // One parser per file: it caches the code-block hashes (variations share blocks; the Python hashes each stage).
    sealed class ContainerParser
    {
        static readonly string[] Stages = { "vertex", "hull", "domain", "geometry", "pixel", "compute" };
        const uint CodeMagicGraphics = 0x12345678, CodeMagicCompute = 0x12345679, ControlMagic = 0x98761234;

        static string SphType(long t) => t switch { 1 => "vertex", 2 => "hull", 3 => "domain", 4 => "geometry", 5 => "pixel", _ => null };

        readonly Dictionary<(byte[], int, int), string> _hashes = new();

        static uint U16(byte[] d, long o) => Py.U16(d, o);
        static uint U32(byte[] d, long o) => Py.U32(d, o);
        static int S32(byte[] d, long o) => Py.S32(d, o);
        static long U64(byte[] d, long o) => Py.U64(d, o);
        static string H(long x) => Py.H(x);

        Stage NewStage(string sid, string kind, long codeOff, long codeSize, long ctrlOff, long ctrlSize, byte[] data)
        {
            var s = new Stage { Id = sid, Kind = kind, CodeOff = codeOff, CodeSize = codeSize, CtrlOff = ctrlOff, CtrlSize = ctrlSize, Data = data };
            var (cs, cn) = Py.SliceRange(data, codeOff, codeOff + codeSize);
            if (!_hashes.TryGetValue((data, cs, cn), out s.Hash))
                _hashes[(data, cs, cn)] = s.Hash = Sha1.Hex16(new ReadOnlySpan<byte>(data, cs, cn));
            var (ts, tn) = Py.SliceRange(data, ctrlOff, ctrlOff + ctrlSize);
            if (ctrlSize >= 0x710)
            {
                if (tn < 0x70C) throw new PyError("struct.error: control section cut short");
                s.Start = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(ts + 0x708));
                s.Length = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(ts + 0x6F8));
                s.ConstOff = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(ts + 0x700));
                s.ConstSize = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(ts + 0x6FC));
            }
            s.Problems = CheckStage(s, data);
            return s;
        }

        static List<string> CheckStage(Stage st, byte[] data)
        {
            var bad = new List<string>();
            if (st.CodeOff + st.CodeSize > data.Length || st.CtrlOff + st.CtrlSize > data.Length) return new List<string> { "out of file" };
            uint magic = U32(data, st.CodeOff);
            uint want = st.Kind == "compute" ? CodeMagicCompute : CodeMagicGraphics;
            bool headerless = st.Kind == "compute" && st.Start == 0 && st.CtrlSize >= 0x710;
            if (magic != want && !headerless) bad.Add($"code magic {H(magic)}");
            if (U32(data, st.CtrlOff) != ControlMagic) bad.Add("control magic");
            else if (st.CtrlSize >= 0x710)
            {
                long c = st.CodeOff, end = st.Start + st.Length;
                if (U32(data, st.CtrlOff + 0x704) != st.CodeSize) bad.Add("ctrl+0x704 != code size");
                if (Py.Any(Py.Slice(data, c + 4, c + st.Start))) bad.Add("header not zero");
                if (st.ConstOff != ((end + 0xFF) & ~0xFFL) || st.CodeSize != ((st.ConstOff + st.ConstSize + 0xFF) & ~0xFFL))
                    bad.Add("program / constants / size do not line up");
                else if (Py.Any(Py.Slice(data, c + end, c + st.ConstOff)) || Py.Any(Py.Slice(data, c + st.ConstOff + st.ConstSize, c + st.CodeSize)))
                    bad.Add("padding not zero");
            }
            if (st.Kind != "compute")
            {
                long sph = U32(data, st.CodeOff + 0x30);
                if (SphType((sph >> 10) & 0xF) != st.Kind) bad.Add($"SPH type {(sph >> 10) & 0xF}");
            }
            return bad;
        }

        // ---- nn::util ResDic / RelocationTable -----------------------------------------------------------------

        static List<string> ResDic(byte[] d, long b, long off)
        {
            var keys = new List<string>();
            if (off == 0) return keys;
            long o = b + off;
            var sig = Py.Slice(d, o, o + 4);
            if (!(Py.Eq(sig, "_DIC") || Py.Eq(sig, "\0\0\0\0")) || S32(d, o + 8) != -1)
                throw new PyError($"ValueError: no dictionary at {H(o)}");
            long n = U32(d, o + 4);
            for (long k = 1; k < n + 1; k++) keys.Add(Py.BinStr(d, b + U64(d, o + 8 + k * 0x10 + 8)));
            return keys;
        }

        static (int Sections, long Pointers) Rlt(byte[] d, long b, long off)
        {
            long o = b + off;
            if (!Py.Eq(Py.Slice(d, o, o + 4), "_RLT")) throw new PyError($"ValueError: no _RLT at {H(o)}");
            long nsec = U32(d, o + 8);
            var sections = new List<(long First, long Count)>();
            for (long k = 0; k < nsec; k++)
            {
                long e = o + 0x10 + k * 0x18;
                U64(d, e);   // "<QIIII": from +0 to +0x18
                sections.Add((U32(d, e + 0x10), U32(d, e + 0x14)));
            }
            long entries = o + 0x10 + nsec * 0x18, ptrs = 0;
            foreach (var (first, count) in sections)
                for (long k = first; k < first + count; k++)
                {
                    long e = entries + k * 8;
                    U64(d, e);   // "<IHBB": the whole 8-byte entry is unpacked
                    long structs = U16(d, e + 4), n = Py.Byte(d, e + 6);
                    ptrs += structs * n;
                }
            return ((int)nsec, ptrs);
        }

        // ---- BNSH ----------------------------------------------------------------------------------------------

        Container ParseBnsh(byte[] d, long b, string path)
        {
            if (!Py.Eq(Py.Slice(d, b, b + 8), "BNSH\0\0\0\0")) throw new PyError($"ValueError: no BNSH at {H(b)}");
            long ver = U32(d, b + 8);
            long firstBlock = U16(d, b + 0x16);
            long rltOff = U32(d, b + 0x18);
            long size = U32(d, b + 0x1C);
            var c = new Container { Kind = "BNSH", Path = path, Off = b, Size = size };
            long g = b + firstBlock;
            if (!Py.Eq(Py.Slice(d, g, g + 4), "grsc")) throw new PyError($"ValueError: no grsc block at {H(g)}");
            long api = U16(d, g + 0x10), apiVer = U16(d, g + 0x12);
            long codeType = Py.Byte(d, g + 0x14);
            long compiler = U32(d, g + 0x18);
            long nvar = U32(d, g + 0x1C);
            long varArray = U64(d, g + 0x20);
            long pool = U64(d, g + 0x28);
            long poolProp = U32(d, b + pool), poolSize = U32(d, b + pool + 4), poolOff = U64(d, b + pool + 8);
            var (nsec, nptrs) = Rlt(d, b, rltOff);
            c.Info.Add($"version {ver >> 16}.{(ver >> 8) & 0xFF}.{ver & 0xFF}, api {api} (4 = NVN) v{apiVer}, " +
                       $"code type {codeType}, compiler {H(compiler)}, {nvar} variations");
            c.Info.Add($"code pool {H(poolOff)}..{H(poolOff + poolSize)} ({H(poolSize)} bytes, property {H(poolProp)}); " +
                       $"_RLT at {H(rltOff)}: {nsec} sections, {nptrs} pointers");
            for (long k = 0; k < nvar; k++)
            {
                long binary = U64(d, b + varArray + k * 0x40 + 0x10);
                string vn = "v" + k.ToString("D5");
                var stages = new List<Stage>();
                if (binary == 0)
                {
                    c.Programs.Add((vn, stages));
                    continue;
                }
                c.Variations++;
                long p = b + binary;
                for (int s = 0; s < Stages.Length; s++)
                {
                    long sc = U64(d, p + 8 + s * 8);
                    if (sc == 0) continue;
                    sc += b;
                    long ctrl = U64(d, sc + 8), code = U64(d, sc + 0x10);
                    long codeSize = U32(d, sc + 0x18), ctrlSize = U32(d, sc + 0x1C);
                    stages.Add(NewStage($"{path}/{vn}/{Stages[s]}", Stages[s], b + code, codeSize, b + ctrl, ctrlSize, d));
                }
                c.Programs.Add((vn, stages));
            }
            var blocks = new SortedSet<(long, long)>();
            var ctrls = new HashSet<long>();
            foreach (var (_, ss) in c.Programs)
                foreach (Stage s in ss) { blocks.Add((s.CodeOff - b, s.CodeSize)); ctrls.Add(s.CtrlOff - b); }
            long at = poolOff, gaps = 0, total = 0;
            foreach (var (off, n) in blocks)
            {
                if (off != at) gaps++;
                at = Math.Max(at, off + n);
                total += n;
            }
            c.Info.Add($"{blocks.Count} distinct code blocks, {H(total)} bytes, " +
                       $"{(gaps == 0 ? "back to back from the pool start" : $"{gaps} gaps")} to {H(at)} " +
                       $"({(at <= poolOff + poolSize ? "inside" : "PAST")} the pool); " +
                       $"{ctrls.Count} distinct control sections, " +
                       $"{ctrls.Count(x => poolOff <= x && x < poolOff + poolSize)} inside the pool");
            return c;
        }

        // ---- FSHA ----------------------------------------------------------------------------------------------

        Container ParseFsha(byte[] d, long b, string path)
        {
            if (!Py.Eq(Py.Slice(d, b, b + 8), "FSHA    ")) throw new PyError($"ValueError: no FSHA at {H(b)}");
            long ver = U32(d, b + 8);
            long size = U32(d, b + 0x1C);
            var c = new Container { Kind = "FSHA", Path = path, Off = b, Size = size };
            long arc = b + U64(d, b + 0x20);
            string name = Py.BinStr(d, b + U64(d, arc));
            long models = U64(d, arc + 0x10), nmodels = U16(d, arc + 0x40);
            c.Info.Add($"archive '{name}', version {ver >> 16}.{(ver >> 8) & 0xFF}.{ver & 0xFF}, {nmodels} shading models");
            (string, int)[] dics = { ("static options", 0x10), ("dynamic options", 0x20), ("attributes", 0x30), ("samplers", 0x40),
                                     ("images", 0x50), ("uniform blocks", 0x60), ("storage buffers", 0x78) };
            for (long m = 0; m < nmodels; m++)
            {
                long o = b + models + m * 0x100;
                string mname = Py.BinStr(d, b + U64(d, o));
                var dic = dics.Select(x => (x.Item1, ResDic(d, b, U64(d, o + x.Item2)))).ToList();
                long progs = U64(d, o + 0x88), keys = U64(d, o + 0x90), bnsh = U64(d, o + 0xA8);
                long nstatic = U16(d, o + 0xE4), ndynamic = U16(d, o + 0xE6), nprog = U16(d, o + 0xE8);
                long keyStatic = Py.Byte(d, o + 0xEA), keyDynamic = Py.Byte(d, o + 0xEB);
                c.Named += (int)nprog;
                c.Info.Add($"model {m} '{mname}': {nprog} programs, {nstatic} static + {ndynamic} dynamic options, " +
                           $"key {keyStatic}+{keyDynamic} words at {H(keys)}, BNSH at {H(bnsh)}");
                c.Info.Add("  " + string.Join(", ", dic.Select(x => $"{x.Item2.Count} {x.Item1}")));
                c.Info.Add("  uniform blocks: " + string.Join(" ", dic[5].Item2));
                Container sub = ParseBnsh(d, b + bnsh, $"{path}/{mname}");
                long varArray = U64(d, b + bnsh + U16(d, b + bnsh + 0x16) + 0x20);
                var index = new List<long>();
                for (long k = 0; k < nprog; k++)
                {
                    long v = U64(d, b + progs + k * 0x40 + 0x20) - bnsh - varArray;
                    if (v < 0 || v % 0x40 != 0 || v / 0x40 >= sub.Programs.Count)
                        throw new PyError($"ValueError: model {mname} program {k} points outside its BNSH");
                    index.Add(v / 0x40);
                }
                bool inOrder = index.Zip(index.Skip(1)).All(x => x.First <= x.Second);
                c.Info.Add($"  programs -> variations: {index.Distinct().Count()} distinct of {sub.Programs.Count}, " +
                           $"{(inOrder ? "in order" : "not in order")}");
                c.Children.Add(sub);
            }
            return c;
        }

        // ---- VFXB ----------------------------------------------------------------------------------------------

        Container ParseVfxb(byte[] d, long b, string path)
        {
            if (!Py.Eq(Py.Slice(d, b, b + 8), "VFXB    ")) throw new PyError($"ValueError: no VFXB at {H(b)}");
            long size = U32(d, b + 0x1C);
            var c = new Container { Kind = "VFXB", Path = path, Off = b, Size = size };
            var names = new List<string>();
            var todo = new Stack<long>();
            todo.Push(b + U16(d, b + 0x16));
            var seen = new HashSet<long>();
            while (todo.Count > 0)
            {
                long s = todo.Pop();
                while (!seen.Contains(s) && s + 0x20 <= b + size && Py.IsAlnum(Py.Slice(d, s, s + 4)))
                {
                    seen.Add(s);
                    string magic = Encoding.ASCII.GetString(Py.Slice(d, s, s + 4));
                    long child = U32(d, s + 8), nxt = U32(d, s + 12), dataOff = U32(d, s + 20);
                    names.Add(magic);
                    bool grs = magic == "GRSR" || magic == "GRSN" || magic == "GRSC";
                    if (grs && Py.Eq(Py.Slice(d, s + dataOff, s + dataOff + 4), "BNSH"))
                        c.Children.Add(ParseBnsh(d, s + dataOff, $"{path}/{magic}"));
                    else if (grs && Py.Eq(Py.Slice(d, s + dataOff, s + dataOff + 8), "FSHA    "))
                        c.Children.Add(ParseFsha(d, s + dataOff, $"{path}/{magic}"));
                    if (child != 0xFFFFFFFF) todo.Push(s + child);
                    if (nxt == 0xFFFFFFFF) break;
                    s += nxt;
                }
            }
            var count = names.GroupBy(x => x).Select(g => (g.Key, g.Count())).OrderBy(x => x.Key, StringComparer.Ordinal);
            c.Info.Add("sections: " + string.Join(", ", count.Select(x => $"{x.Item2} {x.Key}")));
            return c;
        }

        // ---- BAHS ----------------------------------------------------------------------------------------------

        static string BahsStage(long s) => s switch { 0 => "vertex", 1 => "pixel", 2 => "geometry", 3 => "compute", _ => null };

        Container ParseBahs(byte[] d, long b, string path)
        {
            if (!Py.Eq(Py.Slice(d, b, b + 4), "BAHS")) throw new PyError($"ValueError: no BAHS at {H(b)}");
            long ver = U32(d, b + 4), size = U32(d, b + 8);
            long recArea = U32(d, b + 0x14), codeSize = U32(d, b + 0x18), codeArea = U32(d, b + 0x1C);
            var c = new Container { Kind = "BAHS", Path = path, Off = b, Size = size };
            long o = b + recArea;
            while (U32(d, o) == 0) o += 4;
            long areaSize = U32(d, o), count = U32(d, o + 4);
            long end = o + areaSize, r = o + 8;
            var records = new List<(long Stage, long Code, long Ctrl, long CtrlSize)>();
            for (long k = 0; k < count; k++)
            {
                long rsize = U32(d, r), stage = U32(d, r + 4), code = U32(d, r + 0x10), ctrlSize = U32(d, r + 0x18), ctrlRel = S32(d, r + 0x1C);
                records.Add((stage, b + codeArea + code, r + 0x10 + ctrlRel, ctrlSize));
                r += rsize;
            }
            if (r != end) throw new PyError($"ValueError: BAHS records end at {H(r)}, expected {H(end)}");
            long pcount = U32(d, end + 4);
            long p = end + 8;
            var programs = new List<(string Name, long Mask, long First, long Nvar, List<string> Macros)>();
            for (long k = 0; k < pcount; k++)
            {
                long esize = U32(d, p), nlen = U32(d, p + 4), mask = U32(d, p + 8), first = U32(d, p + 12);
                string name = Py.CStr(d, p + 16);
                long q = p + 16 + nlen, variations = 1, e = q + 8;
                var macros = new List<string>();
                long nm = U32(d, q + 4);
                for (long i = 0; i < nm; i++)
                {
                    long msize = U32(d, e), nvals = U32(d, e + 8);
                    variations *= nvals;
                    macros.Add($"{Py.CStr(d, e + 16)}x{nvals}");
                    e += msize;
                }
                programs.Add((name, mask, first, variations, macros));
                p += esize;
            }
            c.Named = (int)pcount;
            c.Info.Add($"version {ver}, {count} stage records, {records.Select(x => x.Ctrl).Distinct().Count()} distinct control " +
                       $"sections, code area {H(codeArea)} ({H(codeSize)} bytes), {pcount} programs");
            for (int i = 0; i < programs.Count; i++)
            {
                var (name, mask, first, nvar, macros) = programs[i];
                long last = i + 1 < programs.Count ? programs[i + 1].First : count;
                var want = Enumerable.Range(0, 8).Where(x => ((mask >> x) & 1) != 0).ToList();
                c.Info.Add($"  {name}: {string.Join("+", want.Select(x => BahsStage(x) ?? x.ToString()))}, records {first}..{last - 1}, " +
                           $"{nvar} variations ({(macros.Count > 0 ? string.Join(" ", macros) : "no macros")})");
                if (last - first != want.Count * nvar)
                    c.Info.Add($"  !! {name}: {last - first} records, expected {want.Count * nvar}");
                for (long v = 0; v < nvar; v++)
                {
                    var stages = new List<Stage>();
                    for (int j = 0; j < want.Count; j++)
                    {
                        long k = first + v * want.Count + j;
                        if (k >= last) break;
                        if (k >= records.Count) throw new PyError("IndexError: list index out of range");
                        var (stage, code, ctrl, ctrlSize) = records[(int)k];
                        string kind = BahsStage(stage) ?? $"stage{stage}";
                        if (stage != want[j]) c.Info.Add($"  !! record {k}: stage {stage}, program mask says {want[j]}");
                        long csize = U32(d, ctrl + 0x704);
                        stages.Add(NewStage($"{path}/{name}/v{v:D3}/{kind}", kind, code, csize, ctrl, ctrlSize, d));
                    }
                    c.Programs.Add(($"{name}/v{v:D3}", stages));
                    c.Variations++;
                }
            }
            var blocks = new SortedSet<(long, long)>();
            foreach (var (_, ss) in c.Programs) foreach (Stage s in ss) blocks.Add((s.CodeOff, s.CodeSize));
            long at = b + codeArea, gaps = 0;
            foreach (var (off, n) in blocks)
            {
                if (off != at) gaps++;
                at = off + n;
            }
            c.Info.Add($"{blocks.Count} distinct code blocks, {(gaps == 0 ? "back to back" : $"{gaps} gaps")}, " +
                       $"{(at <= b + codeArea + codeSize ? "inside" : "PAST")} the code area");
            return c;
        }

        // ---- SARC, sample pair, dispatch -----------------------------------------------------------------------

        static List<(string Name, long Off, long Size)> SarcFiles(byte[] d, long b)
        {
            long data = b + U32(d, b + 0xC);
            long n = U16(d, b + 0x1A);
            long names = b + 0x20 + n * 0x10 + 8;
            var nodes = new List<(string, long, long)>();
            for (long k = 0; k < n; k++)
            {
                long e = b + 0x20 + k * 0x10;
                long attr = U32(d, e + 4), start = U32(d, e + 8), end = U32(d, e + 12);
                string name = (attr >> 24) != 0 ? Py.CStr(d, names + (attr & 0xFFFF) * 4) : $"{k:D4}.bin";
                nodes.Add((name, data + start, end - start));
            }
            return nodes;
        }

        Container ParseSamplePair(byte[] d, long b, long size, string path)
        {
            long vctrl = U32(d, b), fctrl = U32(d, b + 4), vcode = U32(d, b + 8), fcode = U32(d, b + 12);
            var c = new Container { Kind = "NVNPAIR", Path = path, Off = b, Size = size };
            var stages = new List<Stage>();
            foreach (var (kind, ctrl, code, ctrlEnd) in new[] { ("vertex", vctrl, vcode, fctrl), ("pixel", fctrl, fcode, vcode) })
            {
                long codeSize = U32(d, b + ctrl + 0x704);
                stages.Add(NewStage($"{path}/{kind}", kind, b + code, codeSize, b + ctrl, ctrlEnd - ctrl, d));
            }
            c.Programs.Add(("program", stages));
            c.Variations = 1;
            return c;
        }

        // Recognise what is at `b` and return a Container (with children), or null.
        public Container OpenBlob(byte[] d, long b, long size, string path)
        {
            var magic = Py.Slice(d, b, b + 8);
            if (size >= 0x20 && U32(d, b) == 0x10 && U32(d, b + 0x10) == ControlMagic) return ParseSamplePair(d, b, size, path);
            if (magic.Length >= 4 && Py.Eq(magic.Slice(0, 4), "Yaz0"))
            {
                byte[] sub = Yaz0.Decompress(Py.Slice(d, b, b + size));
                return OpenBlob(sub, 0, sub.Length, path);
            }
            if (magic.Length >= 4 && Py.Eq(magic.Slice(0, 4), "SARC"))
            {
                var c = new Container { Kind = "SARC", Path = path, Off = b, Size = size };
                foreach (var (name, off, n) in SarcFiles(d, b))
                {
                    Container sub = OpenBlob(d, off, n, $"{path}/{name}");
                    if (sub != null) c.Children.Add(sub);
                }
                return c.Children.Count > 0 ? c : null;
            }
            if (Py.Eq(magic, "FSHA    ")) return ParseFsha(d, b, path);
            if (Py.Eq(magic, "BNSH\0\0\0\0")) return ParseBnsh(d, b, path);
            if (Py.Eq(magic, "VFXB    ")) return ParseVfxb(d, b, path);
            if (magic.Length >= 4 && Py.Eq(magic.Slice(0, 4), "BAHS")) return ParseBahs(d, b, path);
            if (magic.Length >= 2 && (Py.Eq(magic.Slice(0, 2), "YB") || Py.Eq(magic.Slice(0, 2), "BY")))
            {
                var c = new Container { Kind = "BYML", Path = path, Off = b, Size = size };
                var hay = Py.Slice(d, b, b + size);
                ReadOnlySpan<byte> pat = "VFXB    "u8;
                int at = 0;
                while (true)
                {
                    int i = hay.Slice(at).IndexOf(pat);
                    if (i < 0) break;
                    long m = at + i;
                    // finditer runs on the slice: positions are relative to `b`.
                    c.Children.Add(ParseVfxb(d, b + m, $"{path}/VFXB@{H(m)}"));
                    at = (int)m + pat.Length;
                }
                return c.Children.Count > 0 ? c : null;
            }
            return null;
        }

        // ---- reading, describing, extracting -------------------------------------------------------------------

        public static byte[] ReadFile(string path)
        {
            byte[] raw = File.ReadAllBytes(path);
            if (path.EndsWith(".zs", StringComparison.Ordinal)) raw = Zstd.Decompress(raw);
            if (Yaz0.Is(raw)) raw = Yaz0.Decompress(raw);
            return raw;
        }

        public static void Describe(Container c, StringBuilder o, int depth = 0)
        {
            string pad = new string(' ', 2 * depth);
            o.Append($"{pad}{c.Kind} {c.Path} @ {H(c.Off)} ({H(c.Size)} bytes)\n");
            foreach (string line in c.Info) o.Append($"{pad}  {line}\n");
            if (c.Programs.Count > 0)
            {
                var stages = c.Programs.SelectMany(p => p.Stages).ToList();
                var kinds = new List<(string, int)>();
                foreach (Stage s in stages)
                {
                    int i = kinds.FindIndex(x => x.Item1 == s.Kind);
                    if (i < 0) kinds.Add((s.Kind, 1));
                    else kinds[i] = (s.Kind, kinds[i].Item2 + 1);
                }
                o.Append($"{pad}  {c.Programs.Count} variations, {c.Variations} with code, stages: " +
                         string.Join(", ", kinds.Select(x => $"{x.Item2} {x.Item1}")) +
                         $"; code {H(stages.Sum(s => s.CodeSize))} bytes, " +
                         $"{stages.Select(s => s.CodeOff).Distinct().Count()} distinct code blocks, " +
                         $"{stages.Select(s => s.Hash).Distinct().Count()} distinct by content\n");
                var bad = stages.Where(s => s.Problems.Count > 0).ToList();
                o.Append($"{pad}  layout check: {(bad.Count == 0 ? "all stages OK" : $"{bad.Count} stages FAIL")}\n");
                foreach (Stage s in bad.Take(10)) o.Append($"{pad}    {s.Id}: {string.Join(", ", s.Problems)}\n");
            }
            foreach (Container sub in c.Children) Describe(sub, o, depth + 1);
        }

        public const string ManifestHeader = "id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\t" +
                                             "const_size\tcode_sha1_16\tcode_file\tctrl_file\n";

        // extract(c, outdir, data, unique=True): code_<hash>.bin / ctrl_<hash>.bin once each, one manifest line per stage.
        public static int ExtractUnique(Container c, string outdir)
        {
            Directory.CreateDirectory(outdir);
            var written = new HashSet<string>();
            var m = new StringBuilder(ManifestHeader);
            foreach (Stage s in c.AllStages())
            {
                string codeFile = $"code_{s.Hash}.bin";
                string ctrlFile = $"ctrl_{Sha1.Hex16(s.Ctrl)}.bin";
                if (written.Add(codeFile)) File.WriteAllBytes(Path.Combine(outdir, codeFile), s.Code.ToArray());
                if (written.Add(ctrlFile)) File.WriteAllBytes(Path.Combine(outdir, ctrlFile), s.Ctrl.ToArray());
                m.Append(ManifestLine(s.Id, s, ctrlFile));
            }
            File.WriteAllText(Path.Combine(outdir, "manifest.tsv"), m.ToString(), new UTF8Encoding(false));
            return written.Count;
        }

        public static string ManifestLine(string id, Stage s, string ctrlFile) =>
            $"{id}\t{s.Kind}\t{H(s.CodeOff)}\t{H(s.CodeSize)}\t{H(s.CtrlOff)}\t{H(s.CtrlSize)}\t{H(s.Start)}\t{H(s.Length)}\t" +
            $"{H(s.ConstOff)}\t{H(s.ConstSize)}\t{s.Hash}\tcode_{s.Hash}.bin\t{ctrlFile}\n";
    }
}
