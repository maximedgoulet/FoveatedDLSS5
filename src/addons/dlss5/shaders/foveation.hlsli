// Foveated NR working grid (separable warp).
//
// DLSSNR is a same-resolution network, so a spatially varying working
// resolution is expressed as a warp: the frame is resampled into a smaller
// working image whose density, per axis, is 1 (relative to the uniform
// working scale) inside the centre zone, falls along a smoothstep ramp, and
// settles at Edge.  The axes are independent (corners get Edge x Edge).  The
// model runs once on the warped image with warped guides (fov_guides), and
// every stage that maps between the working grid and the full grid goes
// through FovWarp / FovUnwarp.
//
// Coordinates are normalized: n in [0, 1] across the full image, v in [0, 1]
// across the working image.  Both extend monotonically past [0, 1] so
// off-screen reprojection stays well defined.
//
// Per-axis packed dword (foveation.hpp mirrors it; 0 = identity axis):
//   bits  0..11  a: centre half-extent / screen half-extent, code / 4095
//   bits 12..23  b: fall-off length / screen half-extent, code / 2048
//   bits 24..30  e: edge density, code / 128
//   bit  31      enabled
// gmax = G(1) is computed once on the host and passed beside the dword, so no
// shader evaluates it per pixel.
//
// Cost: identity axes return immediately; the plateau and edge segments are
// closed form; only the fall-off band runs the 4-step Newton inverse.

#ifndef DLSS5_FOVEATION_HLSLI
#define DLSS5_FOVEATION_HLSLI

struct FovAxis {
  float a;     // plateau half-width
  float b;     // ramp length
  float e;     // edge density
  float k;     // 1 - e
  float gmax;  // G(1)
};

bool FovAxisOn(uint packed) {
  return (packed & 0x80000000u) != 0u;
}

bool FovAny(uint2 packed) {
  return ((packed.x | packed.y) & 0x80000000u) != 0u;
}

FovAxis FovDecode(uint packed, float gmax) {
  FovAxis p;
  p.a = float(packed & 0xFFFu) * (1.0 / 4095.0);
  p.b = float((packed >> 12) & 0xFFFu) * (1.0 / 2048.0);
  p.e = float((packed >> 24) & 0x7Fu) * (1.0 / 128.0);
  p.k = 1.0 - p.e;
  p.gmax = gmax;
  return p;
}

// G(tau): integral of the density over [0, tau], tau = |2n - 1|.  The ramp
// may run past tau = 1 (truncated by the screen edge); G stays monotone.
float FovG(float tau, FovAxis p) {
  [branch] if (tau <= p.a) return tau;
  const float ramp_end = p.a + p.b;
  const float ramp_area = p.b * (1.0 - 0.5 * p.k);
  [branch] if (tau >= ramp_end) return p.a + ramp_area + p.e * (tau - ramp_end);
  const float q = (tau - p.a) / p.b;
  const float q2 = q * q;
  return p.a + p.b * (q - p.k * (q2 * q - 0.5 * q2 * q2));
}

// Inverse of FovG.  The ramp segment h(q) = q - k(q^3 - q^4/2) has
// h'(q) = 1 - k * smoothstep(q) >= Edge >= 0.25, so Newton from q = r reaches
// fp32 precision in four steps (measured: <0.003 px at 7680 px, Edge 0.25).
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
float FovWarp1(float n, uint packed, float gmax) {
  [branch] if (!FovAxisOn(packed)) return n;
  const FovAxis p = FovDecode(packed, gmax);
  const float t = 2.0 * n - 1.0;
  return 0.5 + 0.5 * sign(t) * FovG(abs(t), p) / p.gmax;
}

float FovUnwarp1(float v, uint packed, float gmax) {
  [branch] if (!FovAxisOn(packed)) return v;
  const FovAxis p = FovDecode(packed, gmax);
  const float s = 2.0 * v - 1.0;
  return 0.5 + 0.5 * sign(s) * FovGInv(abs(s) * p.gmax, p);
}

float2 FovWarp(float2 n, uint2 packed, float2 gmax) {
  return float2(FovWarp1(n.x, packed.x, gmax.x), FovWarp1(n.y, packed.y, gmax.y));
}

float2 FovUnwarp(float2 v, uint2 packed, float2 gmax) {
  return float2(FovUnwarp1(v.x, packed.x, gmax.x), FovUnwarp1(v.y, packed.y, gmax.y));
}

// Continuous texel position on a `dst` grid (working) of the full-grid pixel
// centre `pixel` (src grid).  Uniform form: (pixel + 0.5) * dst / src - 0.5.
// An axis whose grids match is left alone even if its dword is set.
float2 FovFullToWorkPos(uint2 pixel, uint2 src, uint2 dst, uint2 packed, float2 gmax) {
  float2 pos = (float2(pixel) + 0.5) * float2(dst) / float2(src) - 0.5;
  [branch] if (FovAxisOn(packed.x) && src.x != dst.x) {
    pos.x = FovWarp1((float(pixel.x) + 0.5) / float(src.x), packed.x, gmax.x)
        * float(dst.x) - 0.5;
  }
  [branch] if (FovAxisOn(packed.y) && src.y != dst.y) {
    pos.y = FovWarp1((float(pixel.y) + 0.5) / float(src.y), packed.y, gmax.y)
        * float(dst.y) - 0.5;
  }
  return pos;
}

// Full-grid rectangle [lo, hi) covered by working texel `pixel` (`work`
// texels over `full`): the encode footprint, exact under the warp.
void FovWorkFootprint(uint2 pixel, uint2 work, uint2 full, uint2 packed, float2 gmax,
                      inout float2 lo, inout float2 hi) {
  [branch] if (FovAxisOn(packed.x) && work.x != full.x) {
    lo.x = FovUnwarp1(float(pixel.x) / float(work.x), packed.x, gmax.x) * float(full.x);
    hi.x = FovUnwarp1(float(pixel.x + 1u) / float(work.x), packed.x, gmax.x) * float(full.x);
  }
  [branch] if (FovAxisOn(packed.y) && work.y != full.y) {
    lo.y = FovUnwarp1(float(pixel.y) / float(work.y), packed.y, gmax.y) * float(full.y);
    hi.y = FovUnwarp1(float(pixel.y + 1u) / float(work.y), packed.y, gmax.y) * float(full.y);
  }
  lo = max(lo, 0.0);
  hi = min(hi, float2(full));
}

#endif  // DLSS5_FOVEATION_HLSLI
