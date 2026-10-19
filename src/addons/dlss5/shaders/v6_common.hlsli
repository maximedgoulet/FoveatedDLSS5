Texture2D<float4> Original : register(t0);
Texture2D<float4> Proxy : register(t1);
Texture2D<float4> Neural : register(t2);
Texture2D<float4> OutputOriginal : register(t3);
RWTexture2D<float4> Output : register(u0);

cbuffer CodecConstants : register(b0) {
  uint2 Size;
  uint2 SourceSize;
  uint2 SourceBase;
  uint2 ProxySize;
  uint2 NeuralSize;
  float Divisor;
  float PedestalCap;
  float TransferStrength;
  float ColorStrength;
  uint Encoding;
  float DarkGate;
  // 0 = divisor/sRGB family (linear/Divisor, hue knee at 0.75); 1 = Display
  // fixed curve (localized shadow toe + hue knee; Divisor carries the anchor);
  // 2 = the divisor family with the neural-floor chroma guard in
  // UpgradeToneMap (NRNeuralFloorGuard).  Only `Curve == 1u` selects the
  // Display branches, so 2 encodes and resolves as the divisor family.
  uint Curve;
  // 1 = apply the commit's dark-pedestal removal (NRPedestalRestore on);
  // 0 = diagnose without it.
  float PedestalGate;
  // Normalization governor (v6.1.2), consumed by v6_autoscale ONLY; every
  // other program leaves all four at 0.  SlewStops bounds how far the
  // committed divisor may move in one frame, in log2 stops (0 = governor
  // off, the v6.1.0 behavior where the per-frame candidate IS the applied
  // divisor); under GovernorMode 2 it is the RISING (attack) bound only.
  // SnapNow = 1 discards the limit for this frame and adopts the candidate
  // outright: the first frame of a workset (mode 2), or also a reset-epoch
  // advance / game-signalled cut (mode 1).  See PrepareFrameScale.
  float SlewStops;
  float SnapNow;
  // Chroma bound of the bounded log-space transfer, in stops (both signs).
  float ChromaClampStops;
  // GovernorMode 2 only: the FALLING (release) bound for this frame, stops.
  float ReleaseStops;
  // 0/1 = the v6.1.0/v6.1.2 step-gated estimator with a symmetric limit;
  // 2 = Stable (v6.3): continuous estimator, attack/release limits and a
  // hysteresis hold.  See v6_autoscale.
  uint GovernorMode;
  // Resolve transfer on the Display codec (v6_resolve): 0 = bounded ratio
  // (the N/P gain alone), 1 = consistent (adds the curve-gain correction
  // below).  Only BindCodecV6's resolve dispatch reads it.
  uint TransferMode;
  // Foveated working grid (foveation.hlsli), dwords 24..27: the per-axis
  // packed warp and its host-computed G(1).  0 = uniform axis; every working
  // <-> full mapping below is bit-identical to the uniform one then.
  uint2 Foveation;
  float2 FoveationGmax;
};

#include "foveation.hlsli"

// Display proxy curve (the Curve == 1 branch of v6_encode) as a function of
// the max channel m in proxy units: f = shoulder(toe(m)).  The encode scales
// all three channels by g(m) = f(m) / m, so P = x * g(m) and max(P) = f(m):
// the curve is recoverable from the proxy alone, at proxy resolution.
static const float kDisplayToeA = 0.014;
static const float kDisplayToeB = 0.001;
static const float kDisplayKnee = 0.75;
static const float kDisplayShoulderRate = 5.770780;

float DisplayCurve(float m) {
  const float t = m + kDisplayToeA * m / (m + kDisplayToeB);
  return t > kDisplayKnee
      ? kDisplayKnee
          + (1.0 - kDisplayKnee)
              * (1.0 - exp(-kDisplayShoulderRate * (t - kDisplayKnee)))
      : t;
}

// s(m) = d(srgb(f)) / d(ln m): how much ENCODED proxy signal one neper of
// scene brightness produces.  The model reads and writes the sRGB-encoded
// plane, so its error is uniform there, not in linear light; 1/s is then the
// nepers of scene change per unit of model error - the conditioning of the
// inverse - and it collapses on the shoulder.
float DisplayCurveLogSlope(float m) {
  const float t = m + kDisplayToeA * m / (m + kDisplayToeB);
  const float toe_slope =
      1.0 + kDisplayToeA * kDisplayToeB / ((m + kDisplayToeB) * (m + kDisplayToeB));
  const float shoulder_slope = t > kDisplayKnee
      ? (1.0 - kDisplayKnee) * kDisplayShoulderRate
          * exp(-kDisplayShoulderRate * (t - kDisplayKnee))
      : 1.0;
  const float f = DisplayCurve(m);
  const float encode_slope = f > 0.0031308
      ? (1.055 / 2.4) * pow(f, 1.0 / 2.4 - 1.0)
      : 12.92;
  return m * toe_slope * shoulder_slope * encode_slope;
}

// Exact inverse: the shoulder by its log, the toe by the positive root of
// m^2 + (A + B - t) m - t B = 0, taken in the cancellation-free form on each
// sign of b (the dark range has b < 0 and would lose every digit otherwise).
// u == 1 maps to a finite value; the damping weight is ~0 there anyway.
float DisplayCurveInverse(float u) {
  const float t = u > kDisplayKnee
      ? kDisplayKnee
          - log(max(1.0 - (u - kDisplayKnee) / (1.0 - kDisplayKnee), 1e-7))
              / kDisplayShoulderRate
      : u;
  const float b = t - kDisplayToeA - kDisplayToeB;
  const float root = sqrt(b * b + 4.0 * t * kDisplayToeB);
  return b < 0.0 ? 2.0 * t * kDisplayToeB / (root - b) : 0.5 * (b + root);
}

#include "codec_math.hlsli"

// ST.2084 (PQ) transfer, reference white normalized to 1.0 = 10000 nits.
float3 LinearToPQ(float3 lin) {
  lin = saturate(lin);
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  float3 x = pow(lin, m1);
  float3 y = (c1 + c2 * x) / (1.0 + c3 * x);
  return pow(y, m2);
}

float3 PQToLinear(float3 pq) {
  pq = saturate(pq);
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  float3 x = pow(pq, 1.0 / m2);
  float3 y = max(x - c1, 0.0) / (c2 - c3 * x);
  return pow(y, 1.0 / m1);
}

float3 CbrtSigned(float3 value) {
  return sign(value) * pow(abs(value), 1.0 / 3.0);
}

float3 ToOkLab(float3 color) {
  const float3x3 rgb_to_lms = {
    0.4122214708, 0.5363325363, 0.0514459929,
    0.2119034982, 0.6806995451, 0.1073969566,
    0.0883024619, 0.2817188376, 0.6299787005
  };
  const float3x3 lms_to_lab = {
    0.2104542553, 0.7936177850, -0.0040720468,
    1.9779984951, -2.4285922050, 0.4505937099,
    0.0259040371, 0.7827717662, -0.8086757660
  };
  return mul(lms_to_lab, CbrtSigned(mul(rgb_to_lms, color)));
}

float3 FromOkLab(float3 lab) {
  const float3x3 lab_to_lms = {
    1.0, 0.3963377774, 0.2158037573,
    1.0, -0.1055613458, -0.0638541728,
    1.0, -0.0894841775, -1.2914855480
  };
  const float3x3 lms_to_rgb = {
    4.0767416621, -3.3077115913, 0.2309699292,
    -1.2684380046, 2.6097574011, -0.3413193965,
    -0.0041960863, -0.7034186147, 1.7076147010
  };
  float3 lms = mul(lab_to_lms, lab);
  return mul(lms_to_rgb, lms * lms * lms);
}

float3 ClampAp1(float3 color) {
  const float3x3 bt709_to_ap1 = {
    0.613097, 0.339523, 0.047379,
    0.070194, 0.916354, 0.013452,
    0.020616, 0.109570, 0.869815
  };
  const float3x3 ap1_to_bt709 = {
    1.705051, -0.621792, -0.083259,
    -0.130256, 1.140805, -0.010548,
    -0.024003, -0.128969, 1.152972
  };
  return mul(ap1_to_bt709, max(0.0, mul(bt709_to_ap1, color)));
}

float3 HueOkLab(float3 incorrect, float3 correct) {
  float3 incorrect_lab = ToOkLab(incorrect);
  const float3 correct_lab = ToOkLab(correct);
  const float incorrect_chroma = length(incorrect_lab.yz);
  const float correct_chroma = length(correct_lab.yz);
  incorrect_lab.yz = correct_lab.yz
      * (correct_chroma == 0.0 ? 1.0 : incorrect_chroma / correct_chroma);
  return ClampAp1(FromOkLab(incorrect_lab));
}

float3 UpgradeToneMap(float3 original, float3 proxy, float3 neural) {
  const float original_y = Luminance(original);
  const float proxy_y = Luminance(proxy);
  const float neural_y = Luminance(neural);
  // The DLSSNR proxy output can be degenerate/empty for some HDR inputs (e.g. a
  // PQ buffer linearized through sRGB).  With TransferStrength=1 the ratio path
  // below sets ratio=0 when neural_y==0, which makes `scaled` (and therefore
  // `upgraded`) collapse to black; with ColorStrength=0 the final result is then
  // original*0 -> a fully black frame.  Preserve the original scene instead.
  // Single-exit: FXC X4000-proof (every path returns an initialized value).
  float3 result = original;
  [branch]
  if (neural_y > 1e-5) {
    float ratio = original_y / max(proxy_y, 1e-6);
    if (original_y >= proxy_y) {
      const float new_y = neural_y + max(0.0, original_y - proxy_y);
      ratio = new_y / neural_y;
    }
    float3 scaled = HueOkLab(neural * ratio, neural);
    // Neural-floor chroma guard (Curve 2; ported from v6.6.0's C6).  `ratio`
    // carries the neural colour to the target luminance; for a degenerate
    // neural result (near-black, one channel at a few 8-bit codes) over a
    // bright original it reaches 1e3-1e5 and turned an orange highlight
    // green (harness C6: 40 stops of chroma error).  The colour falls back
    // to the original's only where the neural value sits at the floor AND
    // the pixel is amplified past a stop; the transferred luminance
    // (neural_y * ratio) is kept either way, so this is no tone curve.
    // Trust 1 - above the floor, ratio <= 2, or Curve 0 - is the previous
    // math exactly.
    const float trust = Curve == 2u
        ? max(ProxyFloorTrust(neural), 1.0 - smoothstep(1.0, 3.0, log2(max(ratio, 1.0))))
        : 1.0;
    if (trust < 1.0) {
      scaled = lerp(original * (neural_y * ratio / max(original_y, 1e-6)), scaled, trust);
    }
    result = lerp(original, scaled, TransferStrength);
  }
  return result;
}

// Proxy-domain decode back to linear display units.  This is a FIXED decode
// (plain sRGB) applied identically to the conditioned proxy (P) and the raw
// neural output (N) - it is NOT the inverse of the encode curve.  It never
// needs to be: the Display path only consumes P and N through their ratio,
// and N ~ P keeps that ratio near 1 even where the decode mismatches the
// encoder.
float3 DecodeProxyLinear(float3 encoded) {
  return SrgbDecode(encoded);
}

// Continuous texel position on a working-resolution surface (P or N) of the
// full-grid pixel centre `pixel`, warped per axis on a foveated grid.
float2 WorkPosFromFull(uint2 tex_size, uint2 pixel) {
  return FovFullToWorkPos(pixel, Size, tex_size, Foveation, FoveationGmax);
}

float3 SampleProxyLinearBilinear(uint2 tex_size, uint2 pixel, float2 pos) {
  float3 result;
  if (all(tex_size == Size)) {
    result = DecodeProxyLinear(Proxy.Load(int3(pixel, 0)).rgb);
  } else {
  const int2 max_coord = int2(tex_size) - int2(1, 1);
  const int2 b0 = clamp(int2(floor(pos)), int2(0, 0), max_coord);
  const int2 b1 = min(b0 + int2(1, 1), max_coord);
  const float2 f = saturate(pos - float2(b0));
  const float3 s00 = DecodeProxyLinear(Proxy.Load(int3(b0, 0)).rgb);
  const float3 s10 = DecodeProxyLinear(Proxy.Load(int3(int2(b1.x, b0.y), 0)).rgb);
  const float3 s01 = DecodeProxyLinear(Proxy.Load(int3(int2(b0.x, b1.y), 0)).rgb);
  const float3 s11 = DecodeProxyLinear(Proxy.Load(int3(b1, 0)).rgb);
    result = s00 * (1.0 - f.x) * (1.0 - f.y) + s10 * f.x * (1.0 - f.y)
      + s01 * (1.0 - f.x) * f.y + s11 * f.x * f.y;
  }
  return result;
}

float3 SampleNeuralLinearBilinear(uint2 tex_size, uint2 pixel, float2 pos) {
  float3 result;
  if (all(tex_size == Size)) {
    result = DecodeProxyLinear(Neural.Load(int3(pixel, 0)).rgb);
  } else {
  const int2 max_coord = int2(tex_size) - int2(1, 1);
  const int2 b0 = clamp(int2(floor(pos)), int2(0, 0), max_coord);
  const int2 b1 = min(b0 + int2(1, 1), max_coord);
  const float2 f = saturate(pos - float2(b0));
  const float3 s00 = DecodeProxyLinear(Neural.Load(int3(b0, 0)).rgb);
  const float3 s10 = DecodeProxyLinear(Neural.Load(int3(int2(b1.x, b0.y), 0)).rgb);
  const float3 s01 = DecodeProxyLinear(Neural.Load(int3(int2(b0.x, b1.y), 0)).rgb);
  const float3 s11 = DecodeProxyLinear(Neural.Load(int3(b1, 0)).rgb);
    result = s00 * (1.0 - f.x) * (1.0 - f.y) + s10 * f.x * (1.0 - f.y)
      + s01 * (1.0 - f.x) * f.y + s11 * f.x * f.y;
  }
  return result;
}

// One pixel of the v6 resolve, in linear source units.  Users: v6_resolve,
// and v6_pedestal_reduce built with MEASURE_UNSHAPED (the pedestal measuring
// NR's own lift while the look shapes).
float3 ResolvePixel(uint2 pixel, float3 original) {
  // One texel position per surface; P and N share it in the usual case of
  // equal extents, so the (foveated) mapping runs once per pixel.
  const uint2 proxy_size = max(ProxySize, uint2(1, 1));
  const uint2 neural_size = max(NeuralSize, uint2(1, 1));
  const float2 proxy_pos = WorkPosFromFull(proxy_size, pixel);
  float2 neural_pos = proxy_pos;
  [branch] if (any(neural_size != proxy_size)) neural_pos = WorkPosFromFull(neural_size, pixel);
  const float3 proxy = SampleProxyLinearBilinear(proxy_size, pixel, proxy_pos);
  const float3 neural = SampleNeuralLinearBilinear(neural_size, pixel, neural_pos);
  float3 result;
  [branch]
  if (Curve == 1u) {
    // Display codec: apply the neural residual directly in bounded log space.
    // N == P is exact identity.  Luma is bounded to +/-2 stops and chroma to
    // +/-ChromaClampStops so invalid ratios cannot explode the untouched HDR
    // source.  There is deliberately no host-side health/authority multiplier.
    const float3 safe_ratio =
        max(neural, 1.0 / 1024.0) / max(proxy, 1.0 / 1024.0);
    const float3 lg = log2(safe_ratio);
    const float luma = dot(lg, float3(0.212639, 0.715169, 0.072192));
    const float3 chroma = lg - luma;
    // Consistent transfer (TransferMode 1).  The ratio N/P undoes the encode
    // gain g(m_P) the proxy was built with, but a brightness edit moves the
    // pixel along the curve, where the gain is g(m_N): the exact inverse of
    // the encode is x * (N/P) * g(m_P)/g(m_N).  Without the last factor a
    // +1 stop edit is ~half lost at 0.5-1x white and entirely lost above it
    // (harness: exposure+1.00, curve 1).  m_N is a damped inverse: MAP in
    // ln m with weight w = s^2 / (s^2 + lambda), s = d(encoded f)/d(ln m) at
    // the worse end, so w -> 1 where the curve is invertible and w -> 0 on
    // the flat shoulder, where this reduces to the bounded ratio above.
    // lambda is a noise budget: output change per unit of encoded proxy error
    // peaks at 1 / (2 sqrt(lambda)) nepers, so 1.28e-4 bounds the
    // correction's share at 0.25 stops per 8-bit code of model error.
    // Both gains come from the same forward function, so N == P stays an
    // exact identity.
    float correction = 0.0;
    if (TransferMode == 1u) {
      const float m_proxy =
          DisplayCurveInverse(max(proxy.x, max(proxy.y, proxy.z)));
      const float m_target =
          DisplayCurveInverse(max(neural.x, max(neural.y, neural.z)));
      if (m_proxy > 1e-7 && m_target > 1e-7) {
        const float s = min(
            DisplayCurveLogSlope(m_proxy), DisplayCurveLogSlope(m_target));
        const float m_neural =
            m_proxy * pow(m_target / m_proxy, s * s / (s * s + 1.28e-4));
        correction = log2(
            (DisplayCurve(m_proxy) / m_proxy)
            / (DisplayCurve(m_neural) / m_neural));
      }
    }
    const float l = clamp(luma + correction, -2.0, 2.0);
    const float3 c = clamp(chroma, -ChromaClampStops, ChromaClampStops);
    // The whole bounded gain rides the denominator-floor trust: the luma term
    // used to bypass it, so in the floor-dominated band of a very dark title
    // every pixel carried a +/-2-stop multiplier built from ratio noise, with
    // no reason to be temporally stable. Trust == 1 (well-lit pixels) keeps
    // the exact previous math.
    const float floor_trust = ProxyFloorTrust(proxy);
    const float3 gain = exp2(floor_trust * (l + c));
    result = lerp(original, original * gain, ColorStrength);
  } else {
    // Divisor family: UpgradeToneMap compares in proxy units (the encode
    // divided by Divisor); scale the neural result back to source units
    // immediately so the work surface keeps a single unit system.
    const float divisor = Divisor > 0.0
        ? Divisor
        : max(OutputOriginal.Load(int3(0, 0, 0)).r, 1e-8);
    const float3 upgraded = UpgradeToneMap(max(original, 0.0) / divisor, proxy, neural) * divisor;
    const float original_y = max(Luminance(original), 0.0);
    const float upgraded_y = Luminance(upgraded);
    const float ratio = original_y == 0.0 ? 1.0 : clamp(upgraded_y / original_y, 0.0, 4.0);
    const float3 luminance_only = original * ratio;
    result = lerp(luminance_only, upgraded, ColorStrength);
  }
  return result;
}
