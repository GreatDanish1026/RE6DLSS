// ---------------------------------------------------------------- jitter
//
// Included into main.cpp's anonymous namespace. RE6 has no TAA, so DLAA's sub-pixel jitter is
// added here. Scene vertex shaders place vertices with the camera's view-projection `fViewProj`
// (row-vector layout: clip = (x, y, z, 1) * [r0; r1; r2; r3]), in c1..c4 for most shaders but in
// c23..c26 for the shadow-receiving character shaders used in cutscenes. Each vertex shader's
// constant table (CTAB, kept in its bytecode) gives its fViewProj register. Just before each draw
// into a back-buffer-sized target, a jittered copy of the game's matrix is written to that
// register on the device (clip.x += jx * clip.w, so each row's x += jx * its w, y likewise).
// Every shader that uses fViewProj is jittered, including world-space quads, so depth pre-pass
// and colour pass always agree (a mismatch fails the colour pass's depth test: black shards).
// The draw-time part is NativeJitter in objects.inl: it works from the game's own constant values
// (tracked as the game sets them), so the jitter never accumulates, and the game's values go back
// before any draw that doesn't use that register as fViewProj. This file: the offset sequence.

int g_jitterMode = 0;          // 0 off, 1 on, 2 test x16 (ReShade.ini [RE6DLSS] Jitter)
uint32_t g_jitterIndex = 0;    // frame counter for the sequence
float g_jitterX = 0, g_jitterY = 0;  // this frame's offset in pixels (-0.5..0.5)
bool g_pastTonemap = false;    // set at the tonemap draw, cleared at present
uint32_t g_jitteredDraws = 0, g_jitteredDrawsLast = 0;
float g_frameVp[16] = {}, g_prevVp[16] = {};  // the game's (unjittered) view-projection
bool g_haveFrameVp = false, g_havePrevVp = false;
constexpr uint32_t kJitterPhases = 16;

float Halton(uint32_t i, uint32_t base) {
    float f = 1, r = 0;
    for (; i; i /= base) {
        f /= base;
        r += f * (i % base);
    }
    return r;
}

void NextJitter() {  // at present
    if (g_jitterIndex % 1200 == 600)
        Log("jitter %s: %u scene draws jittered last frame, camera matrix %s, offset %.3f %.3f", g_jitterMode ? "on" : "off",
            g_jitteredDraws, g_haveFrameVp ? "found" : "not found", g_jitterX, g_jitterY);
    g_jitteredDrawsLast = g_jitteredDraws;
    g_jitteredDraws = 0;
    g_pastTonemap = false;
    if (g_haveFrameVp) {
        memcpy(g_prevVp, g_frameVp, sizeof(g_prevVp));
        g_havePrevVp = true;
    }
    g_haveFrameVp = false;
    const uint32_t i = (g_jitterIndex++ % kJitterPhases) + 1;
    const float scale = g_jitterMode == 2 ? 16.0f : 1.0f;  // 2 = test: 16x larger, plainly visible shake
    g_jitterX = g_jitterMode ? (Halton(i, 2) - 0.5f) * scale : 0.0f;
    g_jitterY = g_jitterMode ? (Halton(i, 3) - 0.5f) * scale : 0.0f;
}

// A perspective view-projection: z column = A * w column in rows 0..2 (z_clip = A * z_view + B,
// w_clip = z_view), any near/far planes. Used to pick the frame's camera for motion vectors.
bool LooksLikeViewProj(const float* c) {
    float maxW = 0;
    int ref = 0;
    for (int r = 0; r < 3; ++r) {
        if (!std::isfinite(c[r * 4 + 2]) || !std::isfinite(c[r * 4 + 3])) return false;
        if (std::fabs(c[r * 4 + 3]) > maxW) { maxW = std::fabs(c[r * 4 + 3]); ref = r; }
    }
    if (maxW < 1e-6f || c[15] == 0.0f) return false;
    const float a = c[ref * 4 + 2] / c[ref * 4 + 3];
    if (a < 0.5f || a > 1.5f) return false;
    for (int r = 0; r < 3; ++r)
        if (std::fabs(c[r * 4 + 2] - a * c[r * 4 + 3]) > 1e-3f * maxW) return false;
    return true;
}

