// RE6DLSS-helper64.exe: the 64-bit side of RE6DLSS. DLSS only exists as 64-bit code and RE6 is a
// 32-bit game, so the add-on hands frames to this process through D3D9Ex shared textures.
//
// Proof of concept: for each frame the add-on signals, it either copies the input texture to the
// output texture or inverts it with a pixel shader, waits for its GPU work, and signals back.
// Usage (started by the add-on): RE6DLSS-helper64.exe <game pid>
// It exits when the game exits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "shared.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

FILE* g_log = nullptr;

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

const char kShader[] = R"(
Texture2D<float4> src : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float4 c = src.Load(int3(pos.xy, 0));
    return float4(1.0 - saturate(c.rgb), c.a);
}
)";

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Query* query = nullptr;
    ID3D11Texture2D* in = nullptr;
    ID3D11Texture2D* out = nullptr;
    ID3D11ShaderResourceView* inSrv = nullptr;
    ID3D11RenderTargetView* outRtv = nullptr;
    uint32_t generation = 0;
};

template <typename T>
void Release(T*& p) {
    if (p) p->Release(), p = nullptr;
}

bool Compile(Gpu& g) {
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "helper", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err);
    if (SUCCEEDED(hr)) hr = D3DCompile(kShader, sizeof(kShader) - 1, "helper", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        Log("shader compile failed 0x%08lx %s", hr, err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        Release(err);
        return false;
    }
    g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs);
    g.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps);
    Release(vsb);
    Release(psb);
    return g.vs && g.ps;
}

HRESULT OpenTextures(Gpu& g, SharedControl* ctl) {
    Release(g.inSrv);
    Release(g.outRtv);
    Release(g.in);
    Release(g.out);
    HRESULT hr = g.dev->OpenSharedResource(reinterpret_cast<HANDLE>(ctl->inHandle), __uuidof(ID3D11Texture2D),
                                           reinterpret_cast<void**>(&g.in));
    if (SUCCEEDED(hr))
        hr = g.dev->OpenSharedResource(reinterpret_cast<HANDLE>(ctl->outHandle), __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void**>(&g.out));
    if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(g.in, nullptr, &g.inSrv);
    if (SUCCEEDED(hr)) hr = g.dev->CreateRenderTargetView(g.out, nullptr, &g.outRtv);
    if (FAILED(hr)) return hr;
    D3D11_TEXTURE2D_DESC d;
    g.in->GetDesc(&d);
    Log("opened shared textures: %ux%u format %u (generation %u)", d.Width, d.Height, d.Format, ctl->generation);
    g.generation = ctl->generation;
    return S_OK;
}

void Process(Gpu& g, SharedControl* ctl) {
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    if (ctl->mode == 1) {
        D3D11_TEXTURE2D_DESC d;
        g.out->GetDesc(&d);
        D3D11_VIEWPORT vp = {0, 0, static_cast<float>(d.Width), static_cast<float>(d.Height), 0, 1};
        g.ctx->OMSetRenderTargets(1, &g.outRtv, nullptr);
        g.ctx->RSSetViewports(1, &vp);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->VSSetShader(g.vs, nullptr, 0);
        g.ctx->PSSetShader(g.ps, nullptr, 0);
        g.ctx->PSSetShaderResources(0, 1, &g.inSrv);
        g.ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        g.ctx->PSSetShaderResources(0, 1, &none);
        g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    } else {
        g.ctx->CopyResource(g.out, g.in);
    }
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
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) wcscpy(slash + 1, L"RE6DLSS-helper64.log");
    g_log = _wfopen(exe, L"w");
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
        Log("setup failed: game %p ctl %p go %p done %p", game, ctl, go, done);
        return 2;
    }

    Gpu g;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
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
    ctl->helperAlive = 1;
    Log("D3D11 device ready");

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
        }
        Process(g, ctl);
        SetEvent(done);
    }
    Log("game exited after %u frames", ctl->helperFrames);
    return 0;
}
