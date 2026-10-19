Texture2D<float4> Original : register(t0);
Texture2D<float4> Proxy : register(t1);
Texture2D<float4> Neural : register(t2);
Texture2D<float4> OutputOriginal : register(t3);
RWTexture2D<float4> Output : register(u0);

// Legacy cbuffer view: dwords 0..14 named, dwords 15..18 the float4 padding.
// Declared size 76 B (rounded to 80 by HLSL); the root signature carries 24
// dwords (96 B) and the legacy shaders never read beyond dword 19, so the
// v9 Phase 6 extension (dwords 20..23, see the map at kV6CBuffer) is inert
// here.
cbuffer CodecConstants : register(b0) {
  uint2 Size;
  uint2 SourceSize;
  uint2 SourceBase;
  uint2 ProxySize;
  uint2 NeuralSize;
  float PaperWhiteScale;
  float TransferStrength;
  float ColorStrength;
  uint HdrMode;
  float DiffuseWhiteNits;
  // float4 (not float[]) so the declared cbuffer size stays 16-byte aligned
  // (76 -> 80 B) - a scalar array would 16-byte-stride past it and trip the
  // debug layer's undersized-root-constant check against the root signature.
  float4 Padding;
  // dwords 20..23 unused; the foveated grid (foveation.hlsli) is dwords
  // 24..27 in every codec view.  0 = uniform axis.
  float4 Padding2;
  uint2 Foveation;
  float2 FoveationGmax;
};

#include "codec_math.hlsli"
#include "foveation.hlsli"

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
    const float3 scaled = HueOkLab(neural * ratio, neural);
    result = lerp(original, scaled, TransferStrength);
  }
  return result;
}

// One pixel of the legacy resolve, encoded for the write surface.  Users:
// legacy_decode, and legacy_reduce built with MEASURE_UNSHAPED (the
// black-level restore measuring NR's own lift while the look shapes).
float3 ResolvePixel(uint2 pixel, float3 source) {
  const uint2 proxy_size = max(ProxySize, uint2(1, 1));
  uint2 proxy_pixel = min(
      uint2(((float2(pixel) + 0.5) * float2(proxy_size)) / float2(Size)),
      proxy_size - 1);
  [branch] if (FovAny(Foveation)) {
    proxy_pixel = min(
        uint2(max(FovFullToWorkPos(pixel, Size, proxy_size, Foveation, FoveationGmax) + 0.5,
                  0.0)),
        proxy_size - 1u);
  }
  // The neural surface can run below the output resolution (v5 NR resolution
  // control): sample it with the same scaled mapping as the proxy.  At
  // 1:1 (native) the mapping collapses to the identity.
  const uint2 neural_size = max(NeuralSize, uint2(1, 1));
  uint2 neural_pixel = min(
      uint2(((float2(pixel) + 0.5) * float2(neural_size)) / float2(Size)),
      neural_size - 1);
  [branch] if (FovAny(Foveation)) {
    neural_pixel = min(
        uint2(max(FovFullToWorkPos(pixel, Size, neural_size, Foveation, FoveationGmax) + 0.5,
                  0.0)),
        neural_size - 1u);
  }

  float3 original;
  float3 proxy;
  float3 neural;
  float3 upgraded;
  if (HdrMode == 1) {
    original = max(source, 0.0) / PaperWhiteScale;
    proxy = SrgbDecode(Proxy.Load(int3(proxy_pixel, 0)).rgb);
    neural = SrgbDecode(Neural.Load(int3(neural_pixel, 0)).rgb);
    upgraded = UpgradeToneMap(original, proxy, neural);
  } else if (HdrMode == 2) {
    original = PQToLinear(max(source, 0.0)) / ((DiffuseWhiteNits / 10000.0) * PaperWhiteScale);
    proxy = SrgbDecode(Proxy.Load(int3(proxy_pixel, 0)).rgb);
    neural = SrgbDecode(Neural.Load(int3(neural_pixel, 0)).rgb);
    upgraded = UpgradeToneMap(original, proxy, neural);
  } else {
    original = source;
    proxy = Proxy.Load(int3(proxy_pixel, 0)).rgb;
    neural = Neural.Load(int3(neural_pixel, 0)).rgb;
    upgraded = lerp(original, neural, TransferStrength);
  }

  const float original_y = Luminance(original);
  const float upgraded_y = Luminance(upgraded);
  // Clamp the luma ratio: an HDR proxy is display-referred (<=~0.75 after the
  // soft-clip) while `original` is scene-linear (can exceed 1.0), so the raw
  // ratio would otherwise dim or blow up bright highlights.  Keep it in a sane
  // range so the frame is never blacked out and highlights stay bounded.
  const float ratio = original_y == 0.0 ? 1.0 : clamp(upgraded_y / original_y, 0.0, 4.0);
  const float3 luminance_only = original * ratio;
  const float3 result = lerp(luminance_only, upgraded, ColorStrength);

  float3 encoded = result;
  if (HdrMode == 1) {
    encoded = result * PaperWhiteScale;
  } else if (HdrMode == 2) {
    // Undo the encode-side paper-white/diffuse-white normalization before re-encoding PQ.
    encoded = LinearToPQ(result * ((DiffuseWhiteNits / 10000.0) * PaperWhiteScale));
  }
  return encoded;
}
