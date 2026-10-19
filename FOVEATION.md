# Foveated NR working grid

Patch on top of RenoDX-DLSS5-Generic `main` (v7.5.0-rc5). After-upscale path only.

## What it does

DLSSNR is a same-resolution network, so it cannot take a per-pixel resolution.
This mode warps the working image horizontally instead:

- density 1 (relative to `NRResolutionScale`) inside a central plateau,
- a smoothstep fall-off (C1-continuous density, no visible seam),
- `Edge` density at the left and right edges.

NR runs **once** on the warped image, with one temporal history. Depth and
motion vectors are resampled onto the same warped grid by a new compute pass
(`fov_guides`) and handed to NR at MVecScale 1, so its guides stay registered.
Every stage that maps working ↔ full (v6/legacy encode, v6/legacy resolve,
look upsample, look temporal reprojection, transport footprint) goes through
`FovWarp` / `FovUnwarp` in `shaders/foveation.hlsli`. With the mode off, every
mapping is bit-identical to stock.

## Settings (ReShade.ini, `[RenoDX.DLSS5]` section, or the overlay)

| Key | Default | Meaning |
|---|---|---|
| `NRFoveation` | 0 | 1 = on |
| `NRFoveationCenter` | 0.50 | share of the width kept at full density |
| `NRFoveationRamp` | 0.25 | share of the width spent on the fall-off (split across both sides) |
| `NRFoveationEdge` | 0.50 | horizontal density at the edges (0.25–1.0) |

For "centre 100 %, sides 50 %": `NRResolutionScale=1`, `NRFollowInputRes=0`,
`NRFoveation=1`, defaults for the rest.

## Cost at 7680×2160 (NR pixels, from `foveation.hpp`)

| Centre | Ramp | Edge | Working grid | NR pixels |
|---|---|---|---|---|
| 0.50 | 0.25 | 0.50 | 6244×2160 | 81 % (−19 %) |
| 0.50 | 0.00 | 0.50 | 5764×2160 | 75 % (−25 %) |
| 0.33 | 0.25 | 0.50 | 5603×2160 | 73 % (−27 %) |
| 0.33 | 0.33 | 0.33 | 5109×2160 | 67 % (−33 %) |

The vertical axis is never warped, so the sides drop to 50 % horizontally only.
On a 32:9 panel the full-density centre dominates the cost.

## Files

- `foveation.hpp` – host-side packing, density integral, working width
- `shaders/foveation.hlsli` – warp / unwarp (Newton inverse, ≤0.002 px round-trip error in fp32)
- `shaders/fov_guides.cs_5_1.hlsl` – depth + motion resample onto the warped grid
- `dlssnr.hpp` – settings, working width, guide pass, NR parameter override, overlay controls
- `look_stage.hpp`, `shaders/{v6_common,v6_encode,legacy_common,legacy_encode,look_common,look_upsample,look_compose}` – warp-aware mappings

Codec root signature constants went from 24 to 28 dwords (Foveation is dword
24 in every codec view; the look stage uses its former padding dword).

## Status

Not built with the RenoDX toolchain and not run on hardware. Checked:

- every shader compiles (glslang HLSL front end); cbuffer offsets verified by reflection,
- the add-on parses with clang against mingw-w64 / NGX / Streamline / ReShade headers with **zero new errors vs. the unmodified source** (deliberate typos in the new code are caught, so it is type-checked),
- warp math unit-tested; encode → NR edit → resolve simulated: edit placement error 1e-4 (0.43 if the resolve ignored the warp); warped motion matches exact reprojection.

## Known limits

- Pre-SR placement keeps the uniform grid.
- Changing the warp resets NR's temporal history (expect a brief shimmer while dragging a slider); a changed working width rebuilds after the usual ~0.5 s debounce.
- Screenshot planes for `proxy` / `neural` show the warped working image.
- New overlay strings are English only (not in `ui/lang/*`).
- If the guide pass can't run (unreadable depth/MV format, more than 8 distinct depth/MV pairs, allocation failure), the frame still runs with the game's guides and logs once; side edits may smear in that case.
