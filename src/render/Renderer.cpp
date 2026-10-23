#include "Renderer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "GL.h"

namespace gm {

using namespace gl;

// The built-in shader. Lighting follows YoYo's fixed-function emulation (fullShader.js): up to
// 8 directional / point lights plus ambient, used only for vertices that carry normals.
static const char* kDefaultVS = R"(
attribute vec3 in_Position;
attribute vec4 in_Colour;
attribute vec2 in_TextureCoord;
attribute vec3 in_Normal;
uniform mat4 gm_Matrices[5];
uniform bool gm_LightingEnabled;
uniform vec4 gm_AmbientColour;
uniform vec4 gm_Lights_Direction[8];
uniform vec4 gm_Lights_PosRange[8];
uniform vec4 gm_Lights_Colour[8];
varying vec2 v_vTexcoord;
varying vec4 v_vColour;
void main() {
    vec4 pos = vec4(in_Position, 1.0);
    gl_Position = gm_Matrices[4] * pos;
    gl_PointSize = 1.0;
    v_vColour = in_Colour;
    if (gm_LightingEnabled) {
        vec3 n = -normalize((gm_Matrices[2] * vec4(in_Normal, 0.0)).xyz);
        vec3 wpos = (gm_Matrices[2] * pos).xyz;
        vec4 acc = vec4(0.0);
        for (int i = 0; i < 8; i++) {
            acc += max(0.0, dot(n, gm_Lights_Direction[i].xyz)) * gm_Lights_Colour[i];
            vec4 pr = gm_Lights_PosRange[i];
            vec3 d = wpos - pr.xyz;
            float len = length(d);
            float atten = (pr.w > 0.0 && len <= pr.w) ? 1.0 / (1.0 + len / pr.w) : 0.0;
            if (len > 0.0) acc += gm_Lights_Colour[i] * max(0.0, dot(n, d / len)) * atten;
        }
        acc = min(vec4(1.0), acc * in_Colour + gm_AmbientColour);
        v_vColour = vec4(acc.rgb, in_Colour.a);
    }
    v_vTexcoord = in_TextureCoord;
}
)";

static const char* kDefaultFS = R"(
precision mediump float;
varying vec2 v_vTexcoord;
varying vec4 v_vColour;
uniform sampler2D gm_BaseTexture;
uniform bool gm_PS_FogEnabled;
uniform vec4 gm_FogColour;
uniform bool gm_AlphaTestEnabled;
uniform float gm_AlphaRefValue;
void main() {
    vec4 c = v_vColour * texture2D(gm_BaseTexture, v_vTexcoord);
    if (gm_AlphaTestEnabled && c.a <= gm_AlphaRefValue) discard;
    if (gm_PS_FogEnabled) c.rgb = gm_FogColour.rgb;
    gl_FragColor = c;
}
)";

bool Renderer::init(bool enabled, bool gles) {
    enabled_ = enabled;
    gles_ = gles;
    for (int i = 0; i < 16; i++) world_[i] = view_[i] = proj_[i] = (i % 5 == 0) ? 1.f : 0.f;
    verts_.reserve(1 << 16);
    if (!enabled_) return true;

    GLint screen = 0;
    GetIntegerv(FRAMEBUFFER_BINDING, &screen);
    screenFbo_ = static_cast<uint32_t>(screen);
    GenBuffers(1, &vbo_);
    uint8_t px[4] = {255, 255, 255, 255};
    white_ = createTexture(1, 1, px);
    tex_ = white_;
    if (!buildProgram(builtin_, kDefaultVS, kDefaultFS, "<builtin>")) {
        enabled_ = false;
        return false;
    }
    Disable(DEPTH_TEST);
    Disable(CULL_FACE);
    Enable(BLEND);
    setBlend(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, SRC_ALPHA, ONE_MINUS_SRC_ALPHA, FUNC_ADD);
    fprintf(stderr, "[render] %s | %s | GLSL %s\n", GetString(VERSION), GetString(RENDERER),
            GetString(SHADING_LANGUAGE_VERSION));
    const char* ext = reinterpret_cast<const char*>(GetString(EXTENSIONS));
    if (gles_) {
        repeatAllowed_ = ext && std::strstr(ext, "GL_OES_texture_npot");
        packedDepthStencil_ = ext && std::strstr(ext, "GL_OES_packed_depth_stencil");
        // Core in OpenGL ES 3 (where the list may leave it out), and still switched on by name in
        // the ESSL 1.00 shaders games ship.
        const char* ver = reinterpret_cast<const char*>(GetString(VERSION));  // "OpenGL ES 3.0 ..."
        bool es3 = ver && std::strncmp(ver, "OpenGL ES ", 10) == 0 && ver[10] >= '3';
        derivatives_ = es3 || (ext && std::strstr(ext, "GL_OES_standard_derivatives"));
    } else {
        const char* ver = reinterpret_cast<const char*>(GetString(VERSION));
        packedDepthStencil_ = (ver && ver[0] >= '3') ||
                              (ext && (std::strstr(ext, "GL_EXT_packed_depth_stencil") || std::strstr(ext, "GL_ARB_framebuffer_object")));
    }
    return true;
}

uint32_t Renderer::createTexture(int w, int h, const uint8_t* rgba) {
    if (!enabled_) return 0;
    GLuint t;
    GenTextures(1, &t);
    BindTexture(TEXTURE_2D, t);
    PixelStorei(UNPACK_ALIGNMENT, 1);
    TexImage2D(TEXTURE_2D, 0, RGBA, w, h, 0, RGBA, UNSIGNED_BYTE, rgba);
    TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, NEAREST);
    TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, NEAREST);
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
    BindTexture(TEXTURE_2D, tex_);
    return t;
}

void Renderer::deleteTexture(uint32_t tex) {
    if (!enabled_ || !tex) return;
    if (tex == tex_) {
        flush();
        tex_ = white_;
    }
    GLuint t = tex;
    DeleteTextures(1, &t);
}

int Renderer::createTarget(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    Target t;
    t.w = w;
    t.h = h;
    t.live = true;
    if (enabled_) {
        flush();
        t.tex = createTexture(w, h, nullptr);
        GLint prev = 0;
        GetIntegerv(FRAMEBUFFER_BINDING, &prev);
        GenFramebuffers(1, &t.fbo);
        BindFramebuffer(FRAMEBUFFER, t.fbo);
        FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, t.tex, 0);
        if (CheckFramebufferStatus(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "[render] framebuffer %dx%d incomplete\n", w, h);
        ClearColor(0, 0, 0, 0);
        Clear(COLOR_BUFFER_BIT);
        BindFramebuffer(FRAMEBUFFER, static_cast<GLuint>(prev));
    }
    for (size_t i = 0; i < targets_.size(); i++)
        if (!targets_[i].live) {
            targets_[i] = t;
            return static_cast<int>(i);
        }
    targets_.push_back(t);
    return static_cast<int>(targets_.size() - 1);
}

void Renderer::deleteTarget(int id) {
    if (!targetExists(id)) return;
    Target& t = targets_[id];
    if (enabled_) {
        flush();
        if (target_ == id) bindTarget(-1);
        DeleteFramebuffers(1, &t.fbo);
        if (t.depth) DeleteRenderbuffers(1, &t.depth);
        deleteTexture(t.tex);
    }
    t = Target();
}

bool Renderer::targetExists(int id) const {
    return id >= 0 && static_cast<size_t>(id) < targets_.size() && targets_[id].live;
}
int Renderer::targetWidth(int id) const { return targetExists(id) ? targets_[id].w : 0; }
int Renderer::targetHeight(int id) const { return targetExists(id) ? targets_[id].h : 0; }
uint32_t Renderer::targetTexture(int id) const { return targetExists(id) ? targets_[id].tex : 0; }

void Renderer::bindTarget(int id, int screenW, int screenH) {
    flush();
    target_ = targetExists(id) ? id : -1;
    if (target_ >= 0) {
        targetH_ = targets_[target_].h;
        targetFlipped_ = true;
        if (enabled_) BindFramebuffer(FRAMEBUFFER, targets_[target_].fbo);
    } else {
        targetH_ = screenH;
        targetFlipped_ = false;
        if (enabled_) BindFramebuffer(FRAMEBUFFER, screenFbo_);
    }
    if (target_ < 0 && screenW > 0) setViewport(0, 0, screenW, screenH);
    matrixVersion_++;
    applyDepthState();
}

void Renderer::setViewport(int x, int y, int w, int h) {
    flush();
    if (!enabled_) return;
    // Targets store row 0 at the top, so their viewport y is already top-based.
    int gy = targetFlipped_ ? y : targetH_ - y - h;
    Viewport(x, gy, w, h);
    setScissor(x, y, w, h);
}

void Renderer::setScissor(int x, int y, int w, int h) {
    flush();
    w = std::max(0, w);
    h = std::max(0, h);
    scissor_[0] = x, scissor_[1] = y, scissor_[2] = w, scissor_[3] = h;
    if (!enabled_) return;
    Enable(SCISSOR_TEST);
    Scissor(x, targetFlipped_ ? y : targetH_ - y - h, w, h);
}

void Renderer::setProjection(double x, double y, double w, double h, double angle) {
    flush();
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    // view = look-at from (cx, cy, -16000) towards +z with up (sin(-a), cos(-a), 0);
    // projection = orthographic w x h with z from 1 to 32000.
    double cx = x + w / 2, cy = y + h / 2;
    double a = angle * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
    // right = (ca, sa, 0), up = (-sa, ca, 0), look = (0, 0, 1)
    float v[16] = {static_cast<float>(ca), static_cast<float>(-sa), 0, 0,
                   static_cast<float>(sa), static_cast<float>(ca), 0, 0,
                   0, 0, 1, 0,
                   static_cast<float>(-(ca * cx + sa * cy)), static_cast<float>(-(-sa * cx + ca * cy)), 16000.f, 1};
    float p[16] = {static_cast<float>(2.0 / w), 0, 0, 0,
                   0, static_cast<float>(2.0 / h), 0, 0,
                   0, 0, 1.f / (32000.f - 1.f), 0,
                   0, 0, 1.f / (1.f - 32000.f), 1};
    std::memcpy(view_, v, sizeof v);
    std::memcpy(proj_, p, sizeof p);
    matrixVersion_++;
}

void Renderer::setViewProjection(const float* view, const float* proj) {
    flush();
    std::memcpy(view_, view, sizeof view_);
    std::memcpy(proj_, proj, sizeof proj_);
    matrixVersion_++;
}

void Renderer::setWorld(const float* world) {
    if (std::memcmp(world, world_, sizeof world_) == 0) return;
    flush();
    std::memcpy(world_, world, sizeof world_);
    matrixVersion_++;
}

// Row-vector product: out = a * b (apply a, then b).
static void mul4(const float* a, const float* b, float* out) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    std::memcpy(out, r, sizeof r);
}

void Renderer::setDepth(bool test, bool write, int func) {
    if (test == depthTest_ && write == depthWrite_ && func == depthFunc_) return;
    flush();
    depthTest_ = test;
    depthWrite_ = write;
    depthFunc_ = func;
    applyDepthState();
}

void Renderer::ensureDepthBuffer() {
    if (!enabled_ || target_ < 0) return;
    Target& t = targets_[target_];
    if (t.depth) return;
    GenRenderbuffers(1, &t.depth);
    BindRenderbuffer(RENDERBUFFER, t.depth);
    RenderbufferStorage(RENDERBUFFER, packedDepthStencil_ ? DEPTH24_STENCIL8 : DEPTH_COMPONENT16, t.w, t.h);
    FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, t.depth);
    if (packedDepthStencil_) FramebufferRenderbuffer(FRAMEBUFFER, STENCIL_ATTACHMENT, RENDERBUFFER, t.depth);
    DepthMask(TRUE_);
    StencilMask(0xFF);
    ClearStencil(0);
    Disable(SCISSOR_TEST);
    Clear(DEPTH_BUFFER_BIT | (packedDepthStencil_ ? STENCIL_BUFFER_BIT : 0));
    Enable(SCISSOR_TEST);
    StencilMask(static_cast<GLuint>(stencil_.writeMask));
}

void Renderer::applyDepthState() {
    if (!enabled_) return;
    if (depthTest_ || depthWrite_ || stencil_.enabled) ensureDepthBuffer();
    if (stencil_.enabled && packedDepthStencil_ && target_ >= 0) {
        static const GLenum funcs[9] = {ALWAYS, NEVER, LESS, EQUAL, LEQUAL, GREATER, NOTEQUAL, GEQUAL, ALWAYS};
        static const GLenum ops[9] = {KEEP, KEEP, ZERO, REPLACE, INCR, DECR, INVERT, INCR_WRAP, DECR_WRAP};
        auto op = [&](int o) { return ops[o >= 1 && o <= 8 ? o : 1]; };
        Enable(STENCIL_TEST);
        StencilFunc(funcs[stencil_.func >= 1 && stencil_.func <= 8 ? stencil_.func : 8], stencil_.ref,
                    static_cast<GLuint>(stencil_.readMask));
        StencilOp(op(stencil_.fail), op(stencil_.depthFail), op(stencil_.pass));
        StencilMask(static_cast<GLuint>(stencil_.writeMask));
    } else {
        Disable(STENCIL_TEST);
    }
    static const GLenum funcs[9] = {LEQUAL, NEVER, LESS, EQUAL, LEQUAL, GREATER, NOTEQUAL, GEQUAL, ALWAYS};
    if (depthTest_ || depthWrite_) {
        Enable(DEPTH_TEST);
        // Writing without testing: GL only writes depth while the test is enabled, so pass all.
        DepthFunc(depthTest_ ? funcs[depthFunc_ >= 1 && depthFunc_ <= 8 ? depthFunc_ : 4] : ALWAYS);
    } else {
        Disable(DEPTH_TEST);
    }
    DepthMask(depthWrite_ ? TRUE_ : FALSE_);
    if (cull_ == 0) {
        Disable(CULL_FACE);
    } else {
        // Screen-space clockwise is counter-clockwise in GL window space on render targets
        // (world y grows towards clip -1 there) and clockwise on the real screen.
        Enable(CULL_FACE);
        CullFace(BACK);
        bool cullCW = cull_ == 1;
        FrontFace((cullCW == targetFlipped_) ? CW : CCW);
    }
}

void Renderer::setStencil(const Stencil& st) {
    if (st == stencil_) return;
    flush();
    stencil_ = st;
    applyDepthState();
}

void Renderer::clearStencil(int value) {
    flush();
    if (!enabled_ || !packedDepthStencil_) return;
    ensureDepthBuffer();
    StencilMask(0xFF);
    ClearStencil(value);
    Disable(SCISSOR_TEST);
    Clear(STENCIL_BUFFER_BIT);
    Enable(SCISSOR_TEST);
    StencilMask(static_cast<GLuint>(stencil_.writeMask));
}

void Renderer::setCull(int mode) {
    if (mode == cull_) return;
    flush();
    cull_ = mode;
    applyDepthState();
}

void Renderer::clearDepth() {
    flush();
    if (!enabled_) return;
    ensureDepthBuffer();
    DepthMask(TRUE_);
    Disable(SCISSOR_TEST);
    Clear(DEPTH_BUFFER_BIT);
    Enable(SCISSOR_TEST);
    DepthMask(depthWrite_ ? TRUE_ : FALSE_);
}

void Renderer::setTextureStage(int stage, uint32_t tex) {
    if (stage <= 0) {
        setTexture(tex);
        return;
    }
    if (stage >= 8 || stages_[stage] == tex) return;
    flush();
    stages_[stage] = tex;
}

int Renderer::samplerStage(int shader, const std::string& name) {
    if (!shaderValid(shader)) return -1;
    Program& p = programs_[shader];
    GLint loc = GetUniformLocation(p.id, name.c_str());
    if (loc < 0) return -1;
    if (loc == p.uBaseTexture) return 0;
    for (int i = 1; i < 8; i++) {
        if (p.samplers[i] == loc) return i;
        if (p.samplers[i] < 0) {
            p.samplers[i] = loc;
            UseProgram(p.id);
            Uniform1i(loc, i);
            if (shader_ >= 0) UseProgram(programs_[shader_].id);
            return i;
        }
    }
    return -1;
}

void Renderer::bindStages(Program& p) {
    for (int i = 1; i < 8; i++)
        if (p.samplers[i] >= 0 && stages_[i]) {
            ActiveTexture(TEXTURE0 + i);
            BindTexture(TEXTURE_2D, stages_[i]);
        }
    ActiveTexture(TEXTURE0);
}

void Renderer::setLights(const Lights& l) {
    flush();
    lights_ = l;
    lightVersion_++;
}

int Renderer::attribLocation(Program& p, const std::string& name) {
    auto it = p.attribs.find(name);
    if (it != p.attribs.end()) return it->second;
    int loc = GetAttribLocation(p.id, name.c_str());
    p.attribs[name] = loc;
    return loc;
}

void Renderer::setTexture(uint32_t tex) {
    if (!tex) tex = white_;
    if (tex == tex_) return;
    flush();
    tex_ = tex;
}

void Renderer::setFiltering(bool linear) {
    if (linear == linear_) return;
    flush();
    linear_ = linear;
}

void Renderer::setTextureParams(uint32_t tex, bool repeat, bool linear) {
    if (!enabled_ || !tex) return;
    flush();
    BindTexture(TEXTURE_2D, tex);
    GLint wrap = repeat && repeatAllowed_ ? REPEAT : CLAMP_TO_EDGE;
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, wrap);
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, wrap);
    TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, linear ? LINEAR : NEAREST);
    TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, linear ? LINEAR : NEAREST);
    BindTexture(TEXTURE_2D, tex_);
}

void Renderer::setWrap(bool repeat) {
    repeat = repeat && repeatAllowed_;
    if (repeat == repeat_) return;
    flush();
    repeat_ = repeat;
}

void Renderer::setBlend(uint32_t src, uint32_t dst, uint32_t srcA, uint32_t dstA, uint32_t eq, uint32_t eqA) {
    if (blend_[0] == src && blend_[1] == dst && blend_[2] == srcA && blend_[3] == dstA && blend_[4] == eq &&
        blend_[5] == eqA)
        return;
    flush();
    blend_[0] = src;
    blend_[1] = dst;
    blend_[2] = srcA;
    blend_[3] = dstA;
    blend_[4] = eq;
    blend_[5] = eqA;
    if (!enabled_) return;
    BlendFuncSeparate(src, dst, srcA, dstA);
    if (eq == eqA) BlendEquation(eq);
    else BlendEquationSeparate(eq, eqA);
}

void Renderer::setColorMask(bool r, bool g, bool b, bool a) {
    if (mask_[0] == r && mask_[1] == g && mask_[2] == b && mask_[3] == a) return;
    flush();
    mask_[0] = r;
    mask_[1] = g;
    mask_[2] = b;
    mask_[3] = a;
    if (enabled_) ColorMask(r, g, b, a);
}

void Renderer::setAlphaTest(bool on, float ref) {
    if (on == alphaTest_ && ref == alphaRef_) return;
    flush();
    alphaTest_ = on;
    alphaRef_ = ref;
    stateVersion_++;
}

void Renderer::setFog(bool on, uint32_t color) {
    if (on == fog_ && color == fogColor_) return;
    flush();
    fog_ = on;
    fogColor_ = color;
    stateVersion_++;
}

void Renderer::setShader(int shader) {
    if (shader == shader_) return;
    flush();
    shader_ = shaderValid(shader) ? shader : -1;
}

void Renderer::add(Prim prim, const Vertex* v, int n) {
    if (!enabled_ || n <= 0) return;
    if (prim != prim_) {
        flush();
        prim_ = prim;
    }
    verts_.insert(verts_.end(), v, v + n);
    if (verts_.size() > (1 << 16) - 6) flush();
}

void Renderer::clear(uint32_t color, double alpha) {
    flush();
    if (!enabled_) return;
    ColorMask(true, true, true, true);
    ClearColor((color & 0xFF) / 255.f, ((color >> 8) & 0xFF) / 255.f, ((color >> 16) & 0xFF) / 255.f,
               static_cast<float>(alpha));
    bool depth = target_ >= 0 && targets_[target_].depth;
    if (depth) DepthMask(TRUE_);
    Disable(SCISSOR_TEST);
    Clear(COLOR_BUFFER_BIT | (depth ? DEPTH_BUFFER_BIT : 0));
    Enable(SCISSOR_TEST);
    if (depth) DepthMask(depthWrite_ ? TRUE_ : FALSE_);
    ColorMask(mask_[0], mask_[1], mask_[2], mask_[3]);
}

void Renderer::useProgram(Program& p) {
    UseProgram(p.id);
    if (p.matrixVersion != matrixVersion_) {
        if (p.uMatrices >= 0) {
            // view, projection, world, world-view, world-view-projection. The real screen
            // shows clip +1 at the top, so its projection is flipped.
            float all[16 * 5];
            std::memcpy(all + 0, view_, sizeof view_);
            std::memcpy(all + 16, proj_, sizeof proj_);
            if (!targetFlipped_)
                for (int r = 0; r < 4; r++) all[16 + r * 4 + 1] = -all[16 + r * 4 + 1];
            std::memcpy(all + 32, world_, sizeof world_);
            mul4(world_, view_, all + 48);
            mul4(all + 48, all + 16, all + 64);
            UniformMatrix4fv(p.uMatrices, 5, FALSE_, all);
        }
        p.matrixVersion = matrixVersion_;
    }
    if (p.stateVersion != stateVersion_) {
        if (p.uAlphaTest >= 0) Uniform1i(p.uAlphaTest, alphaTest_);
        if (p.uAlphaRef >= 0) Uniform1f(p.uAlphaRef, alphaRef_);
        if (p.uFogPS >= 0) Uniform1i(p.uFogPS, fog_);
        if (p.uFogVS >= 0) Uniform1i(p.uFogVS, fog_);
        if (p.uFogColour >= 0)
            Uniform4f(p.uFogColour, (fogColor_ & 0xFF) / 255.f, ((fogColor_ >> 8) & 0xFF) / 255.f,
                      ((fogColor_ >> 16) & 0xFF) / 255.f, 1.f);
        if (p.uFogStart >= 0) Uniform1f(p.uFogStart, 0.f);
        if (p.uRcpFogRange >= 0) Uniform1f(p.uRcpFogRange, 0.f);
        if (p.uBaseTexture >= 0) Uniform1i(p.uBaseTexture, 0);
        p.stateVersion = stateVersion_;
    }
    if (p.lightVersion != lightVersion_) {
        float dir[32] = {}, pos[32] = {}, col[32] = {}, amb[4];
        auto unpack = [](uint32_t c, float* o, float a) {
            o[0] = (c & 0xFF) / 255.f;
            o[1] = ((c >> 8) & 0xFF) / 255.f;
            o[2] = ((c >> 16) & 0xFF) / 255.f;
            o[3] = a;
        };
        for (int i = 0; i < 8; i++) {
            if (!lights_.on[i]) continue;
            std::memcpy(lights_.point[i] ? pos + i * 4 : dir + i * 4, lights_.dirOrPos[i], 4 * sizeof(float));
            unpack(lights_.color[i], col + i * 4, 1.f);
        }
        unpack(lights_.ambient, amb, ((lights_.ambient >> 24) & 0xFF) / 255.f);
        if (p.uLightDir >= 0) Uniform4fv(p.uLightDir, 8, dir);
        if (p.uLightPosRange >= 0) Uniform4fv(p.uLightPosRange, 8, pos);
        if (p.uLightColour >= 0) Uniform4fv(p.uLightColour, 8, col);
        if (p.uAmbient >= 0) Uniform4fv(p.uAmbient, 1, amb);
        p.lightVersion = lightVersion_;
    }
}

void Renderer::flush() {
    if (verts_.empty()) return;
    if (!enabled_) {
        verts_.clear();
        return;
    }
    Program& p = shader_ >= 0 ? programs_[shader_] : builtin_;
    useProgram(p);
    if (p.uLighting >= 0) Uniform1i(p.uLighting, 0);  // batched 2D vertices carry no normals
    bindStages(p);
    ActiveTexture(TEXTURE0);
    BindTexture(TEXTURE_2D, tex_);
    GLint filter = linear_ ? LINEAR : NEAREST;
    TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, filter);
    TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, filter);
    GLint wrap = repeat_ ? REPEAT : CLAMP_TO_EDGE;
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, wrap);
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, wrap);
    BindBuffer(ARRAY_BUFFER, vbo_);
    BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(verts_.size() * sizeof(Vertex)), verts_.data(), STREAM_DRAW);
    EnableVertexAttribArray(0);
    EnableVertexAttribArray(1);
    EnableVertexAttribArray(2);
    VertexAttribPointer(0, 3, FLOAT, FALSE_, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, x)));
    VertexAttribPointer(1, 4, UNSIGNED_BYTE, TRUE_, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, color)));
    VertexAttribPointer(2, 2, FLOAT, FALSE_, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));
    GLenum mode = prim_ == TRIANGLES ? gl::TRIANGLES : (prim_ == LINES ? gl::LINES : gl::POINTS);
    DrawArrays(mode, 0, static_cast<GLsizei>(verts_.size()));
    drawCalls_++;
    verts_.clear();
}

void Renderer::drawRaw(uint32_t glMode, const uint8_t* data, int count, int stride, const std::vector<Attr>& attrs) {
    flush();
    if (!enabled_ || count <= 0 || stride <= 0) return;
    Program& p = shader_ >= 0 ? programs_[shader_] : builtin_;
    useProgram(p);
    bindStages(p);
    ActiveTexture(TEXTURE0);
    BindTexture(TEXTURE_2D, tex_);
    GLint filter = linear_ ? LINEAR : NEAREST;
    TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, filter);
    TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, filter);
    GLint wrap = repeat_ ? REPEAT : CLAMP_TO_EDGE;
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, wrap);
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, wrap);
    BindBuffer(ARRAY_BUFFER, vbo_);
    BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(count) * stride, data, STREAM_DRAW);
    // GameMaker's attribute names: in_Position, in_Colour(n), in_TextureCoord(n), in_Normal, ...
    static const char* names[15] = {nullptr, "in_Position", "in_Colour", "in_Normal", "in_TextureCoord",
                                    "in_BlendWeight", "in_BlendIndices", "in_PointSize", "in_Tangent",
                                    "in_Binormal", nullptr, nullptr, "in_Fog", "in_Depth", "in_Sample"};
    std::vector<GLint> used;
    bool hasColour = false, hasNormal = false;
    for (const Attr& a : attrs) {
        if (a.usage < 1 || a.usage > 14 || !names[a.usage]) continue;
        std::string base = names[a.usage];
        GLint loc = -1;
        if (a.index == 0) loc = attribLocation(p, base);
        if (loc < 0) loc = attribLocation(p, base + std::to_string(a.index));
        if (loc < 0) continue;
        EnableVertexAttribArray(static_cast<GLuint>(loc));
        VertexAttribPointer(static_cast<GLuint>(loc), a.components, a.type, a.normalized ? TRUE_ : FALSE_, stride,
                            reinterpret_cast<void*>(static_cast<intptr_t>(a.offset)));
        used.push_back(loc);
        hasColour |= a.usage == 2;
        hasNormal |= a.usage == 3;
    }
    // Missing attributes the shader reads: white colour, zero UVs / normal.
    if (!hasColour) {
        DisableVertexAttribArray(1);
        VertexAttrib4f(1, 1, 1, 1, 1);
    }
    for (GLuint loc : {0u, 2u, 3u})
        if (std::find(used.begin(), used.end(), static_cast<GLint>(loc)) == used.end()) {
            DisableVertexAttribArray(loc);
            VertexAttrib4f(loc, 0, 0, 0, 1);
        }
    if (p.uLighting >= 0) Uniform1i(p.uLighting, lights_.enabled && hasNormal ? 1 : 0);
    DrawArrays(glMode, 0, count);
    drawCalls_++;
    // Later batches only set up locations 0-2.
    for (GLint loc : used)
        if (loc > 2) DisableVertexAttribArray(static_cast<GLuint>(loc));
}

void Renderer::readPixels(int x, int y, int w, int h, uint8_t* rgba) {
    flush();
    if (!enabled_) {
        std::memset(rgba, 0, static_cast<size_t>(w) * h * 4);
        return;
    }
    PixelStorei(PACK_ALIGNMENT, 1);
    int gy = targetFlipped_ ? y : targetH_ - y - h;
    ReadPixels(x, gy, w, h, RGBA, UNSIGNED_BYTE, rgba);
    if (!targetFlipped_) {  // default framebuffer is bottom-up
        std::vector<uint8_t> row(static_cast<size_t>(w) * 4);
        for (int r = 0; r < h / 2; r++) {
            uint8_t* a = rgba + static_cast<size_t>(r) * w * 4;
            uint8_t* b = rgba + static_cast<size_t>(h - 1 - r) * w * 4;
            std::memcpy(row.data(), a, row.size());
            std::memcpy(a, b, row.size());
            std::memcpy(b, row.data(), row.size());
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Shaders

// GameMaker shaders are GLSL ES 1.00, where the non-square matrix names (mat2x3 ... mat4x4) are
// free for the game's own functions and variables; GLSL 1.20 and some phone drivers reserve them
// as types. In ES 1.00 source they can only be the game's, so they get a name of their own.
static std::string renameMatrixNames(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (size_t i = 0; i < src.size(); i++) {
        if (src.compare(i, 3, "mat") == 0 && i + 6 <= src.size() && src[i + 3] >= '2' && src[i + 3] <= '4' &&
            src[i + 4] == 'x' && src[i + 5] >= '2' && src[i + 5] <= '4' && (i == 0 || !ident(src[i - 1])) &&
            (i + 6 == src.size() || !ident(src[i + 6]))) {
            out += "gmr_";
        }
        out += src[i];
    }
    return out;
}

std::string Renderer::adaptSource(const std::string& original, bool fragment) const {
    const std::string src = renameMatrixNames(original);
    if (gles_) {
        // ESSL 1.00 wants #version first and #extension before anything else, so those lines go to
        // the top; shaders using derivatives (dFdx, dFdy, fwidth) get the extension that has them
        // switched on, as GameMaker's own GLES runners do.
        std::string version, extensions, body;
        size_t pos = 0;
        while (pos < src.size()) {
            size_t nl = src.find('\n', pos);
            if (nl == std::string::npos) nl = src.size();
            std::string line = src.substr(pos, nl - pos);
            size_t first = line.find_first_not_of(" \t");
            if (first != std::string::npos && line.compare(first, 8, "#version") == 0) version = line + '\n';
            else if (first != std::string::npos && line.compare(first, 10, "#extension") == 0) extensions += line + '\n';
            else body += line + '\n';
            pos = nl + 1;
        }
        bool usesDerivatives = src.find("dFdx") != std::string::npos || src.find("dFdy") != std::string::npos ||
                               src.find("fwidth") != std::string::npos;
        if (derivatives_ && usesDerivatives && extensions.find("GL_OES_standard_derivatives") == std::string::npos)
            extensions += "#extension GL_OES_standard_derivatives : enable\n";
        std::string out = version + extensions;
        if (fragment && src.find("precision ") == std::string::npos) out += "precision mediump float;\n";
        return out + body;
    }
    // Desktop GLSL 1.20: no precision statements or qualifiers; derivatives are built in (the
    // shaders' GL_OES_standard_derivatives line goes), other #extension lines come first.
    std::string extensions, body;
    size_t pos = 0;
    while (pos < src.size()) {
        size_t nl = src.find('\n', pos);
        if (nl == std::string::npos) nl = src.size();
        std::string line = src.substr(pos, nl - pos);
        size_t first = line.find_first_not_of(" \t");
        bool directive = first != std::string::npos;
        if (directive && line.compare(first, 10, "#extension") == 0) {
            if (line.find("GL_OES_standard_derivatives") == std::string::npos) extensions += line + '\n';
        } else if (!directive || (line.compare(first, 10, "precision ") != 0 && line.compare(first, 8, "#version") != 0)) {
            body += line + '\n';
        } else {
            body += '\n';
        }
        pos = nl + 1;
    }
    return "#version 120\n" + extensions + "#define lowp\n#define mediump\n#define highp\n" + body;
}

static GLuint compileStage(GLenum type, const std::string& src, const std::string& name) {
    GLuint s = CreateShader(type);
    const char* p = src.c_str();
    ShaderSource(s, 1, &p, nullptr);
    CompileShader(s);
    GLint ok = 0;
    GetShaderiv(s, COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        GetShaderInfoLog(s, sizeof log, nullptr, log);
        fprintf(stderr, "[render] %s shader '%s' failed:\n%s\n", type == VERTEX_SHADER ? "vertex" : "fragment",
                name.c_str(), log);
        // GMR_SHADER_SOURCE=1: also print the source as it was compiled, with line numbers.
        if (getenv("GMR_SHADER_SOURCE")) {
            int line = 1;
            fprintf(stderr, "%4d  ", line);
            for (const char* c = src.c_str(); *c; c++) {
                fputc(*c, stderr);
                if (*c == '\n' && c[1]) fprintf(stderr, "%4d  ", ++line);
            }
            fputc('\n', stderr);
        }
        DeleteShader(s);
        return 0;
    }
    return s;
}

bool Renderer::buildProgram(Program& p, const std::string& vs, const std::string& fs, const std::string& name) {
    GLuint v = compileStage(VERTEX_SHADER, adaptSource(vs, false), name);
    GLuint f = compileStage(FRAGMENT_SHADER, adaptSource(fs, true), name);
    if (!v || !f) {
        if (v) DeleteShader(v);
        if (f) DeleteShader(f);
        return false;
    }
    GLuint prog = CreateProgram();
    AttachShader(prog, v);
    AttachShader(prog, f);
    BindAttribLocation(prog, 0, "in_Position");
    BindAttribLocation(prog, 1, "in_Colour");
    BindAttribLocation(prog, 2, "in_TextureCoord");
    BindAttribLocation(prog, 3, "in_Normal");
    LinkProgram(prog);
    DeleteShader(v);
    DeleteShader(f);
    GLint ok = 0;
    GetProgramiv(prog, LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        GetProgramInfoLog(prog, sizeof log, nullptr, log);
        fprintf(stderr, "[render] link of '%s' failed:\n%s\n", name.c_str(), log);
        return false;
    }
    p.id = prog;
    p.valid = true;
    p.uMatrices = GetUniformLocation(prog, "gm_Matrices");
    if (p.uMatrices < 0) p.uMatrices = GetUniformLocation(prog, "gm_Matrices[0]");
    p.uBaseTexture = GetUniformLocation(prog, "gm_BaseTexture");
    p.uAlphaTest = GetUniformLocation(prog, "gm_AlphaTestEnabled");
    p.uAlphaRef = GetUniformLocation(prog, "gm_AlphaRefValue");
    p.uFogPS = GetUniformLocation(prog, "gm_PS_FogEnabled");
    p.uFogVS = GetUniformLocation(prog, "gm_VS_FogEnabled");
    p.uFogColour = GetUniformLocation(prog, "gm_FogColour");
    p.uFogStart = GetUniformLocation(prog, "gm_FogStart");
    p.uRcpFogRange = GetUniformLocation(prog, "gm_RcpFogRange");
    p.uLighting = GetUniformLocation(prog, "gm_LightingEnabled");
    p.uAmbient = GetUniformLocation(prog, "gm_AmbientColour");
    p.uLightDir = GetUniformLocation(prog, "gm_Lights_Direction");
    p.uLightPosRange = GetUniformLocation(prog, "gm_Lights_PosRange");
    p.uLightColour = GetUniformLocation(prog, "gm_Lights_Colour");
    return true;
}

int Renderer::compileShader(const std::string& name, const std::string& vs, const std::string& fs,
                            const std::vector<std::string>&) {
    Program p;
    if (enabled_) buildProgram(p, vs, fs, name);
    programs_.push_back(p);
    return static_cast<int>(programs_.size() - 1);
}

bool Renderer::shaderValid(int id) const {
    return id >= 0 && static_cast<size_t>(id) < programs_.size() && programs_[id].valid;
}

int Renderer::uniformLocation(int shader, const std::string& name) {
    if (!shaderValid(shader)) return -1;
    return GetUniformLocation(programs_[shader].id, name.c_str());
}

uint32_t Renderer::uniformType(int shader, int location) {
    if (!shaderValid(shader) || location < 0) return 0;
    Program& p = programs_[shader];
    auto it = p.uniformTypes.find(location);
    if (it != p.uniformTypes.end()) return it->second;
    uint32_t found = 0;
    GLint n = 0;
    GetProgramiv(p.id, 0x8B86 /* ACTIVE_UNIFORMS */, &n);
    for (GLint i = 0; i < n && !found; i++) {
        char name[256];
        GLsizei len = 0;
        GLint size = 0;
        GLenum type = 0;
        GetActiveUniform(p.id, static_cast<GLuint>(i), sizeof(name), &len, &size, &type, name);
        if (GetUniformLocation(p.id, name) == location) found = type;
    }
    p.uniformTypes[location] = found;
    return found;
}

int Renderer::uniformComponents(int shader, int location) {
    switch (uniformType(shader, location)) {
        case 0x1406: case 0x1404: case 0x8B56: case 0x8B5E: return 1;  // float, int, bool, sampler2D
        case 0x8B50: case 0x8B53: case 0x8B57: return 2;                // vec2, ivec2, bvec2
        case 0x8B51: case 0x8B54: case 0x8B58: return 3;
        case 0x8B52: case 0x8B55: case 0x8B59: return 4;
        case 0x8B5C: return 16;                                          // mat4
        default: return 0;
    }
}

void Renderer::setUniform(int shader, int location, int components, const float* values, int count) {
    if (!shaderValid(shader) || location < 0) return;
    flush();
    Program& p = programs_[shader];
    UseProgram(p.id);
    uint32_t type = uniformType(shader, location);
    bool isInt = type == 0x1404 || (type >= 0x8B53 && type <= 0x8B59) || type == 0x8B5E;  // int, ivec, bool, sampler
    if (isInt) {
        std::vector<GLint> iv(static_cast<size_t>(components) * count);
        for (size_t i = 0; i < iv.size(); i++) iv[i] = static_cast<GLint>(values[i]);
        switch (components) {
            case 1: Uniform1iv(location, count, iv.data()); break;
            case 2: Uniform2iv(location, count, iv.data()); break;
            case 3: Uniform3iv(location, count, iv.data()); break;
            case 4: Uniform4iv(location, count, iv.data()); break;
        }
        return;
    }
    switch (components) {
        case 1: Uniform1fv(location, count, values); break;
        case 2: Uniform2fv(location, count, values); break;
        case 3: Uniform3fv(location, count, values); break;
        case 4: Uniform4fv(location, count, values); break;
    }
}

void Renderer::setUniformMatrix(int shader, int location, const float* m) {
    if (!shaderValid(shader) || location < 0) return;
    flush();
    UseProgram(programs_[shader].id);
    UniformMatrix4fv(location, 1, FALSE_, m);
}

}  // namespace gm
