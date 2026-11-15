// Host test for src/game_sharpen.h: recognising the game's sharpening pass by a CRC-32 of its bytecode, and the
// setting's rules (0 to 200 percent, default 100 when missing or out of range). Expected values are the standard
// CRC-32 check values and the numbers written out here, not the code's.
#include "game_sharpen.h"
#include <cstdio>
#include <cstring>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    // CRC-32 (IEEE 802.3, reflected): the published check value of "123456789", and the empty input
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
    CHECK(crc32("", 0) == 0u);
    CHECK(crc32("a", 1) == 0xE8B7BE43u);
    CHECK(crc32("The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u);
    unsigned char zeros[32] = {};
    CHECK(crc32(zeros, sizeof zeros) == 0x190A55ADu);
    CHECK(kGameSharpenCrc == 0x0D1CD1AAu);

    // The ini value: present and in range is kept; missing, below 0 or above 200 means the game's look (100)
    CHECK(gameSharpenPercent(0, true) == 0);
    CHECK(gameSharpenPercent(1, true) == 1);
    CHECK(gameSharpenPercent(55, true) == 55);
    CHECK(gameSharpenPercent(100, true) == 100);
    CHECK(gameSharpenPercent(200, true) == 200);
    CHECK(gameSharpenPercent(201, true) == 100);
    CHECK(gameSharpenPercent(-1, true) == 100);
    CHECK(gameSharpenPercent(-2147483647 - 1, true) == 100);
    CHECK(gameSharpenPercent(0, false) == 100);
    CHECK(gameSharpenPercent(150, false) == 100);

    // The ini text: whole numbers 0 to 200 only; anything else (empty, words, a sign, a fraction, too big) means 100
    CHECK(gameSharpenIni(L"150") == 150);
    CHECK(gameSharpenIni(L"0") == 0);
    CHECK(gameSharpenIni(L"200") == 200);
    CHECK(gameSharpenIni(L"007") == 7);
    CHECK(gameSharpenIni(L"") == 100);
    CHECK(gameSharpenIni(L"abc") == 100);
    CHECK(gameSharpenIni(L"12a") == 100);
    CHECK(gameSharpenIni(L"-5") == 100);
    CHECK(gameSharpenIni(L"1.5") == 100);
    CHECK(gameSharpenIni(L"201") == 100);
    CHECK(gameSharpenIni(L"99999999999999999999") == 100);

    // The multiplier handed to the pass
    CHECK(gameSharpenScale(0) == 0.0f);
    CHECK(gameSharpenScale(100) == 1.0f);
    CHECK(gameSharpenScale(200) == 2.0f);
    CHECK(gameSharpenScale(50) == 0.5f);

    // The draw call to name when the pass is drawn but never through Draw (the only call the setting changes):
    // the most used other call; none when Draw was used or nothing was drawn. Order: Draw, DrawIndexed,
    // DrawInstanced, DrawIndexedInstanced, DrawInstancedIndirect, DrawIndexedInstancedIndirect.
    { const unsigned d[6] = { 0, 0, 0, 0, 0, 0 }; CHECK(gameSharpenOtherCall(d) == -1); }
    { const unsigned d[6] = { 5, 0, 0, 0, 0, 0 }; CHECK(gameSharpenOtherCall(d) == -1); }
    { const unsigned d[6] = { 3, 9, 0, 0, 0, 0 }; CHECK(gameSharpenOtherCall(d) == -1); }
    { const unsigned d[6] = { 0, 600, 0, 0, 0, 0 }; CHECK(gameSharpenOtherCall(d) == 1); }
    { const unsigned d[6] = { 0, 0, 600, 0, 0, 0 }; CHECK(gameSharpenOtherCall(d) == 2); }
    { const unsigned d[6] = { 0, 2, 0, 7, 0, 0 }; CHECK(gameSharpenOtherCall(d) == 3); }
    { const unsigned d[6] = { 0, 0, 0, 0, 0, 1 }; CHECK(gameSharpenOtherCall(d) == 5); }

    if (g_fail) { std::printf("%d checks failed\n", g_fail); return 1; }
    std::printf("game_sharpen_test: all checks passed\n");
    return 0;
}
