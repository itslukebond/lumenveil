// Which swap chain creations Lumenveil takes as the game's own. The game's call normally reaches our factory hooks
// straight from the game (or from ReShade, which we load). Another DXGI wrapper loaded by the game (OptiScaler as
// version.dll or winmm.dll, Special K) sits in between and passes the game's call on: it is taken too when the game
// is reached within a few frames through modules that are not ours, Streamline's or the system's, and the swap chain
// is on a window that looks like the game's. Pure, so tests/swapchain_caller_test.cpp checks it on the host; main.cpp
// captures the return chain and reads the window.
#pragma once
#include <string_view>

namespace swapchain_caller {

// What a return address on the stack belongs to. kGame and kReShade are decided in main.cpp by module handle.
enum class Mod { kGame, kReShade, kOurs, kStreamline, kSystem, kNone, kOther };

namespace detail {
inline wchar_t lower(wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c; }
// path is dir itself or a file or folder inside it, ignoring case
inline bool under(std::wstring_view path, std::wstring_view dir) {
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.remove_suffix(1);
    if (dir.empty() || path.size() <= dir.size()) return false;
    for (size_t i = 0; i < dir.size(); ++i)
        if (lower(path[i]) != lower(dir[i])) return false;
    return path[dir.size()] == L'\\' || path[dir.size()] == L'/';
}
}  // namespace detail

// path: the module's file name, empty when the address belongs to no module. ourFolder: <game>\ffxv_dlss\.
inline Mod classify(std::wstring_view path, std::wstring_view systemDir, std::wstring_view windowsDir, std::wstring_view ourFolder) {
    if (path.empty()) return Mod::kNone;
    if (detail::under(path, ourFolder)) return Mod::kOurs;
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring_view file = slash == std::wstring_view::npos ? path : path.substr(slash + 1);
    if (file.size() > 3 && detail::lower(file[0]) == L's' && detail::lower(file[1]) == L'l' && file[2] == L'.') return Mod::kStreamline;
    if (detail::under(path, systemDir) || detail::under(path, windowsDir)) return Mod::kSystem;
    return Mod::kOther;
}

struct Verdict {
    enum Kind { kNotGame, kGameDirect, kWrapper } kind;
    int wrapperIndex;    // kWrapper: the entry of the chain that passed the call on (logged by name)
    const char* reason;  // kNotGame: why, for the log
};

// chain[0] is the module of the hook's own return address (the immediate caller), chain[1..] the frames above it.
inline Verdict decide(const Mod* chain, int n, int maxFrames = 8) {
    if (!chain || n <= 0) return { Verdict::kNotGame, -1, "stack unreadable" };
    switch (chain[0]) {
    case Mod::kGame: case Mod::kReShade: return { Verdict::kGameDirect, -1, "" };  // the rule before wrappers, unchanged
    case Mod::kOurs: return { Verdict::kNotGame, -1, "called by one of ours" };
    case Mod::kStreamline: return { Verdict::kNotGame, -1, "called by Streamline" };
    case Mod::kSystem: return { Verdict::kNotGame, -1, "called by a system module" };
    case Mod::kNone: return { Verdict::kNotGame, -1, "called from no module" };
    case Mod::kOther: break;
    }
    const int end = n < maxFrames + 1 ? n : maxFrames + 1;
    for (int i = 1; i < end; ++i) {
        switch (chain[i]) {
        case Mod::kGame: return { Verdict::kWrapper, 0, "" };
        case Mod::kReShade: case Mod::kOther: continue;
        case Mod::kOurs: return { Verdict::kNotGame, -1, "one of ours before the game" };
        case Mod::kStreamline: return { Verdict::kNotGame, -1, "Streamline before the game" };
        case Mod::kSystem: return { Verdict::kNotGame, -1, "a system module before the game" };
        case Mod::kNone: return { Verdict::kNotGame, -1, "an address with no module before the game" };
        }
    }
    return { Verdict::kNotGame, -1, "game not reached" };
}

// The swap chain a wrapper passes on, as main.cpp reads it.
struct Target { bool d3d11Device, topLevel, toolWindow; long clientW, clientH; };

// Why a swap chain passed on by a wrapper is not the game's, or nullptr when it looks like the game's.
inline const char* refuse(const Target& t) {
    if (!t.d3d11Device) return "not a D3D11 device";
    if (!t.topLevel) return "not a top-level window";
    if (t.toolWindow) return "tool window";
    if (t.clientW < 320 || t.clientH < 200) return "window too small";
    return nullptr;
}

}  // namespace swapchain_caller
