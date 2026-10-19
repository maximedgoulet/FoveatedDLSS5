#include "v6_common.hlsli"

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const float4 original = Original.Load(int3(pixel, 0));
  Output[pixel] = float4(ResolvePixel(pixel, original.rgb), original.a);
}
