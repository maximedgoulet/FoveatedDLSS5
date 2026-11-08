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
// Dual foveation (Params::mid_width/height): beyond a midpoint zone NR is
// off entirely - the working image covers only the zone, and the resolve
// keeps the game's pixels outside it.  Distances then run from c to the
// zone's bounds [m0, m1] instead of the screen edges (foveation.hlsli).
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
#include <initializer_list>

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
  float edge = 0.5f;       // midpoint density beyond the fall-off, per axis
  // Where the centre zone sits, 0..1 across the screen (0 = left / top,
  // 0.5 = middle, 1 = right / bottom).  Clamped per frame so the zone stays
  // on screen.
  float center_x = 0.5f;
  float center_y = 0.5f;
  // Dual foveation: the midpoint zone, centred on the same position.  NR is
  // off beyond it (the game's pixels, faded in over `blend`).  At or above
  // the screen size nothing is off: the single-zone foveation of v0.2.
  uint32_t mid_width = 16384;
  uint32_t mid_height = 16384;
  uint32_t blend = 96;
  // Outer limits for the midpoint zone, 0..1 per axis (auto-trim); the whole
  // screen when unset.
  float limit_x0 = 0.f;
  float limit_x1 = 1.f;
  float limit_y0 = 0.f;
  float limit_y1 = 1.f;
};

// A trimmed axis never shrinks below this share of the screen: a limit pair
// closer than that is ignored (whole screen).
inline constexpr double kMinZoneExtent = 0.1;
inline constexpr double kBoundsScale = 65535.0;

// The exact values the shaders decode from one axis dword.
struct Decoded {
  double a = 1.0;
  double b = 0.0;
  double e = 1.0;
  double k = 0.0;
};

// Integral of the density over [0, tau], tau = 2|n - c| (distance from the
// zone's centre in half-screen units).  The fall-off may run past the zone's
// edge: then the midpoint value is never reached and the ramp is truncated,
// keeping its physical length at every resolution.
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

// [m0, m1] from the packed bounds the shaders get (0 = the whole axis).
inline void DecodeBounds(uint32_t bounds, double* m0, double* m1) {
  if (bounds == 0u) {
    *m0 = 0.0;
    *m1 = 1.0;
    return;
  }
  *m0 = static_cast<double>(bounds & 0xFFFFu) / kBoundsScale;
  *m1 = static_cast<double>(bounds >> 16) / kBoundsScale;
}

// G(2(c - m0)) + G(2(m1 - c)): the density integral over the working part of
// the axis, in half-screen units.  Half of it is the mean density.
inline double GTotal(double c, double m0, double m1, const Decoded& p) {
  return G(2.0 * (c - m0), p) + G(2.0 * (m1 - c), p);
}

// One axis: the packed dword (0 = identity), the zone's centre c, the
// midpoint bounds and the blend width (the values the shaders get beside the
// dword), and the mean density gmax, which sizes the working image.
struct Axis {
  uint32_t packed = 0;
  float center = 0.5f;
  uint32_t bounds = 0;  // m0 | m1 << 16, /65535; 0 = whole axis (nothing off)
  float blend = 0.f;    // NR-off fade width, normalized
  float gmax = 1.f;
  // Effective values at this output, for the overlay and the log.
  uint32_t center_px = 0;    // clamped centre extent
  uint32_t position_px = 0;  // where the zone's middle actually sits
  uint32_t mid_lo_px = 0;    // midpoint zone [lo, hi) in output pixels
  uint32_t mid_hi_px = 0;
  uint32_t falloff_px = 0;   // fall-off inside the zone, on the longer side
  float edge_reached = 1.f;  // density at the farther end of the zone
};

inline Axis MakeAxis(uint32_t full, uint32_t center, uint32_t falloff, float edge,
                     float position = 0.5f, uint32_t mid = 0xFFFFFFFFu,
                     uint32_t blend = 0u, float limit_lo = 0.f, float limit_hi = 1.f) {
  Axis axis;
  axis.position_px = full / 2u;
  axis.mid_hi_px = full;
  // Safeguards on the limits (auto-trim): finite, ordered, and never closer
  // than kMinZoneExtent - otherwise the whole axis.
  double l0 = std::isfinite(limit_lo) ? std::clamp<double>(limit_lo, 0.0, 1.0) : 0.0;
  double l1 = std::isfinite(limit_hi) ? std::clamp<double>(limit_hi, 0.0, 1.0) : 1.0;
  if (l1 - l0 < kMinZoneExtent) {
    l0 = 0.0;
    l1 = 1.0;
  }
  const uint32_t avail_px = static_cast<uint32_t>(std::floor((l1 - l0) * full));
  axis.center_px = std::min({center, full, avail_px});
  // The midpoint zone is at least the centre zone and kMinZoneExtent of the
  // screen (an empty zone would be an empty working axis, and its bounds
  // would encode as 0 = the whole axis), and at most what is left.
  const uint32_t min_mid_px = std::min(
      static_cast<uint32_t>(std::ceil(kMinZoneExtent * full)), avail_px);
  const uint32_t mid_px = std::clamp(
      std::min(mid, full), std::max(axis.center_px, min_mid_px), avail_px);
  bool cropped = mid_px < full;
  if (full < 16u || (!cropped && axis.center_px >= full)) return axis;
  uint32_t e_code = static_cast<uint32_t>(
      std::clamp<long>(std::lround(std::clamp(edge, kMinEdge, kMaxEdge) * kEdgeScale),
                       static_cast<long>(kEdgeMin), static_cast<long>(kEdgeMax) + 1));
  if (e_code > kEdgeMax) {
    if (!cropped) return axis;  // midpoint 1.0 and nothing off: no warp
    e_code = kEdgeMax;          // 0.99: the crop still needs the mapping
  }
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
  // reset NR's history); then keep the decoded centre zone [c - a/2, c + a/2]
  // inside the limits, which also keeps both sides of the axis non-empty.
  const double wanted =
      std::isfinite(position) ? std::clamp<double>(position, 0.0, 1.0) : 0.5;
  const double snapped = std::round(wanted * full) / full;
  const double c_lo = l0 + 0.5 * p.a;
  const double c_hi = l1 - 0.5 * p.a;
  axis.center =
      static_cast<float>(c_lo <= c_hi ? std::clamp(snapped, c_lo, c_hi) : 0.5 * (l0 + l1));
  const double c = axis.center;  // exactly what the shaders get
  axis.position_px = static_cast<uint32_t>(std::lround(c * full));
  // Midpoint zone: centred on c, shifted (never shrunk) to stay inside the
  // limits.  It contains the centre zone because it is at least as large and
  // both are inside the same limits.  Codes round outward.
  double m0 = 0.0;
  double m1 = 1.0;
  if (cropped) {
    // At least the DECODED centre extent: a's 12-bit code can round a hair
    // above a same-sized midpoint zone.
    const double extent = std::max(static_cast<double>(mid_px) / full, p.a);
    double lo = c - 0.5 * extent;
    double hi = c + 0.5 * extent;
    if (lo < l0) {
      hi += l0 - lo;
      lo = l0;
    }
    if (hi > l1) {
      lo -= hi - l1;
      hi = l1;
    }
    const uint32_t lo_code = static_cast<uint32_t>(
        std::clamp(std::floor(std::max(lo, 0.0) * kBoundsScale), 0.0, kBoundsScale));
    const uint32_t hi_code = static_cast<uint32_t>(
        std::clamp(std::ceil(std::min(hi, 1.0) * kBoundsScale), 0.0, kBoundsScale));
    if (lo_code == 0u && hi_code == 0xFFFFu) {
      cropped = false;  // rounds to the whole axis
    } else {
      axis.bounds = lo_code | (hi_code << 16);
      DecodeBounds(axis.bounds, &m0, &m1);
    }
  }
  axis.mid_lo_px = static_cast<uint32_t>(std::lround(m0 * full));
  axis.mid_hi_px = static_cast<uint32_t>(std::lround(m1 * full));
  axis.blend = static_cast<float>(std::min<double>(blend, full) / full);
  axis.gmax = static_cast<float>(0.5 * GTotal(c, m0, m1, p));
  // Overlay numbers for the longer side (the one that reaches lowest).
  const double side = 2.0 * std::max(c - m0, m1 - c);
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
  // Centre and bounds are part of the mapping: a moved or resized zone is a
  // new grid (new working size, reset NR history), never a stale warp.  The
  // blend width only weights the resolve, so it is not compared.
  bool operator==(const Grid& o) const {
    return x.packed == o.x.packed && y.packed == o.y.packed
        && x.center == o.x.center && y.center == o.y.center
        && x.bounds == o.x.bounds && y.bounds == o.y.bounds;
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
                      params.center_x, params.mid_width, params.blend, params.limit_x0,
                      params.limit_x1);
    grid.y = MakeAxis(full_height, params.center_height, params.falloff, params.edge,
                      params.center_y, params.mid_height, params.blend, params.limit_y0,
                      params.limit_y1);
    if (!grid.active()) {
      reason = Inactive::kCentreCoversScreen;
    } else if (grid.PixelFactor() > 1.0 - kMinSaving) {
      reason = Inactive::kSavingTooSmall;
    }
    if (reason != Inactive::kNone) {
      // Keep the effective numbers for the overlay, drop the warp.
      for (Axis* axis : {&grid.x, &grid.y}) {
        axis->packed = 0u;
        axis->center = 0.5f;
        axis->bounds = 0u;
        axis->gmax = 1.f;
      }
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
  double m0 = 0.0;
  double m1 = 1.0;
  DecodeBounds(axis.bounds, &m0, &m1);
  const double c = axis.center;
  const double t = n - c;
  const double s = t < 0.0 ? -1.0 : (t > 0.0 ? 1.0 : 0.0);
  return (G(2.0 * (c - m0), p) + s * G(2.0 * std::abs(t), p)) / GTotal(c, m0, m1, p);
}

}  // namespace foveation
