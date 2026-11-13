#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string>
#include <vector>

void logf(const char* fmt, ...);
void flushLog();
std::wstring path(const wchar_t* name);  // file in the game folder

// cbTemporalAA (TAA resolve ps f5990d1d5712e767, cb0)
struct TaaCB {
    float screenSize[4];
    float frameBits[4];
    float uvJitterOffset[4];
    float motionMatrix[16];
    float reconstructMatrix[16];
    float unprojectParams[4];
    float maxLuminanceInv;
    unsigned gamePaused, hairUseAlphaTest, waterResponsiveAA;
};
static_assert(sizeof(TaaCB) == 208, "cbTemporalAA layout");

bool cbShadow(ID3D11Buffer* b, TaaCB* out);  // latest CPU-side contents of a 256-byte constant buffer

// One rendered frame's inputs for frame generation: DLSS's motion vectors (UV units, prev - cur) and depth.
struct FgFrame { ID3D11Texture2D *mv, *depth; UINT w, h; TaaCB cb; float jitterPx[2]; };
bool dlssTakeFrame(FgFrame* out);                 // true once per frame in which DLSS ran

// dlss.cpp
bool dlssEnabled();
void dlssToggle();
// in/out are 0 until DLSS first runs; disp = game swapchain; sharpness 0..100 (%); version "" until DLSS loads
// upscaler: the setting (-1 auto, else an upscaler id); active: what runs (-1 none / game TAA, else an upscaler id).
// Upscaler ids: 0 DLSS, 1 FSR, 2 XeSS. Each upscaler other than DLSS has its own version string.
struct DlssInfo { UINT inW, inH, outW, outH, dispW, dispH; int preset, sharpness; char version[24]; int upscaler, active; char fsrVersion[32], xessVersion[16]; };
DlssInfo dlssInfo();
void dlssSetUpscaler(int u);                     // live
void dlssSetPreset(int preset);                  // render preset (0 = SDK default, 10..13 = J..M, 5/6 = E/F); applied on the next DLSS frame
void dlssSetSharpness(int percent);              // CAS after DLSS, 0 = off; live
bool dlssReplaceTaa(ID3D11DeviceContext* ctx);   // true = DLSS ran, skip the game's TAA draw
void dlssAfterTaa(ID3D11DeviceContext* ctx);     // screenshot hook after either path
// Upscaling below 100% render scale: DLSS outputs display res; the tonemap runs at display res and the blit to the
// UI layer reads that instead of stretching the render-res image.
void dlssSetDisplaySize(UINT w, UINT h);
bool dlssTonemapBegin(ID3D11DeviceContext* ctx);  // true = redirected: draw, then dlssTonemapEnd
void dlssTonemapEnd(ID3D11DeviceContext* ctx);
ID3D11ShaderResourceView* dlssBlitSource(ID3D11DeviceContext* ctx);  // replacement t0 for the blit, or null
void dlssRequestShot(const char* tag);
void shotBackbuffer(ID3D11DeviceContext* ctx, ID3D11Resource* bb, const char* tag);

// main.cpp
void uiToggle();

// glyph2x.cpp: 2048² R8 letter atlas from the game's 1024² BC4 one (mip 0 blocks) and the .otf; empty if the font fails
std::vector<unsigned char> buildGlyph2x(const unsigned char* bc4, const std::vector<unsigned char>& font);

// present12.cpp: D3D12 presentation of the game's D3D11 frames ([d3d12] present=1)
using PFN_CreateSCHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
bool p12Enabled();
HWND p12Init(IUnknown* d3d11Device, IDXGIFactory2* f, PFN_CreateSCHwnd create, HWND wnd, const DXGI_SWAP_CHAIN_DESC1* desc);  // hidden window for the game's swapchain, or null to stay native
void p12Abort();                         // the game's swapchain could not be created on the hidden window
void p12Attach(IDXGISwapChain* game);
bool p12Owns(IDXGISwapChain* sc);        // our D3D12 swapchain: hooks pass it straight through
bool p12IsGame(IDXGISwapChain* sc);
HWND p12GameWindow();                     // the game's own (visible) window while the presenter runs
HRESULT p12Present(UINT syncInterval, UINT flags);
bool p12Busy();                          // creating our swapchain: DXGI hooks pass straight through
void p12WaitInputsFree(ID3D11DeviceContext* ctx);  // before overwriting frame-generation inputs
void p12FirstDraw();                     // Reflex: simulation end / render submit start
// What up12.cpp needs of the presenter: its D3D12 device and queue, the game's D3D11 device/context and the two shared
// fences (each signalled from one API only, used from the game's render thread only).
struct ID3D12Device; struct ID3D12CommandQueue; struct ID3D12Fence; struct ID3D12Resource; struct ID3D11Device5;
struct ID3D11DeviceContext4; struct ID3D11Fence; struct IDXGISwapChain3; struct ID3D12GraphicsCommandList;
struct P12Shared { ID3D12Device* dev; ID3D12CommandQueue* queue; ID3D11Device5* dev11; ID3D11DeviceContext4* ctx11;
                   ID3D12Fence* fence; ID3D11Fence* fence11; UINT64* fenceVal;  // D3D12 -> D3D11 (D3D12 signals)
                   ID3D12Fence* up; ID3D11Fence* up11; UINT64* upVal; };        // D3D11 -> D3D12 (D3D11 signals)
bool p12Shared(P12Shared* out);          // false until the presenter is up
bool p12FsrGenerating();                 // FSR frame generation ran for the last frame
bool p12WantsHudless();                  // FSR or XeSS frame generation is on: main.cpp should produce a HUD-less back buffer
ID3D11RenderTargetView* p12HudlessTarget();  // this frame's HUD-less target (marks it written), or null
int p12OutputEncoding(float lum[2]);     // back buffer encoding: 0 SDR, 1 HDR10 PQ, 2 scRGB; lum: display min / max nits
ID3D12Resource* p12Open(int slot, ID3D11Texture2D* t, UINT w);  // D3D12 view of a SHARED_NTHANDLE texture (0 = mv, 1 = depth), cached

// up12.cpp: D3D12 work on the presenter's queue for an upscaler or frame generation fed from the game's D3D11 frames.
// One allocator ring and one fence counter for every such user.
struct Up12Tex { ID3D12Resource* r12; ID3D11Texture2D* r11; UINT w, h; };  // D3D11-created, opened in D3D12
bool up12Reached(const P12Shared& s, UINT64 v);  // the queue has passed v; false after 2 s (hung GPU): reuse nothing
bool up12Idle(const P12Shared& s);               // signal the queue and wait for it
ID3D12GraphicsCommandList* up12Begin(const P12Shared& s);  // an open list on the next ring slot, or null
bool up12Submit(const P12Shared& s);             // close and run the list; the queue signals the slot's value
// (Re)creates t at w x h (R11G11B10 float, UAV if asked) after the queue has finished with the old one
bool up12Ensure(const P12Shared& s, Up12Tex& t, UINT w, UINT h, bool uav, const char* what);
void up12ToD3D12(const P12Shared& s);  // the queue waits for the game's D3D11 work so far (D3D11 signals up11)
void up12ToD3D11(const P12Shared& s);  // the game's context waits for the queue's work so far (D3D12 signals fence)

// fsr.cpp: AMD FSR (FidelityFX SDK 2; AMD's signed DLLs are user-supplied in ffxv_dlss\fidelityfx)
bool fsrLoad();                          // loads the DLLs once; false if missing
bool fsrPresent();                       // loader DLL in place (no load)
const char* fsrUpscalerVersion();        // "" until the upscaler first runs
const char* fsrFgVersion();
bool fsrUpscale(ID3D11Texture2D* color, ID3D11Texture2D* depth, ID3D11Texture2D* mv, ID3D11Texture2D* out,
                UINT inW, UINT inH, UINT outW, UINT outH, const float jitterPx[2], bool reset);
IDXGISwapChain1* fsrFgSwapchain(IDXGIFactory2* f, ID3D12CommandQueue* q, HWND wnd, DXGI_SWAP_CHAIN_DESC1* desc);  // or null
void fsrFgRelease(IDXGISwapChain3* sc);  // before the swapchain is resized
void fsrFgDestroy(IDXGISwapChain3* sc);  // presenter rollback: frame generation and the swapchain context
void fsrFgUi(ID3D12Resource* ui);        // UI layer FSR composites over every presented frame, or null for none
// once per rendered frame before Present; f / depth / mv null when this frame has no inputs; true = a frame is generated
bool fsrFgFrame(ID3D12Device* dev, IDXGISwapChain3* sc, bool on, const FgFrame* f, ID3D12Resource* depth, ID3D12Resource* mv,
                ID3D12Resource* hudless, bool reset);  // hudless: this frame's back buffer without the HUD, or null

// xess.cpp: Intel XeSS (Intel's DLLs are user-supplied in ffxv_dlss\xess, loaded at run time). XeSS-FG and Xe Low Latency
// (XeLL) share one frame id per rendered frame: XeLL's frame id, the tag id and the present id.
bool xessPresent();                      // libxess.dll in place (no load)
bool xessInputBelowRange();              // XeSS upscaling refused: the render size is below every preset's range
bool xessFgPresent();                    // libxess.dll, libxess_fg.dll and libxell.dll in place (no load)
const char* xessVersion();               // "" until the upscaler's DLL loads
const char* xessFgVersion();             // "" until frame generation's DLLs load
bool xessUpscale(ID3D11Texture2D* color, ID3D11Texture2D* depth, ID3D11Texture2D* mv, ID3D11Texture2D* out,
                 UINT inW, UINT inH, UINT outW, UINT outH, const float jitterPx[2], bool reset);
// XeLL, then Intel's proxy swapchain for the window (or null, with everything made so far destroyed); latency as
// xessFgSleepMode
IDXGISwapChain1* xessFgSwapchain(IDXGIFactory2* f, ID3D12Device* dev, ID3D12CommandQueue* q, HWND wnd, DXGI_SWAP_CHAIN_DESC1* desc, int latency);
void xessFgDestroy();                    // after the last reference to the proxy swapchain is gone: XeSS-FG, then XeLL
void xessFgEnable(bool on);
void xessFgSleepMode(int mode);          // XeLL: 0 off, 1 on, 2 on + boost
void xessFgFrameStart(unsigned id);      // XeLL sleep + simulation start: the frame begins
void xessFgFirstDraw(unsigned id);       // simulation end + render submit start
void xessFgBeforePresent(unsigned id);   // render submit end + present start, and the present id
unsigned xessFgAfterPresent(unsigned id);  // present end; frames shown for this rendered one
// once per rendered frame before Present: tags this frame's inputs (copied by XeSS) and constants; true = generating
bool xessFgFrame(bool on, const FgFrame* f, ID3D12Resource* depth, ID3D12Resource* mv, ID3D12Resource* hudless, bool reset, unsigned id);

// present12.cpp: frame generation settings, for the menu
// frames: 0 = dynamic MFG, 1..5 generated per rendered; limit: Reflex fps cap, 0 off, -1 auto (under refresh, for
// G-Sync); reflex: 0 off, 1 on, 2 on + boost
// antilag: [fg] antilag, AMD Radeon Anti-Lag 2 switched on (0 by default)
struct FgSettings { int enable, frames, target, limit, reflex, antilag; };
// autoCap: the frame cap that limit = -1 ("Auto") uses, just under the display refresh so G-Sync/VRR stays engaged
// backend: the one frame generation backend chosen at launch by [fg] backend (fg_select.h): -1 none, 0 DLSS-G
// (Streamline), 1 FSR, 2 XeSS; formatRefused: XeSS-FG was chosen but the game's back buffer is 16-bit float (scRGB)
// antilag: AMD Radeon Anti-Lag 2's Al2State (antilag_rules.h); antilagOn: it ran on the last frame
struct FgStatus { bool available, generating, dynamicSupported; unsigned maxGenerated; int autoCap; int backend; bool formatRefused; int antilag; bool antilagOn; };
FgSettings fgSettings();
void fgApply(const FgSettings& s);
FgStatus fgStatus();
const char* dlssgVersion();               // nvngx_dlssg.dll's file version, "" until it loads

// menu.cpp: Insert menu and fps counter, drawn by the D3D12 presenter
struct ID3D12Device; struct ID3D12CommandQueue; struct IDXGISwapChain3;
bool menuInit(ID3D12Device* dev, ID3D12CommandQueue* queue, HWND wnd, UINT height);
bool menuTargets(IDXGISwapChain3* sc);     // after the presenter's back buffers are (re)created
void menuReleaseTargets();                 // before they are released (queue already idle)
void menuFrame(IDXGISwapChain3* sc, UINT backBuffer);  // record + submit, before Present
void menuCount(unsigned shown);            // after Present: frames actually shown for this rendered one
// last threads seen polling input (0 = none) and DirectInput polls since the previous call, for Reflex sleep placement
void inputThreads(DWORD* dinput, DWORD* xinput, unsigned* diPolls);
