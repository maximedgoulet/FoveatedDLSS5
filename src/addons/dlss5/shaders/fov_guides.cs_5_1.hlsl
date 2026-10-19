// Foveated NR guides: resamples the game's depth and motion vectors onto the
// warped working grid (foveation.hlsli), so DLSSNR's guides stay registered
// with the warped colour it is given.  Runs once per evaluate, before the
// stack; every stack pass reads the same pair.
//
// Bound with the codec root signature: t0 = game depth, t1 = game motion
// (t2/t3 repeat them), u0 = warped motion, u1 = warped depth.  The runtime
// then reads the outputs 1:1 with MVecScale = (1, 1).
//
// Motion: DLSS vectors point to the PREVIOUS position (previous = current +
// MV * MVecScale, in pixels of the grid the vectors are rendered at).  The
// displacement is carried through the warp, not scaled: a vector that
// crosses the fall-off gets the exact warped length at its endpoint.
//
// Cost: two point loads and one 8-byte + one 4-byte store per working texel;
// the unwarp runs its Newton steps only inside the fall-off bands.

#include "foveation.hlsli"

Texture2D<float> GuideDepth : register(t0);
Texture2D<float2> GuideMotion : register(t1);
RWTexture2D<float2> WarpedMotion : register(u0);
RWTexture2D<float> WarpedDepth : register(u1);

// The codec root signature's 28 dwords, this program's own view.  The warp
// sits at dwords 24..27 like every other codec view.
cbuffer FovGuideConstants : register(b0) {
  uint2 Size;          // working grid (the NR surface)
  uint2 MotionGrid;    // grid the vectors describe (render or output)
  uint2 MotionBase;    // MV subrect origin
  uint2 DepthGrid;     // render grid
  uint2 DepthBase;     // depth subrect origin
  float2 MotionScale;  // game MVecScale x the player's multiplier
  uint Flags;          // bit 0: depth bound, bit 1: motion bound
  uint Reserved0;      // scalars, not uint3: identical offsets under every packing rule
  uint Reserved1;
  uint Reserved2;
  uint4 Reserved3;
  uint4 Reserved4;
  uint2 Foveation;
  float2 FoveationGmax;
};

static const uint kFlagDepth = 1u;
static const uint kFlagMotion = 2u;

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;

  // Normalized full-frame position of this working texel's centre.
  const float2 n = FovUnwarp((float2(pixel) + 0.5) / float2(Size), Foveation, FoveationGmax);

  if ((Flags & kFlagDepth) != 0u) {
    // Nearest, never filtered: blending depth across a silhouette invents a
    // surface that exists in neither layer.
    const uint2 grid = max(DepthGrid, uint2(1, 1));
    const uint2 texel = DepthBase + min(uint2(saturate(n) * float2(grid)), grid - 1u);
    WarpedDepth[pixel] = GuideDepth.Load(int3(texel, 0));
  } else {
    WarpedDepth[pixel] = 0.0;
  }

  float2 warped = float2(0.0, 0.0);
  if ((Flags & kFlagMotion) != 0u) {
    const uint2 grid = max(MotionGrid, uint2(1, 1));
    const uint2 texel = MotionBase + min(uint2(saturate(n) * float2(grid)), grid - 1u);
    const float2 mv = GuideMotion.Load(int3(texel, 0)) * MotionScale;
    // Normalized displacement on the full frame, then through the warp.
    // The current position's warp is the texel centre by construction.
    const float2 d = mv / float2(grid);
    const float2 v_now = (float2(pixel) + 0.5) / float2(Size);
    const float2 v_prev = FovWarp(n + d, Foveation, FoveationGmax);
    warped = (v_prev - v_now) * float2(Size);
  }
  WarpedMotion[pixel] = warped;
}
