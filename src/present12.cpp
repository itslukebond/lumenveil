// D3D12 presentation of the game's native D3D11 frames ([d3d12] present=1), the Community Shaders pattern.
// The game keeps rendering on its own D3D11 device. Its swapchain lives on a hidden window and is never
// presented, so back buffer 0 is always the one it draws into. At Present that buffer is copied into a
// texture shared with D3D12 and shown through a D3D12 flip swapchain on the game window (the API DLSS
// Frame Generation needs). A shared fence orders the two queues on the GPU; the CPU never waits on it.
// With [fg] enable=1 that swapchain comes from Streamline, which adds DLSS Frame Generation at Present, or with the
// FSR backend from AMD's frame interpolation swapchain (fsr.cpp), or with the XeSS backend from Intel's frame generation
// swapchain (xess.cpp). One backend per launch (fg_select.h).
#include "common.h"
#include "fg_gate.h"
#include "fg_select.h"
#include "xess_rules.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_matrix_helpers.h>

ID3D12Device* g_d3d12;
ID3D12CommandQueue* g_d3d12Queue;

// sl.interposer.dll is loaded at runtime from ffxv_dlss\streamline; these give the SL headers' inline helpers
// (slDLSSGSetOptions, slReflexSleep, ...) the core entry points they call.
static HMODULE g_sl;
#define SL_FORWARD(name, params, args) \
    extern "C" sl::Result name params { static auto* fn = (PFun_##name*)GetProcAddress(g_sl, #name); return fn args; }
SL_FORWARD(slInit, (const sl::Preferences& p, uint64_t v), (p, v))
SL_FORWARD(slSetD3DDevice, (void* d), (d))
SL_FORWARD(slUpgradeInterface, (void** i), (i))
SL_FORWARD(slGetFeatureFunction, (sl::Feature f, const char* n, void*& p), (f, n, p))
SL_FORWARD(slGetNewFrameToken, (sl::FrameToken*& t, const uint32_t* i), (t, i))
SL_FORWARD(slSetConstants, (const sl::Constants& c, const sl::FrameToken& t, const sl::ViewportHandle& v), (c, t, v))
SL_FORWARD(slSetTagForFrame, (const sl::FrameToken& t, const sl::ViewportHandle& v, const sl::ResourceTag* r, uint32_t n, sl::CommandBuffer* c), (t, v, r, n, c))

namespace {
constexpr UINT kBuffers = 3;
ID3D11Device5* g_dev11;
ID3D11DeviceContext4* g_ctx11;
IDXGISwapChain3* g_sc;    // ours, on the game window (Streamline's proxy when frame generation is on)
IDXGISwapChain* g_game;   // the game's, on the hidden window
HWND g_wnd, g_dummy;
UINT g_scFlags;
bool g_tearing, g_busy;
// Two shared fences, each signalled from one API only (a fence signalled by both can be set backwards when the
// queues run out of order, and a wait on the higher value then never returns):
ID3D12Fence* g_fence;   // D3D12 -> D3D11: only the D3D12 queue signals it
ID3D11Fence* g_fence11;
UINT64 g_fenceVal;
ID3D12Fence* g_up;      // D3D11 -> D3D12: only the D3D11 context signals it
ID3D11Fence* g_up11;
UINT64 g_upVal;
UINT64 g_copyDone;                  // fence value once D3D12 has copied the shared texture out (signalled before Present)
HANDLE g_idleEvent;
HANDLE g_drainEvent;                // fgDrained's own wait; g_idleEvent belongs to waitIdle
ID3D12Resource* g_shared12;
ID3D11Texture2D* g_shared11;
ID3D11Texture2D* g_gameBB;
ID3D12CommandAllocator* g_alloc;
ID3D12GraphicsCommandList* g_cl[kBuffers];
bool g_fsrFg, g_fsrGen;             // FSR frame generation swapchain in use; generating this frame
bool g_xessFg, g_xessGen;           // XeSS-FG is this launch's backend; generating this frame
bool g_xessSc;                      // Intel's frame generation swapchain is the one in g_sc
bool g_xessRefused;                 // XeSS-FG was chosen but the format the game presents in now rules it out
IDXGIFactory2* g_factory;           // the game's factory and the original creation function: a later swapchain for the
PFN_CreateSCHwnd g_create;          // same window (XeSS-FG once the game's format allows it) is made like the first
int g_colorSpace = -1;              // the colour space the game last set, for a swapchain made later
unsigned g_xeId;                    // XeSS-FG / XeLL frame id: XeLL's frame, the tags and the Present of one rendered frame
UINT g_vendor;                      // adapter PCI vendor: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
// A vendor's frame generation swapchain replaces ours (FSR or XeSS): no Streamline, the presenter paces frames itself.
bool proxyFg() { return g_fsrFg || g_xessSc; }

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
int ini(const wchar_t* sec, const wchar_t* key, int def) { return GetPrivateProfileIntW(sec, key, def, path(L"ffxv_dlss.ini").c_str()); }

void waitIdle() {
    g_d3d12Queue->Signal(g_fence, ++g_fenceVal);
    g_fence->SetEventOnCompletion(g_fenceVal, g_idleEvent);
    WaitForSingleObject(g_idleEvent, 2000);
}

// HUD-less back buffer for FSR frame generation: main.cpp replays the game's final composite without the HUD into the
// current slot; FSR compares it with the real frame to keep the HUD out of interpolation. D3D11-created, shared.
// known-limit: three slots, not a fence, keep the game off one FSR's Present-time work may still read (the pacer holds the
// game one frame behind the GPU); add a fence after Present if HUD-less artefacts ever show.
constexpr UINT kHudless = 3;
ID3D11Texture2D* g_hudless11[kHudless];
ID3D11RenderTargetView* g_hudlessRTV[kHudless];
ID3D12Resource* g_hudless12[kHudless];
UINT g_hudlessSlot;
bool g_hudlessWritten;

void releaseHudless() {
    for (UINT i = 0; i < kHudless; ++i) { release(g_hudlessRTV[i]); release(g_hudless12[i]); release(g_hudless11[i]); }
    g_hudlessWritten = false;
}

// What the back buffer holds: 0 SDR, 1 HDR10 (PQ), 2 scRGB. The game can run HDR10 through NVAPI with Windows HDR off, so the
// DXGI output colour space reads SDR while the display is in PQ: NVAPI's HDR mode for the window's display decides too.
int g_encode = -1;  // unknown until the first refresh
float g_lum[2] = { 0.0001f, 1000.f };  // display min / max nits, for FSR's PQ -> linear

// NV_HDR_MODE of the display (2 UHDA = HDR10, 5 passthrough, 6 UHDA_NB), or -1 if NVAPI can't tell.
int nvapiHdrMode(const wchar_t* gdiName) {
    using QI = void*(__cdecl*)(unsigned);
    static QI qi;
    static bool tried;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryW(L"nvapi64.dll");
        qi = m ? (QI)GetProcAddress(m, "nvapi_QueryInterface") : nullptr;
        auto init = qi ? (int(__cdecl*)())qi(0x0150E828) : nullptr;  // NvAPI_Initialize
        if (!init || init() != 0) qi = nullptr;
    }
    if (!qi) return -1;
    auto byName = (int(__cdecl*)(const char*, unsigned*))qi(0xAE457190);  // NvAPI_DISP_GetDisplayIdByDisplayName
    auto hdr = (int(__cdecl*)(unsigned, void*))qi(0x351DA224);           // NvAPI_Disp_HdrColorControl
    char name[64] = {};
    unsigned id = 0;
    WideCharToMultiByte(CP_ACP, 0, gdiName, -1, name, sizeof name, nullptr, nullptr);
    if (!byName || !hdr || byName(name, &id) != 0) return -1;
    struct { unsigned version; int cmd, mode, metadataId; unsigned short mastering[12]; int format, range, bpc; } d = {};
    d.version = sizeof d | (2u << 16);  // NV_HDR_COLOR_DATA_V2; cmd 0 = get
    if (hdr(id, &d) != 0) { d = {}; d.version = 40 | (1u << 16); if (hdr(id, &d) != 0) return -1; }  // V1
    return d.mode;
}

void refreshEncoding() {
    DXGI_SWAP_CHAIN_DESC1 sd; g_sc->GetDesc1(&sd);
    IDXGIOutput* o = nullptr; IDXGIOutput6* o6 = nullptr; DXGI_OUTPUT_DESC1 od = {};
    if (SUCCEEDED(g_sc->GetContainingOutput(&o)) && SUCCEEDED(o->QueryInterface(IID_PPV_ARGS(&o6)))) o6->GetDesc1(&od);
    release(o6); release(o);
    const int mode = od.DeviceName[0] ? nvapiHdrMode(od.DeviceName) : -1;
    const int enc = sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 2
                  : od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || mode == 2 || mode == 5 || mode == 6 ? 1 : 0;
    if (od.MaxLuminance > 0) { g_lum[0] = od.MinLuminance; g_lum[1] = od.MaxLuminance; }
    if (enc != g_encode) logf("d3d12: output %s (NVAPI HDR mode %d, display %.4f-%.0f nits)", enc == 1 ? "HDR10 (PQ)" : enc == 2 ? "scRGB" : "SDR",
                              mode, g_lum[0], g_lum[1]);
    g_encode = enc;
}

void releaseTargets() {
    waitIdle();
    if (g_fsrFg) fsrFgRelease(g_sc);  // its context is tied to the swapchain size; recreated on the next frame
    releaseHudless();
    for (auto*& c : g_cl) release(c);
    release(g_shared11); release(g_shared12); release(g_gameBB);
    menuReleaseTargets();
}

// Shared texture at the swapchain's size and format, plus one pre-recorded "shared -> back buffer i" copy per buffer.
bool createTargets() {
    DXGI_SWAP_CHAIN_DESC1 d; g_sc->GetDesc1(&d);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, d.Width, d.Height, 1, 1, d.Format, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    HRESULT hr = g_d3d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_shared12));
    HANDLE h = nullptr;
    if (SUCCEEDED(hr)) hr = g_d3d12->CreateSharedHandle(g_shared12, nullptr, GENERIC_ALL, nullptr, &h);
    if (SUCCEEDED(hr)) hr = g_dev11->OpenSharedResource1(h, IID_PPV_ARGS(&g_shared11));
    if (h) CloseHandle(h);
    if (SUCCEEDED(hr)) hr = g_alloc->Reset();
    for (UINT i = 0; SUCCEEDED(hr) && i < kBuffers; ++i) {
        ID3D12Resource* bb = nullptr;
        hr = g_sc->GetBuffer(i, IID_PPV_ARGS(&bb));
        if (SUCCEEDED(hr)) hr = g_d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_cl[i]));
        if (SUCCEEDED(hr)) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Transition = { bb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST };
            g_cl[i]->ResourceBarrier(1, &b);
            g_cl[i]->CopyResource(bb, g_shared12);  // shared texture is promoted from COMMON and decays back
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            g_cl[i]->ResourceBarrier(1, &b);
            hr = g_cl[i]->Close();
        }
        release(bb);  // the swapchain keeps it alive; ResizeBuffers needs our references gone
    }
    logf("d3d12: present targets %ux%u fmt %u: %08x", d.Width, d.Height, d.Format, hr);
    if (SUCCEEDED(hr)) refreshEncoding();
    if (SUCCEEDED(hr)) menuTargets(g_sc);  // no-op until menuInit
    return SUCCEEDED(hr);
}

// ---- DLSS Frame Generation ------------------------------------------------------
// Inputs are DLSS's own motion vectors and depth (render resolution, shared from D3D11), tagged once per rendered
// frame. FFXV exposes no view/projection matrices; clipToPrevClip is exact (from TAA's g_motionMatrix), the
// projection and camera basis are a consistent stand-in. FG runs only while DLSS runs, i.e. in gameplay.
bool g_fg;                          // Streamline + DLSS-G loaded
const sl::ViewportHandle g_vp(0u);
sl::FrameToken* g_token;
uint32_t g_frameIndex;
bool g_simOpen;
sl::DLSSGOptions g_fgOpts;
FgSettings g_set;                   // [fg] in the ini; the menu changes it live
FgStatus g_status;                  // what this GPU/driver supports, from DLSS-G's state
bool g_fgOn;
int g_validRun, g_invalidRun;       // hysteresis, so a skipped DLSS frame doesn't toggle FG
FgGate g_gate;                      // FG waits until the game is past its start-up scene (fg_gate.h)
// Under Wine (Proton), switching DLSS-G back on while the game's first frames after a load are still on the GPU can
// hang the GPU (NVIDIA 620.70 with DXVK and vkd3d-proton). The first activation, which creates DLSS-G's resources on
// a quiet GPU, has never hung, so every activation there is made to look like it: resources released at each switch
// off, and switching on only after a settled second of valid frames and once both devices have finished their work.
bool g_wine;
ULONGLONG g_validSince;             // tick of the first frame of the current run of valid frames
ULONGLONG g_drainRetry;             // after a drain wait times out, no new attempt before this tick
constexpr ULONGLONG kSettleMs = 1000;
UINT64 g_inputsFree;                // fence value after which FG has consumed the last tagged mv/depth
ID3D11Texture2D* g_tagSrc[2]; ID3D12Resource* g_tag12[2]; UINT g_tagW[2];
FgFrame g_last;
sl::DLSSGStatus g_lastStatus = sl::DLSSGStatus(~0u);
unsigned g_rendered, g_presented;
// Present-thread cost per stage, summed over the 500-frame log window (us): copy+flush, menu, FG setup, Present,
// FG state (incl. sleep), Reflex sleep, and the game's own time between our Present calls.
double g_tUs[7];
LARGE_INTEGER g_qpf, g_lastExit;
double lapUs(LARGE_INTEGER& t) {  // us since t, and moves t to now
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    double us = double(n.QuadPart - t.QuadPart) * 1e6 / double(g_qpf.QuadPart);
    t = n;
    return us;
}

void slLog(sl::LogType t, const char* m) {
    if (t == sl::LogType::eInfo) return;
    std::string s(m);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    logf("sl: %s", s.c_str());
}

bool fgStart() {
    if (!g_set.enable) return false;  // known-limit: disabled at launch means Streamline never loads; the menu can't turn it on until a restart
    static std::wstring dir = path(L"ffxv_dlss\\streamline");
    static const wchar_t* paths[] = { dir.c_str() };
    static const sl::Feature feats[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    g_sl = LoadLibraryW((dir + L"\\sl.interposer.dll").c_str());
    if (!g_sl) { logf("fg: %S\\sl.interposer.dll missing, frame generation off", dir.c_str()); return false; }
    if (GetFileAttributesW((dir + L"\\nvngx_dlssg.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        logf("fg: %S\\nvngx_dlssg.dll missing (user-supplied): Reflex only, no frame generation", dir.c_str());
    sl::Preferences p;
    p.pathsToPlugins = paths; p.numPathsToPlugins = 1;
    p.logMessageCallback = slLog;
    p.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    p.featuresToLoad = feats; p.numFeaturesToLoad = _countof(feats);
    p.engine = sl::EngineType::eCustom; p.engineVersion = "ffxv-dlss";
    p.projectId = "6f1c9a3e-2b7d-4e58-9c41-ffa15d1e0715";
    p.renderAPI = sl::RenderAPI::eD3D12;
    sl::Result r = slInit(p, sl::kSDKVersion);
    logf("fg: slInit %d", (int)r);
    return r == sl::Result::eOk;
}

sl::float4x4 rows(const float* m) {
    sl::float4x4 r;
    for (int i = 0; i < 4; ++i) r[i] = sl::float4(m[4 * i], m[4 * i + 1], m[4 * i + 2], m[4 * i + 3]);
    return r;
}

sl::Constants constants(const FgFrame& f, bool reset) {
    sl::Constants c;
    const float aspect = float(f.w) / float(f.h), fov = 1.0472f, zn = 0.1f, zf = 10000.f, cot = 1.f / std::tan(fov * 0.5f);
    const float proj[16] = { cot / aspect, 0, 0, 0,  0, cot, 0, 0,  0, 0, zf / (zf - zn), 1,  0, 0, -zn * zf / (zf - zn), 0 };
    c.cameraViewToClip = rows(proj);
    sl::matrixFullInvert(c.clipToCameraView, c.cameraViewToClip);
    const float id[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    c.clipToLensClip = rows(id);
    // g_motionMatrix maps (u, v, depth, 1) to the previous frame's clip space (row vector), so prepend clip -> uv.
    const float clipToUv[16] = { 0.5f, 0, 0, 0,  0, -0.5f, 0, 0,  0, 0, 1, 0,  0.5f, 0.5f, 0, 1 };
    sl::matrixMul(c.clipToPrevClip, rows(clipToUv), rows(f.cb.motionMatrix));
    sl::matrixFullInvert(c.prevClipToClip, c.clipToPrevClip);
    c.jitterOffset = { f.jitterPx[0], f.jitterPx[1] };
    c.mvecScale = { 1.f, 1.f };  // UV units already span [-1, 1]
    c.cameraPinholeOffset = { 0.f, 0.f };
    c.cameraPos = { 0.f, 0.f, 0.f };
    c.cameraUp = { 0.f, 1.f, 0.f }; c.cameraRight = { 1.f, 0.f, 0.f }; c.cameraFwd = { 0.f, 0.f, 1.f };
    c.cameraNear = zn; c.cameraFar = zf; c.cameraFOV = fov; c.cameraAspectRatio = aspect;
    c.motionVectorsInvalidValue = 0.f;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    return c;
}

// On: the mode from g_set (frames 0 = dynamic MFG if this system has it, else fixed 1..5 generated per rendered).
// g_status is refreshed every frame by fgAfterPresent, which always runs before FG can turn on.
void fgSetMode(bool on) {
    if (proxyFg()) {  // FSR: applied by fsrFgFrame each frame; XeSS: switched here
        if (g_xessSc) xessFgEnable(on);
        g_fgOn = on;
        logf("fg: %s %s", g_xessSc ? "XeSS" : "FSR", on ? "ON" : "off");
        return;
    }
    const bool dynamic = g_set.frames == 0 && g_status.dynamicSupported;
    g_fgOpts.mode = !on ? sl::DLSSGMode::eOff : dynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
    g_fgOpts.numFramesToGenerate = std::max(1u, std::min(UINT(g_set.frames > 0 ? g_set.frames : 1), std::max(1u, g_status.maxGenerated)));
    g_fgOpts.dynamicTargetFrameRate = float(g_set.target);
    g_fgOpts.flags = g_wine ? sl::DLSSGFlags(0) : sl::DLSSGFlags::eRetainResourcesWhenOff;  // Wine: release when off
    sl::Result r = slDLSSGSetOptions(g_vp, g_fgOpts);
    g_fgOn = on;
    logf("fg: %s (%d) mode %d frames %u target %.0f, dynamic %s, max generated %u", on ? "ON" : "off", (int)r, (int)g_fgOpts.mode,
         g_fgOpts.numFramesToGenerate, g_fgOpts.dynamicTargetFrameRate, g_status.dynamicSupported ? "supported" : "unsupported", g_status.maxGenerated);
}

// Frame generation needs Reflex, so "off" only applies while FG is disabled.
int reflexEffective() { return g_set.enable ? std::max(1, g_set.reflex) : g_set.reflex; }

// Reflex's own G-Sync cap: refresh - refresh^2/3600 (240 Hz -> 224, 144 -> 138), so frames never outrun the display
// and VRR stays engaged instead of tearing. The cap applies to output frames, generated ones included.
int autoCap() {
    MONITORINFOEXW mi = {}; mi.cbSize = sizeof mi;
    DEVMODEW dm = {}; dm.dmSize = sizeof dm;
    const HMONITOR mon = MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY);
    const int hz = GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1
                   ? int(dm.dmDisplayFrequency) : 60;
    return hz - hz * hz / 3600;
}
int g_autoCap;

void setReflex() {
    static const sl::ReflexMode modes[] = { sl::ReflexMode::eOff, sl::ReflexMode::eLowLatency, sl::ReflexMode::eLowLatencyWithBoost };
    sl::ReflexOptions ro;
    ro.mode = modes[std::clamp(reflexEffective(), 0, 2)];
    g_autoCap = autoCap();  // cached: the counter reads fgStatus every frame
    const int cap = g_set.limit < 0 ? g_autoCap : g_set.limit;
    ro.frameLimitUs = cap > 0 ? 1000000u / cap : 0;  // Reflex frame limiter, the limiter dynamic MFG is designed around
    slReflexSetOptions(ro);
}

// True once both devices have finished everything submitted so far: the D3D12 queue's copy waits for the D3D11 copy,
// so a drained D3D12 queue with g_up at g_upVal covers both. Waits on the CPU for at most 250 ms; after a time-out it
// declines for 500 ms, so a busy GPU costs one short pause per half second, not one per frame.
bool fgDrained() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_drainRetry) return false;
    const UINT64 target = ++g_fenceVal;
    g_d3d12Queue->Signal(g_fence, target);
    ResetEvent(g_drainEvent);  // a completion from an earlier timed-out wait may have set it since
    g_fence->SetEventOnCompletion(target, g_drainEvent);
    WaitForSingleObject(g_drainEvent, 250);
    if (g_fence->GetCompletedValue() >= target && g_up->GetCompletedValue() >= g_upVal) return true;
    g_drainRetry = GetTickCount64() + 500;
    logf("fg: GPU still busy after %llu ms, frame generation waits", GetTickCount64() - now);
    return false;
}

void fgBeforePresent() {
    FgFrame f = {};
    const bool valid = dlssTakeFrame(&f);
    ID3D12Resource *mv12 = valid ? p12Open(0, f.mv, f.w) : nullptr, *depth12 = valid ? p12Open(1, f.depth, f.w) : nullptr;
    const bool tagged = mv12 && depth12;
    const bool wasOpen = g_gate.open, wasReady = g_gate.ready;
    const bool mayStart = g_gate.frame(tagged, GetTickCount64());
    if (!wasOpen && g_gate.open && !mayStart)
        logf("fg: title screen seen, frame generation waits for 20 s of upscaled frames");
    if (!wasReady && mayStart)
        logf("fg: past the start-up scene (%s), frame generation can start",
             g_gate.how == FgGate::kTitleGap ? "title gap, then 20 s of upscaled frames" : "no start-up gap after 30 s of DLSS frames");
    g_validRun = tagged ? g_validRun + 1 : 0;
    g_invalidRun = tagged ? 0 : g_invalidRun + 1;
    if (g_validRun == 1) g_validSince = GetTickCount64();
    const bool reset = tagged && !g_fgOn;
    if (!g_fgOn && g_validRun >= 8 && g_set.enable && mayStart
        && (!g_wine || proxyFg() || (GetTickCount64() - g_validSince >= kSettleMs && fgDrained()))) fgSetMode(true);
    else if (g_fgOn && (g_invalidRun >= 8 || !g_set.enable)) fgSetMode(false);
    if (g_fsrFg) {
        g_fsrGen = fsrFgFrame(g_d3d12, g_sc, g_fgOn, tagged ? &f : nullptr, depth12, mv12, g_hudlessWritten ? g_hudless12[g_hudlessSlot] : nullptr, reset);
        if (g_hudlessWritten) { g_hudlessWritten = false; g_hudlessSlot = (g_hudlessSlot + 1) % kHudless; }
        g_d3d12Queue->Signal(g_fence, g_inputsFree = ++g_fenceVal);  // the prepare pass has read this frame's mv/depth
        return;
    }
    if (g_xessSc) {
        g_xessGen = xessFgFrame(g_fgOn, tagged ? &f : nullptr, depth12, mv12, g_hudlessWritten ? g_hudless12[g_hudlessSlot] : nullptr, reset, g_xeId);
        if (g_hudlessWritten) { g_hudlessWritten = false; g_hudlessSlot = (g_hudlessSlot + 1) % kHudless; }
        g_d3d12Queue->Signal(g_fence, g_inputsFree = ++g_fenceVal);  // XeSS has copied this frame's mv/depth on its list
        p12FirstDraw();  // no draws this frame: still close the simulation span
        xessFgBeforePresent(g_xeId);
        return;
    }
    if (tagged) g_last = f;
    if (g_last.w) slSetConstants(constants(g_last, reset), *g_token, g_vp);

    sl::Extent ext{ 0, 0, f.w, f.h };
    sl::Resource dr(sl::ResourceType::eTex2d, depth12, D3D12_RESOURCE_STATE_COMMON), mr(sl::ResourceType::eTex2d, mv12, D3D12_RESOURCE_STATE_COMMON);
    sl::ResourceTag tags[] = { { tagged ? &dr : nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ext },
                               { tagged ? &mr : nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &ext } };
    slSetTagForFrame(*g_token, g_vp, tags, _countof(tags), nullptr);

    p12FirstDraw();  // no draws this frame: still close the simulation span
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *g_token);
    slPCLSetMarker(sl::PCLMarker::ePresentStart, *g_token);
}

// Returns how many frames were actually shown for this rendered one (1 + generated).
unsigned fgAfterPresent() {
    slPCLSetMarker(sl::PCLMarker::ePresentEnd, *g_token);
    sl::DLSSGState st;
    unsigned shown = 1;
    if (slDLSSGGetState(g_vp, st, nullptr) == sl::Result::eOk) {
        if (st.status != g_lastStatus) { logf("fg: status %u", (unsigned)st.status); g_lastStatus = st.status; }
        shown = std::max(1u, st.numFramesActuallyPresented);
        g_status.dynamicSupported = st.bIsDynamicMFGSupported != sl::Boolean::eFalse;
        g_status.maxGenerated = st.numFramesToGenerateMax;
        g_presented += st.numFramesActuallyPresented;
        if (st.inputsProcessingCompletionFence)
            g_d3d12Queue->Wait((ID3D12Fence*)st.inputsProcessingCompletionFence, st.lastPresentInputsProcessingCompletionFenceValue);
    }
    g_d3d12Queue->Signal(g_fence, g_inputsFree = ++g_fenceVal);
    if (++g_rendered == 500) {
        dlssgVersion();  // logs the frame generation file and its folder once it has loaded, menu open or not
        // Reflex sleeps here, on the present thread: only right if the game also polls input on it
        DWORD di, xi; unsigned polls; inputThreads(&di, &xi, &polls);
        const double n = g_rendered;
        logf("fg: %u frames presented per %u rendered; threads: present %lu, dinput %lu (%.1f polls/frame), xinput %lu", g_presented, g_rendered,
             GetCurrentThreadId(), di, polls / n, xi);
        logf("fg: present-thread us/frame: copy %.0f menu %.0f fgset %.0f Present %.0f fgstate %.0f reflex sleep %.0f | game %.0f",
             g_tUs[0] / n, g_tUs[1] / n, g_tUs[2] / n, g_tUs[3] / n, (g_tUs[4] - g_tUs[5]) / n, g_tUs[5] / n, g_tUs[6] / n);
        std::fill(std::begin(g_tUs), std::end(g_tUs), 0.0);
        g_rendered = g_presented = 0;
    }
    // next frame starts here
    ++g_frameIndex;
    slGetNewFrameToken(g_token, &g_frameIndex);
    LARGE_INTEGER s; QueryPerformanceCounter(&s);
    slReflexSleep(*g_token);
    g_tUs[5] += lapUs(s);
    slPCLSetMarker(sl::PCLMarker::eSimulationStart, *g_token);
    g_simOpen = true;
    return shown;
}

// Inside our own swapchain creation: the factory hooks pass any swapchain made meanwhile (Streamline's, Intel's) straight through.
struct Busy { bool was = g_busy; Busy() { g_busy = true; } ~Busy() { g_busy = was; } };

void fail(const char* what, HRESULT hr);

// g_xessRefused follows the format the game presents in now (the menu's HDR message), with a log line when it changes.
void noteXessFormat(bool want, unsigned format) {
    const bool refused = g_xessFg && !want;
    if (refused != g_xessRefused)
        logf(refused ? "fg: XeSS frame generation waits: back buffer format %u is not one it takes" : "fg: back buffer format %u, XeSS frame generation can run", format);
    g_xessRefused = refused;
}

// Our swapchain for the game's new size and format, called with frame generation off and the targets released (the
// queue is idle). It is Intel's while XeSS-FG is this launch's backend and the format is one XeSS-FG takes, else a plain
// one: the same kind is resized, a change of kind replaces it on the same window. Format 0 keeps the current format.
// Leaves g_sc null only if no swapchain could be made.
HRESULT fitSwapchain(UINT w, UINT h, DXGI_FORMAT format) {
    DXGI_SWAP_CHAIN_DESC1 sd; g_sc->GetDesc1(&sd);
    if (format == DXGI_FORMAT_UNKNOWN) format = sd.Format;
    const bool want = xessFgWanted(g_xessFg, format);
    noteXessFormat(want, format);
    if (want == g_xessSc) return g_sc->ResizeBuffers(0, w, h, format, g_scFlags);
    Busy busy;
    sd.Width = w; sd.Height = h; sd.Format = format; sd.Flags = g_scFlags;
    sd.BufferCount = kBuffers; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.SampleDesc = { 1, 0 }; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    if (g_xessSc) waitIdle();  // XeSS's tag lists may still use its swapchain's resources
    release(g_sc);
    if (g_xessSc) { xessFgDestroy(); g_simOpen = false; g_xessGen = false; }  // after our last reference to Intel's swapchain, as in fail()
    g_xessSc = false;
    IDXGISwapChain1* sc1 = nullptr;
    if (want && !(sc1 = xessFgSwapchain(g_factory, g_d3d12, g_d3d12Queue, g_wnd, &sd, reflexEffective()))) {
        logf("fg: XeSS swapchain failed, frame generation off");
        g_xessFg = false;
    }
    g_xessSc = sc1 != nullptr;
    HRESULT hr = sc1 ? S_OK : g_create(g_factory, g_d3d12Queue, g_wnd, &sd, nullptr, nullptr, &sc1);
    if (SUCCEEDED(hr)) { hr = sc1->QueryInterface(IID_PPV_ARGS(&g_sc)); sc1->Release(); }
    logf("d3d12: swapchain replaced by %s %ux%u fmt %u: %08x", g_xessSc ? "Intel's frame generation one" : "a plain one", w, h, format, hr);
    if (FAILED(hr)) { g_xessSc = false; if (want) xessFgDestroy(); return hr; }
    if (g_colorSpace >= 0) g_sc->SetColorSpace1(DXGI_COLOR_SPACE_TYPE(g_colorSpace));
    if (g_xessSc) { xessFgFrameStart(g_xeId); g_simOpen = true; }  // opens the first frame, as p12Init does
    return hr;
}

// ---- swapchain hooks on the game's (hidden) swapchain --------------------------------
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_SetColorSpace1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
using PFN_SetHDRMetaData = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain4*, DXGI_HDR_METADATA_TYPE, UINT, void*);
PFN_ResizeBuffers o_ResizeBuffers;
PFN_SetColorSpace1 o_SetColorSpace1;
PFN_SetHDRMetaData o_SetHDRMetaData;

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (sc != g_game) return o_ResizeBuffers(sc, n, w, h, f, fl);
    if (!w || !h) { RECT r; GetClientRect(g_wnd, &r); w = r.right; h = r.bottom; }  // the hidden window's size means nothing
    if (g_fgOn) fgSetMode(false);  // Streamline: FG off across resizes
    g_validRun = 0;  // new targets: under Wine, FG waits for a fresh settled second
    releaseTargets();
    HRESULT hr = o_ResizeBuffers(sc, n, w, h, f, fl);
    HRESULT hr12 = SUCCEEDED(hr) ? fitSwapchain(w, h, f) : E_ABORT;
    logf("d3d12: ResizeBuffers %ux%u fmt %u: game %08x ours %08x", w, h, f, hr, hr12);
    if (!g_sc) { fail("swapchain replacement", hr12); g_game = nullptr; return hr; }  // the game's frames stay on its hidden window
    if (FAILED(hr12) || !createTargets()) logf("d3d12: resize failed, frames will not reach the screen");
    return hr;
}

// HDR state is set on the game's swapchain; mirror it onto ours.
HRESULT STDMETHODCALLTYPE hk_SetColorSpace1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE cs) {
    HRESULT hr = o_SetColorSpace1(sc, cs);
    if ((IDXGISwapChain*)sc == g_game) g_colorSpace = cs;
    if ((IDXGISwapChain*)sc == g_game) logf("d3d12: SetColorSpace1 %d: game %08x ours %08x", cs, hr, g_sc->SetColorSpace1(cs));
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_SetHDRMetaData(IDXGISwapChain4* sc, DXGI_HDR_METADATA_TYPE t, UINT n, void* m) {
    HRESULT hr = o_SetHDRMetaData(sc, t, n, m);
    if ((IDXGISwapChain*)sc == g_game) {
        IDXGISwapChain4* s4 = nullptr;
        if (SUCCEEDED(g_sc->QueryInterface(IID_PPV_ARGS(&s4)))) { s4->SetHDRMetaData(t, n, m); s4->Release(); }
    }
    return hr;
}

HWND newDummy(HWND wnd) {
    RECT r; GetClientRect(wnd, &r);
    return g_dummy = CreateWindowExW(0, L"STATIC", L"ffxv_dlss hidden swapchain", WS_POPUP, 0, 0, r.right, r.bottom, nullptr, nullptr, nullptr, nullptr);
}

// The game can recreate its swapchain (seen once at start-up, before the first frames): keep our presenter and move the new one to a hidden
// window too. known-limit: one leaked hidden window per recreation (the old swapchain may still live on it); fine while rare.
HWND reattach(HWND wnd, const DXGI_SWAP_CHAIN_DESC1* desc) {
    if (!g_sc || wnd != g_wnd) return nullptr;  // presenter failed earlier, or another window: present natively
    if (g_fgOn) fgSetMode(false);
    g_validRun = 0;
    releaseTargets();
    g_game = nullptr;
    UINT w = desc->Width, h = desc->Height;
    if (!w || !h) { RECT r; GetClientRect(wnd, &r); w = r.right; h = r.bottom; }
    HRESULT hr = fitSwapchain(w, h, desc->Format);
    logf("d3d12: game swapchain recreated %ux%u fmt %u flags %x, ours resized: %08x", w, h, desc->Format, desc->Flags, hr);
    if (!g_sc) { fail("swapchain replacement", hr); return nullptr; }  // frees the window for the game's own swapchain
    if (FAILED(hr) || !createTargets()) { logf("d3d12: reattach failed, presenting natively"); return nullptr; }
    return newDummy(wnd);
}

void fail(const char* what, HRESULT hr) {
    logf("d3d12: %s failed %08x, presenting natively", what, hr);
    fsrFgDestroy(g_sc);  // else FSR's context keeps the window's swapchain alive and the native one can't be created
    if (g_xessSc && g_fence && g_idleEvent) waitIdle();  // XeSS's tag lists may still use its swapchain's resources
    release(g_sc);
    xessFgDestroy();  // after our last reference to Intel's swapchain (it refuses otherwise), before the queue it uses
    release(g_cl[0]); release(g_cl[1]); release(g_cl[2]); release(g_alloc);
    release(g_fence11); release(g_fence); release(g_up11); release(g_up); release(g_ctx11); release(g_dev11);
    release(g_d3d12Queue); release(g_d3d12); release(g_factory);
    g_xessSc = false;
    g_fg = g_fsrFg = g_xessFg = false;
}
}  // namespace

bool p12FsrGenerating() { return g_fsrFg && g_fsrGen; }
bool p12WantsHudless() { return proxyFg() && g_fgOn && g_gameBB; }
int p12OutputEncoding(float lum[2]) { if (lum) { lum[0] = g_lum[0]; lum[1] = g_lum[1]; } return std::max(g_encode, 0); }

ID3D11RenderTargetView* p12HudlessTarget() {
    if (!p12WantsHudless()) return nullptr;
    const UINT i = g_hudlessSlot;
    if (!g_hudless11[i]) {
        D3D11_TEXTURE2D_DESC d; g_gameBB->GetDesc(&d);  // the back buffer the composite draws into: same size and format
        d.MipLevels = d.ArraySize = 1; d.SampleDesc = { 1, 0 }; d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        IDXGIResource1* r = nullptr; HANDLE h = nullptr;
        HRESULT hr = g_dev11->CreateTexture2D(&d, nullptr, &g_hudless11[i]);
        if (SUCCEEDED(hr)) hr = g_dev11->CreateRenderTargetView(g_hudless11[i], nullptr, &g_hudlessRTV[i]);
        if (SUCCEEDED(hr)) hr = g_hudless11[i]->QueryInterface(IID_PPV_ARGS(&r));
        if (SUCCEEDED(hr)) hr = r->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
        if (SUCCEEDED(hr)) hr = g_d3d12->OpenSharedHandle(h, IID_PPV_ARGS(&g_hudless12[i]));
        release(r);
        if (h) CloseHandle(h);
        logf("d3d12: HUD-less slot %u %ux%u fmt %u: %08x", i, d.Width, d.Height, d.Format, hr);
        if (FAILED(hr)) { release(g_hudlessRTV[i]); release(g_hudless12[i]); release(g_hudless11[i]); return nullptr; }
    }
    g_hudlessWritten = true;
    return g_hudlessRTV[i];
}

namespace {
// FSR's stand-in for Reflex, run after Present (before the game starts its next frame): wait until the GPU has copied
// out the previous frame, so the game is never more than one frame ahead of it, then hold to the frame cap (output fps,
// as Reflex's: rendered = cap / 2 while generating).
void fsrPace() {
    static HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    static UINT64 prev;
    static LARGE_INTEGER next;
    if (prev && g_fence->GetCompletedValue() < prev) { g_fence->SetEventOnCompletion(prev, ev); WaitForSingleObject(ev, 100); }
    prev = g_copyDone;
    const int cap = g_set.limit < 0 ? g_autoCap : g_set.limit;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (cap <= 0) { next = now; return; }
    const LONGLONG period = g_qpf.QuadPart * (g_fsrGen || g_xessGen ? 2 : 1) / cap;
    next.QuadPart = std::max(next.QuadPart + period, now.QuadPart - period);  // at most one period of catch-up after a hitch
    const LONGLONG left = next.QuadPart - now.QuadPart;
    if (left <= 0) return;
    LARGE_INTEGER due; due.QuadPart = -(left * 10000000 / g_qpf.QuadPart);  // relative, 100 ns units
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
}
}  // namespace

bool p12Shared(P12Shared* s) {
    if (!g_d3d12 || !g_d3d12Queue || !g_fence11 || !g_up11) return false;
    *s = { g_d3d12, g_d3d12Queue, g_dev11, g_ctx11, g_fence, g_fence11, &g_fenceVal, g_up, g_up11, &g_upVal };
    return true;
}

// D3D12 view of a D3D11 texture created with SHARED_NTHANDLE (dlss.cpp), cached per slot.
ID3D12Resource* p12Open(int i, ID3D11Texture2D* t, UINT w) {
    if (!g_d3d12) return nullptr;
    if (g_tagSrc[i] == t && g_tagW[i] == w) return g_tag12[i];
    if (g_tag12[i]) waitIdle();  // FSR or frame generation may still be reading the old one on our queue
    release(g_tag12[i]); g_tagSrc[i] = t; g_tagW[i] = w;
    IDXGIResource1* r = nullptr; HANDLE h = nullptr;
    if (SUCCEEDED(t->QueryInterface(IID_PPV_ARGS(&r)))) { r->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h); r->Release(); }
    if (h) { g_d3d12->OpenSharedHandle(h, IID_PPV_ARGS(&g_tag12[i])); CloseHandle(h); }
    logf("d3d12: %s texture %p -> d3d12 %p", i ? "depth" : "mv", (void*)t, (void*)g_tag12[i]);
    return g_tag12[i];
}

FgSettings fgSettings() { return g_set; }
FgStatus fgStatus() {
    FgStatus s = g_status;
    s.available = g_fg || proxyFg(); s.generating = proxyFg() ? g_fsrGen || g_xessGen : g_fgOn; s.autoCap = g_autoCap;
    s.backend = g_xessSc ? 2 : g_fsrFg ? 1 : g_fg ? 0 : -1;
    s.formatRefused = g_xessRefused;
    if (proxyFg()) { s.dynamicSupported = false; s.maxGenerated = 1; }  // FSR and XeSS generate one frame per rendered one
    return s;
}
// NVIDIA's runtime loads nvngx_dlssg.dll when frame generation first starts. Each copy in the process is logged once with
// its folder. A copy from outside the game folder is the driver's own, which the runtime loads only when it found none of
// ours: the menu then names that one, so it always shows the file that runs.
static void fileVersion(const wchar_t* fn, char* out, size_t len) {
    DWORD h = 0, sz = GetFileVersionInfoSizeW(fn, &h);
    std::vector<BYTE> b(sz ? sz : 1); VS_FIXEDFILEINFO* fi = nullptr; UINT fl = 0;
    if (sz && GetFileVersionInfoW(fn, 0, sz, b.data()) && VerQueryValueW(b.data(), L"\\", (void**)&fi, &fl) && fi)
        snprintf(out, len, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
                 LOWORD(fi->dwFileVersionLS));
}
const char* dlssgVersion() {
    static char v[48];
    if (*v || !GetModuleHandleW(L"nvngx_dlssg.dll")) return v;
    HMODULE mods[1024]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return v;
    const std::wstring game = path(L"");
    char ours[24] = "", driver[24] = "";
    const DWORD n = std::min<DWORD>(need / sizeof(HMODULE), _countof(mods));
    for (DWORD i = 0; i < n; ++i) {
        wchar_t fn[MAX_PATH];
        if (!GetModuleFileNameW(mods[i], fn, MAX_PATH)) continue;
        const wchar_t* base = fn;
        for (const wchar_t* c = fn; *c; ++c) if (*c == L'\\' || *c == L'/') base = c + 1;  // the runtime may use either
        if (_wcsicmp(base, L"nvngx_dlssg.dll")) continue;
        char ver[24] = "?";
        fileVersion(fn, ver, sizeof ver);
        const bool inGame = _wcsnicmp(fn, game.c_str(), game.size()) == 0;
        logf("fg: nvngx_dlssg.dll loaded from %S v%s%s", fn, ver, inGame ? "" : " (the driver's copy, not the game folder's)");
        strcpy_s(inGame ? ours : driver, sizeof ours, ver);
    }
    if (*driver) snprintf(v, sizeof v, "%s (driver's copy)", driver);
    else snprintf(v, sizeof v, "%s", ours);
    return v;
}
void fgApply(const FgSettings& s) {
    const bool reflexChanged = s.limit != g_set.limit || s.reflex != g_set.reflex || s.enable != g_set.enable;
    g_set = s;
    if (!g_fg && !proxyFg()) return;
    if (reflexChanged && g_fg) setReflex();
    if (reflexChanged && g_xessSc) xessFgSleepMode(reflexEffective());  // XeLL takes a new mode without a GPU idle
    if (g_fgOn) fgSetMode(s.enable != 0);  // new mode/target now; on/off otherwise follows gameplay (fgBeforePresent)
}

bool p12Enabled() { return ini(L"d3d12", L"present", 1) != 0; }
bool p12Busy() { return g_busy; }
// Once active, every swapchain but the game's is ours or Streamline's internal one: hooks pass them straight through.
bool p12Owns(IDXGISwapChain* sc) { return g_game && sc != g_game; }
bool p12IsGame(IDXGISwapChain* sc) { return sc && sc == g_game; }

HWND p12Init(IUnknown* dev, IDXGIFactory2* f, PFN_CreateSCHwnd create, HWND wnd, const DXGI_SWAP_CHAIN_DESC1* desc) {
    if (g_d3d12) return reattach(wnd, desc);
    Busy busy;
    logf("d3d12: init for hwnd %p %ux%u fmt %u flags %x", (void*)wnd, desc->Width, desc->Height, desc->Format, desc->Flags); flushLog();
    IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr;
    HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&g_dev11));
    if (SUCCEEDED(hr)) { ID3D11DeviceContext3* c = nullptr; g_dev11->GetImmediateContext3(&c); hr = c->QueryInterface(IID_PPV_ARGS(&g_ctx11)); c->Release(); }
    if (SUCCEEDED(hr)) hr = dev->QueryInterface(IID_PPV_ARGS(&dd));
    if (SUCCEEDED(hr)) { hr = dd->GetAdapter(&ad); dd->Release(); }
    if (FAILED(hr)) { fail("D3D11 device interfaces", hr); return nullptr; }

    DXGI_ADAPTER_DESC adesc;
    if (SUCCEEDED(ad->GetDesc(&adesc))) g_vendor = adesc.VendorId;
    g_wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    g_set = { ini(L"fg", L"enable", 1), ini(L"fg", L"frames", 0), ini(L"fg", L"target", 240), ini(L"fg", L"limit", -1), ini(L"fg", L"reflex", 1) };
    // Frame generation backend, one per session (it decides which swapchain we create): [fg] backend through fgPickBackend
    // (fg_select.h). Exactly one of g_fg, g_fsrFg and g_xessFg is set here, or none.
    const int setting = ini(L"fg", L"backend", -1);
    const bool dlssg = GetFileAttributesW(path(L"ffxv_dlss\\streamline\\nvngx_dlssg.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    const int backend = fgPickBackend(setting, g_vendor, dlssg, xessFgPresent());
    switch (backend) {
    case 0: g_fg = fgStart(); break;                              // Streamline loads only on NVIDIA, or when chosen by hand
    case 1: g_fsrFg = g_set.enable && fsrLoad(); break;           // AMD's swapchain replaces ours: decided at launch
    case 2: g_xessFg = g_set.enable && xessFgPresent(); break;    // Intel's too; its DLLs load when the swapchain is made
    default: break;
    }
    // XeSS-FG takes 8-bit SDR or 10-bit HDR10, not 16-bit float (scRGB). Asked again whenever the game recreates or resizes
    // its swapchain (fitSwapchain): the game starts in 16-bit float and moves to 10-bit a second later.
    const bool xessWant = xessFgWanted(g_xessFg, desc->Format);
    noteXessFormat(xessWant, desc->Format);
    logf("fg: backend %s (setting %d, GPU vendor %04x, Wine %s)", g_xessFg ? "XeSS" : g_fsrFg ? "FSR" : g_fg ? "DLSS-G" : "none", setting, g_vendor,
         g_wine ? "yes: settled, drained re-activation" : "no");
    ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
    if (ini(L"d3d12", L"dred", 0) && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->Release();
        logf("d3d12: DRED breadcrumbs + page faults on");
    }
    hr = D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_d3d12));
    ad->Release();
    // Streamline must see the device first and the present queue's creation (through its proxy device).
    ID3D12Device* queueDev = g_d3d12;
    if (SUCCEEDED(hr) && g_fg) {
        sl::Result r = slSetD3DDevice(g_d3d12);
        queueDev->AddRef();
        if (r == sl::Result::eOk) r = slUpgradeInterface((void**)&queueDev);
        if (r != sl::Result::eOk) { logf("fg: device setup %d, frame generation off", (int)r); g_fg = false; }
    }
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (SUCCEEDED(hr)) hr = queueDev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_d3d12Queue));
    if (SUCCEEDED(hr)) hr = g_d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc));
    HANDLE h = nullptr;
    if (SUCCEEDED(hr)) hr = g_d3d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_fence));
    if (SUCCEEDED(hr)) hr = g_d3d12->CreateSharedHandle(g_fence, nullptr, GENERIC_ALL, nullptr, &h);
    if (SUCCEEDED(hr)) hr = g_dev11->OpenSharedFence(h, IID_PPV_ARGS(&g_fence11));
    if (h) { CloseHandle(h); h = nullptr; }
    if (SUCCEEDED(hr)) hr = g_d3d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_up));
    if (SUCCEEDED(hr)) hr = g_d3d12->CreateSharedHandle(g_up, nullptr, GENERIC_ALL, nullptr, &h);
    if (SUCCEEDED(hr)) hr = g_dev11->OpenSharedFence(h, IID_PPV_ARGS(&g_up11));
    if (h) CloseHandle(h);
    if (FAILED(hr)) { fail("D3D12 device/fence", hr); return nullptr; }
    logf("d3d12: device, queue and shared fence ready"); flushLog();
    if (!g_idleEvent) g_idleEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_drainEvent) g_drainEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Same size, format and flags as the game asked for, so presentation behaves as it would natively.
    DXGI_SWAP_CHAIN_DESC1 sd = *desc;
    sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc = { 1, 0 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Tearing (VRR / uncapped at sync interval 0): without it a windowed flip swapchain tops out at 2x refresh.
    IDXGIFactory5* f5 = nullptr; BOOL tear = FALSE;
    if (SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&f5)))) { f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof tear); f5->Release(); }
    g_tearing = tear != FALSE;
    if (g_tearing) sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    g_scFlags = sd.Flags;
    IDXGISwapChain1* sc1 = nullptr;
    IDXGIFactory2* pf = f;
    if (g_fg) { pf->AddRef(); if (slUpgradeInterface((void**)&pf) != sl::Result::eOk) { logf("fg: factory upgrade failed, frame generation off"); g_fg = false; pf = f; } }
    if (g_fsrFg && !(sc1 = fsrFgSwapchain(f, g_d3d12Queue, wnd, &sd))) { logf("fg: FSR swapchain failed, frame generation off"); g_fsrFg = false; }
    if (xessWant && !(sc1 = xessFgSwapchain(f, g_d3d12, g_d3d12Queue, wnd, &sd, reflexEffective()))) {
        logf("fg: XeSS swapchain failed, frame generation off");
        g_xessFg = false;
    }
    g_xessSc = xessWant && sc1;
    g_factory = f; f->AddRef(); g_create = create;
    hr = sc1 ? S_OK : g_fg ? pf->CreateSwapChainForHwnd(g_d3d12Queue, wnd, &sd, nullptr, nullptr, &sc1) : create(f, g_d3d12Queue, wnd, &sd, nullptr, nullptr, &sc1);
    if (SUCCEEDED(hr)) { hr = sc1->QueryInterface(IID_PPV_ARGS(&g_sc)); sc1->Release(); }
    if (FAILED(hr)) { g_xessSc = false; fail("D3D12 swapchain", hr); return nullptr; }
    if (!createTargets()) { fail("shared targets", E_FAIL); return nullptr; }

    g_wnd = wnd;  // before setReflex: the auto cap is the refresh rate of this window's monitor
    if (!g_fg) g_autoCap = autoCap();  // fsrPace's auto cap; the Streamline path sets it with Reflex
    if (g_fg) {
        setReflex();
        sl::Result r = slGetNewFrameToken(g_token, &g_frameIndex);
        logf("fg: Streamline swapchain %p, Reflex mode %d limit %d fps: %d", (void*)g_sc, reflexEffective(), g_set.limit, (int)r);
        g_fg = r == sl::Result::eOk;
        if (g_fg) { slPCLSetMarker(sl::PCLMarker::eSimulationStart, *g_token); g_simOpen = true; }
    }
    if (g_xessSc) { xessFgFrameStart(g_xeId); g_simOpen = true; }  // opens frame 0, as Streamline's first token does

    newDummy(wnd);
    DXGI_SWAP_CHAIN_DESC1 cur; g_sc->GetDesc1(&cur);
    if (menuInit(g_d3d12, g_d3d12Queue, wnd, cur.Height)) menuTargets(g_sc);
    logf("d3d12: presenting via D3D12 swapchain %p on hwnd %p; game swapchain on hidden %p (flags %x)", (void*)g_sc, (void*)wnd, (void*)g_dummy, sd.Flags);
    flushLog();
    return g_dummy;
}

void p12Abort() { fail("game swapchain on hidden window", E_FAIL); }

void p12Attach(IDXGISwapChain* game) {
    g_game = game;
    void** vt = *(void***)game;
    MH_CreateHook(vt[13], (void*)hk_ResizeBuffers, (void**)&o_ResizeBuffers);
    MH_CreateHook(vt[38], (void*)hk_SetColorSpace1, (void**)&o_SetColorSpace1);
    MH_CreateHook(vt[40], (void*)hk_SetHDRMetaData, (void**)&o_SetHDRMetaData);
    MH_EnableHook(MH_ALL_HOOKS);
}

void p12WaitInputsFree(ID3D11DeviceContext*) { if (g_fgOn) g_ctx11->Wait(g_fence11, g_inputsFree); }

void p12FirstDraw() {
    if (!g_simOpen) return;
    g_simOpen = false;
    if (g_xessSc) { xessFgFirstDraw(g_xeId); return; }
    slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *g_token);
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *g_token);
}

HRESULT p12Present(UINT si, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return g_sc->Present(0, DXGI_PRESENT_TEST);
    if (!g_cl[0]) return S_OK;  // targets lost after a failed resize
    if (!g_gameBB) g_game->GetBuffer(0, IID_PPV_ARGS(&g_gameBB));
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (g_lastExit.QuadPart) { LARGE_INTEGER e = g_lastExit; g_tUs[6] += lapUs(e); }
    g_ctx11->Wait(g_fence11, g_copyDone);  // D3D12 has copied the previous frame out (never waits on its Present)
    g_ctx11->CopyResource(g_shared11, g_gameBB);
    g_ctx11->Signal(g_up11, ++g_upVal);
    g_ctx11->Flush();  // D3D11 only submits at Present, which the game's swapchain never does now
    g_d3d12Queue->Wait(g_up, g_upVal);
    const UINT bb = g_sc->GetCurrentBackBufferIndex();
    ID3D12CommandList* cl = g_cl[bb];
    g_d3d12Queue->ExecuteCommandLists(1, &cl);
    g_d3d12Queue->Signal(g_fence, g_copyDone = ++g_fenceVal);
    g_tUs[0] += lapUs(t);
    // FSR decides this frame's generation first: the menu routes the UI by it (layer while generating, else drawn in)
    if (g_fsrFg) { fgBeforePresent(); g_tUs[2] += lapUs(t); }
    menuFrame(g_sc, bb);  // counter + Insert menu, drawn over the copied frame
    g_tUs[1] += lapUs(t);
    if (g_fg || g_xessSc) fgBeforePresent();  // XeSS: its markers and present id right before Present
    g_tUs[2] += lapUs(t);
    HRESULT hr = g_sc->Present(si, si == 0 && g_tearing ? flags | DXGI_PRESENT_ALLOW_TEARING : flags);  // blocks once 3 frames are queued, like D3D11's default latency
    g_tUs[3] += lapUs(t);
    menuCount(g_fg ? fgAfterPresent() : g_xessSc ? xessFgAfterPresent(g_xeId++) : g_fsrGen ? 2 : 1);
    static unsigned presents;
    if (++presents % 240 == 0) refreshEncoding();  // NVAPI HDR can change after our swapchain exists
    const int up = g_fg ? -1 : dlssInfo().active;
    if (proxyFg() || up == 1 || up == 2) fsrPace();  // FSR or XeSS in use and no Reflex (FG may be off at launch)
    if (g_xessSc) { xessFgFrameStart(g_xeId); g_simOpen = true; }  // next frame starts here: XeLL's sleep after the pacing
    g_tUs[4] += lapUs(t);
    g_lastExit = t;
    return hr;
}
