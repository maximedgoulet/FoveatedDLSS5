#include "v6_common.hlsli"

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const uint2 source_size = max(SourceSize, uint2(1, 1));
  const uint2 source_pixel = SourceBase + min(pixel, source_size - 1u);
  const float4 source = Original.Load(int3(source_pixel, 0));
  float3 linear_rgb = source.rgb;
  if (Encoding == 2u) linear_rgb = PQToLinear(linear_rgb);
  Output[pixel] = float4(linear_rgb, source.a);
}
