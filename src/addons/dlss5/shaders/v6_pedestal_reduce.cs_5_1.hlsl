#include "v6_common.hlsli"

RWTexture2D<float4> BlockMean : register(u0);

groupshared float BlockSumFinal[256];
groupshared float BlockSumUntouched[256];
groupshared float BlockMaxUntouched[256];
groupshared uint  BlockValidCount[256];

[numthreads(16, 16, 1)]
void main(uint3 group_id : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  const uint thread_index = group_thread.y * 16u + group_thread.x;
  const uint2 base = group_id.xy * 32u + group_thread.xy * 2u;
  float sum_final = 0.0;
  float sum_untouched = 0.0;
  float max_untouched = 0.0;
  uint valid = 0u;
  for (uint y = 0u; y < 2u; ++y) {
    for (uint x = 0u; x < 2u; ++x) {
      const uint2 pixel = base + uint2(x, y);
      if (all(pixel < Size)) {
#ifdef MEASURE_UNSHAPED
        // Pass 0's codec set [work0, P, N, norm_scale]: the resolve of the
        // UNSHAPED model output - NR's own lift - against the untouched work0.
        const float3 untouched_rgb = Original.Load(int3(pixel, 0)).rgb;
        sum_final += Luminance(ResolvePixel(pixel, untouched_rgb));
        const float untouched = Luminance(max(untouched_rgb, 0.0));
#else
        sum_final += Luminance(Original.Load(int3(pixel, 0)).rgb);
        const float untouched =
            Luminance(max(OutputOriginal.Load(int3(pixel, 0)).rgb, 0.0));
#endif
        sum_untouched += untouched;
        max_untouched = max(max_untouched, untouched);
        ++valid;
      }
    }
  }
  BlockSumFinal[thread_index] = sum_final;
  BlockSumUntouched[thread_index] = sum_untouched;
  BlockMaxUntouched[thread_index] = max_untouched;
  BlockValidCount[thread_index] = valid;
  GroupMemoryBarrierWithGroupSync();
  if (thread_index == 0u) {
    float total_final = 0.0;
    float total_untouched = 0.0;
    float total_max_untouched = 0.0;
    uint total_valid = 0u;
    for (uint i = 0u; i < 256u; ++i) {
      total_final += BlockSumFinal[i];
      total_untouched += BlockSumUntouched[i];
      total_max_untouched = max(total_max_untouched, BlockMaxUntouched[i]);
      total_valid += BlockValidCount[i];
    }
    const float count = max(float(total_valid), 1.0);
    // z carries the untouched block MAX: the commit's dark gate needs to know
    // whether the block is genuinely near-black, not merely dark ON AVERAGE -
    // a bright feature crossing a dark block lifts the mean lift estimate and
    // turns the per-block subtraction into visible blotches (KCD2 barn-scene
    // capture: 98% of pedestal mass in 32% mean-gated mixed blocks).
    BlockMean[group_id.xy] =
        float4(total_final / count, total_untouched / count, total_max_untouched, 0.0);
  }
}
