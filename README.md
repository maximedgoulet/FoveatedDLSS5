# FoveatedDLSS5

### DLSS 5 Neural Rendering where your eyes are. Nowhere else.

**Up to 5.5× less Neural Rendering work** on a 32:9 screen, and still 2–3× on a plain 16:9 monitor. You get full NR quality in the middle of the action and a softer NR treatment in a contour around it. Beyond that, NR is switched off entirely: the roof liner, the sky above the windshield, the far edges of a super-ultrawide.

FoveatedDLSS5 is the open-source RenoDX DLSS 5 add-on (`renodx-dlss5`, RenoDX-DLSS5-Generic v7.5.0-rc5) with **dual foveation** built in. Everything else in the add-on works exactly as before, and with foveation off it *is* the stock add-on.

> **Status: pre-release.** v0.1 was tested in The Witcher 3 (DX12, 7680×2160, RTX 4090). The movable zones, dual foveation (v0.3), and the percent-based zones with the on-screen preview (v0.4) are new and not yet fully tested in games. Field reports are very welcome.

---

## Why this exists

DLSS 5 NR is gorgeous and **expensive**, and its cost grows with every pixel it touches. On a 7680×2160 panel that's 16.6 million pixels a frame, and most of them sit in your peripheral vision. There, your eye can't tell a native-resolution edit from a half-resolution one, or from no edit at all.

So FoveatedDLSS5 spends NR like a budget, in three zones:

```
┌──────────────────────────────────────────────────────────────────────┐
│ EDGE, NR OFF: game image as-is (sky, roof liner, far edges)          │
│      ┌────────────────────────────────────────────────────────┐      │
│      │ CONTOUR: NR at the contour resolution (e.g. 50 %)      │      │
│      │          ┌──────────────────────────────┐              │      │
│      │          │ CENTRE ZONE                  │              │      │
│      │          │ NR at full centre resolution │              │      │
│      │          └──────────────────────────────┘              │      │
│      │      smooth fall-off between the two (no seam)         │      │
│      └────────────────────────────────────────────────────────┘      │
│      soft blend where NR fades out                                   │
└──────────────────────────────────────────────────────────────────────┘
```

Per axis it's simple arithmetic: **centre + contour + edge = 100 %**. When you switch foveation on, you start with a 50 % centre, a 25 % contour and a 25 % edge. No combination can contradict itself. While you tune, the overlay draws the zones **right over the game** and shows the saving before you even open a frame counter.

Racing game? Slide the zones down so the road and the dash get everything and the sky gets nothing.

## The numbers

How much of a full-screen NR pass is left (NR pixels, centre at native resolution). Lower is better, and the speed-up is in brackets:

| Preset | 16:9 (3840×2160) | 21:9 (3440×1440) | 32:9 (5120×1440 / 7680×2160) |
|---|---|---|---|
| Stock add-on | 100 % | 100 % | 100 % |
| Default layout (50 % + 25 %) | 43 % (2.3×) | 43 % (2.3×) | 43 % (2.3×) |
| **Balanced** | 69 % (1.4×) | 60 % (1.7×) | 41 % (2.5×) |
| **Racing** | 51 % (2.0×) | 40 % (2.5×) | 26 % (3.8×) |
| **Aggressive** | 36 % (2.8×) | 27 % (3.7×) | 18 % (5.5×) |

**Why wider screens win bigger:** what your eyes cover scales with the screen's *height*, not its width. On 32:9 the extra width is all periphery, and that's exactly what foveation trims. The presets below keep the same zone *heights* on every display and take a smaller share of the width on wider screens.

> These are theoretical NR pixel counts from the add-on's own math, not measured frame times. NR's cost tracks its pixel count, but the add-on's own passes (squeeze, guide resample, resolve) and fixed overheads remain, so the real frame-time gain is smaller. Turn on **GPU timers** (Diagnostics) and the overlay shows the measured NR time next to the forecast. Or measure with PresentMon or CapFrameX, and tell us what you get.

## Presets

Paste into `ReShade.ini`, section `[RenoDX.DLSS5]`, or set the same values with the sliders. Heights are the same on every display; take the width pair for your aspect ratio.

| Preset | Centre height | Contour height | Centre width (16:9 / 21:9 / 32:9) | Contour width (16:9 / 21:9 / 32:9) | Contour res. | Vertical pos. |
|---|---|---|---|---|---|---|
| Balanced | 60 | 30 | 67 / 50 / 34 | 33 / 42 / 28 | 0.50 | 50 |
| Racing | 45 | 25 | 56 / 42 / 28 | 44 / 38 / 25 | 0.50 | 62 |
| Aggressive | 40 | 25 | 45 / 33 / 23 | 45 / 34 / 22 | 0.35 | 60 |

All presets use the default fall-off (50 %) and fade (25 %).

**Racing on 32:9 (Forza Horizon, BeamNG.drive, rally):** low zones, roof and sky off, NR on 26 % of the pixels:
```ini
NRResolutionScale=1
NRFollowInputRes=0
NRFoveation=1
NRFoveationCenterW=28
NRFoveationCenterH=45
NRFoveationContourW=25
NRFoveationContourH=25
NRFoveationEdge=0.5
NRFoveationCenterY=62
```

With **Show zones on screen** on, nudge the vertical position until the top of the amber contour sits just above the horizon.

## How it works

DLSS 5 NR runs at one fixed resolution per evaluation, so the add-on can't hand it a mixed-resolution image. Instead it **squeezes** the frame before NR and **stretches** NR's result back:

- **Centre zone:** pixels are kept 1:1 at the centre resolution (`NRResolutionScale`).
- **Fall-off:** resolution blends smoothly (smoothstep, no visible seam) down to the contour resolution.
- **Contour:** resolution stays at the contour value. Horizontal and vertical are independent.
- **Edge, beyond the contour:** nothing. The squeezed image simply doesn't contain it, and the final image keeps the game's own pixels there, with NR fading in over a soft blend.
- NR runs **once** on the small image, with one temporal history. The game's depth and motion vectors are squeezed the same way, so NR's inputs stay aligned.
- Multi-pass NR stacking works as before: every pass uses the same zones.

## Install

1. Download `renodx-dlss5.addon64` from the latest [release](../../releases). For the newest build, open [Actions → Build DLSS5 add-on](../../actions/workflows/build-dlss5.yml) and download the artifact of a green run.
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

ReShade overlay → **Add-ons** → **DLSS 5 Neural Rendering**. The foveation controls sit under the resolution controls. Every setting is also a key in `ReShade.ini`, section `[RenoDX.DLSS5]`.

### Foveation

| Overlay | `ReShade.ini` key | Default | What it does |
|---|---|---|---|
| Foveated resolution | `NRFoveation` | `0` | `1` turns foveation on. Greyed out when NR runs before upscaling. |
| Zone map | — | — | Your screen in its aspect ratio with the zones as they run: teal centre, amber contour, dark edge. |
| NR saving | — | — | The forecast: how much NR work the layout removes and how the screen splits. With GPU timers on, also the measured NR time with and without foveation. |
| Centre width / height | `NRFoveationCenterW` / `NRFoveationCenterH` | `50` / `50` | The full-resolution centre zone, in **% of the screen**. |
| Contour width / height | `NRFoveationContourW` / `NRFoveationContourH` | `25` / `25` | Thickness added around the centre at the contour resolution, in % of the screen, both sides together (25 = 12.5 % per side). It can't exceed what the centre leaves. |
| Edge (NR off) | — | — | Read-only: what's left (100 − centre − contour). NR is off there and you see the game's own image. |
| Vertical / horizontal position | `NRFoveationCenterY` / `NRFoveationCenterX` | `50` / `50` | Where the zones sit, in %: `0` top/left, `50` middle, `100` bottom/right. |
| Contour resolution | `NRFoveationEdge` | `0.50` | Resolution of the contour relative to the centre, per direction. Range 0.25–1.00. |
| Resolution fall-off | `NRFoveationFalloffPct` | `50` | How gradually resolution steps from the centre to the contour value, in % of the contour's thickness. `0` is a hard step. |
| Fade to the edge | `NRFoveationBlendPct` | `25` | How far inside the contour NR fades out toward the edge, in % of the contour's thickness. |
| Show zones on screen | `NRFoveationPreview` | `1` | Outlines the zones over the game: `0` off, `1` while the overlay is open, `2` always (tune while you drive). Drawing only; NR's image is untouched. |

**Built-in safeguards:**

- **Centre + contour + edge = 100 %, always.** The contour slider only goes as far as the centre leaves, and growing the centre takes the room from the contour. What the sliders show is what runs.
- The centre is never smaller than 10 % of the screen.
- A **zone pushed past an edge** stops flush with that edge. Positions snap to whole pixels, and the map shows where the zones really are.
- **Out-of-range `ReShade.ini` values** are clamped, and broken values fall back to the defaults.
- **Saving under 5 %:** foveation switches itself off for that resolution, because the extra passes would cost more than they save. The forecast says so.
- Sizes are percentages, so one setting fits every display.

**Upgrading from v0.3 or earlier:** the pixel keys (`NRFoveationWidth`, `NRFoveationMidWidth`, ...) are no longer read. When you switch foveation on, you start from the default layout.

### Settings that interact with foveation

| Overlay | `ReShade.ini` key | Recommended | Why |
|---|---|---|---|
| Resolution | `NRResolutionScale` | `1` | The **centre resolution**. Everything else is relative to it. |
| Resolution: Match game | `NRFollowInputRes` | `0` | Leave off so the scale above applies. |
| Before upscaling | `NRPreUpscale` | `0` | Foveation works after upscaling only. |
| Upsampling | `NRLookUpsample` | `1` (Edge-aware) | Keeps NR's edits on the edges they belong to, which matters most in the lowered zones. |
| — | `EnableHooks` | `2` by default | `1` for games that reach DLSS only through Streamline (e.g. Call of Duty). |

### Tuning tips

- **Turn on Show zones on screen first.** You see exactly what each slider does.
- **Cut what you never look at first.** A bigger edge (roof, sky, far edges) saves more than lowering resolutions.
- **Then shrink the centre**, then lower the contour resolution. 0.25–0.35 is where edits can start to look softer in your peripheral vision. Check in motion, not on stills.
- **A longer fall-off and fade** hide the transitions better but save less.
- **Expect a brief shimmer while dragging a zone slider.** Moving or resizing a zone resets NR's temporal history, and a size change also rebuilds NR after about half a second.

## On the roadmap

- **Half-rate NR** (optional, off by default): run NR every other frame and carry its edit through the in-between frame with the game's motion vectors. That's about 2× on top of everything above. It switches itself off for 5–10 frames whenever motion gets fast, and stays off until motion calms down.
- **Auto-trim** (optional, off by default): finds the sky (from the game's depth buffer) and blown-out or pitch-black bands, and switches NR off there automatically. It costs under 1 % of NR.

## Checking that it works

- `ReShade.log` contains a line like:
  `foveated NR active: working 3350x1310 for output 7680x2160 (centre ..., midpoint zone ..., 26% of the NR pixels)` (the Racing preset)
- The overlay's **NR saving** row shows the forecast, and **Working at** shows the reduced working resolution.
- Compare frame times with foveation on and off (PresentMon or CapFrameX), not average FPS.

## Limits

- After-upscale placement only. When NR runs before upscaling, it uses the normal uniform resolution.
- Changing foveation settings resets NR's temporal history.
- NR changes the look (lighting, texture, colour), so the border to the off zone is a change of look, not just of sharpness. The fade softens it; size the contour so the border falls where you don't look.
- If the game's depth or motion-vector format can't be read, the frame still runs, but with unaligned inputs: edits outside the centre may smear. The log says so once.
- New overlay text is English only.

Technical details (math, shader changes): [`src/addons/dlss5/FOVEATION.md`](src/addons/dlss5/FOVEATION.md).

## Building it yourself

The [`Build DLSS5 add-on`](.github/workflows/build-dlss5.yml) workflow builds the add-on on GitHub's Windows runners (MSVC, plus a Clang check) on every code push. Pushing a `v*` tag publishes a release (notes from `.github/release-notes/<tag>.md`). To build locally, follow RenoDX's [build guide](docs/CONTRIBUTING.md), then run:

```
cmake --preset vs-x64
cmake --build --preset vs-x64-release --target dlss5
```

## Credits and licence

- [RenoDX](https://github.com/clshortfuse/renodx) by clshortfuse, MIT licence.
- RenoDX-DLSS5-Generic add-on source (v7.5.0-rc5), MIT licence; see [`src/addons/dlss5/LICENSE`](src/addons/dlss5/LICENSE).
- Foveated and dual-foveated working grid: this repository.

DLSS and the NGX runtime are NVIDIA's; this add-on does not ship them.
