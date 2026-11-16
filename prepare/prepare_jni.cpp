// libacnh_prepare.so: the game folder prepared on the phone itself (prepare.h), for the Android app's Prepare screen
// and PrepareService (android/java/io/github/acnhrecomp/). A library of its own beside the player's libacnh.so, so
// preparing loads neither the runtime nor a game. The tools are the APK's: libzig.so and libspirv_harness.so in the
// app's native library folder (the only files Android lets an app run), zig's lib folder unpacked from the APK
// (tools/build_android.sh).
//
// The calls (PrepareNative.java's native*), text in and out as UTF-8 bytes (JNI's own strings are
// "modified UTF-8", wrong for a file name with an emoji):
//   probe(request)   the folders' check (nx::prepare::probe), as JSON: ok, version, build id, the RomFS's and the
//                    add-ons' sizes, the findings (key, args, English text) for the screen to translate
//   start(request)   prep::start(): false when one is running already
//   status(since)    prep::status() as JSON, with the log lines numbered `since` and after
//   cancel()         prep::cancel()
// A request is "key=value" lines: input= (repeated: one folder holding exefs/ and romfs/, or two), dlc= (repeated),
// out=, in_place=1, platforms= (default android-arm64), fps60=0|1, zig=, zig_lib=, shader_tool=, cache=, log=, and for
// tests skip_recompile=1 (prepare.h: synthetic modules are no program; the app never sends it).
#include <android/log.h>
#include <jni.h>

#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "inputs.h"
#include "io_util.h"
#include "prepare.h"

namespace fs = std::filesystem;
namespace np = nx::prepare;
using nx::prepare::detail::u8;

namespace {

std::string bytes_in(JNIEnv* env, jbyteArray a) {
    std::string s;
    if (!a) return s;
    s.resize(size_t(env->GetArrayLength(a)));
    if (!s.empty()) env->GetByteArrayRegion(a, 0, jsize(s.size()), reinterpret_cast<jbyte*>(s.data()));
    return s;
}

jbyteArray bytes_out(JNIEnv* env, const std::string& s) {
    jbyteArray a = env->NewByteArray(jsize(s.size()));
    if (a && !s.empty()) env->SetByteArrayRegion(a, 0, jsize(s.size()), reinterpret_cast<const jbyte*>(s.data()));
    return a;
}

struct Request {
    std::vector<std::string> inputs, dlc;
    std::string out, platforms = "android-arm64", zig, zig_lib, shader_tool, cache, log;
    bool in_place = false, fps60 = true, skip_recompile = false;
};

Request parse(const std::string& text) {
    Request r;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "input" && !v.empty()) r.inputs.push_back(v);
        else if (k == "dlc" && !v.empty()) r.dlc.push_back(v);
        else if (k == "out") r.out = v;
        else if (k == "in_place") r.in_place = v == "1";
        else if (k == "platforms" && !v.empty()) r.platforms = v;
        else if (k == "fps60") r.fps60 = v != "0";
        else if (k == "zig") r.zig = v;
        else if (k == "zig_lib") r.zig_lib = v;
        else if (k == "shader_tool") r.shader_tool = v;
        else if (k == "cache") r.cache = v;
        else if (k == "log") r.log = v;
        else if (k == "skip_recompile") r.skip_recompile = v == "1";
    }
    return r;
}

// ---- JSON out ----

void json_str(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += char(c);
                }
        }
    }
    o += '"';
}

struct Json {
    std::string o = "{";
    bool first = true;
    void key(const char* k) {
        if (!first) o += ',';
        first = false;
        json_str(o, k);
        o += ':';
    }
    void str(const char* k, const std::string& v) { key(k), json_str(o, v); }
    void num(const char* k, double v) {
        key(k);
        char b[32];
        std::snprintf(b, sizeof b, "%.4f", v);
        o += b;
    }
    void u64(const char* k, uint64_t v) { key(k), o += std::to_string(v); }
    void i64(const char* k, long long v) { key(k), o += std::to_string(v); }
    void boolean(const char* k, bool v) { key(k), o += v ? "true" : "false"; }
    void raw(const char* k, const std::string& v) { key(k), o += v; }
    std::string done() { return o + "}"; }
};

const char* severity_name(np::Severity s) {
    return s == np::Severity::Error ? "error" : s == np::Severity::Warning ? "warning" : "info";
}

std::string messages_json(const std::vector<np::Message>& ms) {
    std::string o = "[";
    for (size_t i = 0; i < ms.size(); i++) {
        const np::Message& m = ms[i];
        Json j;
        j.str("severity", severity_name(m.severity));
        j.str("key", m.key);
        std::string args = "[";
        for (size_t k = 0; k < m.args.size(); k++) {
            if (k) args += ',';
            json_str(args, m.args[k]);
        }
        j.raw("args", args + "]");
        j.str("text", m.text);
        j.str("path", m.path);
        o += (i ? "," : "") + j.done();
    }
    return o + "]";
}

// ---- the log, shared by every poller (the screen and the service's notification) ----

struct Shared {
    std::mutex m;
    std::deque<std::pair<long long, prep::LogLine>> lines;  // the last kKeep, numbered
    long long next = 0;
    std::FILE* file = nullptr;  // every line, for support: <app files>/prepare.log, then into <game>/.import/
    std::string log_path;
    bool ended = false;  // the end written to the log (once)
};
constexpr size_t kKeep = 400;

Shared& shared() {
    static Shared s;
    return s;
}

// The game folder's .import/prepare.log, once the preparation ended: the log with the folder it is about.
void copy_log(Shared& s, const std::string& out) {
    if (s.log_path.empty() || out.empty()) return;
    if (s.file) std::fflush(s.file);
    std::error_code ec;
    const fs::path dir = u8(out) / ".import";
    if (fs::is_directory(dir, ec)) fs::copy_file(u8(s.log_path), dir / "prepare.log", fs::copy_options::overwrite_existing, ec);
}

}  // namespace

extern "C" JNIEXPORT jbyteArray JNICALL Java_io_github_acnhrecomp_PrepareNative_nativeProbe(JNIEnv* env, jclass, jbyteArray req) {
    const Request r = parse(bytes_in(env, req));
    np::ProbeOptions po;
    po.dlc = r.dlc;
    const np::ProbeResult p = np::probe(r.inputs, po);
    Json j;
    j.boolean("ok", p.ok);
    j.str("layout", np::layout_name(p.layout));
    j.str("exefs", p.exefs);
    j.str("build_id", p.build_id);
    j.str("version", p.version);
    j.str("romfs", p.romfs.path);
    j.u64("romfs_files", p.romfs.files);
    j.u64("romfs_bytes", p.romfs.bytes);
    j.boolean("romfs_walked", p.romfs.walked);
    std::string dlc = "[";
    for (size_t i = 0; i < p.dlc.size(); i++) {
        Json d;
        d.str("path", p.dlc[i].path);
        d.i64("index", p.dlc[i].index);
        d.u64("files", p.dlc[i].files);
        d.u64("bytes", p.dlc[i].bytes);
        dlc += (i ? "," : "") + d.done();
    }
    j.raw("dlc", dlc + "]");
    j.raw("messages", messages_json(p.messages));
    return bytes_out(env, j.done());
}

extern "C" JNIEXPORT jboolean JNICALL Java_io_github_acnhrecomp_PrepareNative_nativeStart(JNIEnv* env, jclass, jbyteArray req) {
    const Request r = parse(bytes_in(env, req));
    prep::Options o;
    o.inputs = r.inputs;
    o.dlc = r.dlc;
    o.out = r.out;
    o.in_place = r.in_place;
    std::string bad;
    if (!prep::parse_platforms(r.platforms, o.platforms, bad)) return JNI_FALSE;
    o.variant_60fps = r.fps60;
    o.zig = r.zig, o.zig_lib = r.zig_lib, o.shader_tool = r.shader_tool, o.cache = r.cache;
    o.skip_recompile = r.skip_recompile;
    Shared& s = shared();
    std::lock_guard<std::mutex> l(s.m);
    if (!prep::start(o)) return JNI_FALSE;
    s.lines.clear();
    if (s.file) std::fclose(s.file);
    s.file = r.log.empty() ? nullptr : std::fopen(r.log.c_str(), "w");
    s.log_path = s.file ? r.log : "";
    s.ended = false;
    __android_log_print(ANDROID_LOG_INFO, "acnh_prepare", "started: %zu input(s), out %s", r.inputs.size(), r.out.c_str());
    return JNI_TRUE;
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_io_github_acnhrecomp_PrepareNative_nativeStatus(JNIEnv* env, jclass, jlong since) {
    Shared& s = shared();
    std::lock_guard<std::mutex> l(s.m);
    const prep::Status st = prep::status();  // takes the lines written since the last call: kept here for all
    for (const prep::LogLine& ln : st.log) {
        if (ln.text.empty()) continue;  // where a stage began: the stage itself is in the status
        if (s.file) std::fprintf(s.file, "[%s] %s\n", prep::stage_name(ln.stage), ln.text.c_str());
        __android_log_print(ANDROID_LOG_INFO, "acnh_prepare", "[%s] %s", prep::stage_name(ln.stage), ln.text.c_str());
        s.lines.emplace_back(s.next++, ln);
        if (s.lines.size() > kKeep) s.lines.pop_front();
    }
    if (st.finished && !s.ended) {
        s.ended = true;
        if (s.file) std::fprintf(s.file, "rc %d%s%s\n", st.rc, st.error.empty() ? "" : ": ", st.error.c_str());
        __android_log_print(ANDROID_LOG_INFO, "acnh_prepare", "rc %d %s", st.rc, st.error.c_str());
        copy_log(s, st.out);
    } else if (s.file && !st.finished) {
        std::fflush(s.file);
    }
    Json j;
    j.str("stage", prep::stage_name(st.stage));
    j.i64("stage_n", int(st.stage));
    j.num("fraction", st.fraction);
    j.num("overall", st.overall);
    j.str("item", st.item);
    j.i64("platform", st.platform);
    j.i64("platforms", st.platforms);
    j.boolean("running", st.running);
    j.boolean("finished", st.finished);
    j.i64("rc", st.rc);
    j.str("error", st.error);
    j.str("out", st.out);
    j.str("build_id", st.build_id);
    j.str("version", st.version);
    j.raw("messages", messages_json(st.messages));
    std::string log = "[";
    bool first = true;
    for (const auto& [n, ln] : s.lines) {
        if (n < since) continue;
        Json e;
        e.i64("n", n);
        e.str("stage", prep::stage_name(ln.stage));
        e.str("text", ln.text);
        log += (first ? "" : ",") + e.done();
        first = false;
    }
    j.raw("log", log + "]");
    j.i64("log_next", s.next);
    return bytes_out(env, j.done());
}

extern "C" JNIEXPORT void JNICALL Java_io_github_acnhrecomp_PrepareNative_nativeCancel(JNIEnv*, jclass) { prep::cancel(); }
