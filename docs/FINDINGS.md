# RE6DLSS findings

## Constraints (2026-11-23)

- RE6 (BH6.exe) is a 32-bit D3D9 game. DLSS (nvngx_dlss.dll and the driver's _nvngx.dll) is
  64-bit only, so DLSS has to run in a 64-bit helper process; frames are shared with it through
  shared textures.
- RenoDX runs the game's D3D9 on D3D11 inside its proxy device; the proxy's calls don't raise
  ReShade add-on events, so the probe follows the game's D3D9 calls.
- No TAA: no jitter. Jitter has to be added to the projection (RE6Tweaks already hooks the camera
  per frame at 0x60DF70). uMotionBlurFilter owns mpVelocityMap; in gameplay its velocity
  resolution is 0 (full).
- Scope agreed: DLAA only (the game is light).

## 0.0.1 probe

Loads with RE6Tweaks; first present D3D9 3840x2160 bgra8; RenoDX's D3D11 presents ignored.
Capture: F10 ([RE6DLSS] CaptureHotkey=1) or the overlay button -> RE6DLSS-frame-N.txt.
