#include "look_common.hlsli"

// Group F, edge-aware transport (fixes F-L1): at reduced NR resolution the
// look stage hands the resolve output-resolution P_up and N'_up, so every
// resolve takes its existing 1:1 Load path with no resolve change.
//
// Bound with the pass's LOOK set: Reference = the pass reference at full
// resolution, Proxy = P, Neural = N' (alpha = G_low from look_compose).
// Per output pixel the effective edit e = log2(n' / p) of the 4x4 NR pixels
// around it is averaged with joint-bilateral weights (Kopf et al. 2007):
// spatial on the distance in NR pixels, range on the difference between this
// pixel's log2 luminance G_full and the NR pixel's footprint mean G_low.  The
// weights are normalised, so a constant edit transports exactly.  Where no
// neighbour matches (sum of weights ~ 0) the spatial weights alone decide.
// The window is symmetric about the sample (floor - 1 .. floor + 2) and the
// spatial sigma is half an NR pixel.  Until rc6 it was the 3x3 around the
// nearest NR pixel with sigma 1: wider than the footprint and lopsided, it
// staircased a smooth edit ramp (+/-0.011 stop at 0.1 stop per NR pixel,
// now +/-0.002) and measured 0.190 stop RMSE against a 4K edit, now 0.172
// (Classic 0.248; Alan Wake 2 capture, 2x, tools/look/look_reference.py).
//   HDR (linear reference): P_up = the resolve's own bilinear P, and
//     N'_up = max(P_up, floor) * 2^e_up, so the resolve's ratio is e_up.
//   SDR (sRGB reference): N'_up = the full-resolution original times 2^e_up,
//     which keeps the original's detail where the Classic path showed
//     nearest-neighbour NR output.

static const float kSpatialSigma = 0.5;  // NR pixels
static const float kRangeSigma = 0.5;    // stops

float3 EditAt(int2 pixel, out float guide) {
  const float4 neural = Neural.Load(int3(pixel, 0));
  guide = neural.a;
  return log2(max(SrgbDecode(neural.rgb), kRatioFloor))
      - log2(max(SrgbDecode(Proxy.Load(int3(pixel, 0)).rgb), kRatioFloor));
}

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const int2 pixel = int2(dispatch_id.xy);
  if (any(dispatch_id.xy >= Size)) return;
  const int2 last = int2(NrSize) - 1;
  float2 position = (float2(pixel) + 0.5) * float2(NrSize) / float2(Size) - 0.5;
  [branch] if (FovAny(Foveation)) {
    position = FovFullToWorkPos(uint2(pixel), Size, NrSize, Foveation, FoveationGmax);
  }
  const int2 base = int2(floor(position));
  const bool linear_reference = (Flags & kFlagRefLinear) != 0u;
  const float3 reference = Reference.Load(int3(pixel, 0)).rgb;
  const float3 original = linear_reference ? reference : SrgbDecode(reference);
  const float guide_full = log2(max(Luminance(max(original, 0.0)), kGuideFloor));

  float3 joint = 0.0;
  float joint_weight = 0.0;
  float3 spatial = 0.0;
  float spatial_weight = 0.0;
  for (int dy = -1; dy <= 2; ++dy) {
    for (int dx = -1; dx <= 2; ++dx) {
      const int2 tap = base + int2(dx, dy);
      const float2 distance = float2(tap) - position;
      const float ws = exp(-dot(distance, distance)
                           / (2.0 * kSpatialSigma * kSpatialSigma));
      float guide_low;
      const float3 edit = EditAt(clamp(tap, 0, last), guide_low);
      const float difference = guide_full - guide_low;
      const float wr = exp(-(difference * difference)
                           / (2.0 * kRangeSigma * kRangeSigma));
      joint += ws * wr * edit;
      joint_weight += ws * wr;
      spatial += ws * edit;
      spatial_weight += ws;
    }
  }
  const float3 edit_up = joint_weight > 1e-6 ? joint / joint_weight
                                             : spatial / spatial_weight;

  // The resolve's own bilinear reconstruction of P at this pixel.
  const float2 clamped = clamp(position, 0.0, float2(last));
  const int2 b0 = int2(floor(clamped));
  const int2 b1 = min(b0 + 1, last);
  const float2 f = clamped - float2(b0);
  const float3 proxy_up =
      SrgbDecode(Proxy.Load(int3(b0, 0)).rgb) * (1.0 - f.x) * (1.0 - f.y)
      + SrgbDecode(Proxy.Load(int3(int2(b1.x, b0.y), 0)).rgb) * f.x * (1.0 - f.y)
      + SrgbDecode(Proxy.Load(int3(int2(b0.x, b1.y), 0)).rgb) * (1.0 - f.x) * f.y
      + SrgbDecode(Proxy.Load(int3(b1, 0)).rgb) * f.x * f.y;
  const float3 gain = exp2(edit_up);
  const float3 neural_up = linear_reference
      ? max(proxy_up, kRatioFloor) * gain
      : (original >= kRatioFloor ? original * gain
                                 : original + kRatioFloor * (gain - 1.0));
  UpProxy[pixel] = float4(SrgbEncode(proxy_up), 1.0);
  UpNeural[pixel] = float4(SrgbEncode(neural_up), 1.0);
}
