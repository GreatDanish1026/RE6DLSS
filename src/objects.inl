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
// The scene has two colour passes that both draw every character (lighting and colour); motion
// is replayed in the first one only (the first non-pre-pass target with a moving draw this frame).
uintptr_t g_motionTarget = 0;
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

void ReplayMotion(IDirect3DDevice9* dev, IDirect3DVertexShader9* gameVs, ObjVs& o, const DrawHist& prev, const DrawArgs& d) {
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
    if (!o.patched) return;
    if (o.boneSampler >= 0 && !g_prevBonesValid) return;
    const DWORD vsamp = D3DVERTEXTEXTURESAMPLER0 + static_cast<DWORD>(o.spareSampler);
    // Save.
    SavedTargets targets;
    targets.Save(dev);
    SavedStates states;
    states.Save(dev);
    IDirect3DPixelShader9* ps = nullptr;
    dev->GetPixelShader(&ps);
    float psC0[4], vsSpare[32];
    dev->GetPixelShaderConstantF(0, psC0, 1);
    dev->GetVertexShaderConstantF(kSpareConst, vsSpare, 8);
    IDirect3DBaseTexture9* oldTex = nullptr;
    DWORD samp[5] = {};
    if (o.spareSampler >= 0) {
        dev->GetTexture(vsamp, &oldTex);
        dev->GetSamplerState(vsamp, D3DSAMP_MINFILTER, &samp[0]);
        dev->GetSamplerState(vsamp, D3DSAMP_MAGFILTER, &samp[1]);
        dev->GetSamplerState(vsamp, D3DSAMP_MIPFILTER, &samp[2]);
        dev->GetSamplerState(vsamp, D3DSAMP_ADDRESSU, &samp[3]);
        dev->GetSamplerState(vsamp, D3DSAMP_ADDRESSV, &samp[4]);
    }
    // Set.
    dev->SetRenderTarget(0, g_objSurf);
    for (DWORD i = 1; i < 4; ++i) dev->SetRenderTarget(i, nullptr);
    dev->SetViewport(&targets.vp);
    dev->SetVertexShader(o.patched);
    dev->SetPixelShader(g_motionPs);
    const float c0[4] = {static_cast<float>(g_bbWidth), static_cast<float>(g_bbHeight), g_jitterX, g_jitterY};
    dev->SetPixelShaderConstantF(0, c0, 1);
    float spare[32] = {};
    memcpy(spare, g_prevVp, sizeof(float) * 16);
    memcpy(spare + 16, prev.world, sizeof(prev.world));
    memcpy(spare + 28, prev.skin, sizeof(prev.skin));
    dev->SetVertexShaderConstantF(kSpareConst, spare, 8);
    if (o.spareSampler >= 0) {
        dev->SetTexture(vsamp, g_prevBones);
        dev->SetSamplerState(vsamp, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(vsamp, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(vsamp, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(vsamp, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(vsamp, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    if (SUCCEEDED(Redraw(dev, d))) ++g_objStats.motionDraws, g_objWritten = true;
    // Restore.
    if (o.spareSampler >= 0) {
        dev->SetTexture(vsamp, oldTex);
        dev->SetSamplerState(vsamp, D3DSAMP_MINFILTER, samp[0]);
        dev->SetSamplerState(vsamp, D3DSAMP_MAGFILTER, samp[1]);
        dev->SetSamplerState(vsamp, D3DSAMP_MIPFILTER, samp[2]);
        dev->SetSamplerState(vsamp, D3DSAMP_ADDRESSU, samp[3]);
        dev->SetSamplerState(vsamp, D3DSAMP_ADDRESSV, samp[4]);
        if (oldTex) oldTex->Release();
    }
    dev->SetVertexShaderConstantF(kSpareConst, vsSpare, 8);
    dev->SetPixelShaderConstantF(0, psC0, 1);
    dev->SetPixelShader(ps);
    if (ps) ps->Release();
    dev->SetVertexShader(gameVs);
    states.Restore(dev);
    targets.Restore(dev);
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

void OnSceneDrawImpl(IDirect3DDevice9* dev, const DrawArgs& d, uintptr_t ib, TargetStats*& ts);

void OnSceneDraw(IDirect3DDevice9* dev, const DrawArgs& d, uintptr_t ib) {
    if (g_pastTonemap || !g_bbWidth || !ObjectsActive()) return;
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    const uint32_t motion0 = g_objStats.motionDraws, mask0 = g_objStats.maskDraws;
    TargetStats* ts = nullptr;
    OnSceneDrawImpl(dev, d, ib, ts);
    if (ts) ts->motion += g_objStats.motionDraws - motion0, ts->mask += g_objStats.maskDraws - mask0;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    g_objStats.cpuMs += (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}

void OnSceneDrawImpl(IDirect3DDevice9* dev, const DrawArgs& d, uintptr_t ib, TargetStats*& ts) {
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    D3DSURFACE_DESC rd = {};
    rt->GetDesc(&rd);
    IDirect3DTexture9* rtTex = nullptr;
    rt->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&rtTex));
    rt->Release();
    if (rtTex) rtTex->Release();  // only compared
    if (rd.Width != g_bbWidth || rd.Height != g_bbHeight) return;
    {
        auto ins = g_targetStats.try_emplace(reinterpret_cast<uintptr_t>(rtTex));
        ts = &ins.first->second;
        if (ins.second) ts->order = g_targetOrder++;
        ++ts->draws;
        ts->depth = g_depthRes.handle && rtTex == reinterpret_cast<IDirect3DTexture9*>(g_depthRes.handle);
    }
    if (ts->depth) return;  // depth pre-pass
    IDirect3DVertexShader9* vs = nullptr;
    if (FAILED(dev->GetVertexShader(&vs)) || !vs) return;
    ObjVs& o = ObjVsInfo(vs);
    vs->Release();  // the game holds it
    if (o.vpReg < 0) return;  // screen-space pass
    DWORD blend = 0, zwrite = 0;
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    dev->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);

    if (blend && !zwrite) {  // world-space effect: mask
        if (g_effectMaskOn) ReplayMask(dev, d);
        return;
    }
    if (o.boneSampler >= 0) ++ts->skinned;
    else if (o.worldReg >= 0) ++ts->rigid;
    if (!g_objMotionOn || !zwrite || (o.worldReg < 0 && o.boneSampler < 0)) return;
    const uintptr_t target = reinterpret_cast<uintptr_t>(rtTex);
    if (!g_motionTarget) g_motionTarget = target;
    if (target != g_motionTarget) return;
    // History of this draw (keyed by its geometry and how often it was drawn so far this frame).
    IDirect3DVertexBuffer9* vbp = nullptr;
    UINT off = 0, stride = 0;
    dev->GetStreamSource(0, &vbp, &off, &stride);
    const uintptr_t vb = reinterpret_cast<uintptr_t>(vbp) + off;
    if (vbp) vbp->Release();
    DrawKey k = {vb, ib, d.start, static_cast<uint32_t>(d.base), d.prims, 0};
    k.n = g_occurrence[k]++;
    DrawHist cur = {};
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        const Context& c = g_contexts[reinterpret_cast<uint64_t>(dev)];
        if (o.worldReg >= 0) memcpy(cur.world, c.vsConst + o.worldReg * 4, sizeof(cur.world));
        if (o.skinReg >= 0) memcpy(cur.skin, c.vsConst + o.skinReg * 4, sizeof(cur.skin));
    }
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
    IDirect3DVertexShader9* gameVs = nullptr;
    dev->GetVertexShader(&gameVs);
    ReplayMotion(dev, gameVs, o, it->second, d);
    if (gameVs) gameVs->Release();
}

HRESULT STDMETHODCALLTYPE HookDip(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT minV, UINT numV, UINT start,
                                  UINT prims) {
    if (g_inDrawHook) return g_dipFirst(dev, type, base, minV, numV, start, prims);  // another hook chained back to us
    g_inDrawHook = true;
    PollPipeline();
    IDirect3DIndexBuffer9* ib = nullptr;
    if (SUCCEEDED(dev->GetIndices(&ib)) && ib) ib->Release();
    OnSceneDraw(dev, DrawArgs{true, type, base, minV, numV, start, prims}, reinterpret_cast<uintptr_t>(ib));
    const HRESULT hr = g_dip(dev, type, base, minV, numV, start, prims);
    g_inDrawHook = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookDp(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
    if (g_inDrawHook) return g_dpFirst(dev, type, start, prims);
    g_inDrawHook = true;
    PollPipeline();
    OnSceneDraw(dev, DrawArgs{false, type, 0, 0, 0, start, prims}, 0);
    const HRESULT hr = g_dp(dev, type, start, prims);
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

// Called at device creation and again at every present: the slots get replaced after creation,
// so our hook goes back on top (and calls whatever is there now).
void HookDraws(IDirect3DDevice9* dev) {
    void** vt = *reinterpret_cast<void***>(dev);
    if (vt[82] == reinterpret_cast<void*>(&HookDip) && vt[81] == reinterpret_cast<void*>(&HookDp)) return;
    static int hooks = 0;
    DWORD old;
    if (!VirtualProtect(vt + 81, 8, PAGE_EXECUTE_READWRITE, &old)) return;
    if (vt[81] != reinterpret_cast<void*>(&HookDp)) {
        g_dp = reinterpret_cast<DpFn>(vt[81]);
        if (!g_dpFirst) g_dpFirst = g_dp;
        vt[81] = reinterpret_cast<void*>(&HookDp);
    }
    if (vt[82] != reinterpret_cast<void*>(&HookDip)) {
        g_dip = reinterpret_cast<DipFn>(vt[82]);
        if (!g_dipFirst) g_dipFirst = g_dip;
        vt[82] = reinterpret_cast<void*>(&HookDip);
    }
    VirtualProtect(vt + 81, 8, old, &old);
    if (++hooks <= 4)
        Log("objects: hooked draws (vtable %p), next in chain %p in %s", vt, g_dip, ModuleOf(reinterpret_cast<void*>(g_dip)).c_str());
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
    g_prevBonesValid = ok;
}

void ObjectsFrameEnd() {  // at present (after this frame's round trip)
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
    if (g_jitterIndex % 600 == 300 && g_objStats.motionDraws + g_objStats.maskDraws > 0) {
        Log("objects: last frame %u motion draws, %u mask draws, %u without history, %.2f ms CPU; shaders patched %u, failed %u",
            g_objStats.motionDraws, g_objStats.maskDraws, g_objStats.skipped, g_objStats.cpuMs, g_objStats.patched,
            g_objStats.failedPatch);
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
    g_motionTarget = 0;
    g_objStats.motionDraws = g_objStats.maskDraws = g_objStats.skipped = 0;
    g_objStats.cpuMs = 0;
    g_objWritten = g_maskWritten = false;
}
