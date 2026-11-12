// AMD FSR through the FidelityFX SDK 2 API (headers in third_party/fidelityfx; AMD's signed DLLs are user-supplied in
// ffxv_dlss\fidelityfx). Everything runs on the presenter's D3D12 device (present12.cpp):
// - Upscaling replaces DLSS at the TAA point. The game renders in D3D11, so the colour goes to D3D12 through a shared
//   texture, FSR runs on the presenter's queue (up12.cpp: shared fences, GPU waits only) and the result is copied back
//   for the game's post chain. AMD's loader picks the newest FSR the GPU supports.
// - Frame generation is FSR's frame interpolation swapchain in place of Streamline's, fed the same motion vectors and
//   depth as DLSS-G (prepared once per rendered frame before Present).
#include "common.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

namespace {
HMODULE g_dll;
ffxFunctions g_ffx;
int g_loaded = -1;  // -1 untried

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
void ffxMsg(uint32_t type, const wchar_t* m) { logf("fsr: %s %S", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", m); }

FfxApiResource res(ID3D12Resource* r) { return ffxApiGetResourceDX12(r, FFX_API_RESOURCE_STATE_COMMON); }

float frameMs(LARGE_INTEGER& last) {
    LARGE_INTEGER f, n; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&n);
    const float ms = last.QuadPart ? float(double(n.QuadPart - last.QuadPart) * 1000.0 / double(f.QuadPart)) : 16.7f;
    last = n;
    return ms > 0 && ms < 1000 ? ms : 16.7f;
}

// The game provides no camera data: the same stand-ins as Streamline's constants (present12.cpp). Depth is [0, 1], not
// inverted.
constexpr float kNear = 0.1f, kFar = 10000.f, kFovY = 1.0472f;

// ---- upscaling -------------------------------------------------------------------
ffxContext g_up;
UINT g_upIn[2], g_upOut[2];
char g_upVersion[32];
LARGE_INTEGER g_upLast;
Up12Tex g_color, g_out;  // FSR's input colour and output, shared with the game's D3D11 device

bool ensureUpscaler(const P12Shared& s, UINT inW, UINT inH, UINT outW, UINT outH) {
    if (g_up && g_upIn[0] == inW && g_upIn[1] == inH && g_upOut[0] == outW && g_upOut[1] == outH) return true;
    if (g_up) { if (!up12Idle(s)) return false; g_ffx.DestroyContext(&g_up, nullptr); g_up = nullptr; }
    ffxCreateContextDescUpscaleVersion ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ffxCreateBackendDX12Desc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.header.pNext = &ver.header;
    be.device = s.dev;
    ffxCreateContextDescUpscale cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.header.pNext = &be.header;
    cd.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;  // linear HDR scene colour, as DLSS gets
    cd.maxRenderSize = { inW, inH };
    cd.maxUpscaleSize = { outW, outH };
    cd.fpMessage = ffxMsg;
    const ffxReturnCode_t r = g_ffx.CreateContext(&g_up, &cd.header, nullptr);
    ffxQueryGetProviderVersion q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (r == FFX_API_RETURN_OK && g_ffx.Query(&g_up, &q.header) == FFX_API_RETURN_OK && q.versionName)
        snprintf(g_upVersion, sizeof g_upVersion, "%s", q.versionName);
    logf("fsr: upscaler %ux%u -> %ux%u: %u, version %s", inW, inH, outW, outH, r, g_upVersion);
    flushLog();
    if (r != FFX_API_RETURN_OK) { g_up = nullptr; return false; }
    g_upIn[0] = inW; g_upIn[1] = inH; g_upOut[0] = outW; g_upOut[1] = outH;
    return true;
}

// ---- frame generation --------------------------------------------------------------
ffxContext g_scCtx, g_fgCtx;
UINT g_fgSize[2]; DXGI_FORMAT g_fgFmt; int g_fgEnc;
UINT64 g_frameId;
LARGE_INTEGER g_fgLast;
char g_fgVersion[32];

// The swapchain takes the transfer function from SetColorSpace1, which the game never calls (its HDR10 goes through NVAPI):
// tell FSR how the back buffer is encoded, or it linearises PQ as sRGB.
ffxReturnCode_t fgDispatch(ffxDispatchDescFrameGeneration* p, void* ctx) {
    const int enc = p12OutputEncoding(p->minMaxLuminance);
    p->backbufferTransferFunction = enc == 1 ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ
                                  : enc == 2 ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    return g_ffx.Dispatch((ffxContext*)ctx, &p->header);
}

bool ensureFg(ID3D12Device* dev, IDXGISwapChain3* sc) {
    DXGI_SWAP_CHAIN_DESC1 d; sc->GetDesc1(&d);
    const int enc = p12OutputEncoding(nullptr);
    if (g_fgCtx && g_fgSize[0] == d.Width && g_fgSize[1] == d.Height && g_fgFmt == d.Format && g_fgEnc == enc) return true;
    fsrFgRelease(sc);
    ffxCreateContextDescFrameGenerationVersion ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateBackendDX12Desc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.header.pNext = &ver.header;
    be.device = dev;
    ffxCreateContextDescFrameGeneration cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.header.pNext = &be.header;
    const bool hdr = enc != 0;  // the signal, not the format: a 10-bit back buffer can be SDR
    cd.flags = hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0;
    cd.displaySize = { d.Width, d.Height };
    cd.maxRenderSize = { d.Width, d.Height };  // render res is never above the display's
    cd.backBufferFormat = ffxApiGetSurfaceFormatDX12(d.Format);
    const ffxReturnCode_t r = g_ffx.CreateContext(&g_fgCtx, &cd.header, nullptr);
    ffxQueryGetProviderVersion q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (r == FFX_API_RETURN_OK && g_ffx.Query(&g_fgCtx, &q.header) == FFX_API_RETURN_OK && q.versionName)
        snprintf(g_fgVersion, sizeof g_fgVersion, "%s", q.versionName);
    logf("fsr: frame generation %ux%u fmt %u hdr %d: %u, version %s", d.Width, d.Height, d.Format, hdr, r, g_fgVersion);
    flushLog();
    if (r != FFX_API_RETURN_OK) { g_fgCtx = nullptr; return false; }
    g_fgSize[0] = d.Width; g_fgSize[1] = d.Height; g_fgFmt = d.Format; g_fgEnc = enc;
    return true;
}
}  // namespace

bool fsrLoad() {
    if (g_loaded < 0) {
        const std::wstring dll = path(L"ffxv_dlss\\fidelityfx\\amd_fidelityfx_loader_dx12.dll");
        // The loader opens its providers by bare name (exe folder / system search), so load them first by full path:
        // a module already loaded under that name satisfies its LoadLibrary.
        for (const wchar_t* p : { L"ffxv_dlss\\fidelityfx\\amd_fidelityfx_upscaler_dx12.dll", L"ffxv_dlss\\fidelityfx\\amd_fidelityfx_framegeneration_dx12.dll" })
            if (!LoadLibraryExW(path(p).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) logf("fsr: %S missing", p);
        g_dll = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (g_dll) ffxLoadFunctions(&g_ffx, g_dll);
        g_loaded = g_dll && g_ffx.CreateContext && g_ffx.Dispatch ? 1 : 0;
        logf("fsr: %S %s", dll.c_str(), g_loaded ? "loaded" : "missing (user-supplied), FSR off");
    }
    return g_loaded == 1;
}
bool fsrPresent() { return GetFileAttributesW(path(L"ffxv_dlss\\fidelityfx\\amd_fidelityfx_loader_dx12.dll").c_str()) != INVALID_FILE_ATTRIBUTES; }
const char* fsrUpscalerVersion() { return g_upVersion; }
const char* fsrFgVersion() { return g_fgVersion; }

bool fsrUpscale(ID3D11Texture2D* color, ID3D11Texture2D* depth, ID3D11Texture2D* mv, ID3D11Texture2D* out,
                UINT inW, UINT inH, UINT outW, UINT outH, const float jitterPx[2], bool reset) {
    P12Shared s;
    if (!fsrLoad() || !p12Shared(&s)) return false;
    ID3D12Resource *depth12 = p12Open(1, depth, inW), *mv12 = p12Open(0, mv, inW);
    if (!depth12 || !mv12 || !up12Ensure(s, g_color, inW, inH, false, "colour") || !up12Ensure(s, g_out, outW, outH, true, "output") ||
        !ensureUpscaler(s, inW, inH, outW, outH)) return false;
    s.ctx11->CopyResource(g_color.r11, color);
    up12ToD3D12(s);
    ID3D12GraphicsCommandList* cl = up12Begin(s);
    if (!cl) return false;
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cl;
    d.color = res(g_color.r12); d.depth = res(depth12); d.motionVectors = res(mv12); d.output = res(g_out.r12);
    d.jitterOffset = { jitterPx[0], jitterPx[1] };   // the values DLSS gets: pixels
    d.motionVectorScale = { float(inW), float(inH) };  // our motion vectors are UV units (prev - cur), as for DLSS
    d.renderSize = { inW, inH };
    d.upscaleSize = { outW, outH };
    d.frameTimeDelta = frameMs(g_upLast);
    d.preExposure = 1.f;
    d.reset = reset;
    d.cameraNear = kNear; d.cameraFar = kFar; d.cameraFovAngleVertical = kFovY;
    d.viewSpaceToMetersFactor = 1.f;
    ffxReturnCode_t r = g_ffx.Dispatch(&g_up, &d.header);
    if (!up12Submit(s) && r == FFX_API_RETURN_OK) r = FFX_API_RETURN_ERROR;  // closes the list either way
    static int traced;
    if (traced < 2 || r != FFX_API_RETURN_OK) { ++traced; logf("fsr: dispatch %ux%u -> %ux%u jitter %.3f,%.3f reset %d: %u", inW, inH, outW, outH, jitterPx[0], jitterPx[1], reset, r); }
    if (r != FFX_API_RETURN_OK) return false;
    up12ToD3D11(s);
    s.ctx11->CopyResource(out, g_out.r11);
    return true;
}

IDXGISwapChain1* fsrFgSwapchain(IDXGIFactory2* f, ID3D12CommandQueue* q, HWND wnd, DXGI_SWAP_CHAIN_DESC1* desc) {
    if (!fsrLoad()) return nullptr;
    IDXGISwapChain4* sc = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    d.header.pNext = &ver.header;
    d.swapchain = &sc; d.hwnd = wnd; d.desc = desc; d.fullscreenDesc = nullptr; d.dxgiFactory = f; d.gameQueue = q;
    const ffxReturnCode_t r = g_ffx.CreateContext(&g_scCtx, &d.header, nullptr);
    logf("fsr: frame generation swapchain %p: %u", (void*)sc, r);
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (r == FFX_API_RETURN_OK && g_ffx.Query(&g_scCtx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
        logf("fsr: frame generation swapchain version %s", pv.versionName);
    if (r != FFX_API_RETURN_OK) { g_scCtx = nullptr; release(sc); }
    return sc;
}

void fsrFgUi(ID3D12Resource* ui) {
    if (!g_scCtx) return;
    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 d = {};
    d.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    if (ui) d.uiResource = ffxApiGetResourceDX12(ui, FFX_API_RESOURCE_STATE_PIXEL_READ);  // its state between frames
    // premultiplied (as the menu's composite writes it); copied at Present, so the menu can redraw it next frame
    d.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA | FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
    const ffxReturnCode_t r = g_ffx.Configure(&g_scCtx, &d.header);
    static ffxReturnCode_t lastR = ~0u;
    if (r != lastR) { lastR = r; logf("fsr: ui layer %s: %u", ui ? "on" : "off", r); }
}

void fsrFgRelease(IDXGISwapChain3* sc) {
    if (!g_fgCtx) return;
    ffxConfigureDescFrameGeneration c = {};  // detach from the swapchain before the context goes
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = sc;
    g_ffx.Configure(&g_fgCtx, &c.header);
    g_ffx.DestroyContext(&g_fgCtx, nullptr);
    g_fgCtx = nullptr;
}

void fsrFgDestroy(IDXGISwapChain3* sc) {
    fsrFgRelease(sc);
    if (g_scCtx) { g_ffx.DestroyContext(&g_scCtx, nullptr); g_scCtx = nullptr; }  // drops its reference to the swapchain
}

bool fsrFgFrame(ID3D12Device* dev, IDXGISwapChain3* sc, bool on, const FgFrame* f, ID3D12Resource* depth, ID3D12Resource* mv,
                ID3D12Resource* hudless, bool reset) {
    if (!g_scCtx || !ensureFg(dev, sc)) return false;
    P12Shared s;
    if (!p12Shared(&s)) return false;
    const bool gen = on && f && depth && mv;
    ++g_frameId;  // exactly +1 per frame, or FSR resets
    ffxConfigureDescFrameGeneration c = {};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = sc;
    c.frameGenerationCallback = fgDispatch;
    c.frameGenerationCallbackUserContext = &g_fgCtx;
    c.frameGenerationEnabled = gen;
    c.generationRect = { 0, 0, int32_t(g_fgSize[0]), int32_t(g_fgSize[1]) };
    c.frameID = g_frameId;
    if (gen && hudless) c.HUDLessColor = res(hudless);  // same size and format as the back buffer; FSR keeps the HUD out
    ffxReturnCode_t r = g_ffx.Configure(&g_fgCtx, &c.header);
    const float ms = frameMs(g_fgLast);
    ID3D12GraphicsCommandList* cl = gen && r == FFX_API_RETURN_OK ? up12Begin(s) : nullptr;
    if (gen && r == FFX_API_RETURN_OK && !cl) r = FFX_API_RETURN_ERROR;
    if (cl) {
        ffxDispatchDescFrameGenerationPrepareV2 p = {};
        p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        p.frameID = g_frameId;
        p.commandList = cl;
        p.renderSize = { f->w, f->h };
        p.jitterOffset = { f->jitterPx[0], f->jitterPx[1] };
        p.motionVectorScale = { float(f->w), float(f->h) };  // UV-unit motion vectors
        p.frameTimeDelta = ms;
        p.reset = reset;
        p.cameraNear = kNear; p.cameraFar = kFar; p.cameraFovAngleVertical = kFovY;
        p.viewSpaceToMetersFactor = 1.f;
        p.depth = res(depth); p.motionVectors = res(mv);
        p.cameraUp[1] = 1.f; p.cameraRight[0] = 1.f; p.cameraForward[2] = 1.f;
        r = g_ffx.Dispatch(&g_fgCtx, &p.header);
        if (!up12Submit(s) && r == FFX_API_RETURN_OK) r = FFX_API_RETURN_ERROR;
    }
    if (gen && r != FFX_API_RETURN_OK) {  // Present would otherwise interpolate from missing or stale inputs
        c.frameGenerationEnabled = false;
        g_ffx.Configure(&g_fgCtx, &c.header);
    }
    static ffxReturnCode_t lastR = ~0u;
    if (r != lastR) { lastR = r; logf("fsr: frame generation %s: %u", gen ? "on" : "off", r); }
    return gen && r == FFX_API_RETURN_OK;
}
