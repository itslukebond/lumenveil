#pragma once
// The colour space our swap chain presents in after its format changes or it is replaced (host-tested by
// tests/check_colour_space.sh). Our swap chain starts at R16G16B16A16_FLOAT, which Windows shows as scRGB (linear), and
// is resized to the game's SDR format; nothing set its colour space afterwards. A washed-out picture on Windows
// (GitHub #5: raised blacks, pale sky, HUD lifted by the same curve) fits sRGB-encoded pixels shown as linear, so the
// colour space is set explicitly after a format change. Values are DXGI_COLOR_SPACE_TYPE and DXGI_FORMAT numbers
// (checked against the enums in present12.cpp).
namespace colour {
constexpr unsigned kRgba16Float = 10;  // DXGI_FORMAT_R16G16B16A16_FLOAT
constexpr unsigned kSrgb = 0;          // DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
constexpr unsigned kScRgb = 1;         // DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709

// gameSpace: the colour space the game set on its current swap chain, -1 if none. supported: our swap chain can present
// in it at the new format. Otherwise the format's own default: scRGB for FP16, sRGB for the rest.
inline unsigned presentColourSpace(unsigned format, int gameSpace, bool supported) {
    if (gameSpace >= 0 && supported) return unsigned(gameSpace);
    return format == kRgba16Float ? kScRgb : kSrgb;
}
}  // namespace colour
