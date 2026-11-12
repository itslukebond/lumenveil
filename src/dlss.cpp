// Replaces FFXV's TAA resolve with DLSS Super Resolution / DLAA (NGX D3D11).
#include "common.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <nvsdk_ngx_helpers.h>
#include <atomic>
#include <cmath>
#include <vector>
#include <algorithm>
#include <psapi.h>

namespace {

const char* kMvHlsl = R"(
cbuffer cbTemporalAA : register(b0) {
    float4 g_screenSize; float4 g_frameBits; float4 g_uvJitterOffset;
    float4 g_motionMatrix[4]; float4 g_reconstructMatrix[4]; float4 g_unprojectParams;
    float g_maxLuminanceInv; uint g_gamePaused; uint g_hairUseAlphaTest; uint g_waterResponsiveAA;
};
Texture2D<float> depthIn : register(t0);
Texture2D<float4> velIn : register(t1);
RWTexture2D<float2> mvOut : register(u0);
RWTexture2D<float> depthOut : register(u1);

// Same decode as the game's TAA: velocity.y == 1 marks "use camera reprojection",
// |velocity.x| >= 4 carries a flag bit that is stripped.
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (id.x >= (uint)g_screenSize.x || id.y >= (uint)g_screenSize.y) return;
    float2 uv = (id + 0.5) * g_screenSize.zw;
    float d = depthIn[id];
    float4 vel = velIn[id];
    float2 prevUV;
    if (vel.y == 1.0 || g_gamePaused == 0) {
        float4 p = float4(uv, d, 1);
        float x = dot(float4(g_motionMatrix[0].x, g_motionMatrix[1].x, g_motionMatrix[2].x, g_motionMatrix[3].x), p);
        float y = dot(float4(g_motionMatrix[0].y, g_motionMatrix[1].y, g_motionMatrix[2].y, g_motionMatrix[3].y), p);
        float w = dot(float4(g_motionMatrix[0].w, g_motionMatrix[1].w, g_motionMatrix[2].w, g_motionMatrix[3].w), p);
        prevUV = float2(x, y) / w;
    } else {
        float vx = vel.x;
        if (abs(vx) >= 4.0) vx -= 4.0 * sign(vx);
        prevUV = uv + float2(vx, vel.y);
    }
    mvOut[id] = prevUV - uv;
    depthOut[id] = d;
}
)";

// Upscaling (render < display): DLSS writes the display-size image, this CS box-filters it back to render res
// into the TAA target (the game's post chain still runs at render res) and into our own copy of it.
const char* kDownHlsl = R"(
Texture2D<float3> up : register(t0); SamplerState lin : register(s0);
RWTexture2D<float3> taaOut : register(u0); RWTexture2D<float3> copy : register(u1);
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint w, h; taaOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float3 c = up.SampleLevel(lin, (id + 0.5) / float2(w, h), 0);  // 2x: bilinear at the texel centre = 2x2 box
    taaOut[id] = c; copy[id] = c;
}
)";
// At the tonemap: the game's post-processed render-res scene, upsampled, times DLSS's display-res detail (ratio of the
// DLSS image to its own downsample). Where depth of field / motion blur softened the scene (its local contrast is
// below the DLSS downsample's), the detail fades out so those effects stay soft.
const char* kCombineHlsl = R"(
Texture2D<float3> post : register(t0); Texture2D<float3> down : register(t1); Texture2D<float3> up : register(t2);
SamplerState lin : register(s0);
RWTexture2D<float4> outTex : register(u0);
float lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float contrast(Texture2D<float3> t, float2 uv, float2 px) {
    float c = lum(t.SampleLevel(lin, uv, 0));
    float d = abs(lum(t.SampleLevel(lin, uv + float2(px.x, 0), 0)) - c) + abs(lum(t.SampleLevel(lin, uv - float2(px.x, 0), 0)) - c)
            + abs(lum(t.SampleLevel(lin, uv + float2(0, px.y), 0)) - c) + abs(lum(t.SampleLevel(lin, uv - float2(0, px.y), 0)) - c);
    return d / (c + 1e-3);
}
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint W, H; outTex.GetDimensions(W, H);
    if (id.x >= W || id.y >= H) return;
    uint w, h; post.GetDimensions(w, h);
    float2 uv = (id + 0.5) / float2(W, H), px = 1.0 / float2(w, h);
    float3 p = post.SampleLevel(lin, uv, 0), d = down.SampleLevel(lin, uv, 0), u = up[id];
    float k = saturate((contrast(post, uv, px) + 0.02) / (contrast(down, uv, px) + 0.02));
    float3 ratio = clamp((u + 1e-4) / (d + 1e-4), 0.2, 5.0);
    outTex[id] = float4(p * lerp(1.0, ratio, k), 1);
}
)";
// Sharpening after DLSS (its own was removed in 2.5.1): AMD CAS (cross variant), run on max-channel Reinhard so
// bright HDR edges don't ring; that tonemap keeps hue and inverts exactly.
const char* kSharpHlsl = R"(
Texture2D<float3> src : register(t0); RWTexture2D<float3> dst : register(u0);
cbuffer cb : register(b0) { float sharpness; float3 pad; };
float3 tm(float3 c) { return c / (1 + max(c.r, max(c.g, c.b))); }
float3 itm(float3 t) { return t / (1 - min(max(t.r, max(t.g, t.b)), 0.999)); }
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint w, h; dst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    int2 p = id, m = int2(w, h) - 1;
    float3 e = tm(src[p]);
    float3 b = tm(src[int2(p.x, max(p.y - 1, 0))]), hh = tm(src[int2(p.x, min(p.y + 1, m.y))]);
    float3 d = tm(src[int2(max(p.x - 1, 0), p.y)]), f = tm(src[int2(min(p.x + 1, m.x), p.y)]);
    float3 mn = min(e, min(min(b, hh), min(d, f))), mx = max(e, max(max(b, hh), max(d, f)));
    float3 amp = sqrt(saturate(min(mn, 1 - mx) / (mx + 1e-5)));
    float3 wt = amp * lerp(-0.125, -0.2, sharpness);
    dst[id] = itm(saturate((e + wt * (b + hh + d + f)) / (1 + 4 * wt)));
}
)";

ID3D11Device* g_dev;
ID3DDeviceContextState* g_state;
UINT g_dispW, g_dispH;                     // game swapchain size (main.cpp)
ID3D11ComputeShader *g_downCS, *g_combineCS;
ID3D11SamplerState* g_linear;
ID3D11Texture2D *g_upTex, *g_downTex, *g_combTex, *g_tmTex;  // display-res DLSS out, its render-res copy, combine, tonemap twin
ID3D11ShaderResourceView *g_upSRV, *g_downSRV, *g_combSRV, *g_tmSRV;
ID3D11UnorderedAccessView *g_downUAV, *g_combUAV, *g_upUAV;
ID3D11RenderTargetView* g_tmRTV;
// sharpening: DLSS writes g_rawTex, CAS writes the usual target (g_upTex, or the TAA target at 100%)
ID3D11ComputeShader* g_sharpCS;
ID3D11Buffer* g_sharpCB;
ID3D11Texture2D* g_rawTex;
ID3D11ShaderResourceView* g_rawSRV;
std::atomic<int> g_sharp{-1};        // percent; -1 until read from the ini
char g_version[24];                  // loaded nvngx_dlss.dll, "310.9.1"
// game TAA targets (ping-pong), ref held; the oldest are dropped past four (two per render resolution)
std::vector<std::pair<ID3D11Resource*, ID3D11UnorderedAccessView*>> g_taaUAVs;
bool g_upFrame;                            // this frame's DLSS upscaled: the tonemap and blit get redirected
ID3D11Resource* g_tmSrc;                   // game's render-res tonemap target this frame (weak: compared by address)
// tonemap redirect: state saved by dlssTonemapBegin, restored by dlssTonemapEnd
ID3D11RenderTargetView* g_savedRTV[8]; ID3D11DepthStencilView* g_savedDSV; ID3D11ShaderResourceView* g_savedT0;
D3D11_VIEWPORT g_savedVP[16]; UINT g_savedNVP;
NVSDK_NGX_Parameter* g_params;
NVSDK_NGX_Handle* g_feature;
UINT g_fIn[2], g_fOut[2];
std::atomic<int> g_preset{-1};       // -1 until read from the ini; the menu sets it live
std::atomic<bool> g_recreate{false};
ID3D11ComputeShader* g_mvCS;
ID3D11Texture2D *g_mvTex, *g_depthTex;
ID3D11UnorderedAccessView *g_mvUAV, *g_depthUAV;
UINT g_texW, g_texH;
bool g_ready, g_failed;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_reset{true};
std::string g_shotTag;
std::atomic<bool> g_shot{false};
FgFrame g_fg;
bool g_fgReady;

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

int iniInt(const wchar_t* key, int def) {
    return GetPrivateProfileIntW(L"dlss", key, def, path(L"ffxv_dlss.ini").c_str());
}

void NVSDK_CONV ngxLog(const char* msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string s(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    logf("ngx: %s", s.c_str());
}

bool init(ID3D11DeviceContext* ctx) {
    ctx->GetDevice(&g_dev);
    g_dev->Release();  // the game owns the device; keep a weak pointer

    ID3D11Device1* dev1 = nullptr;
    if (FAILED(g_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1))) { logf("dlss: no ID3D11Device1"); return false; }
    D3D_FEATURE_LEVEL fl = g_dev->GetFeatureLevel();
    HRESULT hr = dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state);
    dev1->Release();
    if (FAILED(hr)) { logf("dlss: CreateDeviceContextState %08x", hr); return false; }

    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); wcscat_s(sys, L"\\d3dcompiler_47.dll");
    auto compile = (pD3DCompile)GetProcAddress(LoadLibraryW(sys), "D3DCompile");
    auto makeCS = [&](const char* src, const char* name, ID3D11ComputeShader** cs) {
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT h = compile ? compile(src, strlen(src), name, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err) : E_FAIL;
        if (FAILED(h)) logf("dlss: %s compile failed %s", name, err ? (char*)err->GetBufferPointer() : "");
        if (SUCCEEDED(h)) h = g_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, cs);
        if (code) code->Release();
        if (err) err->Release();
        return SUCCEEDED(h);
    };
    if (!makeCS(kMvHlsl, "ffxv_mv", &g_mvCS)) return false;
    // upscaling helpers: without them DLSS still runs, but only at render res (as before)
    D3D11_SAMPLER_DESC sd = { D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                              D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {}, 0, D3D11_FLOAT32_MAX };
    if (!makeCS(kDownHlsl, "ffxv_down", &g_downCS) || !makeCS(kCombineHlsl, "ffxv_combine", &g_combineCS) ||
        FAILED(g_dev->CreateSamplerState(&sd, &g_linear))) { release(g_downCS); release(g_combineCS); logf("dlss: upscaling off"); }
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    if (!makeCS(kSharpHlsl, "ffxv_sharp", &g_sharpCS) || FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_sharpCB))) {
        release(g_sharpCS); logf("dlss: sharpening off");
    }
    return true;
}

// NGX, loaded the first time DLSS is the upscaler (an AMD GPU running FSR never needs NVIDIA's DLL).
bool initNgx() {
    if (iniInt(L"stage", 3) < 2) { logf("dlss: stage 1 (no NGX)"); return true; }
    static std::wstring modDir = path(L"ffxv_dlss");
    // not shipped with the mod: the user copies NVIDIA's DLL in (see the menu)
    if (GetFileAttributesW((modDir + L"\\nvngx_dlss.dll").c_str()) == INVALID_FILE_ATTRIBUTES) { logf("dlss: %S\\nvngx_dlss.dll missing", modDir.c_str()); return false; }
    // Frame generation's nvngx_dlssg.dll lives with Streamline's files. NVIDIA's runtime picks feature files from the
    // folders given here, and without that folder it loads the older copy that comes with the driver.
    static std::wstring slDir = modDir + L"\\streamline";
    const wchar_t* paths[] = { slDir.c_str(), modDir.c_str() };
    NVSDK_NGX_FeatureCommonInfo fi = {};
    fi.PathListInfo.Path = paths;
    fi.PathListInfo.Length = _countof(paths);
    fi.LoggingInfo.LoggingCallback = ngxLog;
    fi.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    // FFXV's own NGX app id (0xB9DFA64) maps every DLSS feature in this process to the 2018 DLSS 1
    // model, so the default is the SDK's generic developer app id.
    unsigned appId = (unsigned)iniInt(L"app_id", 231313132);
    NVSDK_NGX_Result r = appId
        ? NVSDK_NGX_D3D11_Init(appId, modDir.c_str(), g_dev, &fi)
        : NVSDK_NGX_D3D11_Init_with_ProjectID("6f1c9a3e-2b7d-4e58-9c41-ffa15d1e0715",
              NVSDK_NGX_ENGINE_TYPE_CUSTOM, "ffxv-dlss-1", modDir.c_str(), g_dev, &fi);
    logf("dlss: NGX init app_id=%u -> %08x", appId, r);
    if (NVSDK_NGX_FAILED(r)) { logf("dlss: NGX init failed %08x", r); return false; }
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params);
    int avail = 0, driver = 0;
    if (NVSDK_NGX_SUCCEED(r)) {
        g_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
        g_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &driver);
    }
    logf("dlss: NGX ready=%d SR available=%d needs driver=%d", NVSDK_NGX_SUCCEED(r), avail, driver);
    return avail != 0;
}
int g_ngx = -1;  // -1 untried, 0 failed, 1 ready
bool ngxReady() {
    if (g_ngx < 0) g_ngx = initNgx() ? 1 : 0;
    return g_ngx == 1;
}

// Upscaler: [dlss] upscaler -1 = auto (DLSS on an NVIDIA GPU when NGX loads, XeSS on an Intel GPU with Intel's files,
// else FSR), 0 = DLSS, 1 = FSR, 2 = XeSS; the menu sets it live. Auto only tries NGX on an NVIDIA GPU, so other cards
// never load NVIDIA's DLL unless DLSS is picked.
UINT adapterVendor() {  // PCI vendor of the game's GPU: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel; 0 unknown
    static int vendor = -1;
    if (vendor < 0) {
        vendor = 0;
        IDXGIDevice* d = nullptr; IDXGIAdapter* a = nullptr; DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&d)) && SUCCEEDED(d->GetAdapter(&a)) && SUCCEEDED(a->GetDesc(&ad)))
            vendor = int(ad.VendorId);
        if (a) a->Release();
        if (d) d->Release();
        logf("dlss: GPU vendor %s", vendor == 0x10DE ? "NVIDIA" : vendor == 0x8086 ? "Intel, not NVIDIA" : "not NVIDIA");
    }
    return UINT(vendor);
}
bool nvidiaAdapter() { return adapterVendor() == 0x10DE; }
std::atomic<int> g_upscaler{-2};  // -2 until read from the ini
int g_active = -1;                // this frame's: -1 none (game TAA), 0 DLSS, 1 FSR, 2 XeSS
int pickUpscaler() {
    if (g_upscaler == -2) g_upscaler = iniInt(L"upscaler", -1);
    const bool fsr = p12Enabled() && fsrPresent();    // FSR and XeSS run on the D3D12 presenter
    const bool xess = p12Enabled() && xessPresent();
    switch (g_upscaler.load()) {
    case 0: return ngxReady() ? 0 : -1;
    case 1: return fsr ? 1 : -1;
    case 2: return xess ? 2 : -1;
    default: return nvidiaAdapter() && ngxReady() ? 0 : adapterVendor() == 0x8086 && xess ? 2 : fsr ? 1 : -1;
    }
}

void releaseTargets() {
    for (IUnknown** p : { (IUnknown**)&g_mvUAV, (IUnknown**)&g_depthUAV, (IUnknown**)&g_mvTex, (IUnknown**)&g_depthTex })
        if (*p) { (*p)->Release(); *p = nullptr; }
}

bool ensureTargets(UINT w, UINT h) {
    if (g_mvTex && g_texW == w && g_texH == h) return true;
    releaseTargets();
    // shared with D3D12 when frame generation reads them there
    const UINT misc = p12Enabled() ? D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0;
    D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, DXGI_FORMAT_R16G16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, misc };
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_mvTex)) ||
        FAILED(g_dev->CreateUnorderedAccessView(g_mvTex, nullptr, &g_mvUAV))) return false;
    d.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_depthTex)) ||
        FAILED(g_dev->CreateUnorderedAccessView(g_depthTex, nullptr, &g_depthUAV))) return false;
    g_texW = w; g_texH = h;
    return true;
}

// Display-res DLSS output + its render-res copy + the combine target (sizes change only with the game's settings).
bool ensureUpTargets(UINT inW, UINT inH, UINT outW, UINT outH) {
    D3D11_TEXTURE2D_DESC have = {}, haveDown = {};
    if (g_upTex) g_upTex->GetDesc(&have);
    if (g_downTex) g_downTex->GetDesc(&haveDown);
    if (g_combUAV && have.Width == outW && have.Height == outH && haveDown.Width == inW && haveDown.Height == inH) return true;
    for (IUnknown** p : { (IUnknown**)&g_upSRV, (IUnknown**)&g_upUAV, (IUnknown**)&g_upTex, (IUnknown**)&g_downUAV, (IUnknown**)&g_downSRV,
                          (IUnknown**)&g_downTex, (IUnknown**)&g_combUAV, (IUnknown**)&g_combSRV, (IUnknown**)&g_combTex })
        if (*p) { (*p)->Release(); *p = nullptr; }
    const UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    D3D11_TEXTURE2D_DESC d = { outW, outH, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0 };
    bool ok = SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &g_upTex)) && SUCCEEDED(g_dev->CreateShaderResourceView(g_upTex, nullptr, &g_upSRV)) &&
              SUCCEEDED(g_dev->CreateUnorderedAccessView(g_upTex, nullptr, &g_upUAV));
    d.Width = inW; d.Height = inH;
    ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &g_downTex)) && SUCCEEDED(g_dev->CreateShaderResourceView(g_downTex, nullptr, &g_downSRV)) &&
         SUCCEEDED(g_dev->CreateUnorderedAccessView(g_downTex, nullptr, &g_downUAV));
    d.Width = outW; d.Height = outH; d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &g_combTex)) && SUCCEEDED(g_dev->CreateShaderResourceView(g_combTex, nullptr, &g_combSRV)) &&
         SUCCEEDED(g_dev->CreateUnorderedAccessView(g_combTex, nullptr, &g_combUAV));
    logf("dlss: upscale targets %ux%u -> %ux%u: %s", inW, inH, outW, outH, ok ? "ok" : "FAILED");
    return ok;
}

// DLSS's own output while sharpening is on (CAS then writes where DLSS normally would).
bool ensureRaw(UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC have = {};
    if (g_rawTex) g_rawTex->GetDesc(&have);
    if (g_rawTex && have.Width == w && have.Height == h) return true;
    release(g_rawSRV); release(g_rawTex);
    D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
    const bool ok = SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &g_rawTex)) && SUCCEEDED(g_dev->CreateShaderResourceView(g_rawTex, nullptr, &g_rawSRV));
    logf("dlss: sharpen source %ux%u: %s", w, h, ok ? "ok" : "FAILED");
    if (!ok) { release(g_rawSRV); release(g_rawTex); }
    return ok;
}

// UAV on the game's TAA target, for the downsample (cached: it ping-pongs between two textures).
ID3D11UnorderedAccessView* taaUAV(ID3D11Resource* r) {
    for (auto& e : g_taaUAVs) if (e.first == r) return e.second;
    ID3D11UnorderedAccessView* u = nullptr;
    if (FAILED(g_dev->CreateUnorderedAccessView(r, nullptr, &u))) return nullptr;
    if (g_taaUAVs.size() >= 4) {  // two ping-pong targets per resolution: the oldest belong to an earlier one
        g_taaUAVs.front().second->Release(); g_taaUAVs.front().first->Release();
        g_taaUAVs.erase(g_taaUAVs.begin());
    }
    r->AddRef();  // keeps the address unique while cached
    g_taaUAVs.push_back({ r, u });
    return u;
}

void logNgxModules() {
    HMODULE mods[1024]; DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE); ++i) {
        wchar_t fn[MAX_PATH]; GetModuleFileNameW(mods[i], fn, MAX_PATH);
        if (wcsstr(fn, L"ngx") || wcsstr(fn, L"NGX")) {
            DWORD h = 0, sz = GetFileVersionInfoSizeW(fn, &h);
            std::vector<BYTE> v(sz ? sz : 1); VS_FIXEDFILEINFO* fi = nullptr; UINT fl = 0;
            if (sz && GetFileVersionInfoW(fn, 0, sz, v.data()) && VerQueryValueW(v.data(), L"\\", (void**)&fi, &fl)) {
                if (const wchar_t* b = wcsrchr(fn, L'\\'); b && !_wcsicmp(b + 1, L"nvngx_dlss.dll"))
                    snprintf(g_version, sizeof g_version, "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
                logf("dlss: module %S v%u.%u.%u.%u", fn, HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
            } else logf("dlss: module %S", fn);
        }
    }
}

bool ensureFeature(ID3D11DeviceContext* ctx, UINT inW, UINT inH, UINT outW, UINT outH) {
    const bool recreate = g_recreate.exchange(false);
    if (g_feature && !recreate && g_fIn[0] == inW && g_fIn[1] == inH && g_fOut[0] == outW && g_fOut[1] == outH) return true;
    if (g_feature) { NVSDK_NGX_D3D11_ReleaseFeature(g_feature); g_feature = nullptr; }
    if (g_preset < 0) g_preset = iniInt(L"preset", 0);
    const int preset = g_preset;  // 0 = SDK default (K for DLAA/Quality); 11 = K, 12 = L, 13 = M
    for (const char* k : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality })
        g_params->Set(k, (unsigned)preset);
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = inW; cp.Feature.InHeight = inH;
    cp.Feature.InTargetWidth = outW; cp.Feature.InTargetHeight = outH;
    const float ratio = float(inW) / float(outW);  // game's RenderingResolutionRatio: 1.0 DLAA, .67 Q, .58 B, .5 P, .33 UP
    cp.Feature.InPerfQualityValue = ratio > 0.99f ? NVSDK_NGX_PerfQuality_Value_DLAA : ratio > 0.62f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                  : ratio > 0.54f ? NVSDK_NGX_PerfQuality_Value_Balanced : ratio > 0.42f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                  : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_feature, g_params, &cp);
    logf("dlss: create %ux%u -> %ux%u preset %d: %08x", inW, inH, outW, outH, preset, r);
    logNgxModules();
    flushLog();
    if (NVSDK_NGX_FAILED(r)) { g_feature = nullptr; return false; }
    g_fIn[0] = inW; g_fIn[1] = inH; g_fOut[0] = outW; g_fOut[1] = outH;
    g_reset = true;
    return true;
}

ID3D11Resource* viewRes(ID3D11View* v) {
    ID3D11Resource* r = nullptr;
    if (v) v->GetResource(&r);
    return r;  // caller releases
}

void texSize(ID3D11Resource* r, UINT* w, UINT* h) {
    D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
    *w = d.Width; *h = d.Height;
}

// ---- screenshot: R11G11B10 HDR -> crude tonemapped PNG crop, for A/B checks --------
float f11(unsigned v, int mbits) {
    unsigned e = (v >> mbits) & 31, m = v & ((1u << mbits) - 1);
    if (e == 31) return 65000.f;
    float mf = float(m) / float(1u << mbits);
    return e == 0 ? mf * std::ldexp(1.f, -14) : (1.f + mf) * std::ldexp(1.f, int(e) - 15);
}

float pq(float e) {  // SMPTE ST 2084 EOTF -> nits
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float p = std::pow(e, 1.f / m2);
    return 10000.f * std::pow(std::max(p - c1, 0.f) / (c2 - c3 * p), 1.f / m1);
}

// x0/y0 = crop origin (-1 = centre), step = pixel stride (1 = 1:1 crop, 3 = downscaled full 4K frame)
void writeShot(ID3D11DeviceContext* ctx, ID3D11Resource* res, const char* tag, int x0 = -1, int y0 = -1, UINT step = 1) {
    D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)res)->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R11G11B10_FLOAT && d.Format != DXGI_FORMAT_R10G10B10A2_UNORM) { logf("shot: unexpected format %u", d.Format); return; }
    if (!g_dev) { ctx->GetDevice(&g_dev); g_dev->Release(); }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st))) return;
    ctx->CopyResource(st, res);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return; }
    const UINT cw = std::min(1280u, d.Width / step), ch = std::min(720u, d.Height / step);
    if (x0 < 0) x0 = int(d.Width - cw * step) / 2;
    if (y0 < 0) y0 = int(d.Height - ch * step) / 2;
    x0 = std::min(x0, int(d.Width - cw * step)); y0 = std::min(y0, int(d.Height - ch * step));
    std::vector<float> lin(cw * ch * 3);
    double logSum = 0;
    for (UINT y = 0; y < ch; ++y) {
        const unsigned* row = (const unsigned*)((const char*)m.pData + (y0 + y * step) * m.RowPitch);
        for (UINT x = 0; x < cw; ++x) {
            unsigned v = row[x0 + x * step];
            float* o = &lin[(y * cw + x) * 3];
            if (d.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
                o[0] = f11(v & 0x7ff, 6); o[1] = f11((v >> 11) & 0x7ff, 6); o[2] = f11(v >> 22, 5);
            } else {
                o[0] = pq((v & 0x3ff) / 1023.f); o[1] = pq(((v >> 10) & 0x3ff) / 1023.f); o[2] = pq(((v >> 20) & 0x3ff) / 1023.f);
            }
            logSum += std::log(1e-4 + 0.2126 * o[0] + 0.7152 * o[1] + 0.0722 * o[2]);
        }
    }
    ctx->Unmap(st, 0); st->Release();
    float expo = 0.18f / float(std::exp(logSum / (cw * ch)));
    std::vector<BYTE> px(cw * ch * 3);
    for (size_t i = 0; i < lin.size(); ++i) {
        float c = lin[i] * expo; c = c / (1.f + c);
        px[i] = BYTE(255.f * std::pow(c, 1.f / 2.2f) + 0.5f);
    }
    // WIC wants BGR
    for (size_t i = 0; i < px.size(); i += 3) std::swap(px[i], px[i + 2]);
    IWICImagingFactory* wic = nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return;
    wchar_t fn[MAX_PATH]; swprintf(fn, MAX_PATH, L"ffxv_dlss_shot_%S.png", tag);
    IWICStream* s = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    wic->CreateStream(&s);
    if (s && SUCCEEDED(s->InitializeFromFilename(path(fn).c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr))) {
        fr->Initialize(nullptr);
        fr->SetSize(cw, ch);
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        fr->SetPixelFormat(&pf);
        fr->WritePixels(ch, cw * 3, (UINT)px.size(), px.data());
        fr->Commit(); enc->Commit();
        logf("shot: wrote %S (%ux%u at %d,%d step %u of %ux%u)", fn, cw, ch, x0, y0, step, d.Width, d.Height);
    }
    if (fr) fr->Release(); if (enc) enc->Release(); if (s) s->Release(); wic->Release();
}

// Only run DLSS on the configuration seen in gameplay; anything else (loading screens,
// menus, resolution changes mid-flight) falls back to the game's TAA.
bool inputsValid(ID3D11ShaderResourceView* const* srv, ID3D11Resource* color, ID3D11Resource* out, const TaaCB& taa) {
    char why[256] = {};
    D3D11_TEXTURE2D_DESC c, o, dd, vd;
    ((ID3D11Texture2D*)color)->GetDesc(&c);
    ((ID3D11Texture2D*)out)->GetDesc(&o);
    D3D11_SHADER_RESOURCE_VIEW_DESC dv, vv;
    srv[3]->GetDesc(&dv); srv[6]->GetDesc(&vv);
    ID3D11Resource *dr = viewRes(srv[3]), *vr = viewRes(srv[6]);
    ((ID3D11Texture2D*)dr)->GetDesc(&dd); ((ID3D11Texture2D*)vr)->GetDesc(&vd);
    dr->Release(); vr->Release();
    if (c.Format != DXGI_FORMAT_R11G11B10_FLOAT || !(c.BindFlags & D3D11_BIND_SHADER_RESOURCE) || c.SampleDesc.Count != 1)
        snprintf(why, sizeof why, "color fmt %u bind %x samples %u", c.Format, c.BindFlags, c.SampleDesc.Count);
    else if (o.Format != DXGI_FORMAT_R11G11B10_FLOAT || !(o.BindFlags & D3D11_BIND_UNORDERED_ACCESS) || o.SampleDesc.Count != 1)
        snprintf(why, sizeof why, "output fmt %u bind %x samples %u", o.Format, o.BindFlags, o.SampleDesc.Count);
    else if (o.Width < c.Width || o.Height < c.Height)
        snprintf(why, sizeof why, "output %ux%u smaller than input %ux%u", o.Width, o.Height, c.Width, c.Height);
    else if (dv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
             (dv.Format != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS && dv.Format != DXGI_FORMAT_R32_FLOAT) ||
             dd.Width != c.Width || dd.Height != c.Height)
        snprintf(why, sizeof why, "depth view fmt %u dim %u size %ux%u", dv.Format, dv.ViewDimension, dd.Width, dd.Height);
    else if (vv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || vv.Format != DXGI_FORMAT_R16G16_FLOAT ||
             vd.Width != c.Width || vd.Height != c.Height)
        snprintf(why, sizeof why, "velocity view fmt %u dim %u size %ux%u", vv.Format, vv.ViewDimension, vd.Width, vd.Height);
    else if ((UINT)taa.screenSize[0] != c.Width || (UINT)taa.screenSize[1] != c.Height)
        snprintf(why, sizeof why, "cb screen %gx%g vs color %ux%u", taa.screenSize[0], taa.screenSize[1], c.Width, c.Height);
    static std::string lastWhy = "?";
    if (lastWhy != why) {
        lastWhy = why;
        if (*why) logf("dlss: skipping frame: %s", why);
        else logf("dlss: inputs OK: color %ux%u -> output %ux%u", c.Width, c.Height, o.Width, o.Height);
        flushLog();
    }
    return !*why;
}

// Compute-stage state our work touches (NGX's D3D11 path also runs as compute).
struct CsState {
    ID3D11ComputeShader* cs = nullptr;
    ID3D11Buffer* cb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    ID3D11ShaderResourceView* srv[32] = {};
    ID3D11UnorderedAccessView* uav[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};  // 8: valid at feature level 11_0
    ID3D11SamplerState* smp[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};  // a CS UAV on the game's target unbinds it here
    ID3D11DepthStencilView* dsv = nullptr;
    void save(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(_countof(rtv), rtv, &dsv);
        c->CSGetShader(&cs, nullptr, nullptr);
        c->CSGetConstantBuffers(0, _countof(cb), cb);
        c->CSGetShaderResources(0, _countof(srv), srv);
        c->CSGetUnorderedAccessViews(0, _countof(uav), uav);
        c->CSGetSamplers(0, _countof(smp), smp);
    }
    void restore(ID3D11DeviceContext* c) {
        c->CSSetShader(cs, nullptr, 0);
        c->CSSetConstantBuffers(0, _countof(cb), cb);
        c->CSSetShaderResources(0, _countof(srv), srv);
        c->CSSetUnorderedAccessViews(0, _countof(uav), uav, nullptr);
        c->CSSetSamplers(0, _countof(smp), smp);
        c->OMSetRenderTargets(_countof(rtv), rtv, dsv);
        if (cs) cs->Release();
        if (dsv) dsv->Release();
        for (auto* x : rtv) if (x) x->Release();
        for (auto* x : cb) if (x) x->Release();
        for (auto* x : srv) if (x) x->Release();
        for (auto* x : uav) if (x) x->Release();
        for (auto* x : smp) if (x) x->Release();
    }
};

}  // namespace

bool dlssEnabled() { return g_enabled; }
bool dlssTakeFrame(FgFrame* out) { if (!g_fgReady) return false; *out = g_fg; g_fgReady = false; return true; }
void dlssSetPreset(int preset) { g_preset = preset; g_recreate = true; logf("dlss: preset %d (recreating)", preset); }
DlssInfo dlssInfo() {
    if (g_preset < 0) g_preset = iniInt(L"preset", 0);
    if (g_sharp < 0) g_sharp = iniInt(L"sharpness", 0);
    if (g_upscaler == -2) g_upscaler = iniInt(L"upscaler", -1);
    DlssInfo i = { g_fIn[0], g_fIn[1], g_fOut[0], g_fOut[1], g_dispW, g_dispH, g_preset, g_sharp };
    memcpy(i.version, g_version, sizeof i.version);
    i.upscaler = g_upscaler; i.active = g_active;
    snprintf(i.fsrVersion, sizeof i.fsrVersion, "%s", fsrUpscalerVersion());
    snprintf(i.xessVersion, sizeof i.xessVersion, "%s", xessVersion());
    return i;
}
void dlssSetUpscaler(int u) { g_upscaler = u; g_failed = false; g_reset = true; logf("upscaler setting %d", u); }
void dlssSetSharpness(int percent) { g_sharp = percent; }
void dlssToggle() { g_enabled = !g_enabled; g_reset = true; logf("dlss: %s", g_enabled ? "ON" : "OFF (game TAA)"); }
void dlssRequestShot(const char* tag) { g_shotTag = tag; g_shot = true; }

bool dlssReplaceTaa(ID3D11DeviceContext* ctx) {
    g_upFrame = false;
    if (!g_enabled || g_failed) return false;
    if (!g_ready) {
        g_ready = init(ctx);
        if (!g_ready) { g_failed = true; logf("dlss: disabled, falling back to game TAA"); return false; }
    }
    const int which = pickUpscaler();
    if (which != g_active) {
        g_active = which; g_reset = true;
        logf("upscaler: %s", which == 0 ? "DLSS" : which == 1 ? "FSR" : which == 2 ? "XeSS" : "none available, game TAA");
    }
    if (which < 0) return false;
    ID3D11ShaderResourceView* srv[8] = {};
    ctx->PSGetShaderResources(0, 8, srv);
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    ID3D11Buffer* cb = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &cb);
    TaaCB taa;
    bool ok = srv[0] && srv[3] && srv[6] && rtv && cb && cbShadow(cb, &taa);
    static unsigned misses, hits;
    if (ok) ++hits; else if (++misses % 256 == 1) logf("dlss: TAA inputs not ready (%u misses, %u hits)", misses, hits);
    ID3D11Resource *color = ok ? viewRes(srv[0]) : nullptr, *out = ok ? viewRes(rtv) : nullptr;
    UINT inW = 0, inH = 0, outW = 0, outH = 0;
    if (ok) { texSize(color, &inW, &inH); texSize(out, &outW, &outH); }
    ok = ok && inputsValid(srv, color, out, taa) && ensureTargets(inW, inH);

    if (ok) {
        static const bool useSwap = iniInt(L"swap", 0) != 0, runMv = iniInt(L"mv", 1) != 0;
        static const int stage = iniInt(L"stage", 3);
        ID3D11DeviceContext1* ctx1 = nullptr;
        ID3DDeviceContextState* prev = nullptr;
        CsState saved;
        if (useSwap) {
            ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1);
            ctx1->SwapDeviceContextState(g_state, &prev);  // crashes FFXV (GPU fault ~40 frames later); debug only
        } else saved.save(ctx);

        if (runMv) {
            ID3D11ShaderResourceView* in[2] = { srv[3], srv[6] };
            ID3D11UnorderedAccessView* uav[2] = { g_mvUAV, g_depthUAV };
            p12WaitInputsFree(ctx);  // frame generation may still be reading last frame's mv/depth
            ctx->CSSetShader(g_mvCS, nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, &cb);
            ctx->CSSetShaderResources(0, 2, in);
            ctx->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
            static bool mvTraced;
            if (!mvTraced) { logf("dlss: first mv dispatch %ux%u (swap=%d)", inW, inH, useSwap); flushLog(); }
            ctx->Dispatch((inW + 7) / 8, (inH + 7) / 8, 1);
            if (!mvTraced) { mvTraced = true; ctx->Flush(); logf("dlss: mv dispatch submitted"); flushLog(); }
            ID3D11ShaderResourceView* nullSrv[2] = {}; ID3D11UnorderedAccessView* nullUav[2] = {};
            ctx->CSSetShaderResources(0, 2, nullSrv);
            ctx->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
        }

        // Below 100% render scale, upscale to the display (the TAA target is render-res, like the rest of the post chain).
        ID3D11UnorderedAccessView* outUAV = nullptr;
        const bool up = g_downCS && g_dispW > outW && g_dispH > outH && (outUAV = taaUAV(out)) && ensureUpTargets(outW, outH, g_dispW, g_dispH);
        const UINT dlssW = up ? g_dispW : outW, dlssH = up ? g_dispH : outH;
        ok = stage >= 3 && (which != 0 || ensureFeature(ctx, inW, inH, dlssW, dlssH));
        if (g_sharp < 0) g_sharp = iniInt(L"sharpness", 0);
        ID3D11UnorderedAccessView* sharpDst = ok && g_sharp > 0 && g_sharpCS ? (up ? g_upUAV : taaUAV(out)) : nullptr;
        const bool sharpen = sharpDst && ensureRaw(dlssW, dlssH);
        if (ok) {
            float sx = (float)iniInt(L"jitter_sign_x", 1), sy = (float)iniInt(L"jitter_sign_y", 1);
            NVSDK_NGX_D3D11_DLSS_Eval_Params ev = {};
            ev.Feature.pInColor = color;
            ev.Feature.pInOutput = sharpen ? g_rawTex : up ? g_upTex : out;
            ev.pInDepth = g_depthTex;
            ev.pInMotionVectors = g_mvTex;
            ev.InJitterOffsetX = sx * taa.uvJitterOffset[0] * inW;
            ev.InJitterOffsetY = sy * taa.uvJitterOffset[1] * inH;
            ev.InRenderSubrectDimensions = { inW, inH };
            ev.InMVScaleX = (float)inW;
            ev.InMVScaleY = (float)inH;
            ev.InReset = g_reset.exchange(false) ? 1 : 0;
            static int traced;
            if (traced < 2) { logf("dlss: evaluate #%d jitter %.3f,%.3f px reset %d", traced, ev.InJitterOffsetX, ev.InJitterOffsetY, ev.InReset); flushLog(); }
            // DLSS wants >= 8 * (out/in)^2 jitter phases (32 at 2x): log how many the game's sequence has, once.
            static std::vector<std::pair<int, int>> seq;
            if (seq.size() < 128) {
                seq.push_back({ int(std::lround(ev.InJitterOffsetX * 1000)), int(std::lround(ev.InJitterOffsetY * 1000)) });
                if (seq.size() == 128) {
                    std::vector<std::pair<int, int>> u = seq;
                    std::sort(u.begin(), u.end()); u.erase(std::unique(u.begin(), u.end()), u.end());
                    size_t period = 0;
                    for (size_t p = 1; p < 64 && !period; ++p)
                        if (std::equal(seq.begin() + p, seq.end(), seq.begin())) period = p;
                    logf("dlss: jitter over 128 frames: %zu distinct, period %zu (0 = none < 64), render %ux%u -> %ux%u",
                         u.size(), period, inW, inH, outW, outH);
                }
            }
            const float jitter[2] = { ev.InJitterOffsetX, ev.InJitterOffsetY };
            if (which != 0) {
                ok = (which == 1 ? fsrUpscale : xessUpscale)((ID3D11Texture2D*)color, g_depthTex, g_mvTex, (ID3D11Texture2D*)ev.Feature.pInOutput,
                                                             inW, inH, dlssW, dlssH, jitter, ev.InReset != 0);
                if (!ok) logf("dlss: %s failed", which == 1 ? "FSR" : "XeSS");
                else if (g_fIn[0] != inW || g_fIn[1] != inH || g_fOut[0] != dlssW || g_fOut[1] != dlssH) {
                    g_recreate = true;  // g_fIn/g_fOut also vouch for the NGX feature's size
                    g_fIn[0] = inW; g_fIn[1] = inH; g_fOut[0] = dlssW; g_fOut[1] = dlssH;
                }
            } else {
                NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g_feature, g_params, &ev);
                if (traced < 2) { logf("dlss: evaluate #%d -> %08x", traced, r); flushLog(); }
                if (NVSDK_NGX_FAILED(r)) { logf("dlss: evaluate failed %08x", r); ok = false; }
            }
            if (traced < 2) ++traced;  // both upscalers: the start-up trace above is two frames, not the whole session
            if (ok) { g_fg = { g_mvTex, g_depthTex, inW, inH, taa, { jitter[0], jitter[1] } }; g_fgReady = true; }
            if (ok && sharpen) {
                const float c[4] = { g_sharp / 100.f };
                ctx->UpdateSubresource(g_sharpCB, 0, nullptr, c, 0, 0);
                ctx->CSSetShader(g_sharpCS, nullptr, 0);
                ctx->CSSetConstantBuffers(0, 1, &g_sharpCB);
                ctx->CSSetShaderResources(0, 1, &g_rawSRV);
                ctx->CSSetUnorderedAccessViews(0, 1, &sharpDst, nullptr);
                ctx->Dispatch((dlssW + 7) / 8, (dlssH + 7) / 8, 1);
                ID3D11ShaderResourceView* nullSrv = nullptr; ID3D11UnorderedAccessView* nullUav = nullptr;
                ctx->CSSetShaderResources(0, 1, &nullSrv);
                ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
            }
            if (ok && up) {  // display-res result back down to render res for the game's post chain
                ID3D11UnorderedAccessView* uav[2] = { outUAV, g_downUAV };
                ctx->CSSetShader(g_downCS, nullptr, 0);
                ctx->CSSetShaderResources(0, 1, &g_upSRV);
                ctx->CSSetSamplers(0, 1, &g_linear);
                ctx->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
                ctx->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
                ID3D11ShaderResourceView* nullSrv = nullptr; ID3D11UnorderedAccessView* nullUav[2] = {};
                ctx->CSSetShaderResources(0, 1, &nullSrv);
                ctx->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
            }
            g_upFrame = ok && up;
        }

        if (useSwap) {
            ctx->ClearState();
            ctx1->SwapDeviceContextState(prev, nullptr);
            prev->Release(); ctx1->Release();
        } else saved.restore(ctx);
        if (!ok && stage >= 3) { g_failed = true; logf("dlss: disabled after error, falling back to game TAA"); }
    }

    if (color) color->Release();
    if (out) out->Release();
    if (cb) cb->Release();
    if (rtv) rtv->Release();
    for (auto* s : srv) if (s) s->Release();
    return ok;
}

void dlssAfterTaa(ID3D11DeviceContext* ctx) {
    if (!g_shot.exchange(false)) return;
    if (!g_dev) { ctx->GetDevice(&g_dev); g_dev->Release(); }
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (ID3D11Resource* r = viewRes(rtv)) {
        std::string tag = g_shotTag + (g_enabled && !g_failed ? "_dlss" : "_taa");
        writeShot(ctx, r, tag.c_str());
        r->Release();
    }
    if (rtv) rtv->Release();
}

void dlssSetDisplaySize(UINT w, UINT h) { g_dispW = w; g_dispH = h; }

// Tonemap (ps 4d90b775, resolution-independent: one texture, sampled by uv) at display res: combine the game's
// post-processed render-res scene with the DLSS detail, then let the game's own shader tonemap that into a
// display-res twin of its target. Returns false (game draws as usual) unless this frame was upscaled.
bool dlssTonemapBegin(ID3D11DeviceContext* ctx) {
    if (!g_upFrame) return false;
    ID3D11ShaderResourceView* post = nullptr; ctx->PSGetShaderResources(0, 1, &post);
    ID3D11RenderTargetView* rtv = nullptr; ctx->OMGetRenderTargets(1, &rtv, nullptr);
    ID3D11Resource* tgt = viewRes(rtv);
    if (rtv) rtv->Release();
    D3D11_TEXTURE2D_DESC td = {};
    if (tgt) ((ID3D11Texture2D*)tgt)->GetDesc(&td);
    D3D11_TEXTURE2D_DESC have = {};
    if (g_tmTex) g_tmTex->GetDesc(&have);
    if (tgt && (!g_tmTex || have.Format != td.Format || have.Width != g_dispW || have.Height != g_dispH)) {
        release(g_tmRTV); release(g_tmSRV); release(g_tmTex);
        D3D11_TEXTURE2D_DESC d = { g_dispW, g_dispH, 1, 1, td.Format, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, &g_tmTex);
        if (SUCCEEDED(hr)) hr = g_dev->CreateRenderTargetView(g_tmTex, nullptr, &g_tmRTV);
        if (SUCCEEDED(hr)) hr = g_dev->CreateShaderResourceView(g_tmTex, nullptr, &g_tmSRV);
        logf("dlss: tonemap twin %ux%u fmt %u (game's %ux%u): %08x", g_dispW, g_dispH, td.Format, td.Width, td.Height, hr);
        if (FAILED(hr)) { release(g_tmRTV); release(g_tmSRV); release(g_tmTex); }
    }
    const bool ok = post && tgt && g_tmRTV && td.Width < g_dispW;
    g_tmSrc = ok ? tgt : nullptr;
    if (tgt) tgt->Release();
    if (!ok) { if (post) post->Release(); g_upFrame = false; return false; }

    CsState saved; saved.save(ctx);
    ID3D11ShaderResourceView* in[3] = { post, g_downSRV, g_upSRV };
    ctx->CSSetShader(g_combineCS, nullptr, 0);
    ctx->CSSetShaderResources(0, 3, in);
    ctx->CSSetSamplers(0, 1, &g_linear);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_combUAV, nullptr);
    ctx->Dispatch((g_dispW + 7) / 8, (g_dispH + 7) / 8, 1);
    ID3D11ShaderResourceView* nullSrv[3] = {}; ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 3, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    saved.restore(ctx);

    ctx->OMGetRenderTargets(8, g_savedRTV, &g_savedDSV);
    g_savedNVP = 16; ctx->RSGetViewports(&g_savedNVP, g_savedVP);
    g_savedT0 = post;  // keeps the ref until End
    D3D11_VIEWPORT vp = { 0, 0, float(g_dispW), float(g_dispH), 0, 1 };
    ctx->OMSetRenderTargets(1, &g_tmRTV, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->PSSetShaderResources(0, 1, &g_combSRV);
    static bool traced;
    if (!traced) { traced = true; logf("dlss: tonemap at %ux%u (DLSS detail over the game's post chain)", g_dispW, g_dispH); }
    return true;
}

void dlssTonemapEnd(ID3D11DeviceContext* ctx) {
    ctx->OMSetRenderTargets(8, g_savedRTV, g_savedDSV);
    ctx->RSSetViewports(g_savedNVP, g_savedVP);
    ctx->PSSetShaderResources(0, 1, &g_savedT0);
    for (auto*& r : g_savedRTV) release(r);
    release(g_savedDSV); release(g_savedT0);
}

// The blit to the display-res UI layer (ps 664e3fc6): read our display-res tonemap instead of the stretched one.
ID3D11ShaderResourceView* dlssBlitSource(ID3D11DeviceContext* ctx) {
    if (!g_upFrame || !g_tmSrc) return nullptr;
    ID3D11ShaderResourceView* t0 = nullptr; ctx->PSGetShaderResources(0, 1, &t0);
    ID3D11Resource* r = viewRes(t0);
    const bool match = r && r == g_tmSrc;
    if (r) r->Release();
    if (t0) t0->Release();
    if (!match) return nullptr;
    g_upFrame = false; g_tmSrc = nullptr;  // used once per frame
    return g_tmSRV;
}

void shotBackbuffer(ID3D11DeviceContext* ctx, ID3D11Resource* bb, const char* tag) {
    std::string t = tag;
    writeShot(ctx, bb, (t + "_full").c_str(), 0, 0, 3);
    writeShot(ctx, bb, (t + "_crop").c_str(), iniInt(L"shot_x", 0), iniInt(L"shot_y", 1440), 1);
}
