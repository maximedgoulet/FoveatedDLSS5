// Pure colour math shared by the codec families (legacy_common, v6_common)
// and the NR look stage (look_common).  No resources and no constants here:
// each family declares its own bindings and cbuffer over the same registers,
// so only functions can be shared.

float Luminance(float3 color) {
  return dot(color, float3(0.212639, 0.715169, 0.072192));
}

float3 SrgbEncode(float3 color) {
  color = saturate(color);
  return color <= 0.0031308
      ? color * 12.92
      : 1.055 * pow(color, 1.0 / 2.4) - 0.055;
}

float3 SrgbDecode(float3 color) {
  color = saturate(color);
  return color <= 0.04045
      ? color / 12.92
      : pow((color + 0.055) / 1.055, 2.4);
}

// Per-pixel denominator-floor trust for the N/P ratio gain (luma and chroma):
// where the proxy sits at the floor the ratio is noise and cannot support any
// bounded correction. It is a mathematical bound, not a frame-health or
// engagement heuristic.  Users: v6_resolve (the Display transfer), the
// look stage (N' = n * 2^(t * (e' - e)), so floor pixels keep the network's
// value) and UpgradeToneMap's neural-floor chroma guard (v6_common, Curve 2).
float ProxyFloorTrust(float3 proxy_linear) {
  const float m = max(proxy_linear.x, max(proxy_linear.y, proxy_linear.z));
  return smoothstep(1.0 / 1024.0, 1.0 / 64.0, m);
}
