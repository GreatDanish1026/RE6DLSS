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

## Capture 1 (Leon gameplay, 2026-11-23; probe 0.0.1)

- Shadows: 4096^2 and 2048^2 r32f maps (+ d24s8). Small 128^2 cubemap/probe passes first.
- Full resolution (3840x2160; RenoDX upgrades 8-bit targets to rgba16f, so formats don't say much):
  - #52c54: first pass, cleared to 1,1,1,0, 75 draws + psce54d37f: depth/normal pre-pass (read
    later by the DOF/bloom downsample ps5b4919ca as t1). Gets 97 more draws (vse3afba7e /
    ps47a6657e) after the scene: unknown (velocity?).
  - #52d32: 150 draws + full-screen psfbc8a86f; #52c66: deferred light accumulation
    (ps2be89d3f, psd02c3790, psbbc46988, ps401f3624, psb4eb7475); #52c9e: 17 draws.
  - #52c0a: cleared 0,0,0,1; 63 + 215 draws, ps66cf5787 combines #52c66 + #52c9e = scene colour.
    Copied to #52c30 (twice); particles/effects (5-index draws) on #52c0a in between.
  - Depth: #5c60e d24s8 (not readable as a texture in D3D9 unless INTZ).
- Post: ps5b4919ca (#52c30 + #52c54 -> 1920x1080), bloom/DOF chain down to 120x67 and back,
  exposure (r32f 480x270 -> 64 -> 16 -> 4 -> 1x1), **ps1009a8dd = tonemap/bloom combine:
  #52c30 + exposure 4x4 + bloom -> #52d6a**. Then psf186e4a8 -> #52d44, psc4a5efd2, psb5cf89a3,
  ps763d02c3 (640x360 overlay texture: film grain?), UI ps7682c3fb.
- **DLAA goes on #52c30 before ps1009a8dd.**

## 0.0.2 (title screen)

- The game's device is **IDirect3DDevice9Ex** (RenoDX's upgrade): D3D9Ex shared textures are
  possible, so frames can go to the 64-bit helper.
- RenoDX's D3D11 device is separate (API 0xb000); it only presents. Rendering is real D3D9.
- Constants: push_constants VS = layout param 2, PS = param 5; first/count in floats (first 4 =
  c1). 0.0.1 ignored them (expected param 0), hence all-zero dumps.
- Render target snapshots on capture (RE6DLSS-rt-*.bin, every 4th pixel) + tools/rt2png.py.

## Captures 2-3 with snapshots (0.0.2, still + moving, 2026-11-23)

Snapshots from the moving capture (every 4th pixel, tools/rt2png.py; ids differ per run):
- **Depth is packed into colour**: the pre-pass target (#2965e this run) holds a 24-bit depth
  split over R (high byte, 0.79..1), G and B (8 bits each) -> depth = R + G/255 + B/65025
  (to be confirmed: NDC z or linear). It's readable, unlike the d24s8 depth-stencil.
- #296a8: normals G-buffer. #2972a: blue-only mask (one character + floor).
- #29774 / #29718: scene colour before / after fog and light shafts; #29718 feeds the tonemap.
- **No velocity buffer**: no target holds motion; the 97 draws after the scene don't change the
  depth target (snapshots 1, 6, 8 identical). RE6's motion blur must be camera-only. DLAA's motion
  vectors: camera motion from depth + current/previous view-projection (static scene correct;
  characters' own motion missing -> possible ghosting, revisit later).
- **Camera view-projection = VS c1..c4** in every scene draw, row-vector layout (translation in
  c4; z and w columns nearly equal), e.g. c1 (1.244 0.297 0.444 0.444), c2 (0 2.406 -0.259 -0.259),
  c3 (0.644 -0.573 -0.858 -0.858), c4 (95.4 205.0 233.2 249.2). c5..c7 = object world matrix.
  c6/c8 = (1/3840, 1/2160): D3D9 half-pixel offset.
- **Jitter plan**: hook IDirect3DDevice9::SetVertexShaderConstantF (ReShade's push_constants event
  is read-only) and, while a back-buffer-sized target is bound, add jitter to c1..c4's x and y
  columns (col_x += jx * col_w). Keep each frame's unjittered matrix for motion vectors.

## Feasibility (all inputs available)

Colour (#52c30 path, rgba16f), depth (packed RGB, decode), motion (camera, from depth + VP
history), jitter (VS c1..c4 hook), DLSS in a 64-bit helper via D3D9Ex shared textures.
In-game AA should be off (FXAA runs after the tonemap). Next: helper proof of concept.
