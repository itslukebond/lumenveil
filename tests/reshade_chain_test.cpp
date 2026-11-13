// Host test for src/reshade_chain.h: whether Lumenveil loads the ReShade it kept as dxgi.dll.before_ffxv_dlss, and the
// reason it logs when it does not. Expected values come from the spec (load=0 stops it, a missing kept file, and a kept
// file whose ProductName is not exactly "ReShade" after trimming, case ignored), not from the code.
#include "reshade_chain.h"
#include <cstdio>
#include <cstring>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

using reshade_chain::decide;
using reshade_chain::isReShade;

int main() {
    // the ProductName match
    CHECK(isReShade(L"ReShade"));
    CHECK(isReShade(L"reshade "));
    CHECK(isReShade(L" RESHADE\t"));
    CHECK(!isReShade(L"ReShade Effects"));
    CHECK(!isReShade(L""));
    CHECK(!isReShade(L"Special K"));
    CHECK(!isReShade(L"Re Shade"));
    CHECK(!isReShade(L"ReShad"));

    // loads only with the setting on, the kept file present and it being ReShade
    CHECK(decide(1, true, L"ReShade").load);
    CHECK(decide(1, true, L"reshade ").load);
    CHECK(!decide(0, true, L"ReShade").load);
    CHECK(!decide(1, false, L"ReShade").load);
    CHECK(!decide(1, true, L"").load);
    CHECK(!decide(1, true, L"OptiScaler").load);
    CHECK(!decide(1, true, L"ReShade Effects").load);

    // the reasons logged as "reshade: not loaded (<reason>)"
    CHECK(std::strcmp(decide(1, false, L"").reason, "no ReShade kept") == 0);
    CHECK(std::strcmp(decide(0, true, L"ReShade").reason, "load=0 in ffxv_dlss.ini") == 0);
    CHECK(std::strcmp(decide(0, false, L"").reason, "load=0 in ffxv_dlss.ini") == 0);  // the switch is named first
    CHECK(std::strcmp(decide(1, true, L"").reason, "the kept dxgi.dll is not ReShade") == 0);

    if (g_fail) { std::printf("%d checks failed\n", g_fail); return 1; }
    std::printf("reshade_chain_test: all checks passed\n");
    return 0;
}
