// DX11 bridge depth conversion (PLAN_DX11_V68.md 4.2, the XD route).
//
// Depth formats with stencil (R24G8 / R32G8X24) cannot be shared between
// the two APIs (probe X1: the debug layer refuses the share), so the bridge
// copies the game's depth into a private typeless twin and this pass reads
// its depth plane - bound as R24_UNORM_X8_TYPELESS or R32_FLOAT_X8X24_TYPELESS
// - into a shared R32_FLOAT twin that the D3D12 side opens.  Out-of-range
// threads are harmless: an out-of-bounds UAV write is discarded and an
// out-of-bounds Load returns 0.  No constant buffer, so the only CS state the
// pass touches is the shader, SRV 0 and UAV 0, which the bridge saves and
// restores exactly.

Texture2D<float> Depth : register(t0);
RWTexture2D<float> Twin : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  Twin[id.xy] = Depth.Load(int3(id.xy, 0));
}
