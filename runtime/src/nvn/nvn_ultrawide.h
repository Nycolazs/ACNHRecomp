// Screen scale (docs/nvn/studies/ultrawide-1710.md): the game renders a fixed 16:9 picture (its render targets are
// built from 1920x1080 immediates and its cameras assume 16:9). The runtime resizes every 16:9 colour/depth render
// target ("screen targets") by (sx, sy) = s * (max(1, k), max(1, 1/k)), where s is the internal resolution over the
// game's (video.resolution) and k the picture's aspect over 16:9 (video.aspect, or the window's). Viewports, scissors,
// clears, copies and the window crop follow; the translated shaders get the resolution part through Ryujinx's render
// scale (support buffer: gl_FragCoord, texel fetches and size queries in the game's pixels). The game's screen-space
// passes take their UVs from the sampled target's size, so the aspect part needs nothing there; the UI is squeezed back
// to 16:9 by a gl_Position scale (specialization constants).
// Wider pictures show more to the sides (Hor+), narrower ones more above and below (Vert+), with the 16:9 UI kept at
// its shape in the middle. Both may change while the game runs: the targets are made again at a new generation.
#pragma once

#include "nvn_per_thread.h"

#include <cstdint>
#include <vector>

namespace nx::nvn {

struct ShaderInfo;
struct Texture;
struct GpuImage;

float uw_factor();                       // k of the current generation (1: 16:9)
float uw_resolution();                   // s of the current generation (the internal resolution over the game's)
float uw_resolution_at_boot();           // s of the first generation (taken now if no target was made yet)
inline bool uw_on() { return uw_factor() != 1.0f; }
bool uw_scaling();                       // any resizing at all in the current generation
bool uw_widen_texture(Texture& t);       // nvnTextureInitialize: a screen target is marked (t.uw) and sized
void uw_resize(Texture& t);              // a screen target sized for the current generation (image_of)
float uw_render_ratio();                 // the current generation's 3D scene share (temporal AA upscaling)
uint32_t uw_generation();
void uw_update(uint64_t present);        // game thread, each present: settings and window -> the wanted scale
// A new scale starts with the first frame the game records after it is wanted (its cameras are set at that present):
// a submit carries the wanted generation of when it was queued (uw_allow, on the replay thread before gpu_submit),
// and gpu_submit makes it current (uw_apply, under the GPU lock).
uint32_t uw_wanted_generation();
void uw_allow(uint32_t wanted_gen);
void uw_apply();
void uw_crop(const GpuImage& img, const int32_t in[4], int32_t out[4]);  // nvnWindowSetCrop's rect on the image

// Set by the draw replay before it builds descriptor sets and state: the draw's colour/depth target, when it is a
// scaled screen target, and its factors.
struct UwTarget {
    bool on = false;
    float sx = 1, sy = 1;
    float ax() const { return sy < sx ? sy / sx : 1.0f; }  // the aspect part, <= 1 (the UI squeeze)
    float ay() const { return sx < sy ? sx / sy : 1.0f; }
    float s() const { return sx < sy ? sx : sy; }           // the resolution part (Ryujinx's render scale)
};
extern PerThread<UwTarget> g_uw;  // per replaying thread
// A guest viewport/scissor span on a scaled target: offset and size times `f`, kept inside `extent`.
void uw_span(int32_t& x, int32_t& w, uint32_t extent, float f);
void uw_cameras(uint64_t present);     // camera aspect patch (called at each present)
bool uw_ortho_on();                    // the UI's projections are widened at the source (no UI squeeze)
void uw_scan(uint64_t present);        // NX_UW_SCAN debug
void uw_ui_vertex_shader(const ShaderInfo* vs);  // learned: drew into a UI layer target
bool uw_is_ui_vertex_shader(const ShaderInfo* vs);
bool uw_vs_scaled(const ShaderInfo* vs);  // does this vertex shader's position get the UI squeeze
// A copy of a vertex module whose gl_Position.xy is multiplied by specialization constants 0 and 1 (default 1.0).
bool patch_x_scale(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

}  // namespace nx::nvn
