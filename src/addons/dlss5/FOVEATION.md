# Foveated NR working grid

Patch on top of RenoDX-DLSS5-Generic `main` (v7.5.0-rc5). After-upscale path only.

## What it does

DLSSNR is a same-resolution network, so it cannot take a per-pixel resolution.
This mode warps the working image instead, separately on each axis:

- density 1 (relative to `NRResolutionScale`) inside a centre zone,
- a smoothstep fall-off (C1-continuous density, no seam) to the contour density `NRFoveationEdge`
  (per axis, so corners get Edge × Edge),
- beyond the contour (the midpoint zone), nothing: NR is off and the resolve keeps the game's pixels,
  faded in over a soft blend.

NR runs **once** on the warped image, with one temporal history. Depth and
motion vectors are resampled onto the same warped grid by a compute pass
(`fov_guides`) and handed to NR at MVecScale 1, so its guides stay registered.
Every stage that maps working ↔ full (v6/legacy encode, v6/legacy resolve,
look upsample, look temporal reprojection, transport footprint) goes through
`FovWarp` / `FovUnwarp` in `shaders/foveation.hlsli`. With the mode off, every
mapping is bit-identical to stock.

## Zones in percent

Per axis the screen splits into **centre + contour + edge = 100 %**
(`foveation::Params`, `SizeAxis`):

- the centre is a share of the axis (10–100 %),
- the contour is a *thickness* added around the centre, both sides together, so
  the midpoint zone (centre + contour) can never be smaller than the centre and
  never larger than the screen: the contour's range is what the centre leaves,
  and a growing centre takes the room from the contour,
- the edge is what is left, where NR is off,
- the fall-off and the blend are shares of the contour's per-side thickness
  (the blend has a floor of 2 % of the axis so a thin contour still fades).

`SizeAxis` turns the shares into output pixels for the running output, and
`MakeAxis` applies the remaining safeguards every frame:

- a zone pushed past a screen edge is held flush with it (the midpoint zone is
  shifted, never shrunk) and the position snaps to whole pixels,
- if the result would save under 5 % of the NR pixels, the grid stands down to
  uniform: the guide pass would cost more than it saves,
- non-finite or out-of-range settings fall back to the defaults or are clamped.

The overlay shows a zone map (the screen in its aspect, the zones as they run),
the NR saving forecast (share of a full-screen NR pass, screen split, and the
measured NR time with and without foveation when the GPU timers are on), and
can outline the zones over the game (`NRFoveationPreview`, drawn on ImGui's
background list from ReShade's overlay pass; drawing only).

## Settings (ReShade.ini `[RenoDX.DLSS5]`, or the overlay)

| Key | Default | Meaning |
|---|---|---|
| `NRFoveation` | 0 | 1 = on |
| `NRFoveationCenterW` / `NRFoveationCenterH` | 50 / 50 | centre zone, % of the width / height |
| `NRFoveationContourW` / `NRFoveationContourH` | 25 / 25 | contour thickness, % of the width / height, both sides together |
| `NRFoveationFalloffPct` | 50 | resolution fall-off, % of the contour's per-side thickness |
| `NRFoveationBlendPct` | 25 | NR fade into the edge, % of the contour's per-side thickness |
| `NRFoveationEdge` | 0.50 | contour density, per axis (0.25–1.0) |
| `NRFoveationCenterX` | 50 | zone centre, % of the width (0 left, 100 right) |
| `NRFoveationCenterY` | 50 | zone centre, % of the height (0 top, 100 bottom) |
| `NRFoveationPreview` | 1 | zone outlines over the game: 0 off, 1 while the overlay is open, 2 always |

The pixel keys of v0.1–v0.3 (`NRFoveationWidth`, `NRFoveationMidWidth`, ...)
are no longer read.

Off-centre zones measure distance from the zone's centre c, so each axis has
two sides of different length; the shaders get c in the per-axis float that
used to carry G(1) and derive the normalizer G(2c) + G(2 - 2c) themselves.
c is clamped so the zone stays on screen and snapped to whole pixels; a
moved centre is a new grid (history reset, working size re-derived).

Use with `NRResolutionScale=1`, `NRFollowInputRes=0` for a 100 % centre.

## Default layout

50 % centre + 25 % contour + 25 % edge on both axes, contour at 0.50:
NR runs on **43 %** of the full-screen pixels (2.3×) at every aspect ratio
(3840×2160 → 2520×1418 working grid; 7680×2160 → 5041×1418).

## Performance notes

- One NR evaluate on the smaller grid; no tiles, no extra feature instances.
- Per-axis G(1) is computed on the host and passed as a constant; shaders
  never evaluate it per pixel. Identity axes return before decoding.
- Only the fall-off bands run the 4-step Newton inverse (measured ≤0.003 px
  fp32 error at 7680 px, edge 0.25); plateau and edge are closed form.
- The resolve maps each output pixel once for both P and N.
- Guide pass: one dispatch per evaluate, shared by all stack passes; two point
  loads and 12 bytes written per working texel.
- A warp change resets temporal history instead of recreating NR features; a
  size change rebuilds after the usual ~0.5 s debounce, so slider drags never
  cause a recreate storm.

## Files

- `foveation.hpp` – host packing, clamping, density integral, working size, overlay readout values
- `shaders/foveation.hlsli` – per-axis warp / unwarp
- `shaders/fov_guides.cs_5_1.hlsl` – depth + motion resample onto the warped grid
- `dlssnr.hpp` – settings, working size, guide pass, NR parameter override, overlay controls
- `look_stage.hpp`, `shaders/{v6_common,v6_encode,legacy_common,legacy_encode,look_common,look_upsample,look_compose}` – warp-aware mappings

Codec root constants: 24 → 28 dwords (grid at dwords 24–27). Look root
constants: 32 → 36 dwords (grid at dwords 32–35).

## Status

Pre-release (v0.1.0). Built with the RenoDX toolchain (MSVC, VS 2022 preset)
on GitHub Actions. Tested in The Witcher 3 (DX12, native DLSS + frame
generation) at 7680×2160 on an RTX 4090: foveation active at several centre
sizes and pass counts, NR engaged on 99.8 % of DLSS frames, no errors, no
guide-pass fallback.

Also checked before the first build:

- every shader compiles (glslang HLSL front end); constant offsets verified by reflection,
- warp math and clamping unit-tested at 7680×2160, 5120×2160, 5160×1400, 3840×2160 and 3440×1440; encode → NR edit → resolve simulated per axis (placement error ≤4e-4), warped motion matches exact reprojection.

## Known limits

- Pre-SR placement keeps the uniform grid.
- Changing the warp (or the output resolution) resets NR's temporal history.
- Screenshot planes for `proxy` / `neural` show the warped working image.
- New overlay strings are English only (not in `ui/lang/*`).
- If the guide pass can't run (unreadable depth/MV format, more than 8 distinct depth/MV pairs, allocation failure), the frame runs with the game's guides and logs once.
