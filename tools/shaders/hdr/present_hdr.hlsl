// The present pass with HDR output (video.hdr): present.frag's rotation, fit and crop (the touch controls are drawn
// over it by overlay.frag), then RenoDX's SwapChainPass. The window texture holds the scene and the UI in UI-relative gamma
// (the UI at 0..1, untouched; the scene's highlights above 1 once its tonemap is replaced): it is decoded, scaled to
// the UI brightness, clamped to the peak and encoded HDR10 (PQ, BT.2020) or scRGB.
// DXC: dxc -spirv -T ps_6_0 -HV 2021 -I <renodx>/src/shaders (tools/gen_spirv_inc.py).
struct Push {
    float4 rot;   // as present.frag
    float4 off;
    float4 box;
    float4 crop;
    float4 hdr;   // x UI nits, y peak nits, z gamma (0 sRGB, 1 2.2: renodx GAMMA_CORRECTION_*), w output preset
};
[[vk::push_constant]] ConstantBuffer<Push> p;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D<float4> src_texture;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState src_sampler;

// The window texture holds linear light (the game writes it through an sRGB view, which HDR's RGBA16F texture drops):
// no decoding, then the gamma the SDR picture was seen with (2.2: sRGB-encoded, shown by a 2.2 display; sRGB: none).
#define RENODX_PEAK_NITS                    p.hdr.y
#define RENODX_UI_NITS                      p.hdr.x
#define RENODX_GAMMA_CORRECTION             p.hdr.z
#define RENODX_SWAP_CHAIN_DECODING          0.f  // renodx::draw::ENCODING_NONE
#define RENODX_SWAP_CHAIN_GAMMA_CORRECTION  p.hdr.z
#define RENODX_SWAP_CHAIN_OUTPUT_PRESET     p.hdr.w
#include "renodx.hlsl"

float4 main(float4 position : SV_Position, [[vk::location(0)]] float2 uv : TEXCOORD0) : SV_Target0 {
    float2 up = mul(float2x2(p.rot.x, p.rot.z, p.rot.y, p.rot.w), uv) + p.off.xy;
    float2 b = (up - p.box.xy) / p.box.zw;
    float3 c = 0;
    if (all(b >= 0) && all(b <= 1)) c = src_texture.Sample(src_sampler, p.crop.xy + b * p.crop.zw).rgb;
    // NaN and Inf from anything drawn into the float window (its RGBA8 texture stored them as numbers): 0, and the
    // peak at most (SwapChainPass clamps the rest).
    c = clamp(select(isnan(c), 0.f, c), -65504.f, 65504.f);
    return float4(renodx::draw::SwapChainPass(c, position.xy), 1.f);
}
