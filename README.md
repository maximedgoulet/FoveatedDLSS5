# FoveatedDLSS5

### DLSS 5 Neural Rendering where your eyes are. Nowhere else.

**Up to 5.8× less Neural Rendering work** on a 32:9 screen, and still 2–3× on a plain 16:9 monitor. You get full NR quality in the middle of the action and a softer NR treatment around it. Beyond that, NR is switched off entirely: the roof liner, the sky above the windshield, the far edges of a super-ultrawide.

FoveatedDLSS5 is the open-source RenoDX DLSS 5 add-on (`renodx-dlss5`, RenoDX-DLSS5-Generic v7.5.0-rc5) with **dual foveation** built in. Everything else in the add-on works exactly as before, and with foveation off it *is* the stock add-on.

> **Status: pre-release.** v0.1 was tested in The Witcher 3 (DX12, 7680×2160, RTX 4090). The movable zones and dual foveation (v0.3) are new and not yet tested in a game. Field reports are very welcome.

---

## Why this exists

DLSS 5 NR is gorgeous and **expensive**, and its cost grows with every pixel it touches. On a 7680×2160 panel that's 16.6 million pixels a frame, and most of them sit in your peripheral vision. There, your eye can't tell a native-resolution edit from a half-resolution one, or from no edit at all.

So FoveatedDLSS5 spends NR like a budget, in three zones:

```
┌──────────────────────────────────────────────────────────────────────┐
│ OFF: game image as-is (sky, roof liner, far edges)                   │
│      ┌────────────────────────────────────────────────────────┐      │
│      │ MIDPOINT ZONE: NR at midpoint resolution (e.g. 50 %)   │      │
│      │          ┌──────────────────────────────┐              │      │
│      │          │ CENTRE ZONE                  │              │      │
│      │          │ NR at full centre resolution │              │      │
│      │          └──────────────────────────────┘              │      │
│      │      smooth fall-off between the two (no seam)         │      │
│      └────────────────────────────────────────────────────────┘      │
│      soft blend where NR fades out                                   │
└──────────────────────────────────────────────────────────────────────┘
```

All three rectangles are yours to size and place. Racing game? Slide the zones down so the road and the dash get everything and the sky gets nothing.

## The numbers

How much of a full-screen NR pass is left (NR pixels, centre at native resolution). Lower is better, and the speed-up is in brackets:

| Preset | 16:9 (3840×2160) | 21:9 (3440×1440) | 32:9 (5120×1440) | 32:9 (7680×2160) |
|---|---|---|---|---|
| Stock add-on | 100 % | 100 % | 100 % | 100 % |
| Classic foveation (v0.1 defaults) | 93 % (1.1×) | 82 % (1.2×) | 71 % (1.4×) | 71 % (1.4×) |
| **Balanced** | 71 % (1.4×) | 60 % (1.7×) | 40 % (2.5×) | 40 % (2.5×) |
| **Racing** | 51 % (2.0×) | 39 % (2.5×) | 26 % (3.8×) | 26 % (3.8×) |
| **Aggressive** | 34 % (2.9×) | 26 % (3.9×) | 17 % (5.8×) | 17 % (5.8×) |

**Why wider screens win bigger:** what your eyes cover scales with the screen's *height*, not its width. On 32:9 the extra width is all periphery, and that's exactly what foveation trims.

> These are theoretical NR pixel counts from the add-on's own math, not measured frame times. NR's cost tracks its pixel count, but the add-on's own passes (squeeze, guide resample, resolve) and fixed overheads remain, so the real frame-time gain is smaller. Measure with PresentMon or CapFrameX and tell us what you get.

## Presets

Paste into `ReShade.ini`, section `[RenoDX.DLSS5]`. These are for 7680×2160; for other screens, take the row from the table below.

**Balanced:** NR everywhere you'd look, outer band off:
```ini
NRResolutionScale=1
NRFollowInputRes=0
NRFoveation=1
NRFoveationWidth=2590
NRFoveationHeight=1300
NRFoveationMidWidth=4750
NRFoveationMidHeight=1940
NRFoveationFalloff=260
NRFoveationEdge=0.5
NRFoveationCenterY=50
```

**Racing (Forza Horizon, BeamNG.drive, rally):** low zones, roof and sky off:
```ini
NRFoveationWidth=2160
NRFoveationHeight=970
NRFoveationMidWidth=4100
NRFoveationMidHeight=1510
NRFoveationFalloff=220
NRFoveationEdge=0.5
NRFoveationCenterY=62
```

**Aggressive:** maximum frames:
```ini
NRFoveationWidth=1730
NRFoveationHeight=860
NRFoveationMidWidth=3460
NRFoveationMidHeight=1400
NRFoveationFalloff=170
NRFoveationEdge=0.35
NRFoveationCenterY=60
```

| Preset | Display | Centre W×H | Midpoint W×H | Fall-off | Midpoint res. | Vertical pos. |
|---|---|---|---|---|---|---|
| Balanced | 3840×2160 | 2590×1300 | 3840×1940 | 260 | 0.50 | 50 |
| Balanced | 3440×1440 / 5120×1440 | 1730×860 | 3170×1300 | 170 | 0.50 | 50 |
| Racing | 3840×2160 | 2160×970 | 3840×1510 | 220 | 0.50 | 62 |
| Racing | 3440×1440 / 5120×1440 | 1440×650 | 2740×1010 | 140 | 0.50 | 62 |
| Aggressive | 3840×2160 | 1730×860 | 3460×1400 | 170 | 0.35 | 60 |
| Aggressive | 3440×1440 / 5120×1440 | 1150×580 | 2300×940 | 120 | 0.35 | 60 |

The racing presets put the zone's middle at 62 % of the screen height. Watch the overlay readout and nudge `NRFoveationCenterY` until the top of the midpoint zone sits just above the horizon.

## How it works

DLSS 5 NR runs at one fixed resolution per evaluation, so the add-on can't hand it a mixed-resolution image. Instead it **squeezes** the frame before NR and **stretches** NR's result back:

- **Centre zone:** pixels are kept 1:1 at the centre resolution (`NRResolutionScale`).
- **Fall-off:** resolution blends smoothly (smoothstep, no visible seam) down to the midpoint resolution.
- **Midpoint zone:** resolution stays at the midpoint value. Horizontal and vertical are independent.
- **Beyond the midpoint zone:** nothing. The squeezed image simply doesn't contain it, and the final image keeps the game's own pixels there, with NR fading in over a soft blend.
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
| Centre width / height | `NRFoveationWidth` / `NRFoveationHeight` | `3000` / `1800` | Size of the full-resolution centre zone, in **screen pixels**. |
| Centre vertical position | `NRFoveationCenterY` | `50` | Where the zones sit vertically, in %: `0` top, `50` middle, `100` bottom. |
| Centre horizontal position | `NRFoveationCenterX` | `50` | `0` left, `50` middle, `100` right. |
| Fall-off | `NRFoveationFalloff` | `400` | Distance in screen pixels over which resolution blends from the centre to the midpoint value. `0` is a hard step. |
| Midpoint resolution | `NRFoveationEdge` | `0.50` | Resolution of the midpoint zone relative to the centre, per direction. Range 0.25–1.00. |
| Midpoint zone width / height | `NRFoveationMidWidth` / `NRFoveationMidHeight` | `16384` | Size of the midpoint zone, centred on the same position. **Beyond it NR is off.** The screen size or more = NR everywhere (the classic v0.1 behaviour, and the default). |
| Off-zone blend | `NRFoveationBlend` | `96` | How far inside the midpoint zone NR fades in, so the border to the game's own image is soft. |

The **readout** under the sliders shows what you actually get: the effective zones in pixels, the fall-off that fits, the midpoint value reached, and the share of NR pixels left. Tune with it open.

**Built-in safeguards.** Sizes are in screen pixels, so one setting works on any display. Every frame they're checked against the real output:

- The **midpoint zone is never smaller than the centre zone**, nor smaller than 10 % of the screen. It's shifted (never shrunk) to stay on screen, and the centre zone always sits inside it.
- A **zone pushed past an edge** stops flush with that edge. Positions snap to whole pixels.
- **Out-of-range `ReShade.ini` values** are clamped, and broken values fall back to the defaults.
- A **fall-off longer than the space left** is cut off by the zone's edge. The readout shows the value actually reached.
- **Saving under 5 %:** foveation switches itself off for that resolution, because the extra passes would cost more than they save.
- **Your stored values are never overwritten**, so settings made for a larger display come back when you return to it.

### Settings that interact with foveation

| Overlay | `ReShade.ini` key | Recommended | Why |
|---|---|---|---|
| Resolution | `NRResolutionScale` | `1` | The **centre resolution**. Everything else is relative to it. |
| Resolution: Match game | `NRFollowInputRes` | `0` | Leave off so the scale above applies. |
| Before upscaling | `NRPreUpscale` | `0` | Foveation works after upscaling only. |
| Upsampling | `NRLookUpsample` | `1` (Edge-aware) | Keeps NR's edits on the edges they belong to, which matters most in the lowered zones. |
| — | `EnableHooks` | `2` by default | `1` for games that reach DLSS only through Streamline (e.g. Call of Duty). |

### Tuning tips

- **Cut what you never look at first.** Shrinking the midpoint zone (roof, sky, far edges) saves more than lowering resolutions.
- **Then shrink the centre**, then lower the midpoint resolution. 0.25–0.35 is where edits can start to look softer in your peripheral vision. Check in motion, not on stills.
- **A longer fall-off and blend** hide the transitions better but save less.
- **Expect a brief shimmer while dragging a zone slider.** Moving or resizing a zone resets NR's temporal history, and a size change also rebuilds NR after about half a second.

## On the roadmap

- **Half-rate NR** (optional, off by default): run NR every other frame and carry its edit through the in-between frame with the game's motion vectors. That's about 2× on top of everything above. It switches itself off for 5–10 frames whenever motion gets fast, and stays off until motion calms down.
- **Auto-trim** (optional, off by default): finds the sky (from the game's depth buffer) and blown-out or pitch-black bands, and switches NR off there automatically. It costs under 1 % of NR.

## Checking that it works

- `ReShade.log` contains a line like:
  `foveated NR active: working 3240x1350 for output 7680x2160 (centre 2160x970 at 3840,1339, midpoint zone x 1790-5890 y 584-2094, fall-off on screen 219/220 px, edge density 0.5/0.5, 26% of the NR pixels)` (the Racing preset)
- The add-on's **Diagnostics** section shows the reduced **Working resolution**.
- Compare frame times with foveation on and off (PresentMon or CapFrameX), not average FPS.

## Limits

- After-upscale placement only. When NR runs before upscaling, it uses the normal uniform resolution.
- Changing foveation settings resets NR's temporal history.
- NR changes the look (lighting, texture, colour), so the border to the off zone is a change of look, not just of sharpness. The blend softens it; size the midpoint zone so the border falls where you don't look.
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
