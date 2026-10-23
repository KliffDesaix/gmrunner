#include "runtime/TrueType.h"

#include <algorithm>
#include <cmath>

// Private to this file: Borealis (the Switch game list) links its own copy.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace gm {

struct TrueTypeFace::Info {
    stbtt_fontinfo font{};
};

TrueTypeFace::TrueTypeFace() : info_(std::make_unique<Info>()) {}
TrueTypeFace::~TrueTypeFace() = default;

bool TrueTypeFace::load(const uint8_t* data, std::vector<uint8_t> owned, double points, bool bold, bool italic) {
    owned_ = std::move(owned);
    if (!owned_.empty()) data = owned_.data();
    if (!data) return false;
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&info_->font, data, offset)) return false;
    // GameMaker sizes fonts in points at 96 dpi: Arial 12 is a 16 px em, an 18 px line.
    double px = std::max(1.0, points) * 96.0 / 72.0;
    scale_ = stbtt_ScaleForMappingEmToPixels(&info_->font, static_cast<float>(px));
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&info_->font, &ascent, &descent, &gap);
    baseline_ = static_cast<int>(std::lround(ascent * scale_));
    lineHeight_ = std::max(1, static_cast<int>(std::lround((ascent - descent) * scale_)));
    // A bold request on a face that isn't bold (the macStyle bit in its head table) is drawn
    // one pixel thicker.
    int head = static_cast<int>(stbtt__find_table(info_->font.data, info_->font.fontstart, "head"));
    int macStyle = head ? ttUSHORT(info_->font.data + head + 44) : 0;
    fakeBold_ = bold && !(macStyle & 1);
    slant_ = italic && !(macStyle & 2) ? 0.21 : 0;  // about 12 degrees, as systems slant a regular face
    atlasW_ = std::min(2048, std::max(256, 1 << static_cast<int>(std::ceil(std::log2(lineHeight_ * 16.0)))));
    atlasH_ = std::max(64, atlasW_ / 4);
    pixels_.assign(static_cast<size_t>(atlasW_) * atlasH_ * 4, 0);
    for (size_t i = 0; i < pixels_.size(); i += 4) pixels_[i] = pixels_[i + 1] = pixels_[i + 2] = 255;
    dirty = true;
    return true;
}

bool TrueTypeFace::has(char32_t c) const {
    return c == U' ' || stbtt_FindGlyphIndex(&info_->font, static_cast<int>(c)) != 0;
}

TrueTypeFace::Glyph TrueTypeFace::bake(char32_t c) {
    Glyph g;
    int cp = static_cast<int>(c);
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&info_->font, cp, &advance, &lsb);
    g.advance = std::lround(advance * scale_);  // GameMaker's fake bold keeps the advance
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&info_->font, cp, static_cast<float>(scale_), static_cast<float>(scale_), &x0, &y0, &x1, &y1);
    int bw = x1 - x0, bh = y1 - y0;
    g.offset = x0;
    if (bw <= 0 || bh <= 0) return g;
    // A slanted glyph leans right above the baseline and left below it; the cell widens to fit
    // and starts further left by the most the descent leans.
    int lean = static_cast<int>(std::ceil((lineHeight_ - baseline_) * slant_));
    int widen = static_cast<int>(std::ceil(lineHeight_ * slant_));
    g.offset -= lean;
    int cellW = bw + (fakeBold_ ? 1 : 0) + widen;
    // Rows of cells as tall as the line; a full atlas grows downwards.
    if (penX_ + cellW + 1 > atlasW_) penX_ = 1, penY_ += lineHeight_ + 1;
    while (penY_ + lineHeight_ + 1 > atlasH_) {
        int grown = atlasH_ * 2;
        std::vector<uint8_t> more(static_cast<size_t>(atlasW_) * grown * 4, 0);
        for (size_t i = 0; i < more.size(); i += 4) more[i] = more[i + 1] = more[i + 2] = 255;
        std::copy(pixels_.begin(), pixels_.end(), more.begin());
        pixels_ = std::move(more);
        atlasH_ = grown;
    }
    std::vector<uint8_t> bitmap(static_cast<size_t>(bw) * bh);
    stbtt_MakeCodepointBitmap(&info_->font, bitmap.data(), bw, bh, bw, static_cast<float>(scale_), static_cast<float>(scale_), cp);
    int top = baseline_ + y0;  // the glyph's first row inside its cell
    auto coverage = [&](int y, int x) {  // the upright glyph, thickened when bold is faked
        if (x < 0 || x > bw - 1 + (fakeBold_ ? 1 : 0)) return 0;
        int a = x < bw ? bitmap[static_cast<size_t>(y) * bw + x] : 0;
        if (fakeBold_ && x > 0) a = std::max(a, static_cast<int>(bitmap[static_cast<size_t>(y) * bw + x - 1]));
        return a;
    };
    for (int y = 0; y < bh; y++) {
        int cy = top + y;
        if (cy < 0 || cy >= lineHeight_) continue;
        // Slant: this row moves right by `shift` pixels (fractional, blended between neighbours).
        double shift = lean + (baseline_ - cy - 0.5) * slant_;
        int whole = static_cast<int>(std::floor(shift));
        double frac = shift - whole;
        for (int x = 0; x < cellW; x++) {
            int sx = x - whole;
            int a = slant_ > 0 ? static_cast<int>(std::lround(coverage(y, sx) * (1 - frac) + coverage(y, sx - 1) * frac))
                               : coverage(y, x);
            pixels_[(static_cast<size_t>(penY_ + cy) * atlasW_ + penX_ + x) * 4 + 3] = static_cast<uint8_t>(a);
        }
    }
    g.x = penX_, g.y = penY_, g.w = cellW, g.h = lineHeight_;
    penX_ += cellW + 1;
    dirty = true;
    return g;
}

}  // namespace gm
