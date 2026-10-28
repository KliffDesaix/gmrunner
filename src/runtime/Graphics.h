// GameMaker drawing semantics (sprites, text, surfaces, views) on top of the Renderer.
#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/Renderer.h"
#include "runtime/TrueType.h"
#include "vm/Value.h"

namespace gm {

class Runner;
struct Instance;

class Graphics {
public:
    explicit Graphics(Runner& rt);
    ~Graphics();
    void init(bool enabled);
    Renderer& renderer() { return r_; }

    // ---- frame structure
    void onRoomStart();
    void drawFrame();
    void present(int screenW, int screenH);
    void windowToRoom(double wx, double wy, double& rx, double& ry) const;
    void screenToWindow(double sx, double sy, double& wx, double& wy) const;
    void windowToScreen(double wx, double wy, double& sx, double& sy) const;
    void windowToView(int view, double wx, double wy, double& rx, double& ry) const;
    void portScale(double& sx, double& sy) const;
    int applicationWidth() const { return appW_; }
    int applicationHeight() const { return appH_; }
    uint32_t windowColor = 0;  // window_set_colour: around the game image
    int windowWidth() const { return winW_; }
    int windowHeight() const { return winH_; }
    void setWindowSize(int w, int h);
    int applicationSurface() const { return appSurface_; }
    // The GUI layer's coordinate space (display_set_gui_size / display_set_gui_maximize), mapped
    // onto the window: GUI x = (window x - offsetX) / scaleX.
    void setGuiSize(double w, double h);
    void setGuiMaximize(double xscale, double yscale, double xoffset, double yoffset);
    double guiWidth() const;
    double guiHeight() const;
    void windowToGui(double wx, double wy, double& gx, double& gy) const;
    bool appSurfaceAutoDraw = true;
    // A pointer drawn over the presented frame (window coordinates), for the stick-driven
    // virtual mouse on consoles; set by the host each frame.
    bool overlayCursor = false;
    double overlayCursorX = 0, overlayCursorY = 0;
    // Drawn last over the presented frame, in screen pixels with alpha blending on: the host's
    // on-screen touch controls.
    std::function<void(Renderer&, int screenW, int screenH)> screenOverlay;
    bool presentLinear = false;
    bool presentIntegerScale = false;
    int presentRotation = 0;  // 0 or 90/270 for vertical (TATE) handheld play
    bool readBackbuffer(std::vector<uint8_t>& rgba, int& w, int& h);

    // ---- draw state
    uint32_t color = 0xFFFFFF;
    double alpha = 1;
    int font = -1;
    int halign = 0, valign = 0;
    int circlePrecision = 24;
    void setBlendMode(int mode);
    void setBlendModeExt(int src, int dst);
    void setBlendModeExtSepAlpha(int src, int dst, int srcAlpha, int dstAlpha);
    // gpu_set_blendequation(_sepalpha): bm_eq_* are GL's equation values (0x8006 add, ...).
    void setBlendEquation(int eq, int eqAlpha);
    int blendEquation() const { return blendEq_; }
    int blendEquationAlpha() const { return blendEqA_; }
    void setAlphaBlend(bool on);
    void setColorWrite(bool r, bool g, bool b, bool a);
    void setAlphaTest(bool on);
    void setAlphaTestRef(double ref);
    void setFog(bool on, uint32_t color);
    // gpu_push_state / gpu_pop_state / gpu_get_*: the GPU state GameMaker tracks.
    struct GpuState {
        int blendSrc, blendDst, blendSrcA, blendDstA, blendMode;
        bool alphaBlend, alphaTest;
        double alphaRef;
        bool colorMask[4];
        bool fog;
        uint32_t fogColor;
        bool filter;
        bool zTest, zWrite;
        int zFunc, cull;
        int blendEq, blendEqA;
    };
    GpuState gpuState() const;
    void setGpuState(const GpuState& s);
    std::vector<GpuState> gpuStack;
    int blendMode() const { return blendMode_; }
    int blendSrc() const { return blendSrc_; }
    int blendDst() const { return blendDst_; }
    int blendSrcAlpha() const { return blendSrcA_; }
    int blendDstAlpha() const { return blendDstA_; }
    bool alphaBlendOn() const { return alphaBlend_; }
    bool alphaTestOn() const { return alphaTest_; }
    double alphaTestRef() const { return alphaRef_; }
    const bool* colorMask() const { return colorMask_; }
    bool fogOn() const { return fogOn_; }
    uint32_t fogColor() const { return fogColor_; }
    bool texFilter() const { return texFilter_; }
    bool texRepeat = false;  // gpu_set_tex_repeat
    void setTexRepeat(bool on);
    void setScissor(int x, int y, int w, int h) { r_.setScissor(x, y, w, h); }
    void scissor(int& x, int& y, int& w, int& h) const { r_.scissor(x, y, w, h); }
    void shaderUniformWorldMatrix(int location);  // shader_set_uniform_matrix
    void setTexFilter(bool linear);
    // 3D state (gpu_set_ztestenable, gpu_set_zwriteenable, gpu_set_zfunc, gpu_set_cullmode).
    void setZTest(bool on);
    void setZWrite(bool on);
    void setZFunc(int func);
    void setCullMode(int mode);
    bool zTest() const { return zTest_; }
    bool zWrite() const { return zWrite_; }
    int zFunc() const { return zFunc_; }
    int cullMode() const { return cull_; }
    // View and projection for the rest of the current target (camera_apply, matrix_set).
    void setViewProjection(const float* view, const float* proj);
    bool shaderSet(int shader);
    void shaderReset();
    int currentShader() const { return shader_; }
    int shaderUniform(int shader, const std::string& name);
    void shaderUniformF(int location, const float* v, int n);
    void shaderUniformArray(int location, const float* v, int n);  // shader_set_uniform_f_array
    bool shaderCompiled(int shader) const;

    // ---- sprites and backgrounds
    void drawSprite(int sprite, double subimg, double x, double y, double xs, double ys, double rot, uint32_t col,
                    double a);
    void drawSpritePart(int sprite, double subimg, double left, double top, double w, double h, double x, double y,
                        double xs, double ys, uint32_t col, double a);
    void drawSpriteStretched(int sprite, double subimg, double x, double y, double w, double h, uint32_t col,
                             double a);
    // A nine-slice sprite frame drawn w x h (negative flips) about its origin scaled to that size,
    // or about the top-left corner (zeroOrigin, draw_sprite_stretched).
    void drawNineSlice(int sprite, int tpag, double x, double y, double w, double h, double rot, uint32_t col,
                       double a, bool zeroOrigin);
    void drawBackground(int bg, double x, double y, double xs, double ys, double rot, uint32_t col, double a);
    void drawBackgroundPart(int bg, double left, double top, double w, double h, double x, double y, double xs,
                            double ys, uint32_t col, double a);
    void drawBackgroundTiled(int bg, double x, double y, double xs, double ys, bool htiled, bool vtiled,
                             uint32_t col, double a);
    void backgroundSize(int bg, int& w, int& h) const;
    void drawSelf(Instance* inst);
    // Triangles from a texture page item: room positions, 0-1 coordinates over the item (skeleton sprites).
    void drawTpagTriangles(int tpag, const float* xy, const float* uv, int vertexCount, const uint16_t* indices,
                           int indexCount, uint32_t col, double a);
    // gpu_set_depth: the z of what is drawn next (0 until a game sets it; reset for each
    // instance's draw events, as GameMaker resets it to the depth being drawn), and the depth
    // gpu_get_depth reports otherwise (the layer or instance being drawn).
    double drawZ = 0, drawDepth = 0;
    bool drawZSet = false;
    int texMipEnable = 0;  // gpu_set_tex_mip_enable: recorded (mipmaps aren't generated)
    int spriteFrame(int sprite, double subimg) const;

    // ---- primitives
    void drawRect(double x1, double y1, double x2, double y2, bool outline, uint32_t c1, uint32_t c2, uint32_t c3,
                  uint32_t c4, double a);
    void drawLine(double x1, double y1, double x2, double y2, double width, uint32_t c1, uint32_t c2, double a);
    void drawEllipse(double x1, double y1, double x2, double y2, bool outline, uint32_t c1, uint32_t c2, double a);
    void drawPoint(double x, double y, uint32_t c, double a);
    void primitiveBegin(int kind, int texture = -1);
    void primitiveVertex(double x, double y, uint32_t c, double a, double u = 0, double v = 0);
    void primitiveEnd();
    void drawClear(uint32_t c, double a);

    // ---- text
    int addSpriteFont(int sprite, const std::u32string& map, bool proportional, int sep);
    bool replaceSpriteFont(int font, int sprite, const std::u32string& map, bool proportional, int sep);
    int addPlaceholderFont();
    // font_add: a TrueType font from `data` (or `owned`), with the glyphs `first`..`last` ready
    // and the rest made when text needs them; -1 if the data isn't a font.
    int addTrueTypeFont(const uint8_t* data, std::vector<uint8_t> owned, double points, bool bold, bool italic,
                        int first, int last);
    double fontPoints(int font) const;  // font_get_size for fonts added at run time
    // font_get_info for fonts added at run time (sprite and TrueType fonts).
    struct RuntimeGlyph {
        char32_t ch;
        int frame;  // sprite fonts: the sprite frame (font_get_info reports it as the glyph's "char")
        int x, y, w, h;
        double advance, offset;
    };
    struct RuntimeFontInfo {
        int sprite = -1;
        double points = 0;
        bool trueType = false;
        std::vector<RuntimeGlyph> glyphs;
    };
    bool runtimeFontInfo(int font, RuntimeFontInfo& out) const;
    bool fontExists(int font) const;
    void deleteFont(int font);  // font_delete: fonts added at runtime only
    void drawText(double x, double y, const std::string& s, double sep, double w, double xs, double ys, double rot,
                  uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, double a);
    double stringWidth(const std::string& s, double sep = -1, double w = -1);
    double stringHeight(const std::string& s, double sep = -1, double w = -1);

    // ---- surfaces
    int surfaceCreate(int w, int h);
    void surfaceFree(int id);
    bool surfaceExists(int id) const;
    int surfaceWidth(int id) const;
    int surfaceHeight(int id) const;
    bool surfaceSetTarget(int id);
    bool surfaceResetTarget();
    void surfaceCopy(int dest, double x, double y, int src);
    void surfaceResize(int id, int w, int h);
    void drawSurface(int id, double x, double y, double xs, double ys, double rot, uint32_t col, double a);
    void drawSurfacePart(int id, double left, double top, double w, double h, double x, double y, double xs,
                         double ys, uint32_t col, double a);
    void drawSurfaceStretched(int id, double x, double y, double w, double h, uint32_t col, double a);
    void drawSurfaceTiled(int id, double x, double y, double xs, double ys, uint32_t col, double a);
    void drawSpriteGeneral(int sprite, double subimg, double left, double top, double w, double h, double x, double y,
                           double xs, double ys, double rot, const uint32_t (&cols)[4], double a);
    // draw_surface_general: part of a surface, scaled, rotated about (x, y), one colour per corner.
    void drawSurfaceGeneral(int id, double left, double top, double w, double h, double x, double y, double xs,
                            double ys, double rot, const uint32_t (&cols)[4], double a);
    void surfaceCopyPart(int dest, double x, double y, int src, double left, double top, double w, double h);
    int surfaceGetTarget() const;  // the GML surface being drawn to, -1 for the window
    // Tile maps: the tileset's current animation frame, one tile, and a whole tile map at a position.
    int tilesetFrame(int tileset) const;
    void drawTileData(int tileset, uint32_t data, int frame, double x, double y, uint32_t col, double a);
    void drawTilemapAt(const struct LayerElement& e, double x, double y);

    // GML texture handles (sprite_get_texture, surface_get_texture): a texture page index, or
    // kSurfaceTexBase + surface id; -1 is "no texture".
    static constexpr int kSurfaceTexBase = 1 << 20;
    bool textureInfo(double handle, uint32_t& tex, int& w, int& h);
    int spriteTexture(int sprite, double subimg) const;
    // sprite_get_uvs: page UVs (left, top, right, bottom), trimmed x/y offset, kept width/height ratio.
    bool spriteUVs(int sprite, double subimg, double (&out)[8]) const;
    // vertex_submit: raw vertices in a GameMaker vertex format, with a texture handle (-1 = none).
    void drawVertexBuffer(uint32_t glMode, double texture, const uint8_t* data, int count, int stride,
                          const std::vector<Renderer::Attr>& attrs);
    // draw_sprite_pos: the frame's texture stretched over four corners (TL, TR, BR, BL).
    void drawSpritePos(int sprite, double subimg, const double (&px)[4], const double (&py)[4], double a);
    // A sprite frame through a 2D affine transform of its own pixel space (0,0 = the frame's
    // top-left): x' = m[0]x + m[2]y + m[4], y' = m[1]x + m[3]y + m[5]. Used by sequences.
    void drawSpriteAffine(int sprite, double subimg, const double (&m)[6], uint32_t col, double a);

    // Built-in particle shapes (pt_shape_pixel .. pt_shape_snow): 64x64 white images centred on
    // (32, 32), generated procedurally to resemble GameMaker's.
    void drawParticleShape(int shape, double x, double y, double xs, double ys, double rot, uint32_t col, double a);

    // Surface pixels (top row first, RGBA) for surface_getpixel, buffer_get/set_surface, sprite_create_from_surface.
    bool readSurface(int id, int x, int y, int w, int h, std::vector<uint8_t>& rgba);
    // draw_getpixel: pixels of the target being drawn to (top-left origin).
    bool readCurrentTarget(int x, int y, int w, int h, std::vector<uint8_t>& rgba);
    bool writeSurface(int id, const uint8_t* rgba, int w, int h);
    // A new texture page (run-time sprites); returns its index.
    int addPage(int w, int h, std::vector<uint8_t> rgba);

    // Texture page access for fonts and screenshots.
    struct Page {
        int w = 0, h = 0;
        std::vector<uint8_t> pixels;
        uint32_t tex = 0;
        uint64_t lastUsed = 0;  // frame it was last drawn from
        bool failed = false;    // couldn't be decoded: not tried again
    };
    const std::vector<Page>& pages() const { return pages_; }
    // A page for drawing: data.win pages are decoded and uploaded the first time they're used.
    const Page& page(int index);

private:
    struct Glyph {
        int frame = -1;      // sprite font frame
        int dataIndex = -1;  // data font glyph
        double offset = 0;   // trimmed left edge for proportional sprite fonts
        double width = 0;
        double advance = 0;
        int ax = 0, ay = 0, aw = 0, ah = 0;  // TrueType fonts: the cell in the face's atlas
    };
    struct FontRec {
        bool live = false;
        bool spriteFont = false;
        int sprite = -1;
        int dataFont = -1;
        double height = 12;
        std::unordered_map<char32_t, Glyph> glyphs;
        double spaceAdvance = 8;
        int ttf = -1;        // TrueType fonts: index into ttfs_
        double points = 0;   // their size
    };
    struct TtfFont {
        std::unique_ptr<TrueTypeFace> face;
        uint32_t tex = 0;
        int texW = 0, texH = 0;
    };

    void drawQuad(uint32_t tex, const double (&px)[4], const double (&py)[4], double u0, double v0, double u1,
                  double v1, const uint32_t (&c)[4]);
    void drawTpag(int tpag, double x, double y, double ox, double oy, double xs, double ys, double rot,
                  uint32_t col, double a);
    void drawTpagPart(int tpag, double left, double top, double w, double h, double x, double y, double xs,
                      double ys, double rot, uint32_t col, double a, double ox = 0, double oy = 0);
    void drawLayer(int32_t layerId, uint32_t drawSubtype);
    void drawBackgroundElement(const struct Layer& L, const struct LayerElement& e);
    void drawTilemap(const struct Layer& L, const struct LayerElement& e);
    void drawTexRect(uint32_t tex, int texW, int texH, double sx, double sy, double sw, double sh, double lx,
                     double ly, double lw, double lh, double x, double y, double xs, double ys, double rot,
                     uint32_t col, double a);
    void drawTexRect4(uint32_t tex, int texW, int texH, double sx, double sy, double sw, double sh, double lx,
                      double ly, double lw, double lh, double x, double y, double xs, double ys, double rot,
                      const uint32_t (&packed)[4]);
    struct TargetState {
        int target;
        int vpX, vpY, vpW, vpH;
        double px, py, pw, ph, pa;
        bool custom = false;  // view / projection matrices set by a camera or matrix_set
        std::array<float, 16> view{}, proj{};
    };
    TargetState currentState() const;
    // Layer filters (Filters.cpp): the game's own filter shaders run over a layer (single-layer
    // effects) or over everything drawn up to it (effect layers), as YoYo's yyFilterHost.
    struct EffectParam {
        std::string name;
        int type = 0;      // 0 float, 1 int, 2 bool, 3 sampler
        double convert = 0;  // convertValueToTimeModulated
        bool repeat = false, filter = false;  // sampler options
        Value defaults;
    };
    struct EffectInfo {
        int shader = -1;
        std::vector<EffectParam> params;
    };
    struct FilterPass {
        bool active = false, single = false;
        Value fx;
        const EffectInfo* info = nullptr;
        TargetState saved{};
        GpuState gpu{};
        int temp = -1;
    };
    const EffectInfo* effectInfo(const std::string& name);
    FilterPass beginLayerFilter(const struct Layer& L);
    void endLayerFilter(FilterPass& pass);
    int acquireTempTarget(int w, int h, int avoid = -1);
    void filterQuad(uint32_t tex);
    std::unordered_map<std::string, std::unique_ptr<EffectInfo>> effectInfos_;
    std::vector<int> tempTargets_, tempInUse_;
    std::unordered_map<const void*, double> fxStart_;
    void restoreState(const TargetState& s);
    void bindAndSetup(int target, int w, int h);
    void applyTargetProjection();
    void drawRoomPass();
    void drawRoomBackgrounds(bool foreground);
    void drawTile(const struct Tile& t);
    void runDrawEvent(uint32_t subtype, bool sortedByDepth);
    void ensureBackbuffer();
    void ensureAppSurface();
    void loadFonts();
    FontRec* currentFont();  // the font set, or the default font
    bool makeTrueTypeFont(FontRec& rec, const uint8_t* data, std::vector<uint8_t> owned, double points, bool bold,
                          bool italic = false);
    void bakeGlyphs(FontRec& f, const std::u32string& s);  // TrueType glyphs `s` needs
    uint32_t ttfTexture(TtfFont& t);                        // its atlas, uploaded when it changed
    void layoutLines(const std::u32string& s, double w, double sep, const FontRec* f, std::vector<std::u32string>& lines,
                     std::vector<double>& widths, double& lineHeight);
    double glyphRun(const FontRec* f, const std::u32string& line) const;
    void opaqueColumns(int tpag, int& first, int& last);

    Runner& rt_;
    Renderer r_;
    std::vector<TtfFont> ttfs_;
    FontRec defaultFont_;  // text drawn with no font set: GameMaker's Arial 12, as Liberation Sans 12
    bool defaultFontTried_ = false;
    bool enabled_ = false;
    std::vector<Page> pages_;
    // Pages are loaded when first drawn, as GameMaker does; past the budget (devices with little
    // memory) the ones unused longest are dropped at the end of a frame, to be loaded again later.
    void loadPage(int index);
    void dropIdlePages();
    uint64_t frame_ = 1;
    size_t pageBytes_ = 0, pageBudget_ = 0;  // 0: no budget
    uint32_t particleShapes_[14] = {};
    std::vector<FontRec> fonts_;
    std::vector<int> surfaces_;  // GML surface id -> renderer target (-1 when freed)
    std::vector<TargetState> targetStack_;
    int curTarget_ = -1;
    int vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0;
    int backbuffer_ = -1;   // renderer target for the virtual window
    int appSurface_ = -1;   // GML surface id
    int winW_ = 0, winH_ = 0;
    double guiW_ = 0, guiH_ = 0;                                     // 0: the window size
    double guiScaleX_ = 0, guiScaleY_ = 0, guiOffX_ = 0, guiOffY_ = 0;  // display_set_gui_maximize
    bool windowSetByGame_ = false;
    int appW_ = 0, appH_ = 0;
    bool appSized_ = false;  // GMS2: sized once, by the first room
    int curTargetW_ = 0, curTargetH_ = 0;
    double projX_ = 0, projY_ = 0, projW_ = 0, projH_ = 0, projAngle_ = 0;
    int shader_ = -1;
    int blendSrc_ = 5, blendDst_ = 6, blendSrcA_ = 5, blendDstA_ = 6, blendMode_ = 0;
    std::vector<Vertex> tris_;  // drawTpagTriangles' vertices
    bool alphaBlend_ = true;
    int blendEq_ = 0x8006, blendEqA_ = 0x8006;
    bool alphaTest_ = false;
    double alphaRef_ = 0;
    bool colorMask_[4] = {true, true, true, true};
    bool fogOn_ = false;
    uint32_t fogColor_ = 0;
    bool texFilter_ = false;
    bool zTest_ = false, zWrite_ = false;
    int zFunc_ = 4, cull_ = 0;
    bool customMats_ = false;
    std::array<float, 16> customView_{}, customProj_{};
    // primitive under construction
    int primKind_ = 0;
    uint32_t primTex_ = 0;
    std::vector<Vertex> prim_;
    // last presented frame placement on the real screen (for mouse mapping)
    double presX_ = 0, presY_ = 0, presW_ = 1, presH_ = 1;
    int screenW_ = 0, screenH_ = 0;
};

}  // namespace gm
