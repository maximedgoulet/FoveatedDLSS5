# Foveated NR working grid

Patch on top of RenoDX-DLSS5-Generic `main` (v7.5.0-rc5). After-upscale path only.

## What it does

DLSSNR is a same-resolution network, so it cannot take a per-pixel resolution.
This mode warps the working image instead, separately on each axis:

- density 1 (relative to `NRResolutionScale`) inside a centre zone of W × H **output pixels**,
- a smoothstep fall-off of `Fall-off` pixels on every side (C1-continuous density, no seam),
- `Edge` density beyond it (per axis, so corners get Edge × Edge).

NR runs **once** on the warped image, with one temporal history. Depth and
motion vectors are resampled onto the same warped grid by a compute pass
(`fov_guides`) and handed to NR at MVecScale 1, so its guides stay registered.
Every stage that maps working ↔ full (v6/legacy encode, v6/legacy resolve,
look upsample, look temporal reprojection, transport footprint) goes through
`FovWarp` / `FovUnwarp` in `shaders/foveation.hlsli`. With the mode off, every
mapping is bit-identical to stock.

## Resolution handling

Sizes are output pixels and are clamped against the running output every frame:

- a centre wider/taller than the screen disables that axis (only the other axis is foveated),
- a fall-off longer than the space to the edge keeps its length and is cut by the
  screen edge (the edge value is then not reached; the overlay shows what is),
- if the result would save under 5 % of the NR pixels, the grid stands down to
  uniform: the guide pass would cost more than it saves,
- stored values are never rewritten by a smaller display; switching back restores them.

The overlay sliders range up to the running output, and a readout shows the
effective centre, fall-off, edge density and NR-pixel share at that output.

## Settings (ReShade.ini `[RenoDX.DLSS5]`, or the overlay)

| Key | Default | Meaning |
|---|---|---|
| `NRFoveation` | 0 | 1 = on |
| `NRFoveationWidth` | 3000 | centre zone width, output px |
| `NRFoveationHeight` | 1800 | centre zone height, output px |
| `NRFoveationFalloff` | 400 | fall-off length on every side, output px |
| `NRFoveationEdge` | 0.50 | density beyond the fall-off, per axis (0.25–1.0) |

Use with `NRResolutionScale=1`, `NRFollowInputRes=0` for a 100 % centre.

## Defaults across displays (from `foveation.hpp`)

| Output | Effective centre | Fall-off on screen (H/V) | Edge reached (H/V) | NR grid | NR pixels |
|---|---|---|---|---|---|
| 7680×2160 | 3000×1800 | 400/180 px | 0.50/0.79 | 5540×2132 | 71 % |
| 5120×2160 | 3000×1800 | 400/180 px | 0.50/0.79 | 4260×2132 | 82 % |
| 5160×1400 | 3000×1400 | 400/0 px | 0.50/— | 4280×1400 | 83 % |
| 3840×2160 | 3000×1800 | 400/180 px | 0.50/0.79 | 3620×2132 | 93 % |
| 3440×1440 | 3000×1440 | 220/0 px | 0.71/— | — | bypassed (saves <5 %) |

A 3000×1800 centre is most of a 4K or 3440×1440 screen; scale it down there
(2000×1100 gives 69 % NR pixels at 4K, 83 % at 3440×1440).

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
