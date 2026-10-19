// Feed v2 (NRFeedMode): the NR input scale from the game's own exposure
// instead of from frame content.
//
// The DLSS contract (Programming Guide 3.9 / 3.9.2): the 1x1
// ExposureTexture's first channel is the value "which when multiplied to
// the input color values brings middle gray to an expected level ...
// typically the same value provided to the renderer's tonemapper", with
// DLSS.Exposure.Scale as its correction factor; DLSS.Pre.Exposure is a
// factor the engine pre-multiplied and "later removed (divided out) during
// tonemapping".  The tonemapper's input is therefore
//   color * Exposure * ExposureScale / PreExposure
// (ExposureValue = 0.18 / (AverageLuma * 0.82): mid-grey lands near 0.2).
// Feeding NR that domain puts the game's mid-grey where the game's own
// tonemapper puts it, and it holds still while the game's eye adaptation
// swings: the proxy moves only when the displayed image does.  The divisor
// the encode applies (proxy = color / divisor) is therefore
//   texture present:  divisor = PreExposure / (Exposure * ExposureScale)
//   pre-exposed only: divisor = 1 - the buffer IS the tonemapper input
//                     already; dividing by PreExposure would undo the
//                     game's adaptation (REVIEW_SH2_FLICKER_V630 section 1)
// v1 (v6_autoscale) derives the divisor from a 64x36 content histogram
// under its own attack/release clock; that second clock, racing the game's
// adaptation, is the slow pulsing v2 exists to remove.  There is no
// estimator and no rate limit here: the game's exposure is the signal.
// The read is legal because 3.4 puts every DLSS input, the exposure texture
// included, in NON_PIXEL_SHADER_RESOURCE for the evaluate, and this dispatch
// is recorded inside that window on the same list.
//
// Source (CPU-chosen, see FrameFeed; the FeedSource values): 1 = read the
// exposure texel at t0, 2 = fixed divisor 1, 3 = hold (the exposure view
// ring is full this frame).  A texel that is non-finite or outside
// 2^-24..2^24 - an unwritten texture, a zero, a garbage convention - holds
// the previous committed divisor, or 1 when there is none (SnapNow, or an
// uninitialized texel).
// The only cross-frame state is that held value in Commit (u1), the same
// 1x1 UAV the v1 governor uses, read-then-written by one thread.
//
// Output (u0, norm_scale) .r is the divisor the encode and resolve read
// (t3 of the pass sets) and the dark gate's unit (v6_commit_exposure);
// .g the divisor this frame's reading implied (0 = none), .b the raw
// exposure texel (-1 = non-finite), .a the state for NRNormTrace: the
// Source for a fresh value (1 texture, 2 fixed), else 3 held, 4 defaulted
// to 1.

Texture2D<float4> Exposure : register(t0);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float4> Commit : register(u1);

// Its own view of the codec root constants (dwords 0..3); the host writes
// them after BindCodecV6, see PrepareFrameScale.
cbuffer FeedConstants : register(b0) {
  uint Source;
  float PreExposure;
  float ExposureScale;
  float SnapNow;
};

static const float kMinDivisor = 5.96046448e-8;  // 2^-24
static const float kMaxDivisor = 16777216.0;     // 2^24

bool Finite(float value) {
  return (asuint(value) & 0x7F800000u) != 0x7F800000u;
}

[numthreads(1, 1, 1)]
void main() {
  float exposure = 0.0;
  float reading = Source == 2u ? 1.0 : 0.0;
  if (Source == 1u) {
    exposure = Exposure.Load(int3(0, 0, 0)).r;
    const float divisor = PreExposure / (exposure * ExposureScale);
    // Finite first: the range compares are only meaningful on a number.
    if (Finite(divisor) && divisor >= kMinDivisor && divisor <= kMaxDivisor) {
      reading = divisor;
    }
    if (!Finite(exposure)) exposure = -1.0;
  }
  const float previous = Commit[uint2(0, 0)].r;
  const bool can_hold =
      SnapNow == 0.0 && Finite(previous) && previous >= kMinDivisor
      && previous <= kMaxDivisor;
  float committed = reading;
  float state = float(Source);
  if (!(reading > 0.0)) {
    committed = can_hold ? previous : 1.0;
    state = can_hold ? 3.0 : 4.0;
  }
  Commit[uint2(0, 0)] = float4(committed, 0.0, 0.0, 1.0);
  Output[uint2(0, 0)] = float4(committed, reading, exposure, state);
}
