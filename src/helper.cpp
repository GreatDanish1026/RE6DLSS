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

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

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
    float2 pad;
};
Texture2D<float4> src : register(t0);
Texture2D<float> depthTex : register(t1);

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
    float pad[2];
};

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader *invert = nullptr, *decode = nullptr, *motion = nullptr, *sanitize = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11Query* query = nullptr;
    // shared (opened from the add-on's D3D9Ex handles)
    ID3D11Texture2D *in = nullptr, *out = nullptr, *depthPacked = nullptr;
    ID3D11ShaderResourceView *inSrv = nullptr, *depthPackedSrv = nullptr;
    ID3D11RenderTargetView* outRtv = nullptr;
    // ours
    ID3D11Texture2D *depth = nullptr, *mv = nullptr, *dlssOut = nullptr, *color = nullptr;
    ID3D11ShaderResourceView* depthSrv = nullptr;
    ID3D11RenderTargetView *depthRtv = nullptr, *mvRtv = nullptr, *colorRtv = nullptr;
    uint32_t generation = 0, width = 0, height = 0;
    bool dlssTried = false, dlssOk = false;
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
        {"invert", &g.invert}, {"decode", &g.decode}, {"motion", &g.motion}, {"sanitize", &g.sanitize}};
    for (const auto& s : list) {
        if (!one(s.e, "ps_5_0", &b)) return false;
        g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, s.ps);
        Release(b);
    }
    D3D11_BUFFER_DESC bd = {sizeof(FrameCb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    g.dev->CreateBuffer(&bd, nullptr, &g.cb);
    return g.vs && g.invert && g.decode && g.motion && g.sanitize && g.cb;
}

void ReleaseTextures(Gpu& g) {
    Release(g.inSrv), Release(g.depthPackedSrv), Release(g.outRtv);
    Release(g.in), Release(g.out), Release(g.depthPacked);
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
    ID3D11ShaderResourceView* none[2] = {};
    g.ctx->PSSetShaderResources(0, 2, none);
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
    g.ctx->UpdateSubresource(g.cb, 0, nullptr, &cb, 0, 0);

    FullScreen(g, g.colorRtv, g.sanitize, &g.inSrv, 1);
    FullScreen(g, g.depthRtv, g.decode, &g.depthPackedSrv, 1);
    ID3D11ShaderResourceView* motionSrvs[2] = {g.inSrv, g.depthSrv};  // depth at t1
    FullScreen(g, g.mvRtv, g.motion, motionSrvs, 2);

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
    std::string err;
    if (!dlss::Evaluate(g.ctx, in, err)) {
        snprintf(ctl->helperStatus, sizeof(ctl->helperStatus), "DLSS: %s", err.c_str());
        g.ctx->CopyResource(g.out, g.in);
        return;
    }
    g.ctx->CopyResource(g.out, g.dlssOut);
    snprintf(ctl->helperStatus, sizeof(ctl->helperStatus), "DLSS %s", dlss::Status().c_str());
}

void Process(Gpu& g, SharedControl* ctl) {
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    if (ctl->mode == kModeInvert) FullScreen(g, g.outRtv, g.invert, &g.inSrv, 1);
    else if (ctl->mode == kModeDlaa) Dlaa(g, ctl);
    else g.ctx->CopyResource(g.out, g.in);
    g.ctx->End(g.query);
    g.ctx->Flush();
    while (g.ctx->GetData(g.query, nullptr, 0, 0) == S_FALSE) YieldProcessor();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    ctl->helperGpuMs = static_cast<float>((t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    ++ctl->helperFrames;
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
    if (FAILED(hr) || !Compile(g)) {
        ctl->helperError = hr;
        snprintf(ctl->helperStatus, sizeof(ctl->helperStatus), "helper: D3D11 setup failed 0x%08lx", hr);
        Log("%s", ctl->helperStatus);
        return 3;
    }
    snprintf(ctl->helperStatus, sizeof(ctl->helperStatus), "helper: running");
    Log("D3D11 device ready");
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
