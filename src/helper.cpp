// RE6DLSS-helper64.exe: the 64-bit side of RE6DLSS. DLSS only exists as 64-bit code and RE6 is a
// 32-bit game, so the add-on hands frames to this process through D3D9Ex shared textures.
//
// Per frame the add-on signals (src/shared.h), in one of three modes:
//   copy   - output = input (round trip check),
//   invert - output = 1 - input (visible round trip check),
//   DLAA   - depth decode -> camera motion vectors -> DLSS (DLAA) -> output.
// Then it waits for its GPU work and signals back.
//
// Depth: the game writes its depth (0 near .. 1 far, z/w) into the R, G, B of its pre-pass target,
// 8 bits each: depth = R + G/255 + B/65025. Motion vectors: each pixel's position is rebuilt from
// that depth with this frame's jittered view-projection, then projected with the current and
// previous unjittered ones; their difference in pixels (previous - current) is camera motion
// without jitter. Objects' own motion isn't known (RE6 has no velocity buffer).
//
// Usage (started by the add-on): RE6DLSS-helper64.exe <game pid>. It exits when the game exits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <dxgi.h>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

#include "common.hpp"
#include "dlss.hpp"
#include "shared.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace rd {
namespace {
FILE* g_log = nullptr;
std::wstring g_dir;
}  // namespace

void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

std::wstring ExeDir() { return g_dir; }
}  // namespace rd

namespace {

using rd::Log;

const char kShaders[] = R"(
cbuffer Frame : register(b0) {
    row_major float4x4 invJitteredVp;  // NDC (with this frame's jitter) -> world
    row_major float4x4 curVp;          // world -> clip, unjittered
    row_major float4x4 prevVp;         // last frame's, unjittered
    float2 size;                       // pixels
    float useObjects;                  // 1: the add-on's object motion (objTex.rg where objTex.b = 1) wins
    float pad;
};
Texture2D<float4> src : register(t0);
Texture2D<float> depthTex : register(t1);
Texture2D<float4> objTex : register(t2);  // object motion (rg, b = valid), from the add-on
Texture2D<float4> fxTex : register(t3);   // the effect target before its first see-through effect
Texture2D<float4> fxAfterTex : register(t5); // the same target after them
Texture2D<float> maskView : register(t4); // the effect mask (developer view)

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 invert(float4 pos : SV_Position) : SV_Target {
    float4 c = src.Load(int3(pos.xy, 0));
    return float4(1.0 - saturate(c.rgb), c.a);
}
float decode(float4 pos : SV_Position) : SV_Target {
    float3 p = src.Load(int3(pos.xy, 0)).rgb;
    return dot(p, float3(1.0, 1.0 / 255.0, 1.0 / 65025.0));
}
float2 motion(float4 pos : SV_Position) : SV_Target {
    if (useObjects > 0.5) {
        float4 o = objTex.Load(int3(pos.xy, 0));
        if (o.b > 0.5 && all(abs(o.rg) <= size) && !any(isnan(o.rg))) return o.rg;
    }
    float z = saturate(depthTex.Load(int3(pos.xy, 0)));
    float2 ndc = float2(pos.x / size.x * 2 - 1, 1 - pos.y / size.y * 2);
    float4 world = mul(float4(ndc, z, 1), invJitteredVp);
    if (abs(world.w) < 1e-8) return 0;
    world /= world.w;
    float4 cur = mul(world, curVp);
    float4 prev = mul(world, prevVp);
    if (cur.w <= 1e-6 || prev.w <= 1e-6) return 0;  // behind the camera in either frame
    float2 curNdc = cur.xy / cur.w, prevNdc = prev.xy / prev.w;
    float2 mv = (prevNdc - curNdc) * float2(0.5, -0.5) * size;
    // Invalid or off-screen-sized motion (camera cuts, near-plane extremes) -> none.
    if (any(isnan(mv)) || any(isinf(mv)) || any(abs(mv) > size)) return 0;
    return mv;
}
// Effect mask for DLSS (bias towards this frame's colour).
// Effect mask: how much the see-through effects (drawn after the snapshot) changed the pixel,
// relative to its brightness.
float mask(float4 pos : SV_Position) : SV_Target {
    float3 f = fxAfterTex.Load(int3(pos.xy, 0)).rgb, b = fxTex.Load(int3(pos.xy, 0)).rgb;
    if (any(isnan(f)) || any(isnan(b)) || any(isinf(f)) || any(isinf(b))) return 0;
    float3 d = abs(f - b);
    float change = max(d.r, max(d.g, d.b));
    float level = max(f.r, max(f.g, f.b)) + max(b.r, max(b.g, b.b)) + 0.02;
    return saturate(4.0 * change / level);
}
// Developer views, shown instead of the picture: motion vectors (red/green = x/y, blue = object
// motion from the add-on) or the effect mask.
float4 debugMotion(float4 pos : SV_Position) : SV_Target {
    float2 mv = src.Load(int3(pos.xy, 0)).rg;
    float obj = objTex.Load(int3(pos.xy, 0)).b > 0.5 ? 0.6 : 0;
    return float4(saturate(abs(mv) / 16.0), obj, 1);
}
float4 debugMask(float4 pos : SV_Position) : SV_Target {
    float a = saturate(maskView.Load(int3(pos.xy, 0)));
    return float4(a, a, a, 1);
}
// Colour as DLSS gets it: NaN, infinite or negative values (the game's skin shading can produce
// them in close-ups; its own tonemap hides them, but DLSS's history would spread them) -> 0.
float4 sanitize(float4 pos : SV_Position) : SV_Target {
    float4 c = src.Load(int3(pos.xy, 0));
    if (any(isnan(c)) || any(isinf(c))) c = 0;
    return float4(clamp(c.rgb, 0, 65000), saturate(c.a));
}
)";

struct FrameCb {
    float invJitteredVp[16];
    float curVp[16];
    float prevVp[16];
    float size[2];
    float useObjects;
    float pad;
};

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader *invert = nullptr, *decode = nullptr, *motion = nullptr, *sanitize = nullptr;
    ID3D11PixelShader *mask = nullptr, *debugMotion = nullptr, *debugMask = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11Query* query = nullptr;
    ID3D11Query *disjoint = nullptr, *ts[4] = {};  // GPU timing: start, after prep passes, after DLSS, end
    // shared (opened from the add-on's D3D9Ex handles)
    ID3D11Texture2D *in = nullptr, *out = nullptr, *depthPacked = nullptr, *obj = nullptr, *fx = nullptr, *fxAfter = nullptr;
    ID3D11ShaderResourceView *fxAfterSrv = nullptr;
    ID3D11ShaderResourceView *inSrv = nullptr, *depthPackedSrv = nullptr, *objSrv = nullptr, *fxSrv = nullptr, *maskSrv = nullptr;
    ID3D11RenderTargetView* outRtv = nullptr;
    // ours
    ID3D11Texture2D *depth = nullptr, *mv = nullptr, *dlssOut = nullptr, *color = nullptr, *maskTex = nullptr;
    ID3D11ShaderResourceView *depthSrv = nullptr, *mvSrv = nullptr;
    ID3D11RenderTargetView *depthRtv = nullptr, *mvRtv = nullptr, *colorRtv = nullptr, *maskRtv = nullptr;
    uint32_t generation = 0, width = 0, height = 0;
    bool dlssTried = false, dlssOk = false;
    bool timingPending = false;
};

template <typename T>
void Release(T*& p) {
    if (p) p->Release(), p = nullptr;
}

bool Compile(Gpu& g) {
    auto one = [&](const char* entry, const char* target, ID3DBlob** out) {
        ID3DBlob* err = nullptr;
        const HRESULT hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "helper", nullptr, nullptr, entry, target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
        if (FAILED(hr)) Log("shader %s: 0x%08lx %s", entry, hr, err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        Release(err);
        return SUCCEEDED(hr);
    };
    ID3DBlob *b = nullptr;
    if (!one("vs", "vs_5_0", &b)) return false;
    g.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g.vs);
    Release(b);
    const struct { const char* e; ID3D11PixelShader** ps; } list[] = {
        {"invert", &g.invert}, {"decode", &g.decode}, {"motion", &g.motion}, {"sanitize", &g.sanitize},
        {"mask", &g.mask}, {"debugMotion", &g.debugMotion}, {"debugMask", &g.debugMask}};
    for (const auto& s : list) {
        if (!one(s.e, "ps_5_0", &b)) return false;
        g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, s.ps);
        Release(b);
    }
    D3D11_BUFFER_DESC bd = {sizeof(FrameCb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    g.dev->CreateBuffer(&bd, nullptr, &g.cb);
    return g.vs && g.invert && g.decode && g.motion && g.sanitize && g.mask && g.debugMotion && g.debugMask && g.cb;
}

void ReleaseTextures(Gpu& g) {
    Release(g.inSrv), Release(g.depthPackedSrv), Release(g.outRtv), Release(g.objSrv), Release(g.fxSrv), Release(g.maskSrv);
    Release(g.in), Release(g.out), Release(g.depthPacked), Release(g.obj), Release(g.fx), Release(g.fxAfterSrv), Release(g.fxAfter);
    Release(g.mvSrv), Release(g.maskRtv), Release(g.maskTex);
    Release(g.depthSrv), Release(g.depthRtv), Release(g.mvRtv), Release(g.colorRtv);
    Release(g.depth), Release(g.mv), Release(g.dlssOut), Release(g.color);
}

HRESULT OpenShared(Gpu& g, uint64_t handle, ID3D11Texture2D** tex) {
    return g.dev->OpenSharedResource(reinterpret_cast<HANDLE>(handle), __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex));
}

HRESULT OpenTextures(Gpu& g, SharedControl* ctl) {
    ReleaseTextures(g);
    dlss::ReleaseFeatures();
    HRESULT hr = OpenShared(g, ctl->inHandle, &g.in);
    if (SUCCEEDED(hr)) hr = OpenShared(g, ctl->outHandle, &g.out);
    if (SUCCEEDED(hr)) hr = OpenShared(g, ctl->depthHandle, &g.depthPacked);
    if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(g.in, nullptr, &g.inSrv);
    if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(g.depthPacked, nullptr, &g.depthPackedSrv);
    if (SUCCEEDED(hr)) hr = g.dev->CreateRenderTargetView(g.out, nullptr, &g.outRtv);
    if (SUCCEEDED(hr) && ctl->objHandle) {  // optional: older add-on builds don't send it
        if (SUCCEEDED(OpenShared(g, ctl->objHandle, &g.obj))) g.dev->CreateShaderResourceView(g.obj, nullptr, &g.objSrv);
        else Log("couldn't open the object texture; object motion off");
    }
    if (SUCCEEDED(hr) && ctl->fxHandle) {
        if (SUCCEEDED(OpenShared(g, ctl->fxHandle, &g.fx))) g.dev->CreateShaderResourceView(g.fx, nullptr, &g.fxSrv);
        else Log("couldn't open the effect snapshot; effect mask off");
    }
    if (SUCCEEDED(hr) && ctl->fxAfterHandle) {
        if (SUCCEEDED(OpenShared(g, ctl->fxAfterHandle, &g.fxAfter))) g.dev->CreateShaderResourceView(g.fxAfter, nullptr, &g.fxAfterSrv);
        else Log("couldn't open the effect target copy; effect mask off");
    }
    if (FAILED(hr)) return hr;
    D3D11_TEXTURE2D_DESC d;
    g.in->GetDesc(&d);
    g.width = d.Width, g.height = d.Height;
    auto make = [&](DXGI_FORMAT f, UINT bind, ID3D11Texture2D** t) {
        D3D11_TEXTURE2D_DESC td = {d.Width, d.Height, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
        return g.dev->CreateTexture2D(&td, nullptr, t);
    };
    hr = make(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &g.depth);
    if (SUCCEEDED(hr)) hr = make(DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &g.mv);
    if (SUCCEEDED(hr)) hr = make(d.Format, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &g.dlssOut);
    if (SUCCEEDED(hr)) hr = make(d.Format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &g.color);
    if (SUCCEEDED(hr)) hr = g.dev->CreateRenderTargetView(g.color, nullptr, &g.colorRtv);
    if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(g.depth, nullptr, &g.depthSrv);
    if (SUCCEEDED(hr)) hr = g.dev->CreateRenderTargetView(g.depth, nullptr, &g.depthRtv);
    if (SUCCEEDED(hr)) hr = g.dev->CreateRenderTargetView(g.mv, nullptr, &g.mvRtv);
    if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(g.mv, nullptr, &g.mvSrv);
    if (SUCCEEDED(hr)) hr = make(DXGI_FORMAT_R8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &g.maskTex);
    if (SUCCEEDED(hr)) hr = g.dev->CreateRenderTargetView(g.maskTex, nullptr, &g.maskRtv);
    if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(g.maskTex, nullptr, &g.maskSrv);
    if (FAILED(hr)) return hr;
    Log("opened shared textures: %ux%u format %u (generation %u)", d.Width, d.Height, d.Format, ctl->generation);
    g.generation = ctl->generation;
    return S_OK;
}

void FullScreen(Gpu& g, ID3D11RenderTargetView* rtv, ID3D11PixelShader* ps, ID3D11ShaderResourceView* const* srvs, UINT nsrv) {
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(g.width), static_cast<float>(g.height), 0, 1};
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs, nullptr, 0);
    g.ctx->PSSetShader(ps, nullptr, 0);
    g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->PSSetShaderResources(0, nsrv, srvs);
    g.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[6] = {};
    g.ctx->PSSetShaderResources(0, 6, none);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// 4x4 inverse (row-major, general).
bool Invert4(const float* m, float* out) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = static_cast<double>(m[0]) * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20) return false;
    for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(inv[i] / det);
    return true;
}

void Dlaa(Gpu& g, SharedControl* ctl) {
    if (!g.dlssTried) {
        g.dlssTried = true;
        std::string err;
        g.dlssOk = dlss::Init(g.dev, err);
        if (!g.dlssOk) {
            snprintf(ctl->helperStatus, sizeof(ctl->helperStatus), "DLSS: %s", err.c_str());
            Log("%s", ctl->helperStatus);
        }
    }
    if (!g.dlssOk) {  // pass the frame through unchanged
        g.ctx->CopyResource(g.out, g.in);
        return;
    }
    // Constants: the jittered matrix for rebuilding positions, the unjittered ones for motion.
    FrameCb cb = {};
    float jittered[16];
    memcpy(jittered, ctl->viewProj, sizeof(jittered));
    const float jx = 2.0f * ctl->jitterX / g.width, jy = -2.0f * ctl->jitterY / g.height;
    for (int r = 0; r < 4; ++r) {
        jittered[r * 4 + 0] += jx * jittered[r * 4 + 3];
        jittered[r * 4 + 1] += jy * jittered[r * 4 + 3];
    }
    if (!Invert4(jittered, cb.invJitteredVp)) memset(cb.invJitteredVp, 0, sizeof(cb.invJitteredVp));
    memcpy(cb.curVp, ctl->viewProj, sizeof(cb.curVp));
    memcpy(cb.prevVp, ctl->havePrev ? ctl->prevViewProj : ctl->viewProj, sizeof(cb.prevVp));
    cb.size[0] = static_cast<float>(g.width), cb.size[1] = static_cast<float>(g.height);
    const bool objects = g.objSrv && (ctl->objFlags & kObjMotion);
    const bool masked = g.fxSrv && g.fxAfterSrv && (ctl->objFlags & kObjMask);
    cb.useObjects = objects ? 1.0f : 0.0f;
    g.ctx->UpdateSubresource(g.cb, 0, nullptr, &cb, 0, 0);

    FullScreen(g, g.colorRtv, g.sanitize, &g.inSrv, 1);
    FullScreen(g, g.depthRtv, g.decode, &g.depthPackedSrv, 1);
    ID3D11ShaderResourceView* motionSrvs[3] = {g.inSrv, g.depthSrv, g.objSrv};  // depth at t1, objects at t2
    FullScreen(g, g.mvRtv, g.motion, motionSrvs, 3);
    if (masked) {
        ID3D11ShaderResourceView* maskSrvs[6] = {nullptr, nullptr, nullptr, g.fxSrv, nullptr, g.fxAfterSrv};
        FullScreen(g, g.maskRtv, g.mask, maskSrvs, 6);
    }
    if (g.ts[1]) g.ctx->End(g.ts[1]);

    dlss::Inputs in;
    in.color = g.color;  // sanitized copy of the scene
    in.depth = g.depth;
    in.motion = g.mv;
    in.output = g.dlssOut;
    in.renderWidth = in.outputWidth = g.width;
    in.renderHeight = in.outputHeight = g.height;
    in.jitterX = ctl->jitterX;
    in.jitterY = ctl->jitterY;
    in.reset = ctl->reset != 0 || !ctl->havePrev;
    in.preset = ctl->preset;
    in.depthInverted = false;
    {  // RenoDX: 16-bit float scene (linear HDR); without it the game's own 8-bit scene
        D3D11_TEXTURE2D_DESC cd;
        g.in->GetDesc(&cd);
        in.hdr = cd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || cd.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
                 cd.Format == DXGI_FORMAT_R11G11B10_FLOAT;
    }
    in.biasMask = masked ? g.maskTex : nullptr;
    std::string err;
    if (!dlss::Evaluate(g.ctx, in, err)) {
        snprintf(ctl->helperStatus, sizeof(ctl->helperStatus), "DLSS: %s", err.c_str());
        g.ctx->CopyResource(g.out, g.in);
        return;
    }
    if (g.ts[2]) g.ctx->End(g.ts[2]);
    g.ctx->CopyResource(g.out, g.dlssOut);
    if (ctl->debugView == 1) {
        ID3D11ShaderResourceView* srvs[3] = {g.mvSrv, nullptr, g.objSrv};
        FullScreen(g, g.outRtv, g.debugMotion, srvs, 3);
    } else if (ctl->debugView == 2) {
        if (!masked) {
            const float black[4] = {0, 0, 0, 0};
            g.ctx->ClearRenderTargetView(g.maskRtv, black);
        }
        ID3D11ShaderResourceView* srvs[5] = {nullptr, nullptr, nullptr, nullptr, g.maskSrv};
        FullScreen(g, g.outRtv, g.debugMask, srvs, 5);
    }
    snprintf(ctl->helperStatus, sizeof(ctl->helperStatus), "DLSS %s", dlss::Status().c_str());
}

struct Timing {  // sums over the log interval
    double wall = 0, queue = 0, prep = 0, dlss = 0, copy = 0, maxWall = 0;
    uint32_t frames = 0, timed = 0;
} g_timing;

// GPU time of each part, from the timestamp queries (false if not available yet).
bool ReadTimestamps(Gpu& g, double& prep, double& dlss, double& copy, double& total) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    UINT64 ts[4] = {};
    const UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    bool ok = g.ctx->GetData(g.disjoint, &dj, sizeof(dj), flags) == S_OK && !dj.Disjoint && dj.Frequency;
    for (int i = 0; ok && i < 4; ++i) ok = g.ctx->GetData(g.ts[i], &ts[i], sizeof(UINT64), flags) == S_OK;
    if (!ok || ts[1] < ts[0] || ts[2] < ts[1] || ts[3] < ts[2]) return false;
    const double k = 1000.0 / static_cast<double>(dj.Frequency);
    prep = (ts[1] - ts[0]) * k, dlss = (ts[2] - ts[1]) * k, copy = (ts[3] - ts[2]) * k, total = (ts[3] - ts[0]) * k;
    return true;
}

// One frame: answer once the GPU work is finished (the OS doesn't keep the two processes' GPU
// work in order by itself, so the game must not touch the output before that).
void Process(Gpu& g, SharedControl* ctl) {
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    Timing& t = g_timing;
    double prep, dlssMs, copy, gpu;
    if (g.timingPending) {  // last frame's timestamps (low-latency: read one frame later)
        g.timingPending = false;
        if (ReadTimestamps(g, prep, dlssMs, copy, gpu)) t.prep += prep, t.dlss += dlssMs, t.copy += copy, ++t.timed;
    }
    const bool timed = g.disjoint && ctl->mode == kModeDlaa && g.dlssOk;
    if (timed) {
        g.ctx->Begin(g.disjoint);
        g.ctx->End(g.ts[0]);
    }
    if (ctl->mode == kModeInvert) FullScreen(g, g.outRtv, g.invert, &g.inSrv, 1);
    else if (ctl->mode == kModeDlaa) Dlaa(g, ctl);
    else g.ctx->CopyResource(g.out, g.in);
    if (timed) {
        g.ctx->End(g.ts[3]);
        g.ctx->End(g.disjoint);
    }
    g.ctx->End(g.query);
    g.ctx->Flush();
    while (g.ctx->GetData(g.query, nullptr, 0, 0) == S_FALSE) YieldProcessor();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const double wall = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    ctl->helperGpuMs = static_cast<float>(wall);
    ++ctl->helperFrames;

    t.wall += wall;
    t.maxWall = (std::max)(t.maxWall, wall);
    ++t.frames;
    if (timed && ReadTimestamps(g, prep, dlssMs, copy, gpu)) {
        t.prep += prep, t.dlss += dlssMs, t.copy += copy, t.queue += wall - gpu;
        ++t.timed;
    }
    if (t.frames >= 300) {
        const double n = t.timed ? t.timed : 1;
        Log("last %u frames: wall %.2f ms avg (max %.2f); GPU: prep passes %.2f, DLSS %.2f, copy %.2f; waiting for the GPU %.2f (%u timed)",
                t.frames, t.wall / t.frames, t.maxWall, t.prep / n, t.dlss / n, t.copy / n, t.queue / n, t.timed);
        t = Timing{};
    }
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    const DWORD pid = strtoul(cmd, nullptr, 10);
    if (!pid) return 1;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir.resize(dir.find_last_of(L'\\') + 1);
    rd::g_dir = dir;
    rd::g_log = _wfopen((dir + L"RE6DLSS-helper64.log").c_str(), L"w");
    Log("RE6DLSS-helper64 started for game pid %lu", pid);

    // The game waits for this process every frame, so it must wake and run at once. Windows 11
    // treats a window-less background process as low priority: power throttling (EcoQoS, slower
    // cores) and the default 15.6 ms timer, which showed up as ~16-18 ms stalls on random frames.
    {
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
        PROCESS_POWER_THROTTLING_STATE pt = {};
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask = 0;  // throttling off for both
        const BOOL noThrottle = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
        const MMRESULT timer = timeBeginPeriod(1);
        const BOOL prio = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        Log("scheduling: power throttling off %s, 1 ms timer %s, high priority %s", noThrottle ? "yes" : "no",
            timer == TIMERR_NOERROR ? "yes" : "no", prio ? "yes" : "no");
    }

    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    char name[64];
    snprintf(name, sizeof(name), "RE6DLSS_%lu_ctl", pid);
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    snprintf(name, sizeof(name), "RE6DLSS_%lu_go", pid);
    HANDLE go = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    snprintf(name, sizeof(name), "RE6DLSS_%lu_done", pid);
    HANDLE done = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    auto* ctl = map ? static_cast<SharedControl*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedControl))) : nullptr;
    if (!game || !ctl || !go || !done || ctl->version != RD_SHARED_VERSION) {
        Log("setup failed: game %p ctl %p go %p done %p version %u", game, ctl, go, done, ctl ? ctl->version : 0);
        return 2;
    }

    Gpu g;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.dev,
                                   nullptr, &g.ctx);
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    if (SUCCEEDED(hr)) hr = g.dev->CreateQuery(&qd, &g.query);
    if (SUCCEEDED(hr)) {  // optional GPU timing
        D3D11_QUERY_DESC dq = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, tq = {D3D11_QUERY_TIMESTAMP, 0};
        bool ok = SUCCEEDED(g.dev->CreateQuery(&dq, &g.disjoint));
        for (auto*& q : g.ts) ok = ok && SUCCEEDED(g.dev->CreateQuery(&tq, &q));
        if (!ok) Release(g.disjoint);
    }
    if (FAILED(hr) || !Compile(g)) {
        ctl->helperError = hr;
        snprintf(ctl->helperStatus, sizeof(ctl->helperStatus), "helper: D3D11 setup failed 0x%08lx", hr);
        Log("%s", ctl->helperStatus);
        return 3;
    }
    snprintf(ctl->helperStatus, sizeof(ctl->helperStatus), "helper: running");
    Log("D3D11 device ready");
    {  // GPU side: the helper's work should not queue behind other processes' work
        IDXGIDevice* dxgi = nullptr;
        if (SUCCEEDED(g.dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) {
            const HRESULT ph = dxgi->SetGPUThreadPriority(7);
            Log("GPU thread priority 7: 0x%08lx", ph);
            dxgi->Release();
        }
    }
    {  // start NGX now, so problems show up at once (not at the first DLAA frame)
        std::string err;
        g.dlssTried = true;
        g.dlssOk = dlss::Init(g.dev, err);
        if (!g.dlssOk) snprintf(ctl->helperStatus, sizeof(ctl->helperStatus), "DLSS: %s", err.c_str());
    }
    ctl->helperAlive = 1;

    HANDLE waits[2] = {go, game};
    for (;;) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) break;  // game exited (or error)
        if (ctl->generation != g.generation || !g.in) {
            hr = OpenTextures(g, ctl);
            if (FAILED(hr)) {
                ctl->helperError = hr;
                snprintf(ctl->helperStatus, sizeof(ctl->helperStatus), "helper: opening shared textures failed 0x%08lx", hr);
                Log("%s", ctl->helperStatus);
                SetEvent(done);
                continue;
            }
            ctl->helperReady = g.generation;
            ctl->helperError = 0;
        }
        Process(g, ctl);
        SetEvent(done);
    }
    Log("game exited after %u frames", ctl->helperFrames);
    dlss::Shutdown();
    return 0;
}
