# RE6DLSS 1.0.0

ReShade add-on that adds **DLAA** (NVIDIA DLSS anti-aliasing at native resolution) to
**Resident Evil 6** (Steam, `BH6.exe` 1.1.0.0). It replaces the game's FXAA/MSAA with DLSS's
temporal anti-aliasing: stable, sharp edges on railings, wires, hair and character outlines.

RE6 is a 32-bit Direct3D 9 game and DLSS only exists as 64-bit code, so DLSS runs in a small
64-bit helper program (`RE6DLSS-helper64.exe`) that the add-on starts and stops with the game.
Each frame the scene, its depth and the camera are shared with the helper and the DLAA image comes
back before the game's tonemapping, so bloom, HDR (RenoDX), post effects and the HUD work as usual.

## Requirements

- An **NVIDIA RTX** graphics card and a current driver.
- **ReShade 6.8 or later, 32-bit, with add-on support**, installed as `d3d9.dll` next to `BH6.exe`.
- **RenoDX for Resident Evil 6** (`renodx-residentevil6.addon32`). Besides HDR, it upgrades the
  game's Direct3D 9 device to Direct3D 9Ex, which is what lets the game share images with the
  helper. Without it the add-on says so in the overlay and stays off.
- Recommended: **RE6Tweaks** (fixes the startup crashes on displays with many display modes).

## Install

Copy these into the game folder (where `BH6.exe` is):

- `RE6DLSS.addon32` (the add-on)
- `RE6DLSS-helper64.exe` (the 64-bit helper that runs DLSS)
- `nvngx_dlss.dll` (NVIDIA's DLSS 310.9.16)

Then, in the game's options, set **anti-aliasing to Off** (DLAA replaces it).

## Settings

ReShade overlay (Home key), Add-ons tab, **RE6DLSS**; saved in `ReShade.ini` under `[RE6DLSS]`.

- **Anti-aliasing**: Off or DLAA (default DLAA).
- **DLSS model**: Automatic (K for DLAA), or K, J, L or M.
- **Status**: what's running and its cost per frame, or why it isn't.
- **Developer tools** (collapsed): a round trip test (copy / invert the picture through the
  helper), a jitter test, and a frame capture tool.

## How it works

- **Jitter**: RE6 has no TAA, so the add-on adds DLAA's sub-pixel camera jitter itself, to the
  camera's view-projection (vertex shader constants c1-c4) of the 3D scene's draws.
- **Depth**: the game writes its depth into the colour channels of its first full-screen pass
  (24 bits split over R, G and B); the helper decodes it.
- **Motion vectors**: the game has no velocity buffer, so camera motion is computed from depth
  and the current and previous camera. Characters' own movement isn't known, which can leave light
  trails on moving characters.
- **Hand-off**: at the tonemap pass, the HDR scene and depth go to the helper through Direct3D 9Ex
  shared textures; the helper runs DLSS on Direct3D 11 and hands the result back.

## Notes

- **Cost**: about 3-4 ms per frame at 4K (DLSS itself ~2.8 ms, plus the hand-off). The game waits
  for the helper each frame.
- If the NVIDIA app's DLSS override (model preset) is set for this game, the driver uses that model
  whatever the DLSS model setting says.
- Logs: `RE6DLSS.log` (add-on) and `RE6DLSS-helper64.log` (helper, including NVIDIA's NGX messages)
  next to `BH6.exe`.
- Tested with Leon's campaign at 3840x2160 with RenoDX HDR on an RTX 5080.

## Build

`.\build.ps1 [-Install] [-Package]` (Visual Studio 2022+ with the C++ x86 and x64 tools). Builds the
32-bit add-on and the 64-bit helper; `-Package` makes `bin\release\RE6DLSS-<version>.zip`.
`docs/FINDINGS.md` records how the game's frames were mapped.

## Licence

MIT (LICENSE). Third-party licences: THIRD_PARTY_NOTICES.txt and NVIDIA-DLSS-LICENSE.txt.
