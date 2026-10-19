#include "look_common.hlsli"

// The look compose pass, one thread per NR pixel: the network's edit, its
// bands (after look_band), its temporal filter (history per NR handle),
// then every gain, written as N' = n * 2^(t * (e' - e)) into LookOutput.
// With the transport flag the alpha channel carries G_low, the log2
// luminance of the reference over this pixel's footprint, for look_upsample.

// (a_B, a_D, c.r, c.b) of the current frame at `pixel`.
float4 BandValues(int2 pixel, out Edit x, out float peak) {
  x = ReadEdit(pixel);
  peak = x.I;
  float aB = x.a;
  float aD = 0.0;
  if ((Flags & (kFlagBands | kFlagTemporal)) != 0u) {
    const float2 coefficients = BandCoefficients(pixel, peak);
    aB = coefficients.x * x.I + coefficients.y;
    aD = x.a - aB;
  }
  return float4(aB, aD, x.c.r, x.c.b);
}

float4 LoadHistory(uint2 pixel, out float history_i) {
  const uint3 words = HistoryIn.Load3(HistoryAddress(pixel));
  history_i = f16tof32(words.z);
  return float4(f16tof32(words.x), f16tof32(words.x >> 16),
                f16tof32(words.y), f16tof32(words.y >> 16));
}

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const int2 pixel = int2(dispatch_id.xy);
  if (any(dispatch_id.xy >= NrSize)) return;
  Edit x;
  float peak;
  float4 current = BandValues(pixel, x, peak);
  float3 c = x.c;

  if ((Flags & kFlagTemporal) != 0u) {
    float4 filtered = current;
    if ((Flags & kFlagHistoryValid) != 0u) {
      // DLSS motion vectors point to the PREVIOUS position: previous =
      // current + MV (DLSS Programming Guide, section 3.6).
      float2 previous = float2(pixel);
      if ((Flags & kFlagMotion) != 0u) {
        [branch] if (FovAny(Foveation)) {
          // Foveated grid: look the vector up at this texel's full-frame
          // position and carry the displacement through the warp.
          // MotionScale is MV texel -> NR pixels, so MV * MotionScale / NrSize
          // is the normalized full-frame displacement.
          const float2 n = FovUnwarp(
              (float2(pixel) + 0.5) / float2(NrSize), Foveation, FoveationGmax);
          const uint2 texel = MotionBase
              + min(uint2(saturate(n) * float2(MotionGrid)), MotionGrid - 1u);
          const float2 d =
              Motion.Load(int3(texel, 0)).xy * MotionScale / float2(NrSize);
          previous = FovWarp(n + d, Foveation, FoveationGmax) * float2(NrSize) - 0.5;
        } else {
          const uint2 texel = MotionBase
              + min(uint2((float2(pixel) + 0.5) * float2(MotionGrid) / float2(NrSize)),
                    MotionGrid - 1u);
          previous += Motion.Load(int3(texel, 0)).xy * MotionScale;
        }
      }
      const float2 limit = float2(NrSize) - 0.5;
      const bool onscreen = all(previous >= -0.5) && all(previous <= limit);
      const float2 pos = clamp(previous, 0.0, float2(NrSize - 1u));
      const uint2 h0 = uint2(floor(pos));
      const uint2 h1 = min(h0 + 1u, NrSize - 1u);
      const float2 f = pos - float2(h0);
      float i00, i10, i01, i11;
      const float4 q00 = LoadHistory(h0, i00);
      const float4 q10 = LoadHistory(uint2(h1.x, h0.y), i10);
      const float4 q01 = LoadHistory(uint2(h0.x, h1.y), i01);
      const float4 q11 = LoadHistory(h1, i11);
      const float4 history = q00 * (1.0 - f.x) * (1.0 - f.y) + q10 * f.x * (1.0 - f.y)
          + q01 * (1.0 - f.x) * f.y + q11 * f.x * f.y;
      const float history_i = i00 * (1.0 - f.x) * (1.0 - f.y) + i10 * f.x * (1.0 - f.y)
          + i01 * (1.0 - f.x) * f.y + i11 * f.x * f.y;
      const float confidence = onscreen
          ? 1.0 - smoothstep(0.1, 0.3, abs(x.I - history_i))
          : 0.0;
      // Neighbourhood clipping: history may not leave the 3x3 range of the
      // current frame's values.
      float4 lo = current;
      float4 hi = current;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) continue;
          const int2 neighbour = clamp(pixel + int2(dx, dy), 0, int2(NrSize) - 1);
          Edit unused;
          float unused_peak;
          const float4 value = BandValues(neighbour, unused, unused_peak);
          lo = min(lo, value);
          hi = max(hi, value);
        }
      }
      const float beta = max(Alpha, 1.0 - confidence);
      const float4 clipped = clamp(history, lo, hi);
      filtered = clipped + beta * (current - clipped);
      if ((Flags & kFlagTemporalDetail) == 0u) filtered.y = current.y;
    }
    const uint address = HistoryAddress(uint2(pixel));
    HistoryOut.Store3(address, uint3(
        f32tof16(filtered.x) | (f32tof16(filtered.y) << 16),
        f32tof16(filtered.z) | (f32tof16(filtered.w) << 16),
        f32tof16(x.I)));
    current = filtered;
    c = float3(current.z, -(kW.x * current.z + kW.z * current.w) / kW.y, current.w);
  }

  const float3 shaped = ShapeEdit(x, current.x, current.y, c, peak);
  float guide = 1.0;
  if ((Flags & kFlagTransport) != 0u) {
    // v6_encode's area footprint over the reference.
    const float2 scale = float2(FullSize) / float2(NrSize);
    float2 footprint_min = float2(pixel) * scale;
    float2 footprint_max = min(footprint_min + scale, float2(FullSize));
    [branch] if (FovAny(Foveation)) {
      FovWorkFootprint(uint2(pixel), NrSize, FullSize, Foveation, FoveationGmax,
                       footprint_min, footprint_max);
    }
    const int2 first = int2(floor(footprint_min));
    const int2 last = int2(ceil(footprint_max));
    float3 sum = 0.0;
    float weight = 0.0;
    for (int y = first.y; y < last.y; ++y) {
      for (int xx = first.x; xx < last.x; ++xx) {
        const float2 overlap = min(float2(xx + 1, y + 1), footprint_max)
            - max(float2(xx, y), footprint_min);
        const float w = max(overlap.x, 0.0) * max(overlap.y, 0.0);
        const float3 texel = Reference.Load(int3(xx, y, 0)).rgb;
        sum += max((Flags & kFlagRefLinear) != 0u ? texel : SrgbDecode(texel), 0.0) * w;
        weight += w;
      }
    }
    guide = log2(max(Luminance(sum / max(weight, 1e-12)), kGuideFloor));
  }
  LookOutput[pixel] = float4(SrgbEncode(x.n * exp2(x.t * (shaped - x.e))), guide);
}
