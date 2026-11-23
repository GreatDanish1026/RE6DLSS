// ---------------------------------------------------------------- helper round trip (proof of concept)
//
// Included into main.cpp's anonymous namespace. At the game's tonemap draw (ps1009a8dd, which reads
// the HDR scene at t0) the scene is copied into a D3D9Ex shared texture, the GPU work is waited
// for, RE6DLSS-helper64.exe is signalled, and once it signals back its output texture is copied over
// the scene before the tonemap draw runs. Once per frame. Mode: Off / Copy / Invert / DLAA
// (ReShade.ini [RE6DLSS] RoundTrip=0/1/2/3). For DLAA the game's packed depth (the first
// back-buffer-sized target of the frame, its pre-pass) is shared too, with this frame's jitter and
// the current and previous view-projection.

constexpr uint32_t kTonemapPs = 0x1009a8dd;
constexpr DWORD kHelperTimeoutMs = 200;

int g_rtMode = 0;  // 0 off, 1 copy, 2 invert, 3 DLAA
resource g_depthRes = {0};  // this frame's pre-pass target (packed depth), set at its first bind
int g_lastSentMode = -1;
int g_dlssPreset = -1;  // NVSDK_NGX_DLSS_Hint_Render_Preset_*, -1 automatic
IDirect3DDevice9Ex* g_d9 = nullptr;
bool g_noEx = false;  // the game's device isn't D3D9Ex (RenoDX missing)
SharedControl* g_ctl = nullptr;
HANDLE g_map = nullptr, g_go = nullptr, g_done = nullptr;
PROCESS_INFORMATION g_helper = {};
IDirect3DTexture9* g_inTex = nullptr;
IDirect3DTexture9* g_outTex = nullptr;
IDirect3DTexture9* g_depthTex = nullptr;
D3DFORMAT g_depthFmt = D3DFMT_UNKNOWN;
IDirect3DQuery9* g_query = nullptr;
UINT g_texW = 0, g_texH = 0;
D3DFORMAT g_texFmt = D3DFMT_UNKNOWN;
bool g_rtDoneThisFrame = false;
bool g_rtFailed = false;
std::string g_rtStatus = "off";
double g_rtMsAvg = 0, g_rtWaitAvg = 0;
uint64_t g_rtFrames = 0;

uint32_t DxgiOf(D3DFORMAT f) {
    switch (f) {
        case D3DFMT_A16B16G16R16F: return 10;  // DXGI_FORMAT_R16G16B16A16_FLOAT
        case D3DFMT_A8R8G8B8: return 87;       // B8G8R8A8_UNORM
        case D3DFMT_A32B32G32R32F: return 2;
        default: return 0;
    }
}

bool StartHelper() {
    if (g_ctl) return true;
    const DWORD pid = GetCurrentProcessId();
    char name[64];
    snprintf(name, sizeof(name), "RE6DLSS_%lu_ctl", pid);
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedControl), name);
    g_ctl = g_map ? static_cast<SharedControl*>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedControl))) : nullptr;
    snprintf(name, sizeof(name), "RE6DLSS_%lu_go", pid);
    g_go = CreateEventA(nullptr, FALSE, FALSE, name);
    snprintf(name, sizeof(name), "RE6DLSS_%lu_done", pid);
    g_done = CreateEventA(nullptr, FALSE, FALSE, name);
    if (!g_ctl || !g_go || !g_done) {
        g_rtStatus = "couldn't create the shared control block";
        return false;
    }
    memset(g_ctl, 0, sizeof(SharedControl));
    g_ctl->version = RD_SHARED_VERSION;
    const std::wstring exe = g_exeDir + L"RE6DLSS-helper64.exe";
    std::wstring cmd = L"\"" + exe + L"\" " + std::to_wstring(pid);
    STARTUPINFOW si = {sizeof(si)};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_exeDir.c_str(), &si,
                        &g_helper)) {
        g_rtStatus = Format("couldn't start RE6DLSS-helper64.exe (error %lu)", GetLastError());
        Log("%s", g_rtStatus.c_str());
        UnmapViewOfFile(g_ctl);
        g_ctl = nullptr;
        return false;
    }
    Log("helper started (pid %lu)", g_helper.dwProcessId);
    return true;
}

void StopHelper() {
    if (g_helper.hProcess) {
        TerminateProcess(g_helper.hProcess, 0);
        CloseHandle(g_helper.hProcess);
        CloseHandle(g_helper.hThread);
        g_helper = {};
    }
}

bool EnsureTextures(UINT w, UINT h, D3DFORMAT fmt, D3DFORMAT depthFmt) {
    if (g_inTex && g_texW == w && g_texH == h && g_texFmt == fmt && g_depthFmt == depthFmt) return true;
    if (g_inTex) g_inTex->Release(), g_inTex = nullptr;
    if (g_outTex) g_outTex->Release(), g_outTex = nullptr;
    if (g_depthTex) g_depthTex->Release(), g_depthTex = nullptr;
    HANDLE hin = nullptr, hout = nullptr, hdepth = nullptr;
    HRESULT hr = g_d9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &g_inTex, &hin);
    if (SUCCEEDED(hr)) hr = g_d9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &g_outTex, &hout);
    if (SUCCEEDED(hr)) hr = g_d9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, depthFmt, D3DPOOL_DEFAULT, &g_depthTex, &hdepth);
    if (FAILED(hr) || !hin || !hout || !hdepth) {
        g_rtStatus = Format("shared texture creation failed (0x%08lx)", hr);
        Log("%s", g_rtStatus.c_str());
        return false;
    }
    if (!g_query && FAILED(g_d9->CreateQuery(D3DQUERYTYPE_EVENT, &g_query))) return false;
    g_texW = w, g_texH = h, g_texFmt = fmt, g_depthFmt = depthFmt;
    g_ctl->inHandle = reinterpret_cast<uintptr_t>(hin);
    g_ctl->outHandle = reinterpret_cast<uintptr_t>(hout);
    g_ctl->depthHandle = reinterpret_cast<uintptr_t>(hdepth);
    g_ctl->width = w, g_ctl->height = h;
    g_ctl->format = DxgiOf(fmt);
    ++g_ctl->generation;
    Log("shared textures %ux%u format %d, handles %p %p (generation %u)", w, h, fmt, hin, hout, g_ctl->generation);
    return true;
}

bool WaitGpu() {  // D3D9: event query, flushed, spun until the GPU is done (100 ms cap)
    g_query->Issue(D3DISSUE_END);
    const ULONGLONG until = GetTickCount64() + 100;
    while (g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
        if (GetTickCount64() > until) return false;
    return true;
}

void RoundTrip(IDirect3DTexture9* scene, IDirect3DTexture9* depth) {
    LARGE_INTEGER t0, t1, t2, f;
    QueryPerformanceCounter(&t0);
    D3DSURFACE_DESC d;
    if (FAILED(scene->GetLevelDesc(0, &d)) || !DxgiOf(d.Format)) {
        g_rtStatus = Format("scene texture format %d not handled", d.Format);
        return;
    }
    if (!StartHelper()) {
        g_rtFailed = true;
        return;
    }
    if (!g_ctl->helperAlive) {  // still creating its D3D11 device: skip this frame
        g_rtStatus = "waiting for the helper to start...";
        if (WaitForSingleObject(g_helper.hProcess, 0) == WAIT_OBJECT_0) {
            g_rtStatus = Format("helper exited: %s", g_ctl->helperStatus);
            g_rtFailed = true;
        }
        return;
    }
    const uint32_t generation = g_ctl->generation;
    D3DSURFACE_DESC dd = d;
    if (depth) depth->GetLevelDesc(0, &dd);
    if (dd.Width != d.Width || dd.Height != d.Height) depth = nullptr, dd.Format = d.Format;
    if (!EnsureTextures(d.Width, d.Height, d.Format, dd.Format)) {
        g_rtFailed = true;
        return;
    }
    // The first frame after (re)creating the textures includes the helper opening them.
    const DWORD timeout = g_ctl->generation != generation || g_ctl->helperReady != g_ctl->generation ? 2000 : kHelperTimeoutMs;
    IDirect3DSurface9 *src = nullptr, *in = nullptr, *out = nullptr, *dsrc = nullptr, *dsh = nullptr;
    scene->GetSurfaceLevel(0, &src);
    g_inTex->GetSurfaceLevel(0, &in);
    g_outTex->GetSurfaceLevel(0, &out);
    if (depth) depth->GetSurfaceLevel(0, &dsrc);
    g_depthTex->GetSurfaceLevel(0, &dsh);
    const bool depthOk = dsrc && dsh && SUCCEEDED(g_d9->StretchRect(dsrc, nullptr, dsh, nullptr, D3DTEXF_NONE));
    if (src && in && out && SUCCEEDED(g_d9->StretchRect(src, nullptr, in, nullptr, D3DTEXF_NONE)) && WaitGpu()) {
        g_ctl->mode = g_rtMode == 3 ? (depthOk && g_haveFrameVp ? kModeDlaa : kModeCopy)
                                    : g_rtMode == 2 ? kModeInvert : kModeCopy;
        g_ctl->jitterX = g_jitterX;
        g_ctl->jitterY = g_jitterY;
        memcpy(g_ctl->viewProj, g_frameVp, sizeof(g_ctl->viewProj));
        memcpy(g_ctl->prevViewProj, g_prevVp, sizeof(g_ctl->prevViewProj));
        g_ctl->havePrev = g_havePrevVp ? 1 : 0;
        g_ctl->reset = static_cast<int>(g_ctl->mode) != g_lastSentMode ? 1 : 0;
        g_ctl->preset = g_dlssPreset;
        g_lastSentMode = static_cast<int>(g_ctl->mode);
        QueryPerformanceCounter(&t1);
        ResetEvent(g_done);  // a late answer to an earlier frame must not count for this one
        SetEvent(g_go);
        const DWORD w = WaitForSingleObject(g_done, timeout);
        QueryPerformanceCounter(&t2);
        if (w != WAIT_OBJECT_0) {
            g_rtStatus = "helper didn't answer in time; round trip off";
            Log("%s (helper status: %s)", g_rtStatus.c_str(), g_ctl->helperStatus);
            g_rtFailed = true;
        } else if (g_ctl->helperReady != g_ctl->generation) {
            g_rtStatus = Format("helper error 0x%08lx: %s", static_cast<unsigned long>(g_ctl->helperError), g_ctl->helperStatus);
            Log("%s", g_rtStatus.c_str());
            g_rtFailed = true;
        } else {
            g_d9->StretchRect(out, nullptr, src, nullptr, D3DTEXF_NONE);
            QueryPerformanceFrequency(&f);
            const double total = (t2.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
            const double wait = (t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart;
            g_rtMsAvg = g_rtFrames ? g_rtMsAvg * 0.95 + total * 0.05 : total;
            g_rtWaitAvg = g_rtFrames ? g_rtWaitAvg * 0.95 + wait * 0.05 : wait;
            if (++g_rtFrames == 1 || g_rtFrames % 600 == 0)
                Log("round trip %llu: %.2f ms total, %.2f ms helper (helper's own %.2f ms)",
                    static_cast<unsigned long long>(g_rtFrames), total, wait, g_ctl->helperGpuMs);
            g_rtStatus = Format("%s: %.2f ms per frame (helper %.2f ms). %s",
                                g_rtMode == 3 ? (g_ctl->mode == kModeDlaa ? "DLAA" : "DLAA (waiting for the 3D scene)")
                                : g_rtMode == 2 ? "Invert test" : "Copy test",
                                g_rtMsAvg, g_rtWaitAvg, g_ctl->helperStatus);
        }
    } else {
        g_rtStatus = "copy into the shared texture failed";
    }
    if (src) src->Release();
    if (in) in->Release();
    if (out) out->Release();
    if (dsrc) dsrc->Release();
    if (dsh) dsh->Release();
}

// Called from the draw event (before the game's draw runs).
void MaybeRoundTrip(command_list* cl) {
    if (!g_rtMode || g_rtFailed || g_rtDoneThisFrame || !g_d9) return;
    resource_view view = {0};
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        Context& c = Ctx(cl);
        if (c.ps.shader.id != kTonemapPs) return;
        auto it = c.ps.srv.find(0);
        if (it == c.ps.srv.end()) return;
        view = it->second;
    }
    ViewInfo info;
    if (!FindView(view, &info) || !info.res.handle) return;
    auto* res = reinterpret_cast<IDirect3DResource9*>(info.res.handle);
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) || !tex) return;
    g_rtDoneThisFrame = true;
    IDirect3DTexture9* depth = nullptr;
    if (g_depthRes.handle)
        reinterpret_cast<IDirect3DResource9*>(g_depthRes.handle)
            ->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&depth));
    RoundTrip(tex, depth);
    if (depth) depth->Release();
    tex->Release();
}
