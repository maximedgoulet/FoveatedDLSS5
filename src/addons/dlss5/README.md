# RenoDX DLSS Neural Rendering add-on

Source for the generic RenoDX/ReShade add-on that runs NVIDIA DLSS Neural Rendering
(DLSSNR, NGX feature 18) around a game's existing DLSS evaluation. DLSSNR is a
visual enhancer; it performs no denoising.

This repository contains **add-on source only**. It has no standalone build system,
RenoDX shared utilities, vendor SDKs, ReShade loader, NVIDIA runtime, test data, or
compiled downloads. The files are designed to live at `src/addons/dlss5` in a
compatible [RenoDX](https://github.com/clshortfuse/renodx) source checkout. A
compatible full checkout and its toolchain are required to build the `dlss5` target.

| Branch | Source version | Status |
| --- | --- | --- |
| `main` | v7.5.0-rc5 | Experimental v7.5 maintenance source |
| `v7.6` | v7.6.0-rc3 | Backport candidate; qualification failed, not a release |
| `v8.5dev` | v8.5.0-rc10 | Development snapshot, not a release |

These branches are source snapshots. No branch here implies that a binary has
passed field qualification. The add-on's optional **Upload & copy link** action
sends an issue-report archive to a third-party temporary file host only after
the player selects that action.

This source derives from RenoDX and retains its MIT license and original
copyright notice in [LICENSE](LICENSE). Dependencies have their own licenses
and are not included here.
