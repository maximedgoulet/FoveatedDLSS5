// Half-rate NR (dlssnr.hpp, half_rate.hpp): one program, three modes.
//
// Mode 0, reproject (one thread per output pixel), on a frame that skips the
// NR evaluation: the previous frame's NR edit e = log2(n' / p), read from its
// P (t1) and N' (t2, the look set: the previous frame ran the edge-aware
// transport, so N'.a carries G_low), is moved to this frame with this
// frame's motion vectors (t4; previous = current + MV) and written as the
// output-resolution pair look_upsample would have written, P_up (u5) and
// N'_up (u6).  The resolve then reads the transport set 1:1 exactly as on an
// NR frame.  The grid is the previous frame's: the host only skips while the
// foveated grid is unchanged.  A pixel whose previous position is off screen
// or whose motion exceeds HardLimit gets no edit (identity), never a smear.
//   HDR (linear reference): P_up = bilinear P at the previous position,
//     N'_up = max(P_up, floor) * 2^e.
//   SDR (sRGB reference): N'_up = this frame's original * 2^e.
//
// Mode 1, measure (one thread per sample of a Size grid): motion length in
// OUTPUT pixels inside the midpoint zone, accumulated into HistoryOut (a
// small raw buffer, the host's motion statistics): word 0 = sum of |mv| in
// 1/16 px, word 1 = samples over FastLimit, word 2 = samples, word 3 = max
// |mv| in 1/16 px.  Read back a few frames later (half_rate.hpp).
//
// Mode 2, clear (one thread): zeroes the four words.
//
// Constants reused from LookConstants for this program:
//   MotionScale  MV texel -> OUTPUT pixels
//   Strength     FastLimit, output pixels (mode 1)
//   Brighten     HardLimit, output pixels (mode 0)

#include "look_common.hlsli"

static const float kSpatialSigma = 0.5;  // NR pixels, as look_upsample
static const float kRangeSigma = 0.5;    // stops

// Non-finite test by bit pattern: FXC's /O3 may fold `x != x` away.
bool NotFinite(float x) {
  return (asuint(x) & 0x7F800000u) == 0x7F800000u;
}

float3 EditAt(int2 pixel, out float guide) {
  const float4 neural = Neural.Load(int3(pixel, 0));
  guide = neural.a;
  return log2(max(SrgbDecode(neural.rgb), kRatioFloor))
      - log2(max(SrgbDecode(Proxy.Load(int3(pixel, 0)).rgb), kRatioFloor));
}

// This frame's motion at an output pixel, in output pixels.
float2 MotionAtOutput(float2 output_pixel_centre) {
  const uint2 grid = max(MotionGrid, uint2(1, 1));
  const uint2 texel = MotionBase
      + min(uint2(max(output_pixel_centre * float2(grid) / float2(FullSize), 0.0)),
            grid - 1u);
  return Motion.Load(int3(texel, 0)) * MotionScale;
}

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  if (Mode == 2u) {
    if (all(dispatch_id == uint3(0, 0, 0))) {
      HistoryOut.Store4(0, uint4(0, 0, 0, 0));
    }
    return;
  }
  if (any(dispatch_id.xy >= Size)) return;

  if (Mode == 1u) {
    const float2 centre = (float2(dispatch_id.xy) + 0.5) * float2(FullSize) / float2(Size);
    // Motion where NR is off cannot smear an edit: only the midpoint zone.
    [branch] if (FovAny(Foveation)
                 && FovZoneWeight(centre / float2(FullSize), Foveation, FoveationBounds,
                                  FoveationBlend) <= 0.0) {
      return;
    }
    const float2 mv = MotionAtOutput(centre);
    const float len = length(mv);
    // NaN/Inf vectors count as fast: a broken field must never arm the skip.
    const bool broken = NotFinite(len) || len > 1e6;
    const uint q = broken ? 65535u * 16u : uint(min(len, 65535.0) * 16.0);
    uint ignored;
    HistoryOut.InterlockedAdd(0, q, ignored);
    if (broken || len > Strength) HistoryOut.InterlockedAdd(4, 1u, ignored);
    HistoryOut.InterlockedAdd(8, 1u, ignored);
    HistoryOut.InterlockedMax(12, q, ignored);
    return;
  }

  // Mode 0: reproject.
  const int2 pixel = int2(dispatch_id.xy);
  const float2 full = float2(FullSize);
  const float2 centre = float2(pixel) + 0.5;
  const float2 mv = MotionAtOutput(centre);
  const float len = length(mv);
  const float2 n_prev = (centre + mv) / full;
  const bool usable = !NotFinite(len) && len <= Brighten && all(n_prev >= 0.0)
      && all(n_prev <= 1.0);

  const int2 last = int2(NrSize) - 1;
  const float2 n = usable ? n_prev : centre / full;
  float2 position = n * float2(NrSize) - 0.5;
  [branch] if (FovAny(Foveation)) {
    position = FovWarp(n, Foveation, FoveationCenter, FoveationBounds) * float2(NrSize) - 0.5;
  }
  const int2 base = int2(floor(position));
  const bool linear_reference = (Flags & kFlagRefLinear) != 0u;
  const float3 reference = Reference.Load(int3(pixel, 0)).rgb;
  const float3 original = linear_reference ? reference : SrgbDecode(reference);

  // The resolve's own bilinear reconstruction of P at the source position.
  const float2 clamped = clamp(position, 0.0, float2(last));
  const int2 b0 = int2(floor(clamped));
  const int2 b1 = min(b0 + 1, last);
  const float2 f = clamped - float2(b0);
  const float3 proxy_up =
      SrgbDecode(Proxy.Load(int3(b0, 0)).rgb) * (1.0 - f.x) * (1.0 - f.y)
      + SrgbDecode(Proxy.Load(int3(int2(b1.x, b0.y), 0)).rgb) * f.x * (1.0 - f.y)
      + SrgbDecode(Proxy.Load(int3(int2(b0.x, b1.y), 0)).rgb) * (1.0 - f.x) * f.y
      + SrgbDecode(Proxy.Load(int3(b1, 0)).rgb) * f.x * f.y;

  float3 edit_up = 0.0;
  [branch] if (usable) {
    // look_upsample's joint-bilateral window, guided by this frame's
    // luminance against the previous frame's footprint means.
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
    edit_up = joint_weight > 1e-6 ? joint / joint_weight : spatial / spatial_weight;
  }
  const float3 gain = exp2(edit_up);
  const float3 neural_up = linear_reference
      ? max(proxy_up, kRatioFloor) * gain
      : (original >= kRatioFloor ? original * gain
                                 : original + kRatioFloor * (gain - 1.0));
  UpProxy[pixel] = float4(SrgbEncode(proxy_up), 1.0);
  UpNeural[pixel] = float4(SrgbEncode(neural_up), 1.0);
}
