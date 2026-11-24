// ---------------------------------------------------------------- moving objects
//
// Included into main.cpp's anonymous namespace after jitter.inl and roundtrip.inl.
//
// RE6 has no velocity buffer, so the helper computes camera motion from depth. Things that move by
// themselves (characters, enemies, doors) get per-object motion here, and blended effects (fire,
// smoke, sparks) a "trust this frame" mask for DLSS:
//
// - The game's native D3D9 draw calls are hooked (device vtable slots 81/82; something replaces
//   them after device creation, so the hook is put back on top at every present).
// - Object motion: in the scene's colour pass (back-buffer-sized target, not the depth pre-pass),
//   each draw of a skinned mesh (bones in the vertex texture tMatrixMap, character offset in
//   CBSkinning__packed0) or of a rigid mesh whose fWorld changed since last frame is drawn once
//   more into the shared object texture, with a patched copy of its vertex shader: the original
//   code runs twice, first with last frame's camera, world matrix, bone offset and bone texture
//   (spare constants c248..c255 and a spare vertex sampler), keeping that position in a spare
//   output, then as the game wrote it. A small pixel shader turns the two positions into a motion
//   vector in pixels (rg, b = 1 marks it valid). Depth test less-equal against the finished
//   pre-pass depth keeps only the visible surface.
// - Effect mask: blended draws that don't write depth are drawn once more with the game's own
//   shaders into the object texture's alpha channel (blend max): their alpha is the mask.
// - Last frame's bones: the bone texture (1024x64 float, one per frame) is copied at every present.
// All replays restore every state they touch, below ReShade (native calls), so neither ReShade nor
// the game sees them.

struct ShaderConst {
    std::string name;
    uint16_t set = 0, reg = 0, cnt = 0;  // set: 0 bool, 1 int, 2 float4, 3 sampler
};

void ParseCtab(const std::vector<uint32_t>& code, std::vector<ShaderConst>& out) {
    for (size_t i = 1; i < code.size(); ++i) {
        const uint32_t tok = code[i];
        if ((tok & 0xffff) != 0xfffe) continue;
        const size_t len = (tok >> 16) & 0x7fff;
        if (i + 1 + len > code.size() || len < 8 || code[i + 1] != 0x42415443) {
            i += len;
            continue;
        }
        const auto* base = reinterpret_cast<const uint8_t*>(&code[i + 2]);
        const size_t bytes = (len - 1) * 4;
        const auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= bytes) memcpy(&v, base + o, 4); return v; };
        const auto u16 = [&](size_t o) { uint16_t v = 0; if (o + 2 <= bytes) memcpy(&v, base + o, 2); return v; };
        const uint32_t n = u32(12), info = u32(16);
        for (uint32_t k = 0; k < n && k < 256; ++k) {
            const size_t ci = info + k * 20;
            ShaderConst c;
            for (size_t p = u32(ci); p < bytes && base[p] && c.name.size() < 64; ++p) c.name += static_cast<char>(base[p]);
            c.set = u16(ci + 4), c.reg = u16(ci + 6), c.cnt = u16(ci + 8);
            out.push_back(std::move(c));
        }
        return;
    }
}

// ---- vs_3_0 bytecode
uint32_t RegType(uint32_t t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }
uint32_t WithReg(uint32_t t, uint32_t type, uint32_t num) {
    return (t & ~((7u << 28) | (3u << 11) | 0x7ffu)) | ((type & 7) << 28) | (((type >> 3) & 3) << 11) | (num & 0x7ff);
}
enum : uint32_t { kRegTemp = 0, kRegConst = 2, kRegOutput = 6, kRegSampler = 10 };
constexpr int kSpareConst = 248;  // c248..c255: prev view-proj (4), prev world (3), prev skin offset (1)

bool NoDest(uint32_t op) {
    switch (op) {
        case 0: case 25: case 26: case 27: case 28: case 29: case 30: case 38: case 39:  // nop call callnz loop ret endloop label rep endrep
        case 40: case 41: case 42: case 43: case 44: case 45: case 96: case 65:          // if ifc else endif break breakc breakp texkill
            return true;
        default: return false;
    }
}

struct VsShape {
    bool sm3 = false, subroutines = false;
    int maxTemp = -1, posOut = -1, maxConst = -1;
    uint32_t outputs = 0, samplers = 0;
};

VsShape AnalyzeVs(const std::vector<uint32_t>& code, const std::vector<ShaderConst>& consts) {
    VsShape s;
    for (const ShaderConst& c : consts)
        if (c.set == 2) s.maxConst = std::max(s.maxConst, c.reg + c.cnt - 1);
    if (code.empty() || (code[0] & 0xffff0000) != 0xfffe0000) return s;
    s.sm3 = (code[0] & 0xffff) == 0x0300;
    for (size_t i = 1; i < code.size();) {
        const uint32_t tok = code[i];
        if (tok == 0x0000ffff) break;
        const uint32_t op = tok & 0xffff;
        if (op == 0xfffe) {
            i += 1 + ((tok >> 16) & 0x7fff);
            continue;
        }
        const size_t len = (tok >> 24) & 0xf;
        if (i + 1 + len > code.size()) break;
        const uint32_t* p = &code[i + 1];
        if (op == 31 && len >= 2) {  // dcl
            const uint32_t type = RegType(p[1]), n = p[1] & 0x7ff;
            if (type == kRegOutput && n < 32) {
                s.outputs |= 1u << n;
                if ((p[0] & 0x1f) == 0 && ((p[0] >> 16) & 0xf) == 0) s.posOut = static_cast<int>(n);
            }
            if (type == kRegSampler && n < 32) s.samplers |= 1u << n;
        } else if (op == 81 || op == 48 || op == 47) {  // def / defi / defb
            if (len >= 1 && RegType(p[0]) == kRegConst) s.maxConst = std::max(s.maxConst, static_cast<int>(p[0] & 0x7ff));
        } else {
            if (op == 25 || op == 26 || op == 30) s.subroutines = true;
            for (size_t k = 0; k < len; ++k)
                if ((p[k] & 0x80000000u) && RegType(p[k]) == kRegTemp) s.maxTemp = std::max(s.maxTemp, static_cast<int>(p[k] & 0x7ff));
        }
        i += 1 + len;
    }
    return s;
}

struct ObjVs {
    UINT size = 0;
    int vpReg = -1, worldReg = -1, boneSampler = -1, skinReg = -1;
    VsShape shape;
    std::vector<uint32_t> code;
    bool patchTried = false;
    bool usesIntBool = false;  // int/bool constants (loops, branches)
    IDirect3DVertexShader9* patched = nullptr;
    int spareSampler = -1;
};

// The patched shader (see the top of this file). False if the shader doesn't fit the pattern.
bool PatchVs(const ObjVs& o, std::vector<uint32_t>& out, int& spareSampler, std::string& why) {
    const std::vector<uint32_t>& code = o.code;
    const VsShape& s = o.shape;
    if (!s.sm3) return why = "not vs_3_0", false;
    if (s.subroutines) return why = "subroutines", false;
    if (s.posOut < 0) return why = "no position output", false;
    if (s.maxTemp + 2 > 31) return why = "no spare temporaries", false;
    if (s.maxConst >= kSpareConst) return why = "no spare constants", false;
    int outReg = -1;
    for (int n = 0; n < 12 && outReg < 0; ++n)
        if (!(s.outputs & (1u << n))) outReg = n;
    if (outReg < 0) return why = "no spare output", false;
    spareSampler = -1;
    if (o.boneSampler >= 0) {
        for (int n = 3; n >= 0 && spareSampler < 0; --n)
            if (!(s.samplers & (1u << n))) spareSampler = n;
        if (spareSampler < 0) return why = "no spare vertex sampler", false;
    }
    const uint32_t rPrev = static_cast<uint32_t>(s.maxTemp + 1), rScratch = static_cast<uint32_t>(s.maxTemp + 2);

    std::vector<uint32_t> prologue, body;
    for (size_t i = 1; i < code.size();) {
        const uint32_t tok = code[i];
        if (tok == 0x0000ffff) break;
        const uint32_t op = tok & 0xffff;
        if (op == 0xfffe) {  // comments (constant table): dropped
            i += 1 + ((tok >> 16) & 0x7fff);
            continue;
        }
        const size_t len = (tok >> 24) & 0xf;
        if (i + 1 + len > code.size()) return why = "truncated", false;
        if (op == 28) break;  // ret: end of main
        std::vector<uint32_t>& dst = (op == 31 || op == 81 || op == 48 || op == 47) ? prologue : body;
        dst.insert(dst.end(), code.begin() + i, code.begin() + i + 1 + len);
        i += 1 + len;
    }

    // Copy 1: last frame's inputs, outputs into temporaries.
    std::vector<uint32_t> copy1 = body;
    for (size_t i = 0; i < copy1.size();) {
        const uint32_t op = copy1[i] & 0xffff;
        const size_t len = (copy1[i] >> 24) & 0xf;
        bool dest = !NoDest(op), rel = false;
        for (size_t k = i + 1; k <= i + len; ++k) {
            uint32_t& t = copy1[k];
            if (rel) {  // relative-address token of the previous register
                rel = false;
                continue;
            }
            if (!(t & 0x80000000u)) continue;
            const uint32_t type = RegType(t), n = t & 0x7ff;
            const bool relative = (t & 0x2000) != 0;
            if (dest) {
                dest = false;
                if (type == kRegOutput) {
                    if (relative) return why = "relative output", false;
                    t = WithReg(t, kRegTemp, n == static_cast<uint32_t>(s.posOut) ? rPrev : rScratch);
                }
            } else if (type == kRegConst) {
                int to = -1;
                if (o.vpReg >= 0 && n >= static_cast<uint32_t>(o.vpReg) && n < static_cast<uint32_t>(o.vpReg) + 4) to = kSpareConst + (n - o.vpReg);
                else if (o.worldReg >= 0 && n >= static_cast<uint32_t>(o.worldReg) && n < static_cast<uint32_t>(o.worldReg) + 3)
                    to = kSpareConst + 4 + (n - o.worldReg);
                else if (o.skinReg >= 0 && n == static_cast<uint32_t>(o.skinReg)) to = kSpareConst + 7;
                if (to >= 0) {
                    if (relative) return why = "relative camera/world constant", false;
                    t = WithReg(t, kRegConst, static_cast<uint32_t>(to));
                }
            } else if (type == kRegSampler && o.boneSampler >= 0 && n == static_cast<uint32_t>(o.boneSampler)) {
                t = WithReg(t, kRegSampler, static_cast<uint32_t>(spareSampler));
            }
            if (relative) rel = true;
        }
        i += 1 + len;
    }

    out.clear();
    out.push_back(code[0]);
    out.insert(out.end(), prologue.begin(), prologue.end());
    out.push_back(0x0200001f);                                   // dcl_texcoord15 oN
    out.push_back(0x80000000u | 5u | (15u << 16));
    out.push_back(WithReg(0x80000000u | (0xfu << 16), kRegOutput, static_cast<uint32_t>(outReg)));
    if (spareSampler >= 0) {                                    // dcl_2d sN
        out.push_back(0x0200001f);
        out.push_back(0x80000000u | (2u << 27));
        out.push_back(WithReg(0x80000000u | (0xfu << 16), kRegSampler, static_cast<uint32_t>(spareSampler)));
    }
    out.insert(out.end(), copy1.begin(), copy1.end());
    out.insert(out.end(), body.begin(), body.end());
    out.push_back(0x02000001);                                   // mov oN, rPrev
    out.push_back(WithReg(0x80000000u | (0xfu << 16), kRegOutput, static_cast<uint32_t>(outReg)));
    out.push_back(WithReg(0x80000000u | (0xe4u << 16), kRegTemp, rPrev));
    out.push_back(0x0000ffff);
    return true;
}

std::unordered_map<uintptr_t, ObjVs> g_objVs;  // render thread

ObjVs& ObjVsInfo(IDirect3DVertexShader9* vs) {
    auto it = g_objVs.find(reinterpret_cast<uintptr_t>(vs));
    if (it != g_objVs.end() && it->second.size) return it->second;  // the game keeps its shaders: known by address
    UINT size = 0;
    vs->GetFunction(nullptr, &size);
    ObjVs& o = g_objVs[reinterpret_cast<uintptr_t>(vs)];
    if (o.size == size) return o;
    if (o.patched) o.patched->Release();
    o = ObjVs{};
    o.size = size;
    if (size < 8 || size > (1u << 20)) return o;
    std::vector<uint32_t> code(size / 4);
    UINT got = size;
    if (FAILED(vs->GetFunction(code.data(), &got))) return o;
    std::vector<ShaderConst> cs;
    ParseCtab(code, cs);
    for (const ShaderConst& c : cs) {
        if (c.set == 2 && c.cnt >= 4 && c.name == "fViewProj") o.vpReg = c.reg;
        else if (c.set == 2 && c.cnt >= 3 && c.name == "fWorld") o.worldReg = c.reg;
        else if (c.set == 3 && c.name.find("tMatrixMap") != std::string::npos) o.boneSampler = c.reg;
        else if (c.set == 2 && c.name == "CBSkinning__packed0") o.skinReg = c.reg;
    }
    o.shape = AnalyzeVs(code, cs);
    for (const ShaderConst& c : cs)
        if (c.set == 0 || c.set == 1) o.usesIntBool = true;
    if (o.vpReg >= 0 && (o.worldReg >= 0 || o.boneSampler >= 0)) o.code = std::move(code);
    return o;
}

// ---- state
IDirect3DTexture9* g_prevBones = nullptr;          // last frame's bone texture
IDirect3DSurface9* g_prevBonesSurf = nullptr;
bool g_prevBonesValid = false;
IDirect3DTexture9* g_boneTexThisFrame = nullptr;   // the bone texture seen this frame (AddRef'd)
IDirect3DPixelShader9 *g_motionPs = nullptr, *g_copyPs = nullptr;
IDirect3DVertexShader9* g_copyVs = nullptr;
IDirect3DVertexDeclaration9* g_copyDecl = nullptr;
IDirect3DStateBlock9* g_copyState = nullptr;
bool g_objShadersTried = false, g_objShadersOk = false;
bool g_objWritten = false, g_maskWritten = false;  // this frame (sent to the helper)
std::string g_objStatus = "not started";

struct ObjStats {
    uint32_t motionDraws = 0, maskDraws = 0, skipped = 0, failedPatch = 0, patched = 0;
    double cpuMs = 0;  // time spent in OnSceneDraw (replays included) this frame
} g_objStats, g_objStatsLast;
struct TargetStats {
    uint32_t order = 0, draws = 0, skinned = 0, rigid = 0, motion = 0, mask = 0;
    bool depth = false;
};
std::map<uintptr_t, TargetStats> g_targetStats;  // per render target 0 texture, this frame
uint32_t g_targetOrder = 0;
// The scene has two colour passes; a character part can be drawn in either or both. Each moving
// draw is replayed once per frame: same geometry with the same bone offset / world matrix = done.
std::unordered_set<uint64_t> g_replayed;

uint64_t Fnv(uint64_t h, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
uint32_t g_patchFailLogs = 0;

using D3DCompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT,
                                      UINT, ID3DBlob**, ID3DBlob**);

const char kObjShaders[] = R"(
float4 motionC : register(c0);  // width, height, jitter x, jitter y (pixels)
struct MotionIn { float4 prev : TEXCOORD15; float2 vpos : VPOS; };
float4 motion(MotionIn i) : COLOR0 {
    if (i.prev.w <= 1e-6) return 0;
    float2 ndc = i.prev.xy / i.prev.w;
    float2 prevPix = float2((ndc.x * 0.5 + 0.5) * motionC.x, (0.5 - ndc.y * 0.5) * motionC.y);
    float2 mv = prevPix - (i.vpos - motionC.zw);  // previous - current (current without jitter)
    if (any(abs(mv) > motionC.xy)) return 0;
    return float4(mv, 1, 0);
}
float4 copyC : register(c0);  // 1 / width, 1 / height
sampler2D copySrc : register(s0);
float4 copyVs(float4 p : POSITION) : POSITION { return p; }
float4 copyPs(float2 vpos : VPOS) : COLOR0 { return tex2Dlod(copySrc, float4((vpos + 0.5) * copyC.xy, 0, 0)); }
)";

bool EnsureObjShaders() {
    if (g_objShadersTried) return g_objShadersOk;
    g_objShadersTried = true;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? reinterpret_cast<D3DCompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!compile) {
        g_objStatus = "d3dcompiler_47.dll not found";
        Log("objects: %s", g_objStatus.c_str());
        return false;
    }
    auto build = [&](const char* entry, const char* target, ID3DBlob** blob) {
        ID3DBlob* err = nullptr;
        const HRESULT hr = compile(kObjShaders, sizeof(kObjShaders) - 1, "RE6DLSS objects", nullptr, nullptr, entry, target,
                                   (1 << 15) /* optimization level 3 */, 0, blob, &err);
        if (FAILED(hr)) Log("objects: compiling %s failed: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        return SUCCEEDED(hr);
    };
    ID3DBlob *m = nullptr, *cv = nullptr, *cp = nullptr;
    bool ok = build("motion", "ps_3_0", &m) && build("copyVs", "vs_3_0", &cv) && build("copyPs", "ps_3_0", &cp);
    if (ok) ok = SUCCEEDED(g_d9->CreatePixelShader(static_cast<const DWORD*>(m->GetBufferPointer()), &g_motionPs)) &&
                 SUCCEEDED(g_d9->CreateVertexShader(static_cast<const DWORD*>(cv->GetBufferPointer()), &g_copyVs)) &&
                 SUCCEEDED(g_d9->CreatePixelShader(static_cast<const DWORD*>(cp->GetBufferPointer()), &g_copyPs));
    if (m) m->Release();
    if (cv) cv->Release();
    if (cp) cp->Release();
    const D3DVERTEXELEMENT9 decl[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    if (ok) ok = SUCCEEDED(g_d9->CreateVertexDeclaration(decl, &g_copyDecl)) && SUCCEEDED(g_d9->CreateStateBlock(D3DSBT_ALL, &g_copyState));
    g_objShadersOk = ok;
    g_objStatus = ok ? "ready" : "shader setup failed";
    Log("objects: shaders %s", ok ? "ready" : "failed");
    return ok;
}

bool ObjectsActive() {
    return g_rtMode == 3 && g_jitterMode && !g_rtFailed && g_objSurf && (g_objMotionOn || g_effectMaskOn) && g_d9 &&
           EnsureObjShaders();
}

// ---- per-draw history (colour pass), for last frame's world matrix and bone offset
struct DrawKey {
    uintptr_t vb, ib;
    uint32_t a, b, c, n;  // start index / vertex, base vertex, primitive count, occurrence this frame
    bool operator==(const DrawKey& o) const { return vb == o.vb && ib == o.ib && a == o.a && b == o.b && c == o.c && n == o.n; }
};
struct DrawKeyHash {
    size_t operator()(const DrawKey& k) const {
        size_t h = k.vb * 31 + k.ib;
        for (uint32_t v : {k.a, k.b, k.c, k.n}) h = h * 1000003u ^ v;
        return h;
    }
};
struct DrawHist {
    float world[12];
    float skin[4];
};
std::unordered_map<DrawKey, DrawHist, DrawKeyHash> g_histPrev, g_histCur;
std::unordered_map<DrawKey, uint32_t, DrawKeyHash> g_occurrence;

using DipFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DpFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
DipFn g_dip = nullptr, g_dipFirst = nullptr;  // current next in chain, the one found at first hook
DpFn g_dp = nullptr, g_dpFirst = nullptr;
bool g_inDrawHook = false;  // render thread

struct DrawArgs {
    bool indexed;
    D3DPRIMITIVETYPE type;
    INT base;
    UINT minV, numV, start, prims;
};

HRESULT Redraw(IDirect3DDevice9* dev, const DrawArgs& d) {
    return d.indexed ? g_dip(dev, d.type, d.base, d.minV, d.numV, d.start, d.prims) : g_dp(dev, d.type, d.start, d.prims);
}

// Render targets 0-3 and the viewport (setting target 0 resets it), saved around a replay.
struct SavedTargets {
    IDirect3DSurface9* rt[4] = {};
    D3DVIEWPORT9 vp = {};
    void Save(IDirect3DDevice9* dev) {
        for (DWORD i = 0; i < 4; ++i)
            if (FAILED(dev->GetRenderTarget(i, &rt[i]))) rt[i] = nullptr;
        dev->GetViewport(&vp);
    }
    void Restore(IDirect3DDevice9* dev) {
        for (DWORD i = 0; i < 4; ++i) {
            dev->SetRenderTarget(i, rt[i]);
            if (rt[i]) rt[i]->Release(), rt[i] = nullptr;
        }
        dev->SetViewport(&vp);
    }
};

struct SavedStates {
    static constexpr D3DRENDERSTATETYPE kStates[] = {
        D3DRS_ALPHABLENDENABLE, D3DRS_COLORWRITEENABLE, D3DRS_ZWRITEENABLE,  D3DRS_ZFUNC,         D3DRS_ALPHATESTENABLE,
        D3DRS_SRGBWRITEENABLE,  D3DRS_STENCILENABLE,    D3DRS_SEPARATEALPHABLENDENABLE,          D3DRS_BLENDOPALPHA,
        D3DRS_SRCBLENDALPHA,    D3DRS_DESTBLENDALPHA,   D3DRS_ZENABLE};
    DWORD v[sizeof(kStates) / sizeof(kStates[0])] = {};
    void Save(IDirect3DDevice9* dev) {
        for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); ++i) dev->GetRenderState(kStates[i], &v[i]);
    }
    void Restore(IDirect3DDevice9* dev) {
        for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); ++i) dev->SetRenderState(kStates[i], v[i]);
    }
};

bool EnsurePatched(IDirect3DDevice9* dev, IDirect3DVertexShader9* gameVs, ObjVs& o) {
    if (!o.patchTried) {
        o.patchTried = true;
        std::vector<uint32_t> code;
        std::string why;
        if (PatchVs(o, code, o.spareSampler, why)) {
            const HRESULT hr = dev->CreateVertexShader(reinterpret_cast<const DWORD*>(code.data()), &o.patched);
            if (FAILED(hr)) why = Format("CreateVertexShader 0x%08lx", hr), o.patched = nullptr;
        }
        if (!o.patched) {
            ++g_objStats.failedPatch;
            if (++g_patchFailLogs <= 12) Log("objects: vertex shader %p not patched: %s", gameVs, why.c_str());
        } else {
            ++g_objStats.patched;
        }
    }
    return o.patched != nullptr;
}

// Motion replays are batched per pass: each moving draw is recorded (geometry, the device's vertex
// shader constants as drawn, i.e. with the jitter, bones) and the batch is drawn at the end of the
// pass, with the device state saved and restored once instead of per draw (render target
// switches are what makes a D3D9 replay expensive).
struct MotionItem {
    DrawArgs d;
    IDirect3DVertexShader9* vs;
    IDirect3DVertexDeclaration9* decl;
    IDirect3DIndexBuffer9* ib;
    IDirect3DVertexBuffer9* vb[4];
    UINT off[4], stride[4], freq[4];
    IDirect3DBaseTexture9* bones;
    int boneSampler, spareSampler;
    DWORD cull, depthBias, slopeBias;
    uint32_t nConst;
    size_t constOfs;  // into g_batchConsts
    bool intBool;
    int ints[64];
    BOOL bools[16];
    float spare[32];  // c248..c255: last frame's camera, world, bone offset
};
std::vector<MotionItem> g_batch;
std::vector<float> g_batchConsts;
uintptr_t g_batchTarget = 0;              // render target 0 surface of the batch's pass
IDirect3DSurface9* g_batchDs = nullptr;   // its depth buffer (AddRef'd)
uint32_t g_batches = 0;

template <typename T>
void SafeRelease(T*& p) {
    if (p) p->Release(), p = nullptr;
}

void RecordMotion(IDirect3DDevice9* dev, IDirect3DVertexShader9* gameVs, ObjVs& o, const DrawHist& prev, const DrawArgs& d,
                  uintptr_t target) {
    if (!EnsurePatched(dev, gameVs, o)) return;
    if (o.boneSampler >= 0 && !g_prevBonesValid) return;
    if (g_batch.empty()) {
        g_batchTarget = target;
        SafeRelease(g_batchDs);
        dev->GetDepthStencilSurface(&g_batchDs);
    }
    MotionItem m = {};
    m.d = d;
    m.vs = o.patched;
    m.vs->AddRef();
    dev->GetVertexDeclaration(&m.decl);
    if (d.indexed) dev->GetIndices(&m.ib);
    for (UINT s = 0; s < 4; ++s) {
        dev->GetStreamSource(s, &m.vb[s], &m.off[s], &m.stride[s]);
        dev->GetStreamSourceFreq(s, &m.freq[s]);
    }
    m.boneSampler = o.boneSampler, m.spareSampler = o.spareSampler;
    if (o.boneSampler >= 0) dev->GetTexture(D3DVERTEXTEXTURESAMPLER0 + static_cast<DWORD>(o.boneSampler), &m.bones);
    dev->GetRenderState(D3DRS_CULLMODE, &m.cull);
    dev->GetRenderState(D3DRS_DEPTHBIAS, &m.depthBias);
    dev->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &m.slopeBias);
    m.nConst = static_cast<uint32_t>(std::min(std::max(o.shape.maxConst + 1, 1), kSpareConst));
    m.constOfs = g_batchConsts.size();
    g_batchConsts.resize(m.constOfs + m.nConst * 4);
    dev->GetVertexShaderConstantF(0, &g_batchConsts[m.constOfs], m.nConst);
    m.intBool = o.usesIntBool;
    if (m.intBool) {
        dev->GetVertexShaderConstantI(0, m.ints, 16);
        dev->GetVertexShaderConstantB(0, m.bools, 16);
    }
    memcpy(m.spare, g_prevVp, sizeof(float) * 16);
    memcpy(m.spare + 16, prev.world, sizeof(prev.world));
    memcpy(m.spare + 28, prev.skin, sizeof(prev.skin));
    g_batch.push_back(m);
}

void FlushMotion(IDirect3DDevice9* dev) {
    if (g_batch.empty()) return;
    const bool wasInHook = g_inDrawHook;
    g_inDrawHook = true;  // our draws must not be taken for the game's
    // Save.
    SavedTargets targets;
    targets.Save(dev);
    IDirect3DSurface9* ds = nullptr;
    dev->GetDepthStencilSurface(&ds);
    SavedStates states;
    states.Save(dev);
    DWORD cull, bias, slope, scissor;
    dev->GetRenderState(D3DRS_CULLMODE, &cull);
    dev->GetRenderState(D3DRS_DEPTHBIAS, &bias);
    dev->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &slope);
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    dev->GetVertexShader(&vs);
    dev->GetPixelShader(&ps);
    dev->GetVertexDeclaration(&decl);
    dev->GetIndices(&ib);
    IDirect3DVertexBuffer9* vb[4] = {};
    UINT off[4] = {}, stride[4] = {}, freq[4] = {};
    for (UINT s = 0; s < 4; ++s) {
        dev->GetStreamSource(s, &vb[s], &off[s], &stride[s]);
        dev->GetStreamSourceFreq(s, &freq[s]);
    }
    static float vsF[256 * 4];
    int vsI[64];
    BOOL vsB[16];
    float psC0[4];
    dev->GetVertexShaderConstantF(0, vsF, 256);
    dev->GetVertexShaderConstantI(0, vsI, 16);
    dev->GetVertexShaderConstantB(0, vsB, 16);
    dev->GetPixelShaderConstantF(0, psC0, 1);
    IDirect3DBaseTexture9* vtex[4] = {};
    DWORD vsamp[4][5] = {};
    const D3DSAMPLERSTATETYPE kSamp[5] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV};
    for (DWORD i = 0; i < 4; ++i) {
        dev->GetTexture(D3DVERTEXTEXTURESAMPLER0 + i, &vtex[i]);
        for (int k = 0; k < 5; ++k) dev->GetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, kSamp[k], &vsamp[i][k]);
    }
    // Common state.
    dev->SetRenderTarget(0, g_objSurf);
    for (DWORD i = 1; i < 4; ++i) dev->SetRenderTarget(i, nullptr);
    dev->SetDepthStencilSurface(g_batchDs);
    D3DVIEWPORT9 full = {0, 0, g_bbWidth, g_bbHeight, 0.0f, 1.0f};
    dev->SetViewport(&full);
    dev->SetPixelShader(g_motionPs);
    const float c0[4] = {static_cast<float>(g_bbWidth), static_cast<float>(g_bbHeight), g_jitterX, g_jitterY};
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    for (DWORD i = 0; i < 4; ++i) {
        dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    // The batch.
    const MotionItem* last = nullptr;
    for (const MotionItem& m : g_batch) {
        if (!last || last->vs != m.vs) dev->SetVertexShader(m.vs);
        if (!last || last->decl != m.decl) dev->SetVertexDeclaration(m.decl);
        if (m.d.indexed && (!last || last->ib != m.ib)) dev->SetIndices(m.ib);
        for (UINT s = 0; s < 4; ++s) {
            if (!last || last->vb[s] != m.vb[s] || last->off[s] != m.off[s] || last->stride[s] != m.stride[s])
                dev->SetStreamSource(s, m.vb[s], m.off[s], m.stride[s]);
            if (!last || last->freq[s] != m.freq[s]) dev->SetStreamSourceFreq(s, m.freq[s]);
        }
        dev->SetVertexShaderConstantF(0, &g_batchConsts[m.constOfs], m.nConst);
        dev->SetVertexShaderConstantF(kSpareConst, m.spare, 8);
        if (m.intBool) {
            dev->SetVertexShaderConstantI(0, m.ints, 16);
            dev->SetVertexShaderConstantB(0, m.bools, 16);
        }
        if (m.boneSampler >= 0) dev->SetTexture(D3DVERTEXTEXTURESAMPLER0 + static_cast<DWORD>(m.boneSampler), m.bones);
        if (m.spareSampler >= 0) dev->SetTexture(D3DVERTEXTEXTURESAMPLER0 + static_cast<DWORD>(m.spareSampler), g_prevBones);
        if (!last || last->cull != m.cull) dev->SetRenderState(D3DRS_CULLMODE, m.cull);
        if (!last || last->depthBias != m.depthBias) dev->SetRenderState(D3DRS_DEPTHBIAS, m.depthBias);
        if (!last || last->slopeBias != m.slopeBias) dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, m.slopeBias);
        if (SUCCEEDED(Redraw(dev, m.d))) ++g_objStats.motionDraws, g_objWritten = true;
        last = &m;
    }
    // Restore.
    for (DWORD i = 0; i < 4; ++i) {
        dev->SetTexture(D3DVERTEXTEXTURESAMPLER0 + i, vtex[i]);
        for (int k = 0; k < 5; ++k) dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, kSamp[k], vsamp[i][k]);
        SafeRelease(vtex[i]);
    }
    dev->SetVertexShaderConstantF(0, vsF, 256);
    dev->SetVertexShaderConstantI(0, vsI, 16);
    dev->SetVertexShaderConstantB(0, vsB, 16);
    dev->SetPixelShaderConstantF(0, psC0, 1);
    for (UINT s = 0; s < 4; ++s) {
        dev->SetStreamSource(s, vb[s], off[s], stride[s]);
        dev->SetStreamSourceFreq(s, freq[s]);
        SafeRelease(vb[s]);
    }
    dev->SetIndices(ib);
    dev->SetVertexDeclaration(decl);
    dev->SetVertexShader(vs);
    dev->SetPixelShader(ps);
    SafeRelease(ib), SafeRelease(decl), SafeRelease(vs), SafeRelease(ps);
    dev->SetRenderState(D3DRS_CULLMODE, cull);
    dev->SetRenderState(D3DRS_DEPTHBIAS, bias);
    dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, slope);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
    states.Restore(dev);
    targets.Restore(dev);
    dev->SetDepthStencilSurface(ds);
    SafeRelease(ds);
    for (MotionItem& m : g_batch) {
        SafeRelease(m.vs), SafeRelease(m.decl), SafeRelease(m.ib), SafeRelease(m.bones);
        for (auto*& v : m.vb) SafeRelease(v);
    }
    g_batch.clear();
    g_batchConsts.clear();
    SafeRelease(g_batchDs);
    g_batchTarget = 0;
    ++g_batches;
    g_inDrawHook = wasInHook;
}

void ReplayMask(IDirect3DDevice9* dev, const DrawArgs& d) {
    SavedTargets targets;
    targets.Save(dev);
    SavedStates states;
    states.Save(dev);
    dev->SetRenderTarget(0, g_objSurf);
    for (DWORD i = 1; i < 4; ++i) dev->SetRenderTarget(i, nullptr);
    dev->SetViewport(&targets.vp);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_MAX);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    if (SUCCEEDED(Redraw(dev, d))) ++g_objStats.maskDraws, g_maskWritten = true;
    states.Restore(dev);
    targets.Restore(dev);
}

// ---- native state tracking
//
// The game's render target, shaders and vertex shader constants are tracked as it sets them
// (device vtable slots 37, 92, 94, 107), so a draw needs no queries: jitter, the tonemap round
// trip and the object work all run from the draw hook. Our own state changes (replays, copies)
// pass through untracked (g_inDrawHook) and are undone before returning. ReShade draws its overlay
// with a state block, which restores state without these calls, so the first draw of each frame
// reads the device state once (g_resync).

struct RtInfo {
    UINT w = 0, h = 0;
    IDirect3DTexture9* tex = nullptr;  // compared only
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
};
std::unordered_map<uintptr_t, RtInfo> g_rtCache;

using SetRtFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetVsFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetPsFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetVsFFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
SetRtFn g_setRt = nullptr, g_setRtFirst = nullptr;
SetVsFn g_setVs = nullptr, g_setVsFirst = nullptr;
SetPsFn g_setPs = nullptr, g_setPsFirst = nullptr;
SetVsFFn g_setVsF = nullptr, g_setVsFFirst = nullptr;
bool g_inStateHook = false;

IDirect3DSurface9* g_rt0 = nullptr;
const RtInfo* g_rt0Info = nullptr;
IDirect3DVertexShader9* g_vs = nullptr;
ObjVs* g_vsObj = nullptr;
IDirect3DPixelShader9* g_ps = nullptr;
bool g_psTonemap = false;
bool g_drawSincePs = false;          // a draw happened since the last pixel shader change
float g_vsF[256 * 4] = {};           // the game's vertex shader constants (without our jitter)
int g_jitReg = -1;                   // our jittered fViewProj copy is on the device at this register
bool g_jitDirty = false;             // the game partly overwrote it since
bool g_resync = true;                // read the device state at the next draw
IDirect3DTexture9* g_depthTexFrame = nullptr;  // this frame's pre-pass target (compared only)
std::unordered_map<uintptr_t, bool> g_psIsTonemap;

const RtInfo* RtInfoOf(IDirect3DSurface9* rt) {
    if (!rt) return nullptr;
    const uintptr_t id = reinterpret_cast<uintptr_t>(rt);
    auto it = g_rtCache.find(id);
    if (it != g_rtCache.end()) return &it->second;
    RtInfo info;
    D3DSURFACE_DESC rd = {};
    rt->GetDesc(&rd);
    info.w = rd.Width, info.h = rd.Height, info.fmt = rd.Format;
    rt->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&info.tex));
    if (info.tex) info.tex->Release();
    if (g_rtCache.size() > 4096) g_rtCache.clear();
    return &g_rtCache.emplace(id, info).first->second;
}

bool IsTonemapPs(IDirect3DPixelShader9* ps) {
    if (!ps) return false;
    auto it = g_psIsTonemap.find(reinterpret_cast<uintptr_t>(ps));
    if (it != g_psIsTonemap.end()) return it->second;
    UINT size = 0;
    bool tonemap = false;
    if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size && size < (1u << 20)) {
        std::vector<uint8_t> code(size);
        if (SUCCEEDED(ps->GetFunction(code.data(), &size))) tonemap = Fnv(code.data(), size) == kTonemapPs;
    }
    if (tonemap) Log("objects: tonemap pixel shader %p", ps);
    return g_psIsTonemap[reinterpret_cast<uintptr_t>(ps)] = tonemap;
}

void TrackRt0(IDirect3DSurface9* rt) {
    g_rt0 = rt;
    g_rt0Info = RtInfoOf(rt);
    if (g_rt0Info && !g_pastTonemap && !g_depthTexFrame && g_rt0Info->w == g_bbWidth && g_rt0Info->h == g_bbHeight &&
        g_rt0Info->tex) {
        g_depthTexFrame = g_rt0Info->tex;  // the frame's first back-buffer-sized target: its depth pre-pass
        g_depthRes.handle = reinterpret_cast<uint64_t>(g_depthTexFrame);
    }
}

void TrackVs(IDirect3DVertexShader9* vs) {
    g_vs = vs;
    g_vsObj = vs ? &ObjVsInfo(vs) : nullptr;
}

void Resync(IDirect3DDevice9* dev) {
    g_resync = false;
    IDirect3DSurface9* rt = nullptr;
    dev->GetRenderTarget(0, &rt);
    TrackRt0(rt);
    if (rt) rt->Release();
    IDirect3DVertexShader9* vs = nullptr;
    dev->GetVertexShader(&vs);
    TrackVs(vs);
    if (vs) vs->Release();
    IDirect3DPixelShader9* ps = nullptr;
    dev->GetPixelShader(&ps);
    g_ps = ps, g_psTonemap = IsTonemapPs(ps);
    if (ps) ps->Release();
    dev->GetVertexShaderConstantF(0, g_vsF, 256);
    g_jitReg = -1, g_jitDirty = false;  // the device has the game's values (jitter undone at the tonemap)
}

HRESULT STDMETHODCALLTYPE HookSetRt(IDirect3DDevice9* dev, DWORD i, IDirect3DSurface9* rt) {
    if (g_inStateHook) return g_setRtFirst(dev, i, rt);
    if (!g_inDrawHook && i == 0) {
        if (!g_batch.empty() && reinterpret_cast<uintptr_t>(rt) != g_batchTarget) {  // the batch's pass ends
            g_inDrawHook = true;
            FlushMotion(dev);
            g_inDrawHook = false;
        }
        TrackRt0(rt);
    }
    g_inStateHook = true;
    const HRESULT hr = g_setRt(dev, i, rt);
    g_inStateHook = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSetVs(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs) {
    if (g_inStateHook) return g_setVsFirst(dev, vs);
    if (!g_inDrawHook) TrackVs(vs);
    g_inStateHook = true;
    const HRESULT hr = g_setVs(dev, vs);
    g_inStateHook = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSetPs(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    if (g_inStateHook) return g_setPsFirst(dev, ps);
    if (!g_inDrawHook) {
        // RenoDX swaps in its own version of the tonemap shader right after the game binds it (in
        // ReShade's bind event, before any draw): the replacement is the tonemap pass too.
        if (g_psTonemap && !g_drawSincePs && ps && ps != g_ps && !IsTonemapPs(ps)) {
            g_psIsTonemap[reinterpret_cast<uintptr_t>(ps)] = true;
            Log("objects: tonemap pixel shader %p replaced by %p (RenoDX)", g_ps, ps);
        }
        g_ps = ps, g_psTonemap = IsTonemapPs(ps);
        g_drawSincePs = false;
    }
    g_inStateHook = true;
    const HRESULT hr = g_setPs(dev, ps);
    g_inStateHook = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSetVsF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count) {
    if (g_inStateHook) return g_setVsFFirst(dev, start, data, count);
    if (!g_inDrawHook && data && start < 256) {
        const UINT n = std::min(count, 256 - start);
        memcpy(g_vsF + start * 4, data, n * 16);
        if (g_jitReg >= 0 && start < static_cast<UINT>(g_jitReg) + 4 && start + n > static_cast<UINT>(g_jitReg)) {
            if (start <= static_cast<UINT>(g_jitReg) && start + n >= static_cast<UINT>(g_jitReg) + 4) g_jitReg = -1;  // all of it
            else g_jitDirty = true;
        }
    }
    g_inStateHook = true;
    const HRESULT hr = g_setVsF(dev, start, data, count);
    g_inStateHook = false;
    return hr;
}

void WriteVsF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count) {  // ours: not tracked
    g_inStateHook = true;
    g_setVsF(dev, start, data, count);
    g_inStateHook = false;
}

// Jitter (see jitter.inl): the draw's fViewProj, from the tracked game constants, with this frame's
// offset; the game's values go back before any draw that doesn't use that register as fViewProj.
void NativeJitter(IDirect3DDevice9* dev) {
    const bool scene = g_rt0Info && g_rt0Info->w == g_bbWidth && g_rt0Info->h == g_bbHeight && !g_pastTonemap;
    const int vpReg = scene && g_vsObj ? g_vsObj->vpReg : -1;
    int applyReg = -1;
    if (vpReg >= 0) {
        const float* vp = g_vsF + vpReg * 4;
        bool finite = true;
        for (int i = 0; i < 16; ++i) finite = finite && std::isfinite(vp[i]);
        if (finite && vp[15] != 0.0f) {
            if (!g_haveFrameVp && LooksLikeViewProj(vp)) {  // the frame's camera, for motion vectors
                memcpy(g_frameVp, vp, sizeof(g_frameVp));
                g_haveFrameVp = true;
            }
            if (g_jitterMode) applyReg = vpReg;
        }
    }
    if (g_jitReg >= 0 && g_jitReg != applyReg) {  // put the game's values back
        WriteVsF(dev, static_cast<UINT>(g_jitReg), g_vsF + g_jitReg * 4, 4);
        g_jitReg = -1, g_jitDirty = false;
    }
    if (applyReg < 0) return;
    ++g_jitteredDraws;
    if (g_jitReg == applyReg && !g_jitDirty) return;  // already on the device
    float vp[16];
    memcpy(vp, g_vsF + applyReg * 4, sizeof(vp));
    const float jx = 2.0f * g_jitterX / g_bbWidth, jy = -2.0f * g_jitterY / g_bbHeight;
    for (int r = 0; r < 4; ++r) {
        vp[r * 4 + 0] += jx * vp[r * 4 + 3];
        vp[r * 4 + 1] += jy * vp[r * 4 + 3];
    }
    WriteVsF(dev, static_cast<UINT>(applyReg), vp, 4);
    g_jitReg = applyReg, g_jitDirty = false;
}

void NativeRoundTrip(IDirect3DDevice9* dev) {
    if (!g_rtMode || g_rtFailed || g_rtDoneThisFrame || !g_d9) return;
    IDirect3DBaseTexture9* bt = nullptr;
    dev->GetTexture(0, &bt);  // the tonemap's input: the HDR scene
    IDirect3DTexture9* tex = nullptr;
    if (bt) bt->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex)), bt->Release();
    if (!tex) return;
    g_rtDoneThisFrame = true;
    g_sceneTexLast = tex;
    RoundTrip(tex, g_depthTexFrame);
    tex->Release();
}

void OnSceneDrawImpl(IDirect3DDevice9* dev, const DrawArgs& d, uintptr_t ib, TargetStats*& ts);

// Everything the add-on does before a game draw. `up`: DrawPrimitiveUP / DrawIndexedPrimitiveUP
// (vertices from memory: full-screen quads such as the tonemap pass; never replayed).
void BeforeDraw(IDirect3DDevice9* dev, const DrawArgs& d, uintptr_t ib, bool up = false) {
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    if (g_resync) Resync(dev);
    g_drawSincePs = true;
    PollPipeline();
    if (g_psTonemap && !g_pastTonemap) {
        static int logged = 0;
        if (logged++ < 2) Log("objects: tonemap draw (%s%s)", up ? "UP " : "", d.indexed ? "indexed" : "non-indexed");
        NativeRoundTrip(dev);
        g_pastTonemap = true;
    }
    NativeJitter(dev);
    if (!up && !g_pastTonemap && g_bbWidth && ObjectsActive()) {
        const uint32_t motion0 = g_objStats.motionDraws, mask0 = g_objStats.maskDraws;
        TargetStats* ts = nullptr;
        OnSceneDrawImpl(dev, d, ib, ts);
        if (ts) ts->motion += g_objStats.motionDraws - motion0, ts->mask += g_objStats.maskDraws - mask0;
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    g_objStats.cpuMs += (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}

void OnSceneDrawImpl(IDirect3DDevice9* dev, const DrawArgs& d, uintptr_t ib, TargetStats*& ts) {
    if (!g_rt0Info || !g_vsObj) return;
    const uintptr_t rtId = reinterpret_cast<uintptr_t>(g_rt0);
    if (!g_batch.empty() && rtId != g_batchTarget) FlushMotion(dev);  // the batch's pass ended
    IDirect3DTexture9* rtTex = g_rt0Info->tex;
    if (g_rt0Info->w != g_bbWidth || g_rt0Info->h != g_bbHeight) return;
    {
        auto ins = g_targetStats.try_emplace(reinterpret_cast<uintptr_t>(rtTex));
        ts = &ins.first->second;
        if (ins.second) ts->order = g_targetOrder++;
        ++ts->draws;
        ts->depth = rtTex && rtTex == g_depthTexFrame;
    }
    if (ts->depth) return;  // depth pre-pass
    ObjVs& o = *g_vsObj;
    if (o.vpReg < 0) return;  // screen-space pass
    DWORD blend = 0, zwrite = 0;
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    dev->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);

    if (blend && !zwrite) {  // world-space effect: snapshot of its target before the first one (effect mask)
        if (g_effectMaskOn && !g_fxSnapped && g_fxPrivSurf && rtTex && g_rt0Info->fmt == g_texFmt) {
            g_fxSnapped = SUCCEEDED(dev->StretchRect(g_rt0, nullptr, g_fxPrivSurf, nullptr, D3DTEXF_NONE));
            if (g_fxSnapped) {
                ++g_objStats.maskDraws;
                g_fxSnapTex = rtTex;
                SafeRelease(g_fxTargetSurf);
                g_fxTargetSurf = g_rt0;
                g_fxTargetSurf->AddRef();
            }
        }
        return;
    }
    if (o.boneSampler >= 0) ++ts->skinned;
    else if (o.worldReg >= 0) ++ts->rigid;
    if (!g_objMotionOn || !zwrite || (o.worldReg < 0 && o.boneSampler < 0)) return;
    // History of this draw (keyed by its geometry and how often it was drawn so far this frame).
    IDirect3DVertexBuffer9* vbp = nullptr;
    UINT off = 0, stride = 0;
    dev->GetStreamSource(0, &vbp, &off, &stride);
    const uintptr_t vb = reinterpret_cast<uintptr_t>(vbp) + off;
    if (vbp) vbp->Release();
    DrawKey k = {vb, ib, d.start, static_cast<uint32_t>(d.base), d.prims, 0};
    k.n = g_occurrence[k]++;
    DrawHist cur = {};
    if (o.worldReg >= 0) memcpy(cur.world, g_vsF + o.worldReg * 4, sizeof(cur.world));
    if (o.skinReg >= 0) memcpy(cur.skin, g_vsF + o.skinReg * 4, sizeof(cur.skin));
    g_histCur[k] = cur;
    if (o.boneSampler >= 0 && !g_boneTexThisFrame) {
        IDirect3DBaseTexture9* bt = nullptr;
        dev->GetTexture(D3DVERTEXTEXTURESAMPLER0 + static_cast<DWORD>(o.boneSampler), &bt);
        if (bt) {
            bt->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&g_boneTexThisFrame));
            bt->Release();
        }
    }
    auto it = g_histPrev.find(k);
    if (it == g_histPrev.end() || !g_havePrevVp) {
        ++g_objStats.skipped;
        return;
    }
    const bool moved = o.boneSampler >= 0 || memcmp(it->second.world, cur.world, sizeof(cur.world)) != 0;
    if (!moved) return;  // static: the camera motion is right
    uint64_t h = Fnv(1469598103934665603ull, &k, offsetof(DrawKey, n));  // geometry, not the occurrence
    h = Fnv(h, &cur, sizeof(cur));
    if (!g_replayed.insert(h).second) return;  // already replayed in the other colour pass
    RecordMotion(dev, g_vs, o, it->second, d, rtId);
}

HRESULT STDMETHODCALLTYPE HookDip(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT minV, UINT numV, UINT start,
                                  UINT prims) {
    if (g_inDrawHook) return g_dipFirst(dev, type, base, minV, numV, start, prims);  // another hook chained back to us
    g_inDrawHook = true;
    IDirect3DIndexBuffer9* ib = nullptr;
    if (SUCCEEDED(dev->GetIndices(&ib)) && ib) ib->Release();
    BeforeDraw(dev, DrawArgs{true, type, base, minV, numV, start, prims}, reinterpret_cast<uintptr_t>(ib));
    const HRESULT hr = g_dip(dev, type, base, minV, numV, start, prims);
    g_inDrawHook = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookDp(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
    if (g_inDrawHook) return g_dpFirst(dev, type, start, prims);
    g_inDrawHook = true;
    BeforeDraw(dev, DrawArgs{false, type, 0, 0, 0, start, prims}, 0);
    const HRESULT hr = g_dp(dev, type, start, prims);
    g_inDrawHook = false;
    return hr;
}

using DpUpFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DipUpFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                            const void*, UINT);
DpUpFn g_dpUp = nullptr, g_dpUpFirst = nullptr;
DipUpFn g_dipUp = nullptr, g_dipUpFirst = nullptr;

HRESULT STDMETHODCALLTYPE HookDpUp(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT prims, const void* data, UINT stride) {
    if (g_inDrawHook) return g_dpUpFirst(dev, type, prims, data, stride);
    g_inDrawHook = true;
    BeforeDraw(dev, DrawArgs{false, type, 0, 0, 0, 0, prims}, 0, true);
    const HRESULT hr = g_dpUp(dev, type, prims, data, stride);
    g_inDrawHook = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookDipUp(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT minV, UINT numV, UINT prims,
                                    const void* idx, D3DFORMAT fmt, const void* data, UINT stride) {
    if (g_inDrawHook) return g_dipUpFirst(dev, type, minV, numV, prims, idx, fmt, data, stride);
    g_inDrawHook = true;
    BeforeDraw(dev, DrawArgs{true, type, 0, minV, numV, 0, prims}, 0, true);
    const HRESULT hr = g_dipUp(dev, type, minV, numV, prims, idx, fmt, data, stride);
    g_inDrawHook = false;
    return hr;
}

std::string ModuleOf(const void* p) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<const char*>(p), &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    const char* slash = strrchr(name, '\\');
    return slash ? slash + 1 : name;
}

template <typename Fn>
bool HookSlot(void** vt, int slot, Fn hook, Fn& next, Fn& first) {
    if (vt[slot] == reinterpret_cast<void*>(hook)) return false;
    DWORD old;
    if (!VirtualProtect(vt + slot, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    next = reinterpret_cast<Fn>(vt[slot]);
    if (!first) first = next;
    vt[slot] = reinterpret_cast<void*>(hook);
    VirtualProtect(vt + slot, 4, old, &old);
    return true;
}

// Called at device creation and again at every present: the slots get replaced after creation
// (by d3d9.dll), so our hooks go back on top (and call whatever is there now).
void HookDraws(IDirect3DDevice9* dev) {
    void** vt = *reinterpret_cast<void***>(dev);
    bool any = false;
    any |= HookSlot<DpFn>(vt, 81, &HookDp, g_dp, g_dpFirst);
    any |= HookSlot<DipFn>(vt, 82, &HookDip, g_dip, g_dipFirst);
    any |= HookSlot<DpUpFn>(vt, 83, &HookDpUp, g_dpUp, g_dpUpFirst);
    any |= HookSlot<DipUpFn>(vt, 84, &HookDipUp, g_dipUp, g_dipUpFirst);
    any |= HookSlot<SetRtFn>(vt, 37, &HookSetRt, g_setRt, g_setRtFirst);
    any |= HookSlot<SetVsFn>(vt, 92, &HookSetVs, g_setVs, g_setVsFirst);
    any |= HookSlot<SetVsFFn>(vt, 94, &HookSetVsF, g_setVsF, g_setVsFFirst);
    any |= HookSlot<SetPsFn>(vt, 107, &HookSetPs, g_setPs, g_setPsFirst);
    if (!any) return;
    g_resync = true;  // calls may have been missed while a slot wasn't ours
    static int hooks = 0;
    if (++hooks <= 6)
        Log("objects: hooked draws and state (vtable %p), next in chain %p in %s", vt, g_dip,
            ModuleOf(reinterpret_cast<void*>(g_dip)).c_str());
}

// Last frame's bones: copy the bone texture into our own render target (it isn't one, so a
// full-screen draw does the copy; the state block puts the game's state back).
void CopyBones() {
    g_prevBonesValid = false;
    IDirect3DTexture9* src = g_boneTexThisFrame;
    if (!src || !g_objShadersOk) return;
    D3DSURFACE_DESC sd = {};
    src->GetLevelDesc(0, &sd);
    if (g_prevBones) {
        D3DSURFACE_DESC pd = {};
        g_prevBones->GetLevelDesc(0, &pd);
        if (pd.Width != sd.Width || pd.Height != sd.Height || pd.Format != sd.Format) {
            g_prevBonesSurf->Release(), g_prevBonesSurf = nullptr;
            g_prevBones->Release(), g_prevBones = nullptr;
        }
    }
    if (!g_prevBones) {
        const HRESULT hr = g_d9->CreateTexture(sd.Width, sd.Height, 1, D3DUSAGE_RENDERTARGET, sd.Format, D3DPOOL_DEFAULT, &g_prevBones, nullptr);
        if (FAILED(hr)) {
            Log("objects: bone copy texture %ux%u format %u failed 0x%08lx", sd.Width, sd.Height, sd.Format, hr);
            g_prevBones = nullptr;
            return;
        }
        g_prevBones->GetSurfaceLevel(0, &g_prevBonesSurf);
        Log("objects: bone texture %ux%u format %u", sd.Width, sd.Height, sd.Format);
    }
    IDirect3DDevice9* dev = g_d9;
    const bool wasInHook = g_inDrawHook;
    g_inDrawHook = true;  // our state changes: not tracked
    g_copyState->Capture();
    IDirect3DSurface9 *rt0 = nullptr, *ds = nullptr;
    dev->GetRenderTarget(0, &rt0);
    dev->GetDepthStencilSurface(&ds);
    IDirect3DSurface9* rts[3] = {};
    for (DWORD i = 1; i < 4; ++i)
        if (FAILED(dev->GetRenderTarget(i, &rts[i - 1]))) rts[i - 1] = nullptr;
    dev->SetRenderTarget(0, g_prevBonesSurf);
    for (DWORD i = 1; i < 4; ++i) dev->SetRenderTarget(i, nullptr);
    dev->SetDepthStencilSurface(nullptr);
    dev->SetVertexDeclaration(g_copyDecl);
    dev->SetVertexShader(g_copyVs);
    dev->SetPixelShader(g_copyPs);
    dev->SetTexture(0, src);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    const float c0[4] = {1.0f / sd.Width, 1.0f / sd.Height, 0, 0};
    dev->SetPixelShaderConstantF(0, c0, 1);
    const float tri[12] = {-1, 1, 0.5f, 1, 3, 1, 0.5f, 1, -1, -3, 0.5f, 1};
    const bool ok = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 16));
    dev->SetRenderTarget(0, rt0);
    for (DWORD i = 1; i < 4; ++i) dev->SetRenderTarget(i, rts[i - 1]);
    dev->SetDepthStencilSurface(ds);
    g_copyState->Apply();  // viewport, shaders, declaration, stream 0, textures, states
    if (rt0) rt0->Release();
    if (ds) ds->Release();
    for (IDirect3DSurface9* s : rts)
        if (s) s->Release();
    g_inDrawHook = wasInHook;
    g_prevBonesValid = ok;
}

void ObjectsFrameEnd() {  // at present (after this frame's round trip)
    if (g_d9) FlushMotion(g_d9);
    if (g_d9) HookDraws(g_d9);
    PollPipeline();
    if (ObjectsActive()) {
        CopyBones();
        g_d9->ColorFill(g_objSurf, nullptr, 0);  // next frame starts with no object motion, no mask
    } else {
        g_prevBonesValid = false;
    }
    if (g_boneTexThisFrame) g_boneTexThisFrame->Release(), g_boneTexThisFrame = nullptr;
    g_histPrev.swap(g_histCur);
    g_histCur.clear();
    g_occurrence.clear();
    g_objStatsLast = g_objStats;
    static LARGE_INTEGER lastPresent = {};
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    static double frameMsSum = 0, drawEvSum = 0, objSum = 0;
    static uint32_t frames = 0;
    if (lastPresent.QuadPart) {
        frameMsSum += (now.QuadPart - lastPresent.QuadPart) * 1000.0 / freq.QuadPart;
        drawEvSum += g_drawEventMs, objSum += g_objStats.cpuMs, ++frames;
    }
    lastPresent = now;
    g_drawEventMs = 0;
    if (g_jitterIndex % 600 == 300 && frames) {
        Log("frame time %.2f ms avg (%.0f fps); add-on CPU per frame %.2f ms (draw hook; ReShade draw events %.2f ms) (last %u frames)",
            frameMsSum / frames, 1000.0 * frames / frameMsSum, objSum / frames, drawEvSum / frames, frames);
        frameMsSum = drawEvSum = objSum = 0, frames = 0;
    }
    if (g_jitterIndex % 600 == 300 && g_objStats.motionDraws + g_objStats.maskDraws > 0) {
        Log("objects: last frame %u motion draws, %u effect snapshots (%u used so far, %u not the tonemap's input), %u without "
            "history, %.2f ms CPU; shaders patched %u, failed %u",
            g_objStats.motionDraws, g_objStats.maskDraws, g_fxUsed, g_fxMismatch, g_objStats.skipped, g_objStats.cpuMs,
            g_objStats.patched, g_objStats.failedPatch);
        std::vector<std::pair<uint32_t, std::pair<uintptr_t, TargetStats>>> v;
        for (const auto& e : g_targetStats) v.push_back({e.second.order, e});
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& e : v)
            Log("  target %u (%p)%s: %u draws (%u skinned, %u rigid), %u motion replays, %u mask replays", e.first,
                reinterpret_cast<void*>(e.second.first), e.second.second.depth ? " depth pre-pass" : "", e.second.second.draws,
                e.second.second.skinned, e.second.second.rigid, e.second.second.motion, e.second.second.mask);
    }
    g_targetStats.clear();
    g_targetOrder = 0;
    g_replayed.clear();
    g_objStats.motionDraws = g_objStats.maskDraws = g_objStats.skipped = 0;
    g_objStats.cpuMs = 0;
    g_objWritten = g_maskWritten = false;
    g_fxSnapped = false;
    g_depthTexFrame = nullptr;
    g_resync = true;  // ReShade's overlay restores state with a state block (no Set* calls)
}
