// In-game settings menu (Insert) and fps counter, drawn with Dear ImGui by the D3D12 presenter ([d3d12] present=1).
// ImGui renders into an offscreen RGBA8 texture (premultiplied, sRGB values); one fullscreen-triangle pass blends that
// onto the back buffer, encoding for the output (SDR as is, HDR10 to PQ, scRGB to linear). The game's D3D11 state is
// never touched. While the menu is open the game's DirectInput and XInput reads come back empty, and DirectInput mouse
// deltas drive the menu cursor (so it works even when the game holds the mouse exclusively).
#include "common.h"
#include "fg_select.h"
#include "menu_buttons.h"
#include "menu_pad.h"
#include "xess_rules.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#define DIRECTINPUT_VERSION 0x0800
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
ImFont *g_font, *g_big, *g_bigThin;  // g_bigThin: base fps, lighter than the bold output fps
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
pad::PadView g_padView;
bool g_padUsed, g_padOpened, g_padBusy, g_padRelease;
static_assert(pad::kUp == XINPUT_GAMEPAD_DPAD_UP && pad::kDown == XINPUT_GAMEPAD_DPAD_DOWN && pad::kLeft == XINPUT_GAMEPAD_DPAD_LEFT &&
              pad::kRight == XINPUT_GAMEPAD_DPAD_RIGHT && pad::kL3 == XINPUT_GAMEPAD_LEFT_THUMB && pad::kR3 == XINPUT_GAMEPAD_RIGHT_THUMB &&
              pad::kA == XINPUT_GAMEPAD_A && pad::kB == XINPUT_GAMEPAD_B && pad::kStickDeadzone == XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE,
              "menu_pad.h's button bits match XInput's");
LARGE_INTEGER g_freq, g_last;

// settings ([overlay] in ffxv_dlss.ini; DLSS and FG ones live in their own sections)
bool g_counter;
int g_corner;                                   // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right

// fps: rendered = our Present calls, shown = frames Streamline actually presented (includes generated ones)
unsigned g_rendered, g_shown;
LARGE_INTEGER g_statStart;
float g_fpsRendered, g_fpsShown;

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

// The game's settings: Documents\My Games\FINAL FANTASY XV\Steam\<steam id>\savestorage\GraphicsConfig.ini (newest if
// several accounts have one), or "" if none.
std::wstring gameConfig() {
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return {};
    const std::wstring base = std::wstring(docs) + L"\\My Games\\FINAL FANTASY XV\\Steam\\";
    CoTaskMemFree(docs);
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
PFN_GetDeviceState o_GetDeviceState[2];
PFN_GetDeviceData o_GetDeviceData[2];
PFN_XInputGetState o_XInputGetState;
std::atomic<DWORD> g_diTid, g_xiTid;
std::atomic<unsigned> g_diPolls;

bool isMouse(IDirectInputDevice8W* d) {
    DIDEVCAPS c = { sizeof c };
    return SUCCEEDED(d->GetCapabilities(&c)) && GET_DIDEVICE_TYPE(c.dwDevType) == DI8DEVTYPE_MOUSE;
}

HRESULT getState(int v, IDirectInputDevice8W* d, DWORD cb, LPVOID data) {
    HRESULT hr = o_GetDeviceState[v](d, cb, data);
    g_diTid = GetCurrentThreadId(); ++g_diPolls;
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
    if (!(fl & DIGDD_PEEK)) *n = 0;  // events consumed by the menu
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_GetDeviceStateW(IDirectInputDevice8W* d, DWORD cb, LPVOID p) { return getState(0, d, cb, p); }
HRESULT STDMETHODCALLTYPE hk_GetDeviceStateA(IDirectInputDevice8W* d, DWORD cb, LPVOID p) { return getState(1, d, cb, p); }
HRESULT STDMETHODCALLTYPE hk_GetDeviceDataW(IDirectInputDevice8W* d, DWORD cb, LPDIDEVICEOBJECTDATA o, LPDWORD n, DWORD f) { return getData(0, d, cb, o, n, f); }
HRESULT STDMETHODCALLTYPE hk_GetDeviceDataA(IDirectInputDevice8W* d, DWORD cb, LPDIDEVICEOBJECTDATA o, LPDWORD n, DWORD f) { return getData(1, d, cb, o, n, f); }
// The game's controller read: published for the menu without a lock (this is the game's input thread), then blanked
// while the menu is open; while it is closed, a button held when the menu closed stays hidden until released.
DWORD WINAPI hk_XInputGetState(DWORD i, XINPUT_STATE* s) {
    DWORD r = o_XInputGetState(i, s);
    g_xiTid = GetCurrentThreadId();
    if (s) {
        const bool ok = r == ERROR_SUCCESS;
        g_pad.record(i, ok, ok ? s->Gamepad.wButtons : 0, ok ? s->Gamepad.sThumbLX : 0, ok ? s->Gamepad.sThumbLY : 0, GetTickCount64());
        if (ok && g_open) s->Gamepad = {};
        else if (ok) s->Gamepad.wButtons = g_pad.hide(i, s->Gamepad.wButtons);
    }
    return r;
}

// DirectInput's device methods are shared code for every device: hook them through throwaway devices of our own.
void hookInput() {
    using PFN_DI8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    if (auto create = (PFN_DI8Create)GetProcAddress(LoadLibraryW(L"dinput8.dll"), "DirectInput8Create")) {
        const IID* iids[2] = { &IID_IDirectInput8W, &IID_IDirectInput8A };
        void* states[2] = { (void*)hk_GetDeviceStateW, (void*)hk_GetDeviceStateA };
        void* datas[2] = { (void*)hk_GetDeviceDataW, (void*)hk_GetDeviceDataA };
        void* hooked = nullptr;
        for (int v = 0; v < 2; ++v) {
            IDirectInput8W* di = nullptr; IDirectInputDevice8W* dev = nullptr;
            if (SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *iids[v], (void**)&di, nullptr)) &&
                SUCCEEDED(di->CreateDevice(GUID_SysMouse, &dev, nullptr))) {
                void** vt = *(void***)dev;
                if (vt[9] != hooked) {  // A and W may share the code; hook it once
                    MH_CreateHook(vt[9], states[v], (void**)&o_GetDeviceState[v]);
                    MH_CreateHook(vt[10], datas[v], (void**)&o_GetDeviceData[v]);
                    hooked = vt[9];
                } else { o_GetDeviceState[v] = o_GetDeviceState[0]; o_GetDeviceData[v] = o_GetDeviceData[0]; }
            }
            release(dev); release(di);
        }
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

bool createPso(DXGI_FORMAT fmt) {
    if (g_pso && g_psoFmt == fmt) return true;
    release(g_pso);
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(kHlsl, sizeof kHlsl - 1, "menu", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
    if (SUCCEEDED(hr)) hr = D3DCompile(kHlsl, sizeof kHlsl - 1, "menu", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
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

const ImVec4 kGreen(0.62f, 0.90f, 0.20f, 1), kGrey(0.62f, 0.65f, 0.70f, 1), kAmber(1.0f, 0.75f, 0.25f, 1);

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
// Ticks, slider grabs, combo arrows and list selection take the vendor's colour until the end of the scope.
struct AccentScope {
    explicit AccentScope(Vendor v) {
        const ImVec4 a = kAccents[v].base, h = kAccents[v].hi;
        const ImGuiCol tinted[] = { ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive, ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive };
        const float alpha[] = { 0.25f, 0.35f, 0.45f, 0.25f, 0.35f, 0.45f };
        ImGui::PushStyleColor(ImGuiCol_CheckMark, h);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, a);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, h);
        for (int i = 0; i < 6; ++i) ImGui::PushStyleColor(tinted[i], ImVec4(a.x, a.y, a.z, alpha[i]));
    }
    ~AccentScope() { ImGui::PopStyleColor(9); }
    AccentScope(const AccentScope&) = delete;
    AccentScope& operator=(const AccentScope&) = delete;
};

// Upscalers by id (0 DLSS, 1 FSR, 2 XeSS) and frame generation backends by id (0 DLSS-G, 1 FSR, 2 XeSS), one row each. The vendors'
// DLLs are not shipped with the mod: the player copies them in from the vendors' SDKs (the install scripts do it).
struct Upscaler { const char *name, *shortName; Vendor vendor; const wchar_t* file; const char *missing, *tested; };
const Upscaler kUpscalers[] = {
    { "NVIDIA DLSS", "DLSS", kNvidia, L"ffxv_dlss\\nvngx_dlss.dll",
      "nvngx_dlss.dll not found. Download NVIDIA's DLSS SDK (github.com/NVIDIA/DLSS), copy "
      "lib\\Windows_x86_64\\rel\\nvngx_dlss.dll into the game's ffxv_dlss folder and restart the game.", nullptr },
    { "AMD FSR", "FSR", kAmd, L"ffxv_dlss\\fidelityfx\\amd_fidelityfx_loader_dx12.dll",
      "AMD's FSR files not found. Put FidelityFX-SDK-2.4.0.zip in your Downloads folder and run the installer again, or "
      "copy the three amd_fidelityfx_*_dx12.dll files from Kits\\FidelityFX\\signedbin into ffxv_dlss\\fidelityfx. "
      "INSTALL.txt has the download link.",
      "Tested on an NVIDIA GeForce RTX 5090 only, where AMD's files run FSR 3.1. Untested on AMD Radeon cards and with "
      "AMD's FSR 4." },
    { "Intel XeSS", "XeSS", kIntel, L"ffxv_dlss\\xess\\libxess.dll",
      "Intel's XeSS files not found. Put the XeSS SDK 3.0.2 zip in your Downloads folder and run the installer again, or "
      "copy libxess.dll, libxess_fg.dll and libxell.dll from its bin folder into ffxv_dlss\\xess. INSTALL.txt has the "
      "download link.",
      "Tested on an NVIDIA GeForce RTX 5090 only. Untested on Intel Arc." },
};
struct Backend { const char* name; Vendor vendor; const char* tested; };
const Backend kBackends[] = {
    { "NVIDIA DLSS Frame Generation", kNvidia, nullptr },
    { "AMD FSR Frame Generation", kAmd, kUpscalers[1].tested },
    { "Intel XeSS Frame Generation", kIntel, kUpscalers[2].tested },
};
constexpr int kUpscalerCount = IM_ARRAYSIZE(kUpscalers), kBackendCount = IM_ARRAYSIZE(kBackends);
Vendor backendVendor(int b) { return b >= 0 && b < kBackendCount ? kBackends[b].vendor : kNeutral; }
const char* syncName() { return g_vendorId == 0x1002 ? "FreeSync" : g_vendorId == 0x8086 ? "Adaptive Sync" : "G-Sync"; }

// A "tested on" note: grey text with a thin bar in the vendor's colour on its left.
void testedNote(const char* text, Vendor v) {
    const float bar = 2 * g_scale, gap = 8 * g_scale;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    ImGui::Indent(bar + gap);
    ImGui::TextDisabled("%s", text);
    ImGui::Unindent(bar + gap);
    ImGui::GetWindowDrawList()->AddRectFilled(top, { top.x + bar, ImGui::GetItemRectMax().y }, ImGui::GetColorU32(kAccents[v].base));
}

void drawCounter() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float m = 24 * g_scale;
    const bool right = g_corner & 1, bottom = g_corner & 2;
    ImGui::SetNextWindowPos({ right ? vp->Size.x - m : m, bottom ? vp->Size.y - m : m }, ImGuiCond_Always, { right ? 1.f : 0.f, bottom ? 1.f : 0.f });
    ImGui::SetNextWindowBgAlpha(0.62f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * g_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 16 * g_scale, 10 * g_scale });
    ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    const float y = ImGui::GetCursorPosY();
    auto bigNumber = [y](ImFont* f, float v, const char* unit) {
        ImGui::SetCursorPosY(y);  // SameLine would inherit the lowered unit label's y
        ImGui::PushFont(f);
        ImGui::Text("%.0f", v);
        ImGui::PopFont();
        ImGui::SameLine(0, 8 * g_scale);
        ImGui::SetCursorPosY(y + f->Ascent - g_font->Ascent);  // share the big number's baseline
        ImGui::TextColored(kGrey, "%s", unit);
    };
    const FgStatus fg = fgStatus();
    const bool gen = fg.generating && g_fpsRendered > 0;
    bigNumber(g_big, g_fpsShown, "FPS");
    if (gen) {  // with FG off both numbers are the same, so base is shown only while generating
        ImGui::SameLine(0, 20 * g_scale);
        bigNumber(g_bigThin, g_fpsRendered, "BASE");
        ImGui::TextColored(kAccents[backendVendor(fg.backend)].hi, "FG %.1f\xC3\x97", g_fpsShown / g_fpsRendered);  // the generating vendor's colour
    } else ImGui::TextColored(kGrey, fg.available ? "FG off" : "FG n/a");
    if (*g_gpu) ImGui::TextColored(kGrey, "%s", g_gpu);
    if (g_w && g_h) ImGui::TextColored(kGrey, "%u\xC3\x97%u", g_w, g_h);  // the screen resolution: the swap chain's size
    const DlssInfo di = dlssInfo();
    const int up = di.active == 1 || di.active == 2 ? di.active : 0;  // the upscaler running, else DLSS's version once loaded
    const char* ver = up == 1 ? di.fsrVersion : up == 2 ? di.xessVersion : di.version;
    if (*ver) ImGui::TextColored(kGrey, dlssEnabled() ? "%s %s" : "%s %s (off)", kUpscalers[up].shortName, ver);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void drawMenu() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({ vp->Size.x * 0.5f, vp->Size.y * 0.5f }, ImGuiCond_Appearing, { 0.5f, 0.5f });
    ImGui::SetNextWindowSize({ 560 * g_scale, 0 }, ImGuiCond_Always);
    ImGui::Begin("Lumenveil", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    if (g_padOpened) { pad::focusFirst(); g_padOpened = false; }
    ImGui::PushTextWrapPos(0.0f);  // notes wrap at the window edge instead of being clipped
    const float labelW = 200 * g_scale;
    auto label = [&](const char* t) { ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(t); ImGui::SameLine(labelW); ImGui::SetNextItemWidth(-FLT_MIN); };
    auto missing = [](const wchar_t* f) { return GetFileAttributesW(path(f).c_str()) == INVALID_FILE_ATTRIBUTES; };
    const bool nvidiaGpu = g_vendorId == 0x10DE, intelGpu = g_vendorId == 0x8086;

    if (g_logo) {  // centred, 64% of the content width
        const float avail = ImGui::GetContentRegionAvail().x, w = avail * 0.64f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
        ImGui::Image((ImTextureID)gpuAt(g_srvHeap, 2).ptr, { w, w * g_logoH / g_logoW });
    }

    // ---- Upscaling: its controls take the colour of the upscaler running, else of the one the setting asks for
    ImGui::SeparatorText("Upscaling");
    const DlssInfo di = dlssInfo();
    const bool activeKnown = di.active >= 0 && di.active < kUpscalerCount;
    // the upscaler wanted: the one chosen, else what Auto runs, else what Auto picks on this GPU
    const int want = di.upscaler >= 0 && di.upscaler < kUpscalerCount ? di.upscaler : activeKnown ? di.active
                   : nvidiaGpu ? 0 : intelGpu && !missing(kUpscalers[2].file) ? 2 : 1;
    {
        const Vendor v = kUpscalers[activeKnown ? di.active : want].vendor;
        AccentScope accent(v);
        const char* names[1 + kUpscalerCount] = { "Auto  (DLSS on NVIDIA, XeSS on Intel, else FSR)" };
        for (int i = 0; i < kUpscalerCount; ++i) names[1 + i] = kUpscalers[i].name;
        int us = di.upscaler >= -1 && di.upscaler < kUpscalerCount ? di.upscaler + 1 : 0;
        label("Upscaler");
        if (ImGui::Combo("##upscaler", &us, names, 1 + kUpscalerCount)) { dlssSetUpscaler(us - 1); iniSet(L"dlss", L"upscaler", us - 1); }
        if (want == 0 && !nvidiaGpu) ImGui::TextColored(kAmber, "DLSS needs an NVIDIA GeForce RTX card. The game uses its own anti-aliasing.");
        else if (missing(kUpscalers[want].file)) ImGui::TextColored(kAmber, "%s", kUpscalers[want].missing);
        if (di.active == 1 && *di.fsrVersion)
            ImGui::TextDisabled(di.fsrVersion[0] >= '4' ? "AMD FSR %s (machine learning)." : "AMD FSR %s.", di.fsrVersion);
        if (di.active == 2 && *di.xessVersion) ImGui::TextDisabled("Intel XeSS %s.", di.xessVersion);
        if (kUpscalers[want].tested) testedNote(kUpscalers[want].tested, kUpscalers[want].vendor);
        bool on = dlssEnabled();
        label("Enabled");
        bool flip = ImGui::Checkbox("##upscale", &on);
        flip |= pad::padTick(on);
        if (flip) dlssToggle();
    }

    // Scaling mode = the game's RenderingResolutionRatio, read at launch (so a change applies after a restart)
    struct Mode { const char* name; int pct; };
    const int upPct = ultraPerformancePct(want == 2);  // 0 with XeSS: no game scale value fits it below 50% (xess_rules.h)
    const Mode scaleModes[] = { { "DLAA", 100 }, { "Quality", 67 }, { "Balanced", 58 }, { "Performance", 50 }, { "Ultra Performance", upPct ? upPct : 33 } };
    const int modeCount = upPct ? 5 : 4;  // Ultra Performance last, left out when not offered
    static const std::wstring cfg = gameConfig();
    static int ratio = cfg.empty() ? 0 : GetPrivateProfileIntW(L"BasicSettings", L"RenderingResolutionRatio", 100, cfg.c_str());
    auto modeText = [&](char* buf, size_t n, const char* name, int pct) {
        if (di.dispW) snprintf(buf, n, "%s  \xC2\xB7  %d%%  \xC2\xB7  %u\xC3\x97%u \xE2\x86\x92 %u\xC3\x97%u", name, pct,
                               di.dispW * pct / 100, di.dispH * pct / 100, di.dispW, di.dispH);
        else snprintf(buf, n, "%s  \xC2\xB7  %d%%", name, pct);
    };
    char preview[96] = "GraphicsConfig.ini not found";
    if (ratio) {
        const Mode* cur = nullptr;
        for (const Mode& m : scaleModes) if (m.pct == ratio) cur = &m;  // includes Ultra Performance's 33 when not offered
        if (!cur && (ratio == 33 || ratio == 34)) cur = &scaleModes[4];  // the other upscaler's Ultra Performance
        modeText(preview, sizeof preview, cur ? cur->name : "Custom", ratio);
    }
    label("Scaling mode");
    ImGui::BeginDisabled(!ratio);
    if (ImGui::BeginCombo("##scale", preview)) {
        for (int i = 0; i < modeCount; ++i) {
            const Mode& m = scaleModes[i];
            char t[96]; modeText(t, sizeof t, m.name, m.pct);
            if (ImGui::Selectable(t, m.pct == ratio) && m.pct != ratio) {
                ratio = m.pct;
                WritePrivateProfileStringW(L"BasicSettings", L"RenderingResolutionRatio", std::to_wstring(ratio).c_str(), cfg.c_str());
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    const int live = di.outW ? int(std::lround(100.0 * di.inH / di.outH)) : 0;
    if (live) ImGui::TextDisabled("Now rendering %u\xC3\x97%u \xE2\x86\x92 %u\xC3\x97%u (%d%%).", di.inW, di.inH, di.outW, di.outH, live);
    if (ratio && live && std::abs(live - ratio) > 1) ImGui::TextColored(kGreen, "Restart the game to apply the new scaling mode.");
    else if (ratio && !live) ImGui::TextDisabled("Changes apply the next time the game starts.");
    if (want == 2 && ((ratio && ratio < xessLowestPct()) || xessInputBelowRange())) {
        if (ultraPerformancePct(true))
            ImGui::TextColored(kAmber, "Intel XeSS needs a scaling mode of %d%% or more. Pick Ultra Performance again (%d%% with XeSS) "
                                       "and restart the game.", xessLowestPct(), xessLowestPct());
        else
            ImGui::TextColored(kAmber, "Intel XeSS needs a scaling mode of %d%% or more. Pick Performance and restart the game.", xessLowestPct());
    }

    if (want == 0 && (di.active == 0 || !activeKnown)) {  // DLSS's model; FSR picks its own
        AccentScope accent(kNvidia);
        static const int presets[] = { 0, 11, 13, 12, 10, 6, 5 };
        static const char* presetNames[] = { "Default  (NVIDIA picks per mode)", "K  \xC2\xB7  transformer, best quality",
                                             "M  \xC2\xB7  transformer, made for Performance", "L  \xC2\xB7  transformer, made for Ultra Performance",
                                             "J  \xC2\xB7  transformer, like K: less ghosting, more flicker",
                                             "F  \xC2\xB7  CNN (deprecated)", "E  \xC2\xB7  CNN (deprecated)" };
        // a preset this list doesn't know yet (a newer DLL's, set by hand in the ini) is kept and shown as such
        const char* modelNames[IM_ARRAYSIZE(presets) + 1]; char custom[48];
        int pi = -1, models = 0;
        for (; models < IM_ARRAYSIZE(presets); ++models) { modelNames[models] = presetNames[models]; if (presets[models] == di.preset) pi = models; }
        if (pi < 0) { snprintf(custom, sizeof custom, "%c  \xC2\xB7  set in ffxv_dlss.ini", 'A' + di.preset - 1); modelNames[models] = custom; pi = models++; }
        label("DLSS model");
        if (ImGui::Combo("##preset", &pi, modelNames, models) && pi < IM_ARRAYSIZE(presets)) { iniSet(L"dlss", L"preset", presets[pi]); dlssSetPreset(presets[pi]); }
        if (*di.version && atoi(di.version) < 310)
            ImGui::TextColored(kAmber, "DLSS %s is older than 310: models J, K, L and M need 310 or newer, so NVIDIA's default is used.", di.version);
    }
    int sharp = di.sharpness;
    label("Sharpening");
    const bool sharpChanged = pad::padSlider("##sharp", sharp, 0, 100, 1, sharp ? "%d%%" : "Off");
    if (sharpChanged) { dlssSetSharpness(sharp); iniSet(L"dlss", L"sharpness", sharp); }

    // ---- Frame Generation: its controls take the colour of the backend picked, the readouts that of the one running
    ImGui::SeparatorText("Frame Generation");
    const FgStatus st = fgStatus();
    FgSettings fs = fgSettings();
    bool changed = false;
    // The backend decides which swapchain the presenter creates at launch, so a change applies at the next launch.
    // Auto resolves as the presenter does (fgPickBackend): DLSS-G on an NVIDIA GPU with nvngx_dlssg.dll in place, XeSS-FG
    // on an Intel GPU with Intel's three XeSS files, else FSR.
    static const int launchBackend = iniGet(L"fg", L"backend", -1);
    static const bool launchEnable = fs.enable != 0;  // FSR's swapchain is only created when FG is on at launch
    static int backend = launchBackend >= -1 && launchBackend < kBackendCount ? launchBackend + 1 : 0;
    const bool dlssg = !missing(L"ffxv_dlss\\streamline\\nvngx_dlssg.dll");
    const bool fsrFiles = !missing(kUpscalers[1].file), fsrFgDll = !missing(L"ffxv_dlss\\fidelityfx\\amd_fidelityfx_framegeneration_dx12.dll");
    const bool xessFgFiles = xessFgPresent();
    auto resolve = [&](int b) { return fgPickBackend(b, g_vendorId, dlssg, xessFgFiles); };
    const int running = st.backend >= 0 ? st.backend : resolve(launchBackend), picked = resolve(backend - 1);
    // FSR or XeSS is ready but FG was off at launch (their swapchains are made only then): the setting can still be turned
    // on for the next launch
    const bool fsrOffAtLaunch = !st.available && !launchEnable && !st.formatRefused
                                && ((running == 1 && fsrFiles && fsrFgDll) || (running == 2 && xessFgFiles));
    {
        AccentScope accent(kBackends[picked].vendor);
        const char* names[1 + kBackendCount] = { "Auto  (DLSS on NVIDIA, XeSS on Intel, else FSR)" };
        for (int i = 0; i < kBackendCount; ++i) names[1 + i] = kBackends[i].name;
        label("Backend");
        if (ImGui::Combo("##fgbackend", &backend, names, 1 + kBackendCount)) iniSet(L"fg", L"backend", backend - 1);
        if ((backend - 1 != launchBackend && picked != running) || (fsrOffAtLaunch && fs.enable))
            ImGui::TextColored(kGreen, "Restart the game to switch frame generation.");
        if (!st.available) {
            if (running == 0 && dlssg)
                ImGui::TextDisabled("Not available: DLSS Frame Generation needs an RTX 40 or 50 series card. AMD FSR Frame Generation runs on this card.");
            else if (running == 0)
                ImGui::TextColored(kAmber, "nvngx_dlssg.dll not found. Copy it from the same DLSS SDK folder (lib\\Windows_x86_64\\rel) "
                                           "into ffxv_dlss\\streamline and restart the game.");
            else if (running == 2) {
                if (!xessFgFiles)  // named here: the Upscaling section names Intel's files only while XeSS upscales
                    ImGui::TextColored(kAmber, "Intel's XeSS frame generation needs libxess.dll, libxess_fg.dll and libxell.dll in "
                                               "ffxv_dlss\\xess. Put the XeSS SDK 3.0.2 zip in your Downloads folder and run the "
                                               "installer again, or copy them from its bin folder. INSTALL.txt has the download link.");
                else if (st.formatRefused)
                    ImGui::TextDisabled("Not available: Intel XeSS frame generation does not take the game's HDR output format. "
                                        "Turn HDR off in the game, or pick another backend.");
                else if (launchEnable) ImGui::TextDisabled("Not available: Intel XeSS frame generation did not start. ffxv_dlss.log has the reason.");
            }
            else if (!fsrFiles) ImGui::TextColored(kAmber, "AMD's FSR files not found (see Upscaling above).");
            else if (!fsrFgDll)
                ImGui::TextColored(kAmber, "amd_fidelityfx_framegeneration_dx12.dll not found in ffxv_dlss\\fidelityfx. FSR upscaling works without it.");
            else if (launchEnable) ImGui::TextDisabled("Not available: AMD FSR frame generation did not start. ffxv_dlss.log has the reason.");
        }
        if (st.available && st.backend == 0 && *dlssgVersion()) ImGui::TextDisabled("NVIDIA DLSS frame generation %s.", dlssgVersion());
        if (st.available && st.backend == 1 && *fsrFgVersion()) ImGui::TextDisabled("AMD FSR frame generation %s.", fsrFgVersion());
        if (st.available && st.backend == 2 && *xessFgVersion())
            ImGui::TextDisabled("Intel XeSS frame generation %s, with Xe Low Latency.", xessFgVersion());
        // the tested-only note while the backend is picked, and while it still runs until the restart
        const int noteB = kBackends[picked].tested ? picked : st.backend >= 0 && st.backend < kBackendCount && kBackends[st.backend].tested ? st.backend : -1;
        if (noteB >= 0) testedNote(kBackends[noteB].tested, kBackends[noteB].vendor);
        ImGui::BeginDisabled(!st.available && !fsrOffAtLaunch);
        bool on = fs.enable != 0;
        label("Enabled");
        bool fgFlip = ImGui::Checkbox("##fg", &on);
        fgFlip |= pad::padTick(on);
        if (fgFlip) { fs.enable = on; changed = true; }
        if (st.backend == 0) {  // DLSS-G modes: Dynamic (when supported), then 2x .. (max generated + 1)x; FSR and XeSS 2x
            const char* modes[7]; char modeNames[6][8]; int n = 0, cur = 0;
            if (st.dynamicSupported) modes[n++] = "Dynamic";
            for (unsigned k = 1; k <= std::max(1u, st.maxGenerated) && k <= 5; ++k) {
                snprintf(modeNames[k], sizeof modeNames[k], "%u\xC3\x97", k + 1);
                if (fs.frames == int(k)) cur = n;
                modes[n++] = modeNames[k];
            }
            label("Mode");
            if (ImGui::Combo("##mode", &cur, modes, n)) { fs.frames = st.dynamicSupported ? cur : cur + 1; changed = true; }
            if (fs.frames == 0) {
                label("Target fps");
                changed |= pad::padSlider("##target", fs.target, 60, 480, 5, "%d fps");
            }
        }
        if (st.generating && g_fpsRendered > 0)
            ImGui::TextColored(kAccents[backendVendor(st.backend)].hi, "Generating %.1f\xC3\x97 \xC2\xB7 %.0f shown from %.0f rendered",
                               g_fpsShown / g_fpsRendered, g_fpsShown, g_fpsRendered);
        else ImGui::TextDisabled(st.available ? "Idle: turns on in gameplay, where the upscaler runs." : "");

        if (st.backend == 0) {  // Reflex comes with Streamline: DLSS-G only
            ImGui::SeparatorText("NVIDIA Reflex");
            static const char* reflexModes[] = { "Off", "On", "On + Boost" };
            label("Low latency");
            changed |= ImGui::Combo("##reflex", &fs.reflex, reflexModes, 3);
            if (fs.enable && fs.reflex == 0) ImGui::TextDisabled("Frame generation needs Reflex, so it stays on while FG is enabled.");
            else if (fs.reflex == 2) ImGui::TextDisabled("Boost keeps GPU clocks up when the CPU is the limit. Slightly more power.");
        } else if (st.backend == 2) {  // Xe Low Latency comes with XeSS frame generation
            ImGui::SeparatorText("Intel Xe Low Latency");
            static const char* xellModes[] = { "Off", "On" };
            int xell = fs.reflex > 0 ? 1 : 0;  // the ini's On + Boost (2) shows as On and is kept until changed here
            label("Low latency");
            if (ImGui::Combo("##xell", &xell, xellModes, 2) && xell != (fs.reflex > 0 ? 1 : 0)) { fs.reflex = xell; changed = true; }
            ImGui::TextDisabled("XeSS frame generation needs Xe Low Latency, so it stays on while frame generation is enabled.");
        } else if (st.backend == 1 || di.active == 1) {  // the presenter paces frames itself whenever FSR or XeSS is in use
            ImGui::SeparatorText("Latency");
            ImGui::TextDisabled("The game waits for the GPU each frame instead of queueing frames ahead (FSR has no Reflex).");
        } else if (di.active == 2) {
            ImGui::SeparatorText("Latency");
            ImGui::TextDisabled("The game waits for the GPU each frame instead of queueing frames ahead.");
        }
    }
    ImGui::EndDisabled();
    {
        AccentScope accent(kNeutral);  // the mod's own frame cap, whichever backend runs
        // it applies through Reflex when Streamline runs, else through the presenter's own pacing (FSR or XeSS), so it
        // stays usable without FG
        ImGui::BeginDisabled(!st.available && st.backend < 0 && di.active != 1 && di.active != 2);
        char autoName[32]; snprintf(autoName, sizeof autoName, "Auto (%s)", syncName());
        const char* capModes[] = { "Off", autoName, "Custom" };
        int capMode = fs.limit < 0 ? 1 : fs.limit == 0 ? 0 : 2;
        label("Frame cap");
        if (ImGui::Combo("##capmode", &capMode, capModes, 3)) { fs.limit = capMode == 0 ? 0 : capMode == 1 ? -1 : std::max(st.autoCap, 60); changed = true; }
        if (capMode == 2) {
            label("Cap");
            changed |= pad::padSlider("##limit", fs.limit, 30, 480, 5, "%d fps");
        }
        else if (capMode == 1)
            ImGui::TextDisabled("Output capped at %d fps, just under the display's refresh, so %s stays engaged. "
                                "Pair it with V-Sync on in the driver's control panel.", st.autoCap, syncName());
        ImGui::EndDisabled();
    }
    if (changed) {
        fgApply(fs);
        iniSet(L"fg", L"enable", fs.enable); iniSet(L"fg", L"frames", fs.frames); iniSet(L"fg", L"target", fs.target); iniSet(L"fg", L"limit", fs.limit);
        iniSet(L"fg", L"reflex", fs.reflex);
    }

    ImGui::SeparatorText("Overlay");
    label("FPS counter");
    bool counterFlip = ImGui::Checkbox("##counter", &g_counter);
    counterFlip |= pad::padTick(g_counter);
    if (counterFlip) iniSet(L"overlay", L"enable", g_counter);
    static const char* corners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
    label("Position");
    if (ImGui::Combo("##corner", &g_corner, corners, 4)) iniSet(L"overlay", L"corner", g_corner);

    ImGui::Spacing();
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
    g_padBusy = popup || ImGui::IsAnyItemActive();  // B backs out of the list or slider instead of closing the menu
    if (g_padUsed) pad::drawHints(popup, g_padBusy, g_scale);  // in the footer line's own height: nothing moves
    else ImGui::TextDisabled("Insert or Esc to close \xC2\xB7 changes apply and save immediately");
    ImGui::Spacing();
    ImGui::TextDisabled("Uses NVIDIA DLSS, NVIDIA Reflex and NVIDIA Streamline, AMD FSR and Intel XeSS. NVIDIA, DLSS and "
                        "Reflex are trademarks of NVIDIA Corporation, FSR of AMD and XeSS of Intel.");
    ImGui::PopTextWrapPos();
    ImGui::End();
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
    g_open = false;
    g_padUsed = false;
    g_padRelease = true;
}

void pollToggle() {
    static bool insDown, escDown;
    static pad::ComboHold combo;
    const bool focused = GetForegroundWindow() == g_wnd;  // the keys are global; only react while the game is in front
    const bool ins = focused && (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0, esc = focused && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const ULONGLONG tick = GetTickCount64();
    g_padView = g_pad.merge(tick);
    const bool stickCombo = combo.update(focused && g_padView.comboDown, tick);  // both sticks held in for half a second
    const bool bClose = g_open && !g_padBusy && (g_padView.edges & pad::kB);
    if ((ins && !insDown) || (esc && !escDown && g_open) || stickCombo || bClose) {
        if (g_open) closeMenu();
        else {
            g_open = true;
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
    for (int i = 0; i < 3; ++i) {
        osDownFrames += os[i];
        const ButtonStates s = buttonEvents(di, b.down[i], b.hit[i], os[i]);
        for (int k = 0; k < s.n; ++k) {
            if (s.state[k] && !sent[i]) ++pressesSent;
            sent[i] = s.state[k];
            io.AddMouseButtonEvent(i, s.state[k]);
        }
    }
    if (wheel) io.AddMouseWheelEvent(0, wheel / 120.f);
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
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f4))) && SUCCEEDED(f4->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&ad))) &&
        SUCCEEDED(ad->GetDesc1(&ad1))) {
        g_vendorId = ad1.VendorId;
        const wchar_t* n = ad1.Description;
        for (const wchar_t* p : { L"NVIDIA GeForce ", L"NVIDIA " }) if (!wcsncmp(n, p, wcslen(p))) { n += wcslen(p); break; }
        WideCharToMultiByte(CP_UTF8, 0, n, -1, g_gpu, sizeof g_gpu, nullptr, nullptr);
    }
    if (ad) ad->Release();
    if (f4) f4->Release();
    g_counter = iniGet(L"overlay", L"enable", 1) != 0;
    g_corner = std::clamp(iniGet(L"overlay", L"corner", 0), 0, 3);
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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written next to the game
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NavEnableGamepad;
    loadFonts();
    applyStyle();
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = dev; ii.CommandQueue = queue; ii.NumFramesInFlight = kFrames;
    ii.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ii.SrvDescriptorHeap = g_srvHeap;
    ii.LegacySingleSrvCpuDescriptor = cpuAt(g_srvHeap, 0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ii.LegacySingleSrvGpuDescriptor = gpuAt(g_srvHeap, 0);
    if (!ImGui_ImplDX12_Init(&ii)) { logf("menu: ImGui DX12 init failed"); return false; }
    loadLogo();
    hookInput();
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_last); g_statStart = g_last;
    g_ready = true;
    logf("menu: ready (Insert), ui scale %.2f, fonts %s", g_scale, g_fontName);
    return true;
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
    if (g_open) {
        if (g_padView.connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        pad::feedPad(io, g_padView, g_padBusy);
        if (g_padView.edges || g_padView.comboDown || pad::stickPast(g_padView.lx, 1) || pad::stickPast(g_padView.lx, -1) ||
            pad::stickPast(g_padView.ly, 1) || pad::stickPast(g_padView.ly, -1))
            g_padUsed = true;
    } else if (g_padRelease) { pad::releasePad(io); g_padRelease = false; }
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    if (g_counter) drawCounter();
    if (g_open) drawMenu();
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

// After Present: `shown` = frames actually presented for this rendered frame (2 at 2x frame generation).
void inputThreads(DWORD* dinput, DWORD* xinput, unsigned* diPolls) { *dinput = g_diTid; *xinput = g_xiTid; *diPolls = g_diPolls.exchange(0); }

void menuCount(unsigned shown) {
    ++g_rendered; g_shown += shown;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double dt = double(now.QuadPart - g_statStart.QuadPart) / (g_freq.QuadPart ? g_freq.QuadPart : 1);
    if (dt < 0.5) return;
    g_fpsRendered = float(g_rendered / dt); g_fpsShown = float(g_shown / dt);
    g_rendered = g_shown = 0; g_statStart = now;
}
