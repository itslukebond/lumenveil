// 2x HUD letter atlas, built in the mod from the game's own 1024² letter atlas (so no game data ships): every glyph box
// keeps its place at 2x. ASCII / Latin-1 glyphs are drawn from TeX Gyre Adventor Regular (ffxv_dlss\fonts), the rest
// (Greek, symbols, CJK) is bilinear 2x of the original. glyph_map.h holds the box positions and
// code points.
#include "common.h"
#include "glyph_map.h"
#include <algorithm>
#include <cmath>
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"  // Dear ImGui's copy of stb_truetype (public domain / MIT); reads the CFF .otf

static const int N = 1024, N2 = 2 * N;

static void decodeBC4(const unsigned char* bc4, unsigned char* a) {
    auto lerp = [](int p, int q, int i, int n) { return (2 * ((n - i) * p + i * q) + n) / (2 * n); };  // rounded, as the GPU decodes
    for (int by = 0; by < N / 4; ++by)
        for (int bx = 0; bx < N / 4; ++bx) {
            const unsigned char* b = bc4 + (size_t(by) * (N / 4) + bx) * 8;
            int r0 = b[0], r1 = b[1], pal[8] = { r0, r1, 0, 0, 0, 0, 0, 255 };
            for (int i = 1; i < (r0 > r1 ? 7 : 5); ++i) pal[1 + i] = lerp(r0, r1, i, r0 > r1 ? 7 : 5);
            unsigned long long bits = 0;
            for (int i = 0; i < 6; ++i) bits |= (unsigned long long)b[2 + i] << (8 * i);
            for (int p = 0; p < 16; ++p) a[(by * 4 + p / 4) * N + bx * 4 + p % 4] = (unsigned char)pal[(bits >> (3 * p)) & 7];
        }
}

std::vector<unsigned char> buildGlyph2x(const unsigned char* bc4, const std::vector<unsigned char>& font) {
    stbtt_fontinfo f;
    if (font.empty() || !stbtt_InitFont(&f, font.data(), stbtt_GetFontOffsetForIndex(font.data(), 0))) return {};
    std::vector<unsigned char> a(N * N), o(N2 * N2);
    decodeBC4(bc4, a.data());
    const int pad = kGlyphPad;
    auto at = [&](int x, int y) { return x < 0 || y < 0 || x >= N || y >= N ? 0.0 : double(a[y * N + x]); };
    for (const GlyphBox& b : kGlyphUp)  // bilinear 2x (sample centres at X/2 - 0.25)
        for (int Y = std::max(0, (b.y - pad) * 2); Y < std::min(N2, (b.y + b.h + pad) * 2); ++Y)
            for (int X = std::max(0, (b.x - pad) * 2); X < std::min(N2, (b.x + b.w + pad) * 2); ++X) {
                double sx = X / 2.0 - 0.25, sy = Y / 2.0 - 0.25;
                int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
                double fx = sx - x0, fy = sy - y0;
                double v = (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
                unsigned char& d = o[Y * N2 + X];
                d = std::max(d, (unsigned char)std::nearbyint(v));
            }
    const float scale = stbtt_ScaleForMappingEmToPixels(&f, float(kGlyphEm * 2));
    std::vector<unsigned char> bm;
    for (const GlyphBox& b : kGlyphFont) {
        int gi = stbtt_FindGlyphIndex(&f, b.cp), gx0, gy0, gx1, gy1;
        if (!gi || !stbtt_GetGlyphBox(&f, gi, &gx0, &gy0, &gx1, &gy1)) continue;
        const int ox = (b.x - pad) * 2, oy = (b.y - pad) * 2, w = (b.w + 2 * pad) * 2, h = (b.h + 2 * pad) * 2;
        // font point (u, v) lands at (u*scale + px, -v*scale + py): ink centre on the box centre
        const double px = ox + w / 2.0 - (gx0 + gx1) / 2.0 * scale, py = oy + h / 2.0 + (gy0 + gy1) / 2.0 * scale;
        const int ix = int(std::floor(px)), iy = int(std::floor(py));
        const float sx = float(px - ix), sy = float(py - iy);
        int bx0, by0, bx1, by1;
        stbtt_GetGlyphBitmapBoxSubpixel(&f, gi, scale, scale, sx, sy, &bx0, &by0, &bx1, &by1);
        const int bw = bx1 - bx0, bh = by1 - by0;
        if (bw <= 0 || bh <= 0) continue;
        bm.assign(size_t(bw) * bh, 0);
        stbtt_MakeGlyphBitmapSubpixel(&f, bm.data(), bw, bh, bw, scale, scale, sx, sy, gi);
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                const int X = ix + bx0 + x, Y = iy + by0 + y;
                if (X < std::max(0, ox) || Y < std::max(0, oy) || X >= std::min(N2, ox + w) || Y >= std::min(N2, oy + h)) continue;
                unsigned char& d = o[Y * N2 + X];
                d = std::max(d, bm[y * bw + x]);
            }
    }
    return o;
}
