// Foveated NR working grid (separable warp), dual foveation.
//
// DLSSNR is a same-resolution network, so a spatially varying working
// resolution is expressed as a warp: the frame is resampled into a smaller
// working image whose density, per axis, is 1 (relative to the uniform
// working scale) inside the centre zone, falls along a smoothstep ramp, and
// settles at the midpoint density.  The axes are independent.  The model
// runs once on the warped image with warped guides (fov_guides), and every
// stage that maps between the working grid and the full grid goes through
// FovWarp / FovUnwarp.
//
// Three zones per axis: centre (density 1), midpoint (density e, after the
// fall-off) out to the midpoint zone's bounds [m0, m1], and beyond them
// nothing: the working image covers only [m0, m1], and the resolve keeps the
// game's pixels there (FovZoneWeight, feathered over the blend width).
//
// Coordinates are normalized: n in [0, 1] across the full image, v in [0, 1]
// across the working image.  Both extend monotonically past their ranges so
// off-screen and off-zone reprojection stays well defined.
//
// Per-axis packed dword (foveation.hpp mirrors it; 0 = identity axis):
//   bits  0..11  a: centre extent / screen extent, code / 4095
//   bits 12..23  b: fall-off length / screen half-extent, code / 2048
//   bits 24..30  e: midpoint density, code / 128
//   bit  31      enabled
// Beside it, per axis: the centre c (float, 0..1; the host keeps the centre
// zone inside the bounds), the bounds (uint: m0 | m1 << 16, each code/65535;
// 0 = the whole axis), and the blend width (float, normalized; used only by
// FovZoneWeight).  Distances are measured from c in half-screen units, so the
// sides have lengths 2(c - m0) and 2(m1 - c), and
//   v(n) = (G(2(c - m0)) + sign(n - c) G(2|n - c|)) / (G(2(c - m0)) + G(2(m1 - c))).
// With m0 = 0, m1 = 1 this is the single-zone warp; at c = 0.5 the symmetric
// one.  The normalizer is two closed-form G evaluations.
//
// Cost: identity axes return immediately; the plateau and midpoint segments
// are closed form; only the fall-off band runs the 4-step Newton inverse.

#ifndef DLSS5_FOVEATION_HLSLI
#define DLSS5_FOVEATION_HLSLI

struct FovAxis {
  float a;     // centre extent (plateau width in half-screen units)
  float b;     // ramp length
  float e;     // midpoint density
  float k;     // 1 - e
  float c;     // zone centre, 0..1
  float gl;    // G(2(c - m0)): density integral of the side before c
  float gtot;  // gl + G(2(m1 - c)): the whole working axis
};

bool FovAxisOn(uint packed) {
  return (packed & 0x80000000u) != 0u;
}

bool FovAny(uint2 packed) {
  return ((packed.x | packed.y) & 0x80000000u) != 0u;
}

// Midpoint-zone bounds [m0, m1] of one axis; 0 means the whole axis.
float2 FovBounds(uint bounds) {
  [branch] if (bounds == 0u) return float2(0.0, 1.0);
  return float2(float(bounds & 0xFFFFu), float(bounds >> 16)) * (1.0 / 65535.0);
}

// G(tau): integral of the density over [0, tau], tau = 2|n - c|.  The ramp
// may run past the zone's edge (truncated there); G stays monotone.
float FovG(float tau, FovAxis p) {
  [branch] if (tau <= p.a) return tau;
  const float ramp_end = p.a + p.b;
  const float ramp_area = p.b * (1.0 - 0.5 * p.k);
  [branch] if (tau >= ramp_end) return p.a + ramp_area + p.e * (tau - ramp_end);
  const float q = (tau - p.a) / p.b;
  const float q2 = q * q;
  return p.a + p.b * (q - p.k * (q2 * q - 0.5 * q2 * q2));
}

FovAxis FovDecode(uint packed, float center, uint bounds) {
  FovAxis p;
  p.a = float(packed & 0xFFFu) * (1.0 / 4095.0);
  p.b = float((packed >> 12) & 0xFFFu) * (1.0 / 2048.0);
  p.e = float((packed >> 24) & 0x7Fu) * (1.0 / 128.0);
  p.k = 1.0 - p.e;
  p.c = center;
  const float2 m = FovBounds(bounds);
  p.gl = FovG(2.0 * (center - m.x), p);
  p.gtot = p.gl + FovG(2.0 * (m.y - center), p);
  return p;
}

// Inverse of FovG.  The ramp segment h(q) = q - k(q^3 - q^4/2) has
// h'(q) = 1 - k * smoothstep(q) >= e >= 0.25, so Newton from q = r reaches
// fp32 precision in four steps (measured: <0.003 px at 7680 px, e 0.25).
float FovGInv(float g, FovAxis p) {
  [branch] if (g <= p.a) return g;
  const float ramp_area = p.b * (1.0 - 0.5 * p.k);
  [branch] if (g >= p.a + ramp_area) return p.a + p.b + (g - p.a - ramp_area) / p.e;
  const float r = (g - p.a) / p.b;
  float q = r;
  [unroll]
  for (int i = 0; i < 4; ++i) {
    const float q2 = q * q;
    const float h = q - p.k * (q2 * q - 0.5 * q2 * q2) - r;
    const float dh = 1.0 - p.k * (3.0 * q2 - 2.0 * q2 * q);
    q = saturate(q - h / dh);
  }
  return p.a + p.b * q;
}

// One axis: full normalized -> working normalized, and back.
float FovWarp1(float n, uint packed, float center, uint bounds) {
  [branch] if (!FovAxisOn(packed)) return n;
  const FovAxis p = FovDecode(packed, center, bounds);
  const float t = n - p.c;
  return (p.gl + sign(t) * FovG(2.0 * abs(t), p)) / p.gtot;
}

float FovUnwarp1(float v, uint packed, float center, uint bounds) {
  [branch] if (!FovAxisOn(packed)) return v;
  const FovAxis p = FovDecode(packed, center, bounds);
  const float g = v * p.gtot - p.gl;
  return p.c + 0.5 * sign(g) * FovGInv(abs(g), p);
}

float2 FovWarp(float2 n, uint2 packed, float2 center, uint2 bounds) {
  return float2(FovWarp1(n.x, packed.x, center.x, bounds.x),
                FovWarp1(n.y, packed.y, center.y, bounds.y));
}

float2 FovUnwarp(float2 v, uint2 packed, float2 center, uint2 bounds) {
  return float2(FovUnwarp1(v.x, packed.x, center.x, bounds.x),
                FovUnwarp1(v.y, packed.y, center.y, bounds.y));
}

// Continuous texel position on a `dst` grid (working) of the full-grid pixel
// centre `pixel` (src grid).  Uniform form: (pixel + 0.5) * dst / src - 0.5.
// An axis whose grids match is left alone even if its dword is set (the host
// drops an axis whose working extent reaches the output's).
float2 FovFullToWorkPos(uint2 pixel, uint2 src, uint2 dst, uint2 packed, float2 center,
                        uint2 bounds) {
  float2 pos = (float2(pixel) + 0.5) * float2(dst) / float2(src) - 0.5;
  [branch] if (FovAxisOn(packed.x) && src.x != dst.x) {
    pos.x = FovWarp1((float(pixel.x) + 0.5) / float(src.x), packed.x, center.x, bounds.x)
        * float(dst.x) - 0.5;
  }
  [branch] if (FovAxisOn(packed.y) && src.y != dst.y) {
    pos.y = FovWarp1((float(pixel.y) + 0.5) / float(src.y), packed.y, center.y, bounds.y)
        * float(dst.y) - 0.5;
  }
  return pos;
}

// Full-grid rectangle [lo, hi) covered by working texel `pixel` (`work`
// texels over `full`): the encode footprint, exact under the warp.
void FovWorkFootprint(uint2 pixel, uint2 work, uint2 full, uint2 packed, float2 center,
                      uint2 bounds, inout float2 lo, inout float2 hi) {
  [branch] if (FovAxisOn(packed.x) && work.x != full.x) {
    lo.x = FovUnwarp1(float(pixel.x) / float(work.x), packed.x, center.x, bounds.x)
        * float(full.x);
    hi.x = FovUnwarp1(float(pixel.x + 1u) / float(work.x), packed.x, center.x, bounds.x)
        * float(full.x);
  }
  [branch] if (FovAxisOn(packed.y) && work.y != full.y) {
    lo.y = FovUnwarp1(float(pixel.y) / float(work.y), packed.y, center.y, bounds.y)
        * float(full.y);
    hi.y = FovUnwarp1(float(pixel.y + 1u) / float(work.y), packed.y, center.y, bounds.y)
        * float(full.y);
  }
  lo = max(lo, 0.0);
  hi = min(hi, float2(full));
}

// How much of NR's change a full-grid position keeps: 1 inside the midpoint
// zone, 0 outside it (the game's own pixels), smoothstep over `blend` inside
// each bound that lies on screen.  A bound at the screen edge has nothing
// beyond it, so it never fades.  1 for identity axes.
float FovZoneWeight1(float n, uint packed, uint bounds, float blend) {
  [branch] if (!FovAxisOn(packed) || bounds == 0u) return 1.0;
  const float2 m = FovBounds(bounds);
  const float f = max(blend, 1e-6);
  float w = 1.0;
  [branch] if (m.x > 0.0) w *= smoothstep(m.x, m.x + f, n);
  [branch] if (m.y < 1.0) w *= 1.0 - smoothstep(m.y - f, m.y, n);
  return w;
}

float FovZoneWeight(float2 n, uint2 packed, uint2 bounds, float2 blend) {
  return FovZoneWeight1(n.x, packed.x, bounds.x, blend.x)
      * FovZoneWeight1(n.y, packed.y, bounds.y, blend.y);
}

#endif  // DLSS5_FOVEATION_HLSLI
