// recomp/__main__.py, ported: analysis, emission on worker threads (one Emitter each, as the Python's forked workers
// each have their own), the --variant patch (a second body per changed function), table.cpp and report.txt.
#include "recomp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "emitter.h"

namespace rc {

namespace {

constexpr int INSNS_PER_FILE = 12000;
constexpr uint64_t REF_BASE = 0x4000000000ull;  // where --base addresses are measured from

// open_if_changed: writes the file unless it already holds exactly this text
void write_if_changed(const std::string& path, const std::string& text) {
    {
        std::ifstream f(upath(path), std::ios::binary);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            if (ss.str() == text) return;
        }
    }
    std::string tmp = path + ".part";
    {
        std::ofstream o(upath(tmp), std::ios::binary | std::ios::trunc);
        if (!o) throw std::runtime_error("cannot write " + path);
        o << text;
        if (!o.flush()) throw std::runtime_error("cannot write " + path);
    }
    std::error_code ec;
    std::filesystem::rename(upath(tmp), upath(path), ec);
    if (ec) throw std::runtime_error("cannot write " + path + " (" + ec.message() + ")");
}

std::string cstr_lit(const std::string& s) { return "\"" + replace_all(replace_all(s, "\\", "\\\\"), "\"", "\\\"") + "\""; }

// read_pchtxt: (module offset, 32-bit word) pairs of the enabled sections of a .pchtxt (IPS text) patch
std::vector<std::pair<uint64_t, uint32_t>> read_pchtxt(const std::string& path) {
    std::ifstream f(upath(path));
    if (!f) throw std::runtime_error("cannot open " + path);
    bool enabled = false;
    std::vector<std::pair<uint64_t, uint32_t>> words;
    std::string raw;
    static const std::regex line_re(R"(([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+))");
    while (std::getline(f, raw)) {
        std::string line(strip(raw.substr(0, raw.find("//"))));
        if (line.empty()) continue;
        if (line == "@enabled") enabled = true;
        else if (line == "@disabled") enabled = false;
        else if (line == "@stop") break;
        else {
            std::smatch mm;
            if (enabled && std::regex_match(line, mm, line_re)) {
                uint64_t off = std::stoull(mm[1].str(), nullptr, 16);
                std::string hexd = mm[2].str();
                if (hexd.size() % 2) throw std::runtime_error(path + ": " + line + ": odd hex");  // bytes.fromhex
                std::vector<uint8_t> data;
                for (size_t k = 0; k < hexd.size(); k += 2) data.push_back(uint8_t(std::stoul(hexd.substr(k, 2), nullptr, 16)));
                if (data.size() % 4) throw std::runtime_error(path + ": " + line + ": not whole words");
                for (size_t k = 0; k < data.size(); k += 4)
                    words.push_back({off + k, uint32_t(data[k]) | uint32_t(data[k + 1]) << 8 | uint32_t(data[k + 2]) << 16 |
                                                  uint32_t(data[k + 3]) << 24});
            }
        }
    }
    return words;
}

struct Variant {
    Module m2;
    Analysis an2;
    std::map<uint64_t, Function> changed;  // start -> the patched function
    std::vector<Function> fresh;           // only in the patched code
    std::vector<std::pair<uint64_t, uint32_t>> words;
};

std::unique_ptr<Variant> build_variant(const Module& m, const Analysis& an, std::vector<std::pair<uint64_t, uint32_t>> words,
                                       const std::function<void(const std::string&)>& log) {
    auto t = std::chrono::steady_clock::now();
    auto v = std::make_unique<Variant>();
    v->m2 = m;
    for (auto& [off, w] : words)
        for (int k = 0; k < 4; k++) v->m2.image.at(off + uint64_t(k)) = uint8_t(w >> (8 * k));
    std::vector<uint64_t> patched, text;
    for (auto& [off, _] : words)
        if (m.in_text(off)) patched.push_back(off);
    text = patched;
    std::sort(text.begin(), text.end());
    analyze(v->m2, v->an2, [](const std::string&) {}, patched);
    auto touched = [&](const Function& f) {
        auto k = std::lower_bound(text.begin(), text.end(), f.start);
        return k != text.end() && *k < f.end;
    };
    std::set<uint64_t> base;
    for (auto& f : an.functions) base.insert(f.start);
    std::map<uint64_t, const Function*> by_start;
    for (auto& f : v->an2.functions) by_start[f.start] = &f;  // a later start overwrites, as the dict does
    for (auto& f : an.functions) {
        auto it = by_start.find(f.start);
        if (it != by_start.end() && (touched(f) || touched(*it->second))) v->changed[f.start] = *it->second;
    }
    for (auto& f : v->an2.functions)
        if (!base.count(f.start)) v->fresh.push_back(f);
    v->words = std::move(words);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    char b[96];
    std::snprintf(b, sizeof b, "  variant analysis: %zu functions (%.0fs)", v->an2.functions.size(), s);
    log(b);
    return v;
}

// variant_function: f's code; with the variant and f changed by the patch, both bodies and a dispatcher under f's symbol
std::string variant_function(Emitter& e, Emitter* e2, const Variant* v, const Function& f) {
    std::string code = e.function(f);
    if (!v) return code;
    auto it = v->changed.find(f.start);
    if (it == v->changed.end()) return code;
    std::string sym = e.fn(f.start);
    std::string head = cat("void ", sym, "(nx::CPU& NX_RESTRICT c) {");
    std::string v1 = e2->function(it->second);
    if (!contains(code, head) || !contains(v1, head)) throw std::runtime_error("--variant: no header for " + sym);
    return replace_first(code, head, cat("static void ", sym, "__v0(nx::CPU& NX_RESTRICT c) {")) + "\n\n" +
           replace_first(v1, head, cat("static void ", sym, "__v1(nx::CPU& NX_RESTRICT c) {")) + "\n\n" +
           cat(head, "\n    if (nx_variant_on && nx_variant_ok(", hxu(f.start), ")) ", sym, "__v1(c); else ", sym, "__v0(c);\n}");
}

std::string fmt_s(double s) {  // f"{s:.0f}"
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", s);
    return b;
}

}  // namespace

RecompResult recompile(const RecompOptions& opt) {
    auto log = [&](const std::string& s) {
        if (opt.log) opt.log(s);
    };
    auto t = std::chrono::steady_clock::now();
    auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); };
    const std::string& name = opt.name;
    Module m = load_nso(opt.nso, name);
    Analysis an;
    analyze(m, an, log);
    {
        char b[160];
        std::snprintf(b, sizeof b, "analysis: %zu functions, %zu jump tables (%.1fs)", an.functions.size(),
                      an.jump_tables.size(), since());
        log(b);
    }
    std::filesystem::create_directories(upath(opt.out_dir));
    std::set<uint64_t> hooks;
    if (!opt.hooks.empty() || !opt.hooks_text.empty()) {
        std::istringstream text(opt.hooks_text);
        std::ifstream hf;
        if (!opt.hooks.empty()) {
            hf.open(upath(opt.hooks));
            if (!hf) throw std::runtime_error("cannot open " + opt.hooks);
        }
        std::istream& in = opt.hooks.empty() ? static_cast<std::istream&>(text) : hf;
        std::string ln;
        while (std::getline(in, ln)) {
            std::string a(strip(ln.substr(0, ln.find('#'))));
            if (!a.empty()) hooks.insert(std::stoull(a, nullptr, 16));
        }
        log("  " + std::to_string(hooks.size()) + " hooks from " + (opt.hooks.empty() ? std::string("the game's list") : opt.hooks));
    }
    std::unique_ptr<Variant> variant;
    if (!opt.variant.empty()) variant = build_variant(m, an, read_pchtxt(opt.variant), log);
    std::vector<Function> funcs = an.functions;
    if (!opt.only.empty()) {
        std::set<uint64_t> want(opt.only.begin(), opt.only.end());
        funcs.erase(std::remove_if(funcs.begin(), funcs.end(), [&](const Function& f) { return !want.count(f.start); }), funcs.end());
    }
    Emitter e0(an, name);  // names only (fn)

    {
        std::string h = cat("#pragma once\n#include <nx/cpu.h>\n\n#undef NXB\n#define NXB (NX_BASE + ", hx(int64_t(opt.base - REF_BASE)),
                            "ull)\n\n");
        for (const Function& f : an.functions) h += cat("void ", e0.fn(f.start), "(nx::CPU& c);\n");
        if (variant) {
            h += "\nextern \"C\" bool nx_variant_on;  // the runtime's NX_SPEEDFIX: the --variant bodies\n";
            h += "extern \"C\" uint64_t nx_variant_lo, nx_variant_hi;  // NX_VARIANT_RANGE: only these (bisecting)\n";
            h += "static inline bool nx_variant_ok(uint64_t a) { return a >= nx_variant_lo && a < nx_variant_hi; }\n";
            for (const Function& f : variant->fresh) h += cat("void ", e0.fn(f.start), "(nx::CPU& c);\n");
        }
        write_if_changed(opt.out_dir + "/funcs.h", h);
    }

    // translation units: consecutive functions up to INSNS_PER_FILE instructions each, the same split whatever the
    // number of jobs, so a rerun rewrites only the files whose code changed
    struct Chunk {
        size_t i, lo, hi;
    };
    std::vector<Chunk> chunks;
    {
        size_t first = 0;
        int64_t n = 0;
        for (size_t k = 0; k < funcs.size(); k++) {
            n += int64_t((funcs[k].end - funcs[k].start) / 4);
            if (n >= INSNS_PER_FILE || k == funcs.size() - 1) {
                chunks.push_back({chunks.size(), first, k + 1});
                first = k + 1, n = 0;
            }
        }
    }
    unsigned jobs = opt.jobs ? opt.jobs : std::max(1u, std::thread::hardware_concurrency());
    jobs = std::max<unsigned>(1, std::min<size_t>(jobs, chunks.size()));
    log("  emitting " + std::to_string(funcs.size()) + " functions into " + std::to_string(chunks.size()) + " files with " +
        std::to_string(jobs) + " jobs");

    std::mutex mx;
    std::atomic<size_t> next{0};
    uint64_t handled = 0;
    std::map<std::string, uint64_t> unhandled;
    size_t done = 0;
    std::string error;
    auto worker = [&] {
        try {
            Emitter e(an, name);
            e.hooks = hooks;
            std::unique_ptr<Emitter> e2;
            if (variant) {
                e2 = std::make_unique<Emitter>(variant->an2, name);
                e2->hooks = hooks;
            }
            for (;;) {
                if (opt.cancel && opt.cancel->load()) return;
                size_t c = next++;
                if (c >= chunks.size()) return;
                const Chunk& ch = chunks[c];
                e.handled = 0, e.unhandled.clear();
                std::vector<std::string> code;
                for (size_t k = ch.lo; k < ch.hi; k++) code.push_back(variant_function(e, e2.get(), variant.get(), funcs[k]));
                char nm[32];
                std::snprintf(nm, sizeof nm, "/code_%03zu.cpp", ch.i);
                write_if_changed(opt.out_dir + nm, "#include \"funcs.h\"\n\n" + join(code, "\n\n") + "\n");
                std::lock_guard<std::mutex> l(mx);
                handled += e.handled;
                for (auto& [k, v] : e.unhandled) unhandled[k] += v;
                size_t count = ch.hi - ch.lo;
                done += count;
                if (done / 8000 != (done - count) / 8000)
                    log("  emitted " + std::to_string(done) + "/" + std::to_string(funcs.size()) + " functions (" + fmt_s(since()) + "s)");
                if (opt.progress) opt.progress(done, funcs.size());
            }
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> l(mx);
            if (error.empty()) error = ex.what();
            next = chunks.size();
        }
    };
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < jobs; k++) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    if (!error.empty()) throw std::runtime_error(error);
    if (opt.cancel && opt.cancel->load()) throw std::runtime_error("cancelled");

    std::string variant_path = opt.out_dir + "/code_variant.cpp";
    if (variant) {
        Emitter e2(variant->an2, name);
        e2.hooks = hooks;
        std::vector<std::string> code;
        for (const Function& f : variant->fresh) code.push_back(e2.function(f));
        write_if_changed(variant_path, "#include \"funcs.h\"\n\n" + join(code, "\n\n") + "\n");
        log("  variant: " + std::to_string(variant->changed.size()) + " functions with two bodies, " +
            std::to_string(variant->fresh.size()) + " only in the patched code, " + std::to_string(variant->words.size()) + " words");
    } else {
        std::error_code ec;
        std::filesystem::remove(upath(variant_path), ec);
    }

    // table.cpp
    {
        std::string o = "#include \"funcs.h\"\n#include <nx/module.h>\n\n";
        o += "static const nx::FuncEntry funcs[] = {\n";
        std::vector<uint64_t> starts;
        for (auto& f : an.functions) starts.push_back(f.start);
        if (variant)
            for (auto& f : variant->fresh) starts.push_back(f.start);
        std::sort(starts.begin(), starts.end());
        for (uint64_t a : starts) o += cat("    {", hxu(a), ", ", e0.fn(a), "},\n");
        o += "};\n\n";
        o += "static const nx::Reloc relocs[] = {\n";
        for (const Reloc& r : m.relocs) o += cat("    {", hxu(r.offset), ", ", dec(r.type), ", ", dec(r.sym), ", ", dec(r.addend), "ll},\n");
        o += "};\n\n";
        o += "static const nx::Symbol symbols[] = {\n";
        for (const Symbol& s : m.symbols)
            o += cat("    {", cstr_lit(s.name), ", ", hxu(s.value), ", ", dec(int64_t(s.size)), ", ", dec(s.type), ", ", dec(s.bind), ", ",
                     dec(s.imported() ? 1 : 0), "},\n");
        o += "};\n\n";
        std::vector<std::string> ia, fa;
        for (uint64_t a : m.init_array) ia.push_back(hxu(a));
        ia.push_back("0x0");
        for (uint64_t a : m.fini_array) fa.push_back(hxu(a));
        fa.push_back("0x0");
        o += cat("static const uint64_t init_array[] = {", join(ia, ", "), "};\n");
        o += cat("static const uint64_t fini_array[] = {", join(fa, ", "), "};\n\n");
        o += cat("extern const nx::ModuleInfo nx_module_", name, " = {\n");
        o += cat("    ", cstr_lit(name), ", NX_BASE + ", hx(int64_t(opt.base - REF_BASE)), "ull,\n");
        o += cat("    ", hxu(m.text[0]), ", ", hxu(m.text[1]), ", ", hxu(m.ro[0]), ", ", hxu(m.ro[1]), ", ", hxu(m.data[0]), ", ",
                 hxu(m.data[1]), ", ", hxu(m.bss[0]), ", ", hxu(m.bss[1]), ",\n");
        o += cat("    funcs, ", dec(int64_t(starts.size())), ",\n");
        o += cat("    relocs, ", dec(int64_t(m.relocs.size())), ",\n");
        o += cat("    symbols, ", dec(int64_t(m.symbols.size())), ",\n");
        o += cat("    init_array, ", dec(int64_t(m.init_array.size())), ", fini_array, ", dec(int64_t(m.fini_array.size())), ",\n");
        o += "};\n";
        if (variant) {  // (offset, word) pairs, written into the image by the loader when the variant is on
            o += "\n#include <nx/runtime.h>\n";
            std::vector<std::string> w;
            for (auto& [off, x] : variant->words) w.push_back(cat(hxu(off), ", ", hx010(x)));
            o += cat("\nstatic const uint32_t variant_words[] = {", join(w, ", "), "};\n");
            o += cat("static nx::VariantRegistrar variant_registrar(", cstr_lit(name), ", variant_words, ",
                     dec(int64_t(variant->words.size())), ");\n");
        }
        write_if_changed(opt.out_dir + "/table.cpp", o);
    }

    uint64_t total_unhandled = 0;
    for (auto& [k, v] : unhandled) total_unhandled += v;
    // most_common(): by count, the first seen first among equals (here: by name; the report is not compiled)
    std::vector<std::pair<std::string, uint64_t>> mc(unhandled.begin(), unhandled.end());
    std::stable_sort(mc.begin(), mc.end(), [](auto& a, auto& b) { return a.second > b.second; });
    {
        std::string r = cat("functions ", std::to_string(funcs.size()), ", handled ", std::to_string(handled), ", unhandled ",
                            std::to_string(total_unhandled), "\n");
        r += cat("jump tables ", std::to_string(an.jump_tables.size()), ", unresolved br (indirect tail calls) ",
                 std::to_string(an.unresolved_br.size()), "\n\n");
        for (auto& [mn, n] : mc) {
            char b[64];
            std::snprintf(b, sizeof b, "%8llu ", (unsigned long long)n);
            r += b + mn + "\n";
        }
        std::ofstream(upath(opt.out_dir + "/report.txt"), std::ios::binary | std::ios::trunc) << r;
    }
    log("done: " + std::to_string(chunks.size()) + " files, handled " + std::to_string(handled) + ", unhandled " +
        std::to_string(total_unhandled) + " (" + fmt_s(since()) + "s)");
    for (size_t k = 0; k < mc.size() && k < 30; k++) {
        char b[64];
        std::snprintf(b, sizeof b, "  %7llu ", (unsigned long long)mc[k].second);
        log(b + mc[k].first);
    }
    return {funcs.size(), chunks.size(), handled, total_unhandled};
}

}  // namespace rc
