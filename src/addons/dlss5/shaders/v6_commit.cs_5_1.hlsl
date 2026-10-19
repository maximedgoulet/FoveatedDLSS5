#include "v6_common.hlsli"

RWTexture2D<float4> BlockMean : register(u1);

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const int2 block_max = int2((Size + 31u) / 32u) - int2(1, 1);
  const float2 block_pos = (float2(pixel) + 0.5) / 32.0 - 0.5;
  const int2 b0 = clamp(int2(floor(block_pos)), int2(0, 0), block_max);
  const int2 b1 = min(b0 + int2(1, 1), block_max);
  const float2 f = saturate(block_pos - float2(b0));
  const float4 m00 = BlockMean.Load((int2)b0);
  const float4 m10 = BlockMean.Load((int2)int2(b1.x, b0.y));
  const float4 m01 = BlockMean.Load((int2)int2(b0.x, b1.y));
  const float4 m11 = BlockMean.Load((int2)b1);
  const float final_mean =
      m00.x * (1.0 - f.x) * (1.0 - f.y) + m10.x * f.x * (1.0 - f.y)
      + m01.x * (1.0 - f.x) * f.y + m11.x * f.x * f.y;
  const float untouched_mean =
      m00.y * (1.0 - f.x) * (1.0 - f.y) + m10.y * f.x * (1.0 - f.y)
      + m01.y * (1.0 - f.x) * f.y + m11.y * f.x * f.y;
  // Bilinearly interpolated untouched block max: gating on the interpolated
  // value (not the nearest block's) keeps the dark-gate boundary itself from
  // becoming a step edge.
  const float untouched_max =
      m00.z * (1.0 - f.x) * (1.0 - f.y) + m10.z * f.x * (1.0 - f.y)
      + m01.z * (1.0 - f.x) * f.y + m11.z * f.x * f.y;
  const float4 value = Original.Load(int3(pixel, 0));
  float3 result = value.rgb;
#ifdef GATE_IN_INPUT_UNITS
  // Feed v2: the NR input scale (t2, the norm_scale texel) is the game's
  // exposure and can sit many stops from 1, so "near-black" and the cap are
  // stated for what NR sees and carried back to source units here.
  const float unit = max(Neural.Load(int3(0, 0, 0)).r, 1e-8);
#else
  const float unit = 1.0;
#endif
  // Gate on the block's brightest untouched pixel: a mean-only gate admits
  // mixed dark/bright blocks whose mean-lift estimate is contaminated by the
  // bright feature (the blobbing class; see v6_pedestal_reduce).
  if (untouched_max <= DarkGate * unit) {
    const float pedestal =
        clamp(final_mean - untouched_mean, 0.0, PedestalCap * unit)
        * TransferStrength * PedestalGate;
    if (pedestal > 0.0) result = max(result - pedestal, 0.0);
  }
  if (Encoding == 2u) {
    Output[pixel] = float4(LinearToPQ(max(result, 0.0)), value.a);
  } else {
    Output[pixel] = float4(result, value.a);
  }
}
