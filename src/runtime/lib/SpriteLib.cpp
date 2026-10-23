// Run-time sprites (sprite_add, sprite_create_from_surface, sprite_delete, ...), sprite settings
// (offset, speed, bounding box, collision mask) and surface pixel access (surface_getpixel,
// buffer_get_surface, surface_save). New sprite frames get their own texture pages.
#include <algorithm>
#include <cstring>
#include <deque>

#include "runtime/Builtins.h"
#include "runtime/Graphics.h"
#include "runtime/Stores.h"
#include "util/Stb.h"
#include "util/Png.h"

namespace gm {

#define G (*rt.gfx)

namespace {

std::deque<std::string> g_names;                 // names of run-time sprites (Sprite::name is a view)
std::deque<std::vector<uint8_t>> g_masks;        // their precise collision masks
std::unordered_map<int32_t, std::vector<uint8_t>> g_framePixels;  // tpag -> RGBA of run-time frames

Sprite* sprite(Runner& rt, int id) {
    return id >= 0 && static_cast<size_t>(id) < rt.dw.sprites.size() && !rt.dw.sprites[id].deleted
               ? &rt.dw.sprites[id] : nullptr;
}

// GameMaker's "remove background": pixels matching the bottom-left one become transparent.
void removeBackground(std::vector<uint8_t>& px, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const uint8_t* k = &px[static_cast<size_t>(h - 1) * w * 4];
    uint8_t r = k[0], g = k[1], b = k[2];
    for (size_t i = 0; i < px.size(); i += 4)
        if (px[i] == r && px[i + 1] == g && px[i + 2] == b) px[i + 3] = 0;
}

// A frame (w x h RGBA) as a new texture page item.
int32_t addFrame(Runner& rt, std::vector<uint8_t> px, int w, int h) {
    TexturePageItem t{};
    t.srcW = t.tgtW = t.boundW = static_cast<uint16_t>(w);
    t.srcH = t.tgtH = t.boundH = static_cast<uint16_t>(h);
    int32_t tp = static_cast<int32_t>(rt.dw.tpag.size());
    g_framePixels[tp] = px;
    t.texture = static_cast<int16_t>(G.addPage(w, h, std::move(px)));
    rt.dw.tpag.push_back(t);
    return tp;
}

// Bounding box from the opaque pixels of all frames (automatic mode).
void autoBBox(Runner& rt, Sprite& s) {
    int l = static_cast<int>(s.width), t = static_cast<int>(s.height), r = -1, b = -1;
    for (int32_t tp : s.frames) {
        auto it = g_framePixels.find(tp);
        if (it == g_framePixels.end()) continue;
        const auto& px = it->second;
        int w = rt.dw.tpag[tp].srcW, h = rt.dw.tpag[tp].srcH;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (px[(static_cast<size_t>(y) * w + x) * 4 + 3] > 0) {
                    l = std::min(l, x), r = std::max(r, x), t = std::min(t, y), b = std::max(b, y);
                }
    }
    if (r < 0) l = t = r = b = 0;
    s.marginLeft = l;
    s.marginTop = t;
    s.marginRight = r;
    s.marginBottom = b;
}

// Precise masks (alpha > tolerance) for run-time frames; other sprites keep theirs.
void buildMasks(Runner& rt, Sprite& s, int tolerance) {
    std::vector<const uint8_t*> masks;
    int rowBytes = (static_cast<int>(s.width) + 7) / 8;
    for (int32_t tp : s.frames) {
        auto it = g_framePixels.find(tp);
        if (it == g_framePixels.end()) return;
        const auto& px = it->second;
        int w = rt.dw.tpag[tp].srcW, h = rt.dw.tpag[tp].srcH;
        std::vector<uint8_t> m(static_cast<size_t>(rowBytes) * s.height, 0);
        for (int y = 0; y < h && y < static_cast<int>(s.height); y++)
            for (int x = 0; x < w && x < static_cast<int>(s.width); x++)
                if (px[(static_cast<size_t>(y) * w + x) * 4 + 3] > tolerance) m[y * rowBytes + x / 8] |= 0x80 >> (x % 8);
        g_masks.push_back(std::move(m));
        masks.push_back(g_masks.back().data());
    }
    s.masks = std::move(masks);
}

int newSprite(Runner& rt, int w, int h, int xorig, int yorig) {
    Sprite s;
    g_names.push_back("__newsprite" + std::to_string(rt.dw.sprites.size()));
    s.name = g_names.back();
    s.width = static_cast<uint32_t>(w);
    s.height = static_cast<uint32_t>(h);
    s.originX = xorig;
    s.originY = yorig;
    s.transparent = true;
    s.playbackSpeed = 1;
    s.playbackSpeedType = 1;
    rt.dw.sprites.push_back(s);
    return static_cast<int>(rt.dw.sprites.size() - 1);
}

// Splits an image strip into `count` frames.
bool loadStrip(Runner& rt, const std::string& path, int count, bool removeBack, std::vector<std::vector<uint8_t>>& frames,
               int& fw, int& fh) {
    std::string data;
    FILE* f = std::fopen(rt.files->readPath(path).c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    std::fclose(f);
    int w, h, ch;
    uint8_t* px = stb::loadImage(reinterpret_cast<const uint8_t*>(data.data()), static_cast<int>(data.size()),
                                        &w, &h, &ch, 4);
    if (!px) return false;
    count = std::max(1, count);
    fw = std::max(1, w / count);
    fh = h;
    for (int i = 0; i < count; i++) {
        std::vector<uint8_t> fr(static_cast<size_t>(fw) * fh * 4);
        for (int y = 0; y < fh; y++)
            std::memcpy(&fr[static_cast<size_t>(y) * fw * 4], px + (static_cast<size_t>(y) * w + i * fw) * 4,
                        static_cast<size_t>(fw) * 4);
        if (removeBack) removeBackground(fr, fw, fh);
        frames.push_back(std::move(fr));
    }
    stb::freeImage(px);
    return true;
}

}  // namespace

GMFN(sprite_create_from_surface) {  // (surface, x, y, w, h, removeback, smooth, xorig, yorig)
    std::vector<uint8_t> px;
    if (!G.readSurface(I(0), I(1), I(2), I(3), I(4), px)) return Value(-1.0);
    int w = std::min(I(3), static_cast<int>(px.size() / 4 / std::max(1, I(4)))), h = I(4);
    if (B(5)) removeBackground(px, w, h);
    int id = newSprite(rt, w, h, I(7), I(8));
    Sprite& s = rt.dw.sprites[id];
    s.frames.push_back(addFrame(rt, std::move(px), w, h));
    autoBBox(rt, s);
    return Value(static_cast<double>(id));
}
GMFN(sprite_add_from_surface) {  // (sprite, surface, x, y, w, h, removeback, smooth)
    Sprite* s = sprite(rt, I(0));
    std::vector<uint8_t> px;
    if (!s || !G.readSurface(I(1), I(2), I(3), I(4), I(5), px)) return Value(-1.0);
    int w = I(4), h = I(5);
    if (B(6)) removeBackground(px, w, h);
    s->frames.push_back(addFrame(rt, std::move(px), w, h));
    return Value(static_cast<double>(s->frames.size()));
}
GMFN(sprite_add) {  // (fname, imgnumb, removeback, smooth, xorig, yorig)
    std::vector<std::vector<uint8_t>> frames;
    int fw, fh;
    if (!loadStrip(rt, S(0), I(1), B(2), frames, fw, fh)) return Value(-1.0);
    int id = newSprite(rt, fw, fh, I(4), I(5));
    Sprite& s = rt.dw.sprites[id];
    for (auto& f : frames) s.frames.push_back(addFrame(rt, std::move(f), fw, fh));
    autoBBox(rt, s);
    return Value(static_cast<double>(id));
}
// sprite_add_ext(fname, imgnumb, xorig, yorig, prefetch): loaded at once here, then reported to
// the game through the Async Image Loaded event (async_load: filename, id, status) as it expects.
GMFN(sprite_add_ext) {
    std::vector<std::vector<uint8_t>> frames;
    int fw, fh, id = -1;
    if (loadStrip(rt, S(0), I(1), false, frames, fw, fh)) {
        id = newSprite(rt, fw, fh, I(2), I(3));
        Sprite& s = rt.dw.sprites[id];
        for (auto& f : frames) s.frames.push_back(addFrame(rt, std::move(f), fw, fh));
        autoBBox(rt, s);
    }
    rt.queueAsync(60, {{"filename", Value(S(0))}, {"id", Value(static_cast<double>(id))},
                       {"status", Value(id >= 0 ? 0.0 : -1.0)}});
    return Value(static_cast<double>(id));
}
GMFN(sprite_replace) {  // (index, fname, imgnumb, removeback, smooth, xorig, yorig)
    Sprite* s = sprite(rt, I(0));
    std::vector<std::vector<uint8_t>> frames;
    int fw, fh;
    if (!s || !loadStrip(rt, S(1), I(2), B(3), frames, fw, fh)) return Value();
    s->frames.clear();
    s->masks.clear();
    s->width = static_cast<uint32_t>(fw);
    s->height = static_cast<uint32_t>(fh);
    s->originX = I(5);
    s->originY = I(6);
    for (auto& f : frames) s->frames.push_back(addFrame(rt, std::move(f), fw, fh));
    autoBBox(rt, *s);
    return Value();
}
GMFN(sprite_delete) {
    if (Sprite* s = sprite(rt, I(0))) {
        s->deleted = true;
        s->frames.clear();
        s->masks.clear();
        return boolV(true);
    }
    return boolV(false);
}
GMFN(sprite_duplicate) {
    Sprite* s = sprite(rt, I(0));
    if (!s) return Value(-1.0);
    Sprite copy = *s;
    g_names.push_back("__newsprite" + std::to_string(rt.dw.sprites.size()));
    copy.name = g_names.back();
    rt.dw.sprites.push_back(copy);
    return Value(static_cast<double>(rt.dw.sprites.size() - 1));
}
GMFN(sprite_assign) {  // (dest, src): dest becomes a copy of src
    Sprite* d = sprite(rt, I(0));
    Sprite* src = sprite(rt, I(1));
    if (!d || !src) return Value();
    std::string_view name = d->name;
    *d = *src;
    d->name = name;
    return Value();
}
GMFN(sprite_merge) {  // (ind1, ind2): appends ind2's frames to ind1
    Sprite* a = sprite(rt, I(0));
    Sprite* b = sprite(rt, I(1));
    if (!a || !b) return Value();
    a->frames.insert(a->frames.end(), b->frames.begin(), b->frames.end());
    if (!a->masks.empty() && !b->masks.empty()) a->masks.insert(a->masks.end(), b->masks.begin(), b->masks.end());
    else a->masks.clear();
    return Value();
}
GMFN(sprite_set_offset) {
    if (Sprite* s = sprite(rt, I(0))) {
        s->originX = I(1);
        s->originY = I(2);
    }
    return Value();
}
GMFN(sprite_get_speed) { Sprite* s = sprite(rt, I(0)); return Value(s ? static_cast<double>(s->playbackSpeed) : 0.0); }
GMFN(sprite_get_speed_type) {
    Sprite* s = sprite(rt, I(0));
    return Value(s ? static_cast<double>(s->playbackSpeedType) : 0.0);
}
GMFN(sprite_set_speed) {  // (sprite, speed, type): spritespeed_framespersecond 0 / framespergameframe 1
    if (Sprite* s = sprite(rt, I(0))) {
        s->playbackSpeed = static_cast<float>(D(1));
        s->playbackSpeedType = static_cast<uint32_t>(I(2));
    }
    return Value();
}
GMFN(sprite_get_bbox_mode) { Sprite* s = sprite(rt, I(0)); return Value(s ? static_cast<double>(s->bboxMode) : 0.0); }
GMFN(sprite_set_bbox_mode) {  // bboxmode_automatic 0, fullimage 1, manual 2
    Sprite* s = sprite(rt, I(0));
    if (!s) return Value();
    s->bboxMode = static_cast<uint32_t>(I(1));
    if (I(1) == 1) {
        s->marginLeft = s->marginTop = 0;
        s->marginRight = static_cast<int32_t>(s->width) - 1;
        s->marginBottom = static_cast<int32_t>(s->height) - 1;
    } else if (I(1) == 0) {
        autoBBox(rt, *s);
    }
    return Value();
}
GMFN(sprite_set_bbox) {  // (sprite, left, top, right, bottom): manual mode only
    Sprite* s = sprite(rt, I(0));
    if (!s || s->bboxMode != 2) return Value();
    s->marginLeft = I(1);
    s->marginTop = I(2);
    s->marginRight = I(3);
    s->marginBottom = I(4);
    return Value();
}
// (sprite, sepmasks, bboxmode, left, top, right, bottom, kind, tolerance); kind 0 precise,
// 1 rectangle, 2 ellipse, 3 diamond, 4 precise per frame, 5 rotated rectangle.
GMFN(sprite_collision_mask) {
    Sprite* s = sprite(rt, I(0));
    if (!s) return Value();
    s->sepMasks = B(1);
    s->bboxMode = static_cast<uint32_t>(I(2));
    if (I(2) == 1) {
        s->marginLeft = s->marginTop = 0;
        s->marginRight = static_cast<int32_t>(s->width) - 1;
        s->marginBottom = static_cast<int32_t>(s->height) - 1;
    } else if (I(2) == 2) {
        s->marginLeft = I(3);
        s->marginTop = I(4);
        s->marginRight = I(5);
        s->marginBottom = I(6);
    } else {
        autoBBox(rt, *s);
    }
    int kind = I(7);
    if (kind == 0 || kind == 4) buildMasks(rt, *s, I(8));
    else s->masks.clear();  // rectangle-like kinds use the bounding box
    return Value();
}
GMFN(sprite_save) {  // (sprite, subimg, fname)
    Sprite* s = sprite(rt, I(0));
    if (!s || s->frames.empty()) return Value();
    int tp = s->frames[static_cast<size_t>(std::max(0, I(1))) % s->frames.size()];
    auto it = g_framePixels.find(tp);
    std::vector<uint8_t> px;
    int w = rt.dw.tpag[tp].srcW, h = rt.dw.tpag[tp].srcH;
    if (it != g_framePixels.end()) {
        px = it->second;
    } else {
        // Data sprites: render the frame into a scratch surface and read it back.
        int surf = G.surfaceCreate(static_cast<int>(s->width), static_cast<int>(s->height));
        G.surfaceSetTarget(surf);
        G.drawClear(0, 0);
        G.drawSprite(I(0), I(1), s->originX, s->originY, 1, 1, 0, 0xFFFFFF, 1);
        G.surfaceResetTarget();
        w = static_cast<int>(s->width);
        h = static_cast<int>(s->height);
        G.readSurface(surf, 0, 0, w, h, px);
        G.surfaceFree(surf);
    }
    if (!px.empty()) writePng(rt.files->writePath(S(2)), px.data(), w, h);
    return Value();
}

// ---- surface pixels
GMFN(surface_getpixel) {  // colour (BGR) at x, y
    std::vector<uint8_t> px;
    if (!G.readSurface(I(0), I(1), I(2), 1, 1, px)) return Value(0.0);
    return Value(static_cast<double>(px[0] | px[1] << 8 | px[2] << 16));
}
GMFN(surface_getpixel_ext) {  // colour with alpha in the top byte
    std::vector<uint8_t> px;
    if (!G.readSurface(I(0), I(1), I(2), 1, 1, px)) return Value(0.0);
    return Value(static_cast<double>(static_cast<uint32_t>(px[0] | px[1] << 8 | px[2] << 16) |
                                     static_cast<uint32_t>(px[3]) << 24));
}
// buffer_get_surface(buffer, surface, offset) (GMS 1.4: buffer, surface, mode, offset, modulo)
GMFN(buffer_get_surface) {
    std::vector<uint8_t>* b = bufferBytes(I(0));
    int surf = I(1);
    if (!b || !G.surfaceExists(surf)) return Value();
    std::vector<uint8_t> px;
    G.readSurface(surf, 0, 0, G.surfaceWidth(surf), G.surfaceHeight(surf), px);
    size_t off = static_cast<size_t>(std::max(0, argc >= 5 ? I(3) : I(2)));
    if (b->size() < off + px.size()) b->resize(off + px.size());
    std::memcpy(b->data() + off, px.data(), px.size());
    return Value();
}
GMFN(buffer_set_surface) {
    std::vector<uint8_t>* b = bufferBytes(I(0));
    int surf = I(1);
    if (!b || !G.surfaceExists(surf)) return Value();
    int w = G.surfaceWidth(surf), h = G.surfaceHeight(surf);
    size_t off = static_cast<size_t>(std::max(0, argc >= 5 ? I(3) : I(2)));
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
    if (off < b->size()) std::memcpy(px.data(), b->data() + off, std::min(px.size(), b->size() - off));
    G.writeSurface(surf, px.data(), w, h);
    return Value();
}
GMFN(surface_save) {
    std::vector<uint8_t> px;
    int s = I(0);
    if (G.readSurface(s, 0, 0, G.surfaceWidth(s), G.surfaceHeight(s), px))
        writePng(rt.files->writePath(S(1)), px.data(), G.surfaceWidth(s), G.surfaceHeight(s));
    return Value();
}
GMFN(surface_save_part) {  // (surface, fname, x, y, w, h)
    std::vector<uint8_t> px;
    if (G.readSurface(I(0), I(2), I(3), I(4), I(5), px)) writePng(rt.files->writePath(S(1)), px.data(), I(4), I(5));
    return Value();
}

// screen_save(fname) / screen_save_part(fname, x, y, w, h): the game's picture as a PNG (the
// application surface, or the screen when the game turned it off).
static bool screenPixels(Runner& rt, int x, int y, int w, int h, std::vector<uint8_t>& px, int& outW, int& outH) {
    int app = G.applicationSurface();
    if (G.surfaceExists(app)) {
        if (w < 0) w = G.surfaceWidth(app), h = G.surfaceHeight(app);
        outW = w, outH = h;
        return G.readSurface(app, x, y, w, h, px);
    }
    int bw, bh;
    std::vector<uint8_t> all;
    if (!G.readBackbuffer(all, bw, bh)) return false;
    if (w < 0) w = bw, h = bh;
    x = std::max(0, x), y = std::max(0, y);
    w = std::min(w, bw - x), h = std::min(h, bh - y);
    if (w <= 0 || h <= 0) return false;
    px.resize(static_cast<size_t>(w) * h * 4);
    for (int r = 0; r < h; r++)
        std::copy_n(&all[(static_cast<size_t>(y + r) * bw + x) * 4], static_cast<size_t>(w) * 4, &px[static_cast<size_t>(r) * w * 4]);
    outW = w, outH = h;
    return true;
}
GMFN(screen_save) {
    std::vector<uint8_t> px;
    int w, h;
    if (screenPixels(rt, 0, 0, -1, -1, px, w, h)) {
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;  // the screen is opaque
        writePng(rt.files->writePath(S(0)), px.data(), w, h);
    }
    return Value();
}
GMFN(screen_save_part) {
    std::vector<uint8_t> px;
    int w, h;
    if (screenPixels(rt, I(1), I(2), I(3), I(4), px, w, h)) {
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        writePng(rt.files->writePath(S(0)), px.data(), w, h);
    }
    return Value();
}

void registerSpriteBuiltins(Runner& rt) {
    GMREG(sprite_add_ext);
    GMREG(screen_save); GMREG(screen_save_part);
    GMREG(sprite_create_from_surface); GMREG(sprite_add_from_surface); GMREG(sprite_add); GMREG(sprite_replace);
    GMREG(sprite_delete); GMREG(sprite_duplicate); GMREG(sprite_assign); GMREG(sprite_merge); GMREG(sprite_set_offset);
    GMREG(sprite_get_speed); GMREG(sprite_get_speed_type); GMREG(sprite_set_speed); GMREG(sprite_get_bbox_mode);
    GMREG(sprite_set_bbox_mode); GMREG(sprite_set_bbox); GMREG(sprite_collision_mask); GMREG(sprite_save);
    GMREG(surface_getpixel); GMREG(surface_getpixel_ext); GMREG(buffer_get_surface); GMREG(buffer_set_surface);
    GMREG(surface_save); GMREG(surface_save_part);
}

}  // namespace gm
