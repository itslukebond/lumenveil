// Intel XeSS through Intel's SDK (headers from the third_party/xess submodule; Intel's DLLs are user-supplied in
// ffxv_dlss\xess and loaded by full path at run time, never linked, so a missing file never stops the game starting).
// Everything runs on the presenter's D3D12 device (present12.cpp):
// - Upscaling (XeSS-SR) replaces DLSS at the TAA point, through the same shared textures, allocator ring and fences as
//   FSR (up12.cpp).
// - Frame generation (XeSS-FG) is Intel's proxy swapchain in place of ours, with Xe Low Latency (XeLL) as its only
//   latency path. It gets the same motion vectors, depth and HUD-less image as FSR frame generation, copied by XeSS on a
//   ring list before Present.
#include "common.h"
#include "xess_rules.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cmath>
#include <utility>
#include <xess/xess_d3d12.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell_d3d12.h>

static_assert(DXGI_FORMAT_R10G10B10A2_UNORM == 24 && DXGI_FORMAT_R8G8B8A8_UNORM == 28 && DXGI_FORMAT_R8G8B8A8_UNORM_SRGB == 29 &&
              DXGI_FORMAT_B8G8R8A8_UNORM == 87 && DXGI_FORMAT_B8G8R8A8_UNORM_SRGB == 91, "xess_rules.h format numbers");

namespace {
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
constexpr DWORD kLoadFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;

// An entry point by name; a missing one leaves that part of XeSS unavailable.
template <class F> bool sym(HMODULE m, F& f, const char* name) {
    f = m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
    if (m && !f) logf("xess: %s not found in Intel's DLL", name);
    return f != nullptr;
}
#define SYM(m, t, name) sym(m, t.name, #name)

HMODULE loadDll(const wchar_t* rel) {
    const std::wstring p = path(rel);
    HMODULE m = LoadLibraryExW(p.c_str(), nullptr, kLoadFlags);
    if (!m) logf("xess: %S missing (user-supplied)", p.c_str());
    return m;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
    return b;
}
void flip(D3D12_RESOURCE_BARRIER* b, UINT n) { for (UINT i = 0; i < n; ++i) std::swap(b[i].Transition.StateBefore, b[i].Transition.StateAfter); }

float frameMs(LARGE_INTEGER& last) {
    LARGE_INTEGER f, n; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&n);
    const float ms = last.QuadPart ? float(double(n.QuadPart - last.QuadPart) * 1000.0 / double(f.QuadPart)) : 16.7f;
    last = n;
    return ms > 0 && ms < 1000 ? ms : 16.7f;
}

// ---- upscaling (libxess.dll) -------------------------------------------------------
struct {
    decltype(&xessGetVersion) xessGetVersion;
    decltype(&xessD3D12CreateContext) xessD3D12CreateContext;
    decltype(&xessD3D12BuildPipelines) xessD3D12BuildPipelines;
    decltype(&xessD3D12Init) xessD3D12Init;
    decltype(&xessD3D12Execute) xessD3D12Execute;
    decltype(&xessGetOptimalInputResolution) xessGetOptimalInputResolution;
    decltype(&xessSetVelocityScale) xessSetVelocityScale;
    decltype(&xessSetLoggingCallback) xessSetLoggingCallback;
} sr;
int g_srLoaded = -1;  // -1 untried
char g_srVersion[16];

constexpr uint32_t kSrFlags = XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;  // HDR input, motion vectors at input size, not jittered, depth not inverted
xess_context_handle_t g_sr;
UINT g_srIn[2], g_srOut[2];
int g_srPreset = -1;  // initialised preset, -1 none
bool g_srBelow;       // the game's render size was below every preset's range
Up12Tex g_color, g_out;  // XeSS's input colour and output, shared with the game's D3D11 device

void srLog(const char* m, xess_logging_level_t l) { logf("xess: %s %s", l >= XESS_LOGGING_LEVEL_ERROR ? "error" : "warning", m); }

const char* presetName(int q) {
    switch (q) {
    case XESS_QUALITY_SETTING_AA: return "native anti-aliasing";
    case XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS: return "ultra quality plus";
    case XESS_QUALITY_SETTING_ULTRA_QUALITY: return "ultra quality";
    case XESS_QUALITY_SETTING_QUALITY: return "quality";
    case XESS_QUALITY_SETTING_BALANCED: return "balanced";
    case XESS_QUALITY_SETTING_PERFORMANCE: return "performance";
    case XESS_QUALITY_SETTING_ULTRA_PERFORMANCE: return "ultra performance";
    default: return "?";
    }
}

bool srLoad() {
    if (g_srLoaded < 0) {
        HMODULE m = loadDll(L"ffxv_dlss\\xess\\libxess.dll");
        const bool ok = m && SYM(m, sr, xessGetVersion) && SYM(m, sr, xessD3D12CreateContext) && SYM(m, sr, xessD3D12BuildPipelines) &&
                        SYM(m, sr, xessD3D12Init) && SYM(m, sr, xessD3D12Execute) && SYM(m, sr, xessGetOptimalInputResolution) &&
                        SYM(m, sr, xessSetVelocityScale) && SYM(m, sr, xessSetLoggingCallback);
        xess_version_t v = {};
        if (ok && sr.xessGetVersion(&v) == XESS_RESULT_SUCCESS) snprintf(g_srVersion, sizeof g_srVersion, "%u.%u.%u", v.major, v.minor, v.patch);
        g_srLoaded = ok ? 1 : 0;
        logf("xess: libxess.dll %s, version %s", ok ? "loaded" : "not usable, XeSS upscaling off", g_srVersion);
    }
    return g_srLoaded == 1;
}

// The context once (pipelines built in the background straight away, so the first Init does not stall), then Init
// again whenever the output size or the preset for the input size changes.
bool ensureUpscaler(const P12Shared& s, UINT inW, UINT inH, UINT outW, UINT outH) {
    if (g_srPreset >= 0 && g_srIn[0] == inW && g_srIn[1] == inH && g_srOut[0] == outW && g_srOut[1] == outH) return true;
    if (!g_sr) {
        const xess_result_t r = sr.xessD3D12CreateContext(s.dev, &g_sr);
        logf("xess: upscaler context: %d", r);
        if (r < XESS_RESULT_SUCCESS) { g_sr = nullptr; return false; }  // warnings (an old driver) are positive
        sr.xessSetLoggingCallback(g_sr, XESS_LOGGING_LEVEL_WARNING, srLog);
        sr.xessD3D12BuildPipelines(g_sr, nullptr, false, kSrFlags);
    }
    static const xess_quality_settings_t kPresets[] = {  // highest quality first
        XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS, XESS_QUALITY_SETTING_ULTRA_QUALITY, XESS_QUALITY_SETTING_QUALITY,
        XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE };
    XessRange ranges[6]; int n = 0;
    const xess_2d_t out = { outW, outH };
    for (xess_quality_settings_t q : kPresets) {
        xess_2d_t opt, lo, hi;
        if (sr.xessGetOptimalInputResolution(g_sr, &out, q, &opt, &lo, &hi) == XESS_RESULT_SUCCESS) ranges[n++] = { int(q), lo.x, lo.y, hi.x, hi.y };
    }
    bool fits;
    const int preset = xessPickPreset(inW, inH, outW, outH, ranges, n, XESS_QUALITY_SETTING_AA, &fits);
    if (!fits) {  // nothing out of range is submitted: the menu says to pick a larger scaling mode
        if (!g_srBelow) { g_srBelow = true; logf("xess: input %ux%u is below every preset's range for %ux%u, XeSS upscaling off", inW, inH, outW, outH); flushLog(); }
        return false;
    }
    if (preset != g_srPreset || g_srOut[0] != outW || g_srOut[1] != outH) {
        if (g_srPreset >= 0 && !up12Idle(s)) return false;  // the last Execute may still be running
        g_srPreset = -1;
        xess_d3d12_init_params_t ip = {};
        ip.outputResolution = out;
        ip.qualitySetting = xess_quality_settings_t(preset);
        ip.initFlags = kSrFlags;
        const xess_result_t r = sr.xessD3D12Init(g_sr, &ip);
        logf("xess: upscaler %ux%u -> %ux%u, preset %s: %d, version %s", inW, inH, outW, outH, presetName(preset), r, g_srVersion);
        flushLog();
        if (r < XESS_RESULT_SUCCESS) return false;
        g_srPreset = preset; g_srOut[0] = outW; g_srOut[1] = outH;
    }
    sr.xessSetVelocityScale(g_sr, float(inW), float(inH));  // our motion vectors are UV units (prev - cur), as for DLSS
    g_srIn[0] = inW; g_srIn[1] = inH;
    return true;
}

// ---- frame generation and Xe Low Latency (libxess_fg.dll, libxell.dll) ---------------
struct {
    decltype(&xefgSwapChainGetVersion) xefgSwapChainGetVersion;
    decltype(&xefgSwapChainD3D12CreateContext) xefgSwapChainD3D12CreateContext;
    decltype(&xefgSwapChainSetLoggingCallback) xefgSwapChainSetLoggingCallback;
    decltype(&xefgSwapChainSetLatencyReduction) xefgSwapChainSetLatencyReduction;
    decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) xefgSwapChainD3D12InitFromSwapChainDesc;
    decltype(&xefgSwapChainD3D12GetSwapChainPtr) xefgSwapChainD3D12GetSwapChainPtr;
    decltype(&xefgSwapChainSetUiCompositionState) xefgSwapChainSetUiCompositionState;
    decltype(&xefgSwapChainSetEnabled) xefgSwapChainSetEnabled;
    decltype(&xefgSwapChainD3D12TagFrameResource) xefgSwapChainD3D12TagFrameResource;
    decltype(&xefgSwapChainTagFrameConstants) xefgSwapChainTagFrameConstants;
    decltype(&xefgSwapChainSetPresentId) xefgSwapChainSetPresentId;
    decltype(&xefgSwapChainGetLastPresentStatus) xefgSwapChainGetLastPresentStatus;
    decltype(&xefgSwapChainDestroy) xefgSwapChainDestroy;
} fg;
struct {
    decltype(&xellGetVersion) xellGetVersion;
    decltype(&xellD3D12CreateContext) xellD3D12CreateContext;
    decltype(&xellSetLoggingCallback) xellSetLoggingCallback;
    decltype(&xellSetSleepMode) xellSetSleepMode;
    decltype(&xellSleep) xellSleep;
    decltype(&xellAddMarkerData) xellAddMarkerData;
    decltype(&xellDestroyContext) xellDestroyContext;
} ll;
int g_fgLoaded = -1;
char g_fgVersion[16], g_llVersion[16];
xefg_swapchain_handle_t g_fgCtx;
xell_context_handle_t g_ll;
LARGE_INTEGER g_fgLast;

void fgLog(const char* m, xefg_swapchain_logging_level_t l, void*) { logf("xess: frame generation %s %s", l >= XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR ? "error" : "warning", m); }
void llLog(const char* m, xell_logging_level_t l) { logf("xess: low latency %s %s", l >= XELL_LOGGING_LEVEL_ERROR ? "error" : "warning", m); }

// XeLL first: XeSS-FG needs it to exist for its whole life.
bool fgLoad() {
    if (g_fgLoaded < 0) {
        HMODULE ml = loadDll(L"ffxv_dlss\\xess\\libxell.dll");
        HMODULE mf = ml ? loadDll(L"ffxv_dlss\\xess\\libxess_fg.dll") : nullptr;
        const bool okL = ml && SYM(ml, ll, xellGetVersion) && SYM(ml, ll, xellD3D12CreateContext) && SYM(ml, ll, xellSetLoggingCallback) &&
                         SYM(ml, ll, xellSetSleepMode) && SYM(ml, ll, xellSleep) && SYM(ml, ll, xellAddMarkerData) && SYM(ml, ll, xellDestroyContext);
        const bool okF = mf && SYM(mf, fg, xefgSwapChainGetVersion) && SYM(mf, fg, xefgSwapChainD3D12CreateContext) &&
                         SYM(mf, fg, xefgSwapChainSetLoggingCallback) && SYM(mf, fg, xefgSwapChainSetLatencyReduction) &&
                         SYM(mf, fg, xefgSwapChainD3D12InitFromSwapChainDesc) && SYM(mf, fg, xefgSwapChainD3D12GetSwapChainPtr) &&
                         SYM(mf, fg, xefgSwapChainSetUiCompositionState) && SYM(mf, fg, xefgSwapChainSetEnabled) &&
                         SYM(mf, fg, xefgSwapChainD3D12TagFrameResource) && SYM(mf, fg, xefgSwapChainTagFrameConstants) &&
                         SYM(mf, fg, xefgSwapChainSetPresentId) && SYM(mf, fg, xefgSwapChainGetLastPresentStatus) && SYM(mf, fg, xefgSwapChainDestroy);
        xell_version_t lv = {}; xefg_swapchain_version_t fv = {};
        if (okL && ll.xellGetVersion(&lv) == XELL_RESULT_SUCCESS) snprintf(g_llVersion, sizeof g_llVersion, "%u.%u.%u", lv.major, lv.minor, lv.patch);
        if (okF && fg.xefgSwapChainGetVersion(&fv) == XEFG_SWAPCHAIN_RESULT_SUCCESS) snprintf(g_fgVersion, sizeof g_fgVersion, "%u.%u.%u", fv.major, fv.minor, fv.patch);
        g_fgLoaded = okL && okF ? 1 : 0;
        logf("xess: frame generation DLLs %s, XeSS-FG %s, XeLL %s", g_fgLoaded ? "loaded" : "not usable, XeSS frame generation off", g_fgVersion, g_llVersion);
    }
    return g_fgLoaded == 1;
}

// The game provides no camera data: the same stand-ins as Streamline's constants and FSR (present12.cpp, fsr.cpp).
constexpr float kNear = 0.1f, kFar = 10000.f, kFovY = 1.0472f;
}  // namespace

bool xessInputBelowRange() { return g_srBelow; }

bool xessPresent() { return GetFileAttributesW(path(L"ffxv_dlss\\xess\\libxess.dll").c_str()) != INVALID_FILE_ATTRIBUTES; }
bool xessFgPresent() {
    for (const wchar_t* f : { L"ffxv_dlss\\xess\\libxess.dll", L"ffxv_dlss\\xess\\libxess_fg.dll", L"ffxv_dlss\\xess\\libxell.dll" })
        if (GetFileAttributesW(path(f).c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    return true;
}
const char* xessVersion() { return g_srVersion; }
const char* xessFgVersion() { return g_fgVersion; }

bool xessUpscale(ID3D11Texture2D* color, ID3D11Texture2D* depth, ID3D11Texture2D* mv, ID3D11Texture2D* out,
                 UINT inW, UINT inH, UINT outW, UINT outH, const float jitterPx[2], bool reset) {
    P12Shared s;
    if (!srLoad() || !p12Shared(&s)) return false;
    ID3D12Resource *depth12 = p12Open(1, depth, inW), *mv12 = p12Open(0, mv, inW);
    if (!depth12 || !mv12 || !up12Ensure(s, g_color, inW, inH, false, "colour") || !up12Ensure(s, g_out, outW, outH, true, "output") ||
        !ensureUpscaler(s, inW, inH, outW, outH)) return false;
    s.ctx11->CopyResource(g_color.r11, color);
    up12ToD3D12(s);
    ID3D12GraphicsCommandList* cl = up12Begin(s);
    if (!cl) return false;
    // XeSS adds no barriers of its own: inputs in NON_PIXEL_SHADER_RESOURCE, the output in UNORDERED_ACCESS, back after
    D3D12_RESOURCE_BARRIER b[] = { transition(g_color.r12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                   transition(mv12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                   transition(depth12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                   transition(g_out.r12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) };
    cl->ResourceBarrier(_countof(b), b);
    xess_d3d12_execute_params_t e = {};
    e.pColorTexture = g_color.r12; e.pVelocityTexture = mv12; e.pDepthTexture = depth12; e.pOutputTexture = g_out.r12;
    e.jitterOffsetX = jitterPx[0]; e.jitterOffsetY = jitterPx[1];  // the values DLSS gets: pixels
    e.exposureScale = 1.f;
    e.resetHistory = reset ? 1 : 0;
    e.inputWidth = inW; e.inputHeight = inH;
    xess_result_t r = sr.xessD3D12Execute(g_sr, cl, &e);
    flip(b, _countof(b));
    cl->ResourceBarrier(_countof(b), b);
    if (!up12Submit(s) && r >= XESS_RESULT_SUCCESS) r = XESS_RESULT_ERROR_UNKNOWN;  // closes the list either way
    static int traced;
    if (traced < 2 || r < XESS_RESULT_SUCCESS) { ++traced; logf("xess: execute %ux%u -> %ux%u jitter %.3f,%.3f reset %d: %d", inW, inH, outW, outH, jitterPx[0], jitterPx[1], reset, r); }
    if (r < XESS_RESULT_SUCCESS) return false;
    up12ToD3D11(s);
    s.ctx11->CopyResource(out, g_out.r11);
    return true;
}

IDXGISwapChain1* xessFgSwapchain(IDXGIFactory2* f, ID3D12Device* dev, ID3D12CommandQueue* q, HWND wnd, DXGI_SWAP_CHAIN_DESC1* desc, int latency) {
    if (!fgLoad()) return nullptr;
    const xell_result_t lr = ll.xellD3D12CreateContext(dev, &g_ll);
    logf("xess: low latency context: %d", lr);
    if (lr != XELL_RESULT_SUCCESS) { g_ll = nullptr; return nullptr; }
    ll.xellSetLoggingCallback(g_ll, XELL_LOGGING_LEVEL_WARNING, llLog);
    xessFgSleepMode(latency);
    xefg_swapchain_result_t r = fg.xefgSwapChainD3D12CreateContext(dev, &g_fgCtx);
    if (r >= XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        fg.xefgSwapChainSetLoggingCallback(g_fgCtx, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, fgLog, nullptr);
        r = fg.xefgSwapChainSetLatencyReduction(g_fgCtx, g_ll);
    } else g_fgCtx = nullptr;
    if (r >= XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        xefg_swapchain_d3d12_init_params_t ip = {};
        ip.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;  // depth not inverted, motion vectors at render size, not jittered
        ip.maxInterpolatedFrames = 1;
        ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS;  // interpolate the HUD-less image, take the HUD and menu from the back buffer
        r = fg.xefgSwapChainD3D12InitFromSwapChainDesc(g_fgCtx, wnd, desc, nullptr, q, f, &ip);
    }
    IDXGISwapChain1* sc = nullptr;
    if (r >= XEFG_SWAPCHAIN_RESULT_SUCCESS) r = fg.xefgSwapChainD3D12GetSwapChainPtr(g_fgCtx, IID_PPV_ARGS(&sc));
    if (r >= XEFG_SWAPCHAIN_RESULT_SUCCESS) r = fg.xefgSwapChainSetUiCompositionState(g_fgCtx, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
    logf("xess: frame generation swapchain %p: %d, XeSS-FG %s, XeLL %s", (void*)sc, r, g_fgVersion, g_llVersion);
    flushLog();
    if (r < XEFG_SWAPCHAIN_RESULT_SUCCESS || !sc) { release(sc); xessFgDestroy(); return nullptr; }
    return sc;
}

void xessFgDestroy() {
    if (g_fgCtx) { const xefg_swapchain_result_t r = fg.xefgSwapChainDestroy(g_fgCtx); g_fgCtx = nullptr; logf("xess: frame generation destroyed: %d", r); }
    if (g_ll) { ll.xellDestroyContext(g_ll); g_ll = nullptr; }  // after XeSS-FG, which uses it to the end
}

void xessFgEnable(bool on) {
    if (!g_fgCtx) return;
    const xefg_swapchain_result_t r = fg.xefgSwapChainSetEnabled(g_fgCtx, on ? 1 : 0);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) logf("xess: frame generation %s: %d", on ? "on" : "off", r);
}

void xessFgSleepMode(int mode) {
    if (!g_ll) return;
    xell_sleep_params_t p = {};
    p.minimumIntervalUs = 0;  // the mod's own frame cap (fsrPace) holds the rate: one cap mechanism off NVIDIA
    p.bLowLatencyMode = mode >= 1;
    p.bLowLatencyBoost = mode >= 2;
    const xell_result_t r = ll.xellSetSleepMode(g_ll, &p);
    logf("xess: low latency %s: %d", mode >= 2 ? "on + boost" : mode == 1 ? "on" : "off", r);
}

// Frame `id` starts: XeLL's sleep, then the start of the simulation span.
void xessFgFrameStart(uint32_t id) {
    if (!g_ll) return;
    ll.xellSleep(g_ll, id);
    ll.xellAddMarkerData(g_ll, id, XELL_SIMULATION_START);
}
void xessFgFirstDraw(uint32_t id) {
    if (!g_ll) return;
    ll.xellAddMarkerData(g_ll, id, XELL_SIMULATION_END);
    ll.xellAddMarkerData(g_ll, id, XELL_RENDERSUBMIT_START);
}
void xessFgBeforePresent(uint32_t id) {
    if (!g_ll) return;
    ll.xellAddMarkerData(g_ll, id, XELL_RENDERSUBMIT_END);
    ll.xellAddMarkerData(g_ll, id, XELL_PRESENT_START);
    if (g_fgCtx) fg.xefgSwapChainSetPresentId(g_fgCtx, id);  // the inputs tagged for `id` belong to this Present
}
unsigned xessFgAfterPresent(uint32_t id) {
    if (g_ll) ll.xellAddMarkerData(g_ll, id, XELL_PRESENT_END);
    if (!g_fgCtx) return 1;
    xefg_swapchain_present_status_t st = {};
    fg.xefgSwapChainGetLastPresentStatus(g_fgCtx, &st);
    static int lastR = 1;  // warnings after a history reset or a resize are normal: errors only
    const int r = st.frameGenResult < XEFG_SWAPCHAIN_RESULT_SUCCESS ? int(st.frameGenResult) : 0;
    if (r != lastR) { lastR = r; logf("xess: frame generation present status %d", r); }
    return st.framesPresented ? st.framesPresented : 1;
}

bool xessFgFrame(bool on, const FgFrame* f, ID3D12Resource* depth, ID3D12Resource* mv, ID3D12Resource* hudless, bool reset, uint32_t id) {
    if (!g_fgCtx) return false;
    const float ms = frameMs(g_fgLast);
    xefg_swapchain_frame_constant_data_t c = {};
    c.viewMatrix[0] = c.viewMatrix[5] = c.viewMatrix[10] = c.viewMatrix[15] = 1.f;  // no camera from the game: identity view
    P12Shared s;
    const bool gen = on && f && depth && mv && p12Shared(&s);
    ID3D12GraphicsCommandList* cl = gen ? up12Begin(s) : nullptr;
    if (!cl) {  // no inputs this frame: show the rendered frame only, and start the history again from the next one
        if (on) { c.resetHistory = 1; fg.xefgSwapChainTagFrameConstants(g_fgCtx, id, &c); }
        return false;
    }
    // Our textures are shared with D3D11 and rewritten next frame: XeSS copies them on this list (ONLY_NOW) from COPY_SOURCE.
    ID3D12Resource* res[] = { depth, mv, hudless };
    const xefg_swapchain_resource_type_t type[] = { XEFG_SWAPCHAIN_RES_DEPTH, XEFG_SWAPCHAIN_RES_MOTION_VECTOR, XEFG_SWAPCHAIN_RES_HUDLESS_COLOR };
    const UINT n = hudless ? 3 : 2;
    D3D12_RESOURCE_BARRIER b[3];
    for (UINT i = 0; i < n; ++i) b[i] = transition(res[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->ResourceBarrier(n, b);
    xefg_swapchain_result_t r = XEFG_SWAPCHAIN_RESULT_SUCCESS;
    for (UINT i = 0; i < n && r >= XEFG_SWAPCHAIN_RESULT_SUCCESS; ++i) {
        xefg_swapchain_d3d12_resource_data_t d = {};
        d.type = type[i];
        d.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
        d.pResource = res[i];
        d.incomingState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        const D3D12_RESOURCE_DESC rd = res[i]->GetDesc();
        d.resourceSize = i < 2 ? xefg_swapchain_2d_t{ f->w, f->h } : xefg_swapchain_2d_t{ UINT(rd.Width), rd.Height };  // mv/depth: the render size
        r = fg.xefgSwapChainD3D12TagFrameResource(g_fgCtx, cl, id, &d);
    }
    flip(b, n);
    cl->ResourceBarrier(n, b);
    if (!up12Submit(s) && r >= XEFG_SWAPCHAIN_RESULT_SUCCESS) r = XEFG_SWAPCHAIN_RESULT_ERROR_UNKNOWN;  // closes the list either way
    // the same stand-in projection as Streamline's constants (present12.cpp), row-major
    const float aspect = float(f->w) / float(f->h), cot = 1.f / std::tan(kFovY * 0.5f);
    const float proj[16] = { cot / aspect, 0, 0, 0,  0, cot, 0, 0,  0, 0, kFar / (kFar - kNear), 1,  0, 0, -kNear * kFar / (kFar - kNear), 0 };
    for (int i = 0; i < 16; ++i) c.projectionMatrix[i] = proj[i];
    c.jitterOffsetX = f->jitterPx[0]; c.jitterOffsetY = f->jitterPx[1];
    c.motionVectorScaleX = float(f->w); c.motionVectorScaleY = float(f->h);  // UV-unit motion vectors
    c.resetHistory = reset || r < XEFG_SWAPCHAIN_RESULT_SUCCESS ? 1 : 0;  // a failed tag: no interpolation from missing inputs
    c.frameRenderTime = ms;
    const xefg_swapchain_result_t rc = fg.xefgSwapChainTagFrameConstants(g_fgCtx, id, &c);
    static int lastR = 1;
    const int lr = r < XEFG_SWAPCHAIN_RESULT_SUCCESS ? int(r) : int(rc);
    if (lr != lastR) { lastR = lr; logf("xess: frame generation inputs: %d", lr); }
    return r >= XEFG_SWAPCHAIN_RESULT_SUCCESS && rc >= XEFG_SWAPCHAIN_RESULT_SUCCESS;
}
