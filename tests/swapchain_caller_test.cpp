// Host test for src/swapchain_caller.h: which swap chain creations Lumenveil takes as the game's. The immediate caller
// being the game or ReShade is taken as before; another module in between (a DXGI wrapper such as OptiScaler loaded as
// version.dll) is taken only when the game is reached within 8 frames through modules that are not ours, Streamline's
// or the system's, and the swap chain looks like the game's window. Expected values are written-out cases.
#include "swapchain_caller.h"
#include <cstdio>
#include <cstring>

using namespace swapchain_caller;

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// The rule before this change: only the immediate caller counts.
static bool todayRule(const Mod* chain, int n) { return n > 0 && (chain[0] == Mod::kGame || chain[0] == Mod::kReShade); }

template <int N> static Verdict run(const Mod (&c)[N]) { return decide(c, N); }
static bool has(const char* reason, const char* word) { return reason && std::strstr(reason, word); }

int main() {
    // The bug: a wrapper module, then the game (the Steam Deck report with OptiScaler as version.dll)
    {
        const Mod deck[] = { Mod::kOther, Mod::kGame };
        CHECK(!todayRule(deck, 2));
        const Verdict v = run(deck);
        CHECK(v.kind == Verdict::kWrapper);
        CHECK(v.wrapperIndex == 0);
    }
    // Direct callers: decided by the immediate caller alone, as before
    { const Mod c[] = { Mod::kGame }; CHECK(run(c).kind == Verdict::kGameDirect); }
    { const Mod c[] = { Mod::kReShade }; CHECK(run(c).kind == Verdict::kGameDirect); }
    { const Mod c[] = { Mod::kGame, Mod::kSystem }; CHECK(run(c).kind == Verdict::kGameDirect); }
    { const Mod c[] = { Mod::kReShade, Mod::kOurs, Mod::kNone }; CHECK(run(c).kind == Verdict::kGameDirect); }
    // ReShade and unknown modules between the wrapper and the game are passed through
    { const Mod c[] = { Mod::kOther, Mod::kReShade, Mod::kGame }; CHECK(run(c).kind == Verdict::kWrapper); }
    { const Mod c[] = { Mod::kOther, Mod::kOther, Mod::kOther, Mod::kGame }; CHECK(run(c).kind == Verdict::kWrapper); }
    // Ours, Streamline, the system or an address with no module before the game: not the game's
    {
        const Mod ours[] = { Mod::kOther, Mod::kOurs, Mod::kGame };
        const Mod sl[] = { Mod::kOther, Mod::kStreamline, Mod::kGame };
        const Mod sys[] = { Mod::kOther, Mod::kSystem, Mod::kGame };
        const Mod none[] = { Mod::kOther, Mod::kNone, Mod::kGame };
        const Verdict a = run(ours), b = run(sl), c = run(sys), d = run(none);
        CHECK(a.kind == Verdict::kNotGame && has(a.reason, "ours"));
        CHECK(b.kind == Verdict::kNotGame && has(b.reason, "Streamline"));
        CHECK(c.kind == Verdict::kNotGame && has(c.reason, "system"));
        CHECK(d.kind == Verdict::kNotGame && has(d.reason, "no module"));
    }
    // The immediate caller itself ours, Streamline's, the system's or no module: not the game's
    { const Mod c[] = { Mod::kOurs, Mod::kGame }; CHECK(run(c).kind == Verdict::kNotGame); }
    { const Mod c[] = { Mod::kStreamline, Mod::kGame }; CHECK(run(c).kind == Verdict::kNotGame); }
    { const Mod c[] = { Mod::kSystem, Mod::kGame }; CHECK(run(c).kind == Verdict::kNotGame); }
    { const Mod c[] = { Mod::kNone, Mod::kGame }; CHECK(run(c).kind == Verdict::kNotGame); }
    // The game within 8 frames after the immediate caller: frame 8 is taken, frame 9 is not
    {
        const Mod at8[] = { Mod::kOther, Mod::kOther, Mod::kOther, Mod::kOther, Mod::kOther, Mod::kOther, Mod::kOther,
                            Mod::kOther, Mod::kGame };
        const Mod at9[] = { Mod::kOther, Mod::kOther, Mod::kOther, Mod::kOther, Mod::kOther, Mod::kOther, Mod::kOther,
                            Mod::kOther, Mod::kOther, Mod::kGame };
        CHECK(run(at8).kind == Verdict::kWrapper);
        const Verdict v = run(at9);
        CHECK(v.kind == Verdict::kNotGame && has(v.reason, "not reached"));
    }
    // Never reaching the game, or nothing captured
    { const Mod c[] = { Mod::kOther, Mod::kOther }; const Verdict v = run(c); CHECK(v.kind == Verdict::kNotGame && has(v.reason, "not reached")); }
    { const Mod c[] = { Mod::kOther }; CHECK(run(c).kind == Verdict::kNotGame); }
    { const Verdict v = decide(nullptr, 0); CHECK(v.kind == Verdict::kNotGame && has(v.reason, "unreadable")); }
    // Every verdict that is not the game's says why
    { const Mod c[] = { Mod::kOther, Mod::kOther }; CHECK(run(c).reason && *run(c).reason); }

    // The window check on the wrapper path: a D3D11 device, a top-level window that is not a tool window, 320 x 200 or more
    {
        const Target ok = { true, true, false, 1280, 720 };
        CHECK(refuse(ok) == nullptr);
        Target t = ok; t.d3d11Device = false; CHECK(has(refuse(t), "D3D11"));
        t = ok; t.topLevel = false; CHECK(has(refuse(t), "top-level"));
        t = ok; t.toolWindow = true; CHECK(has(refuse(t), "tool window"));
        t = ok; t.clientW = 319; t.clientH = 200; CHECK(has(refuse(t), "too small"));
        t = ok; t.clientW = 320; t.clientH = 199; CHECK(has(refuse(t), "too small"));
        t = ok; t.clientW = 320; t.clientH = 200; CHECK(refuse(t) == nullptr);
        t = ok; t.clientW = 0; t.clientH = 0; CHECK(has(refuse(t), "too small"));
    }

    // Which module a path belongs to (case does not matter)
    {
        const wchar_t* sys = L"C:\\windows\\system32";
        const wchar_t* win = L"C:\\windows";
        const wchar_t* ours = L"D:\\Games\\FINAL FANTASY XV\\ffxv_dlss\\";
        CHECK(classify(L"", sys, win, ours) == Mod::kNone);
        CHECK(classify(L"D:\\Games\\FINAL FANTASY XV\\ffxv_dlss\\nvngx_dlss.dll", sys, win, ours) == Mod::kOurs);
        CHECK(classify(L"d:\\games\\final fantasy xv\\FFXV_DLSS\\libxess.dll", sys, win, ours) == Mod::kOurs);
        CHECK(classify(L"D:\\Games\\FINAL FANTASY XV\\ffxv_dlss\\sl.interposer.dll", sys, win, ours) == Mod::kOurs);
        CHECK(classify(L"D:\\elsewhere\\sl.interposer.dll", sys, win, ours) == Mod::kStreamline);
        CHECK(classify(L"D:\\elsewhere\\SL.dlss_g.dll", sys, win, ours) == Mod::kStreamline);
        CHECK(classify(L"C:\\Windows\\System32\\dxgi.dll", sys, win, ours) == Mod::kSystem);
        CHECK(classify(L"c:\\windows\\SYSTEM32\\d3d11.dll", sys, win, ours) == Mod::kSystem);
        CHECK(classify(L"C:\\windows\\WinSxS\\x\\comctl32.dll", sys, win, ours) == Mod::kSystem);
        CHECK(classify(L"D:\\Games\\FINAL FANTASY XV\\version.dll", sys, win, ours) == Mod::kOther);
        CHECK(classify(L"D:\\Games\\FINAL FANTASY XV\\winmm.dll", sys, win, ours) == Mod::kOther);
        CHECK(classify(L"C:\\windowsapps\\x.dll", sys, win, ours) == Mod::kOther);       // a folder that only starts like the Windows one
        CHECK(classify(L"D:\\Games\\FINAL FANTASY XV\\ffxv_dlss_old\\x.dll", sys, win, ours) == Mod::kOther);
        CHECK(classify(L"D:\\Games\\slow.dll", sys, win, ours) == Mod::kOther);          // "sl" without the dot
    }

    if (g_fail) { std::printf("%d checks failed\n", g_fail); return 1; }
    std::printf("swapchain_caller_test: all checks passed\n");
    return 0;
}
