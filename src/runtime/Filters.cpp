// Layer filters (GameMaker 2022.1+ effects), as YoYo's yyFilterHost: the game ships each filter
// as a shader named "<filter>_shader" plus a JSON description (FEDS chunk, or the global
// "<filter>_info"). A filter on an effect layer runs over everything drawn up to that layer; a
// filter set on another layer draws that layer into a scratch surface and composites it through
// the shader (premultiplied alpha). Filters run in the main Draw event only.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "Graphics.h"
#include "Runner.h"
#include "runtime/Builtins.h"

namespace gm {
namespace {

Value field(Runner& rt, const Value& st, const char* name) {
    return st.obj() ? rt.vm.getInstanceVar(st.obj()->inst, rt.vm.varIdForName(name)) : Value();
}

void identity(float* m) {
    for (int i = 0; i < 16; i++) m[i] = i % 5 == 0 ? 1.f : 0.f;
}

}  // namespace

const Graphics::EffectInfo* Graphics::effectInfo(const std::string& name) {
    auto it = effectInfos_.find(name);
    if (it != effectInfos_.end()) return it->second.get();
    std::unique_ptr<EffectInfo>& slot = effectInfos_[name];
    std::string json;
    for (auto& [n, j] : rt_.dw.effectDefs)
        if (n == name) json = std::string(j);
    if (json.empty())
        if (Value* g = rt_.vm.findGlobal(rt_.vm.varIdForName(name + "_info")); g && g->isString()) json = g->str();
    int shader = -1;
    for (size_t i = 0; i < rt_.dw.shaders.size(); i++)
        if (rt_.dw.shaders[i].name == name + "_shader") shader = static_cast<int>(i);
    if (shader < 0 || !r_.shaderValid(shader)) {
        if (r_.enabled())  // headless runs compile no shaders
            rt_.vm.warnOnce("fx:" + name, "layer filter " + name + ": no compiled shader " + name + "_shader");
        return nullptr;
    }
    auto info = std::make_unique<EffectInfo>();
    info->shader = shader;
    bool ok = false;
    Value root = json.empty() ? Value() : jsonToValue(rt_, json, ok);
    if (ok)
        if (GMArray* params = field(rt_, root, "parameters").arr(); params && !params->rows.empty())
            for (const Value& p : params->rows[0]) {
                EffectParam ep;
                ep.name = field(rt_, p, "name").toString();
                std::string type = field(rt_, p, "type").toString();
                ep.type = type == "int" ? 1 : type == "bool" ? 2 : type == "sampler" ? 3 : 0;
                ep.defaults = field(rt_, p, "defaults");
                if (ep.defaults.isUndefined()) ep.defaults = field(rt_, p, "default");
                Value conv = field(rt_, p, "convertValueToTimeModulated");
                ep.convert = conv.isNumeric() ? conv.num() : 0;
                std::string options = field(rt_, p, "options").toString();
                ep.repeat = options.find("repeat") != std::string::npos;
                ep.filter = options.find("filter") != std::string::npos;
                info->params.push_back(std::move(ep));
            }
    slot = std::move(info);
    return slot.get();
}

int Graphics::acquireTempTarget(int w, int h, int avoid) {
    for (int t : tempTargets_)
        if (t != avoid && r_.targetExists(t) && r_.targetWidth(t) == w && r_.targetHeight(t) == h &&
            std::find(tempInUse_.begin(), tempInUse_.end(), t) == tempInUse_.end()) {
            tempInUse_.push_back(t);
            return t;
        }
    int t = r_.createTarget(w, h);
    tempTargets_.push_back(t);
    tempInUse_.push_back(t);
    return t;
}

// The whole bound target covered by `tex` (identity matrices: clip space, row 0 at the top).
void Graphics::filterQuad(uint32_t tex) {
    const double px[4] = {-1, 1, 1, -1}, py[4] = {-1, -1, 1, 1};
    const uint32_t c[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    drawQuad(tex, px, py, 0, 0, 1, 1, c);
}

Graphics::FilterPass Graphics::beginLayerFilter(const Layer& L) {
    FilterPass pass;
    std::string name;
    bool single = false;
    if (!L.fxEnabled || L.fx.isUndefined() || !fxInfo(L.fx, name, single)) return pass;
    const EffectInfo* info = effectInfo(name);
    if (!info) return pass;
    pass.active = true;
    pass.single = single;
    pass.fx = L.fx;
    pass.info = info;
    pass.saved = currentState();
    pass.gpu = gpuState();
    if (single) {  // the layer goes into a cleared scratch surface of the same size
        int w = r_.targetWidth(curTarget_), h = r_.targetHeight(curTarget_);
        pass.temp = acquireTempTarget(w, h, curTarget_);
        TargetState t = pass.saved;
        t.target = pass.temp;
        restoreState(t);
        r_.clear(0, 0);
        // Normal blending would write the layer's alpha twice; keep it premultiplied instead.
        if (blendSrc_ == 5 && blendDst_ == 6 && blendSrcA_ == 5 && blendDstA_ == 6) setBlendModeExtSepAlpha(5, 6, 8, 2);
    }
    return pass;
}

void Graphics::endLayerFilter(FilterPass& pass) {
    if (!pass.active) return;
    const EffectInfo& info = *pass.info;
    int prevShader = shader_;
    float world[16];
    std::memcpy(world, r_.worldMatrix(), sizeof world);
    if (pass.single) {
        setGpuState(pass.gpu);
        restoreState(pass.saved);
    }
    TargetState st = currentState();
    int target = st.target, tw = r_.targetWidth(target), th = r_.targetHeight(target);
    int scratch = -1;
    uint32_t source;
    if (pass.single) {
        source = r_.targetTexture(pass.temp);
        restoreState({target, 0, 0, tw, th, 0, 0, static_cast<double>(tw), static_cast<double>(th), 0});
        r_.setScissor(st.vpX, st.vpY, st.vpW, st.vpH);  // only the view being drawn
    } else {
        source = r_.targetTexture(target);
        scratch = acquireTempTarget(tw, th, target);
        restoreState({scratch, 0, 0, tw, th, 0, 0, static_cast<double>(tw), static_cast<double>(th), 0});
    }
    float id[16];
    identity(id);
    setViewProjection(id, id);
    r_.setWorld(id);
    setZTest(false);
    setZWrite(false);
    setCullMode(0);
    shaderSet(info.shader);
    if (pass.single) {
        setBlendModeExtSepAlpha(2, 6, 2, 6);  // premultiplied: one, inv_src_alpha
        setAlphaBlend(true);
    } else {
        setAlphaBlend(false);
    }

    // Common uniforms, then the filter's parameters from the fx struct (or their defaults).
    const void* key = pass.fx.obj();
    double now = rt_.currentTimeMs();
    auto started = fxStart_.try_emplace(key, now).first;
    float t = static_cast<float>((now - started->second) / 1000.0);
    auto set = [&](const char* name, std::initializer_list<float> v) {
        int loc = shaderUniform(info.shader, name);
        if (loc >= 0) shaderUniformF(loc, v.begin(), static_cast<int>(v.size()));
    };
    double camX = 0, camY = 0;
    if (const Camera* c = rt_.activeCamera >= 0 && static_cast<size_t>(rt_.activeCamera) < rt_.cameras.size()
                              ? &rt_.cameras[rt_.activeCamera] : nullptr) {
        camX = c->x;
        camY = c->y;
    }
    set("gm_vTime", {t});
    set("gm_pTime", {t});
    set("gm_pSurfaceDimensions", {static_cast<float>(tw), static_cast<float>(th)});
    set("gm_pSurfaceTexelSize", {tw > 0 ? 1.f / tw : 0.f, th > 0 ? 1.f / th : 0.f});
    set("gm_pCamOffset", {static_cast<float>(camX), static_cast<float>(camY)});
    set("gm_pPreMultiplyAlpha", {pass.single ? 1.f : 0.f});
    for (const EffectParam& p : info.params) {
        Value v = field(rt_, pass.fx, p.name.c_str());
        if (v.isUndefined()) v = p.defaults;
        if (p.type == 3) {  // sampler: a sprite (index or name) bound to the sampler's stage
            int sprite = -1;
            if (v.isString()) {
                for (size_t i = 0; i < rt_.dw.sprites.size(); i++)
                    if (rt_.dw.sprites[i].name == v.str()) sprite = static_cast<int>(i);
            } else if (v.isNumeric()) {
                sprite = v.toInt();
            }
            uint32_t tex;
            int w, h;
            int stage = r_.samplerStage(info.shader, p.name);
            if (sprite < 0 || stage < 0 || !textureInfo(spriteTexture(sprite, 0), tex, w, h)) continue;
            r_.setTextureParams(tex, p.repeat, p.filter);
            r_.setTextureStage(stage, tex);
            const Sprite& sp = rt_.dw.sprites[sprite];
            set((p.name + "Dimensions").c_str(), {static_cast<float>(sp.width), static_cast<float>(sp.height)});
            set((p.name + "TexelSize").c_str(),
                {sp.width ? 1.f / sp.width : 0.f, sp.height ? 1.f / sp.height : 0.f});
            continue;
        }
        std::vector<float> vals;
        if (GMArray* a = v.arr()) {
            if (!a->rows.empty())
                for (const Value& e : a->rows[0]) vals.push_back(static_cast<float>(e.num()));
        } else if (v.isNumeric()) {
            vals.push_back(static_cast<float>(v.num()));
        }
        if (vals.empty()) continue;
        if (p.convert > 0)  // a speed turned into a 0..1 phase of the effect's time
            for (float& f : vals)
                if (std::fabs(f) > 0) {
                    double period = p.convert / std::fabs(f);
                    f = static_cast<float>(std::fmod(t, period) / period * (f < 0 ? -1 : 1));
                }
        int loc = shaderUniform(info.shader, p.name);
        if (loc >= 0) shaderUniformArray(loc, vals.data(), static_cast<int>(vals.size()));
    }
    filterQuad(source);

    if (!pass.single) {  // the filtered copy goes back over the target, unblended
        shaderReset();
        restoreState({target, 0, 0, tw, th, 0, 0, static_cast<double>(tw), static_cast<double>(th), 0});
        setViewProjection(id, id);
        setAlphaBlend(false);
        filterQuad(r_.targetTexture(scratch));
    }
    // Back to how the layer left things.
    if (prevShader >= 0) shaderSet(prevShader);
    else shaderReset();
    r_.setWorld(world);
    setGpuState(pass.gpu);
    restoreState(st);
    tempInUse_.erase(std::remove(tempInUse_.begin(), tempInUse_.end(), pass.temp), tempInUse_.end());
    tempInUse_.erase(std::remove(tempInUse_.begin(), tempInUse_.end(), scratch), tempInUse_.end());
}

}  // namespace gm
