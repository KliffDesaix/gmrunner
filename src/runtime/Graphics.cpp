#include "Graphics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "Runner.h"
#include "Sequences.h"
#include "Stores.h"
#include "render/GL.h"
#include "util/BuiltinFonts.h"
#include "runtime/SkeletonSprites.h"
#include "util/Image.h"
#include "util/Utf8.h"

#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#include "stb_image.h"
#include "util/Stb.h"

namespace gm::stb {
uint8_t* loadImage(const uint8_t* data, int size, int* w, int* h, int* comp, int reqComp) {
    return stbi_load_from_memory(data, size, w, h, comp, reqComp);
}
const char* failureReason() { return stbi_failure_reason(); }
void freeImage(void* pixels) { stbi_image_free(pixels); }
char* zlibDecode(const char* data, int len, int* outLen) { return stbi_zlib_decode_malloc(data, len, outLen); }
char* zlibDecodeRaw(const char* data, int len, int* outLen) { return stbi_zlib_decode_noheader_malloc(data, len, outLen); }
}  // namespace gm::stb

namespace gm {

Graphics::Graphics(Runner& rt) : rt_(rt) {}
Graphics::~Graphics() = default;

// How much texture memory pages may keep before idle ones are dropped: GMR_TEXTURE_BUDGET_MB, or
// a quarter of the memory on devices with 2 GB or less (handhelds, where the GPU shares it).
static size_t textureBudget() {
    if (const char* e = getenv("GMR_TEXTURE_BUDGET_MB")) return static_cast<size_t>(std::max(0L, atol(e))) << 20;
    uint64_t total = 0;
#ifdef __SWITCH__
    u64 v = 0;
    if (R_SUCCEEDED(svcGetInfo(&v, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) total = v;
#elif defined(_SC_PHYS_PAGES)
    long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && size > 0) total = static_cast<uint64_t>(pages) * static_cast<uint64_t>(size);
#endif
    return total && total <= (2ull << 30) ? static_cast<size_t>(total / 4) : 0;
}

void Graphics::uploadPage(int index, std::vector<uint8_t>& px, int w, int h, const std::string& err) {
    Page& p = pages_[index];
    if (px.empty()) {
        rt_.log("[gfx] cannot decode texture page %d: %s", index, err.c_str());
        p.failed = true;
        return;
    }
    p.w = w;
    p.h = h;
    if (!enabled_) return;
    p.tex = r_.createTexture(w, h, px.data());
    pageBytes_ += static_cast<size_t>(w) * h * 4;
}

void Graphics::loadPage(int index) {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    std::string err;
    if (!decodeTexture(rt_.dw.textures[index], px, w, h, err, rt_.gameDir)) px.clear();
    uploadPage(index, px, w, h, err);
}

const Graphics::Page& Graphics::page(int index) {
    Page& p = pages_[index];
    p.lastUsed = frame_;
    if (enabled_ && !p.tex && !p.failed && static_cast<size_t>(index) < rt_.dw.textures.size()) {
        if (rt_.debugTrace) rt_.log("[gfx] loading texture page %d to draw", index);
        loadPage(index);
    }
    return p;
}

void Graphics::dropPagesOver(size_t limit, uint64_t keepFrom) {
    r_.flush();  // nothing queued may still draw from a dropped page
    while (pageBytes_ > limit) {
        Page* oldest = nullptr;
        for (size_t i = 0; i < rt_.dw.textures.size(); i++) {
            Page& p = pages_[i];
            if (p.tex && p.lastUsed < keepFrom && (!oldest || p.lastUsed < oldest->lastUsed)) oldest = &p;
        }
        if (!oldest) break;
        dropPage(static_cast<int>(oldest - pages_.data()));
    }
}

void Graphics::dropPage(int index) {
    Page& p = pages_[index];
    if (!p.tex || static_cast<size_t>(index) >= rt_.dw.textures.size()) return;  // run-time pages stay
    if (rt_.debugTrace) rt_.log("[gfx] dropped texture page %d", index);
    r_.deleteTexture(p.tex);
    p.tex = 0;
    pageBytes_ -= static_cast<size_t>(p.w) * p.h * 4;
}

void Graphics::dropIdlePages() {
    frame_++;
    // Pages drawn in the frame just shown stay.
    if (pageBudget_ && pageBytes_ > pageBudget_) dropPagesOver(pageBudget_, frame_ - 1);
}

void Graphics::prefetchPages(std::vector<int> want) {
    if (!enabled_) return;
    std::sort(want.begin(), want.end());
    want.erase(std::unique(want.begin(), want.end()), want.end());
    std::vector<int> load;
    size_t bytes = 0;
    for (int i : want) {
        if (i < 0 || static_cast<size_t>(i) >= rt_.dw.textures.size()) continue;
        Page& p = pages_[i];
        p.lastUsed = frame_;
        if (p.tex || p.failed) continue;
        size_t b = static_cast<size_t>(p.w) * p.h * 4;
        if (pageBudget_ && bytes + b > pageBudget_) continue;  // the rest load when drawn
        bytes += b;
        load.push_back(i);
    }
    if (load.empty()) return;
    if (rt_.debugTrace) rt_.log("[gfx] prefetching %zu texture pages (%zu MB)", load.size(), bytes >> 20);
    // Room for them: pages not wanted now go first, longest unused first.
    if (pageBudget_ && pageBytes_ + bytes > pageBudget_) dropPagesOver(pageBudget_ - bytes, frame_);
    // Decoded on worker threads a few at a time (decoding is most of the cost on slow CPUs, and
    // small batches keep few decoded pages in memory at once), uploaded here.
#ifdef __SWITCH__
    size_t workers = 3;  // the application's cores (0-2)
#else
    size_t workers = std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
#endif
    struct Decoded {
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        std::string err;
    };
    for (size_t first = 0; first < load.size(); first += workers) {
        size_t n = std::min(workers, load.size() - first);
        std::vector<Decoded> batch(n);
        auto decode = [&](size_t k) {
            Decoded& d = batch[k];
            if (!decodeTexture(rt_.dw.textures[load[first + k]], d.px, d.w, d.h, d.err, rt_.gameDir)) d.px.clear();
        };
#ifdef __SWITCH__
        // libnx's pthreads all start on the caller's core; pin the workers to the other cores.
        struct Job {
            decltype(decode)* fn;
            size_t k;
        };
        std::vector<Job> jobs(n);
        std::vector<Thread> threads(n);
        std::vector<bool> started(n, false);
        for (size_t k = 1; k < n; k++) {
            jobs[k] = {&decode, k};
            int core = static_cast<int>(k % 3);
            if (core == static_cast<int>(svcGetCurrentProcessorNumber())) core = (core + 1) % 3;
            started[k] = R_SUCCEEDED(threadCreate(&threads[k], [](void* a) {
                Job* j = static_cast<Job*>(a);
                (*j->fn)(j->k);
            }, &jobs[k], nullptr, 0x40000, 0x2C, core)) && R_SUCCEEDED(threadStart(&threads[k]));
            if (!started[k]) decode(k);
        }
        decode(0);
        for (size_t k = 1; k < n; k++)
            if (started[k]) threadWaitForExit(&threads[k]), threadClose(&threads[k]);
#else
        std::vector<std::thread> threads;
        for (size_t k = 1; k < n; k++) threads.emplace_back(decode, k);
        decode(0);
        for (auto& t : threads) t.join();
#endif
        for (size_t k = 0; k < n; k++) uploadPage(load[first + k], batch[k].px, batch[k].w, batch[k].h, batch[k].err);
    }
}

void Graphics::flushPages(const std::vector<int>& pages) {
    if (!enabled_) return;
    r_.flush();
    for (int i : pages)
        if (i >= 0 && static_cast<size_t>(i) < pages_.size()) dropPage(i);
}

void Graphics::flushAllPages() {
    if (!enabled_) return;
    r_.flush();
    for (size_t i = 0; i < pages_.size(); i++) dropPage(static_cast<int>(i));
}

bool Graphics::pageLoaded(int index) const {
    return index >= 0 && static_cast<size_t>(index) < pages_.size() && (pages_[index].tex || !enabled_);
}

void Graphics::init(bool enabled) {
    enabled_ = enabled;
    r_.init(enabled, rt_.glES);
    const DataWin& dw = rt_.dw;
    pages_.resize(dw.textures.size());
    // Only the pages' sizes now (sprite UVs need them); their pixels when first drawn.
    for (size_t i = 0; i < dw.textures.size(); i++)
        if (!textureSize(dw.textures[i], pages_[i].w, pages_[i].h, rt_.gameDir)) loadPage(static_cast<int>(i));
    pageBudget_ = textureBudget();
    if (pageBudget_) rt_.log("[gfx] texture budget %zu MB", pageBudget_ >> 20);
    int ok = 0;
    for (auto& s : dw.shaders) {
        std::vector<std::string> attrs;
        for (auto a : s.attributes) attrs.emplace_back(a);
        int id = r_.compileShader(std::string(s.name), std::string(s.glslesVertex), std::string(s.glslesFragment), attrs);
        ok += r_.shaderValid(id);
    }
    if (enabled && !dw.shaders.empty()) rt_.log("[gfx] compiled %d/%zu shaders", ok, dw.shaders.size());
    loadFonts();
    winW_ = dw.gen.windowWidth ? static_cast<int>(dw.gen.windowWidth) : 640;
    winH_ = dw.gen.windowHeight ? static_cast<int>(dw.gen.windowHeight) : 480;
    setBlendMode(0);
}

// ---------------------------------------------------------------------------------------------
// Target / projection state

Graphics::TargetState Graphics::currentState() const {
    return {curTarget_, vpX_, vpY_, vpW_, vpH_, projX_, projY_, projW_, projH_, projAngle_, customMats_, customView_,
            customProj_};
}

void Graphics::restoreState(const TargetState& s) {
    curTarget_ = s.target;
    r_.bindTarget(s.target);
    curTargetW_ = r_.targetWidth(s.target);
    curTargetH_ = r_.targetHeight(s.target);
    vpX_ = s.vpX;
    vpY_ = s.vpY;
    vpW_ = s.vpW;
    vpH_ = s.vpH;
    r_.setViewport(vpX_, vpY_, vpW_, vpH_);
    projX_ = s.px;
    projY_ = s.py;
    projW_ = s.pw;
    projH_ = s.ph;
    projAngle_ = s.pa;
    customMats_ = s.custom;
    customView_ = s.view;
    customProj_ = s.proj;
    if (s.custom) r_.setViewProjection(s.view.data(), s.proj.data());
    else r_.setProjection(projX_, projY_, projW_, projH_, projAngle_);
}

void Graphics::setViewProjection(const float* view, const float* proj) {
    customMats_ = true;
    std::copy(view, view + 16, customView_.begin());
    std::copy(proj, proj + 16, customProj_.begin());
    r_.setViewProjection(view, proj);
}

void Graphics::setZTest(bool on) {
    zTest_ = on;
    r_.setDepth(zTest_, zWrite_, zFunc_);
}
void Graphics::setZWrite(bool on) {
    zWrite_ = on;
    r_.setDepth(zTest_, zWrite_, zFunc_);
}
void Graphics::setZFunc(int func) {
    zFunc_ = func;
    r_.setDepth(zTest_, zWrite_, zFunc_);
}
void Graphics::setCullMode(int mode) {
    cull_ = mode;
    r_.setCull(mode);
}

void Graphics::bindAndSetup(int target, int w, int h) {
    restoreState({target, 0, 0, w, h, 0, 0, static_cast<double>(w), static_cast<double>(h), 0});
}

void Graphics::ensureBackbuffer() {
    if (!r_.targetExists(backbuffer_) || r_.targetWidth(backbuffer_) != winW_ || r_.targetHeight(backbuffer_) != winH_) {
        if (backbuffer_ >= 0) r_.deleteTarget(backbuffer_);
        backbuffer_ = r_.createTarget(winW_, winH_);
    }
}

void Graphics::ensureAppSurface() {
    if (appW_ <= 0 || appH_ <= 0) {
        appW_ = winW_;
        appH_ = winH_;
    }
    if (appSurface_ < 0) {
        appSurface_ = surfaceCreate(appW_, appH_);
    } else if (surfaceWidth(appSurface_) != appW_ || surfaceHeight(appSurface_) != appH_) {
        surfaceResize(appSurface_, appW_, appH_);
    }
}

void Graphics::setWindowSize(int w, int h) {
    if (w < 1 || h < 1) return;
    winW_ = w;
    winH_ = h;
    windowSetByGame_ = true;
}

void Graphics::onRoomStart() {
    // GMS2: the window and the application surface keep the size the game started with (the
    // first room's); a room's view ports are scaled to fill it (portScale). GMS 1.4 sizes them
    // to every room.
    if (rt_.dw.gen.gms2 && appSized_) return;
    int w = 0, h = 0;
    if (rt_.viewsEnabled) {
        for (auto& v : rt_.views)
            if (v.visible) {
                w = std::max(w, static_cast<int>(v.xport + v.wport));
                h = std::max(h, static_cast<int>(v.yport + v.hport));
            }
    }
    if (w <= 0 || h <= 0) {
        w = static_cast<int>(rt_.roomWidth);
        h = static_cast<int>(rt_.roomHeight);
    }
    appW_ = w;
    appH_ = h;
    appSized_ = true;
    if (!windowSetByGame_) {
        winW_ = w;
        winH_ = h;
    }
    // The application surface itself is made by the first draw (ensureAppSurface in drawFrame),
    // as YoYo's runners do, so the game's own early surfaces get the low ids: games keep
    // surface variables at 0 and checks surface_exists(0).
}

// YoYo's g_DisplayScaleX/Y: the bounding box of the visible view ports is stretched over the
// application surface (GMS2), so ports drawn for another size still fill it.
void Graphics::portScale(double& sx, double& sy) const {
    sx = sy = 1;
    if (!rt_.dw.gen.gms2 || !rt_.viewsEnabled) return;
    double l = 1e18, r = -1e18, t = 1e18, b = -1e18;
    for (const View& v : rt_.views) {
        if (!v.visible) continue;
        l = std::min(l, v.xport);
        r = std::max(r, v.xport + v.wport);
        t = std::min(t, v.yport);
        b = std::max(b, v.yport + v.hport);
    }
    if (r > l && b > t) {
        sx = appW_ / (r - l);
        sy = appH_ / (b - t);
    }
}

// ---------------------------------------------------------------------------------------------
// Blending and state

uint32_t glFactor(int bm) {
    switch (bm) {
        case 1: return gl::ZERO;
        case 2: return gl::ONE;
        case 3: return gl::SRC_COLOR;
        case 4: return gl::ONE_MINUS_SRC_COLOR;
        case 5: return gl::SRC_ALPHA;
        case 6: return gl::ONE_MINUS_SRC_ALPHA;
        case 7: return gl::DST_ALPHA;
        case 8: return gl::ONE_MINUS_DST_ALPHA;
        case 9: return gl::DST_COLOR;
        case 10: return gl::ONE_MINUS_DST_COLOR;
        case 11: return gl::SRC_ALPHA_SATURATE;
    }
    return gl::ONE;
}

void Graphics::setBlendMode(int mode) {
    blendMode_ = mode;
    switch (mode) {
        case 1: blendSrc_ = 5; blendDst_ = 2; break;   // bm_add
        case 2: blendSrc_ = 5; blendDst_ = 4; break;   // bm_max
        case 3: blendSrc_ = 1; blendDst_ = 4; break;   // bm_subtract
        default: blendSrc_ = 5; blendDst_ = 6; break;  // bm_normal
    }
    blendSrcA_ = blendSrc_;
    blendDstA_ = blendDst_;
    setAlphaBlend(alphaBlend_);
}

void Graphics::setBlendModeExt(int src, int dst) { setBlendModeExtSepAlpha(src, dst, src, dst); }

void Graphics::setBlendModeExtSepAlpha(int src, int dst, int srcAlpha, int dstAlpha) {
    blendSrc_ = src;
    blendDst_ = dst;
    blendSrcA_ = srcAlpha;
    blendDstA_ = dstAlpha;
    setAlphaBlend(alphaBlend_);
}

void Graphics::setBlendEquation(int eq, int eqAlpha) {
    auto valid = [](int e) {  // add, min, max, subtract, reverse subtract
        return e == 0x8006 || e == 0x8007 || e == 0x8008 || e == 0x800A || e == 0x800B ? e : 0x8006;
    };
    blendEq_ = valid(eq);
    blendEqA_ = valid(eqAlpha);
    setAlphaBlend(alphaBlend_);
}

void Graphics::setAlphaBlend(bool on) {
    alphaBlend_ = on;
    if (on) {
        r_.setBlend(glFactor(blendSrc_), glFactor(blendDst_), glFactor(blendSrcA_), glFactor(blendDstA_),
                    static_cast<uint32_t>(blendEq_), static_cast<uint32_t>(blendEqA_));
    } else {
        r_.setBlend(gl::ONE, gl::ZERO, gl::ONE, gl::ZERO, gl::FUNC_ADD);
    }
}

void Graphics::setColorWrite(bool r, bool g, bool b, bool a) {
    colorMask_[0] = r;
    colorMask_[1] = g;
    colorMask_[2] = b;
    colorMask_[3] = a;
    r_.setColorMask(r, g, b, a);
}
void Graphics::setTexFilter(bool linear) {
    texFilter_ = linear;
    r_.setFiltering(linear);
}
void Graphics::setTexRepeat(bool on) {
    texRepeat = on;
    r_.setWrap(on);
}
void Graphics::shaderUniformWorldMatrix(int location) {
    if (shader_ >= 0) r_.setUniformMatrix(shader_, location, r_.worldMatrix());
}
Graphics::GpuState Graphics::gpuState() const {
    return {blendSrc_, blendDst_, blendSrcA_, blendDstA_, blendMode_, alphaBlend_, alphaTest_, alphaRef_,
            {colorMask_[0], colorMask_[1], colorMask_[2], colorMask_[3]}, fogOn_, fogColor_, texFilter_,
            zTest_, zWrite_, zFunc_, cull_, blendEq_, blendEqA_};
}
void Graphics::setGpuState(const GpuState& st) {
    blendSrc_ = st.blendSrc;
    blendDst_ = st.blendDst;
    blendSrcA_ = st.blendSrcA;
    blendDstA_ = st.blendDstA;
    blendMode_ = st.blendMode;
    blendEq_ = st.blendEq;
    blendEqA_ = st.blendEqA;
    setAlphaBlend(st.alphaBlend);
    alphaRef_ = st.alphaRef;
    setAlphaTest(st.alphaTest);
    setColorWrite(st.colorMask[0], st.colorMask[1], st.colorMask[2], st.colorMask[3]);
    setFog(st.fog, st.fogColor);
    setTexFilter(st.filter);
    zTest_ = st.zTest;
    zWrite_ = st.zWrite;
    zFunc_ = st.zFunc;
    r_.setDepth(zTest_, zWrite_, zFunc_);
    setCullMode(st.cull);
}
void Graphics::setAlphaTest(bool on) {
    alphaTest_ = on;
    r_.setAlphaTest(alphaTest_, static_cast<float>(alphaRef_ / 255.0));
}
void Graphics::setAlphaTestRef(double ref) {
    alphaRef_ = ref;
    r_.setAlphaTest(alphaTest_, static_cast<float>(alphaRef_ / 255.0));
}
void Graphics::setFog(bool on, uint32_t c) {
    fogOn_ = on;
    fogColor_ = c;
    r_.setFog(on, c);
}

bool Graphics::shaderSet(int shader) {
    if (!r_.shaderValid(shader)) return false;
    shader_ = shader;
    r_.setShader(shader);
    return true;
}

void Graphics::shaderReset() {
    shader_ = -1;
    r_.setShader(-1);
}

// Without graphics (--headless, --smoke) every shader the game has counts as compiled, so game
// logic runs as it would with a GPU (text libraries stop the game when theirs don't compile).
bool Graphics::shaderCompiled(int shader) const {
    if (!enabled_) return shader >= 0 && static_cast<size_t>(shader) < rt_.dw.shaders.size();
    return r_.shaderValid(shader);
}

int Graphics::shaderUniform(int shader, const std::string& name) { return r_.uniformLocation(shader, name); }

void Graphics::shaderUniformF(int location, const float* v, int n) {
    if (shader_ < 0) return;
    r_.setUniform(shader_, location, n, v, 1);
}

void Graphics::shaderUniformArray(int location, const float* v, int n) {
    if (shader_ < 0 || n <= 0) return;
    int comps = r_.uniformComponents(shader_, location);
    if (comps == 16) {
        for (int i = 0; i + 16 <= n; i += 16) r_.setUniformMatrix(shader_, location + i / 16, v + i);
    } else if (comps > 0) {
        r_.setUniform(shader_, location, comps, v, n / comps);
    } else {
        r_.setUniform(shader_, location, 1, v, n);
    }
}

// ---------------------------------------------------------------------------------------------
// Textured quads

void Graphics::drawQuad(uint32_t tex, const double (&px)[4], const double (&py)[4], double u0, double v0, double u1,
                        double v1, const uint32_t (&c)[4]) {
    r_.setTexture(tex);
    Vertex v[6];
    auto set = [&](Vertex& o, int i, double u, double vv) {
        o.x = static_cast<float>(px[i]);
        o.y = static_cast<float>(py[i]);
        o.z = static_cast<float>(drawZ);
        o.color = c[i];
        o.u = static_cast<float>(u);
        o.v = static_cast<float>(vv);
    };
    set(v[0], 0, u0, v0);
    set(v[1], 1, u1, v0);
    set(v[2], 2, u1, v1);
    v[3] = v[0];
    v[4] = v[2];
    set(v[5], 3, u0, v1);
    r_.add(Renderer::TRIANGLES, v, 6);
}

void Graphics::drawTexRect(uint32_t tex, int texW, int texH, double sx, double sy, double sw, double sh, double lx,
                           double ly, double lw, double lh, double x, double y, double xs, double ys, double rot,
                           uint32_t col, double a) {
    if (texW <= 0 || texH <= 0) return;
    double lxs[4] = {lx, lx + lw, lx + lw, lx};
    double lys[4] = {ly, ly, ly + lh, ly + lh};
    double px[4], py[4];
    double ca = 1, sa = 0;
    if (rot != 0) {
        double r = rot * M_PI / 180.0;
        ca = std::cos(r);
        sa = std::sin(r);
    }
    for (int i = 0; i < 4; i++) {
        double qx = lxs[i] * xs, qy = lys[i] * ys;
        px[i] = x + qx * ca + qy * sa;
        py[i] = y - qx * sa + qy * ca;
    }
    uint32_t c = packColor(col, a);
    const uint32_t cs[4] = {c, c, c, c};
    drawQuad(tex, px, py, sx / texW, sy / texH, (sx + sw) / texW, (sy + sh) / texH, cs);
}

void Graphics::drawTexRect4(uint32_t tex, int texW, int texH, double sx, double sy, double sw, double sh, double lx,
                            double ly, double lw, double lh, double x, double y, double xs, double ys, double rot,
                            const uint32_t (&packed)[4]) {
    if (texW <= 0 || texH <= 0) return;
    double lxs[4] = {lx, lx + lw, lx + lw, lx};
    double lys[4] = {ly, ly, ly + lh, ly + lh};
    double px[4], py[4];
    double r = rot * M_PI / 180.0, ca = std::cos(r), sa = std::sin(r);
    for (int i = 0; i < 4; i++) {
        double qx = lxs[i] * xs, qy = lys[i] * ys;
        px[i] = x + qx * ca + qy * sa;
        py[i] = y - qx * sa + qy * ca;
    }
    drawQuad(tex, px, py, sx / texW, sy / texH, (sx + sw) / texW, (sy + sh) / texH, packed);
}

void Graphics::drawTpag(int tpag, double x, double y, double ox, double oy, double xs, double ys, double rot,
                        uint32_t col, double a) {
    if (tpag < 0 || static_cast<size_t>(tpag) >= rt_.dw.tpag.size()) return;
    const TexturePageItem& t = rt_.dw.tpag[tpag];
    if (t.texture < 0 || static_cast<size_t>(t.texture) >= pages_.size()) return;
    const Page& p = page(t.texture);
    drawTexRect(p.tex, p.w, p.h, t.srcX, t.srcY, t.srcW, t.srcH, t.tgtX - ox, t.tgtY - oy, t.tgtW, t.tgtH, x, y, xs,
                ys, rot, col, a);
}

void Graphics::drawTpagPart(int tpag, double left, double top, double w, double h, double x, double y, double xs,
                            double ys, double rot, uint32_t col, double a, double ox, double oy) {
    if (tpag < 0 || static_cast<size_t>(tpag) >= rt_.dw.tpag.size()) return;
    const TexturePageItem& t = rt_.dw.tpag[tpag];
    if (t.texture < 0 || static_cast<size_t>(t.texture) >= pages_.size()) return;
    const Page& p = page(t.texture);
    double ix0 = std::max(left, static_cast<double>(t.tgtX));
    double iy0 = std::max(top, static_cast<double>(t.tgtY));
    double ix1 = std::min(left + w, static_cast<double>(t.tgtX + t.tgtW));
    double iy1 = std::min(top + h, static_cast<double>(t.tgtY + t.tgtH));
    if (ix1 <= ix0 || iy1 <= iy0) return;
    double kx = t.tgtW ? static_cast<double>(t.srcW) / t.tgtW : 1, ky = t.tgtH ? static_cast<double>(t.srcH) / t.tgtH : 1;
    drawTexRect(p.tex, p.w, p.h, t.srcX + (ix0 - t.tgtX) * kx, t.srcY + (iy0 - t.tgtY) * ky, (ix1 - ix0) * kx,
                (iy1 - iy0) * ky, ix0 - left - ox, iy0 - top - oy, ix1 - ix0, iy1 - iy0, x, y, xs, ys, rot, col, a);
}

int Graphics::spriteFrame(int sprite, double subimg) const {
    if (sprite < 0 || static_cast<size_t>(sprite) >= rt_.dw.sprites.size()) return -1;
    const Sprite& s = rt_.dw.sprites[sprite];
    int n = static_cast<int>(s.frames.size());
    if (n == 0) return -1;
    int f = static_cast<int>(std::floor(subimg)) % n;
    if (f < 0) f += n;
    return s.frames[f];
}

void Graphics::drawSprite(int sprite, double subimg, double x, double y, double xs, double ys, double rot,
                          uint32_t col, double a) {
    int tp = spriteFrame(sprite, subimg);
    if (tp < 0) return;
    const Sprite& s = rt_.dw.sprites[sprite];
    if (s.nineSlice.enabled) {
        drawNineSlice(sprite, tp, x, y, s.width * xs, s.height * ys, rot, col, a, false);
        return;
    }
    drawTpag(tp, x, y, s.originX, s.originY, xs, ys, rot, col, a);
}

void Graphics::drawNineSlice(int sprite, int tpag, double x, double y, double w, double h, double rot, uint32_t col,
                             double a, bool zeroOrigin) {
    const Sprite& s = rt_.dw.sprites[sprite];
    const NineSlice& n = s.nineSlice;
    if (tpag < 0 || static_cast<size_t>(tpag) >= rt_.dw.tpag.size() || !s.width || !s.height) return;
    const TexturePageItem& t = rt_.dw.tpag[tpag];
    if (t.texture < 0 || static_cast<size_t>(t.texture) >= pages_.size()) return;
    const Page& p = page(t.texture);
    double fx = w < 0 ? -1 : 1, fy = h < 0 ? -1 : 1;
    w = std::fabs(w);
    h = std::fabs(h);

    // Slice edges in the source (U, V) and on screen (X, Y). Borders keep their size; when the
    // target is smaller than the borders they are cut, as the IDE does.
    double left = n.left, right = n.right, top = n.top, bottom = n.bottom;
    double midW = s.width - n.left - n.right, midH = s.height - n.top - n.bottom;
    double xsMid = midW > 0 ? (w - left - right) / midW : 0, ysMid = midH > 0 ? (h - top - bottom) / midH : 0;
    if (w < left) left = w, right = 0, xsMid = 0;
    else if (w < left + right) right = w - left, xsMid = 0;
    if (h < top) top = h, bottom = 0, ysMid = 0;
    else if (h < top + bottom) bottom = h - top, ysMid = 0;
    const double U[4] = {0, left, left + midW, left + midW + right};
    const double V[4] = {0, top, top + midH, top + midH + bottom};
    const double X[4] = {0, left, left + midW * xsMid, left + midW * xsMid + right};
    const double Y[4] = {0, top, top + midH * ysMid, top + midH * ysMid + bottom};
    double ox = zeroOrigin ? 0 : s.originX * (w / s.width), oy = zeroOrigin ? 0 : s.originY * (h / s.height);

    // One piece: source rect -> screen rect (optionally mirrored), cut to the frame's stored
    // (trimmed) area.
    double kx = t.tgtW ? static_cast<double>(t.srcW) / t.tgtW : 1, ky = t.tgtH ? static_cast<double>(t.srcH) / t.tgtH : 1;
    auto piece = [&](double u0, double v0, double u1, double v1, double x0, double y0, double x1, double y1, bool mx,
                     bool my) {
        double cu0 = std::max(u0, static_cast<double>(t.tgtX)), cu1 = std::min(u1, static_cast<double>(t.tgtX + t.tgtW));
        double cv0 = std::max(v0, static_cast<double>(t.tgtY)), cv1 = std::min(v1, static_cast<double>(t.tgtY + t.tgtH));
        if (cu1 <= cu0 || cv1 <= cv0) return;
        double sx = (x1 - x0) / (u1 - u0), sy = (y1 - y0) / (v1 - v0);
        double dx0 = mx ? x1 - (cu1 - u0) * sx : x0 + (cu0 - u0) * sx;
        double dy0 = my ? y1 - (cv1 - v0) * sy : y0 + (cv0 - v0) * sy;
        double tu = t.srcX + ((mx ? cu1 : cu0) - t.tgtX) * kx, tv = t.srcY + ((my ? cv1 : cv0) - t.tgtY) * ky;
        double tw = (cu1 - cu0) * kx * (mx ? -1 : 1), th = (cv1 - cv0) * ky * (my ? -1 : 1);
        drawTexRect(p.tex, p.w, p.h, tu, tv, tw, th, dx0 - ox, dy0 - oy, (cu1 - cu0) * sx, (cv1 - cv0) * sy, x, y, fx,
                    fy, rot, col, a);
    };
    // Corners stretch (they keep their size); edges and the centre use their slice's mode.
    static const int kMode[9] = {-1, 1, -1, 0, 4, 2, -1, 3, -1};
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            int mode = kMode[j * 3 + i] < 0 ? 0 : n.modes[kMode[j * 3 + i]];
            double u0 = U[i], u1 = U[i + 1], v0 = V[j], v1 = V[j + 1];
            double x0 = X[i], x1 = X[i + 1], y0 = Y[j], y1 = Y[j + 1];
            if (mode == 4 || u1 <= u0 || v1 <= v0 || x1 <= x0 || y1 <= y0) continue;
            if (mode == 0) {
                piece(u0, v0, u1, v1, x0, y0, x1, y1, false, false);
                continue;
            }
            // Repeat / mirror: native-size copies, the last one cut; blank repeat: one copy.
            double cw = u1 - u0, ch = v1 - v0;
            int nx = mode == 3 ? 1 : static_cast<int>(std::ceil((x1 - x0) / cw));
            int ny = mode == 3 ? 1 : static_cast<int>(std::ceil((y1 - y0) / ch));
            for (int ty = 0; ty < ny; ty++) {
                double py0 = y0 + ty * ch, py1 = std::min(py0 + ch, y1);
                bool my = mode == 2 && (ty & 1);
                double pv0 = my ? v1 - (py1 - py0) : v0, pv1 = my ? v1 : v0 + (py1 - py0);
                for (int tx = 0; tx < nx; tx++) {
                    double px0 = x0 + tx * cw, px1 = std::min(px0 + cw, x1);
                    bool mx = mode == 2 && (tx & 1);
                    double pu0 = mx ? u1 - (px1 - px0) : u0, pu1 = mx ? u1 : u0 + (px1 - px0);
                    piece(pu0, pv0, pu1, pv1, px0, py0, px1, py1, mx, my);
                }
            }
        }
}

void Graphics::drawSpritePart(int sprite, double subimg, double left, double top, double w, double h, double x,
                              double y, double xs, double ys, uint32_t col, double a) {
    int tp = spriteFrame(sprite, subimg);
    if (tp < 0) return;
    drawTpagPart(tp, left, top, w, h, x, y, xs, ys, 0, col, a);
}

// draw_sprite_general: part of a frame, scaled and rotated about (x, y), one colour per corner
// (top-left, top-right, bottom-right, bottom-left).
void Graphics::drawSpriteGeneral(int sprite, double subimg, double left, double top, double w, double h, double x,
                                 double y, double xs, double ys, double rot, const uint32_t (&cols)[4], double a) {
    int tp = spriteFrame(sprite, subimg);
    if (tp < 0 || static_cast<size_t>(tp) >= rt_.dw.tpag.size()) return;
    const TexturePageItem& t = rt_.dw.tpag[tp];
    if (t.texture < 0 || static_cast<size_t>(t.texture) >= pages_.size()) return;
    const Page& p = page(t.texture);
    double ix0 = std::max(left, static_cast<double>(t.tgtX));
    double iy0 = std::max(top, static_cast<double>(t.tgtY));
    double ix1 = std::min(left + w, static_cast<double>(t.tgtX + t.tgtW));
    double iy1 = std::min(top + h, static_cast<double>(t.tgtY + t.tgtH));
    if (ix1 <= ix0 || iy1 <= iy0) return;
    double kx = t.tgtW ? static_cast<double>(t.srcW) / t.tgtW : 1, ky = t.tgtH ? static_cast<double>(t.srcH) / t.tgtH : 1;
    const uint32_t packed[4] = {packColor(cols[0], a), packColor(cols[1], a), packColor(cols[2], a), packColor(cols[3], a)};
    drawTexRect4(p.tex, p.w, p.h, t.srcX + (ix0 - t.tgtX) * kx, t.srcY + (iy0 - t.tgtY) * ky, (ix1 - ix0) * kx,
                 (iy1 - iy0) * ky, ix0 - left, iy0 - top, ix1 - ix0, iy1 - iy0, x, y, xs, ys, rot, packed);
}

void Graphics::drawSpriteStretched(int sprite, double subimg, double x, double y, double w, double h, uint32_t col,
                                   double a) {
    int tp = spriteFrame(sprite, subimg);
    if (tp < 0) return;
    const Sprite& s = rt_.dw.sprites[sprite];
    if (!s.width || !s.height) return;
    if (s.nineSlice.enabled) {
        drawNineSlice(sprite, tp, x, y, w, h, 0, col, a, true);
        return;
    }
    drawTpag(tp, x, y, 0, 0, w / s.width, h / s.height, 0, col, a);
}

void Graphics::drawSelf(Instance* inst) {
    if (inst->sprite < 0) return;
    if (isSkeletonSprite(rt_, inst->sprite)) {
        skeletonDrawInstance(rt_, inst);
        return;
    }
    drawSprite(inst->sprite, inst->imageIndex, inst->x, inst->y, inst->imageXscale, inst->imageYscale,
               inst->imageAngle, inst->imageBlend, inst->imageAlpha);
}

void Graphics::drawTpagTriangles(int tpag, const float* xy, const float* uv, int vertexCount, const uint16_t* indices,
                                 int indexCount, uint32_t col, double a) {
    if (tpag < 0 || static_cast<size_t>(tpag) >= rt_.dw.tpag.size()) return;
    const TexturePageItem& t = rt_.dw.tpag[tpag];
    if (t.texture < 0 || static_cast<size_t>(t.texture) >= pages_.size()) return;
    const Page& p = page(t.texture);
    if (p.w <= 0 || p.h <= 0) return;
    r_.setTexture(p.tex);
    uint32_t c = packColor(col, a);
    tris_.resize(static_cast<size_t>(indexCount));
    for (int i = 0; i < indexCount; i++) {
        int k = indices[i];
        if (k < 0 || k >= vertexCount) k = 0;
        Vertex& v = tris_[i];
        v.x = xy[k * 2], v.y = xy[k * 2 + 1], v.z = static_cast<float>(drawZ), v.color = c;
        v.u = (t.srcX + uv[k * 2] * t.srcW) / p.w;
        v.v = (t.srcY + uv[k * 2 + 1] * t.srcH) / p.h;
    }
    r_.add(Renderer::TRIANGLES, tris_.data(), indexCount / 3 * 3);
}

void Graphics::backgroundSize(int bg, int& w, int& h) const {
    w = h = 0;
    if (bg < 0 || static_cast<size_t>(bg) >= rt_.dw.backgrounds.size()) return;
    int tp = rt_.dw.backgrounds[bg].tpag;
    if (tp < 0) return;
    w = rt_.dw.tpag[tp].boundW;
    h = rt_.dw.tpag[tp].boundH;
}

void Graphics::drawBackground(int bg, double x, double y, double xs, double ys, double rot, uint32_t col, double a) {
    if (bg < 0 || static_cast<size_t>(bg) >= rt_.dw.backgrounds.size()) return;
    drawTpag(rt_.dw.backgrounds[bg].tpag, x, y, 0, 0, xs, ys, rot, col, a);
}

void Graphics::drawBackgroundPart(int bg, double left, double top, double w, double h, double x, double y, double xs,
                                  double ys, uint32_t col, double a) {
    if (bg < 0 || static_cast<size_t>(bg) >= rt_.dw.backgrounds.size()) return;
    drawTpagPart(rt_.dw.backgrounds[bg].tpag, left, top, w, h, x, y, xs, ys, 0, col, a);
}

void Graphics::drawBackgroundTiled(int bg, double x, double y, double xs, double ys, bool htiled, bool vtiled,
                                   uint32_t col, double a) {
    int w, h;
    backgroundSize(bg, w, h);
    if (w <= 0 || h <= 0) return;
    double tw = w * std::fabs(xs), th = h * std::fabs(ys);
    if (tw < 1 || th < 1) return;
    double x0 = x, y0 = y, x1 = x + tw, y1 = y + th;
    if (htiled) {
        x0 = x - std::ceil((x - projX_) / tw) * tw;
        x1 = projX_ + projW_;
    }
    if (vtiled) {
        y0 = y - std::ceil((y - projY_) / th) * th;
        y1 = projY_ + projH_;
    }
    for (double ty = y0; ty < y1; ty += th)
        for (double tx = x0; tx < x1; tx += tw) drawBackground(bg, tx, ty, xs, ys, 0, col, a);
}

// ---------------------------------------------------------------------------------------------
// Primitives

void Graphics::drawRect(double x1, double y1, double x2, double y2, bool outline, uint32_t c1, uint32_t c2,
                        uint32_t c3, uint32_t c4, double a) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    uint32_t c[4] = {packColor(c1, a), packColor(c2, a), packColor(c3, a), packColor(c4, a)};
    if (!outline) {
        double px[4] = {x1, x2 + 1, x2 + 1, x1}, py[4] = {y1, y1, y2 + 1, y2 + 1};
        drawQuad(r_.whiteTexture(), px, py, 0, 0, 1, 1, c);
        return;
    }
    auto bar = [&](double ax, double ay, double bx, double by, uint32_t ca, uint32_t cb) {
        double px[4] = {ax, bx, bx, ax}, py[4] = {ay, ay, by, by};
        const uint32_t cc[4] = {ca, cb, cb, ca};
        drawQuad(r_.whiteTexture(), px, py, 0, 0, 1, 1, cc);
    };
    bar(x1, y1, x2 + 1, y1 + 1, c[0], c[1]);
    bar(x1, y2, x2 + 1, y2 + 1, c[3], c[2]);
    bar(x1, y1 + 1, x1 + 1, y2, c[0], c[0]);
    bar(x2, y1 + 1, x2 + 1, y2, c[1], c[1]);
}

void Graphics::drawLine(double x1, double y1, double x2, double y2, double width, uint32_t c1, uint32_t c2, double a) {
    x1 += 0.5;
    y1 += 0.5;
    x2 += 0.5;
    y2 += 0.5;
    double dx = x2 - x1, dy = y2 - y1, len = std::sqrt(dx * dx + dy * dy);
    double nx, ny;
    if (len < 1e-9) {
        nx = 0;
        ny = width / 2;
        dx = width / 2;
    } else {
        nx = -dy / len * width / 2;
        ny = dx / len * width / 2;
    }
    uint32_t a1 = packColor(c1, a), a2 = packColor(c2, a);
    double px[4] = {x1 + nx, x2 + nx, x2 - nx, x1 - nx}, py[4] = {y1 + ny, y2 + ny, y2 - ny, y1 - ny};
    if (len < 1e-9) {
        px[0] = x1 - dx; px[1] = x1 + dx; px[2] = x1 + dx; px[3] = x1 - dx;
        py[0] = y1 - ny; py[1] = y1 - ny; py[2] = y1 + ny; py[3] = y1 + ny;
    }
    const uint32_t c[4] = {a1, a2, a2, a1};
    drawQuad(r_.whiteTexture(), px, py, 0, 0, 1, 1, c);
}

void Graphics::drawEllipse(double x1, double y1, double x2, double y2, bool outline, uint32_t c1, uint32_t c2,
                           double a) {
    double cx = (x1 + x2) / 2, cy = (y1 + y2) / 2, rx = std::fabs(x2 - x1) / 2, ry = std::fabs(y2 - y1) / 2;
    int n = std::max(4, circlePrecision);
    uint32_t inner = packColor(c1, a), outer = packColor(c2, a);
    r_.setTexture(r_.whiteTexture());
    std::vector<Vertex> v;
    for (int i = 0; i < n; i++) {
        double t0 = 2 * M_PI * i / n, t1 = 2 * M_PI * (i + 1) / n;
        double ax = cx + rx * std::cos(t0), ay = cy + ry * std::sin(t0);
        double bx = cx + rx * std::cos(t1), by = cy + ry * std::sin(t1);
        if (outline) {
            drawLine(ax - 0.5, ay - 0.5, bx - 0.5, by - 0.5, 1, c2, c2, a);
        } else {
            v.push_back({static_cast<float>(cx), static_cast<float>(cy), 0, inner, 0, 0});
            v.push_back({static_cast<float>(ax), static_cast<float>(ay), 0, outer, 0, 0});
            v.push_back({static_cast<float>(bx), static_cast<float>(by), 0, outer, 0, 0});
        }
    }
    if (!v.empty()) r_.add(Renderer::TRIANGLES, v.data(), static_cast<int>(v.size()));
}

void Graphics::drawPoint(double x, double y, uint32_t c, double a) { drawRect(x, y, x, y, false, c, c, c, c, a); }

// ---------------------------------------------------------------------------------------------
// Particle shapes. Alpha masks approximating GameMaker's built-in images: the same sizes,
// centres and falloffs, but drawn from simple formulas.

namespace {

double hashNoise(int x, int y, int seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
                 static_cast<uint32_t>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0;
}

double valueNoise(double x, double y, int seed) {  // smooth noise in [0, 1]
    int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    double fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    double a = hashNoise(xi, yi, seed), b = hashNoise(xi + 1, yi, seed);
    double c = hashNoise(xi, yi + 1, seed), d = hashNoise(xi + 1, yi + 1, seed);
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

double fractal(double x, double y, int seed) {
    return 0.5 * valueNoise(x / 16, y / 16, seed) + 0.3 * valueNoise(x / 8, y / 8, seed + 1) +
           0.2 * valueNoise(x / 4, y / 4, seed + 2);
}

double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Alpha (0-1) of shape `s` at pixel (x, y).
double shapeAlpha(int s, int px, int py) {
    double x = px + 0.5 - 32, y = py + 0.5 - 32, r = std::hypot(x, y);
    double ang = std::atan2(y, x);
    switch (s) {
        case 0: return std::abs(x) < 1 && std::abs(y) < 1 ? 1 : 0;                       // pixel
        case 1: return clamp01(28.5 - r);                                                  // disk
        case 2: return px >= 3 && px <= 60 && py >= 3 && py <= 60 ? 1 : 0;                 // square
        case 3: {                                                                           // line
            double ends = clamp01((28 - std::abs(x)) / 2);
            return ends * clamp01(std::exp(-y * y / 2.0) + 0.2 * std::exp(-std::abs(y) / 2.5));
        }
        case 4: {                                                                           // star
            double k = std::fmod(ang + M_PI / 2 + 4 * M_PI, 2 * M_PI / 5) / (2 * M_PI / 5);
            double edge = 10 + 17 * std::abs(1 - 2 * k) * std::abs(1 - 2 * k);
            return 0.87 * clamp01(edge - r);
        }
        case 5: return 0.8 * clamp01(1.4 - std::abs(r - 27));                              // circle
        case 6: {                                                                           // ring
            if (r > 28) return 0;
            double in = clamp01((r - 8) / 16);
            return 0.8 * in * in * clamp01((28 - r) / 3.5);
        }
        case 7: return 0.95 * clamp01(1 - r / 30);                                          // sphere
        case 8: return r < 4 ? 1 : clamp01(std::pow(5 / r, 1.6) * clamp01((32 - r) / 4 + 0.3));  // flare
        case 9: {                                                                           // spark
            double rays = 0.5 + 0.5 * std::pow(std::abs(std::cos(ang * 4)), 8);
            return clamp01(std::exp(-r * r / 50) + 0.35 * rays * std::exp(-r / 7));
        }
        case 10: {                                                                          // explosion
            double n = fractal(px, py, 10);
            return clamp01((0.8 - 0.25 * (r / 30) * (r / 30)) * (0.55 + 0.9 * n) * clamp01((31 - r) / 6));
        }
        case 11: {                                                                          // cloud
            double n = fractal(px, py, 11);
            return 0.36 * clamp01(n * 1.6 - 0.35) * clamp01((32 - r) / 10);
        }
        case 12: {                                                                          // smoke
            double n = fractal(px, py, 12);
            return clamp01(0.81 * clamp01(1 - r / 32) * (0.7 + 0.6 * n));
        }
        case 13: {                                                                          // snow
            double k = std::abs(std::fmod(ang + 8 * M_PI, M_PI / 3) / (M_PI / 3) * 2 - 1);  // 6 arms
            double edge = 14 + 14 * std::pow(1 - k, 6);
            return clamp01((edge - r) / 2);
        }
    }
    return 0;
}

}  // namespace

void Graphics::drawParticleShape(int shape, double x, double y, double xs, double ys, double rot, uint32_t col,
                                 double a) {
    if (shape < 0 || shape >= 14 || !enabled_) return;
    if (!particleShapes_[shape]) {
        std::vector<uint8_t> px(64 * 64 * 4);
        for (int py = 0; py < 64; py++)
            for (int pxi = 0; pxi < 64; pxi++) {
                uint8_t* o = &px[(py * 64 + pxi) * 4];
                o[0] = o[1] = o[2] = 255;
                o[3] = static_cast<uint8_t>(std::lround(shapeAlpha(shape, pxi, py) * 255));
            }
        particleShapes_[shape] = r_.createTexture(64, 64, px.data());
    }
    // A 63x63 image with its origin at (32, 32), as in GameMaker's particle texture entries.
    drawTexRect(particleShapes_[shape], 64, 64, 0, 0, 63, 63, -32, -32, 63, 63, x, y, xs, ys, rot, col, a);
}

void Graphics::primitiveBegin(int kind, int texture) {
    primKind_ = kind;
    int w, h;
    if (!textureInfo(texture, primTex_, w, h)) primTex_ = r_.whiteTexture();
    prim_.clear();
}

bool Graphics::textureInfo(double handle, uint32_t& tex, int& w, int& h) {
    int id = static_cast<int>(handle);
    if (id >= kSurfaceTexBase) {
        int sid = id - kSurfaceTexBase;
        if (!surfaceExists(sid)) return false;
        int t = surfaces_[sid];
        tex = r_.targetTexture(t);
        w = r_.targetWidth(t);
        h = r_.targetHeight(t);
        return true;
    }
    if (id < 0 || static_cast<size_t>(id) >= pages_.size() || !page(id).tex) return false;
    tex = pages_[id].tex;
    w = pages_[id].w;
    h = pages_[id].h;
    return true;
}

int Graphics::spriteTexture(int sprite, double subimg) const {
    int tp = spriteFrame(sprite, subimg);
    return tp < 0 ? -1 : rt_.dw.tpag[tp].texture;
}

bool Graphics::spriteUVs(int sprite, double subimg, double (&out)[8]) const {
    int tp = spriteFrame(sprite, subimg);
    if (tp < 0) return false;
    const TexturePageItem& t = rt_.dw.tpag[tp];
    if (t.texture < 0 || static_cast<size_t>(t.texture) >= pages_.size() || pages_[t.texture].w <= 0) return false;
    const Page& p = pages_[t.texture];
    out[0] = static_cast<double>(t.srcX) / p.w;
    out[1] = static_cast<double>(t.srcY) / p.h;
    out[2] = static_cast<double>(t.srcX + t.srcW) / p.w;
    out[3] = static_cast<double>(t.srcY + t.srcH) / p.h;
    out[4] = t.tgtX;
    out[5] = t.tgtY;
    out[6] = t.boundW ? static_cast<double>(t.tgtW) / t.boundW : 1;
    out[7] = t.boundH ? static_cast<double>(t.tgtH) / t.boundH : 1;
    return true;
}

void Graphics::drawVertexBuffer(uint32_t glMode, double texture, const uint8_t* data, int count, int stride,
                                const std::vector<Renderer::Attr>& attrs) {
    uint32_t tex;
    int w, h;
    if (!textureInfo(texture, tex, w, h)) tex = r_.whiteTexture();
    r_.setTexture(tex);
    r_.drawRaw(glMode, data, count, stride, attrs);
}

void Graphics::drawSpritePos(int sprite, double subimg, const double (&px)[4], const double (&py)[4], double a) {
    double uv[8];
    if (!spriteUVs(sprite, subimg, uv)) return;
    uint32_t c = packColor(0xFFFFFF, std::min(1.0, a));
    drawQuad(page(spriteTexture(sprite, subimg)).tex, px, py, uv[0], uv[1], uv[2], uv[3], {c, c, c, c});
}

void Graphics::drawSpriteAffine(int sprite, double subimg, const double (&m)[6], uint32_t col, double a) {
    int tp = spriteFrame(sprite, subimg);
    if (tp < 0) return;
    const Sprite& s = rt_.dw.sprites[sprite];
    if (s.nineSlice.enabled) {  // nine-slice sprites keep their borders: draw at the matrix's scale
        double sx = std::hypot(m[0], m[1]), sy = std::hypot(m[2], m[3]);
        if (m[0] * m[3] - m[1] * m[2] < 0) sy = -sy;
        double rot = std::atan2(-m[1], m[0]) * 180.0 / M_PI;
        drawNineSlice(sprite, tp, m[4], m[5], s.width * sx, s.height * sy, rot, col, a, true);
        return;
    }
    const TexturePageItem& t = rt_.dw.tpag[tp];
    if (t.texture < 0 || static_cast<size_t>(t.texture) >= pages_.size()) return;
    const Page& p = page(t.texture);
    if (p.w <= 0 || p.h <= 0) return;
    const double lx[4] = {double(t.tgtX), double(t.tgtX + t.tgtW), double(t.tgtX + t.tgtW), double(t.tgtX)};
    const double ly[4] = {double(t.tgtY), double(t.tgtY), double(t.tgtY + t.tgtH), double(t.tgtY + t.tgtH)};
    double px[4], py[4];
    for (int i = 0; i < 4; i++) {
        px[i] = m[0] * lx[i] + m[2] * ly[i] + m[4];
        py[i] = m[1] * lx[i] + m[3] * ly[i] + m[5];
    }
    uint32_t c = packColor(col, a);
    const uint32_t cs[4] = {c, c, c, c};
    drawQuad(p.tex, px, py, double(t.srcX) / p.w, double(t.srcY) / p.h, double(t.srcX + t.srcW) / p.w,
             double(t.srcY + t.srcH) / p.h, cs);
}

void Graphics::primitiveVertex(double x, double y, uint32_t c, double a, double u, double v) {
    prim_.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(drawZ), packColor(c, a), static_cast<float>(u),
                     static_cast<float>(v)});
}

void Graphics::primitiveEnd() {
    r_.setTexture(primTex_);
    std::vector<Vertex> out;
    switch (primKind_) {
        case 1:  // pr_pointlist
            r_.add(Renderer::POINTS, prim_.data(), static_cast<int>(prim_.size()));
            break;
        case 2:  // pr_linelist
            r_.add(Renderer::LINES, prim_.data(), static_cast<int>(prim_.size() & ~size_t(1)));
            break;
        case 3:  // pr_linestrip
            for (size_t i = 1; i < prim_.size(); i++) {
                out.push_back(prim_[i - 1]);
                out.push_back(prim_[i]);
            }
            r_.add(Renderer::LINES, out.data(), static_cast<int>(out.size()));
            break;
        case 4:  // pr_trianglelist
            r_.add(Renderer::TRIANGLES, prim_.data(), static_cast<int>(prim_.size() / 3 * 3));
            break;
        case 5:  // pr_trianglestrip
            for (size_t i = 2; i < prim_.size(); i++) {
                if (i % 2) {
                    out.push_back(prim_[i - 1]);
                    out.push_back(prim_[i - 2]);
                } else {
                    out.push_back(prim_[i - 2]);
                    out.push_back(prim_[i - 1]);
                }
                out.push_back(prim_[i]);
            }
            r_.add(Renderer::TRIANGLES, out.data(), static_cast<int>(out.size()));
            break;
        case 6:  // pr_trianglefan
            for (size_t i = 2; i < prim_.size(); i++) {
                out.push_back(prim_[0]);
                out.push_back(prim_[i - 1]);
                out.push_back(prim_[i]);
            }
            r_.add(Renderer::TRIANGLES, out.data(), static_cast<int>(out.size()));
            break;
    }
    prim_.clear();
}

void Graphics::drawClear(uint32_t c, double a) { r_.clear(c, a); }

// ---------------------------------------------------------------------------------------------
// Fonts and text

void Graphics::opaqueColumns(int tpag, int& first, int& last) {
    first = last = -1;
    if (tpag < 0 || static_cast<size_t>(tpag) >= rt_.dw.tpag.size()) return;
    const TexturePageItem& t = rt_.dw.tpag[tpag];
    if (t.texture < 0 || static_cast<size_t>(t.texture) >= pages_.size()) return;
    Page& p = pages_[t.texture];
    if (p.pixels.empty()) {
        int w, h;
        std::string err;
        if (!decodeTexture(rt_.dw.textures[t.texture], p.pixels, w, h, err, rt_.gameDir)) return;
    }
    for (int cx = 0; cx < t.srcW; cx++) {
        bool any = false;
        for (int cy = 0; cy < t.srcH && !any; cy++) {
            size_t idx = (static_cast<size_t>(t.srcY + cy) * p.w + t.srcX + cx) * 4 + 3;
            if (idx < p.pixels.size() && p.pixels[idx] > 0) any = true;
        }
        if (any) {
            int fx = t.tgtX + (t.srcW ? cx * t.tgtW / t.srcW : cx);
            if (first < 0) first = fx;
            last = fx;
        }
    }
}

void Graphics::loadFonts() {
    for (size_t i = 0; i < rt_.dw.fonts.size(); i++) {
        const Font& f = rt_.dw.fonts[i];
        FontRec rec;
        rec.live = true;
        rec.dataFont = static_cast<int>(i);
        double maxH = 0;
        for (size_t g = 0; g < f.glyphs.size(); g++) {
            const auto& gl = f.glyphs[g];
            Glyph glyph;
            glyph.dataIndex = static_cast<int>(g);
            glyph.offset = gl.offset;
            glyph.width = gl.w;
            glyph.advance = gl.shift;
            rec.glyphs[gl.ch] = glyph;
            maxH = std::max(maxH, static_cast<double>(gl.h));
        }
        rec.height = maxH > 0 ? maxH : f.emSize;
        auto sp = rec.glyphs.find(U' ');
        rec.spaceAdvance = sp != rec.glyphs.end() ? sp->second.advance : rec.height / 3;
        fonts_.push_back(std::move(rec));
    }
}

int Graphics::addSpriteFont(int sprite, const std::u32string& map, bool proportional, int sep) {
    fonts_.emplace_back();
    int id = static_cast<int>(fonts_.size() - 1);
    replaceSpriteFont(id, sprite, map, proportional, sep);
    return id;
}

bool Graphics::replaceSpriteFont(int font, int sprite, const std::u32string& map, bool proportional, int sep) {
    if (font < 0 || static_cast<size_t>(font) >= fonts_.size()) return false;
    if (sprite < 0 || static_cast<size_t>(sprite) >= rt_.dw.sprites.size()) return false;
    const Sprite& s = rt_.dw.sprites[sprite];
    FontRec rec;
    rec.live = true;
    rec.spriteFont = true;
    rec.sprite = sprite;
    rec.height = s.height;
    double total = 0;
    int counted = 0;
    for (size_t i = 0; i < map.size() && i < s.frames.size(); i++) {
        Glyph g;
        g.frame = static_cast<int>(i);
        if (proportional) {
            int first, last;
            opaqueColumns(s.frames[i], first, last);
            if (first < 0) {
                g.offset = 0;
                g.width = s.width;
            } else {
                g.offset = first;
                g.width = last - first + 1;
            }
        } else {
            g.width = s.width;
        }
        g.advance = g.width + sep;
        rec.glyphs[map[i]] = g;
        total += g.width;
        counted++;
    }
    auto sp = rec.glyphs.find(U' ');
    if (sp != rec.glyphs.end()) rec.spaceAdvance = sp->second.advance;
    else rec.spaceAdvance = (proportional && counted ? total / counted : s.width) + sep;
    fonts_[font] = std::move(rec);
    return true;
}

int Graphics::addPlaceholderFont() {
    FontRec rec;
    rec.live = true;
    rec.height = 12;
    rec.spaceAdvance = 6;
    fonts_.push_back(rec);
    return static_cast<int>(fonts_.size() - 1);
}

bool Graphics::makeTrueTypeFont(FontRec& rec, const uint8_t* data, std::vector<uint8_t> owned, double points, bool bold,
                                bool italic) {
    TtfFont t;
    t.face = std::make_unique<TrueTypeFace>();
    if (!t.face->load(data, std::move(owned), points, bold, italic)) return false;
    rec = FontRec();
    rec.live = true;
    rec.ttf = static_cast<int>(ttfs_.size());
    rec.points = points;
    rec.height = t.face->lineHeight();
    ttfs_.push_back(std::move(t));
    bakeGlyphs(rec, U" ");
    rec.spaceAdvance = rec.glyphs[U' '].advance;
    return true;
}

int Graphics::addTrueTypeFont(const uint8_t* data, std::vector<uint8_t> owned, double points, bool bold, bool italic,
                              int first, int last) {
    FontRec rec;
    if (!makeTrueTypeFont(rec, data, std::move(owned), points, bold, italic)) return -1;
    std::u32string range;
    for (int c = std::max(first, 32); c <= last && c - first < 1024; c++) range += static_cast<char32_t>(c);
    bakeGlyphs(rec, range);
    fonts_.push_back(std::move(rec));
    return static_cast<int>(fonts_.size() - 1);
}

void Graphics::bakeGlyphs(FontRec& f, const std::u32string& s) {
    if (f.ttf < 0 || !ttfs_[f.ttf].face) return;
    TrueTypeFace& face = *ttfs_[f.ttf].face;
    for (char32_t c : s) {
        if (c < 32 || f.glyphs.count(c) || !face.has(c)) continue;  // a character the font lacks isn't drawn
        TrueTypeFace::Glyph tg = face.bake(c);
        Glyph g;
        g.offset = tg.offset;
        g.width = tg.w;
        g.advance = tg.advance;
        g.ax = tg.x, g.ay = tg.y, g.aw = tg.w, g.ah = tg.h;
        f.glyphs[c] = g;
    }
}

uint32_t Graphics::ttfTexture(TtfFont& t) {
    if (!enabled_ || !t.face) return 0;
    if (t.face->dirty || !t.tex) {
        r_.flush();
        if (t.tex) r_.deleteTexture(t.tex);
        t.tex = r_.createTexture(t.face->atlasWidth(), t.face->atlasHeight(), t.face->pixels().data());
        t.texW = t.face->atlasWidth();
        t.texH = t.face->atlasHeight();
        t.face->dirty = false;
    }
    return t.tex;
}

double Graphics::fontPoints(int font) const { return fontExists(font) ? fonts_[font].points : 0; }

bool Graphics::runtimeFontInfo(int font, RuntimeFontInfo& out) const {
    if (!fontExists(font) || fonts_[font].dataFont >= 0) return false;
    const FontRec& f = fonts_[font];
    out.sprite = f.sprite;
    out.points = f.points;
    out.trueType = f.ttf >= 0;
    int spriteH = f.spriteFont ? static_cast<int>(f.height) : 0;
    for (auto& [c, g] : f.glyphs) {
        if (f.spriteFont)
            out.glyphs.push_back({c, g.frame, 0, 0, static_cast<int>(g.width), spriteH, g.advance, g.offset});
        else
            out.glyphs.push_back({c, -1, g.ax, g.ay, g.aw, g.ah, g.advance, g.offset});
    }
    std::sort(out.glyphs.begin(), out.glyphs.end(), [](auto& a, auto& b) { return a.ch < b.ch; });
    return true;
}

void Graphics::deleteFont(int font) {
    if (!fontExists(font) || static_cast<size_t>(font) < rt_.dw.fonts.size()) return;
    if (int t = fonts_[font].ttf; t >= 0) {
        if (ttfs_[t].tex) r_.deleteTexture(ttfs_[t].tex);
        ttfs_[t] = TtfFont();
    }
    fonts_[font] = FontRec();
    if (this->font == font) this->font = -1;
}

bool Graphics::fontExists(int font) const {
    return font >= 0 && static_cast<size_t>(font) < fonts_.size() && fonts_[font].live;
}

Graphics::FontRec* Graphics::currentFont() {
    if (fontExists(font)) return &fonts_[font];
    if (!defaultFontTried_) {
        defaultFontTried_ = true;
        if (rt_.dw.gen.gms2) {
            // GameMaker 2's built-in font is monospaced: 9px advances on 21px lines. Liberation
            // Mono's advance is 0.6 em, so 15px (11.25pt).
            FontBytes mono = builtinFont(BuiltinFont::Mono);
            if (mono.size && makeTrueTypeFont(defaultFont_, mono.data, {}, 11.25, false)) defaultFont_.height = 21;
        } else {
            FontBytes sans = builtinFont(BuiltinFont::Sans);  // Arial in GameMaker 1.4
            if (sans.size) makeTrueTypeFont(defaultFont_, sans.data, {}, 12, false);
        }
    }
    return defaultFont_.live ? &defaultFont_ : nullptr;
}

double Graphics::glyphRun(const FontRec* f, const std::u32string& line) const {
    if (!f) return line.size() * 8.0;
    double w = 0;
    for (char32_t c : line) {
        auto it = f->glyphs.find(c);
        if (it != f->glyphs.end()) w += it->second.advance;
        else if (c == U' ') w += f->spaceAdvance;
    }
    return w;
}

// GMS 1.4 text: '#' is a line break ("\#" for a literal '#'), as are CR/LF.
void Graphics::layoutLines(const std::u32string& s, double w, double sep, const FontRec* f,
                           std::vector<std::u32string>& lines, std::vector<double>& widths, double& lineHeight) {
    std::vector<std::u32string> raw(1);
    for (size_t i = 0; i < s.size(); i++) {
        char32_t c = s[i];
        bool hashNewline = !rt_.dw.gen.gms2;  // GMS2 uses only real line breaks
        if (hashNewline && c == U'\\' && i + 1 < s.size() && s[i + 1] == U'#') {
            raw.back() += U'#';
            i++;
        } else if ((hashNewline && c == U'#') || c == U'\n') {
            raw.emplace_back();
        } else if (c == U'\r') {
            if (i + 1 < s.size() && s[i + 1] == U'\n') i++;
            raw.emplace_back();
        } else {
            raw.back() += c;
        }
    }
    lineHeight = sep >= 0 ? sep : (f ? f->height : 12);
    for (auto& line : raw) {
        if (w <= 0 || glyphRun(f, line) <= w) {
            lines.push_back(line);
            continue;
        }
        // Word wrap at spaces; a single over-long word stays on its own line.
        std::u32string cur;
        size_t i = 0;
        while (i < line.size()) {
            size_t j = line.find(U' ', i);
            if (j == std::u32string::npos) j = line.size();
            std::u32string word = line.substr(i, j - i);
            std::u32string trial = cur.empty() ? word : cur + U' ' + word;
            if (!cur.empty() && glyphRun(f, trial) > w) {
                lines.push_back(cur);
                cur = word;
            } else {
                cur = trial;
            }
            i = j + 1;
        }
        lines.push_back(cur);
    }
    for (auto& l : lines) widths.push_back(glyphRun(f, l));
}

double Graphics::stringWidth(const std::string& s, double sep, double w) {
    std::vector<std::u32string> lines;
    std::vector<double> widths;
    double lh;
    std::u32string text = utf8Decode(s);
    FontRec* f = currentFont();
    if (f) bakeGlyphs(*f, text);
    layoutLines(text, w, sep, f, lines, widths, lh);
    double m = 0;
    for (double x : widths) m = std::max(m, x);
    return m;
}

double Graphics::stringHeight(const std::string& s, double sep, double w) {
    std::vector<std::u32string> lines;
    std::vector<double> widths;
    double lh;
    std::u32string text = utf8Decode(s);
    FontRec* f = currentFont();
    if (f) bakeGlyphs(*f, text);
    layoutLines(text, w, sep, f, lines, widths, lh);
    return lh * lines.size();
}

void Graphics::drawText(double x, double y, const std::string& s, double sep, double w, double xs, double ys,
                        double rot, uint32_t c1, uint32_t, uint32_t, uint32_t, double a) {
    FontRec* f = currentFont();
    if (!f || !f->live) return;
    std::vector<std::u32string> lines;
    std::vector<double> widths;
    double lh;
    std::u32string text = utf8Decode(s);
    bakeGlyphs(*f, text);
    layoutLines(text, w, sep, f, lines, widths, lh);
    TtfFont* ttf = f->ttf >= 0 ? &ttfs_[f->ttf] : nullptr;
    uint32_t ttfTex = ttf ? ttfTexture(*ttf) : 0;
    double total = lh * lines.size();
    double ly = valign == 1 ? -total / 2 : (valign == 2 ? -total : 0);
    double ca = 1, sa = 0;
    if (rot != 0) {
        double r = rot * M_PI / 180.0;
        ca = std::cos(r);
        sa = std::sin(r);
    }
    const Font* df = f->dataFont >= 0 ? &rt_.dw.fonts[f->dataFont] : nullptr;
    for (size_t li = 0; li < lines.size(); li++) {
        double lx = halign == 1 ? -std::floor(widths[li] / 2) : (halign == 2 ? -widths[li] : 0);
        for (char32_t c : lines[li]) {
            auto it = f->glyphs.find(c);
            if (it == f->glyphs.end()) {
                if (c == U' ') lx += f->spaceAdvance;
                continue;
            }
            const Glyph& g = it->second;
            double qx = lx * xs, qy = ly * ys;
            double wx = x + qx * ca + qy * sa, wy = y - qx * sa + qy * ca;
            if (f->spriteFont) {
                // Sprite fonts never draw the space, only advance past it (YoYo's
                // Draw_Sprite_String): a font tested has a mark in its space frame.
                const Sprite& sp = rt_.dw.sprites[f->sprite];
                if (c != U' ' && g.frame >= 0 && static_cast<size_t>(g.frame) < sp.frames.size())
                    drawTpag(sp.frames[g.frame], wx, wy, g.offset, 0, xs, ys, rot, c1, a);
            } else if (ttf) {
                if (g.aw > 0 && ttfTex)
                    drawTexRect(ttfTex, ttf->texW, ttf->texH, g.ax, g.ay, g.aw, g.ah, g.offset, 0, g.aw, g.ah, wx, wy,
                                xs, ys, rot, c1, a);
            } else if (df && df->tpag >= 0) {
                const auto& gl = df->glyphs[g.dataIndex];
                const TexturePageItem& t = rt_.dw.tpag[df->tpag];
                if (t.texture >= 0 && static_cast<size_t>(t.texture) < pages_.size()) {
                    const Page& p = page(t.texture);
                    drawTexRect(p.tex, p.w, p.h, t.srcX + gl.x, t.srcY + gl.y, gl.w, gl.h, g.offset, 0, gl.w, gl.h,
                                wx, wy, xs, ys, rot, c1, a);
                }
            }
            lx += g.advance;
        }
        ly += lh;
    }
}

// ---------------------------------------------------------------------------------------------
// Surfaces

int Graphics::surfaceCreate(int w, int h) {
    // Surface 0 never exists: games keep "no surface yet" as 0 and test it with surface_exists
    // (window silhouettes), which must not find the application surface there.
    if (surfaces_.empty()) surfaces_.push_back(-1);
    int t = r_.createTarget(w, h);
    surfaces_.push_back(t);
    return static_cast<int>(surfaces_.size() - 1);
}

bool Graphics::surfaceExists(int id) const {
    return id >= 0 && static_cast<size_t>(id) < surfaces_.size() && r_.targetExists(surfaces_[id]);
}

void Graphics::surfaceFree(int id) {
    if (!surfaceExists(id) || id == appSurface_) return;
    for (auto& s : targetStack_)
        if (s.target == surfaces_[id]) return;  // still a render target somewhere up the stack
    if (curTarget_ == surfaces_[id]) return;
    r_.deleteTarget(surfaces_[id]);
    surfaces_[id] = -1;
}

int Graphics::surfaceWidth(int id) const { return surfaceExists(id) ? r_.targetWidth(surfaces_[id]) : -1; }
int Graphics::surfaceHeight(int id) const { return surfaceExists(id) ? r_.targetHeight(surfaces_[id]) : -1; }

void Graphics::surfaceResize(int id, int w, int h) {
    if (id < 0 && appSurface_ < 0 && w > 0 && h > 0) {  // application_surface before the first draw
        appW_ = w;
        appH_ = h;
        return;
    }
    if (!surfaceExists(id)) return;
    r_.deleteTarget(surfaces_[id]);
    surfaces_[id] = r_.createTarget(w, h);
    if (id == appSurface_) {  // surface_resize(application_surface, ...) sets the application size
        appW_ = w;
        appH_ = h;
    }
}

bool Graphics::surfaceSetTarget(int id) {
    if (!surfaceExists(id)) return false;
    targetStack_.push_back(currentState());
    int t = surfaces_[id];
    bindAndSetup(t, r_.targetWidth(t), r_.targetHeight(t));
    return true;
}

bool Graphics::surfaceResetTarget() {
    if (targetStack_.empty()) return false;
    TargetState s = targetStack_.back();
    targetStack_.pop_back();
    restoreState(s);
    return true;
}

void Graphics::surfaceCopy(int dest, double x, double y, int src) {
    if (!surfaceExists(dest) || !surfaceExists(src)) return;
    TargetState saved = currentState();
    int t = surfaces_[dest];
    bindAndSetup(t, r_.targetWidth(t), r_.targetHeight(t));
    bool ab = alphaBlend_;
    int sh = shader_;
    r_.setShader(-1);
    setAlphaBlend(false);
    int st = surfaces_[src];
    int w = r_.targetWidth(st), h = r_.targetHeight(st);
    drawTexRect(r_.targetTexture(st), w, h, 0, 0, w, h, 0, 0, w, h, x, y, 1, 1, 0, 0xFFFFFF, 1);
    setAlphaBlend(ab);
    r_.setShader(sh);
    restoreState(saved);
}

void Graphics::drawSurface(int id, double x, double y, double xs, double ys, double rot, uint32_t col, double a) {
    if (!surfaceExists(id)) return;
    int t = surfaces_[id];
    int w = r_.targetWidth(t), h = r_.targetHeight(t);
    if (t == curTarget_) return;  // GL can't sample the bound target
    drawTexRect(r_.targetTexture(t), w, h, 0, 0, w, h, 0, 0, w, h, x, y, xs, ys, rot, col, a);
}

void Graphics::drawSurfacePart(int id, double left, double top, double w, double h, double x, double y, double xs,
                               double ys, uint32_t col, double a) {
    if (!surfaceExists(id)) return;
    int t = surfaces_[id];
    if (t == curTarget_) return;
    int sw = r_.targetWidth(t), sh = r_.targetHeight(t);
    double l = std::max(0.0, left), tp = std::max(0.0, top);
    double r = std::min<double>(sw, left + w), b = std::min<double>(sh, top + h);
    if (r <= l || b <= tp) return;
    drawTexRect(r_.targetTexture(t), sw, sh, l, tp, r - l, b - tp, l - left, tp - top, r - l, b - tp, x, y, xs, ys, 0,
                col, a);
}

void Graphics::drawSurfaceStretched(int id, double x, double y, double w, double h, uint32_t col, double a) {
    if (!surfaceExists(id)) return;
    int sw = surfaceWidth(id), sh = surfaceHeight(id);
    if (sw <= 0 || sh <= 0) return;
    drawSurface(id, x, y, w / sw, h / sh, 0, col, a);
}

// draw_surface_tiled(_ext): copies of the surface repeated over the area being drawn (the view).
void Graphics::drawSurfaceTiled(int id, double x, double y, double xs, double ys, uint32_t col, double a) {
    if (!surfaceExists(id)) return;
    double tw = surfaceWidth(id) * std::fabs(xs), th = surfaceHeight(id) * std::fabs(ys);
    if (tw < 1 || th < 1) return;
    double x0 = x - std::ceil((x - projX_) / tw) * tw, y0 = y - std::ceil((y - projY_) / th) * th;
    for (double ty = y0; ty < projY_ + projH_; ty += th)
        for (double tx = x0; tx < projX_ + projW_; tx += tw) drawSurface(id, tx, ty, xs, ys, 0, col, a);
}

void Graphics::drawSurfaceGeneral(int id, double left, double top, double w, double h, double x, double y, double xs,
                                  double ys, double rot, const uint32_t (&cols)[4], double a) {
    if (!surfaceExists(id)) return;
    int t = surfaces_[id];
    if (t == curTarget_) return;
    int sw = r_.targetWidth(t), sh = r_.targetHeight(t);
    double l = std::max(0.0, left), tp = std::max(0.0, top);
    double r = std::min<double>(sw, left + w), b = std::min<double>(sh, top + h);
    if (r <= l || b <= tp) return;
    const uint32_t packed[4] = {packColor(cols[0], a), packColor(cols[1], a), packColor(cols[2], a), packColor(cols[3], a)};
    drawTexRect4(r_.targetTexture(t), sw, sh, l, tp, r - l, b - tp, l - left, tp - top, r - l, b - tp, x, y, xs, ys,
                 rot, packed);
}

// surface_copy_part: a region of one surface written (not blended) into another at x, y.
void Graphics::surfaceCopyPart(int dest, double x, double y, int src, double left, double top, double w, double h) {
    if (!surfaceExists(dest) || !surfaceExists(src) || dest == src) return;
    int st = surfaces_[src];
    int sw = r_.targetWidth(st), sh = r_.targetHeight(st);
    double l = std::max(0.0, left), tp = std::max(0.0, top);
    double r = std::min<double>(sw, left + w), b = std::min<double>(sh, top + h);
    if (r <= l || b <= tp) return;
    TargetState saved = currentState();
    int t = surfaces_[dest];
    bindAndSetup(t, r_.targetWidth(t), r_.targetHeight(t));
    bool ab = alphaBlend_;
    int shd = shader_;
    r_.setShader(-1);
    setAlphaBlend(false);
    drawTexRect(r_.targetTexture(st), sw, sh, l, tp, r - l, b - tp, 0, 0, r - l, b - tp, x + (l - left), y + (tp - top),
                1, 1, 0, 0xFFFFFF, 1);
    setAlphaBlend(ab);
    r_.setShader(shd);
    restoreState(saved);
}

int Graphics::surfaceGetTarget() const {
    if (curTarget_ < 0 || curTarget_ == backbuffer_) return -1;
    for (size_t i = 0; i < surfaces_.size(); i++)
        if (surfaces_[i] == curTarget_) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------------------------------------
// Frame

void Graphics::runDrawEvent(uint32_t subtype, bool sortedByDepth) {
    if (rt_.dw.gen.gms2 && !rt_.layers.empty()) {
        // GMS2: layer by layer (invisible layers get no draw events), as YoYo's ExecuteDrawEvent;
        // instances that left their layer (depth set directly) are placed by depth among them.
        struct Item {
            double depth;
            int kind;  // 0 instance, 3 layer
            uint64_t order;
            Instance* inst;
            int32_t layer;
        };
        std::vector<Item> items;
        for (Instance* i : rt_.instances)
            if (Runner::live(i) && i->visible && i->layerElement < 0 && rt_.findHandler(i->object, EV_DRAW, subtype))
                items.push_back({i->depth, 0, i->createOrder, i, -1});
        for (const Layer& L : rt_.layers)
            if (L.visible) items.push_back({L.depth, 3, ~L.order, nullptr, L.id});
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.depth != b.depth) return a.depth > b.depth;
            if (a.kind != b.kind) return a.kind > b.kind;
            return a.order < b.order;
        });
        for (auto& it : items) {
            if (it.kind == 3) {
                drawLayer(it.layer, subtype);
            } else if (Runner::live(it.inst) && it.inst->visible) {
                rt_.performEvent(it.inst, EV_DRAW, subtype);
            }
        }
        return;
    }
    std::vector<Instance*> list;
    for (Instance* i : rt_.instances)
        if (Runner::live(i) && i->visible && rt_.findHandler(i->object, EV_DRAW, subtype)) list.push_back(i);
    if (sortedByDepth)
        std::stable_sort(list.begin(), list.end(), [](Instance* a, Instance* b) { return a->depth > b->depth; });
    for (Instance* i : list)
        if (Runner::live(i)) rt_.performEvent(i, EV_DRAW, subtype);
}

void Graphics::drawTile(const Tile& t) {
    if (rt_.dw.gen.gms2) {  // GMS2 tiles (room asset layers, layer_tile_create) take a sprite's first frame
        const auto& sprites = rt_.dw.sprites;
        if (t.background >= 0 && static_cast<size_t>(t.background) < sprites.size() &&
            !sprites[t.background].frames.empty())
            drawTpagPart(sprites[t.background].frames[0], t.left, t.top, t.width, t.height, t.x, t.y, t.xscale,
                         t.yscale, 0, t.blend, t.alpha);
        return;
    }
    drawBackgroundPart(t.background, t.left, t.top, t.width, t.height, t.x, t.y, t.xscale, t.yscale, t.blend, t.alpha);
}

void Graphics::drawRoomBackgrounds(bool foreground) {
    for (auto& b : rt_.backgrounds) {
        if (!b.visible || b.foreground != foreground || b.index < 0) continue;
        double xs = b.xscale, ys = b.yscale;
        if (b.stretch) {
            int w, h;
            backgroundSize(b.index, w, h);
            if (w > 0 && h > 0) {
                xs = rt_.roomWidth / w;
                ys = rt_.roomHeight / h;
            }
        }
        drawBackgroundTiled(b.index, b.x, b.y, xs, ys, b.htiled, b.vtiled, b.blend, b.alpha);
    }
}

// A GMS2 layer: its elements in order (backgrounds, instances, sprites, tile maps), offset by
// the layer's scroll position.
void Graphics::drawLayer(int32_t layerId, uint32_t subtype) {
    const Layer* L = rt_.layerById(layerId);
    if (!L) return;
    // Draw events and layer scripts may add layers or elements, so work from copies and look
    // the layer up again after running GML.
    Value scriptBegin = L->scriptBegin, scriptEnd = L->scriptEnd;
    int32_t shader = L->shader;
    std::vector<int32_t> elements = L->elements;
    FilterPass filter;
    if (subtype == DRAW_NORMAL) filter = beginLayerFilter(*L);  // layer filters run in the Draw event
    if (shader >= 0) shaderSet(shader);
    drawDepth = L->depth;
    if (drawZSet) drawZ = 0, drawZSet = false;
    rt_.runLayerScript(layerId, scriptBegin, subtype);
    for (int32_t id : elements) {
        if (!(L = rt_.layerById(layerId))) break;
        const LayerElement* e = rt_.elementById(id);
        if (!e || e->layer != layerId) continue;
        if (e->type == ElementType::Instance) {
            Instance* inst = rt_.findById(e->instance);
            // Only while it is still on this layer (destroyed / moved instances are skipped).
            if (!inst || inst->layerElement != e->id || !Runner::live(inst) || !inst->visible) continue;
            if (inst->seqControlled && inst->drawnBySequence) continue;  // its sequence draws it
            drawDepth = inst->depth;
            if (drawZSet) drawZ = 0, drawZSet = false;
            if (subtype == DRAW_NORMAL) {
                if (!rt_.performEvent(inst, EV_DRAW, DRAW_NORMAL)) drawSelf(inst);
            } else if (rt_.findHandler(inst->object, EV_DRAW, subtype)) {
                rt_.performEvent(inst, EV_DRAW, subtype);
            }
            continue;
        }
        if (subtype != DRAW_NORMAL) continue;
        switch (e->type) {
            case ElementType::Background: drawBackgroundElement(*L, *e); break;
            case ElementType::Sprite:
                if (e->sprite >= 0)
                    drawSprite(e->sprite, e->index, e->x + L->x, e->y + L->y, e->xscale, e->yscale, e->angle, e->blend,
                               e->alpha);
                break;
            case ElementType::Tilemap: drawTilemap(*L, *e); break;
            case ElementType::Sequence:
                if (rt_.sequences) rt_.sequences->draw(*L, *e);
                break;
            default: break;
        }
    }
    rt_.runLayerScript(layerId, scriptEnd, subtype);
    if (shader >= 0) shaderReset();
    endLayerFilter(filter);
}

void Graphics::drawBackgroundElement(const Layer& L, const LayerElement& e) {
    if (!e.visible || e.foreground) return;
    if (e.sprite < 0 || static_cast<size_t>(e.sprite) >= rt_.dw.sprites.size()) {  // plain colour fill
        drawRect(projX_ - 1, projY_ - 1, projX_ + projW_ + 1, projY_ + projH_ + 1, false, e.blend, e.blend, e.blend,
                 e.blend, e.alpha);
        return;
    }
    const Sprite& sp = rt_.dw.sprites[e.sprite];
    double xs = e.xscale, ys = e.yscale;
    if (e.stretch && sp.width && sp.height) {
        xs = rt_.roomWidth / sp.width;
        ys = rt_.roomHeight / sp.height;
    }
    double tw = sp.width * std::fabs(xs), th = sp.height * std::fabs(ys);
    if (tw < 1 || th < 1) return;
    double x0 = L.x, y0 = L.y, x1 = L.x + tw, y1 = L.y + th;
    if (e.htiled) {
        x0 = L.x - std::ceil((L.x - projX_) / tw) * tw;
        x1 = projX_ + projW_;
    }
    if (e.vtiled) {
        y0 = L.y - std::ceil((L.y - projY_) / th) * th;
        y1 = projY_ + projH_;
    }
    // Backgrounds ignore the sprite's origin.
    for (double ty = y0; ty < y1; ty += th)
        for (double tx = x0; tx < x1; tx += tw)
            drawSprite(e.sprite, e.index, tx + sp.originX * xs, ty + sp.originY * ys, xs, ys, 0, e.blend, e.alpha);
}

int Graphics::tilesetFrame(int tileset) const {
    if (tileset < 0 || static_cast<size_t>(tileset) >= rt_.dw.backgrounds.size()) return 0;
    const Background& ts = rt_.dw.backgrounds[tileset];
    if (ts.framesPerTile <= 1 || ts.frameTime <= 0) return 0;
    return static_cast<int>(static_cast<uint32_t>(rt_.currentTimeMs() * 1000.0 / ts.frameTime) % ts.framesPerTile);
}

// One tile (tile data: index plus mirror/flip/rotate bits) with its top-left corner at x, y.
void Graphics::drawTileData(int tileset, uint32_t data, int frame, double x, double y, uint32_t col, double a) {
    if (tileset < 0 || static_cast<size_t>(tileset) >= rt_.dw.backgrounds.size()) return;
    const Background& ts = rt_.dw.backgrounds[tileset];
    if (!ts.tileWidth || !ts.tileHeight || !ts.columns) return;
    uint32_t index = data & 0x7FFFF;
    if (index == 0) return;
    if (ts.framesPerTile > 1 && frame > 0) {
        size_t at = static_cast<size_t>(index) * ts.framesPerTile + static_cast<size_t>(frame) % ts.framesPerTile;
        if (at < ts.tileIds.size()) index = ts.tileIds[at];
    }
    bool mirror = data & (1u << 28), flip = data & (1u << 29), rotate = data & (1u << 30);
    double sx = (index % ts.columns) * (ts.tileWidth + 2.0 * ts.borderX) + ts.borderX;
    double sy = (index / ts.columns) * (ts.tileHeight + 2.0 * ts.borderY) + ts.borderY;
    double hw = ts.tileWidth / 2.0, hh = ts.tileHeight / 2.0;
    // Drawn centred so mirror/flip/rotate happen around the tile's middle.
    drawTpagPart(ts.tpag, sx, sy, ts.tileWidth, ts.tileHeight, x + hw, y + hh, mirror ? -1 : 1, flip ? -1 : 1,
                 rotate ? -90 : 0, col, a, hw, hh);
}

void Graphics::drawTilemap(const Layer& L, const LayerElement& e) { drawTilemapAt(e, L.x + e.x, L.y + e.y); }

void Graphics::drawTilemapAt(const LayerElement& e, double ox, double oy) {
    if (e.tileset < 0 || static_cast<size_t>(e.tileset) >= rt_.dw.backgrounds.size()) return;
    const Background& ts = rt_.dw.backgrounds[e.tileset];
    if (!ts.tileWidth || !ts.tileHeight || !ts.columns || e.tiles.size() < static_cast<size_t>(e.tilesX) * e.tilesY) return;
    int frame = tilesetFrame(e.tileset);
    uint32_t mask = e.tileMask & rt_.tileGlobalMask;
    // Only the tiles that intersect the current view.
    int cx0 = std::max(0, static_cast<int>(std::floor((projX_ - ox) / ts.tileWidth)));
    int cy0 = std::max(0, static_cast<int>(std::floor((projY_ - oy) / ts.tileHeight)));
    int cx1 = std::min(static_cast<int>(e.tilesX) - 1, static_cast<int>((projX_ + projW_ - ox) / ts.tileWidth));
    int cy1 = std::min(static_cast<int>(e.tilesY) - 1, static_cast<int>((projY_ + projH_ - oy) / ts.tileHeight));
    for (int ty = cy0; ty <= cy1; ty++)
        for (int tx = cx0; tx <= cx1; tx++)
            drawTileData(e.tileset, e.tiles[static_cast<size_t>(ty) * e.tilesX + tx] & mask, frame, ox + tx * ts.tileWidth,
                         oy + ty * ts.tileHeight, 0xFFFFFF, 1);
}

void Graphics::drawRoomPass() {
    if (rt_.bgShowColor) {
        bool ab = alphaBlend_;
        setAlphaBlend(false);
        int sh = shader_;
        r_.setShader(-1);
        drawRect(projX_ - 1, projY_ - 1, projX_ + projW_ + 1, projY_ + projH_ + 1, false, rt_.bgColor, rt_.bgColor,
                 rt_.bgColor, rt_.bgColor, 1);
        r_.setShader(sh);
        setAlphaBlend(ab);
    }
    drawRoomBackgrounds(false);
    runDrawEvent(DRAW_BEGIN, true);

    struct Item {
        double depth;
        int kind;  // 0 instance, 1 tile, 2 particle system
        uint64_t order;
        void* ptr;
        int index;
        int32_t layer = -1;
    };
    std::vector<Item> items;
    items.reserve(rt_.instances.size() + rt_.tiles.size());
    // Instances on a GMS2 layer are drawn by it; the rest on their own, by depth.
    for (Instance* i : rt_.instances)
        if (Runner::live(i) && i->visible && i->layerElement < 0) items.push_back({i->depth, 0, i->createOrder, i, 0});
    for (size_t t = 0; t < rt_.tiles.size(); t++)
        if (rt_.tiles[t].visible) items.push_back({rt_.tiles[t].depth, 1, t, nullptr, static_cast<int>(t)});
    // Layers of equal depth: the later one is drawn first (GameMaker's sorted layer list).
    for (size_t l = 0; l < rt_.layers.size(); l++) {
        const Layer& L = rt_.layers[l];
        if (L.visible) items.push_back({L.depth, 3, ~L.order, nullptr, static_cast<int>(l), L.id});
    }
    if (rt_.particles)
        for (size_t s = 0; s < rt_.particles->systems.size(); s++) {
            auto& sys = rt_.particles->systems[s];
            const Layer* L = sys.layer >= 0 ? rt_.layerById(sys.layer) : nullptr;
            if (sys.live && sys.autoDraw && !sys.empty() && (sys.layer < 0 || (L && L->visible)))
                items.push_back({sys.depth, 2, s, nullptr, static_cast<int>(s)});
        }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.depth != b.depth) return a.depth > b.depth;
        if (a.kind != b.kind) return a.kind > b.kind;
        return a.order < b.order;
    });
    for (auto& it : items) {
        if (it.kind == 0) {
            Instance* inst = static_cast<Instance*>(it.ptr);
            if (!Runner::live(inst) || !inst->visible) continue;
            if (!rt_.performEvent(inst, EV_DRAW, DRAW_NORMAL)) drawSelf(inst);
        } else if (it.kind == 1) {
            if (static_cast<size_t>(it.index) < rt_.tiles.size()) drawTile(rt_.tiles[it.index]);
        } else if (it.kind == 3) {
            drawLayer(it.layer, DRAW_NORMAL);
        } else {
            rt_.particles->draw(it.index);
        }
    }
    drawRoomBackgrounds(true);
    runDrawEvent(DRAW_END, true);
}

void Graphics::drawFrame() {
    r_.resetStats();
    ensureBackbuffer();
    ensureAppSurface();
    targetStack_.clear();

    bindAndSetup(backbuffer_, winW_, winH_);
    r_.clear(0, 1);
    runDrawEvent(DRAW_PRE, true);

    int app = surfaces_[appSurface_];
    if (rt_.dw.gen.gms2) {
        restoreState({app, 0, 0, appW_, appH_, 0, 0, static_cast<double>(appW_), static_cast<double>(appH_), 0});
        r_.clear(0, 1);
    }
    if (rt_.viewsEnabled) {
        double psx, psy;
        portScale(psx, psy);
        for (int v = 0; v < 8; v++) {
            const View& view = rt_.views[v];
            if (!view.visible) continue;
            rt_.viewCurrent = v;
            double vx = view.xview, vy = view.yview, vw = view.wview, vh = view.hview, va = view.angle;
            const Camera* cam = rt_.cameraForView(v);
            if (cam) {
                vx = cam->x;
                vy = cam->y;
                vw = cam->w;
                vh = cam->h;
                va = cam->angle;
            }
            TargetState st;
            if (surfaceExists(view.surface)) {
                // view_surface_id: the view fills that surface; the port position is ignored.
                int t = surfaces_[view.surface];
                st = {t, 0, 0, r_.targetWidth(t), r_.targetHeight(t), vx, vy, vw, vh, va};
            } else {
                st = {app, static_cast<int>(view.xport * psx), static_cast<int>(view.yport * psy),
                      static_cast<int>(view.wport * psx), static_cast<int>(view.hport * psy), vx, vy, vw, vh, va};
            }
            if (cam && (cam->customView || cam->customProj)) {
                st.custom = true;
                rt_.cameraMatrices(*cam, st.view.data(), st.proj.data());
            }
            restoreState(st);
            rt_.activeCamera = cam ? view.camera : -1;
            drawRoomPass();
        }
        rt_.viewCurrent = 0;
    } else {
        rt_.viewCurrent = 0;
        restoreState({app, 0, 0, appW_, appH_, 0, 0, rt_.roomWidth, rt_.roomHeight, 0});
        rt_.activeCamera = rt_.defaultCamera;
        drawRoomPass();
    }
    rt_.activeCamera = -1;  // none outside the room's draw (YoYo: SetActiveCamera(-1))
    while (!targetStack_.empty()) surfaceResetTarget();

    bindAndSetup(backbuffer_, winW_, winH_);
    if (appSurfaceAutoDraw) {
        bool ab = alphaBlend_;
        int sh = shader_;
        r_.setShader(-1);
        setAlphaBlend(false);
        drawTexRect(r_.targetTexture(app), appW_, appH_, 0, 0, appW_, appH_, 0, 0, winW_, winH_, 0, 0, 1, 1, 0,
                    0xFFFFFF, 1);
        setAlphaBlend(ab);
        r_.setShader(sh);
    }
    runDrawEvent(DRAW_POST, true);
    bindAndSetup(backbuffer_, winW_, winH_);
    if (guiScaleX_ > 0 && guiScaleY_ > 0) {
        restoreState({backbuffer_, 0, 0, winW_, winH_, -guiOffX_ / guiScaleX_, -guiOffY_ / guiScaleY_,
                      winW_ / guiScaleX_, winH_ / guiScaleY_, 0});
    } else if (guiW_ > 0 && guiH_ > 0) {
        restoreState({backbuffer_, 0, 0, winW_, winH_, 0, 0, guiW_, guiH_, 0});
    }
    runDrawEvent(DRAW_GUI_BEGIN, true);
    runDrawEvent(DRAW_GUI, true);
    runDrawEvent(DRAW_GUI_END, true);
    while (!targetStack_.empty()) surfaceResetTarget();
    r_.flush();
}

void Graphics::present(int screenW, int screenH) {
    if (!enabled_ || !r_.targetExists(backbuffer_)) return;
    screenW_ = screenW;
    screenH_ = screenH;
    int sh = shader_;
    bool ab = alphaBlend_;
    r_.setShader(-1);
    r_.setFog(false, 0);
    r_.setAlphaTest(false, 0);
    r_.setColorMask(true, true, true, true);
    bindAndSetup(-1, screenW, screenH);
    r_.bindTarget(-1, screenW, screenH);
    r_.setViewport(0, 0, screenW, screenH);
    r_.setProjection(0, 0, screenW, screenH);
    r_.clear(windowColor, 1);
    setAlphaBlend(false);

    bool rotated = presentRotation == 90 || presentRotation == 270;
    double cw = rotated ? winH_ : winW_, ch = rotated ? winW_ : winH_;
    double scale = std::min(screenW / cw, screenH / ch);
    if (presentIntegerScale && scale >= 1) scale = std::floor(scale);
    double dw = cw * scale, dh = ch * scale;
    presX_ = std::floor((screenW - dw) / 2);
    presY_ = std::floor((screenH - dh) / 2);
    presW_ = dw;
    presH_ = dh;
    r_.setFiltering(presentLinear);
    double cx = presX_ + dw / 2, cy = presY_ + dh / 2;
    double ww = winW_ * scale, hh = winH_ * scale;
    drawTexRect(r_.targetTexture(backbuffer_), winW_, winH_, 0, 0, winW_, winH_, -ww / 2, -hh / 2, ww, hh, cx, cy, 1, 1,
                presentRotation, 0xFFFFFF, 1);
    if (overlayCursor) {
        // An arrow pointing up-left: a black outline under a white body, sized to the screen.
        double sx, sy;
        windowToScreen(overlayCursorX, overlayCursorY, sx, sy);
        double u = std::max(1.0, screenH / 360.0);
        auto tri = [&](double ax, double ay, double bx, double by, double cx2, double cy2, uint32_t col) {
            const double px[4] = {sx + ax * u, sx + bx * u, sx + cx2 * u, sx + cx2 * u};
            const double py[4] = {sy + ay * u, sy + by * u, sy + cy2 * u, sy + cy2 * u};
            uint32_t c = packColor(col, 1);
            const uint32_t cs[4] = {c, c, c, c};
            drawQuad(r_.whiteTexture(), px, py, 0, 0, 1, 1, cs);
        };
        setAlphaBlend(true);
        tri(-1, -1.5, -1, 13, 10, 10, 0x000000);
        tri(0, 0, 0, 10.5, 7.5, 8, 0xFFFFFF);
        setAlphaBlend(false);
    }
    if (screenOverlay) {
        setAlphaBlend(true);
        r_.setFiltering(false);
        screenOverlay(r_, screenW, screenH);
        r_.flush();
        r_.setTexture(r_.whiteTexture());
        setAlphaBlend(false);
    }
    r_.flush();
    r_.setFiltering(false);
    setAlphaBlend(ab);
    r_.setShader(sh);
    dropIdlePages();
}

void Graphics::screenToWindow(double sx, double sy, double& wx, double& wy) const {
    double nx = (sx - presX_) / presW_, ny = (sy - presY_) / presH_;
    if (presentRotation == 90) {
        double t = nx;
        nx = 1 - ny;
        ny = t;
    } else if (presentRotation == 270) {
        double t = nx;
        nx = ny;
        ny = 1 - t;
    }
    wx = nx * winW_;
    wy = ny * winH_;
}

void Graphics::windowToScreen(double wx, double wy, double& sx, double& sy) const {
    double nx = winW_ ? wx / winW_ : 0, ny = winH_ ? wy / winH_ : 0;
    if (presentRotation == 90) {
        double t = ny;
        ny = 1 - nx;
        nx = t;
    } else if (presentRotation == 270) {
        double t = nx;
        nx = 1 - ny;
        ny = t;
    }
    sx = presX_ + nx * presW_;
    sy = presY_ + ny * presH_;
}

void Graphics::windowToView(int view, double wx, double wy, double& rx, double& ry) const {
    double ax = winW_ ? wx * appW_ / winW_ : wx, ay = winH_ ? wy * appH_ / winH_ : wy;
    rx = ax;
    ry = ay;
    if (view < 0 || view >= 8) return;
    const View& v = rt_.views[view];
    if (v.wport <= 0 || v.hport <= 0) return;
    double vx = v.xview, vy = v.yview, vw = v.wview, vh = v.hview;
    if (const Camera* c = const_cast<Runner&>(rt_).cameraForView(view)) {
        vx = c->x;
        vy = c->y;
        vw = c->w;
        vh = c->h;
    }
    double psx, psy;
    portScale(psx, psy);
    rx = vx + (ax - v.xport * psx) * vw / (v.wport * psx);
    ry = vy + (ay - v.yport * psy) * vh / (v.hport * psy);
}

void Graphics::windowToRoom(double wx, double wy, double& rx, double& ry) const {
    if (rt_.viewsEnabled)
        for (int i = 0; i < 8; i++) {
            const View& v = rt_.views[i];
            if (!v.visible || v.wport <= 0 || v.hport <= 0) continue;
            windowToView(i, wx, wy, rx, ry);
            return;
        }
    rx = winW_ ? wx * appW_ / winW_ : wx;
    ry = winH_ ? wy * appH_ / winH_ : wy;
}

void Graphics::setGuiSize(double w, double h) {
    guiW_ = w > 0 ? w : 0;
    guiH_ = h > 0 ? h : 0;
    guiScaleX_ = guiScaleY_ = 0;
}
void Graphics::setGuiMaximize(double xscale, double yscale, double xoffset, double yoffset) {
    guiScaleX_ = xscale > 0 ? xscale : 1;
    guiScaleY_ = yscale > 0 ? yscale : 1;
    guiOffX_ = xoffset;
    guiOffY_ = yoffset;
    guiW_ = guiH_ = 0;
}
double Graphics::guiWidth() const {
    if (guiScaleX_ > 0) return winW_ / guiScaleX_;
    return guiW_ > 0 ? guiW_ : winW_;
}
double Graphics::guiHeight() const {
    if (guiScaleY_ > 0) return winH_ / guiScaleY_;
    return guiH_ > 0 ? guiH_ : winH_;
}
void Graphics::windowToGui(double wx, double wy, double& gx, double& gy) const {
    if (guiScaleX_ > 0 && guiScaleY_ > 0) {
        gx = (wx - guiOffX_) / guiScaleX_;
        gy = (wy - guiOffY_) / guiScaleY_;
        return;
    }
    gx = winW_ ? wx * guiWidth() / winW_ : wx;
    gy = winH_ ? wy * guiHeight() / winH_ : wy;
}

bool Graphics::readSurface(int id, int x, int y, int w, int h, std::vector<uint8_t>& rgba) {
    if (!surfaceExists(id) || w <= 0 || h <= 0) return false;
    int t = surfaces_[id];
    x = std::max(0, x);
    y = std::max(0, y);
    w = std::min(w, r_.targetWidth(t) - x);
    h = std::min(h, r_.targetHeight(t) - y);
    if (w <= 0 || h <= 0) return false;
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    if (!enabled_) return true;
    TargetState saved = currentState();
    r_.bindTarget(t);
    r_.readPixels(x, y, w, h, rgba.data());
    restoreState(saved);
    return true;
}

bool Graphics::readCurrentTarget(int x, int y, int w, int h, std::vector<uint8_t>& rgba) {
    int tw = r_.targetWidth(curTarget_), th = r_.targetHeight(curTarget_);
    if (x < 0 || y < 0 || x + w > tw || y + h > th || w <= 0 || h <= 0) return false;
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    if (enabled_) r_.readPixels(x, y, w, h, rgba.data());
    return true;
}

bool Graphics::writeSurface(int id, const uint8_t* rgba, int w, int h) {
    if (!surfaceExists(id) || !enabled_) return false;
    uint32_t tex = r_.createTexture(w, h, rgba);
    TargetState saved = currentState();
    int t = surfaces_[id];
    bindAndSetup(t, r_.targetWidth(t), r_.targetHeight(t));
    bool ab = alphaBlend_;
    int sh = shader_;
    r_.setShader(-1);
    setAlphaBlend(false);  // the pixels replace what is there, alpha included
    drawTexRect(tex, w, h, 0, 0, w, h, 0, 0, w, h, 0, 0, 1, 1, 0, 0xFFFFFF, 1);
    r_.flush();
    setAlphaBlend(ab);
    r_.setShader(sh);
    restoreState(saved);
    r_.deleteTexture(tex);
    return true;
}

int Graphics::addPage(int w, int h, std::vector<uint8_t> rgba) {
    Page p;
    p.w = w;
    p.h = h;
    if (enabled_) p.tex = r_.createTexture(w, h, rgba.data());
    p.pixels = std::move(rgba);
    pages_.push_back(std::move(p));
    return static_cast<int>(pages_.size() - 1);
}

bool Graphics::readBackbuffer(std::vector<uint8_t>& rgba, int& w, int& h) {
    if (!enabled_ || !r_.targetExists(backbuffer_)) return false;
    w = winW_;
    h = winH_;
    rgba.resize(static_cast<size_t>(w) * h * 4);
    TargetState saved = currentState();
    r_.bindTarget(backbuffer_);
    r_.readPixels(0, 0, w, h, rgba.data());
    restoreState(saved);
    return true;
}

}  // namespace gm
