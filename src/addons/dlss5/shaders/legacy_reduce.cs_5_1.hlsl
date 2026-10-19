#include "legacy_common.hlsli"

RWTexture2D<float4> BlockMean : register(u1);

groupshared float BlockSumDecoded[256];
groupshared float BlockSumOriginal[256];
groupshared uint  BlockValidCount[256];

[numthreads(16, 16, 1)]
void main(uint3 group_id : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  const uint thread_index = group_thread.y * 16u + group_thread.x;
  const uint2 base = group_id.xy * 32u + group_thread.xy * 2u;
  float sum_decoded = 0.0;
  float sum_original = 0.0;
  uint valid = 0u;
  for (uint y = 0u; y < 2u; ++y) {
    for (uint x = 0u; x < 2u; ++x) {
      const uint2 pixel = base + uint2(x, y);
      if (all(pixel < Size)) {
#ifdef MEASURE_UNSHAPED
        // NR's own lift: this pass's resolve of the UNSHAPED model output
        // (t2 of the pass's codec set), not the look-shaped write surface.
        const float4 decoded =
            float4(ResolvePixel(pixel, OutputOriginal.Load(int3(pixel, 0)).rgb), 0.0);
#else
        const float4 decoded = Output.Load((int2)pixel);
#endif
        const float4 original = OutputOriginal.Load(int3(pixel, 0));
        sum_decoded += Luminance(decoded.rgb);
        sum_original += Luminance(max(original.rgb, 0.0));
        ++valid;
      }
    }
  }
  BlockSumDecoded[thread_index] = sum_decoded;
  BlockSumOriginal[thread_index] = sum_original;
  BlockValidCount[thread_index] = valid;
  GroupMemoryBarrierWithGroupSync();
  if (thread_index == 0u) {
    float total_decoded = 0.0;
    float total_original = 0.0;
    uint total_valid = 0u;
    for (uint i = 0u; i < 256u; ++i) {
      total_decoded += BlockSumDecoded[i];
      total_original += BlockSumOriginal[i];
      total_valid += BlockValidCount[i];
    }
    const float count = max(float(total_valid), 1.0);
    BlockMean[group_id.xy] = float4(total_decoded / count, total_original / count, 0.0, 0.0);
  }
}
