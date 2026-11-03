// ACNH's tonemap (fragment shaders 64ae17e74cc3b846, and 3e1bdb382f32d6fe with the room's colour LUT), rebuilt with
// the translated shaders' interface and extended for HDR. tonemap_<hash>.hlsl are compiled into the runtime
// (tools/gen_spirv_inc.py -> runtime/src/nvn/nvn_hdr_tonemap_spirv.inc), which uses them instead of the game's shaders
// while video.hdr is on; a <data root>/spirv_hdr/<hash>.spv overrides them (nvn_shaders.cpp read_spirv).
//
// Vanilla (from the translated SPIR-V):
//   c = (scene + bloom) * e^exposure                      scene RGBA16F 1920x1080, bloom RGBA16F 480x272
//   Y = 0.298912 r + c1[0].x g + c1[0].y b,  T = 1 - e^-Y
//   v = lerp(c * T / Y, 1 - e^-c, T^2)                    luminance tonemap, per channel toward white as T grows
//   v = max + saturation * (v - max)                      saturation c3[1].z
//   v = LUT(v * c3[1].x + c3[1].y)                        3e1bdb382f32d6fe only
// written to an RGB10A2 UNORM target in linear light: the UI is blended over it (linear), and a copy writes it to the
// window's RGBA8 texture through an sRGB view (the encode). With HDR both targets are RGBA16F, the sRGB view is
// dropped, and present_hdr takes the linear picture (gamma correction, UI brightness, encode).
//
// HDR: the luminance curve T is kept up to a knee (T = 0.5) and continues above it with the same exponential shape
// and slope, toward the peak instead of the game's white: v is scaled by T_hdr / T. Then the scene goes from the
// UI's white to its own (game nits over UI nits). Below the knee the picture is the game's own; with
// tonemap = vanilla it is the game's own everywhere, clipped at its white.
#ifndef ACNH_TONEMAP_HLSLI
#define ACNH_TONEMAP_HLSLI

// The translated shader's interface: names, sets and bindings as in its SPIR-V (the runtime maps them by name).
[[vk::binding(0, 0)]] cbuffer support_buffer {
  int4 sb_alpha_test;
  int4 sb_is_bgra[8];
  float4 sb_viewport_inverse;
  float4 sb_viewport_size;
  int4 sb_frag_scale_count;
  float4 sb_render_scale[73];
  int4 sb_tfe_offset;
  int4 sb_tfe_vertex_count;
  float4 sb_hdr[4];  // runtime/src/nvn/nvn_shaders.h SupportBuffer::hdr
};
[[vk::binding(20, 0)]] cbuffer fp_c1 { float4 c1[4096]; };
[[vk::binding(22, 0)]] cbuffer fp_c3 { float4 c3[4096]; };
[[vk::combinedImageSampler]] [[vk::binding(128, 2)]] Texture2D<float4> fp_t_tcb_A;  // scene
[[vk::combinedImageSampler]] [[vk::binding(128, 2)]] SamplerState fp_t_tcb_A_sampler;
[[vk::combinedImageSampler]] [[vk::binding(129, 2)]] Texture2D<float4> fp_t_tcb_8;  // bloom (alpha: the output's)
[[vk::combinedImageSampler]] [[vk::binding(129, 2)]] SamplerState fp_t_tcb_8_sampler;

#define HDR_PEAK_NITS sb_hdr[0].x
#define HDR_GAME_NITS sb_hdr[0].y
#define HDR_UI_NITS   sb_hdr[0].z
#define HDR_TONEMAP     sb_hdr[1].x
#define HDR_EXPOSURE    sb_hdr[1].y
#define HDR_HIGHLIGHTS  sb_hdr[1].z  // 1: neutral (RenoDX's user grading)
#define HDR_SHADOWS     sb_hdr[1].w
#define HDR_CONTRAST    sb_hdr[2].x
#define HDR_SATURATION  sb_hdr[2].y
#define HDR_TEMPERATURE sb_hdr[2].z  // -100..100, 0: the game's white
#define HDR_TINT        sb_hdr[2].w

// RenoDX's ToneMapPass (tonemap = renodrt): RenoDRT on the game's scene, the game's grade (saturation, LUT) carried over
// as the ratio of its graded picture to its neutral one; its own user grading takes the F2 sliders.
#define RENODX_PEAK_WHITE_NITS             HDR_PEAK_NITS
#define RENODX_DIFFUSE_WHITE_NITS          HDR_GAME_NITS
#define RENODX_GRAPHICS_WHITE_NITS         HDR_UI_NITS
#define RENODX_GAMMA_CORRECTION            sb_hdr[0].w
#define RENODX_TONE_MAP_TYPE               3.f  // renodx::draw::TONE_MAP_TYPE_RENO_DRT
#define RENODX_TONE_MAP_HIGHLIGHTS         HDR_HIGHLIGHTS
#define RENODX_TONE_MAP_SHADOWS            HDR_SHADOWS
#define RENODX_TONE_MAP_CONTRAST           HDR_CONTRAST
#define RENODX_TONE_MAP_SATURATION         HDR_SATURATION
#include "draw.hlsl"

// NaN to 0, then within RGBA16F's range. The game's targets were UNORM, which store NaN and Inf as numbers; its
// tonemap makes NaN of an infinite scene (c * T / Y), and the transitions' frames have them: a float target keeps
// them and the HDR output shows them at the peak, a flash.
float3 Finite(float3 x) {
  x = select(isnan(x), 0.f, x);
  return clamp(x, -65504.f, 65504.f);
}

// White balance: the white point moved along the daylight locus (temperature) and across it (tint), applied as a von
// Kries adaptation in LMS (the model Unity's white balance uses). 0, 0: none.
float3 WhiteBalance(float3 bt709, float temperature, float tint) {
  if (temperature == 0 && tint == 0) return bt709;
  float t1 = temperature / 60.f, t2 = tint / 60.f;
  float x = 0.31271f - t1 * (t1 < 0 ? 0.1f : 0.05f);
  float y = 2.87f * x - 3.f * x * x - 0.27509507f + t2 * 0.05f;
  float3 xyz = float3(x / y, 1.f, (1.f - x - y) / y);
  float3 w2 = float3(dot(xyz, float3(0.7328f, 0.4296f, -0.1624f)), dot(xyz, float3(-0.7036f, 1.6975f, 0.0061f)),
                     dot(xyz, float3(0.0030f, 0.0136f, 0.9834f)));
  float3 balance = float3(0.949237f, 1.03542f, 1.08728f) / w2;  // D65 in LMS over the new white
  const float3x3 to_lms = float3x3(0.390405f, 0.549941f, 0.00892632f, 0.0708416f, 0.963172f, 0.00135775f,
                                   0.0231082f, 0.128021f, 0.936245f);
  const float3x3 from_lms = float3x3(2.85847f, -1.62879f, -0.024891f, -0.210182f, 1.1582f, 0.000324281f,
                                     -0.041812f, -0.118169f, 1.06867f);
  return mul(from_lms, mul(to_lms, bt709) * balance);
}

// How much the whole screen is a fade to white (0: a picture, 1: white everywhere). The game fades to white (pressing
// Start, some transitions) by driving the scene far up, which its curve flattens to its white: the HDR curve would
// take the whole screen to the peak. Highlights are small, a fade is everywhere: the darkest of a 4x4 grid over the
// screen (the same 16 texels for every pixel) says which. Over 0.9 (T ~0.6) the extension starts giving way to the
// game's white; from 2.5 (T ~0.92) it is gone.
float WhiteFade() {
  float darkest = 1e9f;
  [unroll] for (int j = 0; j < 4; j++) {
    [unroll] for (int i = 0; i < 4; i++) {
      float2 uv = (float2(i, j) + 0.5f) / 4.f;
      float3 c = Finite(fp_t_tcb_A.SampleLevel(fp_t_tcb_A_sampler, uv, 0).rgb) +
                 Finite(fp_t_tcb_8.SampleLevel(fp_t_tcb_8_sampler, uv, 0).rgb);
      darkest = min(darkest, dot(c, float3(0.298911988735198974609375f, c1[0].x, c1[0].y)));
    }
  }
  return smoothstep(0.9f, 2.5f, darkest * exp(c3[0].z) * exp2(HDR_EXPOSURE));
}

struct Tonemapped {
  float3 untonemapped;  // the scene with bloom and exposure, linear (RenoDRT's input)
  float3 neutral;       // the game's curve alone, before its saturation and LUT (RenoDRT's reference)
  float3 v;             // the game's picture before its LUT
  float scale;          // T_hdr / T: the HDR extension, applied after the LUT
  float fade;           // WhiteFade: the whole screen a fade to white
  float alpha;          // the bloom's, as the game writes it
};

Tonemapped ApplyTonemap(float2 uv) {
  float3 scene = Finite(fp_t_tcb_A.Sample(fp_t_tcb_A_sampler, uv).rgb);
  float4 bloom = fp_t_tcb_8.Sample(fp_t_tcb_8_sampler, uv);
  float3 c = Finite((Finite(bloom.rgb) + scene) * exp(c3[0].z) * exp2(HDR_EXPOSURE));
  float y = dot(c, float3(0.298911988735198974609375f, c1[0].x, c1[0].y));
  float t = 1.f - exp(-y);
  // The game divides by Y: NaN on black, which its UNORM target stored as 0 and a float target would keep.
  float inv_y = y > 0 ? 1.f / y : 0.f;
  float3 v = lerp(c * t * inv_y, 1.f - exp(-c), t * t);
  Tonemapped o;
  o.untonemapped = c;
  o.neutral = v;
  float mx = max(v.r, max(v.g, v.b)), mn = min(v.r, min(v.g, v.b));
  float s = c3[1].z;
  v = (v - mn) * s + (mx - (mx - mn) * s);

  o.v = v;
  o.scale = 1.f;
  o.alpha = bloom.a;
  o.fade = HDR_TONEMAP != 0 ? WhiteFade() : 0.f;
  // The knee: T = 0.5 at Y = ln 2, where the curve's slope is e^-Y = 0.5. Above it T rises toward `top` (the peak in
  // the game's white) with that slope; top = 1 is the game's curve.
  const float knee = 0.5f, knee_y = 0.69314718f;
  float top = max(HDR_PEAK_NITS / max(HDR_GAME_NITS, 1.f), 1.f);
  if (HDR_TONEMAP == 1 && y > knee_y && t > 0 && top > 1.f) {
    float room = top - knee;
    float t_hdr = knee + room * (1.f - exp(-(y - knee_y) * knee / room));
    o.scale = lerp(t_hdr / t, 1.f, o.fade);
  }
  return o;
}

// The picture for the target, linear, with the scene at its own brightness relative to the UI's white:
//   0 vanilla: the game's, clipped at its white as its UNORM target did;
//   1 HDR: the game's with its curve extended above the knee;
//   2 RenoDRT: RenoDX's ToneMapPass on the scene, with the game's grade (its graded over its neutral picture).
// Then the player's grading (F2; ToneMapPass does its own in RenoDRT) and the white balance. Neutral by default.
float4 FinishTonemap(float3 graded, Tonemapped t) {
  float3 v;
  if (HDR_TONEMAP == 2) {
    float3 sdr = saturate(graded);
    v = renodx::draw::ToneMapPass(t.untonemapped, sdr, saturate(t.neutral));
    v = lerp(v, sdr, t.fade);  // a fade to white stays the game's white
  } else {
    v = max(graded, 0) * (HDR_TONEMAP == 1 ? t.scale : 1.f);
    v = renodx::color::grade::UserColorGrading(v, 1.f, HDR_HIGHLIGHTS, HDR_SHADOWS, HDR_CONTRAST, HDR_SATURATION, 0.f, 0.f);
  }
  v = max(WhiteBalance(v, HDR_TEMPERATURE, HDR_TINT), 0);
  v = Finite(v);
  if (HDR_TONEMAP == 0) v = saturate(v);
  // To the UI's white, as RenoDX's RenderIntermediatePass: present_hdr gamma-corrects the whole window (sRGB to 2.2)
  // and then scales it by the UI nits; correcting here, scaling, and undoing the correction makes the scene come out
  // as corrected(scene) * game nits whatever the UI brightness (a plain scale went through the correction: the UI
  // slider moved the scene's contrast).
  const float scale = HDR_GAME_NITS / max(HDR_UI_NITS, 1.f);
  if (sb_hdr[0].w != 0)
    v = renodx::color::correct::GammaSafe(renodx::color::correct::GammaSafe(v, false, 2.2f) * scale, true, 2.2f);
  else
    v *= scale;
  return float4(v, t.alpha);
}

float2 TonemapUV(float4 position) {
  // gl_FragCoord in the game's pixels (Ryujinx render scale [0]) over the viewport size in c3[0].xy.
  float2 p = position.xy / sb_render_scale[0].x;
  return saturate(p / c3[0].xy);
}

#endif
