// In-game settings menu (Insert) and fps counter, drawn with Dear ImGui by the D3D12 presenter, or natively (no frame
// generation backend started, present_mode.h) on the game's own D3D11 device into its own back buffer.
// ImGui renders into an offscreen RGBA8 texture (premultiplied, sRGB values); one fullscreen-triangle pass blends that
// onto the back buffer, encoding for the output (SDR as is, HDR10 to PQ, scRGB to linear). The game's D3D11 state is
// never touched. While the menu is open the game's DirectInput and XInput reads come back empty, and DirectInput mouse
// deltas drive the menu cursor (so it works even when the game holds the mouse exclusively).
#include "common.h"
#include "fg_select.h"
#include "menu_buttons.h"
#include "menu_pad.h"
#include "menu_look.h"
#include "menu_deck.h"
#include "menu_pages.h"
#include "game_config.h"
#include "xess_rules.h"
#include "antilag_rules.h"
#include "native_frame.h"
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#define DIRECTINPUT_VERSION 0x0800
#ifndef LUMENVEIL_VERSION
#error LUMENVEIL_VERSION comes from CMakeLists.txt (project VERSION)
#endif
#include <dinput.h>
#include <wincodec.h>
#include <Xinput.h>
#include <shlobj.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cmath>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_dx11.h>

namespace {
constexpr UINT kFrames = 3;
ID3D12Device* g_dev;
ID3D12CommandQueue* g_queue;
ID3D12DescriptorHeap *g_srvHeap, *g_rtvHeap;   // srv: 0 font, 1 ui texture, 2 logo; rtv: 0 ui texture, 1 back buffer, 2 layer
ID3D12CommandAllocator* g_alloc[kFrames];
ID3D12GraphicsCommandList* g_cl;
ID3D12Fence* g_fence;
UINT64 g_fenceVal, g_allocDone[kFrames];
UINT g_frame;
HANDLE g_event;
ID3D12RootSignature* g_rs;
ID3D12PipelineState *g_pso, *g_psoLayer;  // composite onto the back buffer / into the FSR UI layer
ID3D12Resource* g_layer;  // FSR frame generation: the composited UI, which FSR draws over real and generated frames
bool g_layered;           // g_layer is registered with FSR
DXGI_FORMAT g_psoFmt;
ID3D12Resource* g_tex;
ID3D12Resource* g_logo;  // the menu logo (null if it could not be loaded)
float g_logoW, g_logoH;
UINT g_w, g_h, g_encode;                       // encode: 0 SDR, 1 HDR10 PQ, 2 scRGB
HWND g_wnd;
// Native: the same menu on the game's D3D11 device. ImGui draws into g_tex11, the composite blends it onto the back buffer.
bool g_native;
ID3D11Device* g_dev11;
ID3D11DeviceContext* g_ctx11;
ID3D11DeviceContext1* g_ctx11_1;  // ClearView, to clear only the area drawn in (null: the whole texture)
ID3D11Texture2D* g_tex11;
ID3D11RenderTargetView* g_texRtv11;
ID3D11ShaderResourceView *g_texSrv11, *g_logo11;
ID3D11VertexShader* g_vs11;
ID3D11PixelShader* g_ps11;
ID3D11Buffer* g_cb11;
ID3D11BlendState* g_blend11;
ID3D11RasterizerState* g_rs11;
ID3D11DepthStencilState* g_ds11;
UINT g_cbEncode = ~0u;
D3D12_RECT g_nativeBox;           // this frame's area drawn in, from compose
NativeTargetKey g_nativeBuilt;    // the back buffer g_tex11 was built for
unsigned g_nativeDrawn, g_nativeAcquired;
ImFont *g_font, *g_big, *g_bigThin;  // g_bigThin: base fps, lighter than the bold output fps
ImFont* g_small;                     // the desktop menu's notes and footer, the counter's units and lines
const char* g_fontName = "ImGui default";
char g_gpu[64];  // e.g. "RTX 4070": the adapter's name without the vendor prefix, for the counter
UINT g_vendorId;  // adapter PCI vendor: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
float g_scale = 1;
bool g_ready;
std::atomic<bool> g_open;  // read by the game's input hooks on their own threads
// The controller: hooks publish each pad here; the present thread merges it once a frame (g_padView), times the combo and
// feeds the menu. g_padUsed: the menu has had controller input since it opened (shows the hints); g_padOpened: the combo
// opened it this frame; g_padBusy: last frame a list was open or a slider was being adjusted (B backs out instead of
// closing); g_padRelease: the menu closed, release the controller keys next frame.
pad::PadSlots g_pad;
pad::KeyLatch g_keyLatch;  // keys held as the menu closed, hidden from the game's keyboard reads until released
pad::EmptySlots g_emptySlots;  // empty controller slots answered from the cache (the hook) and their counts (logged once)
pad::PadView g_padView;
bool g_padUsed, g_padOpened, g_padBusy, g_padRelease;
bool g_typing;  // last frame a setting's box was being typed in: Esc then leaves the box, not the menu
// The Steam Deck layout (menu_deck.h): g_isDeck, a Steam Deck was detected; g_layout, [overlay] layout (0 Automatic,
// 1 Steam Deck, 2 Desktop); g_deckOpen, the menu shows the Deck layout (picked when it opens, changed by the layout
// button through g_swap, applied in compose). Its fonts are built at g_deckFontK times their design size, on every
// machine, so a PC can switch to the Deck layout without a restart. g_focusPortable: the desktop menu has just taken
// over from the Deck layout and focuses its layout button.
bool g_isDeck, g_deckOpen, g_focusPortable;
deck::Swap g_swap;
int g_layout;
float g_deckFontK = 1;
ImFont *g_deck26, *g_deck24, *g_deck19;
deck::Nav g_deckNav;
deck::Typing g_deckTyping;
static_assert(pad::kUp == XINPUT_GAMEPAD_DPAD_UP && pad::kDown == XINPUT_GAMEPAD_DPAD_DOWN && pad::kLeft == XINPUT_GAMEPAD_DPAD_LEFT &&
              pad::kRight == XINPUT_GAMEPAD_DPAD_RIGHT && pad::kL3 == XINPUT_GAMEPAD_LEFT_THUMB && pad::kR3 == XINPUT_GAMEPAD_RIGHT_THUMB &&
              pad::kA == XINPUT_GAMEPAD_A && pad::kB == XINPUT_GAMEPAD_B && pad::kStickDeadzone == XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE &&
              pad::kL1 == XINPUT_GAMEPAD_LEFT_SHOULDER && pad::kR1 == XINPUT_GAMEPAD_RIGHT_SHOULDER,
              "menu_pad.h's button bits match XInput's");
LARGE_INTEGER g_freq, g_last;

// settings ([overlay] in ffxv_dlss.ini; DLSS and FG ones live in their own sections)
bool g_counter;
int g_corner;                                   // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right

// fps: rendered = our Present calls, shown = frames Streamline actually presented (includes generated ones)
unsigned g_rendered, g_shown;
LARGE_INTEGER g_statStart;
float g_fpsRendered, g_fpsShown;
// presented frame times for the counter's graph: written by menuCount, read by drawCounter, on the same thread
look::FrameRing g_frames;

// DirectInput mouse, accumulated on whatever thread the game polls from
std::mutex g_inMx;
long g_dx, g_dy, g_wheel;
MouseButtons g_btns;                            // buttons as the game's DirectInput mouse reads report them
// While the menu is open: which input carries the mouse (logged every 2 s, reset after each line)
struct InputStats { unsigned stateReads16, stateReads20, otherSizeReads, otherSize, dataReads, events, ofsSeen, diDown; } g_inStats;
ULONGLONG g_inStatsTick;
ULONGLONG g_diMouseTick;                        // last time a DirectInput mouse was read
ImVec2 g_mouse;

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
int iniGet(const wchar_t* sec, const wchar_t* key, int def) { return GetPrivateProfileIntW(sec, key, def, path(L"ffxv_dlss.ini").c_str()); }
void iniSet(const wchar_t* sec, const wchar_t* key, int v) { WritePrivateProfileStringW(sec, key, std::to_wstring(v).c_str(), path(L"ffxv_dlss.ini").c_str()); }

// The game's settings: %USERPROFILE%\Documents\My Games\FINAL FANTASY XV\Steam\<steam id>\savestorage\GraphicsConfig.ini
// (newest if several accounts have one), or "" if none. game_config.h says why USERPROFILE.
std::wstring gameConfig() {
    const std::wstring base = gameSettingsRoot(envValue([](wchar_t* b, DWORD n) { return GetEnvironmentVariableW(L"USERPROFILE", b, n); }));
    if (base.empty()) { logf("menu: game config not found (no USERPROFILE)"); return {}; }
    std::wstring best; FILETIME bestT = {};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
        const std::wstring p = base + fd.cFileName + L"\\savestorage\\GraphicsConfig.ini";
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (fd.cFileName[0] != L'.' && GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &bestT) > 0) {
            best = p; bestT = a.ftLastWriteTime;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    logf("menu: game config %s", best.empty() ? "not found" : "found");  // the path holds the Steam ID: keep it out of shared logs
    return best;
}

// ---- input hooks --------------------------------------------------------------------
using PFN_GetDeviceState = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using PFN_GetDeviceData = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using PFN_XInputGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
constexpr int kHookSlots = 4;  // distinct DirectInput read methods hooked: mouse and keyboard, each for A and W
PFN_GetDeviceState o_GetDeviceState[kHookSlots];
PFN_GetDeviceData o_GetDeviceData[kHookSlots];
PFN_XInputGetState o_XInputGetState;
std::atomic<DWORD> g_diTid, g_xiTid;
std::atomic<unsigned> g_diPolls;

bool isMouse(IDirectInputDevice8W* d) {
    DIDEVCAPS c = { sizeof c };
    return SUCCEEDED(d->GetCapabilities(&c)) && GET_DIDEVICE_TYPE(c.dwDevType) == DI8DEVTYPE_MOUSE;
}
bool isKeyboard(IDirectInputDevice8W* d) {
    DIDEVCAPS c = { sizeof c };
    return SUCCEEDED(d->GetCapabilities(&c)) && GET_DIDEVICE_TYPE(c.dwDevType) == DI8DEVTYPE_KEYBOARD;
}

HRESULT getState(int v, IDirectInputDevice8W* d, DWORD cb, LPVOID data) {
    HRESULT hr = o_GetDeviceState[v](d, cb, data);
    g_diTid = GetCurrentThreadId(); ++g_diPolls;
    // menu closed: a keyboard state (256 bytes; a mouse's is 16 or 20, a pad's 80 or 272) keeps a key held at the close up
    if (!g_open && SUCCEEDED(hr) && data && cb == 256 && g_keyLatch.any()) g_keyLatch.hide((unsigned char*)data);
    if (!g_open || FAILED(hr) || !data) return hr;
    if (isMouse(d)) {
        std::lock_guard<std::mutex> lk(g_inMx);
        if (cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2)) {
            const DIMOUSESTATE* m = (const DIMOUSESTATE*)data;
            g_dx += m->lX; g_dy += m->lY; g_wheel += m->lZ;
            for (int i = 0; i < 3; ++i) {
                g_btns.read(i, (m->rgbButtons[i] & 0x80) != 0);
                g_inStats.diDown += g_btns.down[i];
            }
            g_diMouseTick = GetTickCount64();
            ++(cb == sizeof(DIMOUSESTATE) ? g_inStats.stateReads16 : g_inStats.stateReads20);
        } else {
            ++g_inStats.otherSizeReads; g_inStats.otherSize = cb;
        }
    }
    memset(data, 0, cb);  // the game sees nothing pressed and no movement
    return hr;
}
HRESULT getData(int v, IDirectInputDevice8W* d, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD n, DWORD fl) {
    HRESULT hr = o_GetDeviceData[v](d, cb, od, n, fl);
    g_diTid = GetCurrentThreadId(); ++g_diPolls;
    if (!g_open || FAILED(hr) || !n) return hr;
    if (od && isMouse(d)) {
        std::lock_guard<std::mutex> lk(g_inMx);
        for (DWORD i = 0; i < *n; ++i) {
            const DIDEVICEOBJECTDATA& e = *(const DIDEVICEOBJECTDATA*)((const BYTE*)od + i * cb);
            if (e.dwOfs == DIMOFS_X) g_dx += long(e.dwData);
            else if (e.dwOfs == DIMOFS_Y) g_dy += long(e.dwData);
            else if (e.dwOfs == DIMOFS_Z) g_wheel += long(e.dwData);
            else if (e.dwOfs >= DIMOFS_BUTTON0 && e.dwOfs <= DIMOFS_BUTTON2) {
                const int b = int(e.dwOfs - DIMOFS_BUTTON0);
                g_btns.read(b, (e.dwData & 0x80) != 0);
                g_inStats.diDown += g_btns.down[b];
            }
            g_inStats.ofsSeen |= e.dwOfs < 31 ? 1u << e.dwOfs : 1u << 31;
        }
        g_diMouseTick = GetTickCount64();
        ++g_inStats.dataReads; g_inStats.events += *n;
    }
    // a keyboard's presses go and its releases stay, so a key held as the menu opened is seen released; other events
    // are consumed by the menu
    if (od && isKeyboard(d)) *n = pad::keepReleases((unsigned char*)od, *n, cb);
    else if (!(fl & DIGDD_PEEK)) *n = 0;
    return hr;
}
template <int S> HRESULT STDMETHODCALLTYPE hk_GetDeviceState(IDirectInputDevice8W* d, DWORD cb, LPVOID p) { return getState(S, d, cb, p); }
template <int S> HRESULT STDMETHODCALLTYPE hk_GetDeviceData(IDirectInputDevice8W* d, DWORD cb, LPDIDEVICEOBJECTDATA o, LPDWORD n, DWORD f) { return getData(S, d, cb, o, n, f); }
// The game's controller read: published for the menu without a lock (this is the game's input thread), then blanked
// while the menu is open; while it is closed, a button held when the menu closed stays hidden until released. A slot
// that answered "not connected" is answered from the cache until its re-ask time, as Microsoft recommends.
DWORD WINAPI hk_XInputGetState(DWORD i, XINPUT_STATE* s) {
    const ULONGLONG t = GetTickCount64();
    DWORD r;
    if (s && g_emptySlots.skip(i, t)) { *s = {}; r = ERROR_DEVICE_NOT_CONNECTED; }  // asked recently: still empty
    else { r = o_XInputGetState(i, s); g_emptySlots.answer(i, r == ERROR_DEVICE_NOT_CONNECTED, t); }
    g_xiTid = GetCurrentThreadId();
    if (s) {
        const bool ok = r == ERROR_SUCCESS;
        g_pad.record(i, ok, ok ? s->Gamepad.wButtons : 0, ok ? s->Gamepad.sThumbLX : 0, ok ? s->Gamepad.sThumbLY : 0, t);
        if (ok && g_open) s->Gamepad = {};
        else if (ok) s->Gamepad.wButtons = g_pad.hide(i, s->Gamepad.wButtons);
    }
    return r;
}

// Each device kind's read methods are hooked through a throwaway device of that kind: another program's overlay can
// give the mouse and the keyboard methods of their own, and A and W may share them. One hook per distinct method.
void hookInput() {
    using PFN_DI8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    if (auto create = (PFN_DI8Create)GetProcAddress(LoadLibraryW(L"dinput8.dll"), "DirectInput8Create")) {
        const IID* iids[2] = { &IID_IDirectInput8W, &IID_IDirectInput8A };
        void* const states[kHookSlots] = { (void*)hk_GetDeviceState<0>, (void*)hk_GetDeviceState<1>, (void*)hk_GetDeviceState<2>, (void*)hk_GetDeviceState<3> };
        void* const datas[kHookSlots] = { (void*)hk_GetDeviceData<0>, (void*)hk_GetDeviceData<1>, (void*)hk_GetDeviceData<2>, (void*)hk_GetDeviceData<3> };
        void* stateAt[kHookSlots] = {}; void* dataAt[kHookSlots] = {};
        int nState = 0, nData = 0;
        auto hookOnce = [](void* target, void** at, int& n, void* const* hooks, void** orig) {
            for (int i = 0; i < n; ++i) if (at[i] == target) return;  // the same method already hooked for another device
            if (n < kHookSlots && MH_CreateHook(target, hooks[n], &orig[n]) == MH_OK) at[n++] = target;
        };
        for (int v = 0; v < 2; ++v) {
            IDirectInput8W* di = nullptr;
            if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *iids[v], (void**)&di, nullptr))) continue;
            for (const GUID* g : { &GUID_SysMouse, &GUID_SysKeyboard }) {
                IDirectInputDevice8W* dev = nullptr;
                if (SUCCEEDED(di->CreateDevice(*g, &dev, nullptr))) {
                    void** vt = *(void***)dev;
                    hookOnce(vt[9], stateAt, nState, states, (void**)o_GetDeviceState);
                    hookOnce(vt[10], dataAt, nData, datas, (void**)o_GetDeviceData);
                }
                release(dev);
            }
            release(di);
        }
        logf("menu: input read hooks: %d state, %d data", nState, nData);
    }
    if (FARPROC x = GetProcAddress(LoadLibraryW(L"xinput9_1_0.dll"), "XInputGetState"))
        MH_CreateHook((void*)x, (void*)hk_XInputGetState, (void**)&o_XInputGetState);
    MH_EnableHook(MH_ALL_HOOKS);
}

// ---- composite pass -----------------------------------------------------------------
const char kHlsl[] = R"(
Texture2D<float4> g_ui : register(t0);
cbuffer C : register(b0) { uint g_encode; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float4 c = g_ui.Load(int3(pos.xy, 0));  // premultiplied: ImGui blended over a transparent clear
    if (c.a <= 0) discard;
    if (g_encode == 0) return c;
    float3 s = c.rgb / c.a;
    float3 l = s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
    if (g_encode == 2) return float4(l * 2.5 * c.a, c.a);  // scRGB: 1.0 = 80 nits, white at 200
    l = mul(float3x3(0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956), l);  // BT.709 -> BT.2020
    float3 y = pow(l * 200.0 / 10000.0, 0.1593017578125);
    return float4(pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375) * c.a, c.a);
}
)";

// The composite's two shaders, for either renderer.
HRESULT compileComposite(ID3DBlob** vs, ID3DBlob** ps, ID3DBlob** err) {
    const pD3DCompile compile = shaderCompiler();
    HRESULT hr = compile ? compile(kHlsl, sizeof kHlsl - 1, "menu", nullptr, nullptr, "vs", "vs_5_0", 0, 0, vs, err) : E_FAIL;
    if (SUCCEEDED(hr)) hr = compile(kHlsl, sizeof kHlsl - 1, "menu", nullptr, nullptr, "ps", "ps_5_0", 0, 0, ps, err);
    return hr;
}

bool createPso(DXGI_FORMAT fmt) {
    if (g_pso && g_psoFmt == fmt) return true;
    release(g_pso);
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    HRESULT hr = compileComposite(&vs, &ps, &err);
    if (SUCCEEDED(hr) && !g_rs) {
        D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[2] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable = { 1, &range }; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants.Num32BitValues = 1; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd = { 2, rp };
        ID3DBlob* rs = nullptr;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err);
        if (SUCCEEDED(hr)) hr = g_dev->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&g_rs));
        release(rs);
    }
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_rs;
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.BlendState.RenderTarget[0] = { TRUE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD, D3D12_BLEND_ZERO, D3D12_BLEND_ONE,
                                          D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE };
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = fmt;
        pd.SampleDesc = { 1, 0 };
        hr = g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso));
        // the layer is cleared, so the shader's premultiplied output (alpha included) is written as is
        pd.BlendState.RenderTarget[0] = { FALSE, FALSE };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (SUCCEEDED(hr) && !g_psoLayer) hr = g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_psoLayer));
    }
    if (FAILED(hr)) logf("menu: composite pipeline failed %08x %s", hr, err ? (const char*)err->GetBufferPointer() : "");
    else g_psoFmt = fmt;
    release(vs); release(ps); release(err);
    return SUCCEEDED(hr);
}

// Natively: the composite's shaders (the same kHlsl, through shaderCompiler) and fixed state, made once.
bool nativeObjects() {
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    HRESULT hr = compileComposite(&vs, &ps, &err);
    if (SUCCEEDED(hr)) hr = g_dev11->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs11);
    if (SUCCEEDED(hr)) hr = g_dev11->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps11);
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    if (SUCCEEDED(hr)) hr = g_dev11->CreateBuffer(&bd, nullptr, &g_cb11);
    D3D11_BLEND_DESC bl = {};  // premultiplied over the frame, alpha left as the game wrote it
    bl.RenderTarget[0] = { TRUE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO, D3D11_BLEND_ONE,
                           D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE };
    if (SUCCEEDED(hr)) hr = g_dev11->CreateBlendState(&bl, &g_blend11);
    D3D11_RASTERIZER_DESC rd = { D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, TRUE, FALSE, FALSE };
    if (SUCCEEDED(hr)) hr = g_dev11->CreateRasterizerState(&rd, &g_rs11);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    if (SUCCEEDED(hr)) hr = g_dev11->CreateDepthStencilState(&dd, &g_ds11);
    if (FAILED(hr)) logf("menu: native composite failed %08x %s", hr, err ? (const char*)err->GetBufferPointer() : "");
    release(vs); release(ps); release(err);
    return SUCCEEDED(hr);
}

// Natively: the game's D3D11 state the native frame touches, saved before its first bind and put back after its last,
// on every path. main.cpp's hooks pass these calls straight through meanwhile (g_nativeDrawing).
struct D3D11State {
    static constexpr UINT kCi = 256;  // D3D11's limit on class instances per stage
    D3D11_PRIMITIVE_TOPOLOGY topo; ID3D11InputLayout* il; ID3D11Buffer *vb, *ib; UINT vbStride, vbOffset, ibOffset; DXGI_FORMAT ibFmt;
    ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11GeometryShader* gs; ID3D11HullShader* hs; ID3D11DomainShader* ds; ID3D11ComputeShader* cs;
    ID3D11ClassInstance* ci[6][kCi]; UINT nci[6];
    ID3D11ShaderResourceView* psSrv; ID3D11SamplerState* psSmp; ID3D11Buffer *psCb, *vsCb;
    ID3D11RasterizerState* rs; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; UINT nvp;
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; UINT nsc;
    ID3D11BlendState* blend; float factor[4]; UINT mask; ID3D11DepthStencilState* dss; UINT ref;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]; ID3D11DepthStencilView* dsv;

    void save(ID3D11DeviceContext* c) {
        c->IAGetPrimitiveTopology(&topo); c->IAGetInputLayout(&il); c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
        c->IAGetIndexBuffer(&ib, &ibFmt, &ibOffset);
        for (UINT& n : nci) n = kCi;
        c->VSGetShader(&vs, ci[0], &nci[0]); c->PSGetShader(&ps, ci[1], &nci[1]); c->GSGetShader(&gs, ci[2], &nci[2]);
        c->HSGetShader(&hs, ci[3], &nci[3]); c->DSGetShader(&ds, ci[4], &nci[4]); c->CSGetShader(&cs, ci[5], &nci[5]);
        c->PSGetShaderResources(0, 1, &psSrv); c->PSGetSamplers(0, 1, &psSmp); c->PSGetConstantBuffers(0, 1, &psCb); c->VSGetConstantBuffers(0, 1, &vsCb);
        c->RSGetState(&rs);
        nvp = nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        c->RSGetViewports(&nvp, vp); c->RSGetScissorRects(&nsc, sc);
        c->OMGetBlendState(&blend, factor, &mask); c->OMGetDepthStencilState(&dss, &ref);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    }
    void restore(ID3D11DeviceContext* c) {
        c->IASetPrimitiveTopology(topo); c->IASetInputLayout(il); c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
        c->IASetIndexBuffer(ib, ibFmt, ibOffset);
        c->VSSetShader(vs, ci[0], nci[0]); c->PSSetShader(ps, ci[1], nci[1]); c->GSSetShader(gs, ci[2], nci[2]);
        c->HSSetShader(hs, ci[3], nci[3]); c->DSSetShader(ds, ci[4], nci[4]); c->CSSetShader(cs, ci[5], nci[5]);
        c->PSSetShaderResources(0, 1, &psSrv); c->PSSetSamplers(0, 1, &psSmp); c->PSSetConstantBuffers(0, 1, &psCb); c->VSSetConstantBuffers(0, 1, &vsCb);
        c->RSSetState(rs); c->RSSetViewports(nvp, vp); c->RSSetScissorRects(nsc, sc);
        c->OMSetBlendState(blend, factor, mask); c->OMSetDepthStencilState(dss, ref);
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        release(il); release(vb); release(ib); release(vs); release(ps); release(gs); release(hs); release(ds); release(cs);
        for (UINT k = 0; k < 6; ++k) for (UINT i = 0; i < nci[k]; ++i) release(ci[k][i]);
        release(psSrv); release(psSmp); release(psCb); release(vsCb); release(rs); release(blend); release(dss);
        for (auto*& r : rtv) release(r);
        release(dsv);
    }
};
D3D11State g_saved;

// Natively: the mod's texture at the back buffer's size (the composite's view on the back buffer takes the swap chain's
// format, as the D3D12 composite's pipeline does).
bool nativeTargets(const NativeTargetKey& k) {
    release(g_texRtv11); release(g_texSrv11); release(g_tex11);
    g_w = k.w; g_h = k.h;
    D3D11_TEXTURE2D_DESC td = { k.w, k.h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    HRESULT hr = g_dev11->CreateTexture2D(&td, nullptr, &g_tex11);
    if (SUCCEEDED(hr)) hr = g_dev11->CreateRenderTargetView(g_tex11, nullptr, &g_texRtv11);
    if (SUCCEEDED(hr)) hr = g_dev11->CreateShaderResourceView(g_tex11, nullptr, &g_texSrv11);
    if (FAILED(hr)) { release(g_texRtv11); release(g_texSrv11); release(g_tex11); }
    g_encode = p12OutputEncoding(nullptr);
    logf("menu: targets %ux%u fmt %u, output %s: %08x", k.w, k.h, k.fmt, g_encode == 1 ? "HDR10 (PQ)" : g_encode == 2 ? "scRGB" : "SDR", hr);
    return SUCCEEDED(hr);
}

// Natively, between save and restore: ImGui into g_tex11 over the area drawn in, then the composite onto the back buffer.
bool nativeDraw(ID3D11RenderTargetView* bbRtv) {
    if (!g_texRtv11) return false;
    ID3D11DeviceContext* c = g_ctx11;
    const float clear[4] = {};
    if (g_ctx11_1) g_ctx11_1->ClearView(g_texRtv11, clear, &g_nativeBox, 1);
    else c->ClearRenderTargetView(g_texRtv11, clear);
    const D3D11_VIEWPORT vp = { 0, 0, float(g_w), float(g_h), 0, 1 };
    c->OMSetRenderTargets(1, &g_texRtv11, nullptr);
    c->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if ((g_encode = p12OutputEncoding(nullptr)) != g_cbEncode) {
        const UINT cb[4] = { g_encode };
        c->UpdateSubresource(g_cb11, 0, nullptr, cb, 0, 0);
        g_cbEncode = g_encode;
    }
    c->OMSetRenderTargets(1, &bbRtv, nullptr);
    c->OMSetBlendState(g_blend11, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(g_ds11, 0);
    c->RSSetState(g_rs11);
    c->RSSetViewports(1, &vp);
    c->RSSetScissorRects(1, &g_nativeBox);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g_vs11, nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0); c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
    c->PSSetShader(g_ps11, nullptr, 0);
    c->PSSetShaderResources(0, 1, &g_texSrv11);
    c->PSSetConstantBuffers(0, 1, &g_cb11);
    c->Draw(3, 0);
    return true;
}

// The real operations nativeFrame (native_frame.h) runs in its fixed order.
struct D3D11FrameOps {
    IDXGISwapChain* sc;
    ID3D11Texture2D* bb = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    bool acquire() {
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) { bb = nullptr; return false; }
        ++g_nativeAcquired;
        bb->GetDesc(&desc);
        return true;
    }
    NativeTargetKey key() const { return { desc.Width, desc.Height, UINT(desc.Format) }; }
    bool view() { if (SUCCEEDED(g_dev11->CreateRenderTargetView(bb, nullptr, &rtv))) return true; rtv = nullptr; return false; }
    bool rebuild(const NativeTargetKey& k) { return nativeTargets(k); }
    void save() { g_nativeDrawing = true; g_saved.save(g_ctx11); }
    bool draw() { return nativeDraw(rtv); }
    void restore() { g_saved.restore(g_ctx11); g_nativeDrawing = false; }
    void releaseView() { release(rtv); }
    void releaseBuffer() { release(bb); }
};

D3D12_CPU_DESCRIPTOR_HANDLE cpuAt(ID3D12DescriptorHeap* h, UINT i, D3D12_DESCRIPTOR_HEAP_TYPE t) {
    D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart(); c.ptr += i * g_dev->GetDescriptorHandleIncrementSize(t); return c;
}
D3D12_GPU_DESCRIPTOR_HANDLE gpuAt(ID3D12DescriptorHeap* h, UINT i) {
    D3D12_GPU_DESCRIPTOR_HANDLE g = h->GetGPUDescriptorHandleForHeapStart(); g.ptr += i * g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); return g;
}
void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER rb = {};
    rb.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    g_cl->ResourceBarrier(1, &rb);
}

// ---- look ---------------------------------------------------------------------------
void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring sys = std::wstring(win) + L"\\Fonts\\", mod = path(L"ffxv_dlss\\fonts\\");
    ImFontConfig cfg; cfg.OversampleH = 3; cfg.OversampleV = 2;
    static const ImWchar ranges[] = { 0x20, 0xFF, 0x2190, 0x2193, 0 };  // Latin-1 + arrows (the menu's "→")
    auto load = [&](const std::wstring& p, float px) -> ImFont* {
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
        char u8[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, u8, sizeof u8, nullptr, nullptr);
        return io.Fonts->AddFontFromFileTTF(u8, px, &cfg, ranges);
    };
    // TeX Gyre Adventor (bundled, GUST Font License): a free ITC Avant Garde Gothic clone, close to FFXV's own UI font.
    // Wider than Segoe UI, hence one px smaller.
    if ((g_font = load(mod + L"texgyreadventor-regular.otf", std::round(16 * g_scale)))) g_fontName = "TeX Gyre Adventor";
    else if ((g_font = load(sys + L"segoeui.ttf", std::round(17 * g_scale)))) g_fontName = "Segoe UI";
    if (!(g_big = load(mod + L"texgyreadventor-bold.otf", std::round(38 * g_scale)))) g_big = load(sys + L"seguisb.ttf", std::round(40 * g_scale));
    if (!g_font) { ImFontConfig d; d.SizePixels = std::round(13 * g_scale); g_font = io.Fonts->AddFontDefault(&d); }
    if (!(g_bigThin = load(mod + L"texgyreadventor-regular.otf", std::round(38 * g_scale)))) g_bigThin = load(sys + L"segoeui.ttf", std::round(40 * g_scale));
    if (!g_big) g_big = g_font;
    if (!g_bigThin) g_bigThin = g_big;
    if (!(g_small = load(mod + L"texgyreadventor-regular.otf", std::round(13 * g_scale)))) g_small = load(sys + L"segoeui.ttf", std::round(14 * g_scale));
    if (!g_small) g_small = g_font;
    {  // the Steam Deck layout's 26, 24 and 19 px, after the desktop's fonts, which stay as they were
        cfg.OversampleH = cfg.OversampleV = 1;  // large sizes need none; keeps the atlas small
        ImFont** deckFonts[3] = { &g_deck26, &g_deck24, &g_deck19 };
        const float px[3] = { 26, 24, 19 };
        for (int i = 0; i < 3; ++i)
            if (!(*deckFonts[i] = load(mod + L"texgyreadventor-regular.otf", std::round(px[i] * g_deckFontK))))
                *deckFonts[i] = load(sys + L"segoeui.ttf", std::round(px[i] * g_deckFontK));
        if (!g_deck24) g_deck24 = g_deck26;
        if (!g_deck19) g_deck19 = g_deck26;
    }
}

void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10; s.ChildRounding = 8; s.FrameRounding = 6; s.PopupRounding = 8; s.GrabRounding = 6; s.ScrollbarRounding = 6;
    s.WindowBorderSize = 1; s.FrameBorderSize = 0; s.WindowPadding = { 18, 16 }; s.FramePadding = { 10, 6 }; s.ItemSpacing = { 10, 9 };
    s.SeparatorTextBorderSize = 1; s.SeparatorTextPadding = { 0, 6 }; s.WindowTitleAlign = { 0.0f, 0.5f };
    // neutral by default: controls that belong to a vendor push its colour (AccentScope)
    const ImVec4 accent(0.59f, 0.62f, 0.67f, 1.0f), accentHi(0.83f, 0.85f, 0.88f, 1.0f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.075f, 0.09f, 0.94f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.095f, 0.11f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.07f, 0.075f, 0.09f, 0.94f);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_CheckMark] = accentHi;
    c[ImGuiCol_NavCursor] = accentHi;  // the controller's outline on the focused setting (FrameRounding, no animation)
    c[ImGuiCol_SliderGrab] = accent; c[ImGuiCol_SliderGrabActive] = accentHi;
    c[ImGuiCol_Button] = c[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.25f);  // Button: the combo arrow
    c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.95f, 1); c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.63f, 1);
    s.ScaleAllSizes(g_scale);
}

const ImVec4 kAmber(1.0f, 0.75f, 0.25f, 1);

// Accent colours: a vendor's own on the controls that belong to it (NVIDIA green, AMD red, Intel blue), neutral grey on
// the game's and the mod's own settings. One row per vendor.
enum Vendor { kNvidia, kAmd, kIntel, kNeutral };
struct Accent { ImVec4 base, hi; };
const Accent kAccents[] = {
    { ImVec4(0.46f, 0.73f, 0.00f, 1), ImVec4(0.56f, 0.83f, 0.10f, 1) },
    { ImVec4(0.86f, 0.10f, 0.13f, 1), ImVec4(0.96f, 0.24f, 0.26f, 1) },
    { ImVec4(0.00f, 0.443f, 0.773f, 1), ImVec4(0.180f, 0.608f, 0.918f, 1) },
    { ImVec4(0.59f, 0.62f, 0.67f, 1), ImVec4(0.83f, 0.85f, 0.88f, 1) },
};

// Upscalers by id (0 DLSS, 1 FSR, 2 XeSS) and frame generation backends by id (0 DLSS-G, 1 FSR, 2 XeSS), one row each. The vendors'
// DLLs are not shipped with the mod: the player copies them in from the vendors' SDKs (the install scripts do it). The
// menu's texts about them live in menu_pages.h.
struct Upscaler { const char *name, *shortName; Vendor vendor; const wchar_t* file; };
const Upscaler kUpscalers[] = {
    { "NVIDIA DLSS", "DLSS", kNvidia, L"ffxv_dlss\\nvngx_dlss.dll" },
    { "AMD FSR", "FSR", kAmd, L"ffxv_dlss\\fidelityfx\\amd_fidelityfx_loader_dx12.dll" },
    { "Intel XeSS", "XeSS", kIntel, L"ffxv_dlss\\xess\\libxess.dll" },
};
struct Backend { const char* name; Vendor vendor; };
const Backend kBackends[] = {
    { "NVIDIA DLSS Frame Generation", kNvidia },
    { "AMD FSR Frame Generation", kAmd },
    { "Intel XeSS Frame Generation", kIntel },
};
constexpr int kUpscalerCount = IM_ARRAYSIZE(kUpscalers), kBackendCount = IM_ARRAYSIZE(kBackends);
Vendor backendVendor(int b) { return b >= 0 && b < kBackendCount ? kBackends[b].vendor : kNeutral; }
const char* syncName() { return g_vendorId == 0x1002 ? "FreeSync" : g_vendorId == 0x8086 ? "Adaptive Sync" : "G-Sync"; }

// The counter's frame time graph in a..b: one point per presented frame at its age over 2 s, redrawn every frame, the
// vertical scale eased toward max(average x 2.2, highest x 1.1) so a stutter does not make the line jump.
void drawGraph(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    static float yTop;
    static int lastFrame = -10;  // the frame the graph was last drawn in: a gap restarts the scale at its target
    const int frame = ImGui::GetFrameCount();
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const double now = double(q.QuadPart) / (g_freq.QuadPart ? g_freq.QuadPart : 1);
    const look::FrameRing& r = g_frames;
    double sum = 0;
    float most = 0;
    int n = 0;
    for (int i = 0; i < r.size(); ++i)
        if (now - r.time(i) <= 2.0) { sum += r.ms(i); most = std::max(most, r.ms(i)); ++n; }
    if (n < 2) { lastFrame = -10; return; }
    const float avg = float(sum / n), want = std::max(avg * 2.2f, most * 1.1f);
    yTop = frame == lastFrame + 1 ? look::easeScale(yTop, want, ImGui::GetIO().DeltaTime) : want;
    lastFrame = frame;
    const float s = g_scale, w = b.x - a.x, h = b.y - a.y;
    auto y = [&](float ms) { return b.y - 1 - std::min(1.f, ms / yTop) * (h - 3); };
    ImGui::PushClipRect(a, b, true);
    const float ay = std::round(y(avg)) + 0.5f;
    dl->AddLine(ImVec2(a.x, ay), ImVec2(b.x, ay), look::col(look::rgba(205, 215, 228, .16f)), s);
    static ImVector<ImVec2> pts;
    pts.resize(0);
    for (int i = 0; i < r.size(); ++i) pts.push_back(ImVec2(look::ageX(now, r.time(i), a.x, w), y(r.ms(i))));
    // the fill under the line: a quad per segment, fading toward the bottom
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImU32 clear = look::col(look::rgba(120, 170, 255, 0));
    auto fill = [&](float yy) { return look::col(look::rgba(120, 170, 255, .28f * std::clamp(1 - (yy - a.y) / h, 0.f, 1.f))); };
    for (int i = 0; i + 1 < pts.Size; ++i) {
        const ImVec2 p0 = pts[i], p1 = pts[i + 1];
        if (p1.x <= p0.x) continue;
        dl->PrimReserve(6, 4);
        const ImDrawIdx i0 = ImDrawIdx(dl->_VtxCurrentIdx);
        dl->PrimWriteVtx(p0, uv, fill(p0.y)); dl->PrimWriteVtx(p1, uv, fill(p1.y));
        dl->PrimWriteVtx(ImVec2(p1.x, b.y), uv, clear); dl->PrimWriteVtx(ImVec2(p0.x, b.y), uv, clear);
        dl->PrimWriteIdx(i0); dl->PrimWriteIdx(ImDrawIdx(i0 + 1)); dl->PrimWriteIdx(ImDrawIdx(i0 + 2));
        dl->PrimWriteIdx(i0); dl->PrimWriteIdx(ImDrawIdx(i0 + 2)); dl->PrimWriteIdx(ImDrawIdx(i0 + 3));
    }
    dl->AddPolyline(pts.Data, pts.Size, look::col(look::rgba(120, 170, 255, .25f)), ImDrawFlags_None, 3.5f * s);
    dl->AddPolyline(pts.Data, pts.Size, look::col(look::rgba(0xa9, 0xc8, 0xff, 1)), ImDrawFlags_None, 1.3f * s);
    ImGui::PopClipRect();
}

void drawCounter() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = g_scale, m = 24 * s;
    look::ctx().s = s; look::ctx().fontSmall = g_small;
    const bool right = g_corner & 1, bottom = g_corner & 2;
    const FgStatus fg = fgStatus();
    // the lines under the graph
    char fgLine[32], res[32] = "", verLine[96] = "";
    const bool gen = pages::counterFg(fgLine, sizeof fgLine, fg.available, fg.generating, fg.noFrames, g_fpsShown, g_fpsRendered);
    if (g_w && g_h) snprintf(res, sizeof res, "%u\xC3\x97%u", g_w, g_h);  // the screen resolution: the swap chain's size
    const DlssInfo di = dlssInfo();
    const int up = di.active == 1 || di.active == 2 ? di.active : 0;  // the upscaler running, else DLSS's version once loaded
    const char* ver = up == 1 ? di.fsrVersion : up == 2 ? di.xessVersion : di.version;
    if (*ver) snprintf(verLine, sizeof verLine, dlssEnabled() ? "%s %s" : "%s %s (off)", kUpscalers[up].shortName, ver);
    // one width while the numbers change and with FG on or off: the number slots are "000" wide and BASE always counts
    ImFont* sm = g_small;
    auto textW = [](ImFont* f, const char* t) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, t).x; };
    const float unit = 12 * s, slotFps = textW(g_big, "000"), slotBase = textW(g_bigThin, "000");
    const float unitFps = look::spacedText(nullptr, sm, unit, ImVec2(0, 0), "FPS", 0.18f, look::kInk2);
    const float unitBase = look::spacedText(nullptr, sm, unit, ImVec2(0, 0), "BASE", 0.18f, look::kInk2);
    const float numbersW = slotFps + 7 * s + unitFps + 22 * s + slotBase + 7 * s + unitBase;
    float widest = 0;
    for (const char* t : { (const char*)fgLine, (const char*)g_gpu, (const char*)res, (const char*)verLine }) widest = std::max(widest, textW(sm, t));
    const float width = look::counterWidth(s, numbersW, widest);
    ImGui::SetNextWindowPos({ right ? vp->Size.x - m : m, bottom ? vp->Size.y - m : m }, ImGuiCond_Always, { right ? 1.f : 0.f, bottom ? 1.f : 0.f });
    ImGui::SetNextWindowSize({ width, 0 }, ImGuiCond_Always);  // 0: the height fits the content
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 18 * s, 12 * s });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 8 * s, 6 * s });
    ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = win->DrawList;
    const ImVec2 wa = win->Pos, wb(win->Pos.x + win->Size.x, win->Pos.y + win->Size.y);
    ImGui::PushClipRect(ImVec2(wa.x, wa.y - 6 * s), wb, false);  // the streak's glow reaches above the box
    look::panel(dl, wa, wb, look::kCounter);
    ImGui::PopClipRect();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float cw = ImGui::GetContentRegionAvail().x;
    // the numbers, right-aligned in their slots, each unit in spaced capitals on its number's baseline
    auto bigNumber = [&](ImFont* f, float x, float slot, float v, const char* unitText) {
        char num[16];
        snprintf(num, sizeof num, "%.0f", v);
        dl->AddText(f, f->FontSize, ImVec2(x + slot - textW(f, num), p.y), look::col(look::kInk), num);
        const float uy = p.y + f->Ascent - sm->Ascent * unit / sm->FontSize;
        look::spacedText(dl, sm, unit, ImVec2(x + slot + 7 * s, uy), unitText, 0.18f, look::kInk2);
    };
    const float baseX = p.x + slotFps + 7 * s + unitFps + 22 * s;
    bigNumber(g_big, p.x, slotFps, g_fpsShown, "FPS");
    if (gen) bigNumber(g_bigThin, baseX, slotBase, g_fpsRendered, "BASE");  // with FG off both numbers are the same
    // the frame time under each number: the shown frames', and the game's own while frame generation runs
    const float my = p.y + g_big->FontSize + 3 * s;
    char ms[24];
    look::msText(ms, sizeof ms, g_fpsShown);
    dl->AddText(sm, sm->FontSize, ImVec2(p.x, my), look::col(look::kInk2), ms);
    if (gen) { look::msText(ms, sizeof ms, g_fpsRendered); dl->AddText(sm, sm->FontSize, ImVec2(baseX, my), look::col(look::kInk2), ms); }
    // the graph, then a fading rule above the lines
    const float gy = my + sm->FontSize + 8 * s, gh = 34 * s, ry = gy + gh + 4 * s + 8 * s;
    drawGraph(dl, ImVec2(p.x, gy), ImVec2(p.x + cw, gy + gh));
    look::fadeRule(dl, p.x, p.x + cw, ry, look::rgba(205, 215, 228, .28f), s);
    ImGui::Dummy(ImVec2(cw, ry + s + 6 * s - p.y - 6 * s));
    ImGui::PushFont(sm);
    if (gen) ImGui::TextColored(kAccents[backendVendor(fg.backend)].hi, "%s", fgLine);  // the generating vendor's colour
    else ImGui::TextColored(look::kInfo, "%s", fgLine);
    if (*g_gpu) ImGui::TextColored(look::kInfo, "%s", g_gpu);
    if (*res) ImGui::TextColored(look::kInfo, "%s", res);
    if (*verLine) ImGui::TextColored(look::kInfo, "%s", verLine);
    ImGui::PopFont();
    ImGui::PushClipRect(wa, wb, false);
    look::brackets(dl, wa, wb, look::kCounter);
    ImGui::PopClipRect();
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// Settings both layouts change, so a change in one shows in the other: the game's scaling mode (GraphicsConfig.ini, read
// once, on the first menu frame) and the frame generation backend (the setting at launch, whether frame generation was
// on at launch, and the menu's choice: 0 Auto, else the backend's id + 1).
struct ScaleState { std::wstring cfg; int ratio; };
ScaleState& scaleState() {
    static ScaleState s = [] {
        ScaleState r{ gameConfig(), 0 };
        r.ratio = r.cfg.empty() ? 0 : GetPrivateProfileIntW(L"BasicSettings", L"RenderingResolutionRatio", 100, r.cfg.c_str());
        return r;
    }();
    return s;
}
struct FgPick { int launchBackend; bool launchEnable; int backend; };
FgPick& fgPick() {
    static FgPick p = [] {
        FgPick r;
        r.launchBackend = iniGet(L"fg", L"backend", -1);
        r.launchEnable = fgSettings().enable != 0;  // FSR's swapchain is only created when FG is on at launch
        r.backend = r.launchBackend >= -1 && r.launchBackend < kBackendCount ? r.launchBackend + 1 : 0;
        return r;
    }();
    return p;
}

// The desktop menu's look, pushed around drawMenu only: applyStyle, which the Steam Deck layout uses, stays as it is.
// ImGui's own frames, ticks, grabs, arrows and nav outline go transparent; menu_look.h draws the look around them.
struct DesktopLook {
    static constexpr int kVars = 11, kColours = 18;
    DesktopLook() {
        const float s = g_scale;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30 * s, 24 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, 6 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * s, 8 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, s);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.f);
        const ImGuiCol clear[] = { ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_CheckMark, ImGuiCol_SliderGrab,
                                   ImGuiCol_SliderGrabActive, ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive, ImGuiCol_NavCursor };
        for (ImGuiCol c : clear) ImGui::PushStyleColor(c, look::kClear);
        ImGui::PushStyleColor(ImGuiCol_Text, look::kInk);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, look::rgba(4, 6, 9, .94f));
        ImGui::PushStyleColor(ImGuiCol_Border, look::kRule);
        ImGui::PushStyleColor(ImGuiCol_Header, look::kSel);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, look::kSel);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, look::kSel);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, look::kDim);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, look::rgba(4, 6, 9, .74f));  // the gutter the panel's fill leaves out
    }
    ~DesktopLook() { ImGui::PopStyleColor(kColours); ImGui::PopStyleVar(kVars); }
    DesktopLook(const DesktopLook&) = delete;
    DesktopLook& operator=(const DesktopLook&) = delete;
};

// ---- the pages both layouts draw (menu_pages.h) ----------------------------------------------------------------------
// The state the page model reads, from the mod's own: the same reads and rules both menus have always used. The text it
// points at (versions) stays valid until the next call.
pages::State fillState() {
    auto missing = [](const wchar_t* f) { return GetFileAttributesW(path(f).c_str()) == INVALID_FILE_ATTRIBUTES; };
    const bool nvidiaGpu = g_vendorId == 0x10DE, intelGpu = g_vendorId == 0x8086;
    static DlssInfo di;
    di = dlssInfo();
    pages::State s;
    s.nvidia = nvidiaGpu;
    s.portable = g_deckOpen;
    const bool activeKnown = di.active >= 0 && di.active < kUpscalerCount;
    s.upscaler = di.upscaler >= 0 && di.upscaler < kUpscalerCount ? di.upscaler : -1;
    // the upscaler wanted: the one chosen, else what Auto runs, else what Auto picks on this GPU
    s.want = s.upscaler >= 0 ? s.upscaler : activeKnown ? di.active : nvidiaGpu ? 0 : intelGpu && !missing(kUpscalers[2].file) ? 2 : 1;
    s.active = activeKnown ? di.active : -1;
    s.upOn = dlssEnabled();
    s.wantMissing = missing(kUpscalers[s.want].file);
    s.dlssVersion = di.version; s.fsrVersion = di.fsrVersion; s.xessVersion = di.xessVersion;
    const ScaleState& ss = scaleState();
    s.ratio = ss.ratio;
    s.live = di.outW ? int(std::lround(100.0 * di.inH / di.outH)) : 0;
    s.upPct = ultraPerformancePct(s.want == 2);  // 0 with XeSS: no game scale value fits it below 50% (xess_rules.h)
    s.xessLowest = xessLowestPct();
    s.xessBelow = s.want == 2 && ((ss.ratio && ss.ratio < xessLowestPct()) || xessInputBelowRange());
    if (s.live) { s.inW = di.inW; s.inH = di.inH; s.outW = di.outW; s.outH = di.outH; }
    s.dispW = di.dispW; s.dispH = di.dispH;
    s.preset = di.preset; s.sharp = di.sharpness; s.gameSharp = gameSharpenGet();
    // frame generation: the backend decides which swapchain the presenter creates at launch, so a change applies at the
    // next launch. Auto resolves as the presenter does (fgPickBackend).
    const FgStatus st = fgStatus();
    const FgSettings fs = fgSettings();
    const FgPick& fp = fgPick();
    s.dlssg = !missing(L"ffxv_dlss\\streamline\\nvngx_dlssg.dll");
    s.fsrFiles = !missing(kUpscalers[1].file);
    s.fsrFgDll = !missing(L"ffxv_dlss\\fidelityfx\\amd_fidelityfx_framegeneration_dx12.dll");
    s.xessFgFiles = xessFgPresent();
    auto resolve = [&](int b) { return fgPickBackend(b, g_vendorId, s.dlssg, s.xessFgFiles); };
    s.running = st.backend >= 0 ? st.backend : resolve(fp.launchBackend);
    s.picked = resolve(fp.backend - 1);
    s.backend = fp.backend; s.launchBackend = fp.launchBackend; s.launchEnable = fp.launchEnable;
    s.fgBackend = st.backend >= 0 && st.backend < kBackendCount ? st.backend : -1;
    s.fgAvailable = st.available; s.generating = st.generating; s.formatRefused = st.formatRefused;
    // FSR or XeSS is ready but FG was off at launch (their swapchains are made only then): it can be turned on for the next
    s.offAtLaunch = !st.available && !fp.launchEnable && !st.formatRefused
                    && ((s.running == 1 && s.fsrFiles && s.fsrFgDll) || (s.running == 2 && s.xessFgFiles));
    s.dynamicSupported = st.dynamicSupported; s.maxGenerated = st.maxGenerated; s.noFrames = st.noFrames;
    s.fgSupport = st.slSupport; s.wine = st.wine;
    s.fgEnable = fs.enable; s.frames = fs.frames; s.target = fs.target; s.limit = fs.limit; s.reflex = fs.reflex; s.antilag = fs.antilag;
    s.dlssgVersion = dlssgVersion(); s.fsrFgVersion = fsrFgVersion(); s.xessFgVersion = xessFgVersion();
    s.shown = g_fpsShown; s.rendered = g_fpsRendered;
    s.al2Shown = al2MenuShown(st.antilag, st.backend); s.al2Ready = st.antilag == kAl2Ready; s.al2Linux = st.antilag == kAl2Wine;
    s.paces = st.backend == 1 || di.active == 1 || di.active == 2;  // the presenter paces frames itself with FSR or XeSS
    // the cap applies through Reflex when Streamline runs, else through the presenter's pacing or the game's VSync
    s.capUsable = st.available || st.backend >= 0 || di.active == 1 || di.active == 2 || st.vsyncPaced;
    s.autoCap = st.autoCap; s.sync = syncName();
    s.counter = g_counter; s.corner = g_corner; s.hud = uiSharp();
    return s;
}

// A list row's choices (short names) and the current one (-1: a value the list does not hold).
int choices(pages::Id id, const pages::State& s, const char* names[8], int* cur) {
    static char scaleNames[5][48], custom[24];
    int n = 0;
    *cur = -1;
    switch (id) {
    case pages::kUpscaler: case pages::kBackend:
        names[n++] = "Auto";
        for (int i = 0; i < 3; ++i) names[n++] = pages::upName(i);
        *cur = id == pages::kUpscaler ? s.upscaler + 1 : std::clamp(s.backend, 0, 3);
        break;
    case pages::kScaling: {
        pages::Scale m[5];
        n = pages::scaleModes(s, m);
        for (int i = 0; i < n; ++i) {
            snprintf(scaleNames[i], sizeof scaleNames[i], "%s \xC2\xB7 %d%%", m[i].name, m[i].pct);
            names[i] = scaleNames[i];
            if (m[i].pct == s.ratio) *cur = i;
        }
        break;
    }
    case pages::kDlssModel:
        for (; n < 7; ++n) names[n] = pages::kPresetNames[n];
        *cur = pages::presetIndex(s.preset);
        if (*cur < 0) { snprintf(custom, sizeof custom, "Model %c \xC2\xB7 ini", char('A' + s.preset - 1)); names[n] = custom; *cur = n++; }
        break;
    case pages::kFgMode: {
        static char modeNames[6][8];
        if (s.fgBackend != 0) { names[n++] = "2\xC3\x97"; *cur = 0; break; }  // FSR and XeSS generate 2x
        if (s.dynamicSupported) { names[n++] = "Dynamic"; if (s.frames == 0) *cur = 0; }
        for (unsigned k = 1; k <= std::max(1u, s.maxGenerated) && k <= 5; ++k) {
            snprintf(modeNames[k], sizeof modeNames[k], "%u\xC3\x97", k + 1);
            if (s.frames == int(k)) *cur = n;
            names[n++] = modeNames[k];
        }
        if (*cur < 0) *cur = 0;
        break;
    }
    case pages::kLowLatency: {
        const pages::Latency l = pages::latency(s);
        if (l == pages::kReflex) { for (; n < 3; ++n) names[n] = pages::kReflexNames[n]; *cur = std::clamp(s.reflex, 0, 2); }
        else { names[n++] = "Off"; names[n++] = "On"; *cur = l == pages::kXell ? s.reflex > 0 : s.al2Ready && s.antilag; }
        break;
    }
    case pages::kFrameCap: names[n++] = "Off"; names[n++] = "Auto"; names[n++] = "Custom"; *cur = pages::capMode(s); break;
    case pages::kPosition: for (; n < 4; ++n) names[n] = pages::kCorners[n]; *cur = std::clamp(s.corner, 0, 3); break;
    default: break;
    }
    return n;
}

// Settings changed from either layout, saved through the same setters and ini keys the menu has always used.
void saveFg(const FgSettings& fs) {
    fgApply(fs);
    iniSet(L"fg", L"enable", fs.enable); iniSet(L"fg", L"frames", fs.frames); iniSet(L"fg", L"target", fs.target); iniSet(L"fg", L"limit", fs.limit);
    iniSet(L"fg", L"reflex", fs.reflex); iniSet(L"fg", L"antilag", fs.antilag);
}
void setScale(int pct) {
    ScaleState& ss = scaleState();
    if (!ss.ratio || pct == ss.ratio) return;
    ss.ratio = pct;
    WritePrivateProfileStringW(L"BasicSettings", L"RenderingResolutionRatio", std::to_wstring(ss.ratio).c_str(), ss.cfg.c_str());
}
// A list row set to choice i (from choices).
void pick(pages::Id id, const pages::State& s, int i) {
    FgSettings fs = fgSettings();
    switch (id) {
    case pages::kUpscaler: dlssSetUpscaler(i - 1); iniSet(L"dlss", L"upscaler", i - 1); break;
    case pages::kScaling: { pages::Scale m[5]; if (i >= 0 && i < pages::scaleModes(s, m)) setScale(m[i].pct); break; }
    case pages::kDlssModel: if (i >= 0 && i < 7) { iniSet(L"dlss", L"preset", pages::kPresets[i]); dlssSetPreset(pages::kPresets[i]); } break;
    case pages::kBackend: fgPick().backend = i; iniSet(L"fg", L"backend", i - 1); break;
    case pages::kFgMode: if (s.fgBackend == 0) { fs.frames = s.dynamicSupported ? i : i + 1; saveFg(fs); } break;
    case pages::kLowLatency: {
        const pages::Latency l = pages::latency(s);
        if (l == pages::kReflex) fs.reflex = i;
        else if (l == pages::kXell) { if (i == (fs.reflex > 0 ? 1 : 0)) return; fs.reflex = i; }  // the ini's Boost is kept until changed
        else { if (!s.al2Ready || i == (fs.antilag ? 1 : 0)) return; fs.antilag = i; }
        saveFg(fs);
        break;
    }
    case pages::kFrameCap: fs.limit = i == 0 ? 0 : i == 1 ? -1 : std::max(fgStatus().autoCap, 60); saveFg(fs); break;
    case pages::kPosition: g_corner = i; iniSet(L"overlay", L"corner", g_corner); break;
    default: break;
    }
}
// A list row stepped by d (-1 / +1), wrapping; a value the list does not hold steps to the first (right) or last (left).
void stepList(pages::Id id, const pages::State& s, int d) {
    const char* names[8]; int cur;
    const int n = choices(id, s, names, &cur);
    if (n < 2 || !d) return;
    pick(id, s, cur < 0 ? (d > 0 ? 0 : n - 1) : look::stepWrap(cur, d, n));
}
// A toggle row flipped.
void flipToggle(pages::Id id) {
    switch (id) {
    case pages::kUpEnabled: dlssToggle(); break;
    case pages::kFgEnabled: { FgSettings fs = fgSettings(); fs.enable = fs.enable ? 0 : 1; saveFg(fs); break; }
    case pages::kCounter: g_counter = !g_counter; iniSet(L"overlay", L"enable", g_counter); break;
    case pages::kHud: { const bool on = !uiSharp(); uiSetSharp(on); iniSet(L"ui", L"sharp", on); break; }
    default: break;
    }
}
// A slider row's range, step and value, and its setter.
struct SliderSpec { int lo, hi, step, value; const char* fmt; };
SliderSpec sliderSpec(pages::Id id, const pages::State& s) {
    switch (id) {
    case pages::kSharpening: return { 0, 100, 1, s.sharp, s.sharp ? "%d%%" : "Off" };
    case pages::kGameSharpening: return { 0, 200, 5, s.gameSharp, s.gameSharp ? "%d%%" : "Off" };
    case pages::kTarget: return { 60, 480, 5, s.target, "%d fps" };
    default: return { 30, 480, 5, s.limit > 0 ? s.limit : std::max(s.autoCap, 60), "%d fps" };
    }
}
void setSlider(pages::Id id, int v) {
    FgSettings fs = fgSettings();
    switch (id) {
    case pages::kSharpening: dlssSetSharpness(v); iniSet(L"dlss", L"sharpness", v); break;
    case pages::kGameSharpening: gameSharpenSet(v); iniSet(L"dlss", L"game_sharpening", v); break;
    case pages::kTarget: fs.target = v; saveFg(fs); break;
    case pages::kCap: if (fs.limit > 0) { fs.limit = v; saveFg(fs); } break;
    default: break;
    }
}
// The layout button's pick: the layout is saved to [overlay] layout (2 Desktop, 1 Portable) and changes on the next frame.
void switchLayout(bool portable) {
    if (!g_deck26) return;  // the Portable fonts did not load
    g_layout = portable ? 1 : 2;
    iniSet(L"overlay", L"layout", g_layout);
    g_swap.flip(portable, ImGui::GetTime(), g_deckOpen);
}

ImVec4 toneColour(pages::Tone t) {
    return t == pages::kAmber ? kAmber : t == pages::kGreen ? look::kGo : t == pages::kDim ? look::kDim : look::kInk2;
}
// The help box's text at p, `w` wide: each paragraph wrapped, one line every `step`, three lines at most. A text that
// needs more is cut and logged once.
void drawHelp(ImDrawList* dl, ImFont* f, float size, ImVec2 p, float w, float step, pages::Id id, const pages::Help& h) {
    static bool logged[pages::kIdCount];
    int line = 0;
    for (int i = 0; i < h.n; ++i) {
        const int left = pages::kHelpLines - line;
        const int n = look::wrapText(left > 0 ? dl : nullptr, f, size, ImVec2(p.x, p.y + line * step), w, step, h.line[i].text, toneColour(h.line[i].tone), std::max(0, left));
        line += n;
    }
    if (line > pages::kHelpLines && !logged[id]) { logged[id] = true; logf("menu: help text for row %d needs %d lines", int(id), line); }
}
// The status line at p: the upscaler and its version in its vendor's colour, render size → output size, and frame
// generation's state.
void drawStatus(ImDrawList* dl, ImFont* f, float size, ImVec2 p, const pages::Status& st) {
    const float gap = f->CalcTextSizeA(size, FLT_MAX, 0, "   ").x;
    float x = p.x;
    auto put = [&](const char* t, const ImVec4& c) { if (!*t) return; dl->AddText(f, size, ImVec2(x, p.y), look::col(c), t); x += f->CalcTextSizeA(size, FLT_MAX, 0, t).x; };
    put(st.up, st.upVendor >= 0 ? kAccents[st.upVendor].hi : look::kDim);
    if (*st.size) { x += gap; put(st.size, look::kInk2); }
    x += gap;
    put("Frame generation ", look::kDim);
    put(st.fg, st.fgTone == pages::kVendor && st.fgVendor >= 0 ? kAccents[st.fgVendor].hi : toneColour(st.fgTone));
}

// The desktop menu's page and the help box's row (kept while the mouse is off the rows); g_pageTurn: Tab, L1
// or R1 from compose, applied by drawMenu.
int g_deskPage;
pages::Id g_deskHelp = pages::kUpscaler;
int g_pageTurn;

// The desktop menu's fixed slots, in its units (times g_scale) from the window's top: the top bar, the tabs, the rows (a
// heading slot and seven rows), the help box (a hairline and three lines), the status line and the footer. The window
// is always this tall: nothing moves between pages or states.
struct DeskSlots {
    static constexpr float kTop = 24, kBarH = 52, kTabsTop = 82, kTabsH = 32, kRowsTop = 122, kSubH = 26, kPitch = 36;
    static constexpr float kHelpRule = kRowsTop + kSubH + 7 * kPitch + 8, kHelpTop = kHelpRule + 9, kHelpStep = 19;
    static constexpr float kStatusTop = kHelpTop + 3 * kHelpStep + 8, kFootRule = kStatusTop + 24, kHintsTop = kFootRule + 9;
    static constexpr float kFineTop = kHintsTop + 26, kHeight = kFineTop + 2 * 16.5f + 22;
};

void drawMenu() {
    using Z = DeskSlots;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({ vp->Size.x * 0.5f, vp->Size.y * 0.5f }, ImGuiCond_Appearing, { 0.5f, 0.5f });
    const float s = g_scale;
    look::ctx().s = s; look::ctx().fontSmall = g_small;
    look::ctx().bob = look::bobX(ImGui::GetTime());  // the glove's sway, once a frame
    ImGui::SetNextWindowSize({ 600 * s, Z::kHeight * s }, ImGuiCond_Always);
    DesktopLook styled;  // popped after End
    ImGui::Begin("Lumenveil", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse);
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = win->DrawList;
    const ImVec2 wa = win->Pos, wb(win->Pos.x + win->Size.x, win->Pos.y + win->Size.y);
    ImGui::PushClipRect(ImVec2(wa.x, wa.y - 6 * s), wb, false);  // the streak's glow reaches above the panel
    look::panel(dl, wa, wb, look::kMenu);
    ImGui::PopClipRect();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * s, 8 * s));  // the lists' padding; the menu's is set
    // the first row takes the focus when the menu opens (not the layout button above it, unless the layout has just
    // changed: then the button keeps it) and on every page change
    const bool padOpened = g_padOpened, opening = ImGui::IsWindowAppearing() && !g_focusPortable;
    if (g_padOpened) { pad::focusFirst(); g_padOpened = false; }
    const float x0 = win->Pos.x + 30 * s, cw = 540 * s, flagW = 22 * s;
    const float labelW = 30 * s + 212 * s;  // the widgets' column, from the window's edge
    const pages::State st = fillState();

    // ---- top bar: the logo left, the layout button right
    if (g_logo || g_logo11) {
        const float h = 40 * s, w = h * g_logoW / g_logoH;
        ImGui::SetCursorPos(ImVec2(30 * s, (Z::kTop + (Z::kBarH - h) * 0.5f) * s));
        ImGui::Image(g_native ? (ImTextureID)(uintptr_t)g_logo11 : (ImTextureID)gpuAt(g_srvHeap, 2).ptr, { w, h });
    }
    bool buttonSel = false;
    {
        const float bw = 230 * s, bh = 34 * s;
        ImGui::SetCursorScreenPos(ImVec2(x0 + cw - bw, win->Pos.y + (Z::kTop + (Z::kBarH - 34) * 0.5f) * s));
        const int want = look::layoutButton("##layout", false, g_deck26 != nullptr, bw, bh);
        buttonSel = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && GImGui->NavCursorVisible);
        look::takeFocus(g_focusPortable, g_padUsed);
        if (want >= 0) switchLayout(want == 1);
    }

    // ---- tabs: a click, Tab or L1 / R1 (from compose) change page; a new page starts on its first row
    static const char* const kPageNames[] = { "Upscaling", "Frame generation", "Overlay" };
    ImGui::SetCursorScreenPos(ImVec2(x0, win->Pos.y + Z::kTabsTop * s));
    int to = look::tabs(g_deskPage, kPageNames, 3, cw, Z::kTabsH * s);
    if (g_pageTurn) { to = pages::turn(g_deskPage, g_pageTurn); g_pageTurn = 0; }
    const bool turned = to != g_deskPage;
    if (turned) g_deskPage = to;

    // ---- rows, in fixed slots
    pages::Row rows[pages::kMaxRows]; int sub;
    const int n = pages::rows(g_deskPage, st, rows, &sub);
    if (n && (turned || pages::pageOf(g_deskHelp) != g_deskPage)) g_deskHelp = rows[0].id;
    const float boxW = 76 * s;
    bool typing = false;
    float y = Z::kRowsTop;
    for (int i = 0; i < n; ++i) {
        const pages::Row& r = rows[i];
        if (i == sub) {  // the heading slot: spaced capitals at its foot, then a rule fading out
            const float hy = win->Pos.y + (y + Z::kSubH - 16) * s, tw = look::spacedText(dl, g_small, 12 * s, ImVec2(x0, hy), "LATENCY", 0.22f, look::kInk2);
            look::fadeRule(dl, x0 + tw + 12 * s, x0 + cw, std::floor(hy + 12 * s * 0.55f), look::kRule);
            y += Z::kSubH;
        }
        ImGui::PushID(int(r.id));
        ImGui::SetCursorPos(ImVec2(30 * s, (y + (Z::kPitch - 28) * 0.5f) * s));
        look::row();
        if (look::ctx().rowSel) g_deskHelp = r.id;
        ImGui::AlignTextToFramePadding();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 22 * s);
        ImGui::BeginDisabled(r.dim);
        ImGui::TextUnformatted(r.label);
        ImGui::SameLine(labelW);
        look::ctx().widgetX = ImGui::GetCursorScreenPos().x;
        ImGui::SetNextItemWidth(-flagW);
        const pages::Value v = pages::value(r.id, st);
        if (r.kind == pages::kList) {
            const char* names[8]; int cur;
            const int count = choices(r.id, st, names, &cur);
            int c = cur;
            if (r.id == pages::kScaling) {  // a value not in the list steps to its first or last entry
                bool open = false;
                const int d = look::optionFrame("##v", v.main, &open, v.rest);
                if (open) {
                    for (int k = 0; k < count; ++k) {
                        if (ImGui::Selectable(names[k], k == cur) && k != cur) pick(r.id, st, k);
                        if (k == cur) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (d) stepList(r.id, st, d);
            } else if (look::option("##v", &c, names, count, v.main, v.rest) && c != cur) pick(r.id, st, c);
        } else if (r.kind == pages::kToggle) {
            bool on = !std::strcmp(v.main, "On");
            bool flip = ImGui::Checkbox("##v", &on);
            if (!r.dim) flip |= pad::padTick(on);
            look::toggleLook(on);
            if (flip) flipToggle(r.id);
        } else {
            const SliderSpec sp = sliderSpec(r.id, st);
            int val = sp.value;
            if (pad::typedSlider("##v", val, sp.lo, sp.hi, sp.step, sp.fmt, boxW, &typing)) setSlider(r.id, val);
            look::sliderLook(val, sp.lo, sp.hi);
        }
        if ((turned || opening || padOpened) && i == 0) look::focusItem(GImGui->NavCursorVisible || padOpened);  // the first row
        ImGui::EndDisabled();
        const pages::Help h = pages::help(r.id, st);
        if (h.flag != pages::kNoFlag)
            look::diamond(dl, ImVec2(x0 + cw - flagW * 0.5f, win->Pos.y + (y + Z::kPitch * 0.5f) * s), 4 * s, h.flag == pages::kWarnFlag ? kAmber : look::kGo, 0);
        ImGui::PopID();
        y += Z::kPitch;
    }
    g_typing = typing;  // after the last box

    // ---- the help box: the selected row's, or the layouts' while the button is selected
    const pages::Id helpId = buttonSel ? pages::kLayout : g_deskHelp;
    const float helpY = win->Pos.y + Z::kHelpRule * s;
    dl->AddRectFilled(ImVec2(x0, helpY), ImVec2(x0 + cw, helpY + s), look::col(look::rgba(205, 215, 228, .12f)));
    drawHelp(dl, g_small, 13 * s, ImVec2(x0 + 22 * s, win->Pos.y + Z::kHelpTop * s), pages::kDeskFit.helpW * s, Z::kHelpStep * s, helpId, pages::help(helpId, st));
    // ---- the status line
    drawStatus(dl, g_small, pages::kDeskFit.statusPx * s, ImVec2(x0 + 22 * s, win->Pos.y + Z::kStatusTop * s), pages::status(st));

    // ---- footer: a faint line, the key hints with the version right, then the trademarks
    dl->AddRectFilled(ImVec2(x0, win->Pos.y + Z::kFootRule * s), ImVec2(x0 + cw, win->Pos.y + (Z::kFootRule + 1) * s), look::col(look::rgba(205, 215, 228, .12f)));
    ImGui::SetCursorScreenPos(ImVec2(x0, win->Pos.y + Z::kHintsTop * s));
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
    g_padBusy = popup || ImGui::IsAnyItemActive();  // B backs out of the list or slider instead of closing the menu
    ImGui::PushClipRect(ImVec2(x0, wa.y), ImVec2(x0 + cw - 110 * s, wb.y), true);  // the hints stop short of the version
    if (g_padUsed) pad::drawHints(popup, g_padBusy, g_scale);  // in the footer line's own height
    else {
        const float h = ImGui::GetTextLineHeight();
        const ImVec2 k = ImGui::GetCursorScreenPos();
        float x = look::keyHint(dl, k.x, k.y, h, "\xE2\x86\x91\xE2\x86\x93", "Select") + 18 * s;
        x = look::keyHint(dl, x, k.y, h, "\xE2\x86\x90\xE2\x86\x92", "Change") + 18 * s;
        x = look::keyHint(dl, x, k.y, h, "Tab", "Page") + 18 * s;
        look::keyHint(dl, x, k.y, h, "Ins", "Close");
    }
    ImGui::PopClipRect();
    {
        const char* ver = "Lumenveil " LUMENVEIL_VERSION;
        const float vw = g_small->CalcTextSizeA(g_small->FontSize, FLT_MAX, 0, ver).x, h = ImGui::GetTextLineHeight();
        dl->AddText(g_small, g_small->FontSize, ImVec2(x0 + cw - vw, win->Pos.y + Z::kHintsTop * s + (h - g_small->FontSize) * 0.5f), look::col(look::kDim), ver);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, win->Pos.y + Z::kFineTop * s));
    ImGui::PushFont(g_small);
    ImGui::PushStyleColor(ImGuiCol_Text, look::kDim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
    look::fineText("Uses NVIDIA DLSS, NVIDIA Reflex and NVIDIA Streamline, AMD FSR and Intel XeSS. NVIDIA, DLSS and "
                   "Reflex are trademarks of NVIDIA Corporation, FSR of AMD and XeSS of Intel.", 11);  // smaller than the help
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopStyleVar();  // the lists' padding
    ImGui::PushClipRect(wa, wb, false);
    look::brackets(dl, wa, wb, look::kMenu);
    ImGui::PopClipRect();
    ImGui::End();
}

// The Portable layout (menu_deck.h): the same three pages, rows, help box and status line as the desktop menu, in its
// own bigger sizes, moved by the controller's focus and touch; saved through the same setters and keys as drawMenu.
void drawDeckMenu() {
    deck::Look L = deck::Look::at(float(g_w), float(g_h), g_deck26, g_deck24, g_deck19);
    deck::Nav& nav = g_deckNav;
    deck::Typing& typing = g_deckTyping;
    g_padOpened = false;  // the focus is the layout's own
    look::ctx().bob = look::bobX(ImGui::GetTime());  // the glove's sway, once a frame
    const pages::State st = fillState();
    pages::Row rows[pages::kMaxRows]; int sub;
    int n = pages::rows(nav.page, st, rows, &sub);
    // the controller: up / down move the focus (up from the first row to the layout button), left / right change the
    // value (repeating only on a slider), A, L1 / R1 change page; B leaves a number box being typed in (pollToggle closes
    // the menu on B otherwise)
    const uint16_t edges = g_padView.edges;
    if (typing.id && (edges & pad::kB)) deck::leaveTyping(typing);
    if (!typing.id) {
        if (edges & pad::kL1) nav.turn(-1);
        if (edges & pad::kR1) nav.turn(1);
        if (edges & (pad::kL1 | pad::kR1)) n = pages::rows(nav.page, st, rows, &sub);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp)) nav.move(n, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown)) nav.move(n, 1);
    }
    if (nav.focus[nav.page] >= n) nav.focus[nav.page] = n - 1;  // a row that went away
    const int lrOnce = typing.id ? 0 : ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) ? -1
                     : ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) ? 1 : 0;
    const int lrRepeat = typing.id ? 0 : pad::padLeftRight();
    const bool a = !typing.id && (edges & pad::kA);

    deck::begin(L);
    if (g_logo || g_logo11)
        deck::logo(L, g_native ? (ImTextureID)(uintptr_t)g_logo11 : (ImTextureID)gpuAt(g_srvHeap, 2).ptr, g_logoW / g_logoH);
    const int want = deck::layoutButton(L, true, nav.focus[nav.page] == deck::kButton, lrOnce, a);
    if (want >= 0) switchLayout(want == 1);
    const int to = deck::tabs(L, nav.page, nullptr);
    if (to != nav.page) { deck::leaveTyping(typing); nav.show(to); n = pages::rows(nav.page, st, rows, &sub); }

    L.y = deck::kBodyTop;
    const int page = nav.page;
    {
        for (int i = 0; i < n; ++i) {
            const pages::Row& r = rows[i];
            if (i == sub) deck::subHeading(L, "Latency");
            const float rowY = L.y;
            deck::RowIn in;
            in.focused = i == nav.focus[page];
            in.dim = r.dim;
            if (in.focused && !r.dim) { in.lr = r.kind == pages::kSlider ? lrRepeat : lrOnce; in.a = a; }
            const pages::Value v = pages::value(r.id, st);
            char id[16]; snprintf(id, sizeof id, "##r%d", int(r.id));
            deck::RowOut o;
            if (r.kind == pages::kList) {
                o = deck::cycler(L, id, r.label, v.main, in, v.rest);
                if (o.step) stepList(r.id, st, o.step);
            } else if (r.kind == pages::kToggle) {
                o = deck::toggle(L, id, r.label, !std::strcmp(v.main, "On"), in);
                if (o.step) flipToggle(r.id);
            } else {
                const SliderSpec sp = sliderSpec(r.id, st);
                int val = sp.value;
                const bool dim = r.dim || (r.id == pages::kCap && st.limit <= 0);
                in.dim = dim;
                if (dim) in.lr = 0, in.a = false;
                if (deck::slider(L, id, r.label, val, sp.lo, sp.hi, std::max(sp.step, 5), r.id == pages::kTarget || r.id == pages::kCap ? " fps" : "%", in, typing, &o))
                    setSlider(r.id, val);
            }
            if (o.press || o.focus) nav.focus[page] = i;  // a tap on a dimmed row focuses it: its help says why
            const pages::Help h = pages::help(r.id, st);
            if (h.flag != pages::kNoFlag) deck::flag(L, rowY, h.flag == pages::kWarnFlag ? deck::kAmber : deck::kGreen);
        }
    }
    // the help box: the focused row's, or the layouts' on the button
    const int focus = nav.focus[page];
    const pages::Id helpId = focus == deck::kButton || n == 0 ? pages::kLayout : rows[std::clamp(focus, 0, n - 1)].id;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(L.p(30, deck::kHelpRule), L.p(deck::kPanelW - 30, deck::kHelpRule + 1), deck::col(ImVec4(205 / 255.f, 215 / 255.f, 228 / 255.f, .12f)));
    drawHelp(dl, L.caption, deck::kHelpPx * L.k, L.p(deck::kRowL + 48, deck::kHelpTop), pages::kDeckFit.helpW * L.k, deck::kHelpLine * L.k, helpId, pages::help(helpId, st));
    drawStatus(dl, L.caption, pages::kDeckFit.statusPx * L.k, L.p(deck::kRowL + 48, deck::kStatusTop), pages::status(st));
    deck::footer(L, "Lumenveil " LUMENVEIL_VERSION, typing.id != 0);
    deck::end(L);
    g_padBusy = g_typing = typing.id != 0;  // B and Esc leave the number box before they close the menu
}

// The menu logo: resource 101 of this DLL (a PNG, src/res/logo.png), decoded with WIC and uploaded once into SRV slot 2
// on the presenter's queue. Without it the menu simply has no logo.
void loadLogo() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&loadLogo, &self);
    HRSRC res = self ? FindResourceW(self, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10)) : nullptr;  // 10 = RT_RCDATA
    HGLOBAL mem = res ? LoadResource(self, res) : nullptr;
    const void* png = mem ? LockResource(mem) : nullptr;
    const DWORD size = res ? SizeofResource(self, res) : 0;
    std::vector<unsigned char> px;
    UINT w = 0, h = 0;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr; IWICStream* s = nullptr; IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    HRESULT hr = png && size ? CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)) : E_FAIL;
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&s);
    if (SUCCEEDED(hr)) hr = s->InitializeFromMemory((BYTE*)png, size);
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &fr);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&cv);
    if (SUCCEEDED(hr)) hr = cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = cv->GetSize(&w, &h);
    if (SUCCEEDED(hr) && (!w || !h || w > 4096 || h > 4096)) hr = E_FAIL;
    if (SUCCEEDED(hr)) { px.resize(size_t(w) * h * 4); hr = cv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()); }
    release(cv); release(fr); release(dec); release(s); release(wic);
    if (co == S_OK || co == S_FALSE) CoUninitialize();
    if (FAILED(hr)) { logf("menu: logo not loaded %08x", hr); return; }
    if (g_native) {  // an ordinary D3D11 texture, straight-alpha RGBA8 like ImGui's font texture
        D3D11_TEXTURE2D_DESC td = { w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        const D3D11_SUBRESOURCE_DATA init = { px.data(), w * 4, 0 };
        ID3D11Texture2D* t = nullptr;
        hr = g_dev11->CreateTexture2D(&td, &init, &t);
        if (SUCCEEDED(hr)) hr = g_dev11->CreateShaderResourceView(t, nullptr, &g_logo11);
        release(t);
        if (FAILED(hr)) { logf("menu: logo upload failed %08x", hr); return; }
        g_logoW = float(w); g_logoH = float(h);
        logf("menu: logo %ux%u", w, h);
        return;
    }

    // straight-alpha RGBA8, drawn by ImGui like its font texture
    const UINT pitch = (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, up = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64(pitch) * h, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource* buf = nullptr;
    hr = g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_logo));
    if (SUCCEEDED(hr)) hr = g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
    void* map = nullptr;
    if (SUCCEEDED(hr)) hr = buf->Map(0, nullptr, &map);
    if (SUCCEEDED(hr)) {
        for (UINT y = 0; y < h; ++y) memcpy((BYTE*)map + size_t(y) * pitch, px.data() + size_t(y) * w * 4, size_t(w) * 4);
        buf->Unmap(0, nullptr);
    }
    if (SUCCEEDED(hr)) hr = g_alloc[0]->Reset();
    if (SUCCEEDED(hr)) hr = g_cl->Reset(g_alloc[0], nullptr);
    bool submitted = false;
    if (SUCCEEDED(hr)) {
        D3D12_TEXTURE_COPY_LOCATION dst = { g_logo, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, src = { buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.SubresourceIndex = 0;
        src.PlacedFootprint = { 0, { DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, pitch } };
        g_cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(g_logo, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        hr = g_cl->Close();
        if (SUCCEEDED(hr)) {
            ID3D12CommandList* cl = g_cl;
            g_queue->ExecuteCommandLists(1, &cl);
            g_queue->Signal(g_fence, g_allocDone[0] = ++g_fenceVal);  // the first menu frame on allocator 0 waits for it too
            submitted = true;
            if (g_fence->GetCompletedValue() < g_fenceVal) { g_fence->SetEventOnCompletion(g_fenceVal, g_event); WaitForSingleObject(g_event, 2000); }
        }
    }
    if (submitted && g_fence->GetCompletedValue() < g_fenceVal) {  // the GPU may still read buf and write g_logo: keep both
        logf("menu: logo upload still running after 2 s, no logo");
        g_logo = nullptr;
        return;
    }
    release(buf);
    if (FAILED(hr)) { logf("menu: logo upload failed %08x", hr); release(g_logo); return; }
    g_dev->CreateShaderResourceView(g_logo, nullptr, cpuAt(g_srvHeap, 2, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    g_logoW = float(w); g_logoH = float(h);
    logf("menu: logo %ux%u", w, h);
}

// Every way the menu closes (Insert, Esc, B, the stick combo): buttons held now stay hidden from the game until they are
// released, so the press that closed the menu does not also reach the game.
void closeMenu() {
    g_pad.latchAll();
    g_keyLatch.latchAll();
    g_open = false;
    g_padUsed = false;
    g_padRelease = true;
    g_typing = false;
}

void pollToggle() {
    static bool insDown, escDown;
    static pad::ComboHold combo;
    const bool focused = GetForegroundWindow() == g_wnd;  // the keys are global; only react while the game is in front
    const bool ins = focused && (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0, esc = focused && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const ULONGLONG tick = GetTickCount64();
    g_padView = g_pad.merge(tick);
    if (g_emptySlots.logDue(tick)) {
        const auto& e = g_emptySlots;
        logf("menu: controller slots in 30 s: XInput asked %u/%u/%u/%u, answered empty by the mod %u/%u/%u/%u",
             e.asked[0].load(), e.asked[1].load(), e.asked[2].load(), e.asked[3].load(),
             e.cached[0].load(), e.cached[1].load(), e.cached[2].load(), e.cached[3].load());
    }
    const bool stickCombo = combo.update(focused && g_padView.comboDown, tick);  // both sticks held in for half a second
    const bool bClose = g_open && !g_padBusy && (g_padView.edges & pad::kB);
    if ((ins && !insDown) || (esc && !escDown && g_open && !g_typing) || stickCombo || bClose) {
        if (g_open) closeMenu();
        else {
            g_open = true;
            g_deckOpen = g_deck26 && deck::deckLayoutOn(g_layout, g_isDeck);
            g_deckTyping = {};
            g_swap = {}; g_focusPortable = false;  // a change still pending when the menu closed is dropped
            g_mouse = { g_w * 0.5f, g_h * 0.5f };
            if (stickCombo) g_padUsed = g_padOpened = true;
        }
        std::lock_guard<std::mutex> lk(g_inMx);
        g_dx = g_dy = g_wheel = 0; g_btns = {};
        g_inStats = {}; g_inStatsTick = GetTickCount64();
    }
    insDown = ins; escDown = esc;
}

void feedMouse(ImGuiIO& io) {
    static bool sent[3];                    // the button states last queued for the menu
    static unsigned osDownFrames, pressesSent;
    if (!g_open) { io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); osDownFrames = pressesSent = 0; sent[0] = sent[1] = sent[2] = false; return; }
    long dx, dy, wheel; MouseButtons b; ULONGLONG tick, now; InputStats st = {};
    bool logNow;
    {
        std::lock_guard<std::mutex> lk(g_inMx);
        dx = g_dx; dy = g_dy; wheel = g_wheel; tick = g_diMouseTick; g_dx = g_dy = g_wheel = 0;
        now = GetTickCount64();  // after the copy: a read stamped later would make tick newer than now
        b = g_btns.take();
        logNow = now - g_inStatsTick >= 2000;
        if (logNow) { st = g_inStats; g_inStats = {}; g_inStatsTick = now; }
    }
    const bool di = now - tick < 1000;
    if (di) {  // the game reads the mouse through DirectInput: follow its deltas
        g_mouse.x = std::clamp(g_mouse.x + dx * g_scale, 0.f, float(g_w - 1));
        g_mouse.y = std::clamp(g_mouse.y + dy * g_scale, 0.f, float(g_h - 1));
    } else {                               // otherwise the OS cursor, scaled from the client area to the back buffer
        POINT p; RECT r;
        if (GetCursorPos(&p) && ScreenToClient(g_wnd, &p) && GetClientRect(g_wnd, &r) && r.right && r.bottom)
            g_mouse = { p.x * float(g_w) / r.right, p.y * float(g_h) / r.bottom };
    }
    const bool os[3] = { (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0, false };
    io.AddMousePosEvent(g_mouse.x, g_mouse.y);
    bool leftPress = false;
    for (int i = 0; i < 3; ++i) {
        osDownFrames += os[i];
        const ButtonStates s = buttonEvents(di, b.down[i], b.hit[i], os[i]);
        for (int k = 0; k < s.n; ++k) {
            if (s.state[k] && !sent[i]) ++pressesSent;
            if (s.state[k] && !sent[i] && i == 0) leftPress = true;
            sent[i] = s.state[k];
            io.AddMouseButtonEvent(i, s.state[k]);
        }
    }
    if (wheel) io.AddMouseWheelEvent(0, wheel / 120.f);
    if (leftPress && g_deckOpen) {  // where a press lands in the Steam Deck layout: a touchscreen tap arrives as a left click
        POINT c = {}; RECT r = {};
        const bool read = GetCursorPos(&c) && ScreenToClient(g_wnd, &c) && GetClientRect(g_wnd, &r);
        logf("menu: Deck layout press at %.0f,%.0f on the menu; OS cursor %ld,%ld in a %ldx%ld window%s; DirectInput delta %ld,%ld; "
             "cursor from %s", g_mouse.x, g_mouse.y, c.x, c.y, r.right, r.bottom, read ? "" : " (not read)", dx, dy, di ? "DirectInput" : "OS");
    }
    if (logNow) {
        logf("menu input: cursor %s; DirectInput mouse: state reads %u of 16 bytes, %u of 20, %u of another size (last %u "
             "bytes); %u buffered reads, %u events, offsets %08x, %u button-down reads; OS button-down frames %u; presses "
             "to the menu %u", di ? "DirectInput" : "OS", st.stateReads16, st.stateReads20, st.otherSizeReads, st.otherSize,
             st.dataReads, st.events,
             st.ofsSeen, st.diDown, osDownFrames, pressesSent);
        osDownFrames = pressesSent = 0;
    }
}
}  // namespace

bool menuInit(ID3D12Device* dev, ID3D12CommandQueue* queue, HWND wnd, UINT height) {
    g_dev = dev; g_queue = queue; g_wnd = wnd;
    g_scale = std::max(1.f, height / 1080.f);
    IDXGIFactory4* f4 = nullptr; IDXGIAdapter1* ad = nullptr; DXGI_ADAPTER_DESC1 ad1 = {};
    if (g_native) {  // the game's D3D11 device's adapter
        IDXGIDevice* dd = nullptr; IDXGIAdapter* a = nullptr;
        if (SUCCEEDED(g_dev11->QueryInterface(IID_PPV_ARGS(&dd))) && SUCCEEDED(dd->GetAdapter(&a))) a->QueryInterface(IID_PPV_ARGS(&ad));
        release(a); release(dd);
    } else if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f4)))) f4->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&ad));
    if (ad && SUCCEEDED(ad->GetDesc1(&ad1))) {
        g_vendorId = ad1.VendorId;
        const wchar_t* n = ad1.Description;
        for (const wchar_t* p : { L"NVIDIA GeForce ", L"NVIDIA " }) if (!wcsncmp(n, p, wcslen(p))) { n += wcslen(p); break; }
        WideCharToMultiByte(CP_UTF8, 0, n, -1, g_gpu, sizeof g_gpu, nullptr, nullptr);
    }
    if (ad) ad->Release();
    if (f4) f4->Release();
    char env[8] = "";
    const DWORD envLen = GetEnvironmentVariableA("SteamDeck", env, sizeof env);
    const char* steamDeck = envLen && envLen < sizeof env ? env : nullptr;
    g_isDeck = deck::isSteamDeck(ad1.VendorId, ad1.DeviceId, steamDeck);
    g_layout = deck::layoutValue(iniGet(L"overlay", L"layout", 0));
    g_deckFontK = height ? height / 800.f : 1.f;
    char why[32];
    snprintf(why, sizeof why, g_isDeck ? "%s" : "SteamDeck %s", g_isDeck ? deck::foundBy(ad1.VendorId, ad1.DeviceId, steamDeck) : steamDeck ? steamDeck : "unset");
    static const char* const kLayoutNames[] = { "Automatic", "Steam Deck", "Desktop" };
    logf("menu: adapter %04x:%04x, Steam Deck %s (%s), layout %s", ad1.VendorId, ad1.DeviceId, g_isDeck ? "found" : "not found", why,
         kLayoutNames[g_layout]);
    g_counter = iniGet(L"overlay", L"enable", 1) != 0;
    g_corner = std::clamp(iniGet(L"overlay", L"corner", 0), 0, 3);
    if (!g_native) {
        D3D12_DESCRIPTOR_HEAP_DESC sd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        D3D12_DESCRIPTOR_HEAP_DESC rd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
        HRESULT hr = dev->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g_srvHeap));
        if (SUCCEEDED(hr)) hr = dev->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&g_rtvHeap));
        for (UINT i = 0; SUCCEEDED(hr) && i < kFrames; ++i) hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]));
        if (SUCCEEDED(hr)) hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_cl));
        if (SUCCEEDED(hr)) hr = g_cl->Close();
        if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        if (FAILED(hr)) { logf("menu: init failed %08x", hr); return false; }
        g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written next to the game
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NavEnableGamepad;
    loadFonts();
    applyStyle();
    if (g_native) {  // one ImGui backend per launch: D3D11 natively, D3D12 on the presenter
        if (!ImGui_ImplDX11_Init(g_dev11, g_ctx11)) { logf("menu: ImGui DX11 init failed"); return false; }
        if (!nativeObjects()) return false;
    } else {
        ImGui_ImplDX12_InitInfo ii;
        ii.Device = dev; ii.CommandQueue = queue; ii.NumFramesInFlight = kFrames;
        ii.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        ii.SrvDescriptorHeap = g_srvHeap;
        ii.LegacySingleSrvCpuDescriptor = cpuAt(g_srvHeap, 0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ii.LegacySingleSrvGpuDescriptor = gpuAt(g_srvHeap, 0);
        if (!ImGui_ImplDX12_Init(&ii)) { logf("menu: ImGui DX12 init failed"); return false; }
    }
    loadLogo();
    hookInput();
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_last); g_statStart = g_last;
    g_ready = true;
    logf("menu: ready (Insert), ui scale %.2f, fonts %s%s", g_scale, g_fontName, g_native ? ", drawn natively on D3D11" : "");
    return true;
}

void menuInitNative(ID3D11Device* dev, HWND wnd, UINT height) {
    static bool tried;  // the game's start-up swap chain recreation attaches again: one init per launch
    if (tried) return;
    tried = true;
    g_native = true;
    g_dev11 = dev;
    dev->GetImmediateContext(&g_ctx11);
    if (FAILED(g_ctx11->QueryInterface(IID_PPV_ARGS(&g_ctx11_1)))) g_ctx11_1 = nullptr;
    menuInit(nullptr, nullptr, wnd, height);
}

void menuReleaseTargets() {
    if (!g_ready) return;
    // the presenter has already waited for its queue to go idle; ours runs on the same queue
    if (g_layered) { fsrFgUi(nullptr); g_layered = false; }  // FSR holds no reference to it
    release(g_tex); release(g_layer);
}

bool menuTargets(IDXGISwapChain3* sc) {
    if (!g_ready) return false;
    DXGI_SWAP_CHAIN_DESC1 d; sc->GetDesc1(&d);
    release(g_tex); release(g_layer);
    g_w = d.Width; g_h = d.Height;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, d.Width, d.Height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_R8G8B8A8_UNORM };
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&g_tex));
    if (FAILED(hr) || !createPso(d.Format)) { logf("menu: targets failed %08x", hr); release(g_tex); return false; }
    g_dev->CreateRenderTargetView(g_tex, nullptr, cpuAt(g_rtvHeap, 0, D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    g_dev->CreateShaderResourceView(g_tex, nullptr, cpuAt(g_srvHeap, 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    td.Format = cv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&g_layer))))
        g_dev->CreateRenderTargetView(g_layer, nullptr, cpuAt(g_rtvHeap, 2, D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    // the presenter's encoding (NVAPI-aware: the game's HDR10 can have the display in PQ with Windows HDR off), re-read
    // every frame
    g_encode = p12OutputEncoding(nullptr);
    logf("menu: targets %ux%u fmt %u, output %s", d.Width, d.Height, d.Format, g_encode == 1 ? "HDR10 (PQ)" : g_encode == 2 ? "scRGB" : "SDR");
    return true;
}

// After the frame copy, before Present: draws the counter and menu over back buffer `index`, or into g_layer for FSR to
// composite. False if nothing was drawn.
bool compose(IDXGISwapChain3* sc, UINT index, bool layer) {
    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    io.DeltaTime = std::max(1e-4f, float(double(now.QuadPart - g_last.QuadPart) / g_freq.QuadPart));
    g_last = now;
    io.DisplaySize = { float(g_w), float(g_h) };
    io.MouseDrawCursor = g_open;
    feedMouse(io);
    static pad::TypingFeed keys;  // for the settings' boxes; every key up once the menu closes or the game is not in front
    const bool front = g_open && GetForegroundWindow() == g_wnd;
    keys.feed(io, [front](int vk) { return front && (GetAsyncKeyState(vk) & 0x8000) != 0; });
    if (g_open) {
        // a layout change lands here, before the controller is fed and the navigation flags are set, so the new layout
        // has its own navigation from its first frame, with its layout button focused
        if (g_swap.due(ImGui::GetTime())) {
            g_deckOpen = g_deck26 && g_swap.want == 1;
            g_swap = {}; g_deckTyping = {};
            g_padBusy = g_typing = false;
            deck::landFocus(g_deckOpen, g_deckNav, g_focusPortable);
        }
        if (g_padView.connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        pad::feedPad(io, g_padView, g_padBusy);
        if (g_padView.edges || g_padView.comboDown || pad::stickPast(g_padView.lx, 1) || pad::stickPast(g_padView.lx, -1) ||
            pad::stickPast(g_padView.ly, 1) || pad::stickPast(g_padView.ly, -1))
            g_padUsed = true;
    } else if (g_padRelease) { pad::releasePad(io); g_padRelease = false; }
    {  // the desktop menu's pages: Tab (never given to ImGui, so its own Tab focus stays off), L1 / R1 or a click on a
       // tab; not while a box is typed in or a list is open. Shift+Tab is Steam's overlay key, so it turns nothing.
        static bool tabDown;
        const bool tab = front && (GetAsyncKeyState(VK_TAB) & 0x8000) != 0, shift = front && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (g_open && !g_deckOpen) {
            int turn = pages::tabStep(tab && !tabDown, shift, g_typing, g_padBusy);
            if (!turn && !g_typing && !g_padBusy) turn = (g_padView.edges & pad::kR1) ? 1 : (g_padView.edges & pad::kL1) ? -1 : 0;
            if (turn) g_pageTurn = turn;
        }
        tabDown = tab;
    }
    // the Steam Deck layout moves its own focus, so ImGui's must not move too; the desktop menu gets it back
    if (g_open && g_deckOpen) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // the desktop menu also moves and changes with the arrow keys (Space and Enter open a list); the Deck layout has its own keys
    if (g_open && g_deckOpen) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (g_native) ImGui_ImplDX11_NewFrame();
    else ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    if (g_counter) drawCounter();
    if (g_open) { if (g_deckOpen) drawDeckMenu(); else drawMenu(); }
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();

    // only the area ImGui drew in is cleared and composited
    D3D12_RECT box = { LONG(g_w), LONG(g_h), 0, 0 };
    for (const ImDrawList* l : dd->CmdLists)
        for (const ImDrawCmd& c : l->CmdBuffer) {
            box.left = std::min(box.left, LONG(c.ClipRect.x)); box.top = std::min(box.top, LONG(c.ClipRect.y));
            box.right = std::max(box.right, LONG(std::ceil(c.ClipRect.z))); box.bottom = std::max(box.bottom, LONG(std::ceil(c.ClipRect.w)));
        }
    box.left = std::max(box.left, 0L); box.top = std::max(box.top, 0L);
    box.right = std::min(box.right, LONG(g_w)); box.bottom = std::min(box.bottom, LONG(g_h));
    if (box.right <= box.left || box.bottom <= box.top) return false;
    if (g_native) { g_nativeBox = box; return true; }  // menuNativeFrame draws it through nativeFrame

    ID3D12Resource* bb = nullptr;
    if (layer) (bb = g_layer)->AddRef();
    else if (FAILED(sc->GetBuffer(index, IID_PPV_ARGS(&bb)))) return false;
    const D3D12_RESOURCE_STATES idle = layer ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_PRESENT;
    const UINT f = g_frame++ % kFrames;
    if (g_fence->GetCompletedValue() < g_allocDone[f]) { g_fence->SetEventOnCompletion(g_allocDone[f], g_event); WaitForSingleObject(g_event, 1000); }
    g_alloc[f]->Reset();
    g_cl->Reset(g_alloc[f], nullptr);
    g_cl->SetDescriptorHeaps(1, &g_srvHeap);

    const D3D12_CPU_DESCRIPTOR_HANDLE texRtv = cpuAt(g_rtvHeap, 0, D3D12_DESCRIPTOR_HEAP_TYPE_RTV), bbRtv = cpuAt(g_rtvHeap, layer ? 2 : 1, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_VIEWPORT vp = { 0, 0, float(g_w), float(g_h), 0, 1 };
    const float clear[4] = {};
    barrier(g_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_cl->ClearRenderTargetView(texRtv, clear, 1, &box);
    g_cl->OMSetRenderTargets(1, &texRtv, FALSE, nullptr);
    g_cl->RSSetViewports(1, &vp);
    ImGui_ImplDX12_RenderDrawData(dd, g_cl);
    barrier(g_tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (!layer) g_dev->CreateRenderTargetView(bb, nullptr, bbRtv);
    barrier(bb, idle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (layer) g_cl->ClearRenderTargetView(bbRtv, clear, 0, nullptr);  // FSR composites all of it
    g_cl->SetGraphicsRootSignature(g_rs);
    g_cl->SetPipelineState(layer ? g_psoLayer : g_pso);
    g_cl->SetGraphicsRootDescriptorTable(0, gpuAt(g_srvHeap, 1));
    g_cl->SetGraphicsRoot32BitConstant(1, g_encode = p12OutputEncoding(nullptr), 0);
    g_cl->RSSetViewports(1, &vp);
    g_cl->RSSetScissorRects(1, &box);
    g_cl->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
    g_cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cl->DrawInstanced(3, 1, 0, 0);
    barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, idle);
    g_cl->Close();
    ID3D12CommandList* cl = g_cl;
    g_queue->ExecuteCommandLists(1, &cl);
    g_queue->Signal(g_fence, g_allocDone[f] = ++g_fenceVal);
    bb->Release();
    return true;
}

void menuFrame(IDXGISwapChain3* sc, UINT index) {
    if (!g_ready || !g_tex) return;
    pollToggle();
    // While FSR generates frames the UI goes to it as a layer, drawn over real and generated frames instead of being
    // interpolated with the scene.
    const bool layer = g_layer && g_psoLayer && p12FsrGenerating();
    // g_padRelease: one ImGui frame after every close even with the counter off, so the menu window misses a frame and
    // reopens on its first setting, and the controller keys are released
    const bool drew = (g_counter || g_open || g_padRelease) && compose(sc, index, layer);
    if (drew && layer) { fsrFgUi(g_layer); g_layered = true; }
    else if (g_layered) { fsrFgUi(nullptr); g_layered = false; }
}

// Natively, before the game's Present: the counter and menu into the game's back buffer. Nothing at all (no buffer
// acquired, nothing bound) when neither is shown.
void menuNativeFrame(IDXGISwapChain* sc) {
    if (!g_ready || !g_native) return;
    pollToggle();
    DXGI_SWAP_CHAIN_DESC d = {};
    if (SUCCEEDED(sc->GetDesc(&d)) && d.BufferDesc.Width && d.BufferDesc.Height) { g_w = d.BufferDesc.Width; g_h = d.BufferDesc.Height; }
    const bool wanted = (g_counter || g_open || g_padRelease) && compose(nullptr, 0, false);
    D3D11FrameOps ops{ sc };
    if (nativeFrame(ops, wanted, g_nativeBuilt) == kNfDrawn) ++g_nativeDrawn;
}
void menuNativeStats(unsigned* drawn, unsigned* acquired) { *drawn = g_nativeDrawn; *acquired = g_nativeAcquired; }
bool g_nativeDrawing;

// After Present: `shown` = frames actually presented for this rendered frame (2 at 2x frame generation).
void inputThreads(DWORD* dinput, DWORD* xinput, unsigned* diPolls) { *dinput = g_diTid; *xinput = g_xiTid; *diPolls = g_diPolls.exchange(0); }

void menuCount(unsigned shown) {
    ++g_rendered; g_shown += shown;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    g_frames.record(double(now.QuadPart) / (g_freq.QuadPart ? g_freq.QuadPart : 1), shown);
    const double dt = double(now.QuadPart - g_statStart.QuadPart) / (g_freq.QuadPart ? g_freq.QuadPart : 1);
    if (dt < 0.25) return;
    g_fpsRendered = float(g_rendered / dt); g_fpsShown = float(g_shown / dt);
    g_rendered = g_shown = 0; g_statStart = now;
}
