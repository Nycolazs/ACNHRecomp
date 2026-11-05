// nn::audio AudioOut at the API level. FMOD (inside main) mixes s16 stereo 48 kHz into AudioOutBuffers and appends
// them; a player thread hands each one to the platform's audio device (XAudio2 / AAudio / SDL3) and gives it back to
// the game (released + system event signalled) when it has been played, so the device clock paces FMOD's mixer.
// Without a device the same pacing runs on a timer. Audio input reports no devices. SDK 16 games (Wonder) use the
// AudioRenderer instead, rendered in process; its final mix goes to the same device (see below).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "audio_mix_ring.h"
#include "config.h"
#include "kernel.h"
#include "platform.h"

namespace nx {

namespace {

int active_output_channels();  // defined below; Android is always stereo

constexpr uint64_t kBytesPerSecond = 48000 * 2 * 2;
constexpr uint32_t kMaxInFlight = 4;  // buffers queued in the device (4 x ~21 ms)

// Our AudioOutBuffer layout (only our HLE reads it): {void* data; u64 capacity; u64 size}.
struct Out {
    std::mutex lock;
    std::condition_variable cv;
    std::deque<uint64_t> queued, released;
    std::shared_ptr<EventState> event = std::make_shared<EventState>();
    bool started = false;
};
Out g_out;
float g_out_volume = 1.0f;
std::atomic<uint64_t> g_out_appended{0}, g_out_played{0}, g_out_taken{0}, g_out_polls{0};

// Every 2 s while an AudioOut is open: how its buffers move (NX_AUDIO_OUT_STATS=1).
void out_stats() {
    static const bool on = std::getenv("NX_AUDIO_OUT_STATS") != nullptr;
    static uint64_t next = 0;
    if (!on) return;
    uint64_t now = plat::monotonic_ns();
    if (now < next) return;
    next = now + 2'000'000'000ull;
    log("audio: AudioOut %s: %llu appended, %llu played, %llu taken back (%llu polls); %zu queued, %zu released",
        g_out.started ? "started" : "stopped", (unsigned long long)g_out_appended.load(),
        (unsigned long long)g_out_played.load(), (unsigned long long)g_out_taken.load(),
        (unsigned long long)g_out_polls.load(), g_out.queued.size(), g_out.released.size());
}

// While an AudioRenderer is running it owns the device; AudioOut buffers (ACNH: the opening movie's sound) are then
// added into the renderer's final mix instead of queued after it. Bounded to 200 ms so a stalled renderer cannot
// make it grow.
std::mutex g_out_mix_lock;
AudioMixRing g_out_mix;
bool renderer_running();

void release(void* ctx) {
    std::lock_guard lk(g_out.lock);
    g_out_played++;
    g_out.released.push_back(reinterpret_cast<uint64_t>(ctx));
    g_out.event->signaled = true;
    notify_all();
    g_out.cv.notify_all();
}

// The platform device has one completion callback: AudioOut buffers are guest addresses, the renderer's blocks
// live in g_render (below) and go back to its free list.
void render_block_done(void* ctx);
bool render_owns(void* ctx);

void device_done(void* ctx) {
    if (render_owns(ctx))
        render_block_done(ctx);
    else
        release(ctx);
}

bool device_open() {
    static const bool ok = plat::audio_open(device_done, active_output_channels());
    return ok;
}

void player() {
    bool device = device_open();
    int chans = active_output_channels();
    log("audio: %s", device ? (chans == 6 ? "device output (48 kHz 5.1 s16)" : "device output (48 kHz stereo s16)")
                            : "no audio device, timer pacing");
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        uint64_t buf = 0, size = 0;
        {
            std::unique_lock lk(g_out.lock);
            auto ready = [&] {
                return g_out.started && !g_out.queued.empty() &&
                       (renderer_running() || !device || plat::audio_queued() < kMaxInFlight);
            };
            g_out.cv.wait_for(lk, std::chrono::milliseconds(5), ready);
            out_stats();
            if (!ready()) continue;
            buf = g_out.queued.front();
            g_out.queued.pop_front();
            size = ld<uint64_t>(buf + 16);
        }
        bool mix = renderer_running();
        if (mix) {
            const int16_t* pcm = reinterpret_cast<const int16_t*>(ld<uint64_t>(buf));
            std::lock_guard lk(g_out_mix_lock);
            for (uint64_t k = 0; k < size / 2; k++) g_out_mix.push(int16_t(float(pcm[k]) * g_out_volume));
        }
        if (!mix && device) {
            const void* send = reinterpret_cast<const void*>(ld<uint64_t>(buf));
            size_t send_size = size;
            if (active_output_channels() == 6) {  // the game's AudioOut buffer is stereo; the device is 6ch
                const int16_t* src = static_cast<const int16_t*>(send);
                size_t frames = size / 4;
                // Reuse allocations; direct AudioOut keeps at most kMaxInFlight buffers queued.
                static std::array<std::vector<int16_t>, 32> g_upmix;
                static size_t next_upmix = 0;
                auto& block = g_upmix[next_upmix];
                block.resize(frames * 6);
                for (size_t s = 0; s < frames; s++) {
                    int16_t l = src[2 * s], r = src[2 * s + 1];
                    block[s * 6 + 0] = l;                         // FL
                    block[s * 6 + 1] = r;                         // FR
                    block[s * 6 + 2] = int16_t((int32_t(l) + r) / 2);  // FC
                    block[s * 6 + 3] = 0;                         // LFE
                    block[s * 6 + 4] = l;                         // RL
                    block[s * 6 + 5] = r;                         // RR
                }
                send = block.data();
                send_size = block.size() * sizeof(int16_t);
                next_upmix = (next_upmix + 1) % g_upmix.size();
            }
            if (plat::audio_submit(send, send_size, reinterpret_cast<void*>(buf)))
                continue;
        }
        next = std::max(next, clock::now() - std::chrono::milliseconds(50));
        next += std::chrono::microseconds(size * 1000000 / kBytesPerSecond);
        std::this_thread::sleep_until(next);
        release(reinterpret_cast<void*>(buf));
    }
}

void InitializeAudioOutParameter(CPU& c) {  // {u32 sample_rate; u16 channels; u16 reserved}
    st<uint32_t>(c.x[0], 0);
    st<uint16_t>(c.x[0] + 4, 0);
    st<uint16_t>(c.x[0] + 6, 0);
}

void OpenDefaultAudioOut(CPU& c) {  // (AudioOut*, os::SystemEvent*, const AudioOutParameter&)
    uint64_t out = c.x[0], sysev = c.x[1];
    if (sysev) {
    // Give the guest a system event backed by our kernel event: nn::os::AttachSystemEvent(type, read, managed,
    // write, managed, clear mode = manual).
    auto w = std::make_shared<KEvent>();
    auto r = std::make_shared<KEvent>();
    w->state = r->state = g_out.event;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[0] = sysev;
    c.x[1] = hr;
    c.x[2] = 1;
    c.x[3] = hw;
    c.x[4] = 1;
    c.x[5] = 1;  // manual clear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    }
    log("audio: AudioOut opened (%u Hz, %u channels asked; %d output channels)", ld<uint32_t>(c.x[2]),
        ld<uint16_t>(c.x[2] + 4), active_output_channels());
    {
        std::lock_guard lk(g_out.lock);
        g_out.queued.clear(), g_out.released.clear();
    }
    st<uint64_t>(out, 0xA0D10);  // opaque
    static std::once_flag once;
    std::call_once(once, [] { std::thread(player).detach(); });
    c.x[0] = 0;
}

void StartAudioOut(CPU& c) {
    std::lock_guard lk(g_out.lock);
    g_out.started = true;
    g_out.cv.notify_all();
    c.x[0] = 0;
}

void StopAudioOut(CPU&) {
    log("audio: AudioOut stopped");
    std::lock_guard lk(g_out.lock);
    g_out.started = false;
    g_out.cv.notify_all();
}

void CloseAudioOut(CPU&) {
    log("audio: AudioOut closed");
    std::lock_guard lk(g_out.lock);
    g_out.started = false;
    g_out.queued.clear(), g_out.released.clear();
    g_out.cv.notify_all();
}

// (AudioOut*, const AudioOutParameter&): the same without a system event (the game polls for released buffers).
void OpenDefaultAudioOutNoEvent(CPU& c) {
    c.x[2] = c.x[1];
    c.x[1] = 0;
    OpenDefaultAudioOut(c);
}
void GetAudioOutSampleRate(CPU& c) { c.x[0] = 48000; }
void GetAudioOutChannelCount(CPU& c) { c.x[0] = active_output_channels(); }
void GetAudioOutSampleFormat(CPU& c) { c.x[0] = 2; }  // SampleFormat_PcmInt16
void SetAudioOutVolume(CPU& c) { g_out_volume = std::clamp(c.v[0].f32[0], 0.0f, 2.0f); }
void GetAudioOutBufferDataSize(CPU& c) { c.x[0] = ld<uint64_t>(c.x[0] + 16); }

void SetAudioOutBufferInfo(CPU& c) {  // (AudioOutBuffer*, void* data, size_t capacity, size_t size)
    st<uint64_t>(c.x[0], c.x[1]);
    st<uint64_t>(c.x[0] + 8, c.x[2]);
    st<uint64_t>(c.x[0] + 16, c.x[3]);
}

void GetAudioOutBufferDataPointer(CPU& c) { c.x[0] = ld<uint64_t>(c.x[0]); }

void AppendAudioOutBuffer(CPU& c) {  // (AudioOut*, AudioOutBuffer*)
    static uint64_t appended = 0;
    if (appended++ % 500 == 0)
        log("audio: AudioOut buffer %llu appended (%llu bytes)", (unsigned long long)appended,
            (unsigned long long)ld<uint64_t>(c.x[1] + 16));
    std::lock_guard lk(g_out.lock);
    g_out_appended++;
    g_out.queued.push_back(c.x[1]);
    g_out.cv.notify_all();
    c.x[0] = 1;  // bool: appended
}

void GetReleasedAudioOutBuffer(CPU& c) {
    std::unique_lock lk(g_out.lock);
    g_out_polls++;
    if (g_out.released.empty() && g_out.started && !g_out.queued.empty()) {
        g_out.cv.wait_for(lk, std::chrono::milliseconds(2), [] { return !g_out.released.empty() || !g_out.started; });
    }
    if (g_out.released.empty()) {
        c.x[0] = 0;
        return;
    }
    c.x[0] = g_out.released.front();
    g_out.released.pop_front();
    g_out_taken++;
    if (g_out.released.empty()) g_out.event->signaled = false;
}

void ListAudioIns(CPU& c) { c.x[0] = 0; }
void audio_in_fail(CPU& c) { c.x[0] = (1u << 9) | 153u; }  // audio: not found
void zero(CPU& c) { c.x[0] = 0; }

// SDK 16 (Super Mario Bros. Wonder): the AudioRenderer. The sdk carries the renderer's server side too and can
// reach it in process ("direct function call") instead of over IPC to audren:u: hand out that one.
// Both return nn::sf::SharedPointer<IAudioRendererManager> through x8.
void CreateAudioRendererManagerByHipc(CPU& c) {
    static uint64_t dfc = find_symbol("_ZN2nn5audio31CreateAudioRendererManagerByDfcEv");
    if (!dfc) fatal("CreateAudioRendererManagerByDfc not found");
    log("audio: AudioRenderer manager in process (DFC) instead of audren:u");
    call(c, dfc);
}

// The AudioRenderer, in process (DFC). On the console the ADSP renders one audio frame every 5 ms and signals the
// renderer's system event, which paces the game's sound thread. In process nothing does that on its own: the
// renderer object (AudioRendererManagerImpl::AudioRendererImpl) has ExecuteAudioRendererRendering, which generates
// the frame's commands and runs the sdk's own DSP command processor on the calling thread, and its render system
// (impl + 0x60) signals its own event, which has no kernel object in process. A guest thread of ours renders every
// 5 ms and signals the game's event itself, then sends the frame's final mix to the host device.
// Wonder's sdk (SDK 16.2) has no symbols for these, so they were fixed offsets; SDK 20 (ACNH) exports both, and the
// symbol wins when it is there.
constexpr uint64_t kRendererVtable = 0xd04910;     // sdk 16.2: vtable of the sf object wrapping AudioRendererImpl
constexpr uint64_t kExecuteRendering = 0x20cac8;   // sdk 16.2: AudioRendererImpl::ExecuteAudioRendererRendering
constexpr const char* kRendererVtableSym =
    "_ZTVN2nn2sf6detail38ObjectImplFactoryWithStatefulAllocatorINS0_13InterfaceInfoINS_5audio6detail14IAudioRenderer"
    "EE4_tABINS7_5_tO2NINS4_6server24AudioRendererManagerImpl17AudioRendererImplEE4typeES6_E4typeENS0_24StatefulAllocat"
    "ionPolicyINS0_16ExpHeapAllocatorEEEE6ObjectE";
constexpr const char* kExecuteRenderingSym =
    "_ZN2nn5audio6server24AudioRendererManagerImpl17AudioRendererImpl29ExecuteAudioRendererRenderingEv";

// The in-process render system hands the client a readable handle of 0 for the renderer's event (there is no
// kernel event behind it), so the SystemEvent the game waits on is ours: an InterProcessEventType keeps its
// readable handle at +0x14 and its writable one at +0x18 (nn::os::detail::WaitInterProcessEvent, sdk+1305a0).
std::shared_ptr<EventState> g_audio_frame = std::make_shared<EventState>();

std::mutex g_renderers_lock;
std::vector<uint64_t> g_renderers;  // AudioRendererImpl*

bool renderer_running() {
    std::lock_guard lk(g_renderers_lock);
    return !g_renderers.empty();
}

uint64_t sdk_base() {
    static uint64_t base = [] {
        const LoadedModule* m = module_at(find_symbol("nvnBootstrapLoader"));
        return m ? m->base : 0;
    }();
    return base;
}

uint64_t renderer_vtable() {
    static uint64_t v = [] { uint64_t a = find_symbol(kRendererVtableSym); return a ? a : sdk_base() + kRendererVtable; }();
    return v;
}

uint64_t execute_rendering() {
    static uint64_t f = [] { uint64_t a = find_symbol(kExecuteRenderingSym); return a ? a : sdk_base() + kExecuteRendering; }();
    return f;
}

// The AudioRendererImpl behind an AudioRendererHandle: an sf object whose vtable pointer is that one, within two
// pointer hops of the handle.
uint64_t find_renderer(uint64_t handle) {
    uint64_t vptr = renderer_vtable() + 0x10;
    auto valid = [](uint64_t p) { return p >= kModuleBase && p < kKernelBase + kKernelSize && (p & 7) == 0; };
    for (uint64_t a = 0; a < 0x40; a += 8) {
        uint64_t p = ld<uint64_t>(handle + a);
        if (!valid(p)) continue;
        if (ld<uint64_t>(p) == vptr) return p + 0x10;
        for (uint64_t b = 0; b < 0x40; b += 8) {
            uint64_t q = ld<uint64_t>(p + b);
            if (valid(q) && ld<uint64_t>(q) == vptr) return q + 0x10;
        }
    }
    return 0;
}

// ---- the renderer's final mix to the host device ----
// The command list ends with a DeviceSink command (id 19) naming the final mix's buffers, but the sdk's DSP command
// processor skips it: on the console the ADSP's sink stage hands those buffers to the output device. So
// CommandListProcessor::Process is wrapped: after the real one has run the list, the sink's input mix buffers (s32
// samples in the PCM16 range, one buffer per channel) are summed into g_render.mix; the render thread converts each
// 5 ms frame to s16 stereo and queues it on the device. Command header: {u32 magic 0xcafebabe; u8 enabled; s8 id;
// s16 size; u32 estimated time}. DeviceSink (CommandBuffer::Generate<DeviceSink>, sdk+211e10): +0x128 the mix
// buffers (or the upsampler's), +0x130 input count, +0x134 s16 buffer index per input.
constexpr uint8_t kCmdDownMix6chTo2ch = 17, kCmdDeviceSink = 19;  // SDK 16.2

// Command ids moved in later SDKs (20.5: DeviceSink is 20, same layout). CommandGenerator::GenerateDeviceSinkCommand
// ends by calling CommandBuffer's generator, which writes the header: `mov w8, #id` then `strb w8, [x20, #5]`.
// The id is read from there; DownMix6chTo2ch keeps its distance to it (two below).
uint8_t device_sink_id() {
    static uint8_t id = [] {
        uint64_t f = find_symbol("_ZN2nn5audio6server16CommandGenerator25GenerateDeviceSinkCommandEiPNS1_12SinkInfoBaseE");
        if (!f) return kCmdDeviceSink;
        uint64_t gen = 0;
        for (uint64_t pc = f; pc < f + 0x100; pc += 4) {
            uint32_t w = ld<uint32_t>(pc);
            if ((w & 0xFC000000) == 0x94000000) gen = pc + uint64_t(int64_t(int32_t(w << 6) >> 4));  // bl
            if (w == 0xD65F03C0) break;                                                           // ret
        }
        uint32_t last = ~0u;
        for (uint64_t pc = gen; gen && pc < gen + 0x80; pc += 4) {
            uint32_t w = ld<uint32_t>(pc);
            if ((w & 0xFFE0001F) == 0x52800008) last = (w >> 5) & 0xFFFF;  // mov w8, #imm16
            if (w == 0x39001688 && last < 0x100) {                           // strb w8, [x20, #5]
                log("audio: DeviceSink command id %u", last);
                return uint8_t(last);
            }
        }
        return kCmdDeviceSink;
    }();
    return id;
}
constexpr int kFrame = 240;          // samples per 5 ms at 48 kHz
constexpr int kBlocks = 64;          // 320 ms of output blocks
constexpr uint32_t kMaxQueued = 12;  // 60 ms in the device queue: past that, frames are dropped
constexpr size_t kPrime = 4;         // after an underrun, 20 ms are gathered before playing again

// The output the game sees at startup: Windows/Linux/macOS report 6 channels (FL FR FC LFE RL RR) when
// audio.surround is on; Android always reports 2. The setting applies on the next device open/restart.
int active_output_channels() {
#if defined(__ANDROID__)
    return 2;  // Android's AAudio backend is stereo-only.
#else
    static const int ch = config_get_bool("audio.surround", false) ? 6 : 2;
    return ch;
#endif
}

constexpr int kMaxChannels = 6;

struct RenderOut {
    std::mutex lock;
    int32_t mix[kFrame * kMaxChannels] = {};
    bool mixed = false;
    int16_t blocks[kBlocks][kFrame * kMaxChannels];
    std::vector<int> free, held;
    bool priming = true;
    // statistics since the last log, and the optional dump (NX_AUDIO_DUMP=file.wav: 10 s from the first sound)
    uint64_t frames = 0, dropped = 0, underruns = 0, silent = 0;
    double sum_sq = 0;
    int peak = 0;
    std::vector<int16_t> dump;
    RenderOut() {
        for (int k = kBlocks - 1; k >= 0; k--) free.push_back(k);
    }
};
RenderOut g_render;

bool render_owns(void* ctx) {
    auto p = static_cast<const uint8_t*>(ctx), b = reinterpret_cast<const uint8_t*>(g_render.blocks);
    return p >= b && p < b + sizeof g_render.blocks;
}

void render_block_done(void* ctx) {
    std::lock_guard lk(g_render.lock);
    g_render.free.push_back(int((static_cast<int16_t*>(ctx) - g_render.blocks[0]) / (kFrame * 6)));
}

void capture_device_sink(uint64_t proc) {  // proc: nn::audio::dsp::CommandListProcessor after Process
    uint64_t cmd = ld<uint64_t>(proc) + 0x20;  // the list header, then the commands (+8 is the cursor: at the end)
    uint32_t count = ld<uint32_t>(proc + 0x1c), samples = ld<uint32_t>(proc + 0x20), rate = ld<uint32_t>(proc + 0x28);
    uint32_t frames = rate == 48000 ? samples : kFrame;  // other rates: the upsampler's 48 kHz buffers
    bool downmixed = false;
    for (uint32_t i = 0; i < count && ld<uint32_t>(cmd) == 0xcafebabe; i++, cmd += ld<int16_t>(cmd + 6)) {
        if (!ld<uint8_t>(cmd + 4)) continue;
        uint8_t id = ld<uint8_t>(cmd + 5);
        if (id == device_sink_id() - (kCmdDeviceSink - kCmdDownMix6chTo2ch)) downmixed = true;
        if (id != device_sink_id()) continue;
        uint64_t buffers = ld<uint64_t>(cmd + 0x128);
        uint32_t inputs = ld<uint32_t>(cmd + 0x130);
        static bool once = false;
        if (!once) {
            once = true;
            log("audio: device sink '%s', %u inputs%s, %u samples at %u Hz", reinterpret_cast<const char*>(cmd + 0x20),
                inputs, downmixed ? " (downmixed to stereo)" : "", samples, rate);
        }
        if (!buffers || !inputs || frames != kFrame) continue;
        auto ch = [&](uint32_t k) {
            return reinterpret_cast<const int32_t*>(buffers + uint64_t(ld<int16_t>(cmd + 0x134 + 2 * k)) * frames * 4);
        };
        const int32_t* l = ch(0);
        const int32_t* r = inputs > 1 ? ch(1) : l;
        // Surround mode keeps six channels discrete. Stereo folds center/rears down, unless the sdk already did so.
        const int32_t *c2 = nullptr, *lfe = nullptr, *rl = nullptr, *rr = nullptr;
        bool discrete = active_output_channels() == 6;
        if (discrete) {  // the mix may carry fewer than 6 buffers; only bind the ones it has
            c2  = inputs > 2 ? ch(2) : nullptr;
            lfe = inputs > 3 ? ch(3) : nullptr;
            rl  = inputs > 4 ? ch(4) : nullptr;
            rr  = inputs > 5 ? ch(5) : nullptr;
        }
        std::lock_guard lk(g_render.lock);
        for (int s = 0; s < kFrame; s++) {
            if (discrete) {
                g_render.mix[6 * s + 0] += int32_t(std::clamp<int64_t>(l[s], INT32_MIN, INT32_MAX));
                g_render.mix[6 * s + 1] += int32_t(std::clamp<int64_t>(r[s], INT32_MIN, INT32_MAX));
                g_render.mix[6 * s + 2] += c2 ? int32_t(std::clamp<int64_t>(c2[s], INT32_MIN, INT32_MAX)) : 0;
                g_render.mix[6 * s + 3] += lfe ? int32_t(std::clamp<int64_t>(lfe[s], INT32_MIN, INT32_MAX)) : 0;
                g_render.mix[6 * s + 4] += rl ? int32_t(std::clamp<int64_t>(rl[s], INT32_MIN, INT32_MAX)) : 0;
                g_render.mix[6 * s + 5] += rr ? int32_t(std::clamp<int64_t>(rr[s], INT32_MIN, INT32_MAX)) : 0;
                continue;
            }
            if (inputs >= 6 && !downmixed) c2 = ch(2), rl = ch(4), rr = ch(5);
            int64_t a = l[s], b = r[s];
            if (c2) {
                a += (int64_t(c2[s]) + rl[s]) * 707 / 1000;
                b += (int64_t(c2[s]) + rr[s]) * 707 / 1000;
            }
            g_render.mix[2 * s] += int32_t(std::clamp<int64_t>(a, INT32_MIN, INT32_MAX));
            g_render.mix[2 * s + 1] += int32_t(std::clamp<int64_t>(b, INT32_MIN, INT32_MAX));
        }
        g_render.mixed = true;
    }
}

// AudioRenderSystem::Update (the game's RequestUpdate) takes the render system's mutex, but GenerateCommand runs
// without it: on the console both run on the audio system's side, one after the other. Here the game's sound thread
// updates while our render thread generates, and an effect being updated mid-generation crashed
// (EffectContext::UpdateStateByDspShared on a null effect). Both are serialized by this lock.
std::mutex g_render_system;

// One update per rendered frame, as on the console: an update copies the game's parameters and a new effect's
// "initialize" status rides on exactly one of them. Our render thread signals the game's audio event every 5 ms
// whether or not it rendered since the last update, so a sound thread that fell behind updated twice in a row, the
// second update overwrote the status, and the next render applied a LightLimiter whose state was never initialized
// (null pointers in ApplyLightLimiterEffect, an intermittent crash at startup; docs/nvn/DEBUG.md #28). An update
// now waits (at most 1 s, outside the lock) until a render ran since the previous one.
std::mutex g_pace_lock;
std::condition_variable g_pace_cv;
uint64_t g_renders = 0, g_renders_at_update = ~0ull;

void AudioRenderSystemUpdate(CPU& c) {
    static uint64_t real = find_symbol("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm");
    {
        std::unique_lock pk(g_pace_lock);
        static uint64_t waited = 0;
        if (g_renders == g_renders_at_update && waited++ < 5) log("audio: update waits for a render");
        // 1 s, not 50 ms: at boot, with every thread loading, a render took longer and the second update went through
        // (the limiter crash again, once in ~25 Windows runs). The render thread runs every 5 ms: 1 s only bounds a stall.
        if (!g_pace_cv.wait_for(pk, std::chrono::seconds(1), [] { return g_renders != g_renders_at_update; }))
            log("audio: no render for 1 s since the last update: updating anyway");
    }
    std::lock_guard lk(g_render_system);
    call(c, real);
    // Counted after the update, under the render lock (renders count themselves under it too): a render that began
    // before this update and ended after it no longer passes for one that used it (the limiter crash, DEBUG-2 #74).
    std::lock_guard pk(g_pace_lock);
    g_renders_at_update = g_renders;
}

void CommandListProcessorProcess(CPU& c) {
    static uint64_t real = find_symbol("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv");
    uint64_t proc = c.x[0];
    call(c, real);
    capture_device_sink(proc);
}

void write_wav(const char* path, const std::vector<int16_t>& pcm, int channels) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = uint32_t(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f), u32(36 + bytes), std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(uint16_t(channels)), u32(48000), u32(uint32_t(48000 * channels * 2)), u16(uint16_t(channels * 2)),
        u16(16);
    std::fwrite("data", 1, 4, f), u32(bytes), std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
    log("audio: wrote %s (%zu frames)", path, pcm.size() / size_t(channels));
}

// Called by the render thread after each 5 ms frame.
void render_submit() {
    static const bool device = device_open();
    static const char* dump_path = std::getenv("NX_AUDIO_DUMP");
    std::lock_guard lk(g_render.lock);
    RenderOut& o = g_render;
    if (!o.mixed) return;
    o.mixed = false;
    int chans = active_output_channels();
    int n = chans == 6 ? kFrame * 6 : kFrame * 2;
    int16_t pcm[kFrame * kMaxChannels];
    bool any = false;
    {
        static int last_chans = 0;  // a live stereo <-> surround switch must not scramble the mix source
        if (last_chans != chans) {
            last_chans = chans;
            std::lock_guard mk(g_out_mix_lock);
            g_out_mix.clear();
        }
        std::lock_guard mk(g_out_mix_lock);
        if (chans == 6) {
            for (int s = 0; s < kFrame; s++) {
                if (g_out_mix.size() < 2) break;
                int16_t l = 0, r = 0;
                g_out_mix.pop_front(l);
                g_out_mix.pop_front(r);
                o.mix[6 * s + 0] += int64_t(l);
                o.mix[6 * s + 1] += int64_t(r);
            }
        } else {
            int16_t sample = 0;
            for (int s = 0; s < kFrame * 2 && g_out_mix.pop_front(sample); s++) {
                o.mix[s] += sample;
            }
        }
    }
    for (int s = 0; s < n; s++) {
        int v = std::clamp(o.mix[s], -32768, 32767);
        pcm[s] = int16_t(v);
        o.mix[s] = 0;
        o.peak = std::max(o.peak, std::abs(v));
        o.sum_sq += double(v) * v;
        any |= v != 0;
    }
    o.silent += !any;
    if (dump_path && o.dump.size() < size_t(48000 * chans * 10) && (any || !o.dump.empty())) {
        o.dump.insert(o.dump.end(), pcm, pcm + n);
        if (o.dump.size() >= size_t(48000 * chans * 10)) write_wav(dump_path, o.dump, chans);
    }
    if (++o.frames % 2000 == 0) {
        log("audio: mix out: peak %d, rms %.0f, %llu of 2000 frames silent, %llu dropped, %llu underruns, queue %u",
            o.peak, std::sqrt(o.sum_sq / (2000.0 * n)), (unsigned long long)o.silent,
            (unsigned long long)o.dropped, (unsigned long long)o.underruns, device ? plat::audio_queued() : 0);
        o.peak = 0, o.sum_sq = 0, o.silent = o.dropped = o.underruns = 0;
    }
    if (!device) return;
    uint32_t queued = plat::audio_queued();
    if (queued + o.held.size() >= kMaxQueued || o.free.empty()) {  // the device fell behind: keep latency bounded
        o.dropped++;
        return;
    }
    if (queued == 0 && !o.priming) o.priming = true, o.underruns++;
    int k = o.free.back();
    o.free.pop_back();
    size_t block_bytes = size_t(kFrame) * size_t(chans) * 2;
    std::memcpy(o.blocks[k], pcm, block_bytes);
    o.held.push_back(k);
    if (o.priming && o.held.size() < kPrime) return;
    o.priming = false;
    for (int h : o.held) plat::audio_submit(o.blocks[h], block_bytes, o.blocks[h]);
    o.held.clear();
}

void audio_render_thread(CPU& c) {
    log("audio: render thread running");
    uint64_t next = plat::monotonic_ns();
    for (;;) {
        next += 5'000'000;
        int64_t wait = int64_t(next - plat::monotonic_ns());
        if (wait > 0) plat::sleep_ns(wait);
        std::vector<uint64_t> list;
        {
            std::lock_guard lk(g_renderers_lock);
            list = g_renderers;
        }
        static uint64_t frames = 0, busy_ns = 0, last_rc = 0;
        uint64_t t0 = plat::monotonic_ns();
        for (uint64_t impl : list) {
            uint64_t saved[31];
            std::memcpy(saved, c.x, sizeof saved);
            c.x[0] = impl;
            std::lock_guard lk(g_render_system);
            call(c, execute_rendering());
            last_rc = c.x[0];
            std::memcpy(c.x, saved, sizeof saved);
            std::lock_guard pk(g_pace_lock);  // counted under the render lock (AudioRenderSystemUpdate)
            g_renders++;
            g_pace_cv.notify_all();
        }
        busy_ns += plat::monotonic_ns() - t0;
        render_submit();
        if (++frames % 2000 == 0)
            log("audio: %llu frames rendered, %.1f us each, last result %llx", (unsigned long long)frames,
                double(busy_ns) / 1000.0 / 2000.0, (unsigned long long)last_rc), busy_ns = 0;
        g_audio_frame->signaled = true;
        notify_all();
    }
}

void start_render_thread(CPU& c) {
    uint64_t create = find_symbol("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    uint64_t start = find_symbol("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    if (!create || !start) {
        log("audio: nn::os::CreateThread/StartThread not found, no render thread");
        return;
    }
    uint64_t thread = kernel_alloc(0x1000), stack = kernel_alloc(0x40000);
    uint64_t saved[31];
    std::memcpy(saved, c.x, sizeof saved);
    c.x[0] = thread, c.x[1] = hle_address("__audio_render_thread"), c.x[2] = 0, c.x[3] = stack, c.x[4] = 0x40000;
    c.x[5] = 16;  // high priority, like the audio system's own threads
    call(c, create);
    uint64_t rc = c.x[0];
    c.x[0] = thread;
    if (rc == 0) call(c, start);
    std::memcpy(c.x, saved, sizeof saved);
    if (rc) log("audio: CreateThread failed (%llx)", (unsigned long long)rc);
}

// OpenAudioRenderer(AudioRendererHandle*, os::SystemEvent*, const AudioRendererParameter&, void* work, size_t)
void OpenAudioRenderer(CPU& c) {
    static uint64_t real = find_symbol(
        "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm");
    uint64_t handle = c.x[0], sysev = c.x[1];
    call(c, real);
    if (c.x[0] == 0 && ld<uint32_t>(sysev + 0x14) == 0) {
        auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
        r->state = w->state = g_audio_frame;
        st<uint32_t>(sysev + 0x14, handle_add(r));
        st<uint32_t>(sysev + 0x18, handle_add(w));
    }
    uint64_t rc = c.x[0];
    uint64_t impl = rc == 0 ? find_renderer(handle) : 0;
    log("audio: renderer opened (result %llx), impl %llx", (unsigned long long)rc, (unsigned long long)impl);
    if (!impl) return;
    // ExecuteAudioRendererRendering only renders in the manual execution mode on the CPU rendering device
    // (AudioRenderSystem::GetExecutionMode / GetRenderingDevice == 1, at render system + 0x2d0 / + 0x2d4); the game
    // opens the renderer in the automatic mode, where the ADSP renders on its own. Our 5 ms thread is that ADSP, so
    // the renderer is switched to manual/CPU; otherwise every call failed with 0x40299 and nothing was ever rendered
    // (streamed music never advanced, and the opening cutscene waited for it forever).
    // Where the two fields are moves between SDKs (16.2: +0x2d0; 20.5: +0x48; ACNH 1.0.0: +0x2d8; 2.0.7: +0x2b8).
    // The sdks that export AudioRenderSystem::GetExecutionMode have it as `ldr w0, [x0, #off]`; SDK 20.5 has no such
    // symbol and AudioRenderSystem::ExecuteAudioRendererRendering starts by loading both with `ldp w8, w9,
    // [x0, #off]`. The offset is read from whichever is there. Writing at a stale offset corrupts the render system (a
    // mutex: StartAudioRenderer then deadlocks; in ACNH 2.0.7 every frame failed with 0x40299 and the sound handles
    // the game stops and waits for never stopped: the island never finished loading).
    uint64_t system = impl + 0x60;
    uint64_t mode_off = 0x2d0;
    if (uint64_t f = find_symbol("_ZNK2nn5audio6server17AudioRenderSystem16GetExecutionModeEv");
        f && (ld<uint32_t>(f) & 0xFFC003FF) == 0xB9400000) {  // ldr w0, [x0, #imm12 * 4]
        mode_off = uint64_t((ld<uint32_t>(f) >> 10) & 0xFFF) * 4;
    } else if (uint64_t g = find_symbol("_ZN2nn5audio6server17AudioRenderSystem29ExecuteAudioRendererRenderingEv")) {
        uint32_t w = ld<uint32_t>(g);
        if ((w & 0xFFC003E0) == 0x29400000)  // ldp wT1, wT2, [x0, #imm7 * 4]
            mode_off = uint64_t(int64_t(int32_t(w << 10) >> 25) * 4);
    }
    log("audio: execution mode %u, rendering device %u (render system +%#llx) -> 1, 1",
        ld<uint32_t>(system + mode_off), ld<uint32_t>(system + mode_off + 4), (unsigned long long)mode_off);
    st<uint32_t>(system + mode_off, 1);
    st<uint32_t>(system + mode_off + 4, 1);
    {
        std::lock_guard lk(g_renderers_lock);
        g_renderers.push_back(impl);
    }
    static bool started = false;
    if (!started) {
        started = true;
        start_render_thread(c);
    }
    c.x[0] = rc;
}

// The audio device (output) side: one stereo output that never changes.
constexpr const char* kOutputName = "AudioTvOutput";

void AcquireAudioDeviceNotificationForOutput(CPU& c) {  // (os::SystemEvent*): an event that is never signaled
    auto w = std::make_shared<KEvent>(), r = std::make_shared<KEvent>();
    r->state = w->state = std::make_shared<EventState>();
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // x0: the SystemEvent's SystemEventType
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    c.x[0] = 0;
}

void ListAudioOutputDeviceName(CPU& c) {  // int (AudioDeviceName* names, int count): 256 chars each
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 256);
    std::strcpy(reinterpret_cast<char*>(c.x[0]), kOutputName);
    c.x[0] = 1;
}

void GetActiveAudioOutputDeviceName(CPU& c) {  // (AudioDeviceName*)
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 256);
    std::strcpy(reinterpret_cast<char*>(c.x[0]), kOutputName);
    c.x[0] = 0;
}

void two(CPU& c) { c.x[0] = active_output_channels(); }

}  // namespace

NX_HLE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", AcquireAudioDeviceNotificationForOutput);
NX_HLE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", ListAudioOutputDeviceName);
NX_HLE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", GetActiveAudioOutputDeviceName);
NX_HLE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", zero);
NX_HLE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", two);
NX_HLE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", InitializeAudioOutParameter);
NX_HLE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", OpenDefaultAudioOut);
NX_HLE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", OpenDefaultAudioOutNoEvent);
NX_HLE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", GetAudioOutSampleRate);
NX_HLE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", GetAudioOutChannelCount);
NX_HLE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", GetAudioOutSampleFormat);
NX_HLE("_ZN2nn5audio17SetAudioOutVolumeEPNS0_8AudioOutEf", SetAudioOutVolume);
NX_HLE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", GetAudioOutBufferDataSize);
NX_HLE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", StartAudioOut);
NX_HLE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", StopAudioOut);
NX_HLE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", CloseAudioOut);
NX_HLE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", SetAudioOutBufferInfo);
NX_HLE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", GetAudioOutBufferDataPointer);
NX_HLE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", AppendAudioOutBuffer);
NX_HLE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", GetReleasedAudioOutBuffer);
NX_HLE("_ZN2nn5audio12ListAudioInsEPNS0_11AudioInInfoEi", ListAudioIns);
NX_HLE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPKcRKNS0_16AudioInParameterE", audio_in_fail);
NX_HLE("_ZN2nn5audio26InitializeAudioInParameterEPNS0_16AudioInParameterE", zero);
NX_HLE("_ZN2nn5audio32CreateAudioRendererManagerByHipcEv", CreateAudioRendererManagerByHipc);
NX_HLE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", OpenAudioRenderer);
NX_HLE("__audio_render_thread", audio_render_thread);
NX_HLE("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv", CommandListProcessorProcess);
NX_HLE("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm", AudioRenderSystemUpdate);

}  // namespace nx

namespace nx {
namespace {

// NX_AUDIO_REVERB=0 (the light presets): the AudioRenderer's I3DL2 reverb runs bypassed. On the console the ADSP
// renders it; here it is recompiled NEON on a CPU core, and in ACNH it was the costliest function of the process
// (~20% of all CPU samples, one core busy). ApplyI3dl2ReverbEffect(parameter, state, enabled, inputs, outputs,
// samples) copies the inputs through when not enabled: the sound keeps everything but the room reverb.
void ApplyI3dl2ReverbEffect(CPU& c) {
    static const uint64_t real = find_symbol(
        "_ZN2nn5audio22ApplyI3dl2ReverbEffectERKNS0_20I3dl2ReverbParameterEPNS0_16I3dl2ReverbStateEbPPKiPPii");
    // NX_AUDIO_REVERB=0 at startup, or the settings screen's audio.reverb switched live.
    static const bool env_off = [] {
        const char* e = std::getenv("NX_AUDIO_REVERB");
        bool o = e && e[0] == '0';
        if (o) log("audio: I3DL2 reverb bypassed (NX_AUDIO_REVERB=0)");
        return o;
    }();
    static uint64_t gen = 0;
    static bool cfg_off = false;
    if (uint64_t g = config_generation(); g != gen) gen = g, cfg_off = !config_get_bool("audio.reverb", true);
    const bool off = env_off || cfg_off;
    if (!real) return;
    if (off) c.x[2] = 0;
    // NX_PROFILE_REVERB=1: a fixed workload to compare recompiler builds. The 500th enabled call is repeated 3000 times
    // on the same arguments (its output buffers are rewritten each time) and the median thread CPU time is logged.
    static const bool prof = std::getenv("NX_PROFILE_REVERB") != nullptr;
    static int seen = 0;
    if (prof && !off && c.x[2] && ++seen == 500) {
        auto now = [] {
#if defined(_WIN32)  // no thread CPU clock in the C runtime: wall time (run it on an idle machine)
            return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
#else
            timespec ts;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
        };
        const CPU saved = c;
        std::vector<uint64_t> t(3000);
        for (auto& v : t) {
            c = saved;
            const uint64_t t0 = now();
            call(c, real);
            v = now() - t0;
        }
        std::sort(t.begin(), t.end());
        log("audio: reverb benchmark: median %llu ns, best %llu ns per call", (unsigned long long)t[t.size() / 2],
            (unsigned long long)t[0]);
        c = saved;
    }
    call(c, real);
}

}  // namespace

NX_HLE("_ZN2nn5audio22ApplyI3dl2ReverbEffectERKNS0_20I3dl2ReverbParameterEPNS0_16I3dl2ReverbStateEbPPKiPPii",
       ApplyI3dl2ReverbEffect);

}  // namespace nx
