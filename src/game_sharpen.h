// The game's own sharpening pass, which it draws after anti-aliasing: how it is recognised and what the menu's setting
// means. Kept free of Windows types so tests/game_sharpen_test.cpp covers it on the host.
#pragma once
#include <cstddef>
#include <cstdint>

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) of a block of bytes.
inline uint32_t crc32(const void* data, size_t len) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// CRC-32 of the bytecode of the game's sharpening pixel shader.
constexpr uint32_t kGameSharpenCrc = 0x0D1CD1AAu;

// The setting is a percent of the game's own strength, 0 (off) to 200; 100 is the game's look. A value missing from
// ffxv_dlss.ini or outside that range means 100.
inline int gameSharpenPercent(int raw, bool present) { return present && raw >= 0 && raw <= 200 ? raw : 100; }

// The setting as ffxv_dlss.ini holds it: a whole number 0 to 200 (the menu writes nothing else). Any other text, or
// none, means 100.
inline int gameSharpenIni(const wchar_t* text) {
    int v = 0, n = 0;
    for (; text[n] >= L'0' && text[n] <= L'9'; ++n) {
        if (n == 3) return 100;  // four digits or more: out of range
        v = v * 10 + (text[n] - L'0');
    }
    return gameSharpenPercent(v, n > 0 && !text[n]);
}

// The multiplier applied to the pass's strength.
inline float gameSharpenScale(int percent) { return float(percent) / 100.0f; }

// The pass is drawn but never through Draw, the only draw call the setting changes: the most used other call
// (index into Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, DrawInstancedIndirect,
// DrawIndexedInstancedIndirect), so the log can name it. -1 when Draw was used or nothing was drawn.
inline int gameSharpenOtherCall(const unsigned draws[6]) {
    if (draws[0]) return -1;
    int top = -1;
    for (int i = 1; i < 6; ++i) if (draws[i] && (top < 0 || draws[i] > draws[top])) top = i;
    return top;
}
