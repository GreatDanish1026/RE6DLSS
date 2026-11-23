// The control block shared by RE6DLSS.addon32 (inside the 32-bit game) and RE6DLSS-helper64.exe.
// Named file mapping "RE6DLSS_<game pid>_ctl"; events "RE6DLSS_<game pid>_go" (add-on -> helper:
// a frame is in the input texture) and "RE6DLSS_<game pid>_done" (helper -> add-on: the output
// texture is written and the GPU work is finished).
#pragma once
#include <cstdint>

#define RD_SHARED_VERSION 2u

struct SharedControl {
    uint32_t version;     // RD_SHARED_VERSION
    uint32_t generation;  // bumped by the add-on when the textures below change
    uint64_t inHandle;    // D3D9Ex legacy shared handles (global values; valid in any process)
    uint64_t outHandle;
    uint32_t width, height;
    uint32_t format;      // DXGI format of both textures
    uint32_t mode;        // 0 = copy, 1 = invert (proof of concept)
    // helper -> add-on
    uint32_t helperReady;    // generation the helper has opened (0 = none)
    uint32_t helperFrames;   // frames processed
    int32_t helperError;     // last HRESULT failure, 0 = none
    float helperGpuMs;       // last frame's helper work, CPU-measured including the GPU wait
    char helperStatus[128];
    uint32_t helperAlive;  // 1 once the helper's D3D11 device is ready (frames before that are skipped)
};
