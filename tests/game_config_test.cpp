// Host test for the folder the Insert menu looks in for the game's settings (src/game_config.h). Built and run by
// tests/check_game_config.sh. The game keeps GraphicsConfig.ini under USERPROFILE's Documents folder even when Windows
// has moved the Documents folder (GitHub #3); expected strings are written out by hand.
#include "game_config.h"
#include <cstdio>
#include <cwchar>

// Stands in for GetEnvironmentVariableW(L"USERPROFILE", ...): the size needed (terminator included) when the buffer is
// too small, else the length copied (terminator excluded); 0 when unset. `later` is the value at the second call.
struct FakeEnv {
    const wchar_t* first; const wchar_t* later; int calls = 0;
    unsigned long operator()(wchar_t* buf, unsigned long size) {
        const wchar_t* v = calls++ ? later : first;
        if (!v) return 0;
        const unsigned long len = (unsigned long)std::wcslen(v);
        if (!buf || size <= len) return len + 1;
        std::wmemcpy(buf, v, len + 1);
        return len;
    }
};

static int g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fails; } } while (0)

int main() {
    const std::wstring want = L"C:\\Users\\Noctis\\Documents\\My Games\\FINAL FANTASY XV\\Steam\\";
    CHECK(gameSettingsRoot(L"C:\\Users\\Noctis") == want, "plain profile folder");
    CHECK(gameSettingsRoot(L"C:\\Users\\Noctis\\") == want, "trailing backslash");
    CHECK(gameSettingsRoot(L"C:\\Users\\Noctis/") == want, "trailing slash");
    CHECK(gameSettingsRoot(L"").empty(), "no profile folder gives no path");

    CHECK(envValue(FakeEnv{L"C:\\Users\\Noctis", L"C:\\Users\\Noctis"}) == L"C:\\Users\\Noctis", "USERPROFILE read");
    CHECK(gameSettingsRoot(envValue(FakeEnv{L"C:\\Users\\Noctis", L"C:\\Users\\Noctis"})) == want, "menu folder from USERPROFILE");
    CHECK(envValue(FakeEnv{nullptr, nullptr}).empty(), "USERPROFILE unset");
    CHECK(envValue(FakeEnv{L"", L""}).empty(), "USERPROFILE empty");
    CHECK(envValue(FakeEnv{L"C:\\Users\\Noctis", L"C:\\Users\\Noctis.Lucis"}).empty(), "USERPROFILE grew between the calls");
    CHECK(envValue(FakeEnv{L"C:\\Users\\Noctis", L"C:\\U"}) == L"C:\\U", "USERPROFILE shrank between the calls");
    if (!g_fails) std::printf("game_config_test: all passed\n");
    return g_fails ? 1 : 0;
}
