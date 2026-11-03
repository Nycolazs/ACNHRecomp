// ACNH's tonemap without a colour LUT (most of the island), HDR: acnh_tonemap.hlsli.
#include "acnh_tonemap.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0 {
  Tonemapped t = ApplyTonemap(TonemapUV(position));
  return FinishTonemap(t.v, t);
}
