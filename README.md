# RE6DLSS 1.2.0

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
- Optional: **RenoDX for Resident Evil 6** (`renodx-residentevil6.addon32`) for HDR. DLAA works
  with or without it. Sharing images with the helper needs a Direct3D 9Ex device: RenoDX upgrades
  the game's device to 9Ex, and without RenoDX the add-on asks ReShade for the upgrade itself. A
  9Ex device can't use exclusive fullscreen the way the game asks for it, so the game then runs
  borderless windowed at the same resolution (as it does with RenoDX). `UpgradeDevice=0` in
  `[RE6DLSS]` turns the upgrade off (DLAA then stays off without RenoDX).
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
- **Character motion** (default off): gives DLAA the movement of characters, enemies and moving
  objects, not only the camera's. RE6's characters look clean without it, and it costs CPU time for
  every character on screen (about 1 ms per frame with several NPCs).
- **Effect mask** (default on): tells DLAA not to carry fire, smoke, sparks and other see-through
  effects over from earlier frames.
- **Pipelined** (default on): the game doesn't wait for DLAA; each frame shows the previous frame's
  DLAA image (one frame of latency for the 3D image). Off: every frame waits for DLAA.
- **Status**: what's running and its cost per frame, or why it isn't.
- **Developer tools** (collapsed): a round trip test (copy / invert the picture through the
  helper), a jitter test, debug views (motion vectors, effect mask) and a frame capture tool
  (needs `Developer=1` in `[RE6DLSS]`).

## How it works

- **Jitter**: RE6 has no TAA, so the add-on adds DLAA's sub-pixel camera jitter itself, to the
  camera's view-projection (`fViewProj`, found in each vertex shader's constant table) of the 3D
  scene's draws.
- **Depth**: the game writes its depth into the colour channels of its first full-screen pass
  (24 bits split over R, G and B); the helper decodes it.
- **Motion vectors**: the game has no velocity buffer, so camera motion is computed from depth
  and the current and previous camera. With Character motion on, each moving character part is
  drawn once more with a patched copy of its vertex shader that also computes last frame's position
  (last frame's bones, world matrix and camera), which gives its own motion.
- **Effect mask**: the target the game draws its see-through effects into is copied before the
  first effect and again at the end; what changed is the mask.
- **Hand-off**: at the tonemap pass, the HDR scene and depth go to the helper through Direct3D 9Ex
  shared textures; the helper runs DLSS on Direct3D 11 and hands the result back (pipelined: at the
  next frame's tonemap pass, so neither the game nor the GPU waits).
- The add-on follows the game's Direct3D 9 calls directly (render targets, shaders, constants,
  draws), which keeps its own CPU time at about 0.2 ms per frame.

## Notes

- **Cost**: about 2.5 ms of GPU time per frame at 4K on an RTX 5080 (DLSS ~1.5 ms plus input
  passes and copies), run alongside the game when pipelined; about 0.2 ms of CPU time on the game's
  render thread (plus about 1 ms with Character motion and several NPCs). On the test system the
  frame rate with DLAA matched the frame rate without it. The helper runs at high priority with
  Windows' power throttling turned off for it; its log lists the GPU time of each step.
- If the NVIDIA app's DLSS override (model preset) is set for this game, the driver uses that model
  whatever the DLSS model setting says.
- Logs: `RE6DLSS.log` (add-on) and `RE6DLSS-helper64.log` (helper, including NVIDIA's NGX messages)
  next to `BH6.exe`.
- Tested with Leon's campaign at 3840x2160 on an RTX 5080, with RenoDX HDR and without RenoDX.
  Without RenoDX the scene is 8-bit, and DLSS is set up for 8-bit colour.

## Changes

- **1.2.0**: works without RenoDX: the add-on asks ReShade for the Direct3D 9Ex device itself
  (the game then runs borderless windowed instead of exclusive fullscreen), and DLSS is set up for
  the game's own 8-bit scene. With RenoDX nothing changes.
- **1.1.0**: much lower cost: pipelined hand-off (no waiting on the game's thread) and the add-on
  follows the game's Direct3D 9 calls directly instead of ReShade's per-draw events; effect mask
  for see-through effects; optional character motion vectors; debug views.
- **1.0.1**: fixed black shards on faces in cutscenes (cutscene character shaders keep the camera
  matrix elsewhere, so they missed the jitter); fixed random 16-18 ms frames (Windows throttled the
  helper); DLSS history resets on camera cuts; invalid colour values are removed before DLSS.
- **1.0.0**: first release.

## Build

`.\build.ps1 [-Install] [-Package]` (Visual Studio 2022+ with the C++ x86 and x64 tools). Builds the
32-bit add-on and the 64-bit helper; `-Package` makes `bin\release\RE6DLSS-<version>.zip`. Put
NVIDIA's `nvngx_dlss.dll` (310.9.16, not in the repository) in `third_party\bin` first.
`docs/FINDINGS.md` records how the game's frames were mapped.

## Licence

MIT (LICENSE). Third-party licences: THIRD_PARTY_NOTICES.txt and NVIDIA-DLSS-LICENSE.txt.
