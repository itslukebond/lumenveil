// D3D12 work on the presenter's queue for an upscaler or frame generation whose inputs the game renders in D3D11.
// The two APIs hand work to each other through two shared fences, each signalled from one API only (present12.cpp):
// D3D11 signals up11 and the queue waits on it; the queue signals fence and D3D11 waits on fence11. Everything here
// runs on the game's render thread.
#include "common.h"
#include <d3d11_4.h>
#include <d3d12.h>

namespace {
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Command lists on the presenter's queue: a ring of allocators, each reused once its last submission has finished.
constexpr unsigned kRing = 8;
ID3D12CommandAllocator* g_alloc[kRing];
UINT64 g_allocDone[kRing];
ID3D12GraphicsCommandList* g_cl;
unsigned g_slot;
HANDLE g_event;
}  // namespace

bool up12Reached(const P12Shared& s, UINT64 v) {
    if (s.fence->GetCompletedValue() >= v) return true;
    if (!g_event) g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_event && SUCCEEDED(s.fence->SetEventOnCompletion(v, g_event))) WaitForSingleObject(g_event, 2000);
    return s.fence->GetCompletedValue() >= v;
}

bool up12Idle(const P12Shared& s) {
    return SUCCEEDED(s.queue->Signal(s.fence, ++*s.fenceVal)) && up12Reached(s, *s.fenceVal);
}

ID3D12GraphicsCommandList* up12Begin(const P12Shared& s) {
    const unsigned i = g_slot % kRing;
    if (g_allocDone[i] && !up12Reached(s, g_allocDone[i])) return nullptr;
    if (!g_alloc[i] && FAILED(s.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i])))) return nullptr;
    if (FAILED(g_alloc[i]->Reset())) return nullptr;
    if (!g_cl) { if (FAILED(s.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[i], nullptr, IID_PPV_ARGS(&g_cl)))) return nullptr; }
    else if (FAILED(g_cl->Reset(g_alloc[i], nullptr))) return nullptr;
    return g_cl;
}

bool up12Submit(const P12Shared& s) {
    const unsigned i = g_slot++ % kRing;
    if (FAILED(g_cl->Close())) return false;
    ID3D12CommandList* l = g_cl;
    s.queue->ExecuteCommandLists(1, &l);
    return SUCCEEDED(s.queue->Signal(s.fence, g_allocDone[i] = ++*s.fenceVal));
}

bool up12Ensure(const P12Shared& s, Up12Tex& t, UINT w, UINT h, bool uav, const char* what) {
    if (t.r12 && t.w == w && t.h == h) return true;
    if (t.r12 && !up12Idle(s)) return false;  // the last dispatch may still read or write it
    release(t.r11); release(t.r12);
    // D3D11-created and opened in D3D12: D3D11 can't open a D3D12-created texture (E_INVALIDARG)
    D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u), 0,
                               D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE };
    HANDLE hd = nullptr;
    IDXGIResource1* r = nullptr;
    HRESULT hr = s.dev11->CreateTexture2D(&d, nullptr, &t.r11);
    if (SUCCEEDED(hr)) hr = t.r11->QueryInterface(IID_PPV_ARGS(&r));
    if (SUCCEEDED(hr)) hr = r->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hd);
    if (SUCCEEDED(hr)) hr = s.dev->OpenSharedHandle(hd, IID_PPV_ARGS(&t.r12));
    release(r);
    if (hd) CloseHandle(hd);
    logf("up12: shared %s %ux%u: %08x", what, w, h, hr);
    if (FAILED(hr)) { release(t.r11); release(t.r12); return false; }
    t.w = w; t.h = h;
    return true;
}

void up12ToD3D12(const P12Shared& s) {
    s.ctx11->Signal(s.up11, ++*s.upVal);
    s.ctx11->Flush();  // the queue waits on that signal, so it must reach the GPU now
    s.queue->Wait(s.up, *s.upVal);
}

void up12ToD3D11(const P12Shared& s) {
    s.ctx11->Wait(s.fence11, *s.fenceVal);
}
