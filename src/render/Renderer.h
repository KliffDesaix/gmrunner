// Batched 2D renderer on OpenGL 2.1 / GLES 2.0.
//
// Render targets (surfaces, the application surface, the virtual backbuffer) store row 0 at
// the top, so every texture - sprite page or surface - is sampled with v=0 at the top.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gm {

struct Vertex {
    float x, y, z;
    uint32_t color;  // RGBA bytes in memory order (GameMaker BGR integer | alpha << 24)
    float u, v;
};

inline uint32_t packColor(uint32_t gmColor, double alpha) {
    int a = static_cast<int>(alpha * 255.0 + 0.5);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return (gmColor & 0xFFFFFF) | (static_cast<uint32_t>(a) << 24);
}

class Renderer {
public:
    enum Prim { TRIANGLES, LINES, POINTS };

    bool init(bool enabled, bool gles);
    bool enabled() const { return enabled_; }
    bool gles() const { return gles_; }

    uint32_t createTexture(int w, int h, const uint8_t* rgba);
    void deleteTexture(uint32_t tex);
    uint32_t whiteTexture() const { return white_; }

    int createTarget(int w, int h);
    void deleteTarget(int id);
    bool targetExists(int id) const;
    int targetWidth(int id) const;
    int targetHeight(int id) const;
    uint32_t targetTexture(int id) const;
    void bindTarget(int id, int screenW = 0, int screenH = 0);  // -1 = default framebuffer
    int boundTarget() const { return target_; }

    void setViewport(int x, int y, int w, int h);  // top-left origin, in target pixels
    // The 2D camera for a world rect: view and orthographic projection as YoYo's Build2DView.
    void setProjection(double x, double y, double w, double h, double angle = 0);

    // Matrices in GameMaker's layout (row vectors; the 16 numbers of a GameMaker matrix). On
    // render targets GameMaker's clip space is used as is; the real screen flips y.
    void setViewProjection(const float* view, const float* proj);
    void setWorld(const float* world);
    const float* worldMatrix() const { return world_; }
    const float* viewMatrix() const { return view_; }
    const float* projMatrix() const { return proj_; }

    // Depth and culling: func is cmpfunc_* (1 never .. 8 always), cull is cull_noculling /
    // cull_clockwise / cull_counterclockwise. Targets get a depth buffer when first needed.
    void setDepth(bool test, bool write, int func);
    void setCull(int mode);
    void clearDepth();
    // Stencil test with GameMaker's values: cmpfunc_* (1 never .. 8 always) and stencilop_*
    // (Direct3D's numbering: 1 keep, 2 zero, 3 replace, 4 incr_sat, 5 decr_sat, 6 invert, 7 incr, 8 decr).
    struct Stencil {
        bool enabled = false;
        int func = 8, ref = 0, readMask = 255, writeMask = 255, fail = 1, depthFail = 1, pass = 1;
        bool operator==(const Stencil& o) const {
            return enabled == o.enabled && func == o.func && ref == o.ref && readMask == o.readMask &&
                   writeMask == o.writeMask && fail == o.fail && depthFail == o.depthFail && pass == o.pass;
        }
    };
    void setStencil(const Stencil& s);
    const Stencil& stencil() const { return stencil_; }
    void clearStencil(int value);
    bool stencilAvailable() const { return packedDepthStencil_; }
    // Extra texture stages for shader samplers (texture_set_stage); stage 0 is the base texture.
    void setTextureStage(int stage, uint32_t tex);
    int samplerStage(int shader, const std::string& name);  // shader_get_sampler_index

    // Lighting for the built-in shader and gm_Lights_* uniforms (draw_set_lighting, draw_light_*).
    struct Lights {
        bool enabled = false;
        uint32_t ambient = 0;
        bool on[8] = {};
        bool point[8] = {};
        float dirOrPos[8][4] = {};  // direction (x, y, z) or position (x, y, z, range)
        uint32_t color[8] = {};
    };
    void setLights(const Lights& l);

    // Draws `count` vertices of `stride` bytes laid out as GameMaker vertex formats describe them.
    struct Attr {
        int usage;       // vertex_usage_*
        int index;       // nth element of that usage
        int components;  // 1-4
        uint32_t type;   // GL type
        bool normalized;
        int offset;
    };
    void drawRaw(uint32_t glMode, const uint8_t* data, int count, int stride, const std::vector<Attr>& attrs);

    void setTexture(uint32_t tex);
    void setFiltering(bool linear);
    // Scissor rectangle in target pixels (row 0 at the top); every viewport change resets it to
    // the viewport, as YoYo's runner. Clears ignore it, as Direct3D's do.
    void setScissor(int x, int y, int w, int h);
    void scissor(int& x, int& y, int& w, int& h) const {
        x = scissor_[0], y = scissor_[1], w = scissor_[2], h = scissor_[3];
    }
    void setWrap(bool repeat);  // texture repeat (gpu_set_tex_repeat) instead of clamping
    void setTextureParams(uint32_t tex, bool repeat, bool linear);  // a sampler stage texture's own wrap/filter
    void setBlend(uint32_t src, uint32_t dst, uint32_t srcA, uint32_t dstA, uint32_t equation, uint32_t equationA);
    void setBlend(uint32_t src, uint32_t dst, uint32_t srcA, uint32_t dstA, uint32_t equation) {
        setBlend(src, dst, srcA, dstA, equation, equation);
    }
    void setColorMask(bool r, bool g, bool b, bool a);
    void setAlphaTest(bool on, float ref);
    void setFog(bool on, uint32_t gmColor);
    void setShader(int shader);  // -1 = built-in
    int shader() const { return shader_; }

    void add(Prim prim, const Vertex* v, int n);
    void clear(uint32_t gmColor, double alpha);
    void flush();
    void readPixels(int x, int y, int w, int h, uint8_t* rgba);  // top-left origin

    // GameMaker shaders (GLSL ES source from the SHDR chunk).
    int compileShader(const std::string& name, const std::string& vs, const std::string& fs,
                      const std::vector<std::string>& attributes);
    bool shaderValid(int id) const;
    int uniformLocation(int shader, const std::string& name);
    int uniformComponents(int shader, int location);  // 1-4 for float/vec uniforms, 16 for mat4, 0 unknown
    uint32_t uniformType(int shader, int location);
    void setUniform(int shader, int location, int components, const float* values, int count);
    void setUniformMatrix(int shader, int location, const float* m);

    int drawCalls() const { return drawCalls_; }
    void resetStats() { drawCalls_ = 0; }

private:
    struct Program {
        uint32_t id = 0;
        bool valid = false;
        int uMatrices = -1, uBaseTexture = -1, uAlphaTest = -1, uAlphaRef = -1, uFogPS = -1, uFogVS = -1,
            uFogColour = -1, uFogStart = -1, uRcpFogRange = -1, uLighting = -1;
        uint64_t matrixVersion = ~0ull;
        uint64_t stateVersion = ~0ull;
        uint64_t lightVersion = ~0ull;
        int uAmbient = -1, uLightDir = -1, uLightPosRange = -1, uLightColour = -1;
        std::unordered_map<int, uint32_t> uniformTypes;  // location -> GL type (0 = not found)
        std::unordered_map<std::string, int> attribs;     // attribute name -> location (-1 = unused)
        int samplers[8] = {-1, -1, -1, -1, -1, -1, -1, -1};  // texture stage -> sampler uniform
    };
    struct Target {
        uint32_t fbo = 0, tex = 0, depth = 0;
        int w = 0, h = 0;
        bool live = false;
    };

    bool buildProgram(Program& p, const std::string& vs, const std::string& fs, const std::string& name);
    std::string adaptSource(const std::string& src, bool fragment) const;
    void useProgram(Program& p);

    bool enabled_ = false, gles_ = false;
    bool repeat_ = false, repeatAllowed_ = true;
    int scissor_[4] = {0, 0, 0, 0};
    Stencil stencil_;
    bool derivatives_ = false;  // GLES: GL_OES_standard_derivatives (dFdx, fwidth in game shaders)
    bool packedDepthStencil_ = false;  // depth buffers carry 8 stencil bits  // GLES 2 repeats non-power-of-two textures only with OES_texture_npot
    std::vector<Vertex> verts_;
    Prim prim_ = TRIANGLES;
    uint32_t vbo_ = 0, white_ = 0;
    uint32_t tex_ = 0;
    bool linear_ = false;
    uint32_t blend_[6] = {0, 0, 0, 0, 0, 0};
    bool mask_[4] = {true, true, true, true};
    bool alphaTest_ = false;
    float alphaRef_ = 0;
    bool fog_ = false;
    uint32_t fogColor_ = 0;
    uint64_t stateVersion_ = 1;
    float world_[16], view_[16], proj_[16];
    uint64_t matrixVersion_ = 1;
    bool depthTest_ = false, depthWrite_ = false;
    int depthFunc_ = 4, cull_ = 0;
    uint32_t stages_[8] = {};
    Lights lights_;
    uint64_t lightVersion_ = 1;
    void ensureDepthBuffer();
    void applyDepthState();
    int attribLocation(Program& p, const std::string& name);
    void bindStages(Program& p);
    int shader_ = -1;
    Program builtin_;
    std::vector<Program> programs_;
    std::vector<Target> targets_;
    // The window's framebuffer: 0 on desktop GL, an FBO SDL creates on iOS (which has no framebuffer 0).
    uint32_t screenFbo_ = 0;
    int target_ = -1;
    int targetH_ = 0;
    bool targetFlipped_ = false;
    int drawCalls_ = 0;
};

}  // namespace gm
