// ---------------------------------------------------------------- jitter
//
// Included into main.cpp's anonymous namespace. RE6 has no TAA, so DLAA's sub-pixel jitter is
// added here. Every scene draw has the camera's view-projection in vertex shader constants c1..c4
// (row-vector layout: clip = (x, y, z, 1) * [c1; c2; c3; c4]). Just before each geometry draw into
// a back-buffer-sized target, and before the tonemap draw, a jittered copy of the game's c1..c4 is
// written to the device (clip.x += jx * clip.w, so each register's x += jx * its w, y likewise).
// The jitter is computed from the game's own values (tracked from its constant pushes; our writes
// go to the native device and raise no events), so it never accumulates; full-screen passes in the
// scene get the game's values back. c1..c3 must look like a view-projection (z ~ w: projection with
// a far plane at infinity-ish), otherwise nothing is changed.

int g_jitterMode = 0;          // 0 off, 1 on (ReShade.ini [RE6DLSS] Jitter)
uint32_t g_jitterIndex = 0;    // frame counter for the sequence
float g_jitterX = 0, g_jitterY = 0;  // this frame's offset in pixels (-0.5..0.5)
bool g_pastTonemap = false;    // set at the tonemap draw, cleared at present
uint32_t g_jitteredDraws = 0, g_jitteredDrawsLast = 0;
float g_frameVp[16] = {}, g_prevVp[16] = {};  // the game's (unjittered) view-projection, c1..c4
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
    g_jitterX = g_jitterMode ? Halton(i, 2) - 0.5f : 0.0f;
    g_jitterY = g_jitterMode ? Halton(i, 3) - 0.5f : 0.0f;
}

bool LooksLikeViewProj(const float* c) {  // c = registers c1..c4 (16 floats)
    for (int r = 0; r < 3; ++r) {
        const float z = c[r * 4 + 2], w = c[r * 4 + 3];
        if (std::fabs(z - w) > 0.01f * std::fabs(w) + 1e-4f) return false;
    }
    return c[15] != 0.0f;
}

// Called from the draw events, before the game's draw runs. `fullscreen`: a quad (post/lighting).
void MaybeJitter(command_list* cl, bool fullscreen) {
    if (g_pastTonemap || !g_bbWidth) return;
    float vp[16];
    bool apply;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        Context& c = Ctx(cl);
        if (!c.sceneTarget) return;
        memcpy(vp, c.vsConst + 4, sizeof(vp));  // c1..c4
        if (!LooksLikeViewProj(vp)) return;
        if (!fullscreen && !g_haveFrameVp) {  // the frame's camera, for motion vectors
            memcpy(g_frameVp, vp, sizeof(vp));
            g_haveFrameVp = true;
        }
        apply = g_jitterMode && !fullscreen;
        if (!apply && !c.jittered) return;  // nothing to change or put back
        c.jittered = apply;
    }
    if (apply) {
        const float jx = 2.0f * g_jitterX / g_bbWidth, jy = -2.0f * g_jitterY / g_bbHeight;
        for (int r = 0; r < 4; ++r) {
            vp[r * 4 + 0] += jx * vp[r * 4 + 3];
            vp[r * 4 + 1] += jy * vp[r * 4 + 3];
        }
        ++g_jitteredDraws;
    }
    reinterpret_cast<IDirect3DDevice9*>(cl->get_native())->SetVertexShaderConstantF(1, vp, 4);
}
