// Minimal drawing for the host's own screens (game picker, on-screen controls): filled
// rectangles and circles, and text in a built-in 5x7 pixel font. Draws through a Renderer whose
// projection is set to screen pixels.
#pragma once

#include <cstdint>
#include <string>

namespace gm {

class Renderer;

namespace host {

constexpr uint32_t rgb(int r, int g, int b) { return static_cast<uint32_t>(r | g << 8 | b << 16); }  // GameMaker's BGR

class Painter {
public:
    explicit Painter(Renderer& r);
    void rect(double x, double y, double w, double h, uint32_t color, double alpha);
    void frame(double x, double y, double w, double h, double thickness, uint32_t color, double alpha);
    void circle(double cx, double cy, double radius, uint32_t color, double alpha);
    void ring(double cx, double cy, double radius, double thickness, uint32_t color, double alpha);
    // Text with its top-left at x, y; each glyph cell is 6 x 8 font pixels of `px` screen pixels.
    void text(double x, double y, double px, const std::string& s, uint32_t color, double alpha);
    void textCentered(double cx, double cy, double px, const std::string& s, uint32_t color, double alpha);
    static double textWidth(const std::string& s, double px) { return s.size() * 6 * px - px; }

private:
    Renderer& r_;
    uint32_t font_ = 0;
};

}  // namespace host
}  // namespace gm
