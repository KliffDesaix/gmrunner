// draw_*, font_*, surface_*, shader_*, window_*, display_* and platform stubs (steam_*, os_*).
#include <algorithm>
#include <cmath>

#include "runtime/Builtins.h"
#include "runtime/SkeletonSprites.h"
#include "runtime/Graphics.h"
#include "runtime/Runner.h"
#include "runtime/Stores.h"
#include "util/BuiltinFonts.h"
#include "util/Fs.h"
#include "util/Utf8.h"

namespace gm {

#define G (*rt.gfx)

static double subimg(Instance* self, double s) { return s < 0 && self ? self->imageIndex : s; }

// ---- state
GMFN(draw_set_font) { G.font = I(0); return Value(); }
GMFN(draw_get_font) { return Value(static_cast<double>(G.font)); }
GMFN(draw_set_color) { G.color = C(0); return Value(); }
GMFN(draw_get_color) { return Value(static_cast<double>(G.color)); }
GMFN(draw_set_alpha) { G.alpha = std::clamp(D(0), 0.0, 1.0); return Value(); }
GMFN(draw_get_alpha) { return Value(G.alpha); }
GMFN(draw_set_halign) { G.halign = I(0); return Value(); }
GMFN(draw_set_valign) { G.valign = I(0); return Value(); }
GMFN(draw_set_circle_precision) { G.circlePrecision = I(0); return Value(); }
GMFN(draw_set_blend_mode) { G.setBlendMode(I(0)); return Value(); }
GMFN(draw_set_blend_mode_ext) { G.setBlendModeExt(I(0), I(1)); return Value(); }
GMFN(draw_enable_alphablend) { G.setAlphaBlend(B(0)); return Value(); }
GMFN(draw_set_colour_write_enable) { G.setColorWrite(B(0), B(1), B(2), B(3)); return Value(); }
GMFN(draw_set_alpha_test) { G.setAlphaTest(B(0)); return Value(); }
GMFN(draw_set_alpha_test_ref_value) { G.setAlphaTestRef(D(0)); return Value(); }
GMFN(d3d_set_fog) { G.setFog(B(0), C(1)); return Value(); }
GMFN(texture_set_interpolation) { G.setTexFilter(B(0)); return Value(); }

// ---- sprites
GMFN(draw_self) {
    if (self) G.drawSelf(self);
    return Value();
}
GMFN(draw_sprite) {
    if (isSkeletonSprite(rt, I(0))) skeletonDrawSprite(rt, self, I(0), subimg(self, D(1)), D(2), D(3), 1, 1, 0, 0xFFFFFF, G.alpha);
    else G.drawSprite(I(0), subimg(self, D(1)), D(2), D(3), 1, 1, 0, 0xFFFFFF, G.alpha);
    return Value();
}
GMFN(draw_sprite_ext) {
    if (isSkeletonSprite(rt, I(0))) skeletonDrawSprite(rt, self, I(0), subimg(self, D(1)), D(2), D(3), D(4), D(5), D(6), C(7), D(8));
    else G.drawSprite(I(0), subimg(self, D(1)), D(2), D(3), D(4), D(5), D(6), C(7), D(8));
    return Value();
}
GMFN(draw_sprite_part) {
    G.drawSpritePart(I(0), subimg(self, D(1)), D(2), D(3), D(4), D(5), D(6), D(7), 1, 1, 0xFFFFFF, G.alpha);
    return Value();
}
GMFN(draw_sprite_part_ext) {
    G.drawSpritePart(I(0), subimg(self, D(1)), D(2), D(3), D(4), D(5), D(6), D(7), D(8), D(9), C(10), D(11));
    return Value();
}
GMFN(draw_sprite_general) {  // (sprite, subimg, left, top, w, h, x, y, xscale, yscale, rot, c1, c2, c3, c4, alpha)
    const uint32_t cols[4] = {C(11), C(12), C(13), C(14)};
    G.drawSpriteGeneral(I(0), subimg(self, D(1)), D(2), D(3), D(4), D(5), D(6), D(7), D(8), D(9), D(10), cols, D(15));
    return Value();
}
GMFN(draw_sprite_stretched) {
    G.drawSpriteStretched(I(0), subimg(self, D(1)), D(2), D(3), D(4), D(5), 0xFFFFFF, G.alpha);
    return Value();
}
GMFN(draw_sprite_stretched_ext) {
    G.drawSpriteStretched(I(0), subimg(self, D(1)), D(2), D(3), D(4), D(5), C(6), D(7));
    return Value();
}

// ---- backgrounds
GMFN(draw_background) { G.drawBackground(I(0), D(1), D(2), 1, 1, 0, 0xFFFFFF, G.alpha); return Value(); }
GMFN(draw_background_ext) { G.drawBackground(I(0), D(1), D(2), D(3), D(4), D(5), C(6), D(7)); return Value(); }
GMFN(draw_background_part) {
    G.drawBackgroundPart(I(0), D(1), D(2), D(3), D(4), D(5), D(6), 1, 1, 0xFFFFFF, G.alpha);
    return Value();
}
GMFN(draw_background_part_ext) {
    G.drawBackgroundPart(I(0), D(1), D(2), D(3), D(4), D(5), D(6), D(7), D(8), C(9), D(10));
    return Value();
}
GMFN(draw_background_stretched) {
    int w, h;
    G.backgroundSize(I(0), w, h);
    if (w > 0 && h > 0) G.drawBackground(I(0), D(1), D(2), D(3) / w, D(4) / h, 0, 0xFFFFFF, G.alpha);
    return Value();
}
GMFN(draw_background_tiled) {
    G.drawBackgroundTiled(I(0), D(1), D(2), 1, 1, true, true, 0xFFFFFF, G.alpha);
    return Value();
}
GMFN(draw_background_tiled_ext) {  // (back, x, y, xscale, yscale, colour, alpha)
    G.drawBackgroundTiled(I(0), D(1), D(2), D(3), D(4), true, true, static_cast<uint32_t>(I(5)) & 0xFFFFFF, D(6));
    return Value();
}

// ---- text
GMFN(draw_text) {
    G.drawText(D(0), D(1), ARG(2).toString(), -1, -1, 1, 1, 0, G.color, G.color, G.color, G.color, G.alpha);
    return Value();
}
GMFN(draw_text_ext) {
    G.drawText(D(0), D(1), ARG(2).toString(), D(3), D(4), 1, 1, 0, G.color, G.color, G.color, G.color, G.alpha);
    return Value();
}
GMFN(draw_text_transformed) {
    G.drawText(D(0), D(1), ARG(2).toString(), -1, -1, D(3), D(4), D(5), G.color, G.color, G.color, G.color, G.alpha);
    return Value();
}
GMFN(draw_text_ext_transformed) {
    G.drawText(D(0), D(1), ARG(2).toString(), D(3), D(4), D(5), D(6), D(7), G.color, G.color, G.color, G.color,
               G.alpha);
    return Value();
}
GMFN(draw_text_color) {
    G.drawText(D(0), D(1), ARG(2).toString(), -1, -1, 1, 1, 0, C(3), C(4), C(5), C(6), D(7));
    return Value();
}
GMFN(draw_text_ext_color) {
    G.drawText(D(0), D(1), ARG(2).toString(), D(3), D(4), 1, 1, 0, C(5), C(6), C(7), C(8), D(9));
    return Value();
}
GMFN(string_width) { return Value(G.stringWidth(ARG(0).toString())); }
GMFN(string_height) { return Value(G.stringHeight(ARG(0).toString())); }
GMFN(string_width_ext) { return Value(G.stringWidth(ARG(0).toString(), D(1), D(2))); }
GMFN(string_height_ext) { return Value(G.stringHeight(ARG(0).toString(), D(1), D(2))); }

// ---- fonts
static std::u32string rangeMap(int first, int count) {
    std::u32string m;
    for (int i = 0; i < count; i++) m += static_cast<char32_t>(first + i);
    return m;
}
GMFN(font_add_sprite) {
    int spr = I(0);
    int n = spr >= 0 && static_cast<size_t>(spr) < rt.dw.sprites.size()
                ? static_cast<int>(rt.dw.sprites[spr].frames.size()) : 0;
    return Value(static_cast<double>(G.addSpriteFont(spr, rangeMap(I(1), n), B(2), I(3))));
}
GMFN(font_add_sprite_ext) {
    return Value(static_cast<double>(G.addSpriteFont(I(0), utf8Decode(S(1)), B(2), I(3))));
}
GMFN(font_replace_sprite) {
    int spr = I(1);
    int n = spr >= 0 && static_cast<size_t>(spr) < rt.dw.sprites.size()
                ? static_cast<int>(rt.dw.sprites[spr].frames.size()) : 0;
    G.replaceSpriteFont(I(0), spr, rangeMap(I(2), n), B(3), I(4));
    return Value();
}
GMFN(font_replace_sprite_ext) {
    G.replaceSpriteFont(I(0), I(1), utf8Decode(S(2)), B(3), I(4));
    return Value();
}
// The built-in font standing in for a system font named by font_add (on Windows GameMaker falls
// back to the installed fonts): Liberation Serif and Mono for serif and fixed-width names,
// Liberation Sans (Arial's metrics) for the rest.
static BuiltinFont systemFontFor(std::string name, bool bold) {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    if (has("courier") || has("mono") || has("consol") || has("terminal") || has("fixed") || has("typewriter"))
        return BuiltinFont::Mono;
    if (!has("sans") && (has("times") || has("serif") || has("georgia") || has("garamond") || has("roman") ||
                         has("cambria") || has("palatino") || has("book")))
        return BuiltinFont::Serif;
    return bold ? BuiltinFont::SansBold : BuiltinFont::Sans;
}

// font_add(name, size, bold, italic, first, last): a TrueType file among the game's files, else
// a system font by that name. Bold and italic are drawn thickened and slanted when the face
// itself isn't bold or italic.
GMFN(font_add) {
    std::string name = S(0);
    std::vector<uint8_t> file;
    const uint8_t* data = nullptr;
    if (!readFile(rt.files->readPath(name), file) || file.empty()) {
        FontBytes b = builtinFont(systemFontFor(name, B(2)));
        data = b.data;
    }
    int id = G.addTrueTypeFont(data, std::move(file), D(1), B(2), B(3), I(4), I(5));
    if (id < 0) {
        rt.vm.warnOnce("font_add", "font_add(" + name + "): not a font this runner can read; text in it will not render");
        return Value(static_cast<double>(G.addPlaceholderFont()));
    }
    return Value(static_cast<double>(id));
}
GMFN(font_exists) { return boolV(G.fontExists(I(0))); }
GMFN(font_delete) { G.deleteFont(I(0)); return Value(); }

// ---- primitives
GMFN(draw_rectangle) {
    G.drawRect(D(0), D(1), D(2), D(3), B(4), G.color, G.color, G.color, G.color, G.alpha);
    return Value();
}
GMFN(draw_rectangle_color) {
    G.drawRect(D(0), D(1), D(2), D(3), B(8), C(4), C(5), C(6), C(7), G.alpha);
    return Value();
}
GMFN(draw_line) { G.drawLine(D(0), D(1), D(2), D(3), 1, G.color, G.color, G.alpha); return Value(); }
GMFN(draw_line_width) { G.drawLine(D(0), D(1), D(2), D(3), D(4), G.color, G.color, G.alpha); return Value(); }
GMFN(draw_line_color) { G.drawLine(D(0), D(1), D(2), D(3), 1, C(4), C(5), G.alpha); return Value(); }
GMFN(draw_line_width_color) { G.drawLine(D(0), D(1), D(2), D(3), D(4), C(5), C(6), G.alpha); return Value(); }
GMFN(draw_circle) {
    G.drawEllipse(D(0) - D(2), D(1) - D(2), D(0) + D(2), D(1) + D(2), B(3), G.color, G.color, G.alpha);
    return Value();
}
GMFN(draw_circle_color) {
    G.drawEllipse(D(0) - D(2), D(1) - D(2), D(0) + D(2), D(1) + D(2), B(5), C(3), C(4), G.alpha);
    return Value();
}
GMFN(draw_ellipse) { G.drawEllipse(D(0), D(1), D(2), D(3), B(4), G.color, G.color, G.alpha); return Value(); }
GMFN(draw_ellipse_color) { G.drawEllipse(D(0), D(1), D(2), D(3), B(6), C(4), C(5), G.alpha); return Value(); }
GMFN(draw_point) { G.drawPoint(D(0), D(1), G.color, G.alpha); return Value(); }
GMFN(draw_point_color) { G.drawPoint(D(0), D(1), C(2), G.alpha); return Value(); }
GMFN(draw_triangle) {
    if (B(6)) {
        G.drawLine(D(0), D(1), D(2), D(3), 1, G.color, G.color, G.alpha);
        G.drawLine(D(2), D(3), D(4), D(5), 1, G.color, G.color, G.alpha);
        G.drawLine(D(4), D(5), D(0), D(1), 1, G.color, G.color, G.alpha);
    } else {
        G.primitiveBegin(4);
        G.primitiveVertex(D(0), D(1), G.color, G.alpha);
        G.primitiveVertex(D(2), D(3), G.color, G.alpha);
        G.primitiveVertex(D(4), D(5), G.color, G.alpha);
        G.primitiveEnd();
    }
    return Value();
}
static uint32_t mixColor(uint32_t a, uint32_t b, double t) {
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        double ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= (static_cast<uint32_t>(ca + (cb - ca) * t) & 0xFF) << sh;
    }
    return out;
}
// draw_healthbar(x1, y1, x2, y2, amount, backcol, mincol, maxcol, direction, showback, showborder)
GMFN(draw_healthbar) {
    double x1 = D(0), y1 = D(1), x2 = D(2), y2 = D(3);
    double amount = std::max(0.0, std::min(100.0, D(4)));
    uint32_t back = C(5), lo = C(6), hi = C(7), mid = mixColor(lo, hi, 0.5);
    bool showBorder = B(10);
    if (B(9)) {
        G.drawRect(x1, y1, x2, y2, false, back, back, back, back, G.alpha);
        if (showBorder) G.drawRect(x1, y1, x2, y2, true, 0, 0, 0, 0, G.alpha);
    }
    double f = amount / 100, bx1 = x1, by1 = y1, bx2 = x2, by2 = y2;
    switch (I(8)) {
        case 1: bx1 = x2 - f * (x2 - x1); break;
        case 2: by2 = y1 + f * (y2 - y1); break;
        case 3: by1 = y2 - f * (y2 - y1); break;
        default: bx2 = x1 + f * (x2 - x1); break;
    }
    uint32_t col = amount > 50 ? mixColor(mid, hi, (amount - 50) / 50) : mixColor(lo, mid, amount / 50);
    G.drawRect(bx1, by1, bx2, by2, false, col, col, col, col, G.alpha);
    if (showBorder) G.drawRect(bx1, by1, bx2, by2, true, 0, 0, 0, 0, G.alpha);
    return Value();
}
GMFN(draw_arrow) {  // (x1, y1, x2, y2, size): a line with a filled head at (x2, y2)
    double x1 = D(0), y1 = D(1), x2 = D(2), y2 = D(3), size = D(4);
    double d = std::hypot(x2 - x1, y2 - y1);
    if (d == 0) return Value();
    size = std::min(size, d);
    double xx = size * (x2 - x1) / d, yy = size * (y2 - y1) / d;
    G.drawLine(x1, y1, x2, y2, 1, G.color, G.color, G.alpha);
    G.primitiveBegin(4);
    G.primitiveVertex(x2 - xx - yy / 3, y2 - yy + xx / 3, G.color, G.alpha);
    G.primitiveVertex(x2, y2, G.color, G.alpha);
    G.primitiveVertex(x2 - xx + yy / 3, y2 - yy - xx / 3, G.color, G.alpha);
    G.primitiveEnd();
    return Value();
}
GMFN(draw_primitive_begin) { G.primitiveBegin(I(0)); return Value(); }
GMFN(draw_primitive_begin_texture) { G.primitiveBegin(I(0), static_cast<int>(texArg(ARG(1)))); return Value(); }
GMFN(draw_vertex) { G.primitiveVertex(D(0), D(1), G.color, G.alpha); return Value(); }
GMFN(draw_vertex_color) { G.primitiveVertex(D(0), D(1), C(2), D(3)); return Value(); }
GMFN(draw_vertex_texture) { G.primitiveVertex(D(0), D(1), G.color, G.alpha, D(2), D(3)); return Value(); }
GMFN(draw_vertex_texture_color) { G.primitiveVertex(D(0), D(1), C(4), D(5), D(2), D(3)); return Value(); }
GMFN(draw_primitive_end) { G.primitiveEnd(); return Value(); }
GMFN(draw_clear) { G.drawClear(C(0), 1); return Value(); }
GMFN(draw_clear_alpha) { G.drawClear(C(0), D(1)); return Value(); }

// ---- surfaces
GMFN(surface_create) { return Value(static_cast<double>(G.surfaceCreate(I(0), I(1)))); }
GMFN(surface_free) { G.surfaceFree(I(0)); return Value(); }
GMFN(surface_exists) { return boolV(G.surfaceExists(I(0))); }
GMFN(surface_get_width) { return Value(static_cast<double>(G.surfaceWidth(I(0)))); }
GMFN(surface_get_height) { return Value(static_cast<double>(G.surfaceHeight(I(0)))); }
GMFN(surface_set_target) { return boolV(G.surfaceSetTarget(I(0))); }
GMFN(surface_reset_target) { return boolV(G.surfaceResetTarget()); }
GMFN(surface_copy) { G.surfaceCopy(I(0), D(1), D(2), I(3)); return Value(); }
GMFN(surface_resize) { G.surfaceResize(I(0), I(1), I(2)); return Value(); }
GMFN(draw_surface) { G.drawSurface(I(0), D(1), D(2), 1, 1, 0, 0xFFFFFF, G.alpha); return Value(); }
GMFN(draw_surface_ext) { G.drawSurface(I(0), D(1), D(2), D(3), D(4), D(5), C(6), D(7)); return Value(); }
GMFN(draw_surface_part) {
    G.drawSurfacePart(I(0), D(1), D(2), D(3), D(4), D(5), D(6), 1, 1, 0xFFFFFF, G.alpha);
    return Value();
}
GMFN(draw_surface_part_ext) {
    G.drawSurfacePart(I(0), D(1), D(2), D(3), D(4), D(5), D(6), D(7), D(8), C(9), D(10));
    return Value();
}
GMFN(draw_surface_stretched) { G.drawSurfaceStretched(I(0), D(1), D(2), D(3), D(4), 0xFFFFFF, G.alpha); return Value(); }
GMFN(draw_surface_stretched_ext) { G.drawSurfaceStretched(I(0), D(1), D(2), D(3), D(4), C(5), D(6)); return Value(); }
GMFN(draw_surface_tiled) { G.drawSurfaceTiled(I(0), D(1), D(2), 1, 1, 0xFFFFFF, G.alpha); return Value(); }
GMFN(draw_surface_tiled_ext) { G.drawSurfaceTiled(I(0), D(1), D(2), D(3), D(4), C(5), D(6)); return Value(); }
GMFN(draw_surface_general) {  // (id, left, top, w, h, x, y, xscale, yscale, rot, c1, c2, c3, c4, alpha)
    const uint32_t cols[4] = {C(10), C(11), C(12), C(13)};
    G.drawSurfaceGeneral(I(0), D(1), D(2), D(3), D(4), D(5), D(6), D(7), D(8), D(9), cols, D(14));
    return Value();
}
GMFN(surface_copy_part) { G.surfaceCopyPart(I(0), D(1), D(2), I(3), D(4), D(5), D(6), D(7)); return Value(); }
GMFN(surface_get_target) { return Value(static_cast<double>(G.surfaceGetTarget())); }
GMFN(application_surface_draw_enable) { G.appSurfaceAutoDraw = B(0); return Value(); }
GMFN(application_surface_enable) { return Value(); }
GMFN(application_surface_is_enabled) { return boolV(true); }

// ---- shaders
GMFN(shader_set) { G.shaderSet(I(0)); return Value(); }
GMFN(shader_reset) { G.shaderReset(); return Value(); }
GMFN(shader_get_uniform) { return Value(static_cast<double>(G.shaderUniform(I(0), S(1)))); }
GMFN(shader_set_uniform_f) {
    float v[4] = {0, 0, 0, 0};
    int n = std::min(4, argc - 1);
    for (int i = 0; i < n; i++) v[i] = static_cast<float>(D(i + 1));
    if (n > 0) G.shaderUniformF(I(0), v, n);
    return Value();
}
GMFN(shader_set_uniform_i) {
    float v[4] = {0, 0, 0, 0};
    int n = std::min(4, argc - 1);
    for (int i = 0; i < n; i++) v[i] = static_cast<float>(I(i + 1));
    if (n > 0) G.shaderUniformF(I(0), v, n);
    return Value();
}
GMFN(shaders_are_supported) { return boolV(true); }
GMFN(shader_is_compiled) { return boolV(G.shaderCompiled(I(0))); }

// ---- window / display
GMFN(window_get_fullscreen) { return boolV(rt.platform.fullscreen()); }
GMFN(window_set_fullscreen) { rt.platform.setFullscreen(B(0)); return Value(); }
GMFN(window_get_width) { return Value(static_cast<double>(G.windowWidth())); }
GMFN(window_get_height) { return Value(static_cast<double>(G.windowHeight())); }
// The parts of the window on each display: visible x1, y1, x2, y2 then that display's x1, y1,
// x2, y2. One display here.
GMFN(window_get_visible_rects) {
    int x, y;
    rt.platform.windowPosition(x, y);
    double w = G.windowWidth(), h = G.windowHeight(), dw = rt.platform.displayWidth(), dh = rt.platform.displayHeight();
    std::vector<Value> r = {Value(std::max(0.0, double(x))), Value(std::max(0.0, double(y))), Value(std::min(dw, x + w)),
                            Value(std::min(dh, y + h)), Value(0.0), Value(0.0), Value(dw), Value(dh)};
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    a->rows[0] = std::move(r);
    return arr;
}
// The debug overlay's views and controls (dbg_view, dbg_section, dbg_button, ...): there is no
// overlay, so they create nothing; dbg_view / dbg_section still return a handle.
GMFN(dbg_handle) {
    GM_UNUSED_FN_ARGS;
    static int64_t next = 1;
    return Value::ptr(reinterpret_cast<void*>(static_cast<intptr_t>(next++)));
}
GMFN(window_get_x) { int x, y; rt.platform.windowPosition(x, y); return Value(static_cast<double>(x)); }
GMFN(window_get_y) { int x, y; rt.platform.windowPosition(x, y); return Value(static_cast<double>(y)); }
// Window state the platform can't report back (or that only a desktop window has).
static struct {
    bool border = true, sizeable = false, onTop = false, visible = true, showIcons = true;
    int minW = 0, minH = 0, maxW = 0, maxH = 0, cursor = 0;
    uint32_t color = 0;
    double regionScale = 1;
} gWin;
GMFN(window_set_position) { rt.platform.setWindowPosition(I(0), I(1)); return Value(); }
GMFN(window_set_rectangle) {
    rt.platform.setWindowPosition(I(0), I(1));
    G.setWindowSize(I(2), I(3));
    rt.platform.setWindowSize(I(2), I(3));
    return Value();
}
GMFN(window_center) {
    int w = G.windowWidth(), h = G.windowHeight();
    rt.platform.setWindowPosition((rt.platform.displayWidth() - w) / 2, (rt.platform.displayHeight() - h) / 2);
    return Value();
}
GMFN(window_default) {  // the window back to the size of the first room (or the game's window size)
    G.setWindowSize(G.applicationWidth(), G.applicationHeight());
    rt.platform.setWindowSize(G.applicationWidth(), G.applicationHeight());
    return Value();
}
static void applyLimits(Runner& rt) { rt.platform.setWindowSizeLimits(gWin.minW, gWin.minH, gWin.maxW, gWin.maxH); }
GMFN(window_set_min_width) { gWin.minW = I(0); applyLimits(rt); return Value(); }
GMFN(window_set_min_height) { gWin.minH = I(0); applyLimits(rt); return Value(); }
GMFN(window_set_max_width) { gWin.maxW = I(0); applyLimits(rt); return Value(); }
GMFN(window_set_max_height) { gWin.maxH = I(0); applyLimits(rt); return Value(); }
GMFN(window_set_showborder) { gWin.border = B(0); rt.platform.setWindowFlag(Platform::WindowFlag::Border, gWin.border); return Value(); }
GMFN(window_get_showborder) { return boolV(gWin.border); }
GMFN(window_set_sizeable) { gWin.sizeable = B(0); rt.platform.setWindowFlag(Platform::WindowFlag::Resizable, gWin.sizeable); return Value(); }
GMFN(window_get_sizeable) { return boolV(gWin.sizeable); }
GMFN(window_set_stayontop) { gWin.onTop = B(0); rt.platform.setWindowFlag(Platform::WindowFlag::OnTop, gWin.onTop); return Value(); }
GMFN(window_get_stayontop) { return boolV(gWin.onTop); }
GMFN(window_set_visible) { gWin.visible = B(0); rt.platform.setWindowFlag(Platform::WindowFlag::Visible, gWin.visible); return Value(); }
GMFN(window_get_visible) { return boolV(gWin.visible); }
GMFN(window_set_showicons) { gWin.showIcons = B(0); return Value(); }
GMFN(window_get_showicons) { return boolV(gWin.showIcons); }
GMFN(window_set_color) { gWin.color = C(0); G.windowColor = gWin.color; return Value(); }
GMFN(window_get_color) { return Value(static_cast<double>(gWin.color)); }
GMFN(window_set_region_scale) { gWin.regionScale = D(0); return Value(); }
GMFN(window_get_region_scale) { return Value(gWin.regionScale); }
GMFN(window_set_cursor) {  // cr_none (-1) hides it; the other cursor shapes all show the arrow
    gWin.cursor = I(0);
    rt.platform.setCursorVisible(gWin.cursor != -1);
    return Value();
}
GMFN(window_get_cursor) { return Value(static_cast<double>(gWin.cursor)); }
GMFN(window_mouse_set) {
    double sx, sy;
    G.windowToScreen(D(0), D(1), sx, sy);
    rt.platform.warpMouse(static_cast<int>(sx), static_cast<int>(sy));
    return Value();
}
GMFN(display_mouse_set) { rt.platform.warpMouseGlobal(I(0), I(1)); return Value(); }
GMFN(display_get_dpi_x) { return Value(rt.platform.displayDpi()); }
GMFN(display_get_dpi_y) { return Value(rt.platform.displayDpi()); }
// window_view_mouse_get_x/y(view): the mouse in that view's room coordinates; window_views_*
// use whichever view the mouse is over (as mouse_x / mouse_y).
GMFN(window_view_mouse_get_x) { double x, y; G.windowToView(I(0), rt.input.mouseX, rt.input.mouseY, x, y); return Value(x); }
GMFN(window_view_mouse_get_y) { double x, y; G.windowToView(I(0), rt.input.mouseX, rt.input.mouseY, x, y); return Value(y); }
GMFN(window_views_mouse_get_x) { double x, y; G.windowToRoom(rt.input.mouseX, rt.input.mouseY, x, y); return Value(x); }
GMFN(window_views_mouse_get_y) { double x, y; G.windowToRoom(rt.input.mouseX, rt.input.mouseY, x, y); return Value(y); }
GMFN(window_set_size) {
    G.setWindowSize(I(0), I(1));
    rt.platform.setWindowSize(I(0), I(1));
    return Value();
}
GMFN(window_set_caption) { rt.platform.setWindowCaption(S(0)); return Value(); }
GMFN(window_get_caption) { return Value(rt.roomCaption); }
GMFN(window_mouse_get_x) { return Value(rt.input.mouseX); }
GMFN(window_mouse_get_y) { return Value(rt.input.mouseY); }
GMFN(window_has_focus) { return boolV(true); }
GMFN(display_get_width) { return Value(static_cast<double>(rt.platform.displayWidth())); }
GMFN(display_get_height) { return Value(static_cast<double>(rt.platform.displayHeight())); }
// display_landscape (0) unless the display is taller than wide (display_portrait, 2).
GMFN(display_get_orientation) {
    return Value(rt.platform.displayHeight() > rt.platform.displayWidth() ? 2.0 : 0.0);
}
GMFN(display_get_gui_width) { return Value(G.guiWidth()); }
GMFN(display_get_gui_height) { return Value(G.guiHeight()); }
GMFN(display_set_gui_size) { G.setGuiSize(D(0), D(1)); return Value(); }
GMFN(display_set_gui_maximize) {  // ([xscale], [yscale], [xoffset], [yoffset])
    G.setGuiMaximize(argc > 0 ? D(0) : 1, argc > 1 ? D(1) : 1, argc > 2 ? D(2) : 0, argc > 3 ? D(3) : 0);
    return Value();
}
GMFN(display_mouse_get_x) { return Value(rt.input.mouseX); }
GMFN(display_mouse_get_y) { return Value(rt.input.mouseY); }

// Functions that only matter on desktop/Steam builds: accept and do nothing.
GMFN(noop) { return Value(); }
GMFN(false_) { return boolV(false); }
GMFN(true_) { return boolV(true); }
GMFN(zero) { return Value(0.0); }
GMFN(virtual_key_add) {  // (x, y, w, h, key): a touch area that presses `key`; ids only here
    GM_UNUSED_FN_ARGS;
    static int next = 0;
    return Value(static_cast<double>(next++));
}
GMFN(display_get_frequency) { GM_UNUSED_FN_ARGS; return Value(60.0); }
// layer_set_target_room(room): GameMaker then runs layer_* functions against that (unloaded)
// room's layers. Only recorded here: layer functions still see the current room.
int gLayerTargetRoom = -1;
GMFN(layer_set_target_room) {
    gLayerTargetRoom = I(0);
    if (gLayerTargetRoom != rt.room)
        rt.vm.warnOnce("layer_set_target_room", "layer_set_target_room: layer functions still act on the current room");
    return Value();
}
GMFN(layer_reset_target_room) { GM_UNUSED_FN_ARGS; gLayerTargetRoom = -1; return Value(); }
GMFN(layer_get_target_room) { return Value(static_cast<double>(gLayerTargetRoom >= 0 ? gLayerTargetRoom : rt.room)); }
// gpu_set_sprite_cull: recorded (sprites outside the view are cheap to draw anyway).
bool gSpriteCull = true;
GMFN(gpu_set_sprite_cull) { gSpriteCull = B(0); return Value(); }
GMFN(gpu_get_sprite_cull) { GM_UNUSED_FN_ARGS; return Value::boolean(gSpriteCull); }
GMFN(surface_get_texture_depth) { GM_UNUSED_FN_ARGS; return texValue(-1); }  // no depth textures
// part_particles_burst(system, x, y, particle system asset): particle system assets (PSYS) aren't
// loaded yet, so nothing is emitted.
GMFN(part_particles_burst) {
    GM_UNUSED_FN_ARGS;
    rt.vm.warnOnce("part_particles_burst", "part_particles_burst: particle system assets are not supported yet");
    return Value();
}
// external_define / external_call (GameMaker 1.x-2.2 DLL calls): no native libraries, so calls
// return 0 or "" by the declared result type (ty_real 0, ty_string 1).
std::vector<int> gExternalResultTypes;
GMFN(external_define) {  // (dll, name, calltype, restype, argnumb, argtypes...)
    gExternalResultTypes.push_back(I(3));
    return Value(static_cast<double>(gExternalResultTypes.size() - 1));
}
GMFN(external_call) {
    int id = I(0);
    bool str = id >= 0 && static_cast<size_t>(id) < gExternalResultTypes.size() && gExternalResultTypes[id] == 1;
    return str ? Value("") : Value(0.0);
}
GMFN(minus_one) { return Value(-1.0); }
GMFN(empty_string) { return Value(""); }
GMFN(os_get_language) { return Value("en"); }

void registerDrawBuiltins(Runner& rt) {
    GMREG(draw_set_font); GMREG(draw_get_font); GMREG(draw_set_color); GMALIAS(draw_set_colour, draw_set_color);
    GMREG(draw_get_color); GMALIAS(draw_get_colour, draw_get_color); GMREG(draw_set_alpha); GMREG(draw_get_alpha);
    GMREG(draw_set_halign); GMREG(draw_set_valign); GMREG(draw_set_circle_precision); GMREG(draw_set_blend_mode);
    GMREG(draw_set_blend_mode_ext); GMREG(draw_enable_alphablend); GMREG(draw_set_colour_write_enable);
    GMALIAS(draw_set_color_write_enable, draw_set_colour_write_enable); GMREG(draw_set_alpha_test);
    GMREG(draw_set_alpha_test_ref_value); GMREG(d3d_set_fog); GMREG(texture_set_interpolation);
    GMREG(draw_self); GMREG(draw_sprite); GMREG(draw_sprite_ext); GMREG(draw_sprite_part); GMREG(draw_sprite_part_ext);
    GMREG(draw_sprite_general); GMREG(draw_sprite_stretched); GMREG(draw_sprite_stretched_ext);
    GMREG(draw_background); GMREG(draw_background_ext); GMREG(draw_background_part); GMREG(draw_background_part_ext);
    GMREG(draw_background_stretched); GMREG(draw_background_tiled); GMREG(draw_background_tiled_ext);
    GMREG(draw_text); GMREG(draw_text_ext); GMREG(draw_text_transformed); GMREG(draw_text_ext_transformed);
    GMREG(draw_text_color); GMALIAS(draw_text_colour, draw_text_color); GMREG(draw_text_ext_color);
    GMALIAS(draw_text_ext_colour, draw_text_ext_color); GMREG(string_width); GMREG(string_height);
    GMREG(string_width_ext); GMREG(string_height_ext);
    GMREG(window_set_position); GMREG(window_set_rectangle); GMREG(window_center); GMREG(window_default);
    GMREG(window_set_min_width); GMREG(window_set_min_height); GMREG(window_set_max_width); GMREG(window_set_max_height);
    GMREG(window_set_showborder); GMREG(window_get_showborder); GMREG(window_set_sizeable); GMREG(window_get_sizeable);
    GMREG(window_set_stayontop); GMREG(window_get_stayontop); GMREG(window_set_visible); GMREG(window_get_visible);
    GMREG(window_set_showicons); GMREG(window_get_showicons); GMREG(window_set_color); GMALIAS(window_set_colour, window_set_color);
    GMREG(window_get_color); GMALIAS(window_get_colour, window_get_color); GMREG(window_set_region_scale);
    GMREG(window_get_region_scale); GMREG(window_set_cursor); GMREG(window_get_cursor); GMREG(window_mouse_set);
    GMREG(display_mouse_set); GMREG(display_get_dpi_x); GMREG(display_get_dpi_y); GMREG(window_view_mouse_get_x);
    GMREG(window_view_mouse_get_y); GMREG(window_views_mouse_get_x); GMREG(window_views_mouse_get_y);
    GMALIAS(display_set_gui_maximise, display_set_gui_maximize);
    GMREG(draw_surface_tiled); GMREG(draw_surface_tiled_ext); GMREG(draw_surface_general); GMREG(surface_copy_part);
    GMREG(surface_get_target); GMREG(draw_healthbar); GMREG(draw_arrow);
    GMREG(font_add_sprite); GMREG(font_add_sprite_ext); GMREG(font_replace_sprite); GMREG(font_replace_sprite_ext);
    GMREG(font_add); GMREG(font_exists); GMREG(font_delete);
    GMREG(draw_rectangle); GMREG(draw_rectangle_color); GMALIAS(draw_rectangle_colour, draw_rectangle_color);
    GMREG(draw_line); GMREG(draw_line_width); GMREG(draw_line_color); GMALIAS(draw_line_colour, draw_line_color);
    GMREG(draw_line_width_color); GMALIAS(draw_line_width_colour, draw_line_width_color); GMREG(draw_circle);
    GMREG(draw_circle_color); GMALIAS(draw_circle_colour, draw_circle_color); GMREG(draw_ellipse);
    GMREG(draw_ellipse_color); GMALIAS(draw_ellipse_colour, draw_ellipse_color); GMREG(draw_point);
    GMREG(draw_point_color); GMALIAS(draw_point_colour, draw_point_color); GMREG(draw_triangle);
    GMREG(draw_primitive_begin); GMREG(draw_primitive_begin_texture); GMREG(draw_vertex); GMREG(draw_vertex_color);
    GMALIAS(draw_vertex_colour, draw_vertex_color); GMREG(draw_vertex_texture); GMREG(draw_vertex_texture_color);
    GMALIAS(draw_vertex_texture_colour, draw_vertex_texture_color); GMREG(draw_primitive_end); GMREG(draw_clear);
    GMREG(draw_clear_alpha);
    GMREG(surface_create); GMREG(surface_free); GMREG(surface_exists); GMREG(surface_get_width);
    GMREG(surface_get_height); GMREG(surface_set_target); GMREG(surface_reset_target); GMREG(surface_copy);
    GMREG(surface_resize); GMREG(draw_surface); GMREG(draw_surface_ext); GMREG(draw_surface_part);
    GMREG(draw_surface_part_ext); GMREG(draw_surface_stretched); GMREG(draw_surface_stretched_ext);
    GMREG(application_surface_draw_enable); GMREG(application_surface_enable); GMREG(application_surface_is_enabled);
    GMREG(shader_set); GMREG(shader_reset); GMREG(shader_get_uniform); GMREG(shader_set_uniform_f);
    GMREG(shader_set_uniform_i); GMREG(shaders_are_supported); GMREG(shader_is_compiled);
    GMREG(window_get_fullscreen); GMREG(window_set_fullscreen); GMREG(window_get_width); GMREG(window_get_height);
    GMREG(window_get_x); GMREG(window_get_y); GMREG(window_set_size); GMREG(window_set_caption);
    GMREG(window_get_caption); GMREG(window_mouse_get_x); GMREG(window_mouse_get_y); GMREG(window_has_focus);
    GMREG(display_get_width); GMREG(display_get_orientation); GMREG(display_get_height); GMREG(display_get_gui_width); GMREG(display_get_gui_height); GMREG(display_set_gui_size); GMREG(display_set_gui_maximize);
    GMREG(display_mouse_get_x); GMREG(display_mouse_get_y);

    for (const char* n : {"window_set_position", "window_center", "window_set_cursor", "display_reset",
                          "show_debug_overlay",
                          "steam_send_screenshot", "steam_set_achievement", "steam_clear_achievement",
                          "steam_set_stat_int", "steam_set_stat_float", "steam_upload_score",
                          "steam_activate_overlay", "achievement_post", "achievement_increment",
                          "display_set_timing_method", "display_set_windows_alternate_sync", "display_set_windows_vertex_buffer_method",
                          "display_set_sleep_margin", "display_set_ui_visibility", "os_lock_orientation", "os_powersave_enable", "window_enable_borderless_fullscreen", "texture_set_repeat",
                          "draw_texture_flush", "draw_flush", "window_set_min_width", "window_set_min_height",
                          "window_set_max_width", "window_set_max_height", "audio_channel_num",
                          // debug overlay
                          "dbg_view_delete", "dbg_section_delete", "dbg_button", "dbg_checkbox", "dbg_text",
                          "dbg_slider", "dbg_slider_int", "dbg_drop_down", "dbg_text_input", "dbg_sprite",
                          "dbg_colour", "dbg_color", "dbg_same_line", "dbg_watch", "dbg_text_separator",
                          "dbg_add_font_glyphs", "dbg_sprite_button", "show_debug_log", "debug_event",
                          // Switch / PlayStation system calls of games built for those consoles
                          "switch_controller_support_set_defaults", "switch_controller_support_set_singleplayer_only",
                          "switch_controller_set_supported_styles", "switch_controller_vibrate_hd",
                          "ps4_touchpad_mouse_enable", "ps5_gamepad_set_vibration_mode"})
        rt.vm.registerBuiltin(n, fn_noop);
    for (const char* n : {"dbg_view", "dbg_section"}) rt.vm.registerBuiltin(n, fn_dbg_handle);
    GMREG(window_get_visible_rects);
    // Networking: there is no network access, so sockets can't be created or connected and
    // nothing is sent (negative results are GameMaker's failure values).
    for (const char* n : {"network_create_socket", "network_create_socket_ext", "network_create_server",
                          "network_create_server_raw", "network_connect", "network_connect_raw",
                          "network_connect_async", "network_connect_raw_async", "network_send_packet",
                          "network_send_raw", "network_send_udp", "network_send_udp_raw", "network_send_broadcast"})
        rt.vm.registerBuiltin(n, fn_minus_one);
    rt.vm.registerBuiltin("network_destroy", fn_noop);
    // Switch save data / accounts: succeed with nothing to do (saves already go to the save folder)
    for (const char* n : {"switch_save_data_mount", "switch_save_data_unmount", "switch_save_data_commit",
                          "switch_controller_support_show", "switch_accounts_select_account",
                          "switch_accounts_open_user", "switch_controller_support_get_selected_id"})
        rt.vm.registerBuiltin(n, fn_zero);
    rt.vm.registerBuiltin("switch_accounts_is_user_open", fn_true_);
    // On-screen keyboard and virtual keys (mobile/console UI): none here.
    for (const char* n : {"keyboard_virtual_show", "keyboard_virtual_hide", "virtual_key_show", "virtual_key_hide",
                          "virtual_key_delete", "gamepad_enumerate", "external_free"})
        rt.vm.registerBuiltin(n, fn_noop);
    for (const char* n : {"keyboard_virtual_status", "switch_controller_joycon_left_connected",
                          "switch_controller_joycon_right_connected"})
        rt.vm.registerBuiltin(n, fn_false_);
    rt.vm.registerBuiltin("keyboard_virtual_height", fn_zero);
    GMREG(virtual_key_add); GMREG(display_get_frequency); GMREG(external_define); GMREG(external_call);
    GMREG(layer_set_target_room); GMREG(layer_reset_target_room); GMREG(layer_get_target_room);
    GMREG(gpu_set_sprite_cull); GMREG(gpu_get_sprite_cull); GMREG(surface_get_texture_depth);
    GMREG(part_particles_burst);
    for (const char* n : {"texture_debug_messages", "gpu_set_tex_mip_enable_ext"}) rt.vm.registerBuiltin(n, fn_noop);
    rt.vm.registerBuiltin("gpu_get_tex_mip_enable_ext", fn_zero);
    for (const char* n : {"steam_is_screenshot_requested", "steam_initialised", "steam_stats_ready",
                          "is_debug_overlay_open", "is_mouse_over_debug_overlay", "is_keyboard_used_debug_overlay",
                          "steam_is_overlay_enabled", "steam_is_overlay_activated", "steam_get_achievement",
                          "os_is_paused", "os_is_network_connected", "achievement_available", "achievement_login_status"})
        rt.vm.registerBuiltin(n, fn_false_);
    for (const char* n : {"steam_get_app_id", "steam_get_user_account_id", "steam_get_stat_int",
                          "steam_get_stat_float", "display_aa", "os_get_region"})
        rt.vm.registerBuiltin(n, fn_zero);
    rt.vm.registerBuiltin("steam_get_persona_name", fn_empty_string);
    rt.vm.registerBuiltin("os_get_language", fn_os_get_language);
    rt.vm.registerBuiltin("switch_language_get_desired_language", fn_os_get_language);
}

}  // namespace gm
