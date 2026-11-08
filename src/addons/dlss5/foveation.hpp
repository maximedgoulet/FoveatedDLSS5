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
// The centre zone can sit anywhere on each axis (Params::center_x/y, e.g.
// lower for a racing game's dashboard).  Distances are measured from that
// centre c, so the two sides of an axis have different lengths (2c and
// 2 - 2c in half-screen units); the working position is
//   v(n) = (G(2c) + sign(n - c) * G(2|n - c|)) / (G(2c) + G(2 - 2c)),
// which is the symmetric warp exactly when c = 0.5.  c is clamped so the
// centre zone never leaves the screen and snapped to whole output pixels.
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
  // Where the centre zone sits, 0..1 across the screen (0 = left / top,
  // 0.5 = middle, 1 = right / bottom).  Clamped per frame so the zone stays
  // on screen.
  float center_x = 0.5f;
  float center_y = 0.5f;
};

// The exact values the shaders decode from one axis dword.
struct Decoded {
  double a = 1.0;
  double b = 0.0;
  double e = 1.0;
  double k = 0.0;
};

// Integral of the density over [0, tau], tau = 2|n - c| (distance from the
// zone's centre in half-screen units; up to 2 when the centre is off the
// middle).  The fall-off may run past the screen edge: then the edge value is
// never reached and the ramp is truncated, keeping its physical length at
// every resolution.
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

// G(2c) + G(2 - 2c): the density integral over the whole axis, in
// half-screen units.  Half of it is the mean density.
inline double GTotal(double c, const Decoded& p) {
  return G(2.0 * c, p) + G(2.0 - 2.0 * c, p);
}

// One axis: the packed dword (0 = identity), the zone's centre c (the float
// the shaders get beside the dword; they derive their normalizer from it)
// and the mean density gmax, which sizes the working image.
struct Axis {
  uint32_t packed = 0;
  float center = 0.5f;
  float gmax = 1.f;
  // Effective values at this output, for the overlay and the log.
  uint32_t center_px = 0;    // clamped centre extent
  uint32_t position_px = 0;  // where the zone's middle actually sits
  uint32_t falloff_px = 0;   // fall-off on screen, on the longer side
  float edge_reached = 1.f;  // density at the farther screen edge
};

inline Axis MakeAxis(uint32_t full, uint32_t center, uint32_t falloff, float edge,
                     float position = 0.5f) {
  Axis axis;
  axis.center_px = std::min(center, full);
  axis.position_px = full / 2u;
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
  // Safeguards on the position: non-finite means the middle; snap to whole
  // output pixels (sub-pixel slider jitter must not rebuild the grid and
  // reset NR's history); then keep the decoded zone [c - a/2, c + a/2] on
  // screen, which also keeps both sides of the axis non-empty.
  const double wanted =
      std::isfinite(position) ? std::clamp<double>(position, 0.0, 1.0) : 0.5;
  const double snapped = std::round(wanted * full) / full;
  axis.center = static_cast<float>(std::clamp(snapped, 0.5 * p.a, 1.0 - 0.5 * p.a));
  const double c = axis.center;  // exactly what the shaders get
  axis.position_px = static_cast<uint32_t>(std::lround(c * full));
  axis.gmax = static_cast<float>(0.5 * GTotal(c, p));
  // Overlay numbers for the longer side (the one that reaches lowest).
  const double side = 2.0 * std::max(c, 1.0 - c);
  const double on_screen = std::clamp(side - p.a, 0.0, p.b);
  axis.falloff_px = static_cast<uint32_t>(std::lround(on_screen * half));
  if (p.b <= 0.0 || side >= p.a + p.b) {
    axis.edge_reached = static_cast<float>(p.e);
  } else {
    const double q = (side - p.a) / p.b;
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
  // The centres are part of the mapping: a moved zone is a new grid (new
  // working size, reset NR history), never a stale warp.
  bool operator==(const Grid& o) const {
    return x.packed == o.x.packed && y.packed == o.y.packed
        && x.center == o.x.center && y.center == o.y.center;
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
    grid.x = MakeAxis(full_width, params.center_width, params.falloff, params.edge,
                      params.center_x);
    grid.y = MakeAxis(full_height, params.center_height, params.falloff, params.edge,
                      params.center_y);
    if (!grid.active()) {
      reason = Inactive::kCentreCoversScreen;
    } else if (grid.PixelFactor() > 1.0 - kMinSaving) {
      reason = Inactive::kSavingTooSmall;
    }
    if (reason != Inactive::kNone) {
      // Keep the effective numbers for the overlay, drop the warp.
      grid.x.packed = 0u;
      grid.x.center = 0.5f;
      grid.x.gmax = 1.f;
      grid.y.packed = 0u;
      grid.y.center = 0.5f;
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
  const double c = axis.center;
  const double t = n - c;
  const double s = t < 0.0 ? -1.0 : (t > 0.0 ? 1.0 : 0.0);
  return (G(2.0 * c, p) + s * G(2.0 * std::abs(t), p)) / GTotal(c, p);
}

}  // namespace foveation
