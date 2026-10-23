// TrueType fonts made at run time (font_add, and the default font for text drawn without one):
// glyphs rasterized with stb_truetype into an atlas as text needs them, laid out the way
// GameMaker's own font pages are (every glyph a cell as tall as the line, the baseline inside it).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace gm {

class TrueTypeFace {
public:
    struct Glyph {
        int x = 0, y = 0, w = 0, h = 0;  // cell in the atlas (w 0: nothing to draw, like the space)
        double offset = 0;               // left bearing
        double advance = 0;
    };

    TrueTypeFace();
    ~TrueTypeFace();
    // `data` must outlive the face unless `owned` holds it. `points` is GameMaker's font size
    // (96 dpi); `bold` thickens a face that isn't bold itself, `italic` slants one that isn't italic.
    bool load(const uint8_t* data, std::vector<uint8_t> owned, double points, bool bold, bool italic = false);
    bool has(char32_t c) const;
    Glyph bake(char32_t c);  // rasterizes into the atlas
    int lineHeight() const { return lineHeight_; }

    // The atlas (white, coverage in alpha), and whether it changed since the last upload.
    const std::vector<uint8_t>& pixels() const { return pixels_; }
    int atlasWidth() const { return atlasW_; }
    int atlasHeight() const { return atlasH_; }
    bool dirty = false;

private:
    struct Info;
    std::unique_ptr<Info> info_;
    std::vector<uint8_t> owned_;
    double scale_ = 1;
    int baseline_ = 0, lineHeight_ = 0;
    bool fakeBold_ = false;
    double slant_ = 0;  // synthetic italic: pixels of shift per row above the baseline
    std::vector<uint8_t> pixels_;
    int atlasW_ = 0, atlasH_ = 0, penX_ = 1, penY_ = 1;
};

}  // namespace gm
