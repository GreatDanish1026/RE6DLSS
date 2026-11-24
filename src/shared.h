// The control block shared by RE6DLSS.addon32 (inside the 32-bit game) and RE6DLSS-helper64.exe.
// Named file mapping "RE6DLSS_<game pid>_ctl"; events "RE6DLSS_<game pid>_go" (add-on -> helper:
// a frame is in the input textures) and "RE6DLSS_<game pid>_done" (helper -> add-on: the output
// texture is written and the GPU work is finished).
#pragma once
#include <cstdint>

#define RD_SHARED_VERSION 4u

enum : uint32_t { kModeCopy = 0, kModeInvert = 1, kModeDlaa = 2 };
enum : uint32_t { kObjMotion = 1, kObjMask = 2 };

struct SharedControl {
    uint32_t version;     // RD_SHARED_VERSION
    uint32_t generation;  // bumped by the add-on when the textures below change
    uint64_t inHandle;    // D3D9Ex legacy shared handles (global values; valid in any process)
    uint64_t outHandle;
    uint64_t depthHandle;  // the game's packed depth (pre-pass target: R + G/255 + B/65025)
    uint32_t width, height;
    uint32_t format;      // DXGI format of the colour textures (and the depth copy)
    uint32_t mode;        // kMode*
    // per frame (add-on -> helper)
    float jitterX, jitterY;  // pixels, the offset this frame was rendered with (projection x += 2jx/W)
    float viewProj[16];      // the game's view-projection, unjittered (row-vector: clip = v * M)
    float prevViewProj[16];  // last frame's
    uint32_t havePrev;       // prevViewProj is valid
    uint32_t reset;          // drop DLSS history (first frame, camera cut, settings change)
    int32_t preset;          // DLSS model: -1 automatic, else NVSDK_NGX_DLSS_Hint_Render_Preset_*
    // helper -> add-on
    uint32_t helperReady;    // generation the helper has opened (0 = none)
    uint32_t helperFrames;   // frames processed
    int32_t helperError;     // last HRESULT failure, 0 = none
    float helperGpuMs;       // last frame's helper work, CPU-measured including the GPU wait
    char helperStatus[160];
    uint32_t helperAlive;    // 1 once the helper's D3D11 device is ready (frames before that are skipped)
    // version 4: moving objects and effects (add-on -> helper)
    uint64_t objHandle;      // A16B16G16R16F: rg object motion in pixels (previous - current), b 1 = valid, a effect mask
    uint32_t objFlags;       // kObj*
    uint32_t debugView;      // 0 off, 1 motion vectors, 2 effect mask (shown instead of the picture)
    uint32_t async;          // 1: answer once the work is submitted (GPU order kept by the OS), not when done
    uint32_t checkMarker;    // non-zero: copy the input's top-left pixel to the output's (order check)
};
