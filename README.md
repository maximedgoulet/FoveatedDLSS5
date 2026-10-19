# FoveatedDLSS5

A ReShade add-on that runs NVIDIA DLSS 5 Neural Rendering (NR) at **full resolution in the centre of the screen and lower resolution toward the edges**, to cut NR's GPU cost on ultrawide and super-ultrawide displays.

It is the open-source RenoDX DLSS 5 add-on (`renodx-dlss5`, RenoDX-DLSS5-Generic v7.5.0-rc5) with a foveated working grid added. Everything else in the add-on works as before. This repository is a fork of [RenoDX](https://github.com/clshortfuse/renodx), which provides the build system; the add-on lives in [`src/addons/dlss5`](src/addons/dlss5).

> Status: pre-release. Tested in The Witcher 3 (DX12, 7680×2160, RTX 4090); other games not yet.

## How it works

DLSS 5 NR only runs at a single, fixed resolution per evaluation. So instead of mixing resolutions, the add-on **squeezes** the frame before NR and **stretches** NR's result back:

```
 output 7680×2160                      NR working image 5540×2132
┌──────┬────┬──────────┬────┬──────┐   ┌───┬───┬──────────┬───┬───┐
│ 50 % │ramp│  100 %   │ramp│ 50 % │ → │   │   │  centre  │   │   │
│      │    │ 3000×1800│    │      │   │   │   │ 1:1 copy │   │   │
└──────┴────┴──────────┴────┴──────┘   └───┴───┴──────────┴───┴───┘
```

- Inside the **centre zone**, pixels are kept 1:1.
- Over the **fall-off**, resolution blends smoothly (smoothstep, no visible seam) down to the **edge** value.
- Beyond the fall-off, resolution stays at the edge value. Horizontal and vertical are independent, so corners get edge × edge (0.5 × 0.5 = 25 % of the pixels).
- NR runs **once** on the smaller image, with one temporal history.
- The game's depth and motion vectors are squeezed the same way, so NR's inputs stay aligned with the image.

With foveation off, the add-on behaves exactly like the stock one.

## Install

1. Download `renodx-dlss5.addon64` from the latest [release](../../releases). For an unreleased build, open [Actions → Build DLSS5 add-on](../../actions/workflows/build-dlss5.yml) and download the artifact of a green run.
2. In the game folder (next to the game's `.exe`):

| File | Action |
|---|---|
| ReShade with add-on support | keep |
| `renodx-dlss.addon64` (ShortFuse's DLSS add-on) | **remove**: both hook the same DLSS calls |
| stock `renodx-dlss5.addon64` | **replace** with this build |
| `nvngx_dlssnr.dll` (use the patched build on RTX 20/30/40) | keep |
| `nvngx_dlss.dll` | keep |
| RenoDX HDR mod for the game | keep |

3. If you manage games with **RHI**, a Refresh can redeploy ShortFuse's or the stock add-on over this one. Turn off RHI's Neural Rendering method for that game, and deploy this file through RHI's custom add-ons folder.

## Settings

Open the ReShade overlay → **Add-ons** → **DLSS 5 Neural Rendering**. The foveation controls sit under the resolution controls. Every setting is also a key in `ReShade.ini`, section `[RenoDX.DLSS5]`.

### Foveation

| Overlay | `ReShade.ini` key | Default | What it does |
|---|---|---|---|
| Foveated resolution | `NRFoveation` | `0` | `1` turns foveation on. Greyed out when NR runs before upscaling (see below). |
| Centre width | `NRFoveationWidth` | `3000` | Width of the full-resolution zone, in **screen pixels**, centred horizontally. |
| Centre height | `NRFoveationHeight` | `1800` | Height of the full-resolution zone, in screen pixels, centred vertically. Set it to your screen height or more to lower only the sides. |
| Fall-off | `NRFoveationFalloff` | `400` | Distance in screen pixels over which resolution blends from 100 % down to the edge value. The same on all four sides. `0` is a hard step. |
| Edge resolution | `NRFoveationEdge` | `0.50` | Resolution beyond the fall-off, relative to the centre, per direction. `0.50` = half. Range 0.25–1.00. |

Below the sliders, a **readout** shows what you actually get at the current resolution: the effective centre, the fall-off that fits on screen, the edge value reached, and the share of NR pixels left. Use it while tuning.

**Sizes are in screen pixels, so one setting works on any display.** Each frame, the add-on checks them against the real output:

- **Centre bigger than the screen** in one direction: that direction isn't lowered at all.
- **Fall-off longer than the space left** to the screen edge: the blend keeps its length and is cut off by the edge, so the edge value isn't reached. The readout shows the value that is.
- **Saving under 5 %:** foveation switches itself off for that resolution. The extra squeeze pass would cost more than it saves, and the readout says "inactive".
- **Your stored values are never overwritten.** Settings made for a larger display come back when you return to it.

### Settings that interact with foveation

| Overlay | `ReShade.ini` key | Recommended | Why |
|---|---|---|---|
| Resolution | `NRResolutionScale` | `1` | Sets the centre's resolution. Foveation lowers everything else relative to it, so `1` gives a native-resolution centre. Below 1, the whole image (centre included) drops first. |
| Resolution: Match game | `NRFollowInputRes` | `0` | When on, NR follows the game's render resolution. Leave it off so the scale above applies. |
| Before upscaling | `NRPreUpscale` | `0` | Foveation only works after upscaling. Turning this on disables foveation. |
| Upsampling | `NRLookUpsample` | `1` (Edge-aware) | How NR's result is stretched back to full size. Edge-aware keeps edits on the edges they belong to, which matters most in the lowered zones. |
| — | `EnableHooks` | `2` by default | `2` hooks NGX only. Use `1` for games that reach DLSS only through Streamline (e.g. Call of Duty). Use whatever already works for the game. |

### Example `ReShade.ini`

```ini
[RenoDX.DLSS5]
NRResolutionScale=1
NRFollowInputRes=0
NRFoveation=1
NRFoveationWidth=3000
NRFoveationHeight=1800
NRFoveationFalloff=400
NRFoveationEdge=0.5
```

## Estimating the saving

Per direction, with screen length `L`, centre `c`, fall-off `f` (fully on screen) and edge value `e`:

```
working length = c + f·(1 + e) + e·(L − c − 2f)
```

NR cost scales with working width × working height. For example, at 7680 wide: 3000 + 400·1.5 + 0.5·(7680 − 3000 − 800) = 5540 px.

Defaults (3000×1800, 400 px, 0.50) on common displays:

| Display | NR works on | NR pixels | Notes |
|---|---|---|---|
| 7680×2160 (G9 57") | 5540×2132 | 71 % | top/bottom only reach 0.79 (180 px of room) |
| 5120×2160 (5K2K) | 4260×2132 | 82 % | |
| 5160×1400 | 4280×1400 | 83 % | height fits inside the centre: sides only |
| 3840×2160 (4K) | 3620×2132 | 93 % | centre covers most of the screen |
| 3440×1440 | — | 100 % | auto-off: would save under 5 % |

On 16:9 and 21:9 screens, shrink the centre to get a real saving. For example, 2000×1100 leaves 69 % of the NR pixels at 4K and 83 % at 3440×1440.

### Tuning tips

- **Bigger saving:** shrink the centre first, then lower the edge value. The centre is most of the cost.
- **Lower edge values** (0.25–0.35) are where NR edits can start to look softer in your peripheral vision. Check in motion, not on stills.
- **A longer fall-off** hides the transition better but saves less. 300–600 px works well at 4K-class heights.
- **Expect a brief shimmer while dragging a slider.** Moving any foveation slider resets NR's temporal history. A size change also rebuilds NR after about half a second.

## Checking that it works

- `ReShade.log` contains a line like:
  `foveated NR active: working 5540x2132 for output 7680x2160 (centre 3000x1800, fall-off on screen 400/180 px, edge density 0.5/0.79, 71% of the NR pixels)`
- The add-on's **Diagnostics** section shows the reduced **Working resolution**.
- Compare frame times with foveation on and off (PresentMon or CapFrameX), not average FPS.

## Limits

- After-upscale placement only. When NR runs before upscaling, it uses the normal uniform resolution.
- Changing foveation settings resets NR's temporal history.
- If the game's depth or motion-vector format can't be read, the frame still runs, but with unaligned inputs: edits on the sides may smear. The log says so once.
- New overlay text is English only.

Technical details (math, shader changes, performance notes): [`src/addons/dlss5/FOVEATION.md`](src/addons/dlss5/FOVEATION.md).

## Building it yourself

The [`Build DLSS5 add-on`](.github/workflows/build-dlss5.yml) workflow builds only this add-on on GitHub's Windows runners. Pushing a `v*` tag builds it and publishes a release (notes from `.github/release-notes/<tag>.md`). To build locally, follow RenoDX's [build guide](docs/CONTRIBUTING.md), then run:

```
cmake --preset vs-x64
cmake --build --preset vs-x64-release --target dlss5
```

## Credits and licence

- [RenoDX](https://github.com/clshortfuse/renodx) by clshortfuse, MIT licence.
- RenoDX-DLSS5-Generic add-on source (v7.5.0-rc5), MIT licence; see [`src/addons/dlss5/LICENSE`](src/addons/dlss5/LICENSE).
- Foveated working grid: this repository.

DLSS and the NGX runtime are NVIDIA's; this add-on does not ship them.
