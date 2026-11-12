// Lumenveil: dxgi.dll proxy and the Direct3D 11 hooks.
#include <windows.h>
#include <d3d11.h>
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
#include "proton_env.h"
#include <wincodec.h>
#include <algorithm>
#include <vector>

#define DECL_REAL(n) extern "C" void* g_real_##n = nullptr;
FFXV_FORWARDS(DECL_REAL)

static HMODULE g_realDxgi;
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
        if (!compute) noteUiDraw(ctx);                                                          \
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
static std::atomic<bool> g_uiFix{true};
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
static void makeHudPS(ID3D11Device* d, HRESULT(STDMETHODCALLTYPE* create)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**),
                      void* game, unsigned long long h) {
    for (int i = 0; i < 3; ++i) {
        if (h != kHudPS[i] || g_hudOurs[i]) continue;
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); wcscat_s(sys, L"\\d3dcompiler_47.dll");
        auto compile = (pD3DCompile)GetProcAddress(LoadLibraryW(sys), "D3DCompile");
        const std::string src = std::string(kCubicHlsl) + kHudHlsl[i];
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT hr = compile ? compile(src.c_str(), src.size(), "ffxv_hud", nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err) : E_FAIL;
        if (SUCCEEDED(hr)) hr = create(d, code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_hudOurs[i]);
        logf("ui: hud ps %016llx cubic replacement %08x %s", h, hr, err ? (char*)err->GetBufferPointer() : "");
        if (code) code->Release();
        if (err) err->Release();
        if (SUCCEEDED(hr)) g_hudGame[i] = game;
    }
}

static void makeFontPS(ID3D11Device* d, HRESULT(STDMETHODCALLTYPE* create)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**),
                       void* game, unsigned long long h) {
    if (h == kFontPS[0]) { g_fontGame[0] = game; return; }  // the soft drop shadow (f23a): left as the game draws it
    for (int i = 1; i < 2; ++i) {
        if (h != kFontPS[i] || g_fontOurs[i]) continue;
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); wcscat_s(sys, L"\\d3dcompiler_47.dll");
        auto compile = (pD3DCompile)GetProcAddress(LoadLibraryW(sys), "D3DCompile");
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
static void STDMETHODCALLTYPE hk_PSSetSamplers(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11SamplerState* const* s) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
    const DlssInfo di = dlssInfo();
    const float bias = dlssEnabled() && di.inW && di.outW > di.inW ? std::log2(float(di.inW) / float(di.outW)) : 0.f;
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
    if (ctx == g_imm) g_immPS = ps;
    if (g_uiFix && ps)
        for (int i = 0; i < 2; ++i) {
            if (ps == g_fontGame[i] && g_fontOurs[i] && !g_glyphSRV) ps = g_fontOurs[i];  // 2x atlas: game's own shader
        }
    if (g_uiFix && ps)
        for (int i = 0; i < 3; ++i) if (ps == g_hudGame[i] && g_hudOurs[i]) ps = g_hudOurs[i];
    orig<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT)>(ctx, 9)(ctx, ps, ci, n);
}

// ---- HUD-less back buffer for FSR frame generation ---------------------------------
// The blit fills the display-res UI layer with the scene; the HUD is then drawn into it and the final composite puts
// the layer into the back buffer. A copy of the layer taken right after the blit is the scene without the HUD;
// replaying the game's own composite draw with that copy bound, into p12HudlessTarget(), gives the back buffer without
// the HUD, encoded exactly as the real one (SDR, HDR10).
static ID3D11Texture2D* g_layerCopy;
static ID3D11ShaderResourceView* g_layerCopySRV;
static ID3D11Resource* g_layerRes;  // identity only: the UI layer the composite reads
static bool g_layerReady;           // copied this frame, not yet replayed

static void hudlessCaptureLayer(ID3D11DeviceContext* ctx) {
    g_layerReady = false;
    if (!p12WantsHudless()) return;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* r = nullptr; rtv->GetResource(&r); rtv->Release();
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
static void hudlessReplay(ID3D11DeviceContext* ctx, UINT c, UINT start) {
    if (!g_layerReady) return;
    g_layerReady = false;
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
        ctx->OMGetRenderTargets(_countof(rtv), rtv, &dsv);
        const float black[4] = {};
        ctx->ClearRenderTargetView(target, black);
        ctx->PSSetShaderResources(slot, 1, &g_layerCopySRV);
        ctx->OMSetRenderTargets(1, &target, nullptr);
        orig<PFN_Draw>(ctx, 13)(ctx, c, start);
        ctx->PSSetShaderResources(slot, 1, &srv[slot]);
        ctx->OMSetRenderTargets(_countof(rtv), rtv, dsv);
        for (auto* x : rtv) if (x) x->Release();
        if (dsv) dsv->Release();
    }
    for (auto* x : srv) if (x) x->Release();
}

static void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext* ctx, UINT c, UINT s) {
    if (ctx == g_imm) p12FirstDraw();
    noteUiDraw(ctx);
    if (ctx == g_imm && g_immPS && g_immPS == g_taaPS) {
        void* ps = g_immPS;
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
            orig<PFN_Draw>(ctx, 13)(ctx, c, s);
            ctx->PSSetShaderResources(0, 1, &game);
            if (game) game->Release();
        } else orig<PFN_Draw>(ctx, 13)(ctx, c, s);
        hudlessCaptureLayer(ctx);  // the scene, before any HUD
        return;
    }
    if (ctx == g_imm && g_immPS && g_immPS == g_compositePS) {
        orig<PFN_Draw>(ctx, 13)(ctx, c, s);
        hudlessReplay(ctx, c, s);
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
    if (ctx == g_imm && SUCCEEDED(hr) && m && type == D3D11_MAP_WRITE_DISCARD && is256CB(r)) {
        std::lock_guard<std::mutex> lk(g_cbMx);
        for (auto& p : g_pend)
            if (!p.r) { p.r = r; p.real = m->pData; m->pData = p.scratch; break; }  // all busy: game writes the real mapping
    }
    return hr;
}
static void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext* ctx, ID3D11Resource* r, UINT sub) {
    if (ctx == g_imm) {
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
    if (ctx == g_imm && src && !box && is256CB(r)) noteCB(r, src);
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
    if (ctx != g_imm || n > 16) return orig<PFN_RSSetViewports>(ctx, 44)(ctx, n, vp);
    g_nvp = n; if (n) memcpy(g_vp, vp, sizeof(D3D11_VIEWPORT) * n);
    if (!g_maskBound) return orig<PFN_RSSetViewports>(ctx, 44)(ctx, n, vp);
    applyViewports(ctx);
}
static void STDMETHODCALLTYPE hk_RSSetScissorRects(ID3D11DeviceContext* ctx, UINT n, const D3D11_RECT* r) {
    if (ctx != g_imm || n > 16) return orig<PFN_RSSetScissor>(ctx, 45)(ctx, n, r);
    g_nsc = n; if (n) memcpy(g_sc, r, sizeof(D3D11_RECT) * n);
    if (!g_maskBound) return orig<PFN_RSSetScissor>(ctx, 45)(ctx, n, r);
    applyViewports(ctx);
}

static void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    using F = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
    if (ctx != g_imm || !g_maskRTV || n > 8) return orig<F>(ctx, 33)(ctx, n, rtv, dsv);
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
    if (ctx == g_imm && g_uiFix && rtv && rtv == g_maskRTV) rtv = g_bigRTV;
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
    if (ctx == g_imm) p12FirstDraw();
    if (ctx == g_imm && !g_maskRTV && g_maskPS && g_immPS == g_maskPS) detectMask(ctx);
    noteUiDraw(ctx);
    orig<PFN_DrawIndexed>(ctx, 12)(ctx, c, s, b);
}

void uiToggle() {
    g_uiFix = !g_uiFix;
    logf("ui: HUD mask upscale %s", g_uiFix ? "ON" : "OFF");
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
    static bool homeDown;
    bool home = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    if (home && !homeDown) dlssToggle();
    homeDown = home;
    if (g_frame % 30 == 0) {
        if (DeleteFileW(path(L"ffxv_dlss.toggle").c_str())) dlssToggle();
        if (DeleteFileW(path(L"ffxv_dlss.uifix").c_str())) uiToggle();
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
static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT si, UINT f) {
    if (p12Owns(sc)) return o_Present(sc, si, f);
    onPresent(sc);
    return p12IsGame(sc) ? p12Present(si, f) : o_Present(sc, si, f);
}
static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT si, UINT f, const DXGI_PRESENT_PARAMETERS* p) {
    if (p12Owns(sc)) return o_Present1(sc, si, f, p);
    onPresent(sc);
    return p12IsGame(sc) ? p12Present(si, f) : o_Present1(sc, si, f, p);
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

// [d3d12] present=1: the game's swapchain goes on a hidden window and ours takes the real one (present12.cpp).
static void createdBy(IDXGISwapChain* sc, bool p12) {
    hookSwapChain(sc);  // our own D3D12 swapchain never gets here: p12Init creates it through the original function
    if (p12) p12Attach(sc);
}
static HRESULT STDMETHODCALLTYPE hk_CreateSC(IDXGIFactory* f, IUnknown* d, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc) {
    if (p12Busy()) return o_CreateSC(f, d, desc, sc);  // Streamline creating its own swapchain inside p12Init
    bool game = calledByGame(_ReturnAddress());
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
    HRESULT hr = o_CreateSC(f, d, desc ? &dd : nullptr, sc);
    if (hidden && FAILED(hr)) { p12Abort(); hidden = nullptr; hr = o_CreateSC(f, d, desc, sc); }
    if (SUCCEEDED(hr) && sc && *sc) createdBy(*sc, hidden != nullptr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreateSCHwnd(IDXGIFactory2* f, IUnknown* d, HWND w, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** sc) {
    if (p12Busy()) return o_CreateSCHwnd(f, d, w, desc, fs, o, sc);
    bool game = calledByGame(_ReturnAddress());
    HWND hidden = game && desc && p12Enabled() ? p12Init(d, f, o_CreateSCHwnd, w, desc) : nullptr;
    HRESULT hr = o_CreateSCHwnd(f, d, hidden ? hidden : w, desc, fs, o, sc);
    if (hidden && FAILED(hr)) { p12Abort(); hidden = nullptr; hr = o_CreateSCHwnd(f, d, w, desc, fs, o, sc); }
    if (SUCCEEDED(hr) && sc && *sc) createdBy(*sc, hidden != nullptr);
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
    return m == g_exe;
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

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** out) {
    init();
    HRESULT hr = realFn<decltype(&CreateDXGIFactory)>("CreateDXGIFactory")(riid, out);
    if (SUCCEEDED(hr)) hookFactory((IUnknown*)*out);
    return hr;
}
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** out) {
    init();
    HRESULT hr = realFn<decltype(&CreateDXGIFactory1)>("CreateDXGIFactory1")(riid, out);
    if (SUCCEEDED(hr)) hookFactory((IUnknown*)*out);
    return hr;
}
extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    init();
    HRESULT hr = realFn<decltype(&CreateDXGIFactory2)>("CreateDXGIFactory2")(flags, riid, out);
    if (SUCCEEDED(hr)) hookFactory((IUnknown*)*out);
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
    }
    return TRUE;
}
