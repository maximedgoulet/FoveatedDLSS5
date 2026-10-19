// Foveated NR working grid - host side.  Mirrors shaders/foveation.hlsli:
// the packed per-axis dword, the density integral G and the working-size
// factor.  Change the two together.
//
// The NR model is same-resolution, so "foveated" means a separable warp of
// the working image: per axis, density 1 inside the centre zone, a smoothstep
// fall-off over `falloff` pixels, and `edge` beyond it.  Centre W x H and the
// fall-off are given in OUTPUT pixels and clamped against the actual output
// each frame, so one setting works at any resolution: an axis whose centre
// covers the whole screen is simply not warped.  Corners get edge x edge.
//
// One NR evaluate, one temporal history, no tile seams.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace foveation {

inline constexpr uint32_t kEnabledBit = 0x80000000u;

// Field layout of one axis dword (foveation.hlsli FovDecode):
//   bits  0..11  a = centre half-extent / half-extent of the screen, code/4095
//   bits 12..23  b = fall-off length / half-extent, code/2048 (0..2)
//   bits 24..30  e = edge density, code/128 (32..127 -> 0.25..0.99)
//   bit  31      axis enabled
inline constexpr uint32_t kCenterMax = 4095u;
inline constexpr double kCenterScale = 4095.0;
inline constexpr uint32_t kRampMax = 4095u;
inline constexpr double kRampScale = 2048.0;
inline constexpr uint32_t kEdgeMin = 32u;
inline constexpr uint32_t kEdgeMax = 127u;
inline constexpr double kEdgeScale = 128.0;

inline constexpr float kMinEdge = 0.25f;
inline constexpr float kMaxEdge = 1.f;

// Below this saving the guide resample and the warped mappings cost more
// than the NR pixels they remove: the grid stands down to uniform.
inline constexpr double kMinSaving = 0.05;

// Player settings, in output pixels.
struct Params {
  uint32_t center_width = 3000;
  uint32_t center_height = 1800;
  uint32_t falloff = 400;  // the same length on all four sides
  float edge = 0.5f;       // density beyond the fall-off, per axis
};

// The exact values the shaders decode from one axis dword.
struct Decoded {
  double a = 1.0;
  double b = 0.0;
  double e = 1.0;
  double k = 0.0;
};

// Integral of the density over [0, tau], tau = |2n - 1|.  The fall-off may
// run past the screen edge (tau = 1): then the edge value is never reached
// and the ramp is truncated, keeping its physical length at every resolution.
inline double G(double tau, const Decoded& p) {
  if (tau <= p.a) return tau;
  const double ramp_end = p.a + p.b;
  const double ramp_area = p.b * (1.0 - 0.5 * p.k);
  if (tau >= ramp_end) return p.a + ramp_area + p.e * (tau - ramp_end);
  const double q = (tau - p.a) / p.b;
  const double q2 = q * q;
  return p.a + p.b * (q - p.k * (q2 * q - 0.5 * q2 * q2));
}

inline Decoded Decode(uint32_t packed) {
  Decoded p;
  if ((packed & kEnabledBit) == 0u) return p;
  p.a = static_cast<double>(packed & 0xFFFu) / kCenterScale;
  p.b = static_cast<double>((packed >> 12) & 0xFFFu) / kRampScale;
  p.e = static_cast<double>((packed >> 24) & 0x7Fu) / kEdgeScale;
  p.k = 1.0 - p.e;
  return p;
}

// One axis: the packed dword (0 = identity) and the mean density G(1), which
// is both the working-size factor and the shader's normalizer.
struct Axis {
  uint32_t packed = 0;
  float gmax = 1.f;
  // Effective values at this output, for the overlay and the log.
  uint32_t center_px = 0;   // clamped centre extent
  uint32_t falloff_px = 0;  // fall-off actually on screen per side
  float edge_reached = 1.f; // density at the screen edge
};

inline Axis MakeAxis(uint32_t full, uint32_t center, uint32_t falloff, float edge) {
  Axis axis;
  axis.center_px = std::min(center, full);
  if (full < 16u || axis.center_px >= full) return axis;
  const uint32_t e_code = static_cast<uint32_t>(
      std::clamp<long>(std::lround(std::clamp(edge, kMinEdge, kMaxEdge) * kEdgeScale),
                       static_cast<long>(kEdgeMin), static_cast<long>(kEdgeMax) + 1));
  if (e_code > kEdgeMax) return axis;  // edge 1.0: no warp
  const double half = 0.5 * static_cast<double>(full);
  const uint32_t a_code = static_cast<uint32_t>(std::clamp<long>(
      std::lround(0.5 * axis.center_px / half * kCenterScale), 0, kCenterMax));
  // Fall-off longer than the screen is pointless: cap at the full extent.
  const double ramp_px = std::min<double>(falloff, full);
  const uint32_t b_code = static_cast<uint32_t>(
      std::clamp<long>(std::lround(ramp_px / half * kRampScale), 0, kRampMax));
  axis.packed = kEnabledBit | a_code | (b_code << 12) | (e_code << 24);
  const Decoded p = Decode(axis.packed);
  axis.gmax = static_cast<float>(G(1.0, p));
  const double on_screen = std::clamp(1.0 - p.a, 0.0, p.b);
  axis.falloff_px = static_cast<uint32_t>(std::lround(on_screen * half));
  if (p.b <= 0.0 || 1.0 >= p.a + p.b) {
    axis.edge_reached = static_cast<float>(p.e);
  } else {
    const double q = (1.0 - p.a) / p.b;
    axis.edge_reached = static_cast<float>(1.0 - p.k * q * q * (3.0 - 2.0 * q));
  }
  return axis;
}

struct Grid {
  Axis x;
  Axis y;
  bool active() const { return x.packed != 0u || y.packed != 0u; }
  // NR pixels over the uniform grid's.
  double PixelFactor() const { return static_cast<double>(x.gmax) * y.gmax; }
  bool operator==(const Grid& o) const {
    return x.packed == o.x.packed && y.packed == o.y.packed;
  }
  bool operator!=(const Grid& o) const { return !(*this == o); }
};

enum class Inactive : uint8_t { kNone, kOff, kCentreCoversScreen, kSavingTooSmall };

// The grid for an output of full_width x full_height.  `why` says why it is
// uniform when it is.
inline Grid MakeGrid(bool enabled, const Params& params, uint32_t full_width,
                     uint32_t full_height, Inactive* why = nullptr) {
  Grid grid;
  Inactive reason = Inactive::kNone;
  if (!enabled) {
    reason = Inactive::kOff;
  } else {
    grid.x = MakeAxis(full_width, params.center_width, params.falloff, params.edge);
    grid.y = MakeAxis(full_height, params.center_height, params.falloff, params.edge);
    if (!grid.active()) {
      reason = Inactive::kCentreCoversScreen;
    } else if (grid.PixelFactor() > 1.0 - kMinSaving) {
      reason = Inactive::kSavingTooSmall;
    }
    if (reason != Inactive::kNone) {
      // Keep the effective numbers for the overlay, drop the warp.
      grid.x.packed = 0u;
      grid.x.gmax = 1.f;
      grid.y.packed = 0u;
      grid.y.gmax = 1.f;
    }
  }
  if (why != nullptr) *why = reason;
  return grid;
}

// Working extent on one axis: the uniform extent times the mean density, so
// the centre keeps the uniform density exactly.
inline uint32_t WarpedExtent(uint32_t uniform, const Axis& axis) {
  if (axis.packed == 0u) return uniform;
  return std::max<uint32_t>(
      16u, static_cast<uint32_t>(std::lround(static_cast<double>(uniform) * axis.gmax)));
}

// Normalized full position -> normalized working position on one axis
// (tests and diagnostics; the shaders carry their own copy).
inline double Warp(double n, const Axis& axis) {
  if (axis.packed == 0u) return n;
  const Decoded p = Decode(axis.packed);
  const double t = 2.0 * n - 1.0;
  const double s = t < 0.0 ? -1.0 : (t > 0.0 ? 1.0 : 0.0);
  return 0.5 + 0.5 * s * G(std::abs(t), p) / axis.gmax;
}

}  // namespace foveation
