// ---------------------------------------------------------------- helper round trip
//
// Included into main.cpp's anonymous namespace. At the game's tonemap draw (ps1009a8dd, which reads
// the HDR scene at t0) the scene, the game's packed depth (the first back-buffer-sized target of the
// frame, its pre-pass) and the object texture (objects.inl) are copied into D3D9Ex shared textures
// for RE6DLSS-helper64.exe, with this frame's jitter and the current and previous view-projection.
// Mode: Off / Copy / Invert / DLAA.
//
// Two hand-offs (setting Pipelined):
// - Waiting: the GPU work is waited for, the helper is signalled, and once it signals back its
//   output is copied over the scene before the tonemap draw runs. The game's render thread and
//   the GPU both stand still for ~2.5-3.5 ms every frame.
// - Pipelined: nothing waits. The copies are queued with an event query behind them; draws later
//   in the frame (and present) check the query without waiting and signal the helper as soon as
//   the copies are done on the GPU. At the next frame's tonemap the helper's output (finished by
//   then) goes over the scene: the 3D image is one frame behind the HUD and post-processing.
//   Each process only touches a shared texture after the other's work on it is known to be finished
//   (the helper answers once its GPU work is done; it starts only after the query), because this
//   system doesn't keep the GPU order between the two processes by itself (checked).

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
IDirect3DTexture9* g_objShared = nullptr;  // copy of g_objTex for the helper
IDirect3DTexture9* g_objTex = nullptr;     // moving objects' motion + effect mask, written during the scene (objects.inl)
IDirect3DSurface9* g_objSurf = nullptr;
bool g_objMotionOn = true, g_effectMaskOn = true;  // settings
bool g_pipelineOn = true;                // setting: pipelined hand-off
int g_debugView = 0;                     // developer: 1 motion vectors, 2 effect mask
D3DFORMAT g_depthFmt = D3DFMT_UNKNOWN;
IDirect3DQuery9* g_query = nullptr;
UINT g_texW = 0, g_texH = 0;
D3DFORMAT g_texFmt = D3DFMT_UNKNOWN;
bool g_rtDoneThisFrame = false;
bool g_rtFailed = false;
std::string g_rtStatus = "off";
double g_rtMsAvg = 0, g_rtWaitAvg = 0;
uint64_t g_rtFrames = 0;

// Pipelined state.
struct FrameParams {
    uint32_t mode;
    float jitterX, jitterY;
    float viewProj[16], prevViewProj[16];
    uint32_t havePrev, reset;
    int32_t preset;
    uint32_t objFlags, debugView;
};
FrameParams g_pend = {};
bool g_queryPending = false;  // copies queued, helper not signalled yet
bool g_inFlight = false;      // helper signalled, answer not collected yet
bool g_haveResult = false;    // the output texture holds a finished frame not yet shown
DWORD g_inFlightTimeout = kHelperTimeoutMs;

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
    if (g_objShared) g_objShared->Release(), g_objShared = nullptr;
    if (g_objSurf) g_objSurf->Release(), g_objSurf = nullptr;
    if (g_objTex) g_objTex->Release(), g_objTex = nullptr;
    g_haveResult = false;
    HANDLE hin = nullptr, hout = nullptr, hdepth = nullptr, hobj = nullptr;
    HRESULT hr = g_d9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &g_inTex, &hin);
    if (SUCCEEDED(hr)) hr = g_d9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &g_outTex, &hout);
    if (SUCCEEDED(hr)) hr = g_d9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, depthFmt, D3DPOOL_DEFAULT, &g_depthTex, &hdepth);
    if (SUCCEEDED(hr))
        hr = g_d9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_objShared, &hobj);
    if (SUCCEEDED(hr))
        hr = g_d9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_objTex, nullptr);
    if (SUCCEEDED(hr)) hr = g_objTex->GetSurfaceLevel(0, &g_objSurf);
    if (SUCCEEDED(hr)) g_d9->ColorFill(g_objSurf, nullptr, 0);
    if (FAILED(hr) || !hin || !hout || !hdepth || !hobj) {
        g_rtStatus = Format("shared texture creation failed (0x%08lx)", hr);
        Log("%s", g_rtStatus.c_str());
        return false;
    }
    if (!g_query && FAILED(g_d9->CreateQuery(D3DQUERYTYPE_EVENT, &g_query))) return false;
    g_texW = w, g_texH = h, g_texFmt = fmt, g_depthFmt = depthFmt;
    g_ctl->inHandle = reinterpret_cast<uintptr_t>(hin);
    g_ctl->outHandle = reinterpret_cast<uintptr_t>(hout);
    g_ctl->depthHandle = reinterpret_cast<uintptr_t>(hdepth);
    g_ctl->objHandle = reinterpret_cast<uintptr_t>(hobj);
    g_ctl->width = w, g_ctl->height = h;
    g_ctl->format = DxgiOf(fmt);
    ++g_ctl->generation;
    Log("shared textures %ux%u format %d, handles %p %p (generation %u)", w, h, fmt, hin, hout, g_ctl->generation);
    return true;
}

// A camera cut (cutscene shot change, teleport): the view-projection jumps. Rows 1-3 hold rotation
// and projection (values around 0..3), row 4 the translation (scene units); a normal frame changes
// them only a little. DLSS's history is dropped on a cut so the previous shot doesn't smear in.
uint32_t g_cuts = 0;

bool CameraCut(const float* cur, const float* prev) {
    float rot = 0, trans = 0, scale = 1;
    for (int i = 0; i < 12; ++i) rot = std::max(rot, std::fabs(cur[i] - prev[i]));
    for (int i = 12; i < 16; ++i) {
        trans = std::max(trans, std::fabs(cur[i] - prev[i]));
        scale = std::max(scale, std::fabs(cur[i]));
    }
    return rot > 0.25f || trans > 0.25f * scale;
}

bool WaitGpu() {  // D3D9: event query, flushed, spun until the GPU is done (100 ms cap)
    g_query->Issue(D3DISSUE_END);
    const ULONGLONG until = GetTickCount64() + 100;
    while (g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
        if (GetTickCount64() > until) return false;
    return true;
}

// This frame's parameters for the helper (taken at the tonemap draw, sent now or later).
FrameParams CaptureParams(bool depthOk) {
    FrameParams p = {};
    p.mode = g_rtMode == 3 ? (depthOk && g_haveFrameVp ? kModeDlaa : kModeCopy) : g_rtMode == 2 ? kModeInvert : kModeCopy;
    p.jitterX = g_jitterX, p.jitterY = g_jitterY;
    memcpy(p.viewProj, g_frameVp, sizeof(p.viewProj));
    memcpy(p.prevViewProj, g_prevVp, sizeof(p.prevViewProj));
    p.havePrev = g_havePrevVp ? 1 : 0;
    const bool cut = g_havePrevVp && CameraCut(g_frameVp, g_prevVp);
    if (cut) ++g_cuts;
    p.reset = static_cast<int>(p.mode) != g_lastSentMode || cut ? 1 : 0;
    p.preset = g_dlssPreset;
    p.objFlags = (g_objMotionOn ? kObjMotion : 0u) | (g_effectMaskOn ? kObjMask : 0u);
    p.debugView = static_cast<uint32_t>(g_debugView);
    g_lastSentMode = static_cast<int>(p.mode);
    return p;
}

void Signal(const FrameParams& p, DWORD timeout) {
    g_ctl->mode = p.mode;
    g_ctl->jitterX = p.jitterX, g_ctl->jitterY = p.jitterY;
    memcpy(g_ctl->viewProj, p.viewProj, sizeof(p.viewProj));
    memcpy(g_ctl->prevViewProj, p.prevViewProj, sizeof(p.prevViewProj));
    g_ctl->havePrev = p.havePrev, g_ctl->reset = p.reset, g_ctl->preset = p.preset;
    g_ctl->objFlags = p.objFlags, g_ctl->debugView = p.debugView;
    g_ctl->async = 0, g_ctl->checkMarker = 0;
    ResetEvent(g_done);  // a late answer to an earlier frame must not count for this one
    SetEvent(g_go);
    g_inFlight = true;
    g_inFlightTimeout = timeout;
}

// Collect the helper's answer. False (and the round trip off) on a timeout or helper error.
bool Collect() {
    if (!g_inFlight) return true;
    g_inFlight = false;
    const DWORD w = WaitForSingleObject(g_done, g_inFlightTimeout);
    if (w != WAIT_OBJECT_0) {
        g_rtStatus = "helper didn't answer in time; round trip off";
        Log("%s (helper status: %s)", g_rtStatus.c_str(), g_ctl->helperStatus);
        g_rtFailed = true;
        return false;
    }
    if (g_ctl->helperReady != g_ctl->generation) {
        g_rtStatus = Format("helper error 0x%08lx: %s", static_cast<unsigned long>(g_ctl->helperError), g_ctl->helperStatus);
        Log("%s", g_rtStatus.c_str());
        g_rtFailed = true;
        return false;
    }
    return true;
}

// Pipelined: called from the draw hooks and at present. Signals the helper once the queued copies
// are finished on the GPU (no waiting: the query is only checked).
void PollPipeline() {
    if (!g_queryPending || !g_query) return;
    if (g_query->GetData(nullptr, 0, 0) != S_OK) return;
    g_queryPending = false;
    Signal(g_pend, g_inFlightTimeout);
}

// Settle anything still in flight (mode switch, texture change, or the pipelined frame start).
bool Drain() {
    if (g_queryPending) {
        g_queryPending = false;
        const ULONGLONG until = GetTickCount64() + 100;
        while (g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
            if (GetTickCount64() > until) break;
        Signal(g_pend, g_inFlightTimeout);
    }
    if (g_inFlight) {
        if (!Collect()) return false;
        g_haveResult = true;
    }
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
    // Last frame's work (pipelined): finished by now in the normal case, so no real wait.
    QueryPerformanceCounter(&t1);
    if (!Drain()) return;
    QueryPerformanceCounter(&t2);
    const double drainMs = static_cast<double>(t2.QuadPart - t1.QuadPart);

    const uint32_t generation = g_ctl->generation;
    D3DSURFACE_DESC dd = d;
    if (depth) depth->GetLevelDesc(0, &dd);
    if (dd.Width != d.Width || dd.Height != d.Height) depth = nullptr, dd.Format = d.Format;
    if (!EnsureTextures(d.Width, d.Height, d.Format, dd.Format)) {
        g_rtFailed = true;
        return;
    }
    const bool pipelined = g_pipelineOn;
    if (!pipelined) g_haveResult = false;
    // The first frame after (re)creating the textures includes the helper opening them.
    const DWORD timeout = g_ctl->generation != generation || g_ctl->helperReady != g_ctl->generation ? 2000 : kHelperTimeoutMs;
    IDirect3DSurface9 *src = nullptr, *in = nullptr, *out = nullptr, *dsrc = nullptr, *dsh = nullptr, *osh = nullptr;
    scene->GetSurfaceLevel(0, &src);
    g_inTex->GetSurfaceLevel(0, &in);
    g_outTex->GetSurfaceLevel(0, &out);
    if (depth) depth->GetSurfaceLevel(0, &dsrc);
    g_depthTex->GetSurfaceLevel(0, &dsh);
    g_objShared->GetSurfaceLevel(0, &osh);
    const bool depthOk = dsrc && dsh && SUCCEEDED(g_d9->StretchRect(dsrc, nullptr, dsh, nullptr, D3DTEXF_NONE));
    if (osh && g_objSurf) g_d9->StretchRect(g_objSurf, nullptr, osh, nullptr, D3DTEXF_NONE);
    const bool copied = src && in && out && SUCCEEDED(g_d9->StretchRect(src, nullptr, in, nullptr, D3DTEXF_NONE));
    if (!copied) {
        g_rtStatus = "copy into the shared texture failed";
    } else if (pipelined) {
        // Last frame's result over this frame's scene (after this frame's scene was copied out).
        if (g_haveResult) g_d9->StretchRect(out, nullptr, src, nullptr, D3DTEXF_NONE);
        g_haveResult = false;
        g_pend = CaptureParams(depthOk);
        g_inFlightTimeout = timeout;
        g_query->Issue(D3DISSUE_END);
        g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH);  // submit, don't wait
        g_queryPending = true;
    } else if (WaitGpu()) {
        Signal(CaptureParams(depthOk), timeout);
        if (Collect()) g_d9->StretchRect(out, nullptr, src, nullptr, D3DTEXF_NONE);
    }
    if (src) src->Release();
    if (in) in->Release();
    if (out) out->Release();
    if (dsrc) dsrc->Release();
    if (dsh) dsh->Release();
    if (osh) osh->Release();
    if (g_rtFailed || !copied) return;

    QueryPerformanceCounter(&t2);
    QueryPerformanceFrequency(&f);
    const double total = (t2.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    const double drain = drainMs * 1000.0 / f.QuadPart;
    g_rtMsAvg = g_rtFrames ? g_rtMsAvg * 0.95 + total * 0.05 : total;
    g_rtWaitAvg = g_rtFrames ? g_rtWaitAvg * 0.95 + drain * 0.05 : drain;
    if (++g_rtFrames == 1 || g_rtFrames % 600 == 0)
        Log("round trip %llu (%s): %.2f ms on the game's render thread (%.2f ms collecting last frame), helper's own %.2f ms, "
            "%u camera cuts so far",
            static_cast<unsigned long long>(g_rtFrames), pipelined ? "pipelined" : "waiting", total, drain, g_ctl->helperGpuMs, g_cuts);
    g_rtStatus = Format("%s%s: %.2f ms per frame on the game's thread (helper %.2f ms). %s", pipelined ? "Pipelined " : "",
                        g_rtMode == 3 ? (g_pend.mode == kModeDlaa || !pipelined ? "DLAA" : "DLAA (waiting for the 3D scene)")
                        : g_rtMode == 2 ? "Invert test" : "Copy test",
                        g_rtMsAvg, g_ctl->helperGpuMs, g_ctl->helperStatus);
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
