#include "platform/sdl/Ui.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "render/Renderer.h"

namespace gm::host {

namespace {

// 5x7 glyphs for ASCII 32-95 (lower case is drawn as upper case), one string of 7 rows each.
const char* const kGlyphs[64] = {
    /*   */ ".....|.....|.....|.....|.....|.....|.....",
    /* ! */ "..#..|..#..|..#..|..#..|..#..|.....|..#..",
    /* " */ ".#.#.|.#.#.|.....|.....|.....|.....|.....",
    /* # */ ".#.#.|.#.#.|#####|.#.#.|#####|.#.#.|.#.#.",
    /* $ */ "..#..|.####|#.#..|.###.|..#.#|####.|..#..",
    /* % */ "##..#|##..#|...#.|..#..|.#...|#..##|#..##",
    /* & */ ".##..|#..#.|#.#..|.#...|#.#.#|#..#.|.##.#",
    /* ' */ "..#..|..#..|.#...|.....|.....|.....|.....",
    /* ( */ "...#.|..#..|.#...|.#...|.#...|..#..|...#.",
    /* ) */ ".#...|..#..|...#.|...#.|...#.|..#..|.#...",
    /* * */ ".....|#.#.#|.###.|#####|.###.|#.#.#|.....",
    /* + */ ".....|..#..|..#..|#####|..#..|..#..|.....",
    /* , */ ".....|.....|.....|.....|.##..|..#..|.#...",
    /* - */ ".....|.....|.....|#####|.....|.....|.....",
    /* . */ ".....|.....|.....|.....|.....|.##..|.##..",
    /* / */ "....#|....#|...#.|..#..|.#...|#....|#....",
    /* 0 */ ".###.|#...#|#..##|#.#.#|##..#|#...#|.###.",
    /* 1 */ "..#..|.##..|..#..|..#..|..#..|..#..|.###.",
    /* 2 */ ".###.|#...#|....#|...#.|..#..|.#...|#####",
    /* 3 */ "####.|....#|....#|.###.|....#|....#|####.",
    /* 4 */ "...#.|..##.|.#.#.|#..#.|#####|...#.|...#.",
    /* 5 */ "#####|#....|####.|....#|....#|#...#|.###.",
    /* 6 */ ".###.|#....|#....|####.|#...#|#...#|.###.",
    /* 7 */ "#####|....#|...#.|..#..|.#...|.#...|.#...",
    /* 8 */ ".###.|#...#|#...#|.###.|#...#|#...#|.###.",
    /* 9 */ ".###.|#...#|#...#|.####|....#|....#|.###.",
    /* : */ ".....|.##..|.##..|.....|.##..|.##..|.....",
    /* ; */ ".....|.##..|.##..|.....|.##..|..#..|.#...",
    /* < */ "...#.|..#..|.#...|#....|.#...|..#..|...#.",
    /* = */ ".....|.....|#####|.....|#####|.....|.....",
    /* > */ ".#...|..#..|...#.|....#|...#.|..#..|.#...",
    /* ? */ ".###.|#...#|....#|...#.|..#..|.....|..#..",
    /* @ */ ".###.|#...#|#.###|#.#.#|#.###|#....|.###.",
    /* A */ ".###.|#...#|#...#|#####|#...#|#...#|#...#",
    /* B */ "####.|#...#|#...#|####.|#...#|#...#|####.",
    /* C */ ".###.|#...#|#....|#....|#....|#...#|.###.",
    /* D */ "###..|#..#.|#...#|#...#|#...#|#..#.|###..",
    /* E */ "#####|#....|#....|####.|#....|#....|#####",
    /* F */ "#####|#....|#....|####.|#....|#....|#....",
    /* G */ ".###.|#...#|#....|#.###|#...#|#...#|.####",
    /* H */ "#...#|#...#|#...#|#####|#...#|#...#|#...#",
    /* I */ ".###.|..#..|..#..|..#..|..#..|..#..|.###.",
    /* J */ "..###|...#.|...#.|...#.|...#.|#..#.|.##..",
    /* K */ "#...#|#..#.|#.#..|##...|#.#..|#..#.|#...#",
    /* L */ "#....|#....|#....|#....|#....|#....|#####",
    /* M */ "#...#|##.##|#.#.#|#.#.#|#...#|#...#|#...#",
    /* N */ "#...#|#...#|##..#|#.#.#|#..##|#...#|#...#",
    /* O */ ".###.|#...#|#...#|#...#|#...#|#...#|.###.",
    /* P */ "####.|#...#|#...#|####.|#....|#....|#....",
    /* Q */ ".###.|#...#|#...#|#...#|#.#.#|#..#.|.##.#",
    /* R */ "####.|#...#|#...#|####.|#.#..|#..#.|#...#",
    /* S */ ".####|#....|#....|.###.|....#|....#|####.",
    /* T */ "#####|..#..|..#..|..#..|..#..|..#..|..#..",
    /* U */ "#...#|#...#|#...#|#...#|#...#|#...#|.###.",
    /* V */ "#...#|#...#|#...#|#...#|#...#|.#.#.|..#..",
    /* W */ "#...#|#...#|#...#|#.#.#|#.#.#|#.#.#|.#.#.",
    /* X */ "#...#|#...#|.#.#.|..#..|.#.#.|#...#|#...#",
    /* Y */ "#...#|#...#|.#.#.|..#..|..#..|..#..|..#..",
    /* Z */ "#####|....#|...#.|..#..|.#...|#....|#####",
    /* [ */ ".###.|.#...|.#...|.#...|.#...|.#...|.###.",
    /* \ */ "#....|#....|.#...|..#..|...#.|....#|....#",
    /* ] */ ".###.|...#.|...#.|...#.|...#.|...#.|.###.",
    /* ^ */ "..#..|.#.#.|#...#|.....|.....|.....|.....",
    /* _ */ ".....|.....|.....|.....|.....|.....|#####",
};
constexpr double kTau = 6.283185307179586;
constexpr int kCellW = 6, kCellH = 8, kCols = 16, kRows = 4;  // atlas: 16 x 4 cells

int glyphIndex(unsigned char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 32);
    if (c < 32 || c > 95) c = '?';
    return c - 32;
}

}  // namespace

Painter::Painter(Renderer& r) : r_(r) {
    int w = kCols * kCellW, h = kRows * kCellH;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
    for (int g = 0; g < 64; g++) {
        int ox = (g % kCols) * kCellW, oy = (g / kCols) * kCellH;
        const char* s = kGlyphs[g];
        for (int row = 0, col = 0; *s; s++) {
            if (*s == '|') {
                row++, col = 0;
                continue;
            }
            if (*s == '#') {
                uint8_t* p = &px[(static_cast<size_t>(oy + row) * w + ox + col) * 4];
                p[0] = p[1] = p[2] = p[3] = 255;
            }
            col++;
        }
    }
    font_ = r_.createTexture(w, h, px.data());
}

void Painter::rect(double x, double y, double w, double h, uint32_t color, double alpha) {
    uint32_t c = packColor(color, alpha);
    float x0 = static_cast<float>(x), y0 = static_cast<float>(y), x1 = static_cast<float>(x + w), y1 = static_cast<float>(y + h);
    Vertex v[6] = {{x0, y0, 0, c, 0, 0}, {x1, y0, 0, c, 1, 0}, {x1, y1, 0, c, 1, 1},
                   {x0, y0, 0, c, 0, 0}, {x1, y1, 0, c, 1, 1}, {x0, y1, 0, c, 0, 1}};
    r_.setTexture(r_.whiteTexture());
    r_.add(Renderer::TRIANGLES, v, 6);
}

void Painter::frame(double x, double y, double w, double h, double t, uint32_t color, double alpha) {
    rect(x, y, w, t, color, alpha);
    rect(x, y + h - t, w, t, color, alpha);
    rect(x, y + t, t, h - 2 * t, color, alpha);
    rect(x + w - t, y + t, t, h - 2 * t, color, alpha);
}

void Painter::ring(double cx, double cy, double radius, double thickness, uint32_t color, double alpha) {
    uint32_t c = packColor(color, alpha);
    const int n = 40;
    double r0 = radius - thickness;
    r_.setTexture(r_.whiteTexture());
    for (int i = 0; i < n; i++) {
        double a0 = kTau * i / n, a1 = kTau * (i + 1) / n;
        auto pt = [&](double rad, double a) {
            return Vertex{static_cast<float>(cx + std::cos(a) * rad), static_cast<float>(cy + std::sin(a) * rad), 0, c, 0, 0};
        };
        Vertex v[6] = {pt(r0, a0), pt(radius, a0), pt(radius, a1), pt(r0, a0), pt(radius, a1), pt(r0, a1)};
        r_.add(Renderer::TRIANGLES, v, 6);
    }
}

void Painter::circle(double cx, double cy, double radius, uint32_t color, double alpha) {
    ring(cx, cy, radius, radius, color, alpha);
}

void Painter::text(double x, double y, double px, const std::string& s, uint32_t color, double alpha) {
    uint32_t c = packColor(color, alpha);
    const float aw = kCols * kCellW, ah = kRows * kCellH;
    r_.setTexture(font_);
    for (size_t i = 0; i < s.size(); i++) {
        int g = glyphIndex(static_cast<unsigned char>(s[i]));
        if (g == 0) continue;
        float u0 = (g % kCols) * kCellW / aw, v0 = (g / kCols) * kCellH / ah;
        float u1 = u0 + kCellW / aw, v1 = v0 + kCellH / ah;
        float x0 = static_cast<float>(x + i * kCellW * px), y0 = static_cast<float>(y);
        float x1 = x0 + static_cast<float>(kCellW * px), y1 = y0 + static_cast<float>(kCellH * px);
        Vertex v[6] = {{x0, y0, 0, c, u0, v0}, {x1, y0, 0, c, u1, v0}, {x1, y1, 0, c, u1, v1},
                       {x0, y0, 0, c, u0, v0}, {x1, y1, 0, c, u1, v1}, {x0, y1, 0, c, u0, v1}};
        r_.add(Renderer::TRIANGLES, v, 6);
    }
}

void Painter::textCentered(double cx, double cy, double px, const std::string& s, uint32_t color, double alpha) {
    text(cx - textWidth(s, px) / 2, cy - 7 * px / 2, px, s, color, alpha);
}

}  // namespace gm::host
