#include "look_common.hlsli"

// Group D's band split: the fast guided filter (He & Sun, "Fast Guided
// Filter", 2015) of the achromatic edit `a`, guided by the input's log
// luminance I, with coefficients at half NR resolution.  Five passes over the
// half-resolution grid (Size), chosen by Mode, each separated by a UAV
// barrier on the host:
//   0 down    BandA = 2x2 means of (I, a, I*I, I*a); BandMaxA = 2x2 max of I
//   1 H       BandB = horizontal box of BandA; BandMaxB = horizontal dilation
//   2 V+coef  box/dilation vertically, then A = cov(I, a) / (var(I) + GfEps)
//             and B = mean(a) - A * mean(I) into BandA.xy; BandMaxA = the
//             dilated I, the halo neighbourhood look_compose reads
//   3 H       BandB.xy = horizontal box of (A, B)
//   4 V       BandA.xy = vertical box of BandB.xy
// Box windows are clamped at the border and normalised by the in-bounds
// count, so a constant field stays constant.  look_reference.py
// (bands_half) mirrors the order of every sum.
//
// The dilation is round and falls off with distance: the max over the
// window of I - kHaloPenalty * (d / Radius)^2, d in half-res pixels, over
// 2 * Radius (where the penalty reaches 8 stops).  The two separable passes
// together subtract the penalty of the Euclidean distance.  Until rc6 it was
// a flat max over a square of Radius: the halo weight stayed full to the
// square's edge and then dropped, which drew the square.
static const float kHaloPenalty = 2.0;  // stops at the band radius

float4 BoxMoments(int2 pixel, bool horizontal) {
  const int2 last = int2(Size) - 1;
  const int radius = int(Radius);
  const int centre = horizontal ? pixel.x : pixel.y;
  const int lo = max(centre - radius, 0);
  const int hi = min(centre + radius, horizontal ? last.x : last.y);
  float4 sum = 0.0;
  for (int i = lo; i <= hi; ++i) {
    sum += horizontal ? BandA[int2(i, pixel.y)] : BandB[int2(pixel.x, i)];
  }
  return sum / float(hi - lo + 1);
}

float Dilate(int2 pixel, bool horizontal) {
  const int2 last = int2(Size) - 1;
  const int radius = int(Radius);
  const float k = kHaloPenalty / float(radius * radius);
  const int centre = horizontal ? pixel.x : pixel.y;
  const int lo = max(centre - 2 * radius, 0);
  const int hi = min(centre + 2 * radius, horizontal ? last.x : last.y);
  float peak = -1e30;
  for (int i = lo; i <= hi; ++i) {
    const float d = float(i - centre);
    peak = max(peak, (horizontal ? BandMaxA[int2(i, pixel.y)] : BandMaxB[int2(pixel.x, i)])
                         - k * d * d);
  }
  return peak;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const int2 pixel = int2(dispatch_id.xy);
  if (any(dispatch_id.xy >= Size)) return;
  if (Mode == 0u) {
    float4 moments = 0.0;
    float peak = -1e30;
    float count = 0.0;
    for (int dy = 0; dy <= 1; ++dy) {
      for (int dx = 0; dx <= 1; ++dx) {
        const int2 source = pixel * 2 + int2(dx, dy);
        if (any(source >= int2(NrSize))) continue;
        const Edit x = ReadEdit(source);
        moments += float4(x.I, x.a, x.I * x.I, x.I * x.a);
        peak = max(peak, x.I);
        count += 1.0;
      }
    }
    BandA[pixel] = moments / count;
    BandMaxA[pixel] = peak;
  } else if (Mode == 1u) {
    BandB[pixel] = BoxMoments(pixel, true);
    BandMaxB[pixel] = Dilate(pixel, true);
  } else if (Mode == 2u) {
    const float4 m = BoxMoments(pixel, false);
    const float variance = max(m.z - m.x * m.x, 0.0);
    const float covariance = m.w - m.x * m.y;
    const float slope = covariance / (variance + GfEps);
    BandA[pixel] = float4(slope, m.y - slope * m.x, 0.0, 0.0);
    BandMaxA[pixel] = Dilate(pixel, false);
  } else if (Mode == 3u) {
    BandB[pixel] = BoxMoments(pixel, true);
  } else {
    BandA[pixel] = BoxMoments(pixel, false);
  }
}
