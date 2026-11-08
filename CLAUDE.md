# FoveatedDLSS5: notes for AI coding sessions

This repo is a copy of upstream RenoDX carrying one product: the **foveated DLSS 5
Neural Rendering add-on** (`renodx-dlss5.addon64`) in `src/addons/dlss5`. Everything
else in the tree is upstream and untouched. Read `AGENTS.md` (upstream rules) and
`docs/CXX_STYLE.md` before editing C++; this file adds what is specific to the fork.

## Where things are

| What | Where |
|---|---|
| Add-on (header-only, `dlssnr.hpp` is ~19k lines) | `src/addons/dlss5/` |
| Zone math, host side (`Params`, `MakeAxis`, `MakeGrid`, `Warp`, `SizeAxis`) | `src/addons/dlss5/foveation.hpp` |
| Zone math, shader side (`FovDecode`, `FovWarp`, `FovZoneWeight`) | `src/addons/dlss5/shaders/foveation.hlsli` |
| Settings, overlay panel, zone map, on-screen zone preview | `src/addons/dlss5/dlssnr.hpp` (search `kSetFoveation`; `DrawZoneMap`, `OnZonePreviewOverlay`) |
| Technical doc (math, keys, safeguards) | `src/addons/dlss5/FOVEATION.md` |
| Player-facing doc, presets, savings tables | `README.md` |
| The only CI workflow | `.github/workflows/build-dlss5.yml` |
| Release notes, one file per tag | `.github/release-notes/vX.Y.Z.md` |
| Comparison site (before/after slider), https://maximedgoulet.github.io/FoveatedDLSS5/ | branch `gh-pages`: `comparisons.js` lists pairs, lossless WebP in `images/` (no LFS there) |
| README screenshots | `docs/images/*.jpg` (Git LFS via `.gitattributes`; GitHub still renders them) |

`foveation.hpp` and `foveation.hlsli` mirror each other (packed per-axis dword, density
integral G, bounds). Change them together. Shader cbuffers carry
`Foveation / FoveationCenter / FoveationBounds / FoveationBlend` in `v6_common.hlsli`,
`legacy_common.hlsli`, `look_common.hlsli` and `fov_guides.cs_5_1.hlsl`; root-constant
sizes are static_asserted in `dlssnr.hpp` and `look_stage.hpp`.

## Building and checking

- **No full local build on this machine**: the submodules (`external/reshade`,
  `Streamline`, `DLSS`, ...) are not checked out. CI builds the add-on.
- **Zone math alone** compiles with the WinLibs g++ on PATH. Write a small harness
  that includes `foveation.hpp`, compile it with
  `g++ -std=c++20 -I src/addons/dlss5 -include cmath -include utility test.cpp`, and
  sweep the settings (centre inside the midpoint zone, zone on screen, no NaN).
- **Shader syntax check**: `bin/dxc.exe -T cs_6_0 -HV 2016 <file>` (from
  `scripts/setup-dev-env.ps1`). Release builds use FXC with `cs_5_1`, which folds
  `x == x` to true: test NaN by bit pattern.
- **ReShade headers** for the ImGui function table: fetch
  `include/reshade_overlay.hpp` from `crosire/reshade` at the submodule commit
  (`git ls-tree HEAD external/reshade`). Only functions in that table are callable.
- Windows headers define `small` as a macro: never name a variable `small`.

## CI and releases

- `build-dlss5.yml` has two jobs: `build` (MSVC, uploads the `renodx-dlss5-foveated`
  artifact) and `clang` (latest Clang + lld-link, a check only; it caught an lld-link
  crash MSVC did not).
- Pushes to `main` trigger it, except Markdown/docs-only pushes. **Pushes to other
  branches do not**: dispatch the workflow through the API with `{"ref": "<branch>"}`.
- **Release**: add `.github/release-notes/vX.Y.Z.md`, push, then dispatch with
  `{"ref": "main", "inputs": {"release": "vX.Y.Z"}}`. The workflow tags the built
  commit and publishes the add-on and its `.sha256`; 0.x versions are pre-releases.
- No `gh` CLI. Git Credential Manager is signed in; for API calls take the token from
  `printf "protocol=https\nhost=github.com\n\n" | git credential fill` and never
  print it.
- Upstream workflows (nightly, snapshot, auto-merge) were removed on purpose. Do not
  bring them back.

## Working conventions

- Files use CRLF in the working copy (`core.autocrlf=true`, LF in the repo). Write
  with CRLF from scripts. Git Bash `sed -i` rewrites line endings, so check
  `git diff --stat` for files you didn't mean to touch.
- Multi-line patches: write a Python script to a scratch folder and run it. Inline
  heredocs with quotes and backticks break in Git Bash.
- Features go on a `feat/*` branch, get a green CI run, and are merged to `main` once
  the owner has tested them in a game. Don't merge untested features without asking.
- The add-on's savings figures are NR pixel shares from `MakeGrid().PixelFactor()`,
  not measured frame times. Say so wherever they appear.

## Design rules agreed with the owner

- **Idiot-proof controls**: settings must be unable to contradict each other by
  construction, not through hidden clamps. Zones are percentages per axis,
  centre + contour + edge = 100; the contour is a thickness around the centre, and
  its slider's range is what the centre leaves. What a slider shows is what runs.
- **Show, don't tell**: the panel has a zone map and an NR saving forecast, and the
  zones can be outlined over the game (`NRFoveationPreview`).
- **New features default off** and must not change the image when off.
- The owner plays racing games (Forza Horizon, BeamNG, rally) on 7680x2160: test
  ideas against a cockpit view (roof liner and sky on top, dash at the bottom).

## Roadmap state (update when it changes)

- `main`: v0.4.0 released (percent zones, zone map, NR saving forecast, on-screen
  zone preview). v0.3.0 had pixel-based settings; those keys are no longer read.
  Measured 2026-11-08 (2 NR passes, 7680x2160): Forza Horizon 15 -> 25 fps (v0.4),
  The Witcher 3 49 -> 81 fps (v0.1). README section "Measured in games".
- `feat/half-rate`: half-rate NR (NR every other frame, reprojected through motion
  vectors, disarmed for a debounce of frames when motion is fast). CI green, not
  tested in game. Will need a rebase on the percent-zones settings.
- Not started: auto-trim (sky by depth with a bottom-up horizon scan, plus too-dark and
  too-bright bands turned off; optional, default off, at most ~10 % of NR's cost; reuse
  `half_rate.hpp`'s readback ring and `Params::limit_*`).
