// spirv_harness --extract: tools/extract_shaders.py in C# (with exefs_shaders.py and model_shaders.py), so one native
// binary extracts and translates the game's shaders. The output tree is the one the Python writes, byte for byte:
// one group per container (<group>/code_<hash>.bin, ctrl_<hash>.bin, manifest.tsv, <group>.log), then `exefs` (the
// blocks embedded in main) and `model` (the blocks only models carry). The one difference: model/manifest.tsv lists
// its rows in model-file order, where the Python's (multiprocessing imap_unordered) order changes from run to run.

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace SpirvHarness.Extract
{
    static class Extractor
    {
        static readonly UTF8Encoding Utf8 = new(false);

        // ---- the driver (extract_shaders.py) -------------------------------------------------------------------

        public static int Run(string romfs, string mainBin, string o, int jobs, bool full)
        {
            if (!Directory.Exists(romfs))
            {
                Console.Error.WriteLine($"no {romfs}: import the game first");
                return 1;
            }
            if (!OutputAllowed(o))
            {
                Console.Error.WriteLine($"refusing to write game data into the repository outside private/: {o}");
                return 1;
            }
            Directory.CreateDirectory(o);

            var groups = new List<(string Name, string Path)>();
            void Grp(string path, string prefix) => groups.Add(($"{prefix}_{Path.GetFileName(path).Split('.')[0]}", path));
            List<string> Files(string d)
            {
                d = Path.Combine(romfs, d);
                return Directory.Exists(d) ? Directory.EnumerateFileSystemEntries(d).OrderBy(x => x, StringComparer.Ordinal).ToList() : new List<string>();
            }

            foreach (string f in Files("Shader")) Grp(f, "shader");
            foreach (string f in Files("Effect")) Grp(f, "effect");
            foreach (var (dir, names) in Walk(Path.Combine(romfs, "System")).OrderBy(x => x.Dir, StringComparer.Ordinal))
                foreach (string n in names.OrderBy(x => x, StringComparer.Ordinal))
                    Grp(Path.Combine(dir, n), "system_" + Path.GetFileName(dir));
            foreach (string f in Files(Path.Combine("Swkbd", "shader")))
                if (f.EndsWith("_Nx.arc.szs", StringComparison.Ordinal)) Grp(f, "swkbd");
            foreach (string f in Files("Layout")) Grp(f, "layout");
            if (full)
                foreach (string f in Files("Model")) Grp(f, "model");

            var rc = new int[groups.Count];
            var opts = new ParallelOptions { MaxDegreeOfParallelism = jobs };
            Parallel.ForEach(System.Collections.Concurrent.Partitioner.Create(Enumerable.Range(0, groups.Count),
                                 EnumerablePartitionerOptions.NoBuffering), opts, i =>
            {
                var (name, path) = groups[i];
                var log = new StringBuilder();
                rc[i] = ExtractContainer(path, Path.Combine(o, name), log);
                File.WriteAllText(Path.Combine(o, name + ".log"), log.ToString(), Utf8);
            });
            for (int i = 0; i < groups.Count; i++)
            {
                if (rc[i] == 0) continue;
                // Most layouts (and some other archives) hold no shader at all: not a failure.
                if (File.ReadAllText(Path.Combine(o, groups[i].Name + ".log")).Contains("no shader container recognised")) continue;
                Console.WriteLine($"FAIL {groups[i].Name}");
            }

            // Containers without shaders leave an empty folder (or none): drop them and their logs.
            foreach (string p in Directory.GetDirectories(o))
                if (!Directory.EnumerateFileSystemEntries(p).Any(x => Path.GetFileName(x) != "manifest.tsv"))
                    try { Directory.Delete(p, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            foreach (string f in Directory.GetFiles(o))
                if (f.EndsWith(".log", StringComparison.Ordinal) && !Directory.Exists(f[..^4])) File.Delete(f);

            // The code blocks embedded in `main` that no container holds; after the others: it skips what they have.
            var xlog = new StringBuilder();
            if (Guard(() => Exefs(mainBin, Path.Combine(o, "exefs"), xlog), xlog) != 0) Console.WriteLine("FAIL exefs");
            File.WriteAllText(Path.Combine(o, "exefs.log"), xlog.ToString(), Utf8);
            // The variations only models carry; last: it keeps only the blocks no other group has.
            var mlog = new StringBuilder();
            int mrc;
            using (var w = new StreamWriter(Path.Combine(o, "model.log"), false, Utf8) { AutoFlush = true })
            {
                mrc = Guard(() => Models(Path.Combine(romfs, "Model"), o, jobs, mlog, w), mlog);
                w.Write(mlog.ToString());
            }
            if (mrc != 0) Console.WriteLine("FAIL model");

            int ngroups = Directory.GetDirectories(o).Length;
            int nblocks = Directory.EnumerateFiles(o, "*", SearchOption.AllDirectories).Select(Path.GetFileName)
                .Count(f => f.EndsWith(".code.bin", StringComparison.Ordinal) || (f.StartsWith("code_", StringComparison.Ordinal) && f.EndsWith(".bin", StringComparison.Ordinal)));
            Console.WriteLine($"groups: {ngroups}  blocks: {nblocks}");
            return 0;
        }

        static int Guard(Func<int> f, StringBuilder log)
        {
            try { return f(); }
            catch (Exception e) when (e is not OutOfMemoryException)
            {
                log.Append($"{e.GetType().Name}: {e.Message}\n");
                return 1;
            }
        }

        // os.walk (no descent into symbolic links to directories): (directory, non-directory names) for every directory.
        static List<(string Dir, List<string> Names)> Walk(string top)
        {
            var o = new List<(string, List<string>)>();
            if (!Directory.Exists(top)) return o;
            var names = new List<string>();
            var dirs = new List<string>();
            foreach (var e in new DirectoryInfo(top).EnumerateFileSystemInfos())
            {
                if (e is DirectoryInfo) dirs.Add(e.FullName);
                else if (e.LinkTarget != null && Directory.Exists(e.FullName)) dirs.Add(e.FullName);
                else names.Add(e.Name);
            }
            o.Add((top, names));
            foreach (string d in dirs)
            {
                var di = new DirectoryInfo(d);
                if (di.LinkTarget != null) continue;   // listed as a directory, not walked
                o.AddRange(Walk(Path.Combine(top, di.Name)));
            }
            return o;
        }

        // Game data may go to private/ or anywhere outside the repository (an install folder), never elsewhere in a
        // checkout, where it could end up in a commit. The repository is found from the output path upwards.
        public static bool OutputAllowed(string path)
        {
            string real = Path.GetFullPath(path);
            for (string d = Path.GetDirectoryName(real); d != null; d = Path.GetDirectoryName(d))
                if (File.Exists(Path.Combine(d, "tools", "maxwell", "containers.py")))
                    return real.StartsWith(Path.Combine(d, "private") + Path.DirectorySeparatorChar, StringComparison.Ordinal);
            return true;
        }

        // containers.py <path> --extract <outdir> --unique: 0, or 1 with the reason in the log.
        static int ExtractContainer(string path, string outdir, StringBuilder log)
        {
            try
            {
                byte[] data = ContainerParser.ReadFile(path);
                Container c = new ContainerParser().OpenBlob(data, 0, data.Length, Path.GetFileName(path));
                if (c == null)
                {
                    log.Append($"{path}: no shader container recognised\n");
                    return 1;
                }
                ContainerParser.Describe(c, log);
                int files = ContainerParser.ExtractUnique(c, outdir);
                log.Append($"extracted {c.AllStages().Count()} stages ({files} files) to {outdir}\n");
                return 0;
            }
            catch (Exception e) when (e is not OutOfMemoryException)
            {
                log.Append($"{e.GetType().Name}: {e.Message}\n");
                return 1;
            }
        }

        static void AddKnown(HashSet<string> known, string manifest, bool strict)
        {
            string[] lines = File.ReadAllText(manifest).Split('\n');
            for (int i = 1; i < lines.Length; i++)
            {
                string l = lines[i];
                if (l.Length == 0) continue;
                string[] f = l.Split('\t');
                if (f.Length > 10) known.Add(f[10]);
                else if (strict) throw new PyError($"IndexError: list index out of range ({manifest})");
            }
        }

        // ---- exefs_shaders.py ----------------------------------------------------------------------------------

        static uint U32(byte[] d, long o) => Py.U32(d, o);
        static long Up(long x) => (x + 0xff) & ~0xffL;

        static List<(long Code, long Ctrl)> FindInMain(byte[] d, StringBuilder log)
        {
            ReadOnlySpan<byte> all = d;
            var ctrls = new List<long>();
            ReadOnlySpan<byte> cm = new byte[] { 0x34, 0x12, 0x76, 0x98 };
            for (int at = 0; ;)
            {
                int i = all.Slice(at).IndexOf(cm);
                if (i < 0) break;
                long m = at + i;
                if (m % 4 == 0 && m + 0x880 <= d.Length)
                {
                    long s = U32(d, m + 0x704);
                    if (s > 0 && s < 1 << 24) ctrls.Add(m);
                }
                at = (int)m + 4;
            }
            // (?:\x78|\x79)\x56\x34\x12 + 0x2c zero bytes: matches cannot overlap, so every position is one.
            var codes = new List<long>();
            ReadOnlySpan<byte> km = new byte[] { 0x56, 0x34, 0x12 };
            for (int at = 1; ;)
            {
                int i = all.Slice(at).IndexOf(km);
                if (i < 0) break;
                int q = at + i, p = q - 1;
                if ((d[p] == 0x78 || d[p] == 0x79) && p + 0x30 <= d.Length && !Py.Any(all.Slice(p + 4, 0x2c))) codes.Add(p);
                at = q + 1;
            }

            byte[] exit = BitConverter.GetBytes(0xe30000000007000fUL);
            bool Fits(long p, long c)
            {
                long s = U32(d, c + 0x704), start = U32(d, c + 0x708), plen = U32(d, c + 0x6f8), csize = U32(d, c + 0x6fc), coff = U32(d, c + 0x700);
                bool compute = U32(d, p) == 0x12345679;
                if (p + s > d.Length || start != (compute ? 0x100 : 0x30) || start + plen > s) return false;
                var tail = Py.Slice(d, p + start + plen - 8, p + start + plen);   // the program must not end in zeros
                if (Py.Slice(d, p + start, p + start + plen).IndexOf(exit) < 0 || tail.Length == 8 && !Py.Any(tail)) return false;
                if (csize != 0)
                {
                    if (coff != Up(start + plen) || s != Up(coff + csize) || Py.Any(Py.Slice(d, p + coff + csize, p + s))) return false;
                }
                else if (s != Up(start + plen)) return false;
                if (Py.Any(Py.Slice(d, p + start + plen, p + Up(start + plen)))) return false;
                return compute || ((U32(d, p + start) >> 10) & 0xf) is 1 or 5;
            }

            var found = new List<(long, long)>();
            for (int i = 0; i < codes.Count; i++)
            {
                long p = codes[i];
                long room = (i + 1 < codes.Count ? codes[i + 1] : d.Length) - p;
                var cands = ctrls.Where(c => U32(d, c + 0x704) <= room && Fits(p, c)).ToList();
                if (cands.Count > 0)
                {
                    long size = cands.Max(c => (long)U32(d, c + 0x704));
                    cands = cands.Where(c => U32(d, c + 0x704) == size).ToList();
                }
                int keys = cands.Select(c => Convert.ToHexString(Py.Slice(d, c + 0x6f8, c + 0x70c)) + "/" +
                                             Convert.ToHexString(Py.Slice(d, c + 0x748, c + 0x758))).Distinct().Count();
                if (keys > 1) log.Append($"code at {Py.H(p)}: {cands.Count} control sections that disagree, skipped\n");
                else if (cands.Count > 0) found.Add((p, cands[0]));
            }
            return found;
        }

        static int Exefs(string mainBin, string outPath, StringBuilder log)
        {
            byte[] d = File.ReadAllBytes(mainBin);
            string out_ = outPath.TrimEnd(Path.DirectorySeparatorChar, '/');
            if (!OutputAllowed(out_)) throw new PyError($"refusing to write game data into the repository outside private/: {out_}");
            var known = new HashSet<string>();
            string parent = Path.GetDirectoryName(Path.GetFullPath(out_));
            if (Directory.Exists(parent))
                foreach (string sub in Directory.GetFileSystemEntries(parent))
                {
                    string manifest = Path.Combine(sub, "manifest.tsv");
                    if (Path.GetFileName(sub) != Path.GetFileName(out_) && File.Exists(manifest)) AddKnown(known, manifest, true);
                }
            var errs = new StringBuilder();
            var found = FindInMain(d, errs);
            Directory.CreateDirectory(out_);
            var seen = new HashSet<string>();
            var m = new StringBuilder(ContainerParser.ManifestHeader);
            foreach (var (p, c) in found)
            {
                long s = U32(d, c + 0x704);
                byte[] code = Py.Slice(d, p, p + s).ToArray(), ctrl = Py.Slice(d, c, c + 0x880).ToArray();
                string h = Sha1.Hex16(code);
                if (seen.Contains(h) || known.Contains(h)) continue;
                seen.Add(h);
                string stage = U32(d, p) == 0x12345679 ? "compute" : ((U32(d, p + 0x30) >> 10) & 0xf) switch
                {
                    1 => "vertex",
                    5 => "pixel",
                    var x => throw new PyError($"KeyError: {x}")
                };
                string cf = $"ctrl_{Sha1.Hex16(ctrl)}.bin";
                File.WriteAllBytes(Path.Combine(out_, $"code_{h}.bin"), code);
                File.WriteAllBytes(Path.Combine(out_, cf), ctrl);
                m.Append($"main/{Py.H(p)}/{stage}\t{stage}\t{Py.H(p)}\t{Py.H(s)}\t{Py.H(c)}\t{Py.H(0x880)}\t{Py.H(U32(d, c + 0x708))}\t" +
                         $"{Py.H(U32(d, c + 0x6f8))}\t{Py.H(U32(d, c + 0x700))}\t{Py.H(U32(d, c + 0x6fc))}\t{h}\tcode_{h}.bin\t{cf}\n");
            }
            File.WriteAllText(Path.Combine(out_, "manifest.tsv"), m.ToString(), Utf8);
            log.Append(errs);   // stderr first: the Python's stdout is buffered to the end
            log.Append($"{seen.Count} code blocks extracted to {out_}\n");
            return 0;
        }

        // ---- model_shaders.py ----------------------------------------------------------------------------------

        sealed record ModelStage(Stage S, byte[] Code, byte[] Ctrl);

        static int Models(string models, string root, int jobs, StringBuilder log, StreamWriter progress)
        {
            const string group = "model";
            string outDir = Path.Combine(root, group);
            if (!OutputAllowed(outDir)) throw new PyError($"refusing to write game data into the repository outside private/: {outDir}");
            var known = new HashSet<string>();
            foreach (string dir in Directory.EnumerateDirectories(root, "*", SearchOption.AllDirectories).Prepend(root))
            {
                string manifest = Path.Combine(dir, "manifest.tsv");
                if (Path.GetFileName(dir) != group && File.Exists(manifest)) AddKnown(known, manifest, false);
            }
            var files = Directory.EnumerateFiles(models).Where(f => f.EndsWith(".Nin_NX_NVN.zs", StringComparison.Ordinal))
                .OrderBy(f => f, StringComparer.Ordinal).ToList();
            progress.Write($"{files.Count} model archives, {known.Count} code blocks known from the other groups\n");
            Directory.CreateDirectory(outDir);
            var written = new HashSet<string>();
            int rows = 0, failed = 0;
            var m = new StringBuilder(ContainerParser.ManifestHeader);
            var opts = new ParallelOptions { MaxDegreeOfParallelism = jobs };
            const int Chunk = 2000;   // results are taken in file order, a chunk at a time
            for (int c0 = 0; c0 < files.Count; c0 += Chunk)
            {
                int n = Math.Min(Chunk, files.Count - c0);
                var res = new (List<ModelStage> Stages, bool Error)[n];
                Parallel.For(0, n, opts, i => res[i] = Scan(files[c0 + i], known));
                for (int i = 0; i < n; i++)
                {
                    if (res[i].Error) failed++;
                    foreach (var (s, code, ctrl) in res[i].Stages)
                    {
                        if (!written.Add(s.Hash)) continue;
                        string ctrlFile = $"ctrl_{Sha1.Hex16(ctrl)}.bin";
                        File.WriteAllBytes(Path.Combine(outDir, $"code_{s.Hash}.bin"), code);
                        File.WriteAllBytes(Path.Combine(outDir, ctrlFile), ctrl);
                        int slash = s.Id.IndexOf('/');
                        string sid = Path.GetFileName(files[c0 + i]) + "/" + (slash >= 0 ? s.Id[(slash + 1)..] : s.Id);
                        m.Append(ContainerParser.ManifestLine(sid, s, ctrlFile));
                        rows++;
                    }
                    int done = c0 + i + 1;
                    if (done % 2000 == 0) progress.Write($"  {done}/{files.Count} archives, {rows} new code blocks\n");
                }
            }
            File.WriteAllText(Path.Combine(outDir, "manifest.tsv"), m.ToString(), Utf8);
            log.Append($"{rows} code blocks only in models -> {outDir} ({failed} archives not parsed)\n");
            return 0;
        }

        // The stages of one model file whose code no other group has.
        static (List<ModelStage>, bool) Scan(string path, HashSet<string> known)
        {
            var o = new List<ModelStage>();
            Container c;
            try
            {
                byte[] data = ContainerParser.ReadFile(path);
                c = new ContainerParser().OpenBlob(data, 0, data.Length, Path.GetFileName(path));
            }
            catch (Exception e) when (e is not OutOfMemoryException)
            {
                return (o, true);
            }
            if (c != null)
            {
                var seen = new HashSet<string>();
                foreach (Stage s in c.AllStages())
                {
                    if (known.Contains(s.Hash) || !seen.Add(s.Hash)) continue;
                    o.Add(new ModelStage(s, s.Code.ToArray(), s.Ctrl.ToArray()));
                }
            }
            return (o, false);
        }
    }
}
