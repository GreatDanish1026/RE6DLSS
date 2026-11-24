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
// The jitter is computed from the game's own values (tracked from its constant pushes; our writes
// go to the native device and raise no events), so it never accumulates, and the game's values
// are put back before any draw that doesn't use that register as fViewProj.

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

// The fViewProj register of a vertex shader, from its constant table; -1 if it has none.
int FindViewProjRegister(IDirect3DVertexShader9* vs, UINT& size) {
    size = 0;
    if (FAILED(vs->GetFunction(nullptr, &size)) || size < 8 || size > (1u << 20)) return -1;
    std::vector<uint32_t> code(size / 4);
    UINT got = size;
    if (FAILED(vs->GetFunction(code.data(), &got))) return -1;
    for (size_t i = 1; i < code.size(); ++i) {
        const uint32_t tok = code[i];
        if ((tok & 0xffff) != 0xfffe) continue;  // comment token, length in dwords in bits 16..30
        const size_t len = (tok >> 16) & 0x7fff;
        if (i + 1 + len > code.size() || len < 8 || code[i + 1] != 0x42415443) {  // not 'CTAB'
            i += len;
            continue;
        }
        const auto* base = reinterpret_cast<const uint8_t*>(&code[i + 2]);
        const size_t bytes = (len - 1) * 4;
        const auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= bytes) memcpy(&v, base + o, 4); return v; };
        const auto u16 = [&](size_t o) { uint16_t v = 0; if (o + 2 <= bytes) memcpy(&v, base + o, 2); return v; };
        const uint32_t n = u32(12), info = u32(16);  // D3DXSHADER_CONSTANTTABLE: Constants, ConstantInfo
        for (uint32_t k = 0; k < n && k < 256; ++k) {
            const size_t ci = info + k * 20;  // D3DXSHADER_CONSTANTINFO
            const uint32_t name = u32(ci);
            const uint16_t set = u16(ci + 4), reg = u16(ci + 6), cnt = u16(ci + 8);
            static const char kName[] = "fViewProj";
            if (set != 2 || cnt < 4 || reg > 252 || name + sizeof(kName) > bytes) continue;  // float4 registers
            if (memcmp(base + name, kName, sizeof(kName)) == 0) return reg;
        }
        return -1;
    }
    return -1;
}

struct VsInfo {
    UINT size = 0;
    int vpReg = -1;
};
std::unordered_map<uintptr_t, VsInfo> g_vsInfo;  // render thread only
uint32_t g_vsOffC1 = 0;                          // shaders seen with fViewProj elsewhere than c1

int ViewProjRegister(IDirect3DDevice9* dev) {
    IDirect3DVertexShader9* vs = nullptr;
    if (FAILED(dev->GetVertexShader(&vs)) || !vs) return -1;
    VsInfo& info = g_vsInfo[reinterpret_cast<uintptr_t>(vs)];
    if (!info.size) {  // new shader (the game keeps its shaders, so known by address)
        info.vpReg = FindViewProjRegister(vs, info.size);
        if (info.vpReg > 1 && ++g_vsOffC1 <= 8) Log("vertex shader %p: fViewProj in c%d", vs, info.vpReg);
    }
    vs->Release();
    return info.vpReg;
}

// Called from the draw events, before the game's draw runs.
void MaybeJitter(command_list* cl) {
    if (!g_bbWidth) return;
    auto* dev = reinterpret_cast<IDirect3DDevice9*>(cl->get_native());
    int restoreReg = -1, applyReg = -1;
    float restore[16], vp[16];
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        Context& c = Ctx(cl);
        const bool scene = c.sceneTarget && !g_pastTonemap;
        if (!scene && c.jitteredReg < 0) return;
        const int reg = scene && g_jitterMode ? ViewProjRegister(dev) : -1;
        if (reg >= 0) {
            memcpy(vp, c.vsConst + reg * 4, sizeof(vp));
            bool finite = true;
            for (float v : vp) finite = finite && std::isfinite(v);
            if (finite && vp[15] != 0.0f) applyReg = reg;
            if (applyReg >= 0 && !g_haveFrameVp && LooksLikeViewProj(vp)) {  // the frame's camera, for motion vectors
                memcpy(g_frameVp, vp, sizeof(vp));
                g_haveFrameVp = true;
            }
        } else if (scene && !g_jitterMode && !g_haveFrameVp && LooksLikeViewProj(c.vsConst + 4)) {
            memcpy(g_frameVp, c.vsConst + 4, sizeof(g_frameVp));  // jitter off: camera still tracked
            g_haveFrameVp = true;
        }
        if (c.jitteredReg >= 0 && c.jitteredReg != applyReg) {  // put the game's values back
            restoreReg = c.jitteredReg;
            memcpy(restore, c.vsConst + restoreReg * 4, sizeof(restore));
        }
        c.jitteredReg = applyReg;
    }
    if (restoreReg >= 0) dev->SetVertexShaderConstantF(restoreReg, restore, 4);
    if (applyReg < 0) return;
    const float jx = 2.0f * g_jitterX / g_bbWidth, jy = -2.0f * g_jitterY / g_bbHeight;
    for (int r = 0; r < 4; ++r) {
        vp[r * 4 + 0] += jx * vp[r * 4 + 3];
        vp[r * 4 + 1] += jy * vp[r * 4 + 3];
    }
    ++g_jitteredDraws;
    dev->SetVertexShaderConstantF(applyReg, vp, 4);
}
