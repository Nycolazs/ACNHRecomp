// 60 fps with the game's own logic stepping at 60 Hz (video.frame_rate = 60native: NX_NATIVE60=1 with the present
// interval at 1). ACNH advances every system by a fixed step per frame (docs/STATUS.md, "60 fps"); here each system
// whose step lives in one place gets half of it per frame, through instruction hooks (games/acnh/hooks.txt). What is
// covered and what is not: docs/NATIVE60.md. NX_NATIVE60_TRACE=1 logs what the hooks see (a few lines per site).
#include <atomic>
#include <cstdlib>

#include <nx/runtime.h>

namespace nx {

bool native60() {
    static const bool on = [] {
        const char* e = std::getenv("NX_NATIVE60");
        bool v = e && e[0] == '1';
        if (v) log("acnh: native 60 Hz logic (NX_NATIVE60): the systems in docs/NATIVE60.md step half per frame");
        return v;
    }();
    return on;
}

namespace {

bool tracing() {
    static const bool on = std::getenv("NX_NATIVE60_TRACE") != nullptr;
    return on;
}

// The first few hits of a site, then one in 4096.
void trace(std::atomic<uint64_t>& hits, const char* what, CPU& c, float v) {
    uint64_t n = hits++;
    if (tracing() && (n < 6 || n % 4096 == 0))
        log("native60: %s %g (hit %llu, lr %s)", what, double(v), (unsigned long long)n, describe(c.x[30]).c_str());
}

// Halves s0, the step about to be used at a hooked instruction.
template <const char* What>
void half_step(CPU& c) {
    static std::atomic<uint64_t> hits;
    trace(hits, What, c, c.v[0].f32[0]);
    if (native60()) c.v[0].f32[0] *= 0.5f;
}

// The game's layout animation update (main+3a1d5e0, from the UI thread's layout calc at main+3c585e0): the step
// comes from this->vtable[0x128]() (1 per frame), then every animator in the list at this+0x70 gets Animate() and,
// while it plays, UpdateFrame(step) (vtable+0x18). Hooked where the step is kept (fmov s8, s0 at main+3a1d614). The
// game's animators are its own classes: nn::ui2d::Animator::UpdateFrame (main+2613b4) and AnimatorEx (main+2742dc)
// never run in the intro, and AnimTransformBasic's UpdateFrame is empty.
constexpr char kLayout[] = "layout step";
// The model animation update (main+38b2b00, x0 the animation group, x1 the model, s0 the step): each slot's frame
// control gets step = slot speed (x0+0x30) * s0 and advances by it (fmul s8, s1, s0 at main+38b2b34). Called each
// frame by the model calc (main+38d6dc0) with the scene's step (scene+0x55bc, 1), and with 0 right after a change.
// Only some models animate this way (a few thousand calls in the intro); the characters do not.
constexpr char kModelAnim[] = "model anim step";
// VfxMgr::calc (main+3a09200) runs nn::vfx::System::Calculate (main+23de30) for each of the 64 groups with the
// group's frame rate (VfxMgr+0x3f4 + 4 * group: 1, or 0 while paused) in s0: hooked on the call (main+3a09404).
// Emitter ages, emission timing and particle motion follow that rate.
constexpr char kVfx[] = "vfx frame rate";
// The Havok world step (main+4127c0, its timer marker is "TtPreStepSignal") as main+3e01a40 runs it each frame for
// its worlds, s0 = the step in seconds (1/30; hooked on the call, main+3e01f08). The one-time settling at
// main+3e02b58 (30 steps of 1/30 s) is not per frame and is left alone.
constexpr char kPhysics[] = "physics step";

// The character animation graph update (main+3dc3210: this, w1 "reset", w2, s0 step): each root node is evaluated
// with the step (main+3dbf770), which advances the nodes' frames, and the resulting (frame, weight) entries are
// applied to the model's animation slots (main+3dc10e0 -> play slot main+3751af8 / set frame main+38afe40). This is
// what moves the characters (Timmy, Tommy, the player, villagers). Hooked where the step is kept (fmov s8, s0 at
// main+3dc3298).
constexpr char kCharAnim[] = "character anim step";

}  // namespace

NX_HOOK_VERSIONS(0x3dc3298, 0x3dc32a8, half_step<kCharAnim>);  // 3.0.2, 3.0.3
NX_HOOK_VERSIONS(0x3a1d614, 0x3a1d624, half_step<kLayout>);  // 3.0.2, 3.0.3
NX_HOOK_VERSIONS(0x38b2b34, 0x38b2b44, half_step<kModelAnim>);  // 3.0.2, 3.0.3
NX_HOOK_VERSIONS(0x3a09404, 0x3a09414, half_step<kVfx>);  // 3.0.2, 3.0.3
NX_HOOK_VERSIONS(0x3e01f08, 0x3e01f18, half_step<kPhysics>);  // 3.0.2, 3.0.3

}  // namespace nx
