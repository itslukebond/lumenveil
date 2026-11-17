// Lumenveil: dxgi.dll proxy and the Direct3D 11 hooks.
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include "forward_list.h"
#include "common.h"
#include "game_sharpen.h"
#include "proton_env.h"
#include "reshade_chain.h"
#include "swapchain_caller.h"
#include <wincodec.h>
#include <algorithm>
#include <vector>

#define DECL_REAL(n) extern "C" void* g_real_##n = nullptr;
FFXV_FORWARDS(DECL_REAL)

static HMODULE g_realDxgi;
static HMODULE g_reshade;  // ReShade, loaded by reshadeLoad() underneath this mod; null when not loaded
static wchar_t g_dir[MAX_PATH];  // game folder, with trailing backslash
static FILE* g_log;
static std::mutex g_logMx;
static unsigned long long g_frame;

void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log);
    if (g_frame < 600) fflush(g_log);  // startup: keep the log complete if the game hangs or is killed
}

void flushLog() { std::lock_guard<std::mutex> lk(g_logMx); if (g_log) fflush(g_log); }

std::wstring path(const wchar_t* name) { return std::wstring(g_dir) + name; }

// Every copy of the game ships Microsoft's d3dcompiler_47.dll in its folder, and loading it by its full path gets that
// copy under Proton too. Proton's own d3dcompiler_47 (vkd3d-shader before 2.0, as on the Steam Deck) cannot build some
// of the mod's shaders ("GetDimensions is not defined on type RWTexture2D<float3>"). System32's copy is the fallback.
pD3DCompile shaderCompiler() {
    static const pD3DCompile fn = [] {
        auto load = [](const std::wstring& file) {
            HMODULE m = LoadLibraryW(file.c_str());
            auto f = m ? (pD3DCompile)GetProcAddress(m, "D3DCompile") : nullptr;
            if (f) logf("shaders: compiler %ls", file.c_str());
            else logf("shaders: compiler %ls %s", file.c_str(), m ? "has no D3DCompile" : "not loaded");
            return f;
        };
        if (auto f = load(path(L"d3dcompiler_47.dll"))) return f;
        wchar_t sys[MAX_PATH];
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        return n && n < MAX_PATH ? load(std::wstring(sys, n) + L"\\d3dcompiler_47.dll") : nullptr;
    }();
    return fn;
}

// ---- shader registry -------------------------------------------------------
static const unsigned long long kTaaPS = 0xf5990d1d5712e767ull;
static void* g_taaPS;
static void* g_maskPS;
static void* g_tonemapPS;  // 4d90: post chain -> tonemapped render-res target (HDR10 or SDR)
static void* g_blitPS;     // 664e: that target -> display-res UI layer (the game's upscale when render < display)
static void* g_compositePS;  // 9875: the final composite, UI layer (scene + HUD) -> back buffer

static void registerShader(void* obj, const void* code, SIZE_T len) {
    // DXBC header bytes 4..19 are its MD5 checksum; the first 8 are a stable id.
    unsigned long long h = 0;
    if (len >= 20) memcpy(&h, (const char*)code + 4, 8);
    if (h == kTaaPS) g_taaPS = obj;
    if (h == 0x2dbb7368a56120b4ull) g_maskPS = obj;
    if (h == 0x4d90b775b1166becull) g_tonemapPS = obj;
    if (h == 0x664e3fc6829510e5ull) g_blitPS = obj;
    if (h == 0x98754b7c5f047a5eull) g_compositePS = obj;
}

static ID3D11DeviceContext* g_imm;
static ID3D11Device* g_device;

// The game's sharpening pass (the pass itself is with the draw hooks, "the game's sharpening pass").
static void* g_gameSharpPS;  // its pixel shader, once recognised
static std::atomic<int> g_gameSharpPct{ 100 };  // the menu's setting: percent of the game's strength
static std::atomic<unsigned> g_gsBinds, g_gsOtherBinds;  // times it was bound on the immediate context / on another
static void gameSharpenCount(ID3D11DeviceContext* ctx, int idx);

// ---- context hooks ----------------------------------------------------------
// Immediate and deferred contexts may have distinct vtables; originals are kept per vtable.
static void* g_ctxVt[2];
static void* g_ctxOrig[2][64];
template <class F> static F orig(void* obj, int idx) {
    return (F)g_ctxOrig[*(void**)obj == g_ctxVt[1] ? 1 : 0][idx];
}

static void noteUiDraw(ID3D11DeviceContext* ctx);
#define CTX_HOOK(idx, name, compute, sig, args, la, lb)                                         \
    static void STDMETHODCALLTYPE hk_##name sig {                                               \
        if (!compute && !g_nativeDrawing) { noteUiDraw(ctx); gameSharpenCount(ctx, idx); }      \
        orig<void(STDMETHODCALLTYPE*) sig>(ctx, idx) args;                                      \
    }
CTX_HOOK(20, DrawIndexedInstanced, false, (ID3D11DeviceContext* ctx, UINT c, UINT i, UINT s, INT b, UINT si), (ctx, c, i, s, b, si), c, i)
CTX_HOOK(21, DrawInstanced, false, (ID3D11DeviceContext* ctx, UINT c, UINT i, UINT s, UINT si), (ctx, c, i, s, si), c, i)
CTX_HOOK(39, DrawIndexedInstancedIndirect, false, (ID3D11DeviceContext* ctx, ID3D11Buffer* b, UINT o), (ctx, b, o), 0, 0)
CTX_HOOK(40, DrawInstancedIndirect, false, (ID3D11DeviceContext* ctx, ID3D11Buffer* b, UINT o), (ctx, b, o), 0, 0)
CTX_HOOK(41, Dispatch, true, (ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z), (ctx, x, y, z), x, y)
CTX_HOOK(42, DispatchIndirect, true, (ID3D11DeviceContext* ctx, ID3D11Buffer* b, UINT o), (ctx, b, o), 0, 0)

// ---- HUD font sharpening ----------------------------------------------------
// HUD glyphs are 1080p-sized bitmaps (1024² BC4 atlas) magnified 2x at 4K, so bilinear edges are
// ~2 px of blur over the 1080p grid. The replacements steepen coverage by its screen-space slope around
// a pivot, giving ~1 px edges at any magnification (unchanged when the text is not magnified). Pivot is
// 0.25 of final alpha (outline: 0.25/1.333): measured 91% of the original stroke weight with 2.6x fewer
// edge pixels; 0.5 thinned glyphs to 78% because the soft original reads heavier in the linear HDR UI.
static std::atomic<bool> g_uiFix{true};  // Sharper HUD ([ui] sharp, Insert menu): every replacement below and the mask twin
// f23a: soft drop shadow (blurred atlas, peaks ~0.6); ef8f: the letters (crisp thin atlas, x1.333 + COLOR1)
static const unsigned long long kFontPS[2] = { 0xf23a393a78379d69ull, 0xef8f94a6372f311eull };
static void* g_fontGame[2];
static ID3D11Resource* g_glyphRes;           // 2x letters (see loadGlyph2x): the game's letter atlas (ref held: address stays unique)
static ID3D11ShaderResourceView* g_glyphSRV;  // ours, 2048² R8 with mips, or null when the file is missing
static ID3D11PixelShader* g_fontOurs[2];
static const char* kFontHlsl[2] = { R"(
Texture2D t0 : register(t0); SamplerState s0 : register(s0);
float cov(float2 uv, float t) {
    float2 sz; t0.GetDimensions(sz.x, sz.y);  // cubic B-spline from 4 bilinear taps: smooth contours, no texel-grid kinks
    float2 p = uv * sz - 0.5, f = frac(p), i = p - f, f2 = f * f, f3 = f2 * f;
    float2 w0 = (1 - 3 * f + 3 * f2 - f3) / 6, w1 = (4 - 6 * f2 + 3 * f3) / 6, w2 = (1 + 3 * f + 3 * f2 - 3 * f3) / 6, w3 = f3 / 6;
    float2 g0 = w0 + w1, g1 = w2 + w3, h0 = (i - 0.5 + w1 / g0) / sz, h1 = (i + 1.5 + w3 / g1) / sz;
    float c = g0.y * (g0.x * t0.Sample(s0, h0).x + g1.x * t0.Sample(s0, float2(h1.x, h0.y)).x)
            + g1.y * (g0.x * t0.Sample(s0, float2(h0.x, h1.y)).x + g1.x * t0.Sample(s0, h1).x);
    float w = abs(ddx_fine(c)) + abs(ddy_fine(c));
    return saturate((c - t) * clamp(1.0 / max(w, 1e-4), 1.0, 4.0) + t);
}
float4 main(float4 pos : SV_POSITION, float4 col : COLOR0, float2 uv : TEXCOORD0) : SV_TARGET { return float4(col.rgb, cov(uv, 0.25) * col.a); }
)", R"(
Texture2D t0 : register(t0); SamplerState s0 : register(s0);
float cov(float2 uv, float t) {
    float2 sz; t0.GetDimensions(sz.x, sz.y);  // cubic B-spline from 4 bilinear taps: smooth contours, no texel-grid kinks
    float2 p = uv * sz - 0.5, f = frac(p), i = p - f, f2 = f * f, f3 = f2 * f;
    float2 w0 = (1 - 3 * f + 3 * f2 - f3) / 6, w1 = (4 - 6 * f2 + 3 * f3) / 6, w2 = (1 + 3 * f + 3 * f2 - 3 * f3) / 6, w3 = f3 / 6;
    float2 g0 = w0 + w1, g1 = w2 + w3, h0 = (i - 0.5 + w1 / g0) / sz, h1 = (i + 1.5 + w3 / g1) / sz;
    float c = g0.y * (g0.x * t0.Sample(s0, h0).x + g1.x * t0.Sample(s0, float2(h1.x, h0.y)).x)
            + g1.y * (g0.x * t0.Sample(s0, float2(h0.x, h1.y)).x + g1.x * t0.Sample(s0, h1).x);
    float w = abs(ddx_fine(c)) + abs(ddy_fine(c));
    return saturate((c - t) * clamp(1.0 / max(w, 1e-4), 1.0, 4.0) + t);
}
float4 main(float4 pos : SV_POSITION, float4 col : COLOR0, float4 add : COLOR1, float2 uv : TEXCOORD0) : SV_TARGET {
    return float4(col.rgb + add.rgb, cov(uv, 0.1875) * col.a * 1.333333 + add.a);
}
)" };

// HUD element (13e7) and HUD mask (2dbb) shaders with Catmull-Rom magnification: their icon sheets, minimap tiles and
// key prompts are 1080p-sized and drawn 2x at 4K, where bilinear is soft. Same maths as the game's otherwise.
// + the render-res -> display stretch (664e): when DLSS doesn't run (title screen, menus) the scene is upscaled with it.
static const unsigned long long kHudPS[3] = { 0x13e70e5e08fb9294ull, 0x2dbb7368a56120b4ull, 0x664e3fc6829510e5ull };
static void* g_hudGame[3];
static ID3D11PixelShader* g_hudOurs[3];
static const char* kCubicHlsl = R"(
Texture2D t0 : register(t0); Texture2D t1 : register(t1); SamplerState s0 : register(s0);
// 9-tap Catmull-Rom from bilinear taps, only while magnifying (minified: the game's mipmapped sample)
float4 catmullRom(float2 uv) {  // no derivatives: usable in flow control
    float2 sz; t0.GetDimensions(sz.x, sz.y);
    float2 p = uv * sz;
    float2 tc = floor(p - 0.5) + 0.5, f = p - tc;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2, a = (tc - 1) / sz, b = (tc + w2 / w12) / sz, c = (tc + 2) / sz;
    float4 r = t0.SampleLevel(s0, float2(a.x, a.y), 0) * w0.x * w0.y + t0.SampleLevel(s0, float2(b.x, a.y), 0) * w12.x * w0.y
             + t0.SampleLevel(s0, float2(c.x, a.y), 0) * w3.x * w0.y + t0.SampleLevel(s0, float2(a.x, b.y), 0) * w0.x * w12.y
             + t0.SampleLevel(s0, float2(b.x, b.y), 0) * w12.x * w12.y + t0.SampleLevel(s0, float2(c.x, b.y), 0) * w3.x * w12.y
             + t0.SampleLevel(s0, float2(a.x, c.y), 0) * w0.x * w3.y + t0.SampleLevel(s0, float2(b.x, c.y), 0) * w12.x * w3.y
             + t0.SampleLevel(s0, float2(c.x, c.y), 0) * w3.x * w3.y;
    return saturate(r);  // Catmull-Rom overshoots at hard edges
}
float4 sampleSharp(float2 uv) {
    float4 base = t0.Sample(s0, uv);
    float2 sz; t0.GetDimensions(sz.x, sz.y);
    float2 d = fwidth(uv * sz);
    return max(d.x, d.y) >= 0.95 ? base : catmullRom(uv);
}
)";
static const char* kHudHlsl[3] = { R"(
float4 main(float4 pos : SV_POSITION, float4 col : COLOR0, float4 uv : TEXCOORD0, float4 add : TEXCOORD1) : SV_TARGET {
    float4 c = sampleSharp(uv.xy);
    c.w *= t1.Sample(s0, uv.zw).x;  // the element's coverage mask (display res via the twin)
    return saturate(c * col * 2 + add);
}
)", R"(
float4 main(float4 pos : SV_POSITION, float4 col : COLOR0, float4 uv : TEXCOORD0, float4 add : TEXCOORD1) : SV_TARGET {
    return saturate(sampleSharp(uv.xy).w * col.w * 2 + add.w).xxxx;
}
)", R"(
// stretch: Catmull-Rom, pushed further from bilinear and clamped to the 4 nearest texels (crisper, no halos). The title
// screen has no anti-aliasing, so on coherent edges (structure tensor) it also averages Catmull-Rom taps up to 2 texels
// along the edge: the stair-steps are what varies along an edge, the edge itself stays sharp.
float luma(int2 p, int2 m) { return dot(t0.Load(int3(clamp(p, 0, m), 0)).rgb, float3(0.299, 0.587, 0.114)); }
float4 main(float4 pos : SV_POSITION, float4 col : COLOR0, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 b = t0.Sample(s0, uv);
    float2 sz; t0.GetDimensions(sz.x, sz.y);
    float2 d = fwidth(uv * sz);
    if (max(d.x, d.y) >= 0.95) return b * col;  // 1:1 (DLSS's display-res twin) or shrinking: the game's sample
    float4 c = catmullRom(uv);
    int2 i = int2(floor(uv * sz - 0.5)), m = int2(sz) - 1;
    float4 a0 = t0.Load(int3(clamp(i, 0, m), 0)), a1 = t0.Load(int3(clamp(i + int2(1, 0), 0, m), 0));
    float4 a2 = t0.Load(int3(clamp(i + int2(0, 1), 0, m), 0)), a3 = t0.Load(int3(clamp(i + 1, 0, m), 0));
    float4 sharp = clamp(c + 0.5 * (c - b), min(min(a0, a1), min(a2, a3)), max(max(a0, a1), max(a2, a3)));
    float jxx = 0, jyy = 0, jxy = 0;
    [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) {
        int2 q = i + int2(x, y);
        float gx = luma(q + int2(1, 0), m) - luma(q - int2(1, 0), m), gy = luma(q + int2(0, 1), m) - luma(q - int2(0, 1), m);
        jxx += gx * gx; jyy += gy * gy; jxy += gx * gy;
    }
    float tr = jxx + jyy, root = sqrt((jxx - jyy) * (jxx - jyy) + 4 * jxy * jxy), l1 = (tr + root) * 0.5, l2 = (tr - root) * 0.5;
    float coh = (l1 - l2) / (l1 + l2 + 1e-6);
    float w = coh * coh * saturate(sqrt(l1) * 4);
    if (w < 0.02) return sharp * col;
    float2 g = abs(jxy) > 1e-6 ? float2(jxy, l1 - jxx) : (jxx >= jyy ? float2(1, 0) : float2(0, 1));  // across the edge
    float2 e = float2(-g.y, g.x) * rsqrt(dot(g, g)) / sz;  // one texel along it, in uv
    float4 s = c;
    [unroll] for (int k = 1; k <= 3; ++k) s += catmullRom(uv + e * (k * 0.6667)) + catmullRom(uv - e * (k * 0.6667));
    return lerp(sharp, s / 7, w) * col;
}
)" };
// The stretch on frames in which the game's TAA drew (gameplay): TAA has already smoothed the edges, so only the
// sharpened Catmull-Rom with its clamp, without the edge-directed smoothing the title screen needs.
static const char* kStretchLightHlsl = R"(
float4 main(float4 pos : SV_POSITION, float4 col : COLOR0, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 b = t0.Sample(s0, uv);
    float2 sz; t0.GetDimensions(sz.x, sz.y);
    float2 d = fwidth(uv * sz);
    if (max(d.x, d.y) >= 0.95) return b * col;
    float4 c = catmullRom(uv);
    int2 i = int2(floor(uv * sz - 0.5)), m = int2(sz) - 1;
    float4 a0 = t0.Load(int3(clamp(i, 0, m), 0)), a1 = t0.Load(int3(clamp(i + int2(1, 0), 0, m), 0));
    float4 a2 = t0.Load(int3(clamp(i + int2(0, 1), 0, m), 0)), a3 = t0.Load(int3(clamp(i + 1, 0, m), 0));
    return clamp(c + 0.5 * (c - b), min(min(a0, a1), min(a2, a3)), max(max(a0, a1), max(a2, a3))) * col;
}
)";
static ID3D11PixelShader* g_stretchLight;  // null: the full stretch wherever it magnifies
static std::atomic<bool> g_taaThisFrame;    // the game's TAA drew since the last present
static void makeHudPS(ID3D11Device* d, HRESULT(STDMETHODCALLTYPE* create)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**),
                      void* game, unsigned long long h) {
    for (int i = 0; i < 3; ++i) {
        if (h != kHudPS[i] || g_hudOurs[i]) continue;
        const pD3DCompile compile = shaderCompiler();
        auto build = [&](const char* body, ID3D11PixelShader** out, const char* what) {
            const std::string src = std::string(kCubicHlsl) + body;
            ID3DBlob *code = nullptr, *err = nullptr;
            HRESULT hr = compile ? compile(src.c_str(), src.size(), "ffxv_hud", nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err) : E_FAIL;
            if (SUCCEEDED(hr)) hr = create(d, code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
            logf("ui: hud ps %016llx %s %08x %s", h, what, hr, err ? (char*)err->GetBufferPointer() : "");
            if (code) code->Release();
            if (err) err->Release();
            return SUCCEEDED(hr);
        };
        if (!build(kHudHlsl[i], &g_hudOurs[i], "cubic replacement")) continue;
        if (i == 2) build(kStretchLightHlsl, &g_stretchLight, "light stretch");
        g_hudGame[i] = game;
    }
}

static void makeFontPS(ID3D11Device* d, HRESULT(STDMETHODCALLTYPE* create)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**),
                       void* game, unsigned long long h) {
    if (h == kFontPS[0]) { g_fontGame[0] = game; return; }  // the soft drop shadow (f23a): left as the game draws it
    for (int i = 1; i < 2; ++i) {
        if (h != kFontPS[i] || g_fontOurs[i]) continue;
        const pD3DCompile compile = shaderCompiler();
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT hr = compile ? compile(kFontHlsl[i], strlen(kFontHlsl[i]), "ffxv_font", nullptr, nullptr, "main", "ps_5_0",
                                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err) : E_FAIL;
        if (SUCCEEDED(hr)) hr = create(d, code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_fontOurs[i]);
        logf("ui: font ps %016llx replacement %08x %s", h, hr, err ? (char*)err->GetBufferPointer() : "");
        if (code) code->Release();
        if (err) err->Release();
        if (SUCCEEDED(hr)) g_fontGame[i] = game;
    }
}

// ---- TAA replacement --------------------------------------------------------
static void* g_immPS;  // currently bound pixel shader on the immediate context

// Mip 0 as tightly packed rows (block rows for BC formats), as in the replacement hashes.
static bool texLayout(const D3D11_TEXTURE2D_DESC& d, UINT* rowBytes, UINT* rows) {
    UINT blockBytes = 0, px = 0;  // block-compressed bytes per 4x4 block, or bytes per pixel
    switch (d.Format) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_UNORM: blockBytes = 8; break;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: blockBytes = 16; break;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM: px = 1; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: px = 4; break;
    default: return false;
    }
    *rowBytes = blockBytes ? (d.Width + 3) / 4 * blockBytes : d.Width * px;
    *rows = blockBytes ? (d.Height + 3) / 4 : d.Height;
    return true;
}
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);

// ---- texture mip bias while DLSS upscales -------------------------------------
// Rendering below display res, the GPU picks mips for the render res, but DLSS outputs display res: world textures need
// log2(render/display) more detail (-1 at 50%), as in native DLSS integrations. Biased clones replace the game's
// anisotropic wrap samplers (world materials); clamp samplers (post, UI) are left alone.
static std::mutex g_smpMx;
static std::unordered_map<ID3D11SamplerState*, ID3D11SamplerState*> g_biased;  // game's (ref held) -> ours, or null = untouched
static float g_smpBias;
static std::atomic<float> g_frameBias;  // this frame's bias, worked out in onPresent: 0 = none
static void STDMETHODCALLTYPE hk_PSSetSamplers(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11SamplerState* const* s) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
    if (g_nativeDrawing) return orig<F>(ctx, 10)(ctx, start, n, s);
    const float bias = g_frameBias.load(std::memory_order_relaxed);
    if (bias == 0.f || !s || n > 16) return orig<F>(ctx, 10)(ctx, start, n, s);
    ID3D11SamplerState* v[16]; bool any = false;
    {
        std::lock_guard<std::mutex> lk(g_smpMx);
        if (bias != g_smpBias) {  // render scale changed: rebuild the clones
            for (auto& e : g_biased) { e.first->Release(); if (e.second) e.second->Release(); }
            g_biased.clear(); g_smpBias = bias;
        }
        for (UINT i = 0; i < n; ++i) {
            v[i] = s[i];
            if (!s[i]) continue;
            auto it = g_biased.find(s[i]);
            if (it == g_biased.end()) {
                D3D11_SAMPLER_DESC d; s[i]->GetDesc(&d);
                const bool world = d.Filter == D3D11_FILTER_ANISOTROPIC && d.AddressU == D3D11_TEXTURE_ADDRESS_WRAP;
                if (g_biased.size() < 48)
                    logf("dlss: sampler %p filter %x addr %u/%u aniso %u bias %.2f -> %s", (void*)s[i], d.Filter, d.AddressU, d.AddressV,
                         d.MaxAnisotropy, d.MipLODBias, world ? "biased" : "kept");
                ID3D11SamplerState* b = nullptr;
                if (world) { d.MipLODBias += bias; g_device->CreateSamplerState(&d, &b); }
                s[i]->AddRef();  // keeps the address unique while mapped
                it = g_biased.emplace(s[i], b).first;
            }
            if (it->second) { v[i] = it->second; v[i]->AddRef(); any = true; }
        }
    }
    orig<F>(ctx, 10)(ctx, start, n, any ? v : s);
    if (any) for (UINT i = 0; i < n; ++i) if (v[i] != s[i]) v[i]->Release();  // the context holds its own while bound
}

static void STDMETHODCALLTYPE hk_PSSetShader(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n) {
    if (g_nativeDrawing) return orig<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT)>(ctx, 9)(ctx, ps, ci, n);
    if (ctx == g_imm) g_immPS = ps;
    if (ps && ps == g_gameSharpPS) ++(ctx == g_imm ? g_gsBinds : g_gsOtherBinds);
    if (g_uiFix && ps)
        for (int i = 0; i < 2; ++i) {
            if (ps == g_fontGame[i] && g_fontOurs[i] && !g_glyphSRV) ps = g_fontOurs[i];  // 2x atlas: game's own shader
        }
    if (g_uiFix && ps)
        for (int i = 0; i < 2; ++i) if (ps == g_hudGame[i] && g_hudOurs[i]) ps = g_hudOurs[i];  // 664e: per draw, stretchPick
    orig<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT)>(ctx, 9)(ctx, ps, ci, n);
}

// ---- HUD-less back buffer for FSR frame generation ---------------------------------
// The blit fills the display-res UI layer with the scene; the HUD is then drawn into it and the final composite puts
// the layer into the back buffer. A copy of the layer taken right after the blit is the scene without the HUD;
// replaying the game's own composite draw with that copy bound, into p12HudlessTarget(), gives the back buffer without
// the HUD, encoded exactly as the real one (SDR, HDR10). The composite is a DrawIndexed. The blit's pixel shader also
// draws into other targets (the HUD mask among them) and draws into the UI layer again later in the frame, so only its
// first draw of the frame into the texture the composite reads at t0 is taken.
static ID3D11Texture2D* g_layerCopy;
static ID3D11ShaderResourceView* g_layerCopySRV;
static ID3D11Resource* g_uiLayer;   // identity only: what the last composite read at t0
static ID3D11Resource* g_layerRes;  // identity only: the UI layer the copy was taken from
static bool g_layerReady;           // copied this frame, not yet replayed
static unsigned long long g_layerFrame = ~0ull;  // g_frame of the last copy

static void hudlessCaptureLayer(ID3D11DeviceContext* ctx) {
    if (!p12WantsHudless() || !g_uiLayer || g_layerFrame == g_frame) return;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* r = nullptr; rtv->GetResource(&r); rtv->Release();
    if (r != g_uiLayer) { r->Release(); return; }
    g_layerReady = false;
    g_layerFrame = g_frame;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    D3D11_TEXTURE2D_DESC d = {}, have = {};
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ((ID3D11Texture2D*)r)->GetDesc(&d);
    if (g_layerCopy) g_layerCopy->GetDesc(&have);
    if (d.Width && (!g_layerCopy || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format)) {
        if (g_layerCopySRV) { g_layerCopySRV->Release(); g_layerCopySRV = nullptr; }
        if (g_layerCopy) { g_layerCopy->Release(); g_layerCopy = nullptr; }
        D3D11_TEXTURE2D_DESC c = { d.Width, d.Height, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        HRESULT hr = g_device->CreateTexture2D(&c, nullptr, &g_layerCopy);
        if (SUCCEEDED(hr)) hr = g_device->CreateShaderResourceView(g_layerCopy, nullptr, &g_layerCopySRV);
        logf("hudless: UI layer copy %ux%u fmt %u: %08x", d.Width, d.Height, d.Format, hr);
        if (FAILED(hr) && g_layerCopy) { g_layerCopy->Release(); g_layerCopy = nullptr; }
    }
    if (d.Width && g_layerCopySRV) {
        ctx->CopyResource(g_layerCopy, r);
        g_layerRes = r;
        g_layerReady = true;
    }
    r->Release();
}

// Right after the game's composite: the same draw again with the HUD-less layer, into this frame's HUD-less target.
// indexed: the composite was a DrawIndexed(c, start, base), else a Draw(c, start). Bound state is the game's, so the
// replay draws with its buffers; binds go through the original calls, past the mod's own view and target swaps.
static void hudlessReplay(ID3D11DeviceContext* ctx, bool indexed, UINT c, UINT start, INT base) {
    if (!p12WantsHudless()) return;
    {
        ID3D11ShaderResourceView* t0 = nullptr;
        ctx->PSGetShaderResources(0, 1, &t0);
        ID3D11Resource* r = nullptr;
        if (t0) { t0->GetResource(&r); t0->Release(); }
        g_uiLayer = r;  // identity only
        if (r) r->Release();
    }
    const bool fresh = g_layerReady && g_layerFrame == g_frame;  // a copy from an earlier frame is never replayed
    g_layerReady = false;
    if (!fresh) return;
    ID3D11ShaderResourceView* srv[8] = {};
    ctx->PSGetShaderResources(0, 8, srv);
    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; ++i)
        if (srv[i]) { ID3D11Resource* r = nullptr; srv[i]->GetResource(&r); if (r == g_layerRes) slot = i; r->Release(); }
    ID3D11RenderTargetView* target = slot >= 0 ? p12HudlessTarget() : nullptr;
    static bool logged;
    if (!logged) { logged = true; logf("hudless: composite reads the UI layer at t%d, target %p", slot, (void*)target); }
    if (target) {
        ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView* dsv = nullptr;
        using PFN_PSSetSRV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
        using PFN_OMSetRT = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
        using PFN_ClearRTV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
        ctx->OMGetRenderTargets(_countof(rtv), rtv, &dsv);
        const float black[4] = {};
        orig<PFN_ClearRTV>(ctx, 50)(ctx, target, black);
        orig<PFN_PSSetSRV>(ctx, 8)(ctx, slot, 1, &g_layerCopySRV);
        orig<PFN_OMSetRT>(ctx, 33)(ctx, 1, &target, nullptr);
        if (indexed) orig<PFN_DrawIndexed>(ctx, 12)(ctx, c, start, base);
        else orig<PFN_Draw>(ctx, 13)(ctx, c, start);
        orig<PFN_PSSetSRV>(ctx, 8)(ctx, slot, 1, &srv[slot]);
        orig<PFN_OMSetRT>(ctx, 33)(ctx, _countof(rtv), rtv, dsv);
        for (auto* x : rtv) if (x) x->Release();
        if (dsv) dsv->Release();
    }
    for (auto* x : srv) if (x) x->Release();
}

// ---- the game's sharpening pass -------------------------------------------------------
// The menu's "Game sharpening" scales the strength the game gives the sharpening pass it draws after anti-aliasing.
// At the pass's draw, our compute shader copies the constants bound for it with the strength multiplied, the pass draws
// with that copy at b0, and the game's buffer is bound again. At 100% the draw is the game's, untouched. Render thread
// only, apart from the setting and the bind counts.
static const char kGsHlsl[] = R"(
cbuffer In : register(b0) { float4 c[4096]; };
cbuffer P : register(b1) { float m; uint n; uint2 unused; };
RWBuffer<float4> Out : register(u0);
RWBuffer<float4> Log : register(u1);
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID) {
    if (i >= n) return;
    float4 v = c[i];
    if (i == 0) Log[0] = c[32];
    if (i == 32) v.x *= m;  // the strength
    Out[i] = v;
}
)";
constexpr UINT kGsStrength = 32;  // the strength is .x of constant 32
static ID3D11ComputeShader* g_gsCS;
static ID3D11Buffer *g_gsParam, *g_gsLog, *g_gsStage, *g_gsOut, *g_gsCB;
static ID3D11UnorderedAccessView *g_gsLogUAV, *g_gsOutUAV;
static UINT g_gsOutBytes;
static bool g_gsBroken;  // our objects could not be made: the pass draws as the game's
static unsigned g_gsDraws[6];  // draws while it is bound on the immediate context, by draw call (kGsDrawNames)
static const char* const kGsDrawNames[6] = { "Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced", "DrawInstancedIndirect", "DrawIndexedInstancedIndirect" };
static struct {
    unsigned long long id, sampleFrame, taaFrame;  // the shader's id; frame of the last sample; of the first TAA draw
    unsigned drawsAtSample, lines, cbBytes, w, h, fmt;
    float rate, strength = -1;  // draws per frame over the last 600 frames; the game's strength last logged
    int setting = -1;
    bool started, pending, notDrawnLogged;
} g_gs;

void gameSharpenSet(int percent) { g_gameSharpPct = gameSharpenPercent(percent, true); }
int gameSharpenGet() { return g_gameSharpPct; }

// At load, when the game creates its sharpening shader: our compute shader and the small buffers. Nothing here costs
// a frame.
static void gameSharpenMake(ID3D11Device* d, void* ps, const void* code, SIZE_T len) {
    if (g_gameSharpPS || crc32(code, len) != kGameSharpenCrc) return;
    unsigned long long h = 0;
    if (len >= 20) memcpy(&h, (const char*)code + 4, 8);
    logf("game sharpening: shader %016llx recognised", h);
    g_gs.id = h;
    const pD3DCompile compile = shaderCompiler();
    ID3DBlob *bc = nullptr, *err = nullptr;
    HRESULT hr = compile ? compile(kGsHlsl, strlen(kGsHlsl), "ffxv_game_sharpen", nullptr, nullptr, "main", "cs_5_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bc, &err) : E_FAIL;
    if (SUCCEEDED(hr)) hr = d->CreateComputeShader(bc->GetBufferPointer(), bc->GetBufferSize(), nullptr, &g_gsCS);
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    if (SUCCEEDED(hr)) hr = d->CreateBuffer(&bd, nullptr, &g_gsParam);
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (SUCCEEDED(hr)) hr = d->CreateBuffer(&bd, nullptr, &g_gsLog);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 1;
    if (SUCCEEDED(hr)) hr = d->CreateUnorderedAccessView(g_gsLog, &ud, &g_gsLogUAV);
    bd = { 16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
    if (SUCCEEDED(hr)) hr = d->CreateBuffer(&bd, nullptr, &g_gsStage);
    if (FAILED(hr)) {
        logf("game sharpening: setup failed %08x %s; the game's own sharpening stays as it is", hr, err ? (const char*)err->GetBufferPointer() : "");
        g_gsBroken = true;
    }
    if (bc) bc->Release();
    if (err) err->Release();
    g_gameSharpPS = ps;
}

// Our copy of the pass's constants, as a UAV for the compute shader and as the constant buffer the pass then reads,
// sized for the constants bound for it (recreated if that changes).
static bool gameSharpenBuffers(UINT bytes) {
    if (bytes == g_gsOutBytes) return true;
    for (ID3D11Buffer** b : { &g_gsOut, &g_gsCB }) if (*b) { (*b)->Release(); *b = nullptr; }
    if (g_gsOutUAV) { g_gsOutUAV->Release(); g_gsOutUAV = nullptr; }
    g_gsOutBytes = 0;
    D3D11_BUFFER_DESC bd = { bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0 };
    HRESULT hr = g_device->CreateBuffer(&bd, nullptr, &g_gsOut);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = bytes / 16;
    if (SUCCEEDED(hr)) hr = g_device->CreateUnorderedAccessView(g_gsOut, &ud, &g_gsOutUAV);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = g_device->CreateBuffer(&bd, nullptr, &g_gsCB);
    if (FAILED(hr)) {
        logf("game sharpening: buffers of %u bytes failed %08x; the game's own sharpening stays as it is", bytes, hr);
        g_gsBroken = true;
        return false;
    }
    g_gsOutBytes = bytes;
    return true;
}

static void gameSharpenCount(ID3D11DeviceContext* ctx, int idx) {
    if (ctx != g_imm || !g_immPS || g_immPS != g_gameSharpPS) return;
    const int k = idx == 12 ? 1 : idx == 21 ? 2 : idx == 20 ? 3 : idx == 40 ? 4 : idx == 39 ? 5 : 0;
    ++g_gsDraws[k];
}

// The strength the game gave the pass, read back a few frames after a sample (never waiting on the GPU), and the log
// line when it or the setting changed: at most 20 a run.
static void gameSharpenRead(ID3D11DeviceContext* ctx, int setting) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_gsStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return;  // not ready: next draw
    const float strength = *(const float*)m.pData;
    ctx->Unmap(g_gsStage, 0);
    g_gs.pending = false;
    if (strength == g_gs.strength && setting == g_gs.setting) return;
    g_gs.strength = strength; g_gs.setting = setting;
    unsigned top = 0;
    for (unsigned i = 0; i < 6; ++i) if (g_gsDraws[i] > g_gsDraws[top]) top = i;
    logf("game sharpening: ps %016llx, game strength %.3f, setting %d%%, %.2f draws/frame (%s), target %ux%u fmt %u, cb %u bytes",
         g_gs.id, strength, setting, g_gs.rate, kGsDrawNames[top], g_gs.w, g_gs.h, g_gs.fmt, g_gs.cbBytes);
    flushLog();
    ++g_gs.lines;
}

// At each draw of the game's TAA: once, 600 frames after the first TAA draw, if the pass has never been drawn through
// Draw on the immediate context, say so and name the call it was drawn with, if any (the setting then has no effect).
static void gameSharpenAfterTaa() {
    if (!g_gs.taaFrame) g_gs.taaFrame = g_frame ? g_frame : 1;
    if (!g_gameSharpPS || g_gs.notDrawnLogged || g_frame < g_gs.taaFrame + 600) return;
    const int other = gameSharpenOtherCall(g_gsDraws);
    if (other >= 0) {
        g_gs.notDrawnLogged = true;
        logf("game sharpening: pass drawn with %s, which the setting does not change (%u draws, binds %u)",
             kGsDrawNames[other], g_gsDraws[other], g_gsBinds.load());
        flushLog();
        return;
    }
    if (g_gsDraws[0]) return;
    g_gs.notDrawnLogged = true;
    logf("game sharpening: pass not drawn on the immediate context (binds %u, deferred %u)", g_gsBinds.load(), g_gsOtherBinds.load());
    flushLog();
}

// The pass's draw (Draw, on the immediate context).
static void gameSharpenDraw(ID3D11DeviceContext* ctx, UINT c, UINT s) {
    ++g_gsDraws[0];
    const int pct = g_gameSharpPct;
    static ID3D11DeviceContext1* ctx1 = [] {
        ID3D11DeviceContext1* x = nullptr;
        g_imm->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&x);
        return x;  // kept for the process, like the context itself
    }();
    if (!g_gs.started) { g_gs.started = true; g_gs.sampleFrame = g_frame; g_gs.drawsAtSample = 1; }  // the first window opens
    if (g_gs.pending) gameSharpenRead(ctx, pct);
    const bool sample = !g_gs.pending && g_gs.lines < 20 && g_frame >= g_gs.sampleFrame + 600;
    ID3D11Buffer* game = nullptr;
    UINT first = 0, num = 0;
    if (ctx1) ctx1->PSGetConstantBuffers1(0, 1, &game, &first, &num);
    else ctx->PSGetConstantBuffers(0, 1, &game);
    D3D11_BUFFER_DESC gd = {};
    if (game) game->GetDesc(&gd);
    UINT n = game && first * 16 < gd.ByteWidth ? gd.ByteWidth / 16 - first : 0;  // constants the pass can see
    if (num && num < n) n = num;
    if (n > 4096) n = 4096;
    const bool ranged = ctx1 && (first || (num && num * 16 < gd.ByteWidth));  // bound with an offset or a size
    if (g_gsBroken || !g_gsCS || n <= kGsStrength || (pct == 100 && !sample) || !gameSharpenBuffers(n * 16)) {
        orig<PFN_Draw>(ctx, 13)(ctx, c, s);  // the game's draw, nothing of ours bound
        if (game) game->Release();
        return;
    }
    // our copy of the constants, on the GPU: compute state saved and restored around it
    ID3D11ComputeShader* cs = nullptr;
    ID3D11Buffer* cb[2] = {};
    UINT cbFirst[2] = {}, cbNum[2] = {};
    ID3D11UnorderedAccessView* uav[2] = {};
    ctx->CSGetShader(&cs, nullptr, nullptr);
    if (ctx1) ctx1->CSGetConstantBuffers1(0, 2, cb, cbFirst, cbNum);
    else ctx->CSGetConstantBuffers(0, 2, cb);
    ctx->CSGetUnorderedAccessViews(0, 2, uav);
    const struct { float m; UINT n; UINT unused[2]; } param = { gameSharpenScale(pct), n, {} };
    ctx->UpdateSubresource(g_gsParam, 0, nullptr, &param, 0, 0);
    ctx->CSSetShader(g_gsCS, nullptr, 0);
    if (ranged) ctx1->CSSetConstantBuffers1(0, 1, &game, &first, &num);
    else ctx->CSSetConstantBuffers(0, 1, &game);
    ctx->CSSetConstantBuffers(1, 1, &g_gsParam);
    ID3D11UnorderedAccessView* ours[2] = { g_gsOutUAV, g_gsLogUAV };
    ctx->CSSetUnorderedAccessViews(0, 2, ours, nullptr);
    ctx->Dispatch((n + 63) / 64, 1, 1);
    ctx->CSSetShader(cs, nullptr, 0);
    if (ctx1) ctx1->CSSetConstantBuffers1(0, 2, cb, cbFirst, cbNum);
    else ctx->CSSetConstantBuffers(0, 2, cb);
    ctx->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
    if (cs) cs->Release();
    for (auto* x : cb) if (x) x->Release();
    for (auto* x : uav) if (x) x->Release();
    if (sample) {
        ctx->CopyResource(g_gsStage, g_gsLog);
        unsigned all = 0;
        for (unsigned d : g_gsDraws) all += d;
        g_gs.rate = float(all - g_gs.drawsAtSample) / float(g_frame - g_gs.sampleFrame);  // at least 600 frames
        g_gs.pending = true; g_gs.sampleFrame = g_frame; g_gs.drawsAtSample = all; g_gs.cbBytes = n * 16;
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) {
            ID3D11Resource* r = nullptr; rtv->GetResource(&r);
            ID3D11Texture2D* t = nullptr;
            if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) {
                D3D11_TEXTURE2D_DESC td; t->GetDesc(&td); g_gs.w = td.Width; g_gs.h = td.Height; g_gs.fmt = td.Format; t->Release();
            }
            if (r) r->Release();
            rtv->Release();
        }
    }
    if (pct == 100) {  // a log sample only: the game's draw, nothing of ours bound
        orig<PFN_Draw>(ctx, 13)(ctx, c, s);
        game->Release();
        return;
    }
    ctx->CopyResource(g_gsCB, g_gsOut);
    ctx->PSSetConstantBuffers(0, 1, &g_gsCB);
    orig<PFN_Draw>(ctx, 13)(ctx, c, s);
    if (ranged) ctx1->PSSetConstantBuffers1(0, 1, &game, &first, &num);  // the game's buffer back, as it was bound
    else ctx->PSSetConstantBuffers(0, 1, &game);
    game->Release();
}

static void setPS(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps) {
    orig<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT)>(ctx, 9)(ctx, ps, nullptr, 0);
}
// The stretch draw gets its shader here, where its source and viewport are known. The game makes 664e twice and uses
// the other object for fills and a glow upsample, so it is never swapped when bound. Not magnifying (render scale 100%,
// or DLSS's display-res output): the game's own shader stays, which is what the replacements return there. Magnifying:
// the light stretch on frames in which the game's TAA drew, the full one otherwise. True: swapped, put the game's back.
static bool stretchPick(ID3D11DeviceContext* ctx) {
    if (!g_uiFix || !g_hudOurs[2]) return false;
    D3D11_TEXTURE2D_DESC d = {};
    ID3D11ShaderResourceView* srv = nullptr; ctx->PSGetShaderResources(0, 1, &srv);
    if (srv) {
        ID3D11Resource* r = nullptr; srv->GetResource(&r); srv->Release();
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ((ID3D11Texture2D*)r)->GetDesc(&d);
        r->Release();
    }
    D3D11_VIEWPORT vp = {}; UINT n = 1; ctx->RSGetViewports(&n, &vp);
    const bool magnify = d.Width && d.Height && (vp.Width > d.Width * 1.05f || vp.Height > d.Height * 1.05f);
    if (!magnify) return false;
    setPS(ctx, g_taaThisFrame && g_stretchLight ? g_stretchLight : g_hudOurs[2]);
    return true;
}

static void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext* ctx, UINT c, UINT s) {
    if (g_nativeDrawing) return orig<PFN_Draw>(ctx, 13)(ctx, c, s);
    if (ctx == g_imm) p12FirstDraw();
    noteUiDraw(ctx);
    if (ctx == g_imm && g_immPS && g_immPS == g_gameSharpPS) {
        gameSharpenDraw(ctx, c, s);
        return;
    }
    if (ctx == g_imm && g_immPS && g_immPS == g_taaPS) {
        g_taaThisFrame = true;
        void* ps = g_immPS;
        gameSharpenAfterTaa();
        if (!dlssReplaceTaa(ctx)) orig<PFN_Draw>(ctx, 13)(ctx, c, s);
        g_immPS = ps;  // our own work may have gone through hk_PSSetShader
        dlssAfterTaa(ctx);
        return;
    }
    if (ctx == g_imm && g_immPS && g_immPS == g_tonemapPS && dlssTonemapBegin(ctx)) {
        orig<PFN_Draw>(ctx, 13)(ctx, c, s);
        dlssTonemapEnd(ctx);
        return;
    }
    if (ctx == g_imm && g_immPS && g_immPS == g_blitPS) {
        if (ID3D11ShaderResourceView* src = dlssBlitSource(ctx)) {
            ID3D11ShaderResourceView* game = nullptr; ctx->PSGetShaderResources(0, 1, &game);
            ctx->PSSetShaderResources(0, 1, &src);
            orig<PFN_Draw>(ctx, 13)(ctx, c, s);  // display res: no stretch
            ctx->PSSetShaderResources(0, 1, &game);
            if (game) game->Release();
        } else {
            const bool ours = stretchPick(ctx);
            orig<PFN_Draw>(ctx, 13)(ctx, c, s);
            if (ours) setPS(ctx, (ID3D11PixelShader*)g_blitPS);
        }
        hudlessCaptureLayer(ctx);  // the scene, before any HUD
        return;
    }
    if (ctx == g_imm && g_immPS && g_immPS == g_compositePS) {
        orig<PFN_Draw>(ctx, 13)(ctx, c, s);
        hudlessReplay(ctx, false, c, s, 0);
        return;
    }
    orig<PFN_Draw>(ctx, 13)(ctx, c, s);
}

// ---- TAA constant-buffer shadow (CPU copy of cbTemporalAA, for the jitter) ---------
// The TAA cbuffer comes from the engine's general pool of 256-byte dynamic cbuffers. On the
// immediate context we hand the game a cached scratch block for those maps and copy it into
// the real (write-combined) mapping at Unmap; reading WC memory directly cost ~4 ms/frame.
// The TAA instance is recognised by content, not pointer.
struct PendingCB { ID3D11Resource* r; void* real; alignas(16) unsigned char scratch[256]; };
static PendingCB g_pend[8];
struct TaaCopy { ID3D11Resource* r; TaaCB cb; };
static TaaCopy g_taaRecent[4]; static unsigned g_taaNext;
static std::mutex g_cbMx;

static bool is256CB(ID3D11Resource* r) {
    D3D11_RESOURCE_DIMENSION d; r->GetType(&d);
    if (d != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC bd; ((ID3D11Buffer*)r)->GetDesc(&bd);
    return bd.ByteWidth == 256 && (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && bd.Usage == D3D11_USAGE_DYNAMIC;
}

static bool looksLikeTaaCB(const TaaCB* c) {
    const float* s = c->screenSize; const float* f = c->frameBits;
    return s[0] >= 64 && s[1] >= 64 && std::fabs(s[0] * s[2] - 1.f) < 1e-3f && std::fabs(s[1] * s[3] - 1.f) < 1e-3f &&
           f[0] + f[1] == 255.f && f[2] + f[3] == 255.f && std::fabs(c->uvJitterOffset[0]) <= s[2] && std::fabs(c->uvJitterOffset[1]) <= s[3];
}

static void noteCB(ID3D11Resource* r, const void* data) {
    const TaaCB* c = (const TaaCB*)data;
    if (!looksLikeTaaCB(c)) return;
    std::lock_guard<std::mutex> lk(g_cbMx);
    TaaCopy& t = g_taaRecent[g_taaNext++ % 4];
    t.r = r; t.cb = *c;
}

bool cbShadow(ID3D11Buffer* b, TaaCB* out) {
    std::lock_guard<std::mutex> lk(g_cbMx);
    for (unsigned i = 0; i < 4; ++i) {
        const TaaCopy& t = g_taaRecent[(g_taaNext - 1 - i) % 4];
        if (t.r == b) { *out = t.cb; return true; }
    }
    return false;
}

static HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext* ctx, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT fl, D3D11_MAPPED_SUBRESOURCE* m) {
    HRESULT hr = orig<HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*)>(ctx, 14)(ctx, r, sub, type, fl, m);
    if (ctx == g_imm && !g_nativeDrawing && SUCCEEDED(hr) && m && type == D3D11_MAP_WRITE_DISCARD && is256CB(r)) {
        std::lock_guard<std::mutex> lk(g_cbMx);
        for (auto& p : g_pend)
            if (!p.r) { p.r = r; p.real = m->pData; m->pData = p.scratch; break; }  // all busy: game writes the real mapping
    }
    return hr;
}
static void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext* ctx, ID3D11Resource* r, UINT sub) {
    if (ctx == g_imm && !g_nativeDrawing) {
        PendingCB* hit = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_cbMx);
            for (auto& p : g_pend) if (p.r == r) { hit = &p; break; }
        }
        if (hit) {
            memcpy(hit->real, hit->scratch, 256);
            noteCB(r, hit->scratch);
            std::lock_guard<std::mutex> lk(g_cbMx);
            hit->r = nullptr;
        }
    }
    orig<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT)>(ctx, 15)(ctx, r, sub);
}
static void STDMETHODCALLTYPE hk_UpdateSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, UINT rp, UINT dp) {
    if (ctx == g_imm && !g_nativeDrawing && src && !box && is256CB(r)) noteCB(r, src);
    orig<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT)>(ctx, 48)(ctx, r, sub, box, src, rp, dp);
}

// ---- HUD mask upscale -----------------------------------------------------------
// The HUD rasterises each element's coverage into a half-res R8 mask, then multiplies the
// 4K draw by it. Swap in a full-res twin of that mask wherever it is bound.
static ID3D11RenderTargetView* g_maskRTV;  // game's (weak)
static ID3D11Resource* g_maskRes;          // game's (weak)
static ID3D11RenderTargetView* g_bigRTV;
static ID3D11ShaderResourceView* g_bigSRV;
static bool g_maskBound;
static D3D11_VIEWPORT g_vp[16]; static UINT g_nvp;
static D3D11_RECT g_sc[16]; static UINT g_nsc;
static std::unordered_map<void*, ID3D11ShaderResourceView*> g_srvSwap;  // game SRV -> ours (mask twin / 2x letters) or null

// ---- 2x HUD letter atlas -------------------------------------------------------
// The letters (ps ef8f) come from a 1024² atlas of 1080p-sized glyphs, magnified 2x at 4K. glyph2x.cpp rebuilds it at
// 2048² from the live atlas with every glyph in the same place (ASCII + Latin-1 drawn from TeX Gyre Adventor, the rest
// upsampled); UVs are normalised, so binding it in place of the game's atlas is enough. Learned from the first ef8f draw.
// Pixels -> texture with a full mip chain generated on the GPU (UI art is also drawn smaller than 1:1).
static HRESULT mipTexture(ID3D11DeviceContext* ctx, const BYTE* px, UINT w, UINT h, DXGI_FORMAT fmt, UINT bpp, ID3D11ShaderResourceView** srv) {
    ID3D11Texture2D* tex = nullptr;
    D3D11_TEXTURE2D_DESC d = { w, h, 0, 1, fmt, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_GENERATE_MIPS };
    HRESULT hr = g_device->CreateTexture2D(&d, nullptr, &tex);
    if (SUCCEEDED(hr)) hr = g_device->CreateShaderResourceView(tex, nullptr, srv);
    if (SUCCEEDED(hr)) { ctx->UpdateSubresource(tex, 0, nullptr, px, w * bpp, 0); ctx->GenerateMips(*srv); }
    if (tex) tex->Release();
    return hr;
}
static HRESULT loadPng(ID3D11DeviceContext* ctx, const std::wstring& file, REFWICPixelFormatGUID wicFmt, DXGI_FORMAT fmt, UINT bpp,
                       ID3D11ShaderResourceView** srv, UINT* w, UINT* h) {
    IWICImagingFactory* wic = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<BYTE> px;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &fr);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&cv);
    if (SUCCEEDED(hr)) hr = cv->Initialize(fr, wicFmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = cv->GetSize(w, h);
    if (SUCCEEDED(hr)) { px.resize(size_t(*w) * *h * bpp); hr = cv->CopyPixels(nullptr, *w * bpp, UINT(px.size()), px.data()); }
    for (IUnknown* p : { (IUnknown*)cv, (IUnknown*)fr, (IUnknown*)dec, (IUnknown*)wic }) if (p) p->Release();
    if (SUCCEEDED(hr)) hr = mipTexture(ctx, px.data(), *w, *h, fmt, bpp, srv);
    return hr;
}
static void loadGlyph2x(ID3D11DeviceContext* ctx) {  // g_glyphRes: 1024² BC4
    std::vector<BYTE> bc4(1024 * 1024 / 2);  // mip 0: 256 rows of 256 8-byte blocks
    D3D11_TEXTURE2D_DESC sd; ((ID3D11Texture2D*)g_glyphRes)->GetDesc(&sd);
    sd.MipLevels = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr; D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = g_device->CreateTexture2D(&sd, nullptr, &st);
    if (SUCCEEDED(hr)) {
        ctx->CopySubresourceRegion(st, 0, 0, 0, 0, g_glyphRes, 0, nullptr);
        if (SUCCEEDED(hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            for (UINT y = 0; y < 256; ++y) memcpy(&bc4[y * 2048], (const BYTE*)m.pData + size_t(y) * m.RowPitch, 2048);
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    unsigned long long h = 1469598103934665603ull;  // same hash as the UI replacements
    for (BYTE b : bc4) h = (h ^ b) * 1099511628211ull;
    if (FAILED(hr) || h != 0x22128d5c747791aeull) {  // glyph_map.h describes this atlas only
        logf("ui: letter atlas hash %016llx: %08x, not the one glyph_map.h describes; letters keep the sharpening shader", h, hr);
        return;
    }
    std::vector<unsigned char> font;
    if (FILE* f = _wfopen(path(L"ffxv_dlss\\fonts\\texgyreadventor-regular.otf").c_str(), L"rb")) {
        fseek(f, 0, SEEK_END); font.resize(ftell(f)); fseek(f, 0, SEEK_SET);
        if (fread(font.data(), 1, font.size(), f) != font.size()) font.clear();
        fclose(f);
    }
    const std::vector<unsigned char> px = buildGlyph2x(bc4.data(), font);
    hr = px.empty() ? E_FAIL : mipTexture(ctx, px.data(), 2048, 2048, DXGI_FORMAT_R8_UNORM, 1, &g_glyphSRV);
    logf("ui: 2x letter atlas 2048x2048: %08x%s", hr, SUCCEEDED(hr) ? "" : " (ffxv_dlss\\fonts\\texgyreadventor-regular.otf missing? letters keep the sharpening shader)");
}

// Optional 2x UI art supplied by the player: ffxv_dlss\ui\<hash>.png, where
// hash = FNV-1a 64 of the game texture's mip 0 rows, but started from 1469598103934665603 (a digit short of FNV's basis;
// kept, since replacement file names depend on it).
// Checked once per texture, only for single-mip immutable 2D textures
// (the UI sheets); a match is bound in place of the game's texture (UVs are normalised, so any size is a drop-in).
static int g_uiRepl = -1;  // ffxv_dlss\ui exists
// The replacement is attached to the game's texture and view as D3D private data, so it is freed with them (loading
// screens etc. would otherwise pile up 2x textures for the whole session).
static const GUID kReplSrv = { 0x7d3c2a51, 0x4b1e, 0x4c8a, { 0x9e, 0x21, 0x5a, 0x60, 0x13, 0xc4, 0x8f, 0x02 } };   // our SRV
static const GUID kReplNone = { 0x7d3c2a51, 0x4b1e, 0x4c8a, { 0x9e, 0x21, 0x5a, 0x60, 0x13, 0xc4, 0x8f, 0x03 } };  // checked: none
static ID3D11ShaderResourceView* attachedRepl(ID3D11DeviceChild* o) {  // weak: o holds the reference
    IUnknown* u = nullptr; UINT sz = sizeof(u);
    if (FAILED(o->GetPrivateData(kReplSrv, &sz, &u)) || !u) return nullptr;
    u->Release();
    return (ID3D11ShaderResourceView*)u;
}
static bool markedNone(ID3D11DeviceChild* o) { UINT sz = 0; return SUCCEEDED(o->GetPrivateData(kReplNone, &sz, nullptr)); }
static void attachRepl(ID3D11DeviceChild* o, ID3D11ShaderResourceView* r) {
    if (r) o->SetPrivateDataInterface(kReplSrv, r);
    else { const BYTE none = 1; o->SetPrivateData(kReplNone, 1, &none); }
}
static ID3D11ShaderResourceView* loadReplacement(ID3D11DeviceContext* ctx, ID3D11Resource* r) {
    ID3D11ShaderResourceView* out = nullptr;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    D3D11_TEXTURE2D_DESC d = {}; UINT rowBytes = 0, rows = 0;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ((ID3D11Texture2D*)r)->GetDesc(&d);
    if (d.MipLevels == 1 && d.ArraySize == 1 && d.Usage == D3D11_USAGE_IMMUTABLE && d.BindFlags == D3D11_BIND_SHADER_RESOURCE &&
        texLayout(d, &rowBytes, &rows)) {
        D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st = nullptr; D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_device->CreateTexture2D(&sd, nullptr, &st))) {
            ctx->CopyResource(st, r);
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                unsigned long long h = 1469598103934665603ull;
                for (UINT y = 0; y < rows; ++y)
                    for (const BYTE *b = (const BYTE*)m.pData + size_t(y) * m.RowPitch, *e = b + rowBytes; b < e; ++b) h = (h ^ *b) * 1099511628211ull;
                ctx->Unmap(st, 0);
                wchar_t name[64]; swprintf_s(name, L"ffxv_dlss\\ui\\%016llx.png", h);
                static int listed;
                if (listed < 64) { ++listed; logf("ui: checked %ux%u fmt %u hash %016llx", d.Width, d.Height, d.Format, h); }
                if (GetFileAttributesW(path(name).c_str()) != INVALID_FILE_ATTRIBUTES) {
                    const bool srgb = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                                      d.Format == DXGI_FORMAT_BC1_UNORM_SRGB || d.Format == DXGI_FORMAT_BC2_UNORM_SRGB ||
                                      d.Format == DXGI_FORMAT_BC3_UNORM_SRGB || d.Format == DXGI_FORMAT_BC7_UNORM_SRGB;
                    UINT w = 0, hh = 0;
                    HRESULT hr = loadPng(ctx, path(name), GUID_WICPixelFormat32bppBGRA, srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM,
                                         4, &out, &w, &hh);
                    logf("ui: %ux%u fmt %u -> %S %ux%u: %08x", d.Width, d.Height, d.Format, name, w, hh, hr);
                }
            }
            st->Release();
        }
    }
    return out;  // caller owns it
}
static ID3D11ShaderResourceView* uiReplacement(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* v) {
    if (!g_uiRepl) return nullptr;
    if (ID3D11ShaderResourceView* a = attachedRepl(v)) return a;
    if (markedNone(v)) return nullptr;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    ID3D11ShaderResourceView* out = attachedRepl(r);
    if (!out && !markedNone(r)) {  // first view of this texture: hash it once
        out = loadReplacement(ctx, r);
        attachRepl(r, out);
        if (out) out->Release();  // r holds it now
    }
    attachRepl(v, out);
    r->Release();
    return out;
}
static void noteGlyphAtlas(ID3D11DeviceContext* ctx) {
    if (g_glyphRes || ctx != g_imm || !g_immPS || g_immPS != g_fontGame[1]) return;
    ID3D11ShaderResourceView* srv = nullptr; ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return;
    srv->GetResource(&g_glyphRes); srv->Release();  // keeps the ref
    D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)g_glyphRes)->GetDesc(&d);
    logf("ui: letter atlas %p %ux%u fmt %u", (void*)g_glyphRes, d.Width, d.Height, d.Format);
    if (d.Width == 1024 && d.Height == 1024 && d.Format == DXGI_FORMAT_BC4_UNORM) loadGlyph2x(ctx);
    g_srvSwap.clear();  // cached "not ours" answers predate the atlas
}

static void noteUiDraw(ID3D11DeviceContext* ctx) {
    if (ctx != g_imm || !g_immPS) return;
    noteGlyphAtlas(ctx);
}

using PFN_RSSetViewports = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using PFN_RSSetScissor = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);

static void applyViewports(ID3D11DeviceContext* ctx) {
    D3D11_VIEWPORT v[16]; memcpy(v, g_vp, sizeof(D3D11_VIEWPORT) * g_nvp);
    D3D11_RECT r[16]; memcpy(r, g_sc, sizeof(D3D11_RECT) * g_nsc);
    if (g_maskBound) {
        for (UINT i = 0; i < g_nvp; ++i) { v[i].TopLeftX *= 2; v[i].TopLeftY *= 2; v[i].Width *= 2; v[i].Height *= 2; }
        for (UINT i = 0; i < g_nsc; ++i) { r[i].left *= 2; r[i].top *= 2; r[i].right *= 2; r[i].bottom *= 2; }
    }
    orig<PFN_RSSetViewports>(ctx, 44)(ctx, g_nvp, v);
    orig<PFN_RSSetScissor>(ctx, 45)(ctx, g_nsc, r);
}

static void STDMETHODCALLTYPE hk_RSSetViewports(ID3D11DeviceContext* ctx, UINT n, const D3D11_VIEWPORT* vp) {
    if (ctx != g_imm || g_nativeDrawing || n > 16) return orig<PFN_RSSetViewports>(ctx, 44)(ctx, n, vp);
    g_nvp = n; if (n) memcpy(g_vp, vp, sizeof(D3D11_VIEWPORT) * n);
    if (!g_maskBound) return orig<PFN_RSSetViewports>(ctx, 44)(ctx, n, vp);
    applyViewports(ctx);
}
static void STDMETHODCALLTYPE hk_RSSetScissorRects(ID3D11DeviceContext* ctx, UINT n, const D3D11_RECT* r) {
    if (ctx != g_imm || g_nativeDrawing || n > 16) return orig<PFN_RSSetScissor>(ctx, 45)(ctx, n, r);
    g_nsc = n; if (n) memcpy(g_sc, r, sizeof(D3D11_RECT) * n);
    if (!g_maskBound) return orig<PFN_RSSetScissor>(ctx, 45)(ctx, n, r);
    applyViewports(ctx);
}

static void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
    if (ctx != g_imm || g_nativeDrawing || !g_maskRTV || n > 8) return orig<F>(ctx, 33)(ctx, n, rtv, dsv);
    ID3D11RenderTargetView* v[8]; bool bound = false;
    for (UINT i = 0; i < n; ++i) {
        v[i] = rtv[i];
        if (g_uiFix && rtv[i] == g_maskRTV) { v[i] = g_bigRTV; bound = true; }
    }
    orig<F>(ctx, 33)(ctx, n, n ? v : rtv, dsv);
    if (bound != g_maskBound) { g_maskBound = bound; applyViewports(ctx); }
}

static void STDMETHODCALLTYPE hk_PSSetShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11ShaderResourceView* const* srv) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
    if (g_nativeDrawing) return orig<F>(ctx, 8)(ctx, start, n, srv);  // the native menu's own views, and the game's put back
    if (g_uiRepl < 0) {
        g_uiRepl = GetFileAttributesW(path(L"ffxv_dlss\\ui").c_str()) != INVALID_FILE_ATTRIBUTES;
        logf("ui: replacement folder ffxv_dlss\\ui %s", g_uiRepl ? "found" : "absent");
    }
    if (ctx != g_imm || !g_uiFix || (!g_maskRes && !g_glyphSRV && !g_uiRepl) || n > 16) return orig<F>(ctx, 8)(ctx, start, n, srv);
    ID3D11ShaderResourceView* v[16]; bool any = false;
    for (UINT i = 0; i < n; ++i) {
        v[i] = srv[i];
        if (!srv[i]) continue;
        auto it = g_srvSwap.find(srv[i]);
        if (it == g_srvSwap.end()) {
            ID3D11Resource* r = nullptr; srv[i]->GetResource(&r);
            it = g_srvSwap.emplace(srv[i], r == g_maskRes ? g_bigSRV : r == g_glyphRes ? g_glyphSRV : nullptr).first;
            if (r) r->Release();
        }
        if (ID3D11ShaderResourceView* s = it->second ? it->second : uiReplacement(ctx, srv[i])) { v[i] = s; any = true; }
    }
    orig<F>(ctx, 8)(ctx, start, n, any ? v : srv);
}

static void STDMETHODCALLTYPE hk_ClearRenderTargetView(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const FLOAT c[4]) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
    if (ctx == g_imm && !g_nativeDrawing && g_uiFix && rtv && rtv == g_maskRTV) rtv = g_bigRTV;
    orig<F>(ctx, 50)(ctx, rtv, c);
}

static void detectMask(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* r = nullptr; rtv->GetResource(&r);
    D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
    D3D11_TEXTURE2D_DESC bd = d; bd.Width *= 2; bd.Height *= 2;
    bd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = 0;
    ID3D11Texture2D* big = nullptr;
    if (d.Format == DXGI_FORMAT_R8_UNORM && SUCCEEDED(g_device->CreateTexture2D(&bd, nullptr, &big))) {
        g_device->CreateRenderTargetView(big, nullptr, &g_bigRTV);
        g_device->CreateShaderResourceView(big, nullptr, &g_bigSRV);
        big->Release();  // views hold it
        g_maskRes = r; g_maskRTV = rtv;
        logf("ui: HUD mask %p %ux%u -> twin %ux%u", (void*)r, d.Width, d.Height, bd.Width, bd.Height);
    }
    r->Release(); rtv->Release();
}

static void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext* ctx, UINT c, UINT s, INT b) {
    if (g_nativeDrawing) return orig<PFN_DrawIndexed>(ctx, 12)(ctx, c, s, b);
    if (ctx == g_imm) p12FirstDraw();
    if (ctx == g_imm && !g_maskRTV && g_maskPS && g_immPS == g_maskPS) detectMask(ctx);
    noteUiDraw(ctx);
    gameSharpenCount(ctx, 12);
    orig<PFN_DrawIndexed>(ctx, 12)(ctx, c, s, b);
    if (ctx == g_imm && g_immPS && g_immPS == g_compositePS) hudlessReplay(ctx, true, c, s, b);
}

bool uiSharp() { return g_uiFix; }
// From the menu, between frames.
void uiSetSharp(bool on) {
    g_uiFix = on;
    logf("ui: sharper HUD %s", on ? "on" : "off");
}

static void STDMETHODCALLTYPE hk_ExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* cl, BOOL r) {
    orig<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL)>(ctx, 58)(ctx, cl, r);
}

// ini [hooks] groups, for bisecting crashes: cb = Map/Unmap/UpdateSubresource, ui = HUD mask redirect, ps = PSSetShader
static bool hookGroupOn(int idx) {
    auto on = [](const wchar_t* k) { return GetPrivateProfileIntW(L"hooks", k, 1, path(L"ffxv_dlss.ini").c_str()) != 0; };
    switch (idx) {
    case 14: case 15: return on(L"cb") && on(L"map");
    case 48: return on(L"cb") && on(L"upd");
    case 8: case 33: case 44: case 45: case 50: return on(L"ui");
    case 9: return on(L"ps");
    default: return true;
    }
}

static void hookVtable(void** vt, int slot, const std::pair<int, void*>* hooks, size_t n) {
    g_ctxVt[slot] = vt;
    for (size_t i = 0; i < n; ++i) {
        int idx = hooks[i].first;
        if (slot == 1 && idx != 14 && idx != 15 && vt[idx] == ((void**)g_ctxVt[0])[idx]) { g_ctxOrig[1][idx] = g_ctxOrig[0][idx]; continue; }
        if (!hookGroupOn(idx)) { g_ctxOrig[slot][idx] = vt[idx]; continue; }
        if ((idx == 14 || idx == 15) && vt[idx] == hooks[i].second) { g_ctxOrig[slot][idx] = g_ctxOrig[0][idx]; continue; }
        if (idx == 14 || idx == 15) {
            // Map/Unmap: swap the vtable entry instead of patching code (inline hooks here crash the game)
            DWORD prot;
            VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &prot);
            g_ctxOrig[slot][idx] = vt[idx];
            vt[idx] = hooks[i].second;
            VirtualProtect(&vt[idx], sizeof(void*), prot, &prot);
            continue;
        }
        MH_STATUS st = MH_CreateHook(vt[idx], hooks[i].second, &g_ctxOrig[slot][idx]);
        if (st == MH_OK) st = MH_EnableHook(vt[idx]);
        if (st != MH_OK) { logf("hook ctx vt%d[%d] %p failed: %s", slot, idx, vt[idx], MH_StatusToString(st)); g_ctxOrig[slot][idx] = vt[idx]; }
    }
}

static const std::pair<int, void*> kCtxHooks[] = {
    {12, (void*)hk_DrawIndexed}, {13, (void*)hk_Draw}, {20, (void*)hk_DrawIndexedInstanced},
    {21, (void*)hk_DrawInstanced}, {39, (void*)hk_DrawIndexedInstancedIndirect},
    {40, (void*)hk_DrawInstancedIndirect}, {41, (void*)hk_Dispatch}, {42, (void*)hk_DispatchIndirect},
    {58, (void*)hk_ExecuteCommandList}, {8, (void*)hk_PSSetShaderResources}, {9, (void*)hk_PSSetShader}, {10, (void*)hk_PSSetSamplers},
    {14, (void*)hk_Map}, {15, (void*)hk_Unmap}, {33, (void*)hk_OMSetRenderTargets}, {44, (void*)hk_RSSetViewports},
    {45, (void*)hk_RSSetScissorRects}, {48, (void*)hk_UpdateSubresource}, {50, (void*)hk_ClearRenderTargetView},
};

// ---- device hooks -----------------------------------------------------------
using PFN_CreateVS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using PFN_CreatePS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using PFN_CreateCS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
static PFN_CreateVS o_CreateVS; static PFN_CreatePS o_CreatePS; static PFN_CreateCS o_CreateCS;

static HRESULT STDMETHODCALLTYPE hk_CreateVS(ID3D11Device* d, const void* c, SIZE_T l, ID3D11ClassLinkage* k, ID3D11VertexShader** o) {
    HRESULT hr = o_CreateVS(d, c, l, k, o);
    if (SUCCEEDED(hr) && o && *o) registerShader(*o, c, l);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreatePS(ID3D11Device* d, const void* c, SIZE_T l, ID3D11ClassLinkage* k, ID3D11PixelShader** o) {
    HRESULT hr = o_CreatePS(d, c, l, k, o);
    if (SUCCEEDED(hr) && o && *o) {
        registerShader(*o, c, l);
        if (l >= 20) { unsigned long long h; memcpy(&h, (const char*)c + 4, 8); makeFontPS(d, o_CreatePS, *o, h); makeHudPS(d, o_CreatePS, *o, h); }
        gameSharpenMake(d, *o, c, l);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreateCS(ID3D11Device* d, const void* c, SIZE_T l, ID3D11ClassLinkage* k, ID3D11ComputeShader** o) {
    HRESULT hr = o_CreateCS(d, c, l, k, o);
    if (SUCCEEDED(hr) && o && *o) registerShader(*o, c, l);
    return hr;
}

static void hookDevice(ID3D11Device* dev, ID3D11DeviceContext* imm) {
    static std::once_flag once;
    std::call_once(once, [&] {
        g_device = dev; g_imm = imm;
        wchar_t gs[16] = {};
        GetPrivateProfileStringW(L"dlss", L"game_sharpening", L"", gs, _countof(gs), path(L"ffxv_dlss.ini").c_str());
        g_gameSharpPct = gameSharpenIni(gs);
        g_uiFix = GetPrivateProfileIntW(L"ui", L"sharp", 1, path(L"ffxv_dlss.ini").c_str()) != 0;
        logf("ui: sharper HUD %s", g_uiFix ? "on" : "off");
        void** dvt = *(void***)dev;
        MH_CreateHook(dvt[12], (void*)hk_CreateVS, (void**)&o_CreateVS);
        MH_CreateHook(dvt[15], (void*)hk_CreatePS, (void**)&o_CreatePS);
        MH_CreateHook(dvt[18], (void*)hk_CreateCS, (void**)&o_CreateCS);
        MH_EnableHook(MH_ALL_HOOKS);
        hookVtable(*(void***)imm, 0, kCtxHooks, _countof(kCtxHooks));
        ID3D11DeviceContext* def = nullptr;
        if (SUCCEEDED(dev->CreateDeferredContext(0, &def))) {
            hookVtable(*(void***)def, 1, kCtxHooks, _countof(kCtxHooks));
            def->Release();
        }
        logf("hooked device %p imm %p (deferred vtable %s)", (void*)dev, (void*)imm,
             g_ctxVt[0] == g_ctxVt[1] ? "shared" : "separate");
    });
}

// ---- swapchain / present ----------------------------------------------------
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PFN_Present o_Present; static PFN_Present1 o_Present1;

static void checkDeviceRemoved();
static void onPresent(IDXGISwapChain* sc) {
    ++g_frame;
    dlssEndFrame();
    g_taaThisFrame = false;
    {
        const DlssInfo di = dlssInfo();  // the sampler hook's bias for the next frame
        g_frameBias.store(dlssEnabled() && di.inW && di.outW > di.inW ? std::log2(float(di.inW) / float(di.outW)) : 0.f,
                          std::memory_order_relaxed);
    }
    if (g_frame % 60 == 1) { DXGI_SWAP_CHAIN_DESC d; if (SUCCEEDED(sc->GetDesc(&d))) dlssSetDisplaySize(d.BufferDesc.Width, d.BufferDesc.Height); }
    if (g_frame % 30 == 0) checkDeviceRemoved();
    static LARGE_INTEGER freq, last, prev; static unsigned long long lastFrame;
    static std::vector<float> ft;  // frame times (ms) in this window, for stutter: p99 and max against the median
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); last = prev = now; }
    ft.push_back(float(1000.0 * (now.QuadPart - prev.QuadPart) / freq.QuadPart));
    prev = now;
    if (now.QuadPart - last.QuadPart >= 2 * freq.QuadPart) {
        double s = double(now.QuadPart - last.QuadPart) / freq.QuadPart;
        std::sort(ft.begin(), ft.end());
        logf("fps %.1f (frame %llu) ms p50 %.2f p99 %.2f max %.2f", (g_frame - lastFrame) / s, g_frame,
             ft[ft.size() / 2], ft[ft.size() * 99 / 100], ft.back());
        ft.clear();
        std::lock_guard<std::mutex> lk(g_logMx); fflush(g_log);
        last = now; lastFrame = g_frame;
    }
    static bool homeDown, homeLogged;
    bool home = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    if (home && !homeDown && g_reshade) {  // Home opens ReShade's overlay: the DLSS toggle steps aside
        if (!homeLogged) { logf("reshade: Home key left to ReShade"); homeLogged = true; }
    } else if (home && !homeDown) dlssToggle();
    homeDown = home;
    if (g_frame % 30 == 0) {
        if (DeleteFileW(path(L"ffxv_dlss.toggle").c_str())) dlssToggle();
        if (DeleteFileW(path(L"ffxv_dlss.shot").c_str())) {
            char tag[32]; snprintf(tag, sizeof tag, "%llu", g_frame);
            dlssRequestShot(tag);
            ID3D11Texture2D* bb = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
                std::string t = std::string(tag) + (g_uiFix ? "_bb_uifix" : "_bb");
                shotBackbuffer(g_imm, bb, t.c_str());
                bb->Release();
            }
        }
    }
}
// Native presentation: the game's own Present with the game's interval, the menu drawn just before it. A Present with
// the tearing flag that DXGI refuses goes again without it, and the flag is not added again.
template <class F> static HRESULT nativePresent(IDXGISwapChain* sc, UINT si, UINT f, F present) {
    onPresent(sc);
    const UINT fl = p12NativeBefore(sc, si, f);
    HRESULT hr = present(fl);
    if (hr == DXGI_ERROR_INVALID_CALL && fl != f) { p12NativeTearingRefused(); hr = present(f); }
    p12NativeAfter(sc, f);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT si, UINT f) {
    if (p12Owns(sc)) return o_Present(sc, si, f);
    if (p12IsNative(sc)) return nativePresent(sc, si, f, [&](UINT fl) { return o_Present(sc, si, fl); });
    onPresent(sc);
    return p12IsGame(sc) ? p12Present(si, f) : o_Present(sc, si, f);
}
static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT si, UINT f, const DXGI_PRESENT_PARAMETERS* p) {
    if (p12Owns(sc)) return o_Present1(sc, si, f, p);
    if (p12IsNative(sc)) return nativePresent(sc, si, f, [&](UINT fl) { return o_Present1(sc, si, fl, p); });
    onPresent(sc);
    return p12IsGame(sc) ? p12Present(si, f) : o_Present1(sc, si, f, p);
}

// With ReShade loaded, the game's swapchain reports the game's own window: the swapchain sits on the hidden window
// (present12.cpp), and ReShade takes the window it reads keys and the mouse from out of GetDesc.
using PFN_GetDesc = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
static PFN_GetDesc o_GetDesc;
static HRESULT STDMETHODCALLTYPE hk_GetDesc(IDXGISwapChain* sc, DXGI_SWAP_CHAIN_DESC* d) {
    HRESULT hr = o_GetDesc(sc, d);
    if (SUCCEEDED(hr) && d && p12IsGame(sc)) d->OutputWindow = p12GameWindow();
    return hr;
}

static void hookSwapChain(IDXGISwapChain* sc) {
    static std::once_flag once;
    std::call_once(once, [&] {
        void** vt = *(void***)sc;
        MH_CreateHook(vt[8], (void*)hk_Present, (void**)&o_Present);
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1))) {
            MH_CreateHook((*(void***)sc1)[22], (void*)hk_Present1, (void**)&o_Present1);
            sc1->Release();
        }
        if (g_reshade) MH_CreateHook(vt[12], (void*)hk_GetDesc, (void**)&o_GetDesc);  // only with ReShade, see hk_GetDesc
        MH_EnableHook(MH_ALL_HOOKS);
        DXGI_SWAP_CHAIN_DESC d; sc->GetDesc(&d);
        logf("hooked swapchain %p %ux%u fmt %u buffers %u swapeffect %u windowed %d", (void*)sc,
             d.BufferDesc.Width, d.BufferDesc.Height, d.BufferDesc.Format, d.BufferCount, d.SwapEffect, d.Windowed);
    });
    ID3D11Device* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) {
        ID3D11DeviceContext* imm = nullptr; dev->GetImmediateContext(&imm);
        hookDevice(dev, imm);
        imm->Release(); dev->Release();
    }
}

using PFN_CreateSC = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
static PFN_CreateSC o_CreateSC; static PFN_CreateSCHwnd o_CreateSCHwnd;
static bool calledByGame(void* ret);
static bool takeAsGame(void* ret, IUnknown* device, HWND w, bool* viaWrapper);
static void noteNotTaken(void* ret);
static void notePresentOff();
// Creation hooks running on this thread: the system DXGI's own nested creation inside ours (DXVK's CreateSwapChain calls
// CreateSwapChainForHwnd) is not a separate swap chain to report.
static thread_local int s_createDepth;
struct CreateDepth { CreateDepth() { ++s_createDepth; } ~CreateDepth() { --s_createDepth; } };

// A DXGI wrapper (OptiScaler, Special K) keeps the game's swap chain and reads its window from it: while our presenter
// has it, GetDesc and GetHwnd report the game's own window, as hk_GetDesc does for ReShade.
using PFN_GetHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, HWND*);
static PFN_GetHwnd o_GetHwnd;
static HRESULT STDMETHODCALLTYPE hk_GetHwnd(IDXGISwapChain1* sc, HWND* w) {
    HRESULT hr = o_GetHwnd(sc, w);
    if (SUCCEEDED(hr) && w && p12IsGame(sc)) *w = p12GameWindow();
    return hr;
}
static void hookWrapperView(IDXGISwapChain* sc) {
    static std::once_flag once;
    std::call_once(once, [&] {
        void** vt = *(void***)sc;
        if (!o_GetDesc) MH_CreateHook(vt[12], (void*)hk_GetDesc, (void**)&o_GetDesc);
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1))) {
            MH_CreateHook((*(void***)sc1)[20], (void*)hk_GetHwnd, (void**)&o_GetHwnd);
            sc1->Release();
        }
        MH_EnableHook(MH_ALL_HOOKS);
    });
}

// Set by a creation hook for the game's swap chain on a native launch, read by createdBy: natively the game's swap chain
// stays on its window and is presented by its own Present (present12.cpp, present_mode.h).
static bool s_nativeCreate, s_nativeTear;
static bool flipModel(DXGI_SWAP_EFFECT e) { return e == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || e == DXGI_SWAP_EFFECT_FLIP_DISCARD; }

// [d3d12] present=1: the game's swapchain goes on a hidden window and ours takes the real one (present12.cpp).
static void createdBy(IDXGISwapChain* sc, bool p12, bool viaWrapper) {
    hookSwapChain(sc);  // our own D3D12 swapchain never gets here: p12Init creates it through the original function
    if (p12) p12Attach(sc);
    if (p12 && viaWrapper) hookWrapperView(sc);
    if (s_nativeCreate) p12NativeAttach(sc, s_nativeTear);
    s_nativeCreate = false;
}
static HRESULT STDMETHODCALLTYPE hk_CreateSC(IDXGIFactory* f, IUnknown* d, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc) {
    if (p12Busy()) return o_CreateSC(f, d, desc, sc);  // Streamline creating its own swapchain inside p12Init
    bool viaWrapper = false;
    bool game = takeAsGame(_ReturnAddress(), d, desc ? desc->OutputWindow : nullptr, &viaWrapper);
    if (!game) noteNotTaken(_ReturnAddress());
    else if (!p12Enabled()) notePresentOff();
    CreateDepth depth;
    HWND hidden = nullptr;
    DXGI_SWAP_CHAIN_DESC dd = desc ? *desc : DXGI_SWAP_CHAIN_DESC{};
    IDXGIFactory2* f2 = nullptr;
    if (game && desc && p12Enabled() && SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&f2)))) {
        DXGI_SWAP_CHAIN_DESC1 d1 = { dd.BufferDesc.Width, dd.BufferDesc.Height, dd.BufferDesc.Format, FALSE, dd.SampleDesc,
                                     dd.BufferUsage, dd.BufferCount, DXGI_SCALING_STRETCH, dd.SwapEffect, DXGI_ALPHA_MODE_UNSPECIFIED, dd.Flags };
        hidden = p12Init(d, f2, o_CreateSCHwnd, dd.OutputWindow, &d1);
        f2->Release();
        if (hidden) dd.OutputWindow = hidden;
    }
    // natively on Windows the game's flip-model swap chain gets the tearing flag; refused: made without it, never again
    s_nativeCreate = game && desc && p12Native();
    s_nativeTear = s_nativeCreate && p12NativeTearing() && flipModel(dd.SwapEffect);
    if (s_nativeTear) dd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    HRESULT hr = o_CreateSC(f, d, desc ? &dd : nullptr, sc);
    if (s_nativeTear && FAILED(hr)) { s_nativeTear = false; p12NativeTearingRefused(); hr = o_CreateSC(f, d, desc, sc); }
    if (hidden && FAILED(hr)) { p12Abort(); hidden = nullptr; hr = o_CreateSC(f, d, desc, sc); }
    if (SUCCEEDED(hr) && sc && *sc) createdBy(*sc, hidden != nullptr, viaWrapper);
    s_nativeCreate = false;
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreateSCHwnd(IDXGIFactory2* f, IUnknown* d, HWND w, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** sc) {
    if (p12Busy()) return o_CreateSCHwnd(f, d, w, desc, fs, o, sc);
    bool viaWrapper = false;
    bool game = takeAsGame(_ReturnAddress(), d, w, &viaWrapper);
    if (!game) noteNotTaken(_ReturnAddress());
    else if (!p12Enabled()) notePresentOff();
    CreateDepth depth;
    HWND hidden = game && desc && p12Enabled() ? p12Init(d, f, o_CreateSCHwnd, w, desc) : nullptr;
    // natively on Windows the game's flip-model swap chain gets the tearing flag; refused: made without it, never again
    DXGI_SWAP_CHAIN_DESC1 nd = desc ? *desc : DXGI_SWAP_CHAIN_DESC1{};
    s_nativeCreate = game && desc && p12Native();
    s_nativeTear = s_nativeCreate && p12NativeTearing() && flipModel(nd.SwapEffect);
    if (s_nativeTear) nd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    HRESULT hr = o_CreateSCHwnd(f, d, hidden ? hidden : w, s_nativeTear ? &nd : desc, fs, o, sc);
    if (s_nativeTear && FAILED(hr)) { s_nativeTear = false; p12NativeTearingRefused(); hr = o_CreateSCHwnd(f, d, w, desc, fs, o, sc); }
    if (hidden && FAILED(hr)) { p12Abort(); hidden = nullptr; hr = o_CreateSCHwnd(f, d, w, desc, fs, o, sc); }
    if (SUCCEEDED(hr) && sc && *sc) createdBy(*sc, hidden != nullptr, viaWrapper);
    s_nativeCreate = false;
    return hr;
}

static void hookFactory(IUnknown* fac) {
    static std::once_flag once;
    std::call_once(once, [&] {
        void** vt = *(void***)fac;
        MH_CreateHook(vt[10], (void*)hk_CreateSC, (void**)&o_CreateSC);
        IDXGIFactory2* f2 = nullptr;
        if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))) {
            MH_CreateHook((*(void***)f2)[15], (void*)hk_CreateSCHwnd, (void**)&o_CreateSCHwnd);
            f2->Release();
        }
        MH_EnableHook(MH_ALL_HOOKS);
        logf("hooked factory");
    });
}

// ---- keep the game's own NGX 1.2 from initialising ---------------------------------
// The NGX core is per-process and takes the first app id it sees; FFXV's id pins every DLSS
// feature to its 2018 DLSS 1 model. The game only needs NGX for DLSS 1, which this mod replaces.
using PFN_GetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
static PFN_GetProcAddress o_GetProcAddress;
static HMODULE g_exe, g_self;

static unsigned long long ngxInitBlocked() { return 0xBAD00001ull; }  // NVSDK_NGX_Result_FAIL_FeatureNotSupported

static bool isModule(HMODULE m, const wchar_t* name) {
    wchar_t fn[MAX_PATH];
    if (!m || !GetModuleFileNameW(m, fn, MAX_PATH)) return false;
    const wchar_t* b = wcsrchr(fn, L'\\');
    return b && _wcsicmp(b + 1, name) == 0;
}

extern ID3D12Device* g_d3d12;

static bool calledByGame(void* ret) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &m);
    // With ReShade loaded, the game's calls reach the real factory from ReShade's wrapper
    return m == g_exe || (g_reshade && m == g_reshade);
}

// What a code address belongs to (swapchain_caller.h); *m gets its module, null when none.
static swapchain_caller::Mod moduleOf(void* addr, HMODULE* m) {
    using swapchain_caller::Mod;
    *m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, m);
    if (*m && *m == g_exe) return Mod::kGame;
    if (*m && *m == g_reshade) return Mod::kReShade;
    if (*m && *m == g_self) return Mod::kOurs;
    wchar_t fn[MAX_PATH], sys[MAX_PATH] = {}, win[MAX_PATH] = {};
    if (!*m || !GetModuleFileNameW(*m, fn, MAX_PATH)) return Mod::kNone;
    GetSystemDirectoryW(sys, MAX_PATH);
    GetWindowsDirectoryW(win, MAX_PATH);
    return swapchain_caller::classify(fn, sys, win, path(L"ffxv_dlss\\"));
}

// The modules of the return chain from the hook's caller upwards: out[0] is ret's. 0 when the captured stack does
// not hold ret.
static int callerChain(void* ret, swapchain_caller::Mod* out, int cap) {
    void* frames[32];
    const int n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    int at = 0;
    while (at < n && frames[at] != ret) ++at;
    int k = 0;
    HMODULE m;
    for (int i = at; i < n && k < cap; ++i) out[k++] = moduleOf(frames[i], &m);
    return k;
}

// Why a swap chain passed on by a wrapper is not the game's (swapchain_caller::refuse), or nullptr.
static const char* wrapperRefusal(IUnknown* device, HWND w) {
    if (!w || !IsWindow(w)) return "no window";
    swapchain_caller::Target t = {};
    ID3D11Device* d11 = nullptr;
    t.d3d11Device = device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d11)));
    if (d11) d11->Release();
    t.topLevel = GetAncestor(w, GA_ROOT) == w;
    t.toolWindow = (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
    RECT r = {};
    GetClientRect(w, &r);
    t.clientW = r.right - r.left; t.clientH = r.bottom - r.top;
    return swapchain_caller::refuse(t);
}

// Whether a swap chain creation is the game's. Straight from the game or ReShade: as before, nothing walked or logged.
// From another module: walk the return chain (swapchain_caller::decide) and check the window; the log names the module.
static bool takeAsGame(void* ret, IUnknown* device, HWND w, bool* viaWrapper) {
    if (calledByGame(ret)) return true;
    HMODULE first = nullptr;
    if (moduleOf(ret, &first) != swapchain_caller::Mod::kOther) return false;  // ours, Streamline, the system: as before
    swapchain_caller::Mod chain[16];
    const int n = callerChain(ret, chain, 16);
    const swapchain_caller::Verdict v = swapchain_caller::decide(chain, n);
    const char* why = v.kind != swapchain_caller::Verdict::kWrapper ? v.reason : wrapperRefusal(device, w);
    wchar_t fn[MAX_PATH] = L"?";
    if (!first || !GetModuleFileNameW(first, fn, MAX_PATH)) wcscpy_s(fn, L"?");
    const wchar_t* name = wcsrchr(fn, L'\\') ? wcsrchr(fn, L'\\') + 1 : fn;
    if (why) {
        static std::atomic<int> logged;
        if (logged.fetch_add(1) < 8) logf("dxgi: swap chain via %ls not taken (%s), passed through", name, why);
        return false;
    }
    static std::atomic<bool> named;
    if (!named.exchange(true)) logf("dxgi: the game's swap chain was passed on by %ls", name);
    *viaWrapper = true;
    return true;
}

// A swap chain creation passed through from a caller takeAsGame does not log: a system module, or code in no module
// (another program's hook, most often), when it is not the system's nested call inside one of ours. Our own and
// Streamline's stay silent. Capped at 8 lines.
static void noteNotTaken(void* ret) {
    if (s_createDepth) return;
    HMODULE m = nullptr;
    const swapchain_caller::Mod k = moduleOf(ret, &m);
    if (k != swapchain_caller::Mod::kSystem && k != swapchain_caller::Mod::kNone) return;
    static std::atomic<int> logged;
    if (logged.fetch_add(1) >= 8) return;
    wchar_t fn[MAX_PATH] = L"?";
    if (!m || !GetModuleFileNameW(m, fn, MAX_PATH)) wcscpy_s(fn, L"?");
    const wchar_t* name = wcsrchr(fn, L'\\') ? wcsrchr(fn, L'\\') + 1 : fn;
    if (k == swapchain_caller::Mod::kSystem) logf("dxgi: swap chain from %ls not taken (called by a system module), passed through", name);
    else logf("dxgi: swap chain from %p not taken (called from code in no module, often another program's overlay), passed through", ret);
}

// [d3d12] present=0: the game's swap chain is taken but our presenter stays off. Said once.
static void notePresentOff() {
    static std::atomic<bool> told;
    if (!told.exchange(true)) logf("d3d12: present=0 in ffxv_dlss.ini, the game keeps its own swap chain: no menu or frame generation");
}

// Called from Present: once the D3D12 device is lost, log why (and DRED's last operations / page fault, if enabled).
static void checkDeviceRemoved() {
    static bool reported;
    if (!g_d3d12 || reported) return;
    HRESULT why = g_d3d12->GetDeviceRemovedReason();
    if (SUCCEEDED(why)) return;
    reported = true;
    logf("d3d12: device removed, reason %08x", why);
    ID3D12DeviceRemovedExtendedData* d = nullptr;
    if (SUCCEEDED(g_d3d12->QueryInterface(IID_PPV_ARGS(&d)))) {
        D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
        if (SUCCEEDED(d->GetPageFaultAllocationOutput(&pf))) {
            logf("d3d12: page fault va %llx", pf.PageFaultVA);
            for (auto* n = pf.pHeadExistingAllocationNode; n; n = n->pNext) logf("   existing alloc %s type %d", n->ObjectNameA ? n->ObjectNameA : "-", n->AllocationType);
            for (auto* n = pf.pHeadRecentFreedAllocationNode; n; n = n->pNext) logf("   freed alloc %s type %d", n->ObjectNameA ? n->ObjectNameA : "-", n->AllocationType);
        }
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc = {};
        if (SUCCEEDED(d->GetAutoBreadcrumbsOutput(&bc)))
            for (auto* n = bc.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
                UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                if (done >= n->BreadcrumbCount) continue;  // list finished; only the in-flight one matters
                logf("   in-flight cmdlist %s: %u/%u ops done", n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "-", done, n->BreadcrumbCount);
                for (UINT i = done > 3 ? done - 3 : 0; i < n->BreadcrumbCount && i < done + 3; ++i) logf("      op %u: %d%s", i, n->pCommandHistory[i], i == done ? "  <- faulting" : "");
            }
        d->Release();
    }
    flushLog();
}

static FARPROC WINAPI hk_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC r = o_GetProcAddress(m, name);
    if (!IS_INTRESOURCE(name) && strncmp(name, "NVSDK_NGX_", 10) == 0 && isModule(m, L"_nvngx.dll")) {
        HMODULE caller = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)_ReturnAddress(), &caller);
        // the game reaches the core through the driver's legacy nvngx.dll shim; ours and Streamline's pass
        bool game = caller == g_exe || isModule(caller, L"nvngx.dll");
        bool block = game && strstr(name, "_Init") &&
                     GetPrivateProfileIntW(L"dlss", L"block_game_ngx", 1, path(L"ffxv_dlss.ini").c_str());
        if (strstr(name, "_Init")) logf("ngx core: %s requested by %s%s", name, game ? "game/shim" : "mod/streamline", block ? " -> blocked" : "");
        if (block) return (FARPROC)ngxInitBlocked;
    }
    return r;
}

// ---- exports ------------------------------------------------------------------
// Logs access violations with the faulting address and return addresses found on the stack.
static LONG CALLBACK onException(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<int> n{0};
    if (n++ > 4) return EXCEPTION_CONTINUE_SEARCH;
    auto modOf = [](DWORD64 a, char* out, size_t len) {
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
            char fn[MAX_PATH]; GetModuleFileNameA(m, fn, MAX_PATH);
            snprintf(out, len, "%s+%llx", strrchr(fn, '\\') + 1, a - (DWORD64)m);
        } else snprintf(out, len, "%llx", a);
    };
    CONTEXT* c = e->ContextRecord;
    char where[MAX_PATH]; modOf(c->Rip, where, sizeof where);
    logf("AV at %s (%s %llx) rax=%llx rcx=%llx rdx=%llx r8=%llx", where,
         e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", e->ExceptionRecord->ExceptionInformation[1],
         c->Rax, c->Rcx, c->Rdx, c->R8);
    DWORD64* sp = (DWORD64*)c->Rsp;
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; ++i) {
        DWORD64 v = sp[i];
        HMODULE m = nullptr;
        if (v > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m)) {
            char w[MAX_PATH]; modOf(v, w, sizeof w); logf("   stack[%d] %s", i, w); ++shown;
        }
    }
    std::lock_guard<std::mutex> lk(g_logMx); fflush(g_log);
    return EXCEPTION_CONTINUE_SEARCH;
}

static std::string g_protonEnvLog;  // written by protonEnv() in DllMain, logged by init(); empty on Windows

static void init() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_log = _wfopen(path(L"ffxv_dlss.log").c_str(), L"w");
        MH_Initialize();
        AddVectoredExceptionHandler(0, onException);  // last in chain: only sees what nothing else handled first
        g_exe = GetModuleHandleW(nullptr);
        MH_CreateHookApi(L"kernel32", "GetProcAddress", (void*)hk_GetProcAddress, (void**)&o_GetProcAddress);
        MH_EnableHook(MH_ALL_HOOKS);
        logf("ffxv-dlss stage2 loaded (D3D12 presentation %s)", p12Enabled() ? "on" : "off");
        if (!g_protonEnvLog.empty()) logf("proton: raw access chains off (%s)", g_protonEnvLog.c_str());
    });
}

template <class F> static F realFn(const char* name) { return (F)GetProcAddress(g_realDxgi, name); }

// ---- ReShade underneath ---------------------------------------------------------
// A player's ReShade dxgi.dll is kept by the installer as dxgi.dll.before_ffxv_dlss and loaded here, at start-up.
// Before it loads, pass-through hooks go on the system's factory and D3D12 device exports. ReShade's own hooks then
// sit on top of them, so these see the real objects: the factory hooks reach the real factory, and present12.cpp
// creates its device through g_createDevice12, which ReShade never wraps, so ReShade sees only the game's swapchain.
using PFN_CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
static PFN_CreateFactory o_bypassFactory, o_bypassFactory1;
static PFN_CreateFactory2 o_bypassFactory2;
static PFN_D3D12_CREATE_DEVICE o_bypassDevice;
PFN_D3D12_CREATE_DEVICE g_createDevice12;  // set while ReShade is loaded (present12.cpp p12Init); null otherwise

static HRESULT WINAPI bypassFactory(REFIID riid, void** out) {
    HRESULT hr = o_bypassFactory(riid, out);
    if (SUCCEEDED(hr) && out && *out) hookFactory((IUnknown*)*out);
    return hr;
}
static HRESULT WINAPI bypassFactory1(REFIID riid, void** out) {
    HRESULT hr = o_bypassFactory1(riid, out);
    if (SUCCEEDED(hr) && out && *out) hookFactory((IUnknown*)*out);
    return hr;
}
static HRESULT WINAPI bypassFactory2(UINT flags, REFIID riid, void** out) {
    HRESULT hr = o_bypassFactory2(flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) hookFactory((IUnknown*)*out);
    return hr;
}
static HRESULT WINAPI bypassDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** out) {
    return o_bypassDevice(adapter, level, riid, out);
}

// ProductName from a file's version information; empty when it has none.
static std::wstring productName(const std::wstring& file) {
    DWORD unused = 0, n = GetFileVersionInfoSizeW(file.c_str(), &unused);
    if (!n) return {};
    std::vector<char> buf(n);
    if (!GetFileVersionInfoW(file.c_str(), 0, n, buf.data())) return {};
    struct Lang { WORD lang, codePage; }* tr = nullptr;
    UINT len = 0;
    wchar_t key[64];
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && tr && len >= sizeof(Lang))
        swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\ProductName", tr->lang, tr->codePage);
    else
        wcscpy_s(key, L"\\StringFileInfo\\040904b0\\ProductName");  // US English, Unicode: what most DLLs carry
    wchar_t* v = nullptr;
    if (!VerQueryValueW(buf.data(), key, (void**)&v, &len) || !v) return {};
    return std::wstring(v, wcsnlen(v, len));
}

static void reshadeLoad() {
    const std::wstring kept = path(L"dxgi.dll.before_ffxv_dlss");
    const bool exists = GetFileAttributesW(kept.c_str()) != INVALID_FILE_ATTRIBUTES;
    const int setting = GetPrivateProfileIntW(L"reshade", L"load", 1, path(L"ffxv_dlss.ini").c_str());
    const auto d = reshade_chain::decide(setting, exists, exists ? productName(kept) : std::wstring());
    if (!d.load) {
        logf("reshade: not loaded (%s)", d.reason);
        return;
    }
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    struct Hook { void* target; void* detour; void** original; };
    const Hook hooks[] = {
        { (void*)GetProcAddress(g_realDxgi, "CreateDXGIFactory"), (void*)bypassFactory, (void**)&o_bypassFactory },
        { (void*)GetProcAddress(g_realDxgi, "CreateDXGIFactory1"), (void*)bypassFactory1, (void**)&o_bypassFactory1 },
        { (void*)GetProcAddress(g_realDxgi, "CreateDXGIFactory2"), (void*)bypassFactory2, (void**)&o_bypassFactory2 },
        { d3d12 ? (void*)GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr, (void*)bypassDevice, (void**)&o_bypassDevice },
    };
    int made = 0;
    for (const Hook& h : hooks) {
        if (!h.target || MH_CreateHook(h.target, h.detour, h.original) != MH_OK) break;
        ++made;
    }
    if (made == 4 && MH_EnableHook(MH_ALL_HOOKS) != MH_OK) made = -1;
    if (made != 4) {
        for (int i = 0; i < (made < 0 ? 4 : made); ++i) { MH_DisableHook(hooks[i].target); MH_RemoveHook(hooks[i].target); }
        logf("reshade: not loaded (the pass-through hooks could not be made)");
        return;
    }
    // ReShade refuses to start under a name it does not expect unless this is set; it is put back straight after, so it
    // never reaches a program the game starts.
    wchar_t had[64];
    const DWORD hadLen = GetEnvironmentVariableW(L"RESHADE_DISABLE_LOADING_CHECK", had, 64);
    SetEnvironmentVariableW(L"RESHADE_DISABLE_LOADING_CHECK", L"1");
    HMODULE mod = LoadLibraryW(kept.c_str());
    const DWORD err = GetLastError();
    SetEnvironmentVariableW(L"RESHADE_DISABLE_LOADING_CHECK", hadLen && hadLen < 64 ? had : nullptr);
    if (!mod) {
        for (const Hook& h : hooks) { MH_DisableHook(h.target); MH_RemoveHook(h.target); }
        logf("reshade: load failed (error %lu)", err);
        return;
    }
    g_reshade = mod;
    g_createDevice12 = o_bypassDevice;
    const char* const* version = (const char* const*)GetProcAddress(mod, "ReShadeVersion");
    logf("reshade: loaded %s from dxgi.dll.before_ffxv_dlss", version && *version ? *version : "(version unknown)");
    if (GetPrivateProfileIntW(L"INSTALL", L"HookStreamline", 0, path(L"ReShade.ini").c_str()))
        logf("reshade: ReShade.ini has HookStreamline=1, so ReShade will not see the game; set it to 0 to use ReShade with Lumenveil");
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** out) {
    init();
    HRESULT hr = realFn<decltype(&CreateDXGIFactory)>("CreateDXGIFactory")(riid, out);
    if (SUCCEEDED(hr) && !g_reshade) hookFactory((IUnknown*)*out);  // with ReShade this is its wrapper: bypassFactory hooked the real one
    return hr;
}
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** out) {
    init();
    HRESULT hr = realFn<decltype(&CreateDXGIFactory1)>("CreateDXGIFactory1")(riid, out);
    if (SUCCEEDED(hr) && !g_reshade) hookFactory((IUnknown*)*out);  // with ReShade this is its wrapper: bypassFactory hooked the real one
    return hr;
}
extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    init();
    HRESULT hr = realFn<decltype(&CreateDXGIFactory2)>("CreateDXGIFactory2")(flags, riid, out);
    if (SUCCEEDED(hr) && !g_reshade) hookFactory((IUnknown*)*out);  // with ReShade this is its wrapper: bypassFactory hooked the real one
    return hr;
}

// Under Proton, turn off NVIDIA's raw access chains in DXVK and vkd3d-proton (see proton_env.h). Runs at the top of
// DllMain, before DXVK's dxgi.dll loads: both layers read these variables later, when they start up. Windows: nothing.
static void protonEnv() {
    if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return;
    auto apply = [](const char* var, proton_env::Outcome (*work)(const std::string&)) {
        char buf[1024];
        DWORD n = GetEnvironmentVariableA(var, buf, sizeof buf);
        proton_env::Outcome o = n < sizeof buf ? work(std::string(buf, n)) : proton_env::Outcome{proton_env::Outcome::kTooLong, {}};
        std::string s = var;
        if (o.kind == proton_env::Outcome::kKept) return s + " kept (player value)";
        if (o.kind == proton_env::Outcome::kTooLong) return s + " skipped (too long)";
        if (!SetEnvironmentVariableA(var, o.value.c_str())) return s + " not set (error " + std::to_string(GetLastError()) + ")";
        return s + "=" + o.value;
    };
    g_protonEnvLog = apply("VKD3D_DISABLE_EXTENSIONS", proton_env::vkd3dDisableList) + ", " + apply("DXVK_CONFIG", proton_env::dxvkConfig);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        protonEnv();
        g_self = inst;
        GetModuleFileNameW(inst, g_dir, MAX_PATH);
        *(wcsrchr(g_dir, L'\\') + 1) = 0;
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); wcscat_s(sys, L"\\dxgi.dll");
        g_realDxgi = LoadLibraryW(sys);
        if (!g_realDxgi) return FALSE;
#define LOAD_REAL(n) g_real_##n = (void*)GetProcAddress(g_realDxgi, #n);
        FFXV_FORWARDS(LOAD_REAL)
        init();
        reshadeLoad();
    }
    return TRUE;
}
