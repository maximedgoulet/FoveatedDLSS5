#include "v6_common.hlsli"

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const uint2 source_size = max(SourceSize, uint2(1, 1));
  const float2 scale = float2(source_size) / float2(Size);
  const float2 center = (float2(pixel) + 0.5) * scale;
  float2 footprint_min = max(center - 0.5 * scale, float2(0.0, 0.0));
  float2 footprint_max = min(center + 0.5 * scale, float2(source_size));
  [branch] if (FovAny(Foveation)) {
    // Foveated grid: toward the borders a working texel covers a larger
    // full-frame span; keep the area filter exact over it.
    FovWorkFootprint(pixel, Size, source_size, Foveation, FoveationGmax,
                     footprint_min, footprint_max);
  }
  const int2 first = int2(floor(footprint_min));
  const int2 last = int2(ceil(footprint_max));
  float3 sum = float3(0.0, 0.0, 0.0);
  float weight = 0.0;
  for (int y = first.y; y < last.y; ++y) {
    for (int x = first.x; x < last.x; ++x) {
      const float2 overlap = min(float2(x + 1, y + 1), footprint_max)
          - max(float2(x, y), footprint_min);
      const float w = max(overlap.x, 0.0) * max(overlap.y, 0.0);
      sum += Original.Load(int3(int2(x, y), 0)).rgb * w;
      weight += w;
    }
  }
  float3 proxy = weight > 0.0 ? sum / weight : float3(0.0, 0.0, 0.0);
  // Divisor > 0 means calibrated absolute units.  Divisor == 0 means the
  // source is engine-relative and the 1x1 same-frame GPU scale is authoritative.
  const float divisor = Divisor > 0.0
      ? Divisor
      : max(OutputOriginal.Load(int3(0, 0, 0)).r, 1e-8);
  proxy = max(proxy, 0.0) / divisor;
  // Hue-preserving shoulder: compress uniformly from the max channel instead
  // of per channel. Per-channel clipping skewed bright colored content
  // toward primaries ("oddly saturated").
  float m = max(proxy.x, max(proxy.y, proxy.z));
  if (Curve == 1u) {
    // Display codec: LOCALIZED shadow toe (codec_gain.hpp DisplayShadowToe,
    // A = 0.014 / B = 0.001) lifts only the FP8-fragile dark range - slope
    // 15 at black, near-identity by midtones - as a uniform max-channel
    // scale; the shared highlight shoulder then bounds the top.  The resolve
    // never inverts this curve; it transfers the neural delta as a bounded
    // gain, so the exact inverse does not exist here by design.
    const float m2 = m + 0.014 * m / (m + 0.001);
    proxy *= m2 / max(m, 1e-6);
    m = m2;
  }
  [branch]
  if (m > 0.75) {
    proxy *= (0.75 + 0.25 * (1.0 - exp(-5.770780 * (m - 0.75)))) / max(m, 1e-6);
  }
  Output[pixel] = float4(SrgbEncode(proxy), 1.0);
}
