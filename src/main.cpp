// RE6DLSS: DLAA for Resident Evil 6 (32-bit D3D9; RenoDX runs it on D3D11). 0.0.x is the probe:
// the frame capture tool that finds the scene colour, depth, the motion blur velocity map and the
// camera matrices (docs/FINDINGS.md). Started from RE2DLSS-DX11's D3D11 probe.
//
// RE6 records D3D9 calls; RenoDX's D3D9->D3D11 proxy replays them on its own device without add-on
// events, so this probe follows the game's D3D9 calls: textures bound per sampler slot
// (sampler_with_resource_view descriptors), and shader constants pushed as registers
// (push_constants; D3D9 has no constant buffers). It dumps the vertex shader constants c0..c31 at
// the first scene draw after each render target change (view/projection matrices) and the pixel
// shader constants c0..c15 of every full-screen pass.
//
// Frame capture (overlay button, or F10 with ReShade.ini [RE6DLSS] CaptureHotkey=1) records
// two frames to RE6DLSS-frame-N.txt next to BH6.exe:
//   - every compute dispatch and every full-screen draw (up to 4 vertices / 6 indices, one
//     instance), with a stable id of its shaders (FNV-1a of the bytecode), the viewport, render
//     targets, and the textures, UAVs and constant buffers bound in the registers its shader
//     declares;
//   - other draws only as a count between those passes, plus copies, resolves and which device
//     context recorded them (deferred contexts' work appears where the immediate context runs it).
// D3D11 keeps bindings until they're changed, so they're tracked all the time, not only while
// recording. Shaders listed in ReShade.ini [RE2DLSS-DX11] DumpShaders=ps1234abcd,cs... also get
// their bytecode saved and their constant buffers dumped (from the CPU's last write to them).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shared.h"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = RD_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION = "DLAA (NVIDIA DLSS at native resolution) for " RD_GAME ", through a 64-bit helper process.";

namespace {

using namespace reshade::api;

constexpr int kCaptureHotkey = VK_F10;
constexpr int kCaptureFrames = 2;

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;
uint32_t g_bbWidth = 0, g_bbHeight = 0;  // the game's back buffer (set at the first present)
std::wstring g_exeDir;

void LogV(const char* fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%5.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_exeDir.assign(path, n);
    g_exeDir.resize(g_exeDir.find_last_of(L"\\/") + 1);
    g_log = _wfopen((g_exeDir + L"" RD_PRODUCT ".log").c_str(), L"w");
    Log(RD_PRODUCT " " RD_VERSION " loaded");
}

std::string Format(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// ---------------------------------------------------------------- crash catcher
//
// Logs the first access violations with this add-on on the stack (module + offset and a stack).

HMODULE g_self = nullptr;
void* g_veh = nullptr;
std::atomic<int> g_faults{0};

std::string ModuleOffset(const void* addr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &m) || !m)
        return Format("%p", addr);
    char name[MAX_PATH] = {};
    GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    return Format("%s+0x%llx", base ? base + 1 : name,
                  static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(addr) - reinterpret_cast<const uint8_t*>(m)));
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    void* frames[32];
    const USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    bool ours = false;
    for (USHORT i = 1; i < count && !ours; ++i) {
        HMODULE m = nullptr;
        ours = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCWSTR>(frames[i]), &m) && m == g_self;
    }
    if (!ours || ++g_faults > 3) return EXCEPTION_CONTINUE_SEARCH;
    Log("EXCEPTION 0x%08lx at %s (thread %lu)", code, ModuleOffset(e->ExceptionRecord->ExceptionAddress).c_str(),
        GetCurrentThreadId());
    for (USHORT i = 0; i < count; ++i) Log("  stack %2u: %s", i, ModuleOffset(frames[i]).c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- shader ids
//
// D3D11 shaders are separate objects (ReShade reports each as a pipeline with one shader); a hash
// of the bytecode names them the same way every run.

struct Decl {
    char cls;  // 't', 'u', 'b' or 's'
    uint32_t lo, hi;
};

struct ShaderInfo {
    char kind = 0;  // 'v', 'p', 'c' or 0
    uint32_t id = 0;
};

std::mutex g_pipeMutex;
std::unordered_map<uint64_t, ShaderInfo> g_shaders;              // shader object -> id
std::unordered_map<uint32_t, std::vector<Decl>> g_decls;         // id -> registers it declares
size_t g_counts[3] = {};                                          // vertex, pixel, compute shaders seen
std::unordered_set<uint32_t> g_dumpPs, g_dumpCs, g_dumpVs;        // ReShade.ini DumpShaders

uint32_t Fnv(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// The registers an SM 5.0 shader declares (DXBC RDEF part).
std::vector<Decl> ParseDecls(const uint8_t* p, size_t n) {
    std::vector<Decl> out;
    auto u32 = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        if (off + 4 <= n) memcpy(&v, p + off, 4);
        return v;
    };
    if (n < 32 || memcmp(p, "DXBC", 4) != 0) return out;
    const uint32_t parts = u32(28);
    for (uint32_t i = 0; i < parts && 32 + i * 4 + 4 <= n; ++i) {
        const size_t part = u32(32 + i * 4);
        if (part + 8 > n || memcmp(p + part, "RDEF", 4) != 0) continue;
        const size_t data = part + 8, size = u32(part + 4);
        if (data + size > n) continue;
        const uint32_t count = u32(data + 8), off = u32(data + 12);
        const uint32_t version = u32(data + 16);
        const bool sm51 = ((version >> 8) & 0xff) == 5 && (version & 0xff) == 1;
        const uint32_t stride = sm51 ? 40 : 32;
        for (uint32_t k = 0; k < count; ++k) {
            const size_t at = data + off + k * stride;
            if (at + stride > data + size) break;
            const uint32_t type = u32(at + 4);  // D3D_SHADER_INPUT_TYPE
            const char cls = type == 0 ? 'b' : type == 3 ? 's' : (type == 4 || type >= 6) && type != 7 ? 'u' : 't';
            const uint32_t lo = u32(at + 20), cnt = u32(at + 24);
            out.push_back({cls, lo, cnt ? lo + cnt - 1 : lo});
        }
    }
    return out;
}

void SaveBytecode(char kind, uint32_t id, const void* code, size_t size) {
    static std::mutex m;
    static std::unordered_set<uint64_t> saved;
    std::lock_guard<std::mutex> lock(m);
    if (!saved.insert((static_cast<uint64_t>(kind) << 32) | id).second) return;
    wchar_t name[96];
    swprintf(name, 96, L"" RD_PRODUCT "-%cs%08x.cso", kind, id);
    if (FILE* f = _wfopen((g_exeDir + name).c_str(), L"wb")) {
        fwrite(code, 1, size, f);
        fclose(f);
        Log("saved %cs%08x bytecode (%zu bytes)", kind, id, size);
    }
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipe) {
    for (uint32_t i = 0; i < count; ++i) {
        const pipeline_subobject& s = subobjects[i];
        char kind = 0;
        int slot = 0;
        if (s.type == pipeline_subobject_type::vertex_shader) kind = 'v', slot = 0;
        else if (s.type == pipeline_subobject_type::pixel_shader) kind = 'p', slot = 1;
        else if (s.type == pipeline_subobject_type::compute_shader) kind = 'c', slot = 2;
        if (!kind || !s.data) continue;
        const shader_desc& d = *static_cast<const shader_desc*>(s.data);
        if (!d.code || !d.code_size) continue;
        const uint32_t id = Fnv(d.code, d.code_size);
        if ((kind == 'p' && g_dumpPs.count(id)) || (kind == 'c' && g_dumpCs.count(id)) || (kind == 'v' && g_dumpVs.count(id)))
            SaveBytecode(kind, id, d.code, d.code_size);
        std::vector<Decl> decls = ParseDecls(static_cast<const uint8_t*>(d.code), d.code_size);
        std::lock_guard<std::mutex> lock(g_pipeMutex);
        g_decls[id] = std::move(decls);
        g_shaders[pipe.handle] = {kind, id};
        ++g_counts[slot];
        return;  // D3D11: one shader per object
    }
}

void OnDestroyPipeline(device*, pipeline pipe) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    g_shaders.erase(pipe.handle);
}

ShaderInfo Shader(pipeline pipe) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    auto it = g_shaders.find(pipe.handle);
    return it == g_shaders.end() ? ShaderInfo{} : it->second;
}

std::vector<Decl> Decls(uint32_t id, bool* known) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    auto it = g_decls.find(id);
    *known = it != g_decls.end() && !it->second.empty();
    return *known ? it->second : std::vector<Decl>{};
}

void LoadDumpList() {
    char buf[2048] = {};
    size_t size = sizeof(buf);
    if (!reshade::get_config_value(nullptr, RD_PRODUCT, "DumpShaders", buf, &size)) return;
    std::string s(buf, size && size <= sizeof(buf) ? size : strlen(buf));
    for (char& c : s)
        if (c == ' ' || c == ';' || c == '\0') c = ',';
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string t = s.substr(pos, end - pos);
        pos = end + 1;
        if (t.size() != 10) continue;
        const uint32_t id = static_cast<uint32_t>(strtoul(t.c_str() + 2, nullptr, 16));
        if (t.compare(0, 2, "ps") == 0) g_dumpPs.insert(id);
        else if (t.compare(0, 2, "cs") == 0) g_dumpCs.insert(id);
        else if (t.compare(0, 2, "vs") == 0) g_dumpVs.insert(id);
    }
    Log("dumping %zu pixel, %zu compute and %zu vertex shaders", g_dumpPs.size(), g_dumpCs.size(), g_dumpVs.size());
}

// ---------------------------------------------------------------- resources

const char* FormatName(format f) {
    switch (static_cast<uint32_t>(f)) {
        case 2: return "rgba32f";
        case 10: return "rgba16f";
        case 11: return "rgba16unorm";
        case 13: return "rgba16snorm";
        case 16: return "rg32f";
        case 19: return "r32g8x24_typeless";
        case 20: return "d32s8";
        case 21: return "r32f_x8x24";
        case 23: return "rgb10a2_typeless";
        case 24: return "rgb10a2unorm";
        case 26: return "r11g11b10f";
        case 27: return "rgba8_typeless";
        case 28: return "rgba8unorm";
        case 29: return "rgba8srgb";
        case 33: return "rg16_typeless";
        case 34: return "rg16f";
        case 35: return "rg16unorm";
        case 36: return "rg16uint";
        case 37: return "rg16snorm";
        case 39: return "r32_typeless";
        case 40: return "d32f";
        case 41: return "r32f";
        case 42: return "r32uint";
        case 44: return "r24g8_typeless";
        case 45: return "d24s8";
        case 46: return "r24x8";
        case 49: return "rg8unorm";
        case 50: return "rg8uint";
        case 51: return "rg8snorm";
        case 53: return "r16_typeless";
        case 54: return "r16f";
        case 55: return "d16";
        case 56: return "r16unorm";
        case 57: return "r16uint";
        case 61: return "r8unorm";
        case 62: return "r8uint";
        case 87: return "bgra8unorm";
        default: return nullptr;
    }
}

struct ViewInfo {
    resource res;
    resource_view_desc desc;
};

std::mutex g_resMutex;
std::unordered_map<uint64_t, resource_desc> g_resources;
std::unordered_map<uint64_t, ViewInfo> g_views;

// Constant buffers' contents as the CPU last wrote them (Map/Unmap, UpdateSubresource), for
// dumping the constants of shaders in the dump list. Only buffers of 4 KB or less.
std::unordered_map<uint64_t, std::vector<uint8_t>> g_cbData;
std::unordered_map<uint64_t, void*> g_mapped;

void OnInitResource(device*, const resource_desc& desc, const subresource_data* initial, resource_usage, resource r) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_resources[r.handle] = desc;
    if (desc.type == resource_type::buffer && (desc.usage & resource_usage::constant_buffer) != 0 && desc.buffer.size <= 4096) {
        std::vector<uint8_t>& v = g_cbData[r.handle];
        v.assign(static_cast<size_t>(desc.buffer.size), 0);
        if (initial && initial->data) memcpy(v.data(), initial->data, v.size());
    }
}

void OnDestroyResource(device*, resource r) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_resources.erase(r.handle);
    g_cbData.erase(r.handle);
    g_mapped.erase(r.handle);
}

void OnInitView(device*, resource r, resource_usage, const resource_view_desc& desc, resource_view v) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_views[v.handle] = {r, desc};
}

void OnDestroyView(device*, resource_view v) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_views.erase(v.handle);
}

void OnMapBuffer(device*, resource r, uint64_t offset, uint64_t, map_access access, void** data) {
    if (access == map_access::read_only || !data || !*data || offset) return;
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (g_cbData.count(r.handle)) g_mapped[r.handle] = *data;
}

void OnUnmapBuffer(device*, resource r) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    auto m = g_mapped.find(r.handle);
    if (m == g_mapped.end()) return;
    std::vector<uint8_t>& v = g_cbData[r.handle];
    memcpy(v.data(), m->second, v.size());
    g_mapped.erase(m);
}

void CopyCb(resource r, const void* data, uint64_t offset, uint64_t size) {
    if (!data) return;
    std::lock_guard<std::mutex> lock(g_resMutex);
    auto it = g_cbData.find(r.handle);
    if (it == g_cbData.end() || offset >= it->second.size()) return;
    if (size == UINT64_MAX || offset + size > it->second.size()) size = it->second.size() - offset;
    memcpy(it->second.data() + offset, data, static_cast<size_t>(size));
}

bool OnUpdateBuffer(device*, const void* data, resource r, uint64_t offset, uint64_t size) {
    CopyCb(r, data, offset, size);
    return false;
}

bool OnUpdateBufferCommand(command_list*, const void* data, resource r, uint64_t offset, uint64_t size) {
    CopyCb(r, data, offset, size);
    return false;
}

bool FindView(resource_view v, ViewInfo* out) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    auto it = g_views.find(v.handle);
    if (it == g_views.end()) return false;
    *out = it->second;
    return true;
}

std::string Describe(device* dev, resource r) {
    if (!r.handle) return "null";
    std::string id = Format("#%05llx", (r.handle >> 4) & 0xfffff);
    resource_desc d;
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        auto it = g_resources.find(r.handle);
        d = it != g_resources.end() ? it->second : dev->get_resource_desc(r);
    }
    if (d.type == resource_type::buffer) return id + Format(" buf %llu", d.buffer.size);
    const char* fn = FormatName(d.texture.format);
    std::string fmt = fn ? fn : Format("fmt%u", static_cast<uint32_t>(d.texture.format));
    std::string s = id + Format(" tex %ux%u %s", d.texture.width, d.texture.height, fmt.c_str());
    if (d.texture.depth_or_layers > 1) s += Format(" x%u", d.texture.depth_or_layers);
    if (d.texture.levels > 1) s += Format(" mips%u", d.texture.levels);
    return s;
}

std::string DescribeView(device* dev, resource_view v) {
    if (!v.handle) return "null";
    ViewInfo info;
    if (!FindView(v, &info)) return Format("view %llx (unknown)", v.handle);
    std::string s = Describe(dev, info.res);
    if (const char* fn = FormatName(info.desc.format)) s += Format(" as %s", fn);
    if (info.desc.type == resource_view_type::texture_2d && info.desc.texture.first_level)
        s += Format(" mip%u", info.desc.texture.first_level);
    return s;
}

// ---------------------------------------------------------------- binding state (always tracked)

struct Stage {
    ShaderInfo shader;
    std::map<uint32_t, resource_view> srv, uav;
    std::map<uint32_t, buffer_range> cb;
};

struct Context {
    Stage vs, ps, cs;
    float vsConst[256 * 4] = {}, psConst[224 * 4] = {};  // D3D9 float constant registers
    bool dumpVsNext = false;
    bool sceneTarget = false;  // render target 0 is back-buffer sized (jitter)
    int jitteredReg = -1;      // first register of our jittered fViewProj copy on the device, -1 none
    viewport vp = {};
    std::vector<resource_view> rtvs;
    resource_view dsv = {0};
    // capture
    std::vector<std::string> lines;
    uint32_t draws = 0;
    uint64_t id = 0;
};

std::mutex g_ctxMutex;
std::unordered_map<uint64_t, Context> g_contexts;                           // device context -> state
std::unordered_map<uint64_t, std::vector<std::string>> g_finished;         // ID3D11CommandList -> its lines
uint64_t g_nextCtx = 1;
uint64_t g_immediate = 0;  // the immediate context (the one that presents)
std::atomic<uint64_t> g_executes{0}, g_deferredSeen{0};

Context& Ctx(command_list* cl) {  // g_ctxMutex held
    Context& c = g_contexts[cl->get_native()];
    if (!c.id) c.id = g_nextCtx++;
    return c;
}

Stage* StageOf(Context& c, shader_stage s) {
    if ((s & shader_stage::compute) != 0) return &c.cs;
    if ((s & shader_stage::pixel) != 0) return &c.ps;
    if ((s & shader_stage::vertex) != 0) return &c.vs;
    return nullptr;
}

#include "jitter.inl"
#include "roundtrip.inl"

void MarkTonemap(command_list* cl) {  // the tonemap draw ends the scene phase (jitter)
    if (g_pastTonemap) return;
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    if (Ctx(cl).ps.shader.id == kTonemapPs) g_pastTonemap = true;
}

void OnInitDevice(device* dev) {
    if (dev->get_api() != device_api::d3d9) {
        Log("device: API %u", static_cast<unsigned>(dev->get_api()));
        return;
    }
    auto* d9 = reinterpret_cast<IDirect3DDevice9*>(dev->get_native());
    IDirect3DDevice9Ex* ex = nullptr;
    const HRESULT hr = d9 ? d9->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&ex)) : E_POINTER;
    Log("device: D3D9 %p, IDirect3DDevice9Ex: %s (hr 0x%08lx)", d9, SUCCEEDED(hr) && ex ? "yes" : "no", hr);
    if (ex) {
        g_d9 = ex;  // kept for the device's lifetime (round trip)
        g_noEx = false;
        ex->Release();
    } else {
        g_noEx = true;
    }
}

void OnInitCommandList(command_list* cl) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Ctx(cl);
}

void OnDestroyCommandList(command_list* cl) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    g_contexts.erase(cl->get_native());
    g_finished.erase(cl->get_native());
}

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    const ShaderInfo s = pipe.handle ? Shader(pipe) : ShaderInfo{};
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    if ((stages & pipeline_stage::compute_shader) != 0) c.cs.shader = s;
    if ((stages & pipeline_stage::pixel_shader) != 0) c.ps.shader = s;
    if ((stages & pipeline_stage::vertex_shader) != 0) c.vs.shader = s;
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update& u) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    Stage* st = StageOf(c, stages);
    if (!st) return;
    for (uint32_t i = 0; i < u.count; ++i) {
        const uint32_t slot = u.binding + i;
        switch (u.type) {
            case descriptor_type::constant_buffer: st->cb[slot] = static_cast<const buffer_range*>(u.descriptors)[i]; break;
            case descriptor_type::shader_resource_view:  // = texture_shader_resource_view
            case descriptor_type::buffer_shader_resource_view:
                st->srv[slot] = static_cast<const resource_view*>(u.descriptors)[i];
                break;
            case descriptor_type::sampler_with_resource_view:  // D3D9 SetTexture
                st->srv[slot] = static_cast<const sampler_with_resource_view*>(u.descriptors)[i].view;
                break;
            case descriptor_type::unordered_access_view:  // = texture_unordered_access_view
            case descriptor_type::buffer_unordered_access_view:
                st->uav[slot] = static_cast<const resource_view*>(u.descriptors)[i];
                break;
            default: break;
        }
    }
}

// D3D9 SetVertexShaderConstantF / SetPixelShaderConstantF: `first` and `count` are in floats
// (register * 4) for the float constant parameter.
void OnPushConstants(command_list* cl, shader_stage stages, pipeline_layout, uint32_t param, uint32_t first,
                     uint32_t count, const void* values) {
    if (!values) return;
    static std::atomic<int> logged{0};
    if (logged < 24) {  // learn how D3D9 constants arrive (stage, layout parameter, first, count)
        ++logged;
        Log("push_constants: stages 0x%x param %u first %u count %u", static_cast<unsigned>(stages), param, first, count);
    }
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    float* dst = (stages & shader_stage::vertex) != 0 ? c.vsConst : (stages & shader_stage::pixel) != 0 ? c.psConst : nullptr;
    const uint32_t cap = dst == c.vsConst ? 256 * 4 : 224 * 4;
    if (!dst || first >= cap) return;
    if (first + count > cap) count = cap - first;
    memcpy(dst + first, values, count * 4);
    if (dst == c.vsConst && c.jitteredReg >= 0 && first <= static_cast<uint32_t>(c.jitteredReg) * 4 &&
        first + count >= static_cast<uint32_t>(c.jitteredReg) * 4 + 16)
        c.jitteredReg = -1;  // the game rewrote the whole jittered matrix
}

void OnBindViewports(command_list* cl, uint32_t first, uint32_t count, const viewport* vps) {
    if (first != 0 || !count) return;
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Ctx(cl).vp = vps[0];
}

// ---------------------------------------------------------------- capture

enum class Capture { Idle, Recording };
std::atomic<Capture> g_capture{Capture::Idle};
int g_framesLeft = 0;
int g_captureCount = 0;
std::string g_status;
std::vector<std::string> g_frame;  // g_ctxMutex
size_t g_passes = 0;

bool Recording() { return g_capture.load(std::memory_order_relaxed) == Capture::Recording; }

void FlushDraws(Context& c) {
    if (!c.draws) return;
    c.lines.push_back(Format("  draw x%u", c.draws));
    c.draws = 0;
}

void AddLocked(Context& c, std::string line) {
    FlushDraws(c);
    c.lines.push_back("  " + std::move(line));
}

// Render target snapshots (first captured frame only): when the game switches away from a render
// target of back-buffer size, a copy of it is read back and saved every 4th pixel to
// RE6DLSS-rt-NN-<id>.bin (header: 'RT4' + NUL, width, height, format, step; then rows of
// pixels). tools/rt2png.py turns them into PNGs.
int g_snapshots = 0;
bool g_snapFrame = false;
constexpr int kMaxSnapshots = 24;

uint32_t PixelBytes(format f) {
    switch (static_cast<uint32_t>(f)) {
        case 10: return 8;   // rgba16f
        case 2: return 16;   // rgba32f
        case 16: return 8;   // rg32f
        case 34: case 35: case 41: case 28: case 87: case 24: case 26: return 4;
        case 54: case 56: return 2;
        default: return 0;
    }
}

void Snapshot(command_list* cl, resource_view rtv) {
    if (!g_snapFrame || g_snapshots >= kMaxSnapshots || !rtv.handle) return;
    device* dev = cl->get_device();
    ViewInfo info;
    if (!FindView(rtv, &info)) return;
    const resource_desc d = dev->get_resource_desc(info.res);
    if (d.type != resource_type::texture_2d || d.texture.width != g_bbWidth || d.texture.height != g_bbHeight) return;
    const uint32_t bpp = PixelBytes(d.texture.format);
    if (!bpp) return;
    resource_desc rd = d;
    rd.texture.levels = 1;
    rd.heap = memory_heap::readback;
    rd.usage = resource_usage::copy_dest;
    rd.flags = resource_flags::none;
    resource dst = {};
    if (!dev->create_resource(rd, nullptr, resource_usage::copy_dest, &dst)) {
        Log("snapshot: readback texture failed (format %u)", static_cast<unsigned>(d.texture.format));
        return;
    }
    cl->copy_texture_region(info.res, 0, nullptr, dst, 0, nullptr);
    subresource_data m = {};
    if (dev->map_texture_region(dst, 0, nullptr, map_access::read_only, &m) && m.data) {
        const int n = ++g_snapshots;
        wchar_t name[96];
        swprintf(name, 96, L"" RD_PRODUCT "-rt-%02d-%05llx.bin", n, (info.res.handle >> 4) & 0xfffff);
        if (FILE* f = _wfopen((g_exeDir + name).c_str(), L"wb")) {
            const uint32_t step = 4, w = d.texture.width / step, h = d.texture.height / step;
            const uint32_t hdr[5] = {0x00345452, w, h, static_cast<uint32_t>(d.texture.format), step};
            fwrite(hdr, 4, 5, f);
            std::vector<uint8_t> row(static_cast<size_t>(w) * bpp);
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(m.data) + static_cast<size_t>(y * step) * m.row_pitch;
                for (uint32_t x = 0; x < w; ++x) memcpy(&row[x * bpp], src + static_cast<size_t>(x * step) * bpp, bpp);
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
        dev->unmap_texture_region(dst, 0);
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        AddLocked(Ctx(cl), Format("SNAPSHOT %d of %s", n, Describe(dev, info.res).c_str()));
    }
    dev->destroy_resource(dst);
}

void OnBindRenderTargets(command_list* cl, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    if (Recording() && g_snapFrame) {  // the target being switched away from
        resource_view prev = {0};
        {
            std::lock_guard<std::mutex> lock(g_ctxMutex);
            Context& c = Ctx(cl);
            if (!c.rtvs.empty()) prev = c.rtvs[0];
        }
        if (prev.handle && (!count || rtvs[0].handle != prev.handle)) Snapshot(cl, prev);
    }
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    c.rtvs.assign(rtvs, rtvs + count);
    c.dsv = dsv;
    c.sceneTarget = false;
    if (count && rtvs[0].handle && g_bbWidth) {
        ViewInfo vi;
        if (FindView(rtvs[0], &vi)) {
            const resource_desc rd = cl->get_device()->get_resource_desc(vi.res);
            c.sceneTarget = rd.texture.width == g_bbWidth && rd.texture.height == g_bbHeight;
            if (c.sceneTarget && !g_depthRes.handle && !g_pastTonemap) g_depthRes = vi.res;  // the frame's pre-pass
        }
    }
    if (!Recording()) return;
    c.dumpVsNext = true;
    device* dev = cl->get_device();
    std::string s;
    for (uint32_t i = 0; i < count; ++i) s += " " + DescribeView(dev, rtvs[i]);
    if (dsv.handle) s += " depth " + DescribeView(dev, dsv);
    AddLocked(c, "render targets:" + s);
}

std::string Floats(const uint32_t* v, size_t n) {
    std::string s;
    for (size_t k = 0; k < n; ++k) {
        float f;
        memcpy(&f, &v[k], 4);
        const uint32_t e = (v[k] >> 23) & 0xff;
        if (v[k] == 0) s += "          0";
        else if (e > 90 && e < 165) s += Format(" %10.6g", f);
        else s += Format("  0x%08x", v[k]);
    }
    return s;
}

void DumpCb(Context& c, uint32_t slot, const buffer_range& b) {
    std::vector<uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        auto it = g_cbData.find(b.buffer.handle);
        if (it == g_cbData.end()) {
            AddLocked(c, Format("    b%u: contents not tracked (larger than 4 KB?)", slot));
            return;
        }
        data = it->second;
    }
    const size_t off = static_cast<size_t>(b.offset);
    if (off >= data.size()) return;
    std::vector<uint32_t> v((data.size() - off) / 4);
    memcpy(v.data(), data.data() + off, v.size() * 4);
    AddLocked(c, Format("    b%u: %zu bytes (CPU's last write)", slot, v.size() * 4));
    for (size_t i = 0; i < v.size(); i += 4)
        AddLocked(c, Format("      c %4zu:", i * 4) + Floats(&v[i], v.size() - i < 4 ? v.size() - i : 4));
}

void WriteStage(device* dev, Context& c, const Stage& st, bool dump) {
    bool known = false;
    const std::vector<Decl> decls = Decls(st.shader.id, &known);
    auto declared = [&](char cls, uint32_t reg) {
        if (!known) return reg < 16;
        for (const Decl& d : decls)
            if (d.cls == cls && reg >= d.lo && reg <= d.hi) return true;
        return false;
    };
    if (!known) AddLocked(c, "    (shader declarations unknown: first 16 slots shown)");
    for (const auto& [slot, v] : st.srv)
        if (v.handle && declared('t', slot)) AddLocked(c, Format("    t%u = %s", slot, DescribeView(dev, v).c_str()));
    for (const auto& [slot, v] : st.uav)
        if (v.handle && declared('u', slot)) AddLocked(c, Format("    u%u = %s", slot, DescribeView(dev, v).c_str()));
    for (const auto& [slot, b] : st.cb) {
        if (!b.buffer.handle || !declared('b', slot)) continue;
        AddLocked(c, Format("    b%u = cb %s +%llu", slot, Describe(dev, b.buffer).c_str(), b.offset));
        if (dump) DumpCb(c, slot, b);
    }
}

void DumpRegs(Context& c, const char* what, const float* regs, uint32_t n, bool skipZero) {
    for (uint32_t r = 0; r < n; ++r) {
        const float* v = regs + r * 4;
        if (skipZero && !v[0] && !v[1] && !v[2] && !v[3]) continue;
        uint32_t u[4];
        memcpy(u, v, 16);
        AddLocked(c, Format("    %s c%-3u", what, r) + Floats(u, 4));
    }
}

void Pass(command_list* cl, bool compute, const std::string& head) {
    device* dev = cl->get_device();
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    ++g_passes;
    if (compute) {
        AddLocked(c, Format("DISPATCH cs%08x ", c.cs.shader.id) + head);
        WriteStage(dev, c, c.cs, g_dumpCs.count(c.cs.shader.id) != 0);
        return;
    }
    std::string rts;
    for (resource_view v : c.rtvs) rts += " " + DescribeView(dev, v);
    if (c.dsv.handle) rts += " depth " + DescribeView(dev, c.dsv);
    AddLocked(c, Format("FULLSCREEN ps%08x vs%08x ", c.ps.shader.id, c.vs.shader.id) + head +
                     Format(" viewport %gx%g at %g,%g ->%s", c.vp.width, c.vp.height, c.vp.x, c.vp.y, rts.c_str()));
    WriteStage(dev, c, c.ps, g_dumpPs.count(c.ps.shader.id) != 0);
    DumpRegs(c, "ps", c.psConst, 16, true);
    if (!c.ps.uav.empty()) {
        for (const auto& [slot, v] : c.ps.uav)
            if (v.handle) AddLocked(c, Format("    ps u%u = %s", slot, DescribeView(dev, v).c_str()));
    }
}

bool OnDispatch(command_list* cl, uint32_t x, uint32_t y, uint32_t z) {
    if (Recording()) Pass(cl, true, Format("%u x %u x %u", x, y, z));
    return false;
}

void Other(command_list* cl) {
    device* dev = cl->get_device();
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    if (c.dumpVsNext) {  // first scene draw after a render target change: shaders, textures, VS constants
        c.dumpVsNext = false;
        AddLocked(c, Format("SCENE DRAW vs%08x ps%08x (first after render target change)", c.vs.shader.id, c.ps.shader.id));
        WriteStage(dev, c, c.ps, false);
        DumpRegs(c, "vs", c.vsConst, 32, false);
    }
    ++c.draws;
}

bool OnDraw(command_list* cl, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
    MaybeRoundTrip(cl);
    MarkTonemap(cl);
    MaybeJitter(cl);
    if (!Recording()) return false;
    if (vertices <= 4 && instances == 1) Pass(cl, false, Format("draw %u verts", vertices));
    else Other(cl);
    return false;
}

bool OnDrawIndexed(command_list* cl, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
    MaybeJitter(cl);
    if (!Recording()) return false;
    if (indices <= 6 && instances == 1) Pass(cl, false, Format("draw %u indices", indices));
    else Other(cl);
    return false;
}

bool OnIndirect(command_list* cl, indirect_command type, resource, uint64_t, uint32_t count, uint32_t) {
    if (!Recording()) return false;
    if (type == indirect_command::dispatch) Pass(cl, true, Format("indirect x%u", count));
    else Other(cl);
    return false;
}

void Note(command_list* cl, std::string s) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    AddLocked(Ctx(cl), std::move(s));
}

bool OnCopyResource(command_list* cl, resource src, resource dst) {
    if (Recording()) Note(cl, Format("copy %s -> %s", Describe(cl->get_device(), src).c_str(), Describe(cl->get_device(), dst).c_str()));
    return false;
}

bool OnCopyTexture(command_list* cl, resource src, uint32_t, const subresource_box*, resource dst, uint32_t,
                   const subresource_box*, filter_mode) {
    if (Recording())
        Note(cl, Format("copy region %s -> %s", Describe(cl->get_device(), src).c_str(), Describe(cl->get_device(), dst).c_str()));
    return false;
}

bool OnResolve(command_list* cl, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, uint32_t,
               uint32_t, uint32_t, format) {
    if (Recording())
        Note(cl, Format("resolve %s -> %s", Describe(cl->get_device(), src).c_str(), Describe(cl->get_device(), dst).c_str()));
    return false;
}

bool OnClearRtv(command_list* cl, resource_view rtv, const float color[4], uint32_t, const rect*) {
    if (Recording())
        Note(cl, Format("clear %s to %g %g %g %g", DescribeView(cl->get_device(), rtv).c_str(), color[0], color[1], color[2], color[3]));
    return false;
}

// D3D11: FinishCommandList (secondary = the new command list, cl = the deferred context) and
// ExecuteCommandList (cl = the context running it). The deferred context's lines move with the
// command list into the stream of the context that runs it.
void OnSecondary(command_list* cl, command_list* secondary) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    const uint64_t key = secondary->get_native();
    auto f = g_finished.find(key);
    if (f == g_finished.end()) {  // FinishCommandList
        Context& c = Ctx(cl);
        ++g_deferredSeen;
        if (!Recording()) return;
        FlushDraws(c);
        std::vector<std::string>& out = g_finished[key];
        out.push_back(Format("  -- command list from deferred context C%llu, %zu events --", c.id, c.lines.size()));
        for (std::string& s : c.lines) out.push_back("  " + std::move(s));
        c.lines.clear();
        return;
    }
    ++g_executes;  // ExecuteCommandList
    Context& c = Ctx(cl);
    if (Recording()) {
        FlushDraws(c);
        for (std::string& s : f->second) c.lines.push_back(std::move(s));
    }
    g_finished.erase(f);
}

void WriteCapture() {
    std::vector<std::string> frame;
    size_t passes;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        frame.swap(g_frame);
        for (auto& [k, c] : g_contexts) c.lines.clear(), c.draws = 0;
        g_finished.clear();
        passes = g_passes;
    }
    ++g_captureCount;
    std::wstring name = g_exeDir + L"" RD_PRODUCT "-frame-" + std::to_wstring(g_captureCount) + L".txt";
    FILE* f = _wfopen(name.c_str(), L"w");
    if (!f) {
        g_status = "couldn't write the capture file";
        return;
    }
    for (const std::string& s : frame) {
        fputs(s.c_str(), f);
        fputc('\n', f);
    }
    fclose(f);
    g_status = Format("Capture %d: %zu lines, %zu passes -> " RD_PRODUCT "-frame-%d.txt", g_captureCount, frame.size(),
                      passes, g_captureCount);
    Log("%s", g_status.c_str());
}

void StartCapture() {
    if (g_capture.load() != Capture::Idle) return;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        for (auto& [k, c] : g_contexts) c.lines.clear(), c.draws = 0;
        g_finished.clear();
        g_frame.clear();
        g_passes = 0;
        g_frame.push_back("==== capture start (deferred work recorded before this point is missing)");
    }
    g_framesLeft = kCaptureFrames;
    g_snapshots = 0;
    g_snapFrame = true;
    g_capture = Capture::Recording;
    Log("capture started");
}

bool g_keyWasDown = false;
bool g_captureHotkey = false;
std::atomic<uint64_t> g_presents{0};

void OnPresent(command_queue* q, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    // Only the game's own D3D9 frames (RenoDX's D3D11 proxy may present too).
    const device_api api = sc->get_device()->get_api();
    if (api != device_api::d3d9) {
        static bool logged = false;
        if (!logged) logged = true, Log("present on API %u ignored (not D3D9)", static_cast<unsigned>(api));
        return;
    }
    ++g_presents;
    g_rtDoneThisFrame = false;
    g_depthRes = {0};
    NextJitter();
    if (!g_immediate) {
        g_immediate = q->get_native();
        const resource_desc d = sc->get_device()->get_resource_desc(sc->get_current_back_buffer());
        g_bbWidth = d.texture.width;
        g_bbHeight = d.texture.height;
        const char* fn = FormatName(d.texture.format);
        Log("first present: API D3D9, back buffer %ux%u %s (format %u)", d.texture.width, d.texture.height, fn ? fn : "?",
            static_cast<unsigned>(d.texture.format));
    }
    if (g_capture.load() == Capture::Recording) {
        {
            std::lock_guard<std::mutex> lock(g_ctxMutex);
            Context& c = g_contexts[q->get_native()];  // D3D11: the queue is the immediate context
            if (!c.id) c.id = g_nextCtx++;
            FlushDraws(c);
            g_frame.push_back(Format("---- immediate context C%llu, %zu events", c.id, c.lines.size()));
            for (std::string& s : c.lines) g_frame.push_back(std::move(s));
            c.lines.clear();
            for (auto& [k, other] : g_contexts) {  // deferred work never executed (shouldn't happen)
                if (k == q->get_native() || other.lines.empty()) continue;
                g_frame.push_back(Format("---- deferred context C%llu, %zu events not yet in a command list", other.id,
                                         other.lines.size()));
            }
            g_frame.push_back(Format("==== PRESENT (back buffer %s), %d more", Describe(sc->get_device(), sc->get_current_back_buffer()).c_str(),
                                     g_framesLeft - 1));
        }
        g_snapFrame = false;  // snapshots: first captured frame only
        if (--g_framesLeft <= 0) {
            WriteCapture();
            g_capture = Capture::Idle;
        }
    }
    if (g_captureHotkey) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kCaptureHotkey) & 0x8000);
        if (down && !g_keyWasDown) StartCapture();
        g_keyWasDown = down;
    }
}

// ---------------------------------------------------------------- settings
//
// ReShade.ini [RE6DLSS]: AntiAliasing (0 off, 1 DLAA; default DLAA), DlssModel (0 Automatic,
// 1 K, 2 J, 3 L, 4 M). The developer tools (round trip test, jitter test, frame capture) are not
// saved, except CaptureHotkey.

int g_aa = 1;
int g_model = 0;
int g_debugRoundTrip = 0;  // 0 none, 1 copy, 2 invert (overrides DLAA while set)
bool g_jitterTest = false;
const char* const kModels[] = {"Automatic", "K", "J", "L", "M"};
const int kModelPreset[] = {-1, 11, 10, 12, 13};  // NVSDK_NGX_DLSS_Hint_Render_Preset_*

void ApplySettings() {
    g_rtMode = g_debugRoundTrip ? g_debugRoundTrip : (g_aa ? 3 : 0);
    g_jitterMode = g_jitterTest ? 2 : (g_aa || g_debugRoundTrip ? 1 : 0);
    g_dlssPreset = kModelPreset[g_model];
    g_rtFailed = false;
}

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "CaptureHotkey", g_captureHotkey);
    reshade::get_config_value(nullptr, RD_PRODUCT, "AntiAliasing", g_aa);
    reshade::get_config_value(nullptr, RD_PRODUCT, "DlssModel", g_model);
    if (g_aa < 0 || g_aa > 1) g_aa = 1;
    if (g_model < 0 || g_model > 4) g_model = 0;
    ApplySettings();
    Log("settings: anti-aliasing %s, DLSS model %s", g_aa ? "DLAA" : "off", kModels[g_model]);
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "AntiAliasing", g_aa);
    reshade::set_config_value(nullptr, RD_PRODUCT, "DlssModel", g_model);
    ApplySettings();
}

// ---------------------------------------------------------------- overlay

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void DrawOverlay(effect_runtime*) {
    ImGui::TextDisabled(RD_PRODUCT " " RD_VERSION);
    static const char* const kAa[] = {"Off", "DLAA"};
    if (ImGui::Combo("Anti-aliasing", &g_aa, kAa, 2)) SaveSettings();
    Help("DLAA: NVIDIA DLSS at native resolution, in place of the game's anti-aliasing (set the game's "
         "anti-aliasing to Off). Needs an NVIDIA RTX GPU and RenoDX. Costs about 3-4 ms per frame at 4K.");
    ImGui::BeginDisabled(!g_aa);
    if (ImGui::Combo("DLSS model", &g_model, kModels, 5)) SaveSettings();
    Help("Automatic: the model NVIDIA recommends (K for DLAA). The NVIDIA app's DLSS override, if set for "
         "this game, takes precedence.");
    ImGui::EndDisabled();

    const ImVec4 warn(1.0f, 0.75f, 0.3f, 1.0f);
    if (g_noEx)
        ImGui::TextColored(warn, "The game's device isn't D3D9Ex: install RenoDX for Resident Evil 6 (DLAA needs it).");
    else if (!g_d9)
        ImGui::TextDisabled("Waiting for the game's device...");
    else if (g_rtMode)
        ImGui::TextWrapped("%s", g_rtStatus.c_str());
    else
        ImGui::TextDisabled("Off.");

    if (!ImGui::CollapsingHeader("Developer tools")) return;
    static const char* const kTest[] = {"Off (normal)", "Copy (picture unchanged)", "Invert (picture inverted)"};
    if (ImGui::Combo("Helper round trip test", &g_debugRoundTrip, kTest, 3)) ApplySettings();
    if (ImGui::Checkbox("Jitter test (16x, the picture shakes)", &g_jitterTest)) ApplySettings();
    ImGui::TextDisabled("jitter %.3f, %.3f px; %u scene draws jittered last frame; camera matrix %s", g_jitterX,
                        g_jitterY, g_jitteredDrawsLast, g_havePrevVp ? "tracked" : "not seen");
    ImGui::BeginDisabled(g_capture.load() != Capture::Idle);
    if (ImGui::Button(g_captureHotkey ? "Capture frame (F10)" : "Capture frame")) StartCapture();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_capture.load() == Capture::Recording ? "recording..." : g_status.c_str());
    {
        std::lock_guard<std::mutex> lock(g_pipeMutex);
        ImGui::TextDisabled("shaders: %zu vertex, %zu pixel", g_counts[0], g_counts[1]);
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            OpenLog();
            g_self = module;
            g_veh = AddVectoredExceptionHandler(1, &OnException);
            LoadDumpList();
            LoadSettings();
            reshade::register_event<reshade::addon_event::init_device>(&OnInitDevice);
            reshade::register_event<reshade::addon_event::init_resource>(&OnInitResource);
            reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
            reshade::register_event<reshade::addon_event::init_resource_view>(&OnInitView);
            reshade::register_event<reshade::addon_event::destroy_resource_view>(&OnDestroyView);
            reshade::register_event<reshade::addon_event::map_buffer_region>(&OnMapBuffer);
            reshade::register_event<reshade::addon_event::unmap_buffer_region>(&OnUnmapBuffer);
            reshade::register_event<reshade::addon_event::update_buffer_region>(&OnUpdateBuffer);
            reshade::register_event<reshade::addon_event::update_buffer_region_command>(&OnUpdateBufferCommand);
            reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
            reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
            reshade::register_event<reshade::addon_event::init_command_list>(&OnInitCommandList);
            reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyCommandList);
            reshade::register_event<reshade::addon_event::execute_secondary_command_list>(&OnSecondary);
            reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
            reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
            reshade::register_event<reshade::addon_event::bind_viewports>(&OnBindViewports);
            reshade::register_event<reshade::addon_event::push_constants>(&OnPushConstants);
            reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
            reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
            reshade::register_event<reshade::addon_event::draw>(&OnDraw);
            reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
            reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(&OnIndirect);
            reshade::register_event<reshade::addon_event::copy_resource>(&OnCopyResource);
            reshade::register_event<reshade::addon_event::copy_texture_region>(&OnCopyTexture);
            reshade::register_event<reshade::addon_event::resolve_texture_region>(&OnResolve);
            reshade::register_event<reshade::addon_event::clear_render_target_view>(&OnClearRtv);
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_overlay(RD_PRODUCT, &DrawOverlay);
            break;
        case DLL_PROCESS_DETACH:
            StopHelper();
            if (g_veh) RemoveVectoredExceptionHandler(g_veh);
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
