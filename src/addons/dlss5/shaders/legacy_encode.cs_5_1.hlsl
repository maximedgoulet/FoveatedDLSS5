#include "legacy_common.hlsli"

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const uint2 source_size = max(SourceSize, uint2(1, 1));
  uint2 source_pixel = SourceBase + min(
      uint2(((float2(pixel) + 0.5) * float2(source_size)) / float2(Size)),
      source_size - 1);
  [branch] if (FovAny(Foveation)) {
    const float2 uniform_n = (float2(pixel) + 0.5) / float2(Size);
    float2 n = FovUnwarp(uniform_n, Foveation, FoveationGmax);
    // An axis whose grids match stays uniform (as FovFullToWorkPos does).
    if (Size.x == source_size.x) n.x = uniform_n.x;
    if (Size.y == source_size.y) n.y = uniform_n.y;
    source_pixel = SourceBase + min(uint2(saturate(n) * float2(source_size)),
                                    source_size - 1u);
  }
  const float4 source = Original.Load(int3(source_pixel, 0));
  float3 proxy = max(source.rgb, 0.0);
  if (HdrMode == 1) {
    // Scene-linear HDR (scRGB / linear FP16): normalize to paper white, soft-clip,
    // then encode to sRGB so DLSSNR sees a display-referred proxy.
    // Default PaperWhiteScale = 2.5375 = 203/80: scRGB is 1.0 = 80 nits and the
    // model expects paper white (203 nits) at 1.0, so raw scRGB at scale 1.0
    // leaves the proxy overranged (shoulder starting at only 60 nits).
    proxy /= PaperWhiteScale;
    // Hue-preserving shoulder: compress uniformly from the max channel instead
    // of per channel. Per-channel clipping skewed bright colored content
    // toward primaries ("oddly saturated").
    float m = max(proxy.x, max(proxy.y, proxy.z));
    [branch]
    if (m > 0.75) {
      proxy *= (0.75 + 0.25 * (1.0 - exp(-5.770780 * (m - 0.75)))) / max(m, 1e-6);
    }
    proxy = SrgbEncode(proxy);
  } else if (HdrMode == 2) {
    // PQ/display-encoded HDR (R10G10B10A2): linearize PQ, normalize to paper
    // white, soft-clip, then sRGB.  PQToLinear is absolute (1.0 = 10000 nits),
    // so without the normalization every ordinary tone (<=~300 nits) lands in
    // the bottom ~3% of the proxy; with AutoExposure forced, the model then
    // misreads the scene as near-black and lifts/smears shadow noise.
    // DiffuseWhiteNits (default 203) = BT.2408 reference white over the
    // 10000-nits PQ range - the bridge anchor (NRDiffuseWhiteNits);
    // PaperWhiteScale is the user's live calibration multiplier on top.
    proxy = PQToLinear(proxy) / ((DiffuseWhiteNits / 10000.0) * PaperWhiteScale);
    float m = max(proxy.x, max(proxy.y, proxy.z));
    [branch]
    if (m > 0.75) {
      proxy *= (0.75 + 0.25 * (1.0 - exp(-5.770780 * (m - 0.75)))) / max(m, 1e-6);
    }
    proxy = SrgbEncode(proxy);
  }
  // HdrMode == 0: SDR, already display-referred; pass through unchanged.
  // The DLSSNR color input must be opaque: a 0 alpha (common for R10G10B10A2
  // HDR where the game writes alpha 0) makes the runtime emit an empty/black
  // neural output.  The real output alpha is restored from OutputOriginal in
  // the decode stage, so forcing 1.0 here is safe for every HDR mode.
  Output[pixel] = float4(proxy, 1.0);
}
