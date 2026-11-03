// ACNH's tonemap with the room's colour LUT (3D, sampled with the tonemapped picture), HDR: acnh_tonemap.hlsli.
// The LUT takes the game's 0..1 picture as it always did; the HDR extension scales its result.
#include "acnh_tonemap.hlsli"

[[vk::combinedImageSampler]] [[vk::binding(130, 2)]] Texture3D<float4> fp_t_tcb_C;
[[vk::combinedImageSampler]] [[vk::binding(130, 2)]] SamplerState fp_t_tcb_C_sampler;

float4 main(float4 position : SV_Position) : SV_Target0 {
  Tonemapped t = ApplyTonemap(TonemapUV(position));
  float3 graded = fp_t_tcb_C.Sample(fp_t_tcb_C_sampler, t.v * c3[1].x + c3[1].y).rgb;
  return FinishTonemap(graded, t);
}
