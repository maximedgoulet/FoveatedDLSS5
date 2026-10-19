// NR look stage (PLAN_NR_LOOK_V71.md): the bindings of the look root
// signature (look_stage.hpp) and the edit-field math every look program
// shares.  tools/look/look_reference.py mirrors each function in float32 and
// is the oracle the EditRig fixtures compare against; change them together.
//
// DLSSNR is a Neural Rendering enhancer.  Nothing here denoises: the stage
// reshapes the network's EDIT e = log2(n / p) per channel and writes
// N' = n * 2^(t * (e' - e)), so e' == e returns the network's own value.

#include "codec_math.hlsli"
#include "foveation.hlsli"

// Root parameter 0: a codec per-pass SRV set (kDescriptor* in dlssnr.hpp),
// or the pass's look set, whose Neural slot is N' instead of N.
Texture2D<float4> Reference : register(t0);  // the pass's reference image
Texture2D<float4> Proxy : register(t1);      // P, the NR input (sRGB-encoded)
Texture2D<float4> Neural : register(t2);     // N, or N' in the look set
// Root parameter 5: one view of the workset's motion-vector ring.
Texture2D<float2> Motion : register(t4);
// Root parameter 1: the workset's look UAV region, all resting in
// UNORDERED_ACCESS.
RWTexture2D<float4> LookOutput : register(u0);  // N' (NR res; .a = G_low)
RWTexture2D<float4> BandA : register(u1);       // half NR res
RWTexture2D<float4> BandB : register(u2);
RWTexture2D<float> BandMaxA : register(u3);
RWTexture2D<float> BandMaxB : register(u4);
RWTexture2D<float4> UpProxy : register(u5);     // full res (transport)
RWTexture2D<float4> UpNeural : register(u6);
RWByteAddressBuffer TraceHistogram : register(u7);
RWTexture2D<float4> TraceBlocks : register(u8);
// Root parameters 4 and 3: the per-NR-handle history ping-pong.  Both halves
// live in UNORDERED_ACCESS for their whole life (like the governor's
// norm_commit), so the host orders frames with UAV barriers only.
RWByteAddressBuffer HistoryOut : register(u9);
RWByteAddressBuffer HistoryIn : register(u10);

// 32 root constants; LookConstants in look_stage.hpp mirrors it.
cbuffer LookConstants : register(b0) {
  uint2 Size;       // this dispatch's grid
  uint2 NrSize;     // the NR working resolution (P, N, N', history)
  uint2 FullSize;   // the pass's reference / resolve resolution
  uint Flags;       // kFlag* below
  uint Mode;        // program-specific (band pass, trace step)
  float Strength;   // A: stops multiplier of the whole edit
  float Brighten;   // A: gain on a > 0
  float Darken;     // A: gain on a < 0
  float MaxBrighten;  // A: soft knee in stops, 0 = off
  float MaxDarken;
  float ColourGain;   // B: radial (toward/away from grey)
  float HueGain;      // B: tangential (tint)
  float MaxColour;    // B: soft knee on the w-norm, 0 = off
  float Shadows;      // C
  float Midtones;
  float Highlights;
  float Tone;         // D: large-scale band
  float Detail;       // D: fine band
  float Halo;         // D: darkening suppression near brighter pixels
  uint Radius;        // D: box radius, half-res pixels
  float Alpha;        // E: 1 - exp(-dt / tau)
  float2 MotionScale; // E: MV texel -> NR pixels (previous = current + MV)
  uint2 MotionBase;   // E: MV subrect origin
  uint2 MotionGrid;   // E: the pixel grid the MV texels describe
  float GfEps;        // D: guided-filter regularization, stops^2
  uint Padding0;
  // Foveated working grid (foveation.hlsli), dwords 32..35: per-axis packed
  // warp and host-computed G(1); 0 = uniform axis.
  uint2 Foveation;
  float2 FoveationGmax;
};

static const uint kFlagBands = 1u << 0;
static const uint kFlagTemporal = 1u << 1;
static const uint kFlagMotion = 1u << 2;
static const uint kFlagTemporalDetail = 1u << 3;
static const uint kFlagTransport = 1u << 4;
static const uint kFlagRefLinear = 1u << 5;
static const uint kFlagHistoryValid = 1u << 6;

static const float kRatioFloor = 1.0 / 1024.0;  // v6_resolve's N/P floor
static const float3 kW = float3(0.212639, 0.715169, 0.072192);
static const float kLog2Grey = -2.4739312;      // log2(0.18)
static const float kGuideFloor = 1e-8;

// The network's edit at one NR pixel (section 2 of the plan).
struct Edit {
  float3 n;   // linear network output
  float3 lp;  // log2 of the linear input, floored
  float3 e;   // per-channel edit, stops
  float a;    // achromatic edit: w-weighted mean of e
  float3 c;   // opponent edit, dot(w, c) = 0
  float I;    // log2 relative luminance of the input
  float t;    // floor trust
};

Edit ReadEdit(int2 pixel) {
  Edit x;
  const float3 p = SrgbDecode(Proxy.Load(int3(pixel, 0)).rgb);
  x.n = SrgbDecode(Neural.Load(int3(pixel, 0)).rgb);
  x.lp = log2(max(p, kRatioFloor));
  x.e = log2(max(x.n, kRatioFloor)) - x.lp;
  x.a = dot(kW, x.e);
  x.c = x.e - x.a;
  x.I = log2(max(dot(kW, p), kRatioFloor));
  x.t = ProxyFloorTrust(p);
  return x;
}

// Radial and tangential parts of the colour edit in the input's own
// log-opponent direction o; s fades the split out near grey.
void ColourSplit(float3 c, float3 lp, out float3 c_r, out float3 c_t, out float s) {
  const float3 o = lp - dot(kW, lp);
  const float oo = dot(kW, o * o);
  c_r = (dot(kW, c * o) / max(oo, 1e-6)) * o;
  c_t = c - c_r;
  s = smoothstep(0.05, 0.25, sqrt(oo));
}

float ZoneGain(float I) {
  const float L = I - kLog2Grey;
  const float w_sh = 1.0 - smoothstep(-3.5, -1.5, L);
  const float w_hi = smoothstep(0.5, 2.5, L);
  return w_sh * Shadows + (1.0 - w_sh - w_hi) * Midtones + w_hi * Highlights;
}

// Every gain acts on stops; the operators are C0 and monotonic.  The
// direction split is piecewise linear: the plan's smoothstep blend
// a * lerp(Darken, Brighten, s(a)) is not monotonic when the gains differ
// (look_reference.py --selftest measures the bump).
//
// Halo lifts only the darkening DETAIL a_D next to brighter pixels (plan
// section 3 D).  Until rc6 it lifted the whole darkening there: a surface NR
// darkened evenly came back lighter inside the neighbourhood of every bright
// feature, a lighter box around the letters on a dark jacket (Alan Wake 2).
// NR's measured overshoot there is 2-4 NR px and 0.1 stop deep, well inside
// the fine band; wider halos need a larger Detail radius.
float3 ShapeEdit(Edit x, float aB, float aD, float3 c, float m) {
  aD -= Halo * smoothstep(1.0, 3.0, m - x.I) * min(aD, 0.0);
  float a = Tone * aB + Detail * aD;
  a = Brighten * max(a, 0.0) + Darken * min(a, 0.0);
  float3 c_r;
  float3 c_t;
  float s;
  ColourSplit(c, x.lp, c_r, c_t, s);
  c = s * (ColourGain * c_r + HueGain * c_t) + (1.0 - s) * ColourGain * c;
  const float g = Strength * ZoneGain(x.I);
  a *= g;
  c *= g;
  // fxc expands tanh(x) as (e^x - e^-x) / (e^x + e^-x): past |x| ~ 88 e^x
  // overflows and the result is Inf * 0 = NaN, a white pixel (EditRig T7,
  // every gain 2 under 0.25-stop knees).  tanh(10) is 1.0 in float32, so the
  // clamp changes nothing else.
  if (a > 0.0 && MaxBrighten > 0.0) a = MaxBrighten * tanh(min(a / MaxBrighten, 10.0));
  if (a < 0.0 && MaxDarken > 0.0) a = MaxDarken * tanh(max(a / MaxDarken, -10.0));
  if (MaxColour > 0.0) {
    const float norm = sqrt(dot(kW, c * c));
    if (norm > 1e-6) c *= MaxColour * tanh(min(norm / MaxColour, 10.0)) / norm;
  }
  return a + c;
}

// Half-res band coefficients (A, B) at an NR pixel: bilinear at
// (x + 0.5) * 0.5 - 0.5, clamped; `peak` is the dilated input (the halo
// neighbourhood) at the same taps, also bilinear: the round dilation is
// continuous, so the halo weight has no 2-pixel steps.
float2 BandCoefficients(int2 pixel, out float peak) {
  const uint2 half_size = (NrSize + 1u) / 2u;
  const float2 pos = clamp((float2(pixel) + 0.5) * 0.5 - 0.5, 0.0,
                           float2(half_size - 1u));
  const int2 b0 = int2(floor(pos));
  const int2 b1 = min(b0 + 1, int2(half_size) - 1);
  const float2 f = pos - float2(b0);
  const float2 q00 = BandA[b0].xy;
  const float2 q10 = BandA[int2(b1.x, b0.y)].xy;
  const float2 q01 = BandA[int2(b0.x, b1.y)].xy;
  const float2 q11 = BandA[b1].xy;
  peak = BandMaxA[b0] * (1.0 - f.x) * (1.0 - f.y) + BandMaxA[int2(b1.x, b0.y)] * f.x * (1.0 - f.y)
      + BandMaxA[int2(b0.x, b1.y)] * (1.0 - f.x) * f.y + BandMaxA[b1] * f.x * f.y;
  return q00 * (1.0 - f.x) * (1.0 - f.y) + q10 * f.x * (1.0 - f.y)
      + q01 * (1.0 - f.x) * f.y + q11 * f.x * f.y;
}

// History texel: (a_B, a_D, c.r, c.b) and the input's I, as halves in three
// words; c.g follows from dot(w, c) = 0.
uint HistoryAddress(uint2 pixel) {
  return (pixel.y * NrSize.x + pixel.x) * 12u;
}
