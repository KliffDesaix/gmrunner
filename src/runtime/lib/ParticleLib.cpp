#include <map>
// part_system_*, part_emitter_*, part_type_*, part_particles_* (following YoYo's runner,
// yyParticle.js) and the legacy drag-and-drop action_* functions.
#include <algorithm>
#include <array>
#include <cmath>

#include "runtime/Audio.h"
#include "runtime/Builtins.h"
#include "runtime/Graphics.h"
#include "runtime/Stores.h"

namespace gm {

enum { COL_ONE, COL_TWO, COL_THREE, COL_RGB, COL_HSV, COL_MIX };
enum { DISTR_LINEAR, DISTR_GAUSSIAN, DISTR_INVGAUSSIAN };
enum { ESHAPE_RECTANGLE, ESHAPE_ELLIPSE, ESHAPE_DIAMOND, ESHAPE_LINE };

namespace {

uint32_t mergeColor(uint32_t a, uint32_t b, double t) {
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        double ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= (static_cast<uint32_t>(std::lround(ca * (1 - t) + cb * t)) & 0xFF) << sh;
    }
    return out;
}

uint32_t multiplyColor(uint32_t a, uint32_t b) {
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) out |= ((((a >> sh) & 0xFF) * ((b >> sh) & 0xFF) / 255) & 0xFF) << sh;
    return out;
}

uint32_t hsvColor(double h, double s, double v) {  // make_color_hsv, 0-255 ranges
    h = h / 255.0 * 360.0;
    s /= 255.0;
    v /= 255.0;
    double c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1)), m = v - c;
    double r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else { r = c; b = x; }
    auto to8 = [](double f) { return static_cast<uint32_t>(std::lround(std::max(0.0, std::min(1.0, f)) * 255)); };
    return to8(r + m) | (to8(g + m) << 8) | (to8(b + m) << 16);
}

double vectorToDirection(double h, double v) {
    double dir;
    if (h == 0) dir = v > 0 ? 270 : (v < 0 ? 90 : 0);
    else {
        double dd = 180.0 * std::atan2(v, h) / M_PI;
        dir = dd <= 0 ? -dd : 360.0 - dd;
    }
    return dir - 360.0 * std::floor(dir / 360.0);
}

double wiggle(double age, double ran, double ranMul, double period, double div) {  // a -1..1 triangle wave
    double r = std::fmod(age + ranMul * ran, period) / div;
    if (r > 2.0) r = 4.0 - r;
    return r - 1.0;
}

}  // namespace

// The particle types of the particle system assets are made at start, one per PSEM emitter in
// order, so the game's own part_type_create returns the ids after them (as YoYo's runner).
ParticleStore::ParticleStore(Runner& r) : rt(r), emitterLists(r.dw.hasChunk("PSEM")) {
    for (const PsEmitter& e : rt.dw.psEmitters) {
        ParticleType t;
        t.sprite = e.sprite;
        t.spriteStart = static_cast<int>(e.frameIndex);
        t.spriteAnim = e.animate;
        t.spriteStretch = e.stretch;
        t.spriteRandom = e.randomFrame;
        t.shape = e.texture;
        t.sizeMinX = e.sizeMinX, t.sizeMaxX = e.sizeMaxX, t.sizeMinY = e.sizeMinY, t.sizeMaxY = e.sizeMaxY;
        t.sizeIncrX = e.sizeIncX, t.sizeIncrY = e.sizeIncY, t.sizeRandX = e.sizeWiggleX, t.sizeRandY = e.sizeWiggleY;
        t.xscale = e.scaleX, t.yscale = e.scaleY;
        t.lifeMin = static_cast<int>(e.lifeMin), t.lifeMax = static_cast<int>(e.lifeMax);
        // spawn emitters are PSEM indices, which are these types' ids
        t.deathType = e.spawnOnDeath, t.deathNumber = e.spawnOnDeath >= 0 ? e.spawnOnDeathCount : 0;
        t.stepType = e.spawnOnUpdate, t.stepNumber = e.spawnOnUpdate >= 0 ? e.spawnOnUpdateCount : 0;
        t.spMin = e.speedMin, t.spMax = e.speedMax, t.spIncr = e.speedInc, t.spRand = e.speedWiggle;
        t.dirMin = e.dirMin, t.dirMax = e.dirMax, t.dirIncr = e.dirInc, t.dirRand = e.dirWiggle;
        t.grav = e.gravity, t.gravDir = e.gravityDir;
        t.angMin = e.angMin, t.angMax = e.angMax, t.angIncr = e.angInc, t.angRand = e.angWiggle;
        t.angDir = e.angRelative;
        t.colMode = COL_THREE;
        t.colPar[0] = e.startColor & 0xFFFFFF, t.colPar[1] = e.midColor & 0xFFFFFF, t.colPar[2] = e.endColor & 0xFFFFFF;
        t.alphaStart = (e.startColor >> 24) / 255.0;
        t.alphaMiddle = (e.midColor >> 24) / 255.0;
        t.alphaEnd = (e.endColor >> 24) / 255.0;
        t.additive = e.additive;
        assetTypes.push_back(static_cast<int>(types.size()));
        types.push_back(t);
    }
}

static double randomizeTime(Runner& rt, double minv, double maxv, int unit) {
    if (minv == 0 && maxv == 0) return 0;
    if (unit == 1) {
        int a = static_cast<int>(minv), b = static_cast<int>(maxv);
        return a + static_cast<int>(rt.rng.next() * (b - a + 1));
    }
    return minv + rt.rng.next() * (maxv - minv);
}

// As YoYo's MakeInstance: the asset's emitters are added last first, streams start, and bursts
// without a delay go off at once.
int ParticleStore::instantiate(int asset) {
    if (asset < 0 || static_cast<size_t>(asset) >= rt.dw.psSystems.size()) return -1;
    const PsSystem& a = rt.dw.psSystems[asset];
    systems.emplace_back();
    int id = static_cast<int>(systems.size() - 1);
    ParticleSystem& s = systems.back();
    s.originX = a.originX;
    s.originY = a.originY;
    s.oldToNew = a.drawOrder == 0;
    for (auto it = a.emitters.rbegin(); it != a.emitters.rend(); ++it) {
        if (*it < 0 || static_cast<size_t>(*it) >= rt.dw.psEmitters.size()) continue;
        const PsEmitter& t = rt.dw.psEmitters[*it];
        int em = createEmitter(id);
        ParticleEmitter& e = systems[id].emitters[em];
        e.enabled = t.enabled;
        e.burstMode = t.mode != 0;
        e.number = t.count;
        e.relative = t.relative;
        e.distr = t.distribution;
        e.shape = t.shape;
        e.xmin = t.regionX - t.regionW * 0.5, e.xmax = t.regionX + t.regionW * 0.5;
        e.ymin = t.regionY - t.regionH * 0.5, e.ymax = t.regionY + t.regionH * 0.5;
        e.partType = assetTypes[*it];
        e.delayMin = t.delayMin, e.delayMax = t.delayMax, e.delayUnit = t.delayUnit;
        e.delayCurrent = randomizeTime(rt, t.delayMin, t.delayMax, t.delayUnit);
        e.intervalMin = t.intervalMin, e.intervalMax = t.intervalMax, e.intervalUnit = t.intervalUnit;
        e.intervalCurrent = randomizeTime(rt, t.intervalMin, t.intervalMax, t.intervalUnit);
        if (e.enabled && e.burstMode && e.delayCurrent <= 0) burst(id, em, e.partType, e.number);
    }
    return id;
}

double ParticleStore::random(double range) { return rt.rng.next() * range; }

double ParticleStore::myRandom(double minv, double maxv, int distr) {
    double range = maxv - minv;
    if (range <= 0) return minv;
    double xx;
    switch (distr) {
        case DISTR_GAUSSIAN:
            do { xx = (random(1) - 0.5) * 6.0; } while (std::exp(-(xx * xx) * 0.5) <= random(1));
            return minv + (xx + 3.0) * (1.0 / 6.0) * range;
        case DISTR_INVGAUSSIAN:
            do { xx = (random(1) - 0.5) * 6.0; } while (!(std::exp(-(xx * xx) * 0.5) > random(1)));
            if (xx < 0.0) xx += 6.0;
            return minv + xx * (1.0 / 6.0) * range;
        default: return minv + random(1) * range;
    }
}

void ParticleStore::computeColor(Particle& p) {
    if (p.age <= 0 || p.lifetime <= 0) {
        switch (p.colMode) {
            case COL_RGB: {
                int r = static_cast<int>(myRandom(p.colPar[0], p.colPar[1], DISTR_LINEAR));
                int g = static_cast<int>(myRandom(p.colPar[2], p.colPar[3], DISTR_LINEAR));
                int b = static_cast<int>(myRandom(p.colPar[4], p.colPar[5], DISTR_LINEAR));
                p.color = static_cast<uint32_t>((b & 0xFF) << 16 | (g & 0xFF) << 8 | (r & 0xFF));
                break;
            }
            case COL_HSV: {
                int h = static_cast<int>(myRandom(p.colPar[0], p.colPar[1], DISTR_LINEAR));
                int sat = static_cast<int>(myRandom(p.colPar[2], p.colPar[3], DISTR_LINEAR));
                int v = static_cast<int>(myRandom(p.colPar[4], p.colPar[5], DISTR_LINEAR));
                p.color = hsvColor(h, sat, v);
                break;
            }
            case COL_MIX:
                p.color = mergeColor(static_cast<uint32_t>(p.colPar[0]), static_cast<uint32_t>(p.colPar[1]), random(1));
                break;
            default: p.color = static_cast<uint32_t>(p.colPar[0]); break;
        }
        return;
    }
    switch (p.colMode) {
        case COL_ONE: p.color = static_cast<uint32_t>(p.colPar[0]); break;
        case COL_TWO:
            p.color = mergeColor(static_cast<uint32_t>(p.colPar[0]), static_cast<uint32_t>(p.colPar[1]),
                                 std::min(1.0, p.age / p.lifetime));
            break;
        case COL_THREE: {
            double v = std::min(2.0, 2.0 * p.age / p.lifetime);
            p.color = v < 1 ? mergeColor(static_cast<uint32_t>(p.colPar[0]), static_cast<uint32_t>(p.colPar[1]), v)
                            : mergeColor(static_cast<uint32_t>(p.colPar[1]), static_cast<uint32_t>(p.colPar[2]), v - 1);
            break;
        }
        default: break;  // RGB / HSV / mix keep the colour picked at creation
    }
}

std::vector<Particle>& ParticleStore::listFor(ParticleSystem& s, int em) {
    if (emitterLists && em >= 0 && static_cast<size_t>(em) < s.emitters.size()) return s.emitters[em].particles;
    return s.particles;
}

int ParticleStore::createEmitter(int sys) {
    ParticleSystem* s = system(sys);
    if (!s) return -1;
    for (size_t i = 0; i < s->emitters.size(); i++)
        if (!s->emitters[i].created && !s->emitters[i].zombie) {
            std::vector<Particle> keep = std::move(s->emitters[i].particles);
            s->emitters[i] = ParticleEmitter();
            s->emitters[i].particles = std::move(keep);
            return static_cast<int>(i);
        }
    s->emitters.emplace_back();
    return static_cast<int>(s->emitters.size() - 1);
}

void ParticleStore::emit(int sys, int em, double x, double y, int typeId, int number, bool colored, uint32_t color) {
    ParticleSystem* s = system(sys);
    if (!s) return;
    for (int i = 0; i < number; i++) {
        ParticleType* t = type(typeId);
        if (!t) return;
        Particle p;
        p.type = typeId;
        p.x = x;
        p.y = y;
        p.speed = myRandom(t->spMin, t->spMax, DISTR_LINEAR);
        p.dir = myRandom(t->dirMin, t->dirMax, DISTR_LINEAR);
        p.ang = myRandom(t->angMin, t->angMax, DISTR_LINEAR);
        p.lifetime = myRandom(t->lifeMin, t->lifeMax, DISTR_LINEAR);
        p.colMode = t->colMode;
        std::copy(std::begin(t->colPar), std::end(t->colPar), p.colPar);
        computeColor(p);
        p.alpha = t->alphaStart;
        double rs = t->sizeMaxX > t->sizeMinX || t->sizeMaxY > t->sizeMinY ? random(1) : 0;  // one draw for both
        p.xsize = t->sizeMinX + (t->sizeMaxX - t->sizeMinX) * rs;
        p.ysize = t->sizeMinY + (t->sizeMaxY - t->sizeMinY) * rs;
        p.spriteStart = t->spriteRandom ? random(10000) : t->spriteStart;
        p.subimg = p.spriteStart;
        p.ran = random(100000);
        if (colored) {
            p.colMode = COL_ONE;
            p.colPar[0] = color;
            p.color = color;
        }
        // Re-fetch: computeColor and the RNG don't touch the list, but spawning can grow it.
        if (ParticleSystem* again = system(sys)) listFor(*again, em).push_back(p);
    }
}

void ParticleStore::create(int sys, double x, double y, int typeId, int number, bool colored, uint32_t color) {
    ParticleSystem* s = system(sys);
    if (!s || !type(typeId)) return;
    int em = -1;
    if (emitterLists) {
        for (size_t i = 0; i < s->emitters.size() && em < 0; i++)
            if (s->emitters[i].enabled && s->emitters[i].created) em = static_cast<int>(i);
        if (em < 0) em = createEmitter(sys);
    }
    emit(sys, em, x, y, typeId, number, colored, color);
}

void ParticleStore::burstRegion(int sys, int em, double x, double y, double w, double h, int shape, int distr,
                                int typeId, double number) {
    ParticleEmitter* e = emitter(sys, em);
    if (e && e->relative) number = w * h * number * 0.00003;
    if (number < 0) {
        if (static_cast<int>(random(-number)) != 0) return;
        number = 1;
    }
    double whole = std::trunc(number), fract = number - whole;
    int n = static_cast<int>(whole);
    if (fract > 0.0 && random(1) <= fract) n++;
    for (int i = 0; i < n; i++) {
        double xx, yy;
        for (;;) {
            xx = myRandom(0.0, 1.0, distr);
            yy = myRandom(0.0, 1.0, distr);
            if (distr == DISTR_INVGAUSSIAN && shape != ESHAPE_LINE) {
                if (random(1) < 0.5) xx = myRandom(0.0, 1.0, DISTR_LINEAR);
                else yy = myRandom(0.0, 1.0, DISTR_LINEAR);
            }
            if (shape == ESHAPE_ELLIPSE) {
                if ((xx - 0.5) * (xx - 0.5) + (yy - 0.5) * (yy - 0.5) <= 0.25) break;
            } else if (shape == ESHAPE_DIAMOND) {
                if (std::fabs(xx - 0.5) + std::fabs(yy - 0.5) <= 0.5) break;
            } else {
                break;
            }
        }
        double px = shape == ESHAPE_LINE ? x + w * xx : x + w * xx;
        double py = shape == ESHAPE_LINE ? y + h * xx : y + h * yy;
        emit(sys, em, px, py, typeId, 1);
    }
}

void ParticleStore::burst(int sys, int em, int typeId, double number) {
    ParticleEmitter* e = emitter(sys, em);
    if (!e || !e->enabled) return;
    burstRegion(sys, em, e->xmin, e->ymin, e->xmax - e->xmin, e->ymax - e->ymin, e->shape, e->distr, typeId, number);
}

// Life (with step and death spawns), motion and shape of one particle list, as YoYo's
// HandleLife / HandleMotion / HandleShape.
void ParticleStore::handle(int sys, int em) {
    auto list = [&]() -> std::vector<Particle>* {
        ParticleSystem* s = system(sys);
        return s ? &listFor(*s, em) : nullptr;
    };
    for (size_t i = 0; list() && i < list()->size();) {
        Particle& p = (*list())[i];
        p.age++;
        ParticleType* t = type(p.type);
        if (p.age >= p.lifetime) {
            double px = p.x, py = p.y;
            list()->erase(list()->begin() + static_cast<long>(i));
            if (t) {
                int numb = t->deathNumber;
                if (numb < 0) numb = static_cast<int>(random(-numb)) == 0 ? 1 : 0;
                if (numb > 0) emit(sys, em, px, py, t->deathType, numb);
            }
            continue;
        }
        if (t) {
            int numb = t->stepNumber;
            if (numb < 0) numb = static_cast<int>(random(-numb)) == 0 ? 1 : 0;
            if (numb > 0) emit(sys, em, p.x, p.y, t->stepType, numb);
        }
        i++;
    }
    std::vector<Particle>* ps = list();
    if (!ps) return;
    for (Particle& p : *ps) {
        ParticleType* t = type(p.type);
        if (!t) continue;
        p.speed = std::max(0.0, p.speed + t->spIncr);
        p.dir += t->dirIncr;
        p.ang += t->angIncr;
        if (t->grav != 0) {
            double h = p.speed * std::cos(p.dir * M_PI / 180.0) + t->grav * std::cos(t->gravDir * M_PI / 180.0);
            double v = -p.speed * std::sin(p.dir * M_PI / 180.0) - t->grav * std::sin(t->gravDir * M_PI / 180.0);
            p.dir = vectorToDirection(h, v);
            p.speed = std::sqrt(h * h + v * v);
        }
        double rd = wiggle(p.age, p.ran, 3, 24, 6.0), rs = wiggle(p.age, p.ran, 4, 20, 5.0);
        double d = (p.dir + rd * t->dirRand) * M_PI / 180.0, sp = p.speed + rs * t->spRand;
        p.x += sp * std::cos(d);
        p.y -= sp * std::sin(d);
    }
    for (Particle& p : *ps) {
        ParticleType* t = type(p.type);
        if (!t) continue;
        p.xsize = std::max(0.0, p.xsize + t->sizeIncrX);
        p.ysize = std::max(0.0, p.ysize + t->sizeIncrY);
        computeColor(p);
        double passed = p.lifetime > 0 ? 2.0 * p.age / p.lifetime : 1;
        p.alpha = passed < 1 ? t->alphaStart * (1.0 - passed) + t->alphaMiddle * passed
                             : t->alphaMiddle * (2.0 - passed) + t->alphaEnd * (passed - 1);
        if (t->sprite >= 0 && static_cast<size_t>(t->sprite) < rt.dw.sprites.size() &&
            !rt.dw.sprites[t->sprite].frames.empty()) {
            if (!t->spriteAnim) p.subimg = p.spriteStart;
            else if (t->spriteStretch)
                p.subimg = p.spriteStart + rt.dw.sprites[t->sprite].frames.size() * p.age / p.lifetime;
            else p.subimg += 1.0;
        }
    }
}

void ParticleStore::updateSystem(int sys) {
    ParticleSystem* s = system(sys);
    if (!s) return;
    if (!emitterLists) {
        // One shared list: age and move everything, then let the emitters stream.
        handle(sys, -1);
        for (size_t i = 0; (s = system(sys)) && i < s->emitters.size(); i++) {
            ParticleEmitter& e = s->emitters[i];
            if (e.created && e.enabled && !e.burstMode && e.number != 0) burst(sys, static_cast<int>(i), e.partType, e.number);
        }
        return;
    }
    double dtSeconds = 1.0 / (rt.roomSpeed > 0 ? rt.roomSpeed : 60);
    for (size_t i = 0; (s = system(sys)) && i < s->emitters.size(); i++) {
        if (!s->emitters[i].enabled || (!s->emitters[i].created && !s->emitters[i].zombie)) continue;
        handle(sys, static_cast<int>(i));
        if (!(s = system(sys))) return;
        ParticleEmitter& e = s->emitters[i];
        auto randomize = [&](double minv, double maxv, int unit) {
            if (minv == 0 && maxv == 0) return 0.0;
            if (unit == 1) {
                int a = static_cast<int>(minv), b = static_cast<int>(maxv);
                return static_cast<double>(a + static_cast<int>(rt.rng.next() * (b - a + 1)));
            }
            return minv + rt.rng.next() * (maxv - minv);
        };
        if (e.delayCurrent > 0.0) {
            e.delayCurrent -= e.delayUnit == 1 ? 1.0 : dtSeconds;
            if (e.delayCurrent <= 0.0) burst(sys, static_cast<int>(i), e.partType, e.number);
            continue;
        }
        if (!e.burstMode) {
            e.intervalCurrent -= e.intervalUnit == 1 ? 1.0 : dtSeconds;
            if (e.intervalCurrent <= 0.0) {
                burst(sys, static_cast<int>(i), e.partType, e.number);
                if ((s = system(sys))) {
                    ParticleEmitter& e2 = s->emitters[i];
                    e2.intervalCurrent = randomize(e2.intervalMin, e2.intervalMax, e2.intervalUnit);
                }
            }
        }
        if ((s = system(sys)) && s->emitters[i].particles.empty()) s->emitters[i].zombie = false;
    }
}

void ParticleStore::update() {
    for (size_t i = 0; i < systems.size(); i++)
        if (systems[i].live && systems[i].autoUpdate) updateSystem(static_cast<int>(i));
}

void ParticleStore::draw(int sysId, const float* world, uint32_t color, double alpha) {
    ParticleSystem* s = system(sysId);
    if (!s) return;
    Graphics& g = *rt.gfx;
    Graphics::GpuState saved = g.gpuState();
    bool additive = false;
    // A system that is turned, scaled or has an origin, or is placed by a sequence, is drawn in
    // its own space: origin, scale, rotation, then its position, then `world`.
    Renderer& r = g.renderer();
    const bool transformed = world || s->angle != 0 || s->xscale != 1 || s->yscale != 1 || s->originX != 0 ||
                             s->originY != 0;
    float savedWorld[16];
    double ox = s->x, oy = s->y;
    if (transformed) {
        std::copy(r.worldMatrix(), r.worldMatrix() + 16, savedWorld);
        double rad = s->angle * M_PI / 180.0, c = std::cos(rad), sn = std::sin(rad);
        // row vectors: x' = x * a + y * c2 + tx, y' = x * b + y * d + ty
        double a = s->xscale * c, b = -s->xscale * sn, c2 = s->yscale * sn, d = s->yscale * c;
        double tx = -(s->originX * a + s->originY * c2) + s->x, ty = -(s->originX * b + s->originY * d) + s->y;
        float local[16] = {static_cast<float>(a), static_cast<float>(b), 0, 0, static_cast<float>(c2), static_cast<float>(d), 0, 0,
                           0, 0, 1, 0, static_cast<float>(tx), static_cast<float>(ty), 0, 1};
        const float* outer = world ? world : savedWorld;
        float m[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                m[i * 4 + j] = local[i * 4] * outer[j] + local[i * 4 + 1] * outer[4 + j] + local[i * 4 + 2] * outer[8 + j] +
                               local[i * 4 + 3] * outer[12 + j];
        r.setWorld(m);
        ox = oy = 0;
    }
    uint32_t sysColor = multiplyColor(s->color, color);
    double sysAlpha = s->alpha * alpha;
    auto drawList = [&](const std::vector<Particle>& list) {
        auto drawOne = [&](const Particle& p) {
            if (p.lifetime <= 0) return;
            ParticleType* t = type(p.type);
            if (!t) return;
            if (t->additive != additive) {
                additive = t->additive;
                if (additive) g.setBlendMode(1);
                else g.setGpuState(saved);
            }
            double aa = p.ang + (t->angDir ? p.dir : 0) + wiggle(p.age, p.ran, 2, 16, 4.0) * t->angRand;
            double r = wiggle(p.age, p.ran, 1, 16, 4.0);
            double sx = p.xsize + r * t->sizeRandX, sy = p.ysize + r * t->sizeRandY;
            uint32_t col = multiplyColor(sysColor, p.color);
            double palpha = sysAlpha * p.alpha;
            bool sprite = t->sprite >= 0 && static_cast<size_t>(t->sprite) < rt.dw.sprites.size() &&
                          !rt.dw.sprites[t->sprite].frames.empty();
            if (sprite) {
                g.drawSprite(t->sprite, p.subimg, p.x + ox, p.y + oy, t->xscale * sx, t->yscale * sy, aa, col, palpha);
            } else {
                g.drawParticleShape(t->shape, std::trunc(p.x + ox), std::trunc(p.y + oy), t->xscale * sx,
                                    t->yscale * sy, aa, col, palpha);
            }
        };
        if (s->oldToNew)
            for (const Particle& p : list) drawOne(p);
        else
            for (auto it = list.rbegin(); it != list.rend(); ++it) drawOne(*it);
    };
    drawList(s->particles);
    for (auto& e : s->emitters)
        if (e.enabled) drawList(e.particles);
    if (additive) g.setGpuState(saved);
    if (transformed) r.setWorld(savedWorld);
}

#define PS (*rt.particles)

// ---- systems
GMFN(part_system_create) {  // ([particle system asset])
    if (argc > 0 && !ARG(0).isUndefined()) {
        int id = PS.instantiate(I(0));
        if (id >= 0) return Value(static_cast<double>(id));
    }
    PS.systems.emplace_back();
    return Value(static_cast<double>(PS.systems.size() - 1));
}
GMFN(part_system_create_layer) {  // (layer, persistent, [asset]): drawn at the layer's depth
    int id = argc > 2 && !ARG(2).isUndefined() ? PS.instantiate(I(2)) : -1;
    if (id < 0) {
        PS.systems.emplace_back();
        id = static_cast<int>(PS.systems.size() - 1);
    }
    ParticleSystem& s = PS.systems[id];
    if (Layer* L = rt.layerFromValue(ARG(0))) {
        s.depth = L->depth;
        s.layer = L->id;
    }
    s.persistent = argc > 1 && B(1);
    return Value(static_cast<double>(id));
}
GMFN(part_system_angle) { if (ParticleSystem* s = PS.system(I(0))) s->angle = D(1); return Value(); }
GMFN(part_system_destroy) {
    if (ParticleSystem* s = PS.system(I(0))) {
        *s = ParticleSystem();
        s->live = false;
    }
    return Value();
}
GMFN(part_system_exists) { return boolV(PS.system(I(0)) != nullptr); }
GMFN(part_system_clear) {  // removes particles and emitters, keeps position and settings
    if (ParticleSystem* s = PS.system(I(0))) {
        s->particles.clear();
        s->emitters.clear();
    }
    return Value();
}
GMFN(part_system_depth) { if (ParticleSystem* s = PS.system(I(0))) s->depth = D(1); return Value(); }
GMFN(part_system_layer) {
    ParticleSystem* s = PS.system(I(0));
    if (Layer* L = rt.layerFromValue(ARG(1)); s && L) s->depth = L->depth;
    return Value();
}
GMFN(part_system_position) {
    if (ParticleSystem* s = PS.system(I(0))) {
        s->x = D(1);
        s->y = D(2);
    }
    return Value();
}
GMFN(part_system_color) {
    if (ParticleSystem* s = PS.system(I(0))) {
        s->color = C(1);
        s->alpha = D(2);
    }
    return Value();
}
GMFN(part_system_automatic_update) { if (ParticleSystem* s = PS.system(I(0))) s->autoUpdate = B(1); return Value(); }
GMFN(part_system_automatic_draw) { if (ParticleSystem* s = PS.system(I(0))) s->autoDraw = B(1); return Value(); }
GMFN(part_system_draw_order) { if (ParticleSystem* s = PS.system(I(0))) s->oldToNew = B(1); return Value(); }
GMFN(part_system_drawit) { PS.draw(I(0)); return Value(); }
GMFN(part_system_update) { PS.updateSystem(I(0)); return Value(); }
GMFN(part_particles_create) { PS.create(I(0), D(1), D(2), I(3), I(4)); return Value(); }
GMFN(part_particles_create_color) { PS.create(I(0), D(1), D(2), I(3), I(5), true, C(4)); return Value(); }
GMFN(part_particles_clear) {
    if (ParticleSystem* s = PS.system(I(0))) {
        s->particles.clear();
        for (auto& e : s->emitters) e.particles.clear();
    }
    return Value();
}
GMFN(part_particles_count) {
    ParticleSystem* s = PS.system(I(0));
    if (!s) return Value(0.0);
    size_t n = s->particles.size();
    for (auto& e : s->emitters) n += e.particles.size();
    return Value(static_cast<double>(n));
}

// ---- emitters
GMFN(part_emitter_create) { return Value(static_cast<double>(PS.createEmitter(I(0)))); }
GMFN(part_emitter_destroy) {
    if (ParticleEmitter* e = PS.emitter(I(0), I(1))) {
        e->created = false;
        e->zombie = PS.emitterLists;  // its particles live on until they die
        if (!PS.emitterLists) {
            *e = ParticleEmitter();
            e->created = false;
        }
    }
    return Value();
}
GMFN(part_emitter_destroy_all) {
    if (ParticleSystem* s = PS.system(I(0)))
        for (int i = static_cast<int>(s->emitters.size()) - 1; i >= 0; i--) fn_part_emitter_destroy(rt, self, other, 2,
            std::array<Value, 2>{ARG(0), Value(static_cast<double>(i))}.data());
    return Value();
}
GMFN(part_emitter_exists) { return boolV(PS.emitter(I(0), I(1)) != nullptr); }
GMFN(part_emitter_clear) {
    if (ParticleEmitter* e = PS.emitter(I(0), I(1))) {
        std::vector<Particle> keep = std::move(e->particles);
        *e = ParticleEmitter();
        e->particles = std::move(keep);
    }
    return Value();
}
GMFN(part_emitter_region) {  // (ps, em, xmin, xmax, ymin, ymax, shape, distribution)
    if (ParticleEmitter* e = PS.emitter(I(0), I(1))) {
        e->xmin = D(2);
        e->xmax = D(3);
        e->ymin = D(4);
        e->ymax = D(5);
        e->shape = I(6);
        e->distr = I(7);
    }
    return Value();
}
GMFN(part_emitter_burst) { PS.burst(I(0), I(1), I(2), D(3)); return Value(); }
GMFN(part_emitter_stream) {
    if (ParticleEmitter* e = PS.emitter(I(0), I(1))) {
        e->partType = I(2);
        e->number = D(3);
    }
    return Value();
}
GMFN(part_emitter_enable) { if (ParticleEmitter* e = PS.emitter(I(0), I(1))) e->enabled = B(2); return Value(); }
GMFN(part_emitter_relative) { if (ParticleEmitter* e = PS.emitter(I(0), I(1))) e->relative = B(2); return Value(); }
GMFN(part_emitter_delay) {  // (ps, em, min, max, unit)
    if (ParticleEmitter* e = PS.emitter(I(0), I(1))) {
        e->delayMin = D(2);
        e->delayMax = D(3);
        e->delayUnit = I(4);
        e->delayCurrent = randomizeTime(rt, e->delayMin, e->delayMax, e->delayUnit);
    }
    return Value();
}
GMFN(part_emitter_interval) {
    if (ParticleEmitter* e = PS.emitter(I(0), I(1))) {
        e->intervalMin = D(2);
        e->intervalMax = D(3);
        e->intervalUnit = I(4);
        e->intervalCurrent = randomizeTime(rt, e->intervalMin, e->intervalMax, e->intervalUnit);
    }
    return Value();
}

// ---- types
GMFN(part_type_create) {
    for (size_t i = 0; i < PS.types.size(); i++)
        if (!PS.types[i].live) {
            PS.types[i] = ParticleType();
            return Value(static_cast<double>(i));
        }
    PS.types.emplace_back();
    return Value(static_cast<double>(PS.types.size() - 1));
}
GMFN(part_type_destroy) { if (ParticleType* t = PS.type(I(0))) t->live = false; return Value(); }
GMFN(part_type_exists) { return boolV(PS.type(I(0)) != nullptr); }
GMFN(part_type_clear) { if (ParticleType* t = PS.type(I(0))) *t = ParticleType(); return Value(); }
#define PT_SET(body) \
    if (ParticleType* t = PS.type(I(0))) { body; } \
    return Value();
GMFN(part_type_sprite) { PT_SET(t->sprite = I(1); t->spriteAnim = B(2); t->spriteStretch = B(3); t->spriteRandom = B(4)) }
GMFN(part_type_subimage) { PT_SET(t->spriteStart = I(1)) }
GMFN(part_type_shape) { PT_SET(t->shape = I(1); t->sprite = -1) }
GMFN(part_type_size) {
    PT_SET(t->sizeMinX = t->sizeMinY = D(1); t->sizeMaxX = t->sizeMaxY = D(2); t->sizeIncrX = t->sizeIncrY = D(3);
           t->sizeRandX = t->sizeRandY = D(4))
}
GMFN(part_type_size_x) { PT_SET(t->sizeMinX = D(1); t->sizeMaxX = D(2); t->sizeIncrX = D(3); t->sizeRandX = D(4)) }
GMFN(part_type_size_y) { PT_SET(t->sizeMinY = D(1); t->sizeMaxY = D(2); t->sizeIncrY = D(3); t->sizeRandY = D(4)) }
GMFN(part_type_scale) { PT_SET(t->xscale = D(1); t->yscale = D(2)) }
GMFN(part_type_life) { PT_SET(t->lifeMin = I(1); t->lifeMax = I(2)) }
GMFN(part_type_speed) { PT_SET(t->spMin = D(1); t->spMax = D(2); t->spIncr = D(3); t->spRand = D(4)) }
GMFN(part_type_direction) { PT_SET(t->dirMin = D(1); t->dirMax = D(2); t->dirIncr = D(3); t->dirRand = D(4)) }
GMFN(part_type_orientation) {
    PT_SET(t->angMin = D(1); t->angMax = D(2); t->angIncr = D(3); t->angRand = D(4); t->angDir = B(5))
}
GMFN(part_type_gravity) { PT_SET(t->grav = D(1); t->gravDir = D(2)) }
GMFN(part_type_color1) { PT_SET(t->colMode = COL_ONE; t->colPar[0] = C(1)) }
GMFN(part_type_color2) { PT_SET(t->colMode = COL_TWO; t->colPar[0] = C(1); t->colPar[1] = C(2)) }
GMFN(part_type_color3) { PT_SET(t->colMode = COL_THREE; t->colPar[0] = C(1); t->colPar[1] = C(2); t->colPar[2] = C(3)) }
GMFN(part_type_color_mix) { PT_SET(t->colMode = COL_MIX; t->colPar[0] = C(1); t->colPar[1] = C(2)) }
GMFN(part_type_color_rgb) { PT_SET(t->colMode = COL_RGB; for (int i = 0; i < 6; i++) t->colPar[i] = D(i + 1)) }
GMFN(part_type_color_hsv) { PT_SET(t->colMode = COL_HSV; for (int i = 0; i < 6; i++) t->colPar[i] = D(i + 1)) }
GMFN(part_type_alpha1) { PT_SET(t->alphaStart = t->alphaMiddle = t->alphaEnd = D(1)) }
GMFN(part_type_alpha2) { PT_SET(t->alphaStart = D(1); t->alphaMiddle = (D(1) + D(2)) / 2.0; t->alphaEnd = D(2)) }
GMFN(part_type_alpha3) { PT_SET(t->alphaStart = D(1); t->alphaMiddle = D(2); t->alphaEnd = D(3)) }
GMFN(part_type_blend) { PT_SET(t->additive = B(1)) }
GMFN(part_type_death) { PT_SET(t->deathNumber = I(1); t->deathType = I(2)) }
GMFN(part_type_step) { PT_SET(t->stepNumber = I(1); t->stepType = I(2)) }

// ---- effect_create_* (ef_explosion .. ef_snow), YoYo's built-in effects: each kind and size
// is a particle type recipe, burst into one system drawn above everything (depth -100000) or one
// below (100000). Times and speeds are scaled by the speed factor, 30 / game speed above 30 fps.
namespace {

struct EffectRecipe {
    int shape;
    double size0, size1, sizeIncr;  // speeds and increments are per step at 30 fps
    double life0, life1;
    int count;
    double spread = 0;               // particles start within +-spread of the point
    double sp0 = 0, sp1 = 0, spIncr = 0, dir0 = 0, dir1 = 0, dirWiggle = 0;
    bool randomAngle = false;
    double alphaStart = 1, alphaMid = -1, alphaEnd = 1;  // alphaMid < 0: halfway between
    double xscale = 1, yscale = 1, grav = 0;
    bool black = false;  // the explosion's dark core
};

enum { SH_LINE = 3, SH_STAR = 4, SH_RING = 6, SH_FLARE = 8, SH_SPARK = 9, SH_EXPLOSION = 10, SH_SNOW = 13 };

// Recipes per effect kind, then small / medium / large where the kind has sizes.
std::vector<EffectRecipe> effectRecipes(int kind, int size) {
    int z = size <= 0 ? 0 : size >= 2 ? 2 : 1;
    auto pick = [&](auto a, auto b, auto c) { return z == 0 ? a : z == 1 ? b : c; };
    switch (kind) {
        case 0: {  // explosion: a coloured burst and a dark core
            EffectRecipe burst{SH_EXPLOSION, pick(0.1, 0.3, 0.4), pick(0.1, 0.3, 0.4), pick(0.05, 0.1, 0.2),
                               pick(10.0, 12.0, 15.0), pick(15.0, 17.0, 20.0), 20};
            burst.sp0 = burst.sp1 = pick(2.0, 4.0, 7.0);
            burst.spIncr = pick(-0.1, -0.18, -0.2);
            burst.dir1 = 360;
            burst.randomAngle = true;
            burst.alphaStart = 0.6, burst.alphaEnd = 0;
            EffectRecipe core{SH_EXPLOSION, pick(0.1, 0.3, 0.4), pick(0.1, 0.3, 0.4), pick(0.1, 0.2, 0.4),
                              pick(15.0, 17.0, 20.0), pick(15.0, 17.0, 20.0), 1};
            core.randomAngle = true;
            core.alphaStart = 0.8, core.alphaEnd = 0;
            core.black = true;
            return {burst, core};
        }
        case 1:    // ring
        case 2: {  // ellipse: a flattened ring
            bool ell = kind == 2;
            EffectRecipe r{SH_RING, 0, 0, ell ? pick(0.2, 0.35, 0.6) : pick(0.15, 0.25, 0.4), pick(10.0, 13.0, 18.0),
                           pick(12.0, 15.0, 20.0), 1};
            r.alphaStart = 1, r.alphaEnd = 0;
            if (ell) r.yscale = 0.5;
            return {r};
        }
        case 3: {  // firework: flares thrown out under gravity
            EffectRecipe r{SH_FLARE, 0.1, 0.2, 0, pick(15.0, 20.0, 30.0), pick(25.0, 30.0, 40.0), pick(75, 150, 250)};
            r.sp0 = 0.5, r.sp1 = pick(3.0, 6.0, 8.0), r.dir1 = 360;
            r.alphaStart = 1, r.alphaEnd = 0.4;
            r.grav = pick(0.10, 0.15, 0.17);
            return {r};
        }
        case 4:    // smoke
        case 5: {  // smoke rising
            EffectRecipe r{SH_EXPLOSION, pick(0.2, 0.4, 0.4), pick(0.4, 0.7, 1.0), -0.01, pick(25.0, 30.0, 50.0),
                           pick(25.0, 30.0, 50.0), pick(6, 11, 16), pick(5.0, 15.0, 30.0)};
            r.alphaStart = 0.4, r.alphaEnd = 0;
            if (kind == 5) r.sp0 = pick(3.0, 5.0, 6.0), r.sp1 = pick(4.0, 6.0, 7.0), r.dir0 = r.dir1 = 90;
            return {r};
        }
        case 6:    // star
        case 7:    // spark
        case 8: {  // flare
            EffectRecipe r{kind == 6 ? SH_STAR : kind == 7 ? SH_SPARK : SH_FLARE, pick(0.4, 0.75, 1.2),
                           pick(kind == 6 ? 0.3 : 0.4, 0.75, 1.2), pick(-0.02, -0.03, -0.04), pick(20.0, 25.0, 30.0),
                           pick(20.0, 25.0, 30.0), 1};
            r.randomAngle = true;
            return {r};
        }
        case 9: {  // cloud
            EffectRecipe r{SH_EXPLOSION, pick(2.0, 4.0, 8.0), pick(2.0, 4.0, 8.0), 0, 100, 100, 1};
            r.yscale = 0.5;
            r.alphaStart = 0, r.alphaMid = 0.3, r.alphaEnd = 0;
            return {r};
        }
        default: return {};
    }
}

struct EffectState {
    int below = -1, above = -1;
    std::map<int, int> types;  // one particle type per kind, size and part, as YoYo's pt_* arrays
} gEffects;

int effectSystem(Runner& rt, bool above) {
    int& id = above ? gEffects.above : gEffects.below;
    if (!PS.system(id)) {
        PS.systems.emplace_back();
        PS.systems.back().depth = above ? -100000 : 100000;
        id = static_cast<int>(PS.systems.size() - 1);
    }
    return id;
}

void effectCreate(Runner& rt, int sys, int kind, double x, double y, int size, uint32_t col) {
    double speed = rt.dw.gen.gms2 ? rt.gameSpeed : rt.roomSpeed;
    double f = speed <= 30 ? 1.0 : 30.0 / speed;
    int clampedSize = size <= 0 ? 0 : size >= 2 ? 2 : 1;
    auto typeFor = [&](int part) {
        int key = (kind * 3 + clampedSize) * 4 + part;
        auto it = gEffects.types.find(key);
        if (it == gEffects.types.end() || !PS.type(it->second)) {
            PS.types.emplace_back();
            it = gEffects.types.insert_or_assign(key, static_cast<int>(PS.types.size() - 1)).first;
        }
        return it->second;
    };
    int part = 0;
    auto life = [&](double v) { return static_cast<int>(std::lround(v / f)); };
    auto fire = [&](const EffectRecipe& r, double px, double py) {
        int type = typeFor(part);
        ParticleType& t = PS.types[type];
        t = ParticleType();
        t.shape = r.shape;
        t.sizeMinX = t.sizeMinY = r.size0;
        t.sizeMaxX = t.sizeMaxY = r.size1;
        t.sizeIncrX = t.sizeIncrY = r.sizeIncr * f;
        t.xscale = r.xscale;
        t.yscale = r.yscale;
        t.spMin = r.sp0 * f, t.spMax = r.sp1 * f, t.spIncr = r.spIncr * f;
        t.dirMin = r.dir0, t.dirMax = r.dir1, t.dirRand = r.dirWiggle;
        if (r.randomAngle) t.angMin = 0, t.angMax = 360;
        t.alphaStart = r.alphaStart;
        t.alphaEnd = r.alphaEnd;
        t.alphaMiddle = r.alphaMid >= 0 ? r.alphaMid : (r.alphaStart + r.alphaEnd) / 2;
        t.grav = r.grav * f, t.gravDir = 270;
        t.lifeMin = life(r.life0), t.lifeMax = life(r.life1);
        if (kind == 10) t.angDir = true;  // rain streaks point along their fall
        PS.create(sys, px, py, type, r.count, true, r.black ? 0 : col);
    };
    if (kind == 10 || kind == 11) {  // rain and snow fall across the whole room from above
        bool rain = kind == 10;
        EffectRecipe r{rain ? SH_LINE : SH_SNOW, rain ? 0.2 : 0.1, rain ? 0.3 : 0.25, 0,
                       (rain ? 0.2 : 0.5) * rt.roomHeight, (rain ? 0.2 : 0.5) * rt.roomHeight, 1};
        r.sp0 = rain ? 7 : 2.5, r.sp1 = rain ? 7 : 3;
        r.dir0 = rain ? 260 : 240, r.dir1 = rain ? 260 : 300, r.dirWiggle = rain ? 0 : 20;
        r.alphaStart = r.alphaMid = r.alphaEnd = rain ? 0.4 : 0.6;
        r.randomAngle = !rain;
        int n = rain ? (size <= 0 ? 2 : size >= 2 ? 9 : 5) : (size <= 0 ? 1 : size >= 2 ? 7 : 3);
        for (int i = 0; i < n; i++) {
            fire(r, PS.random(1) * 1.2 * rt.roomWidth - (rain ? 0 : 60), -30 + PS.random(20));
        }
        return;
    }
    for (const EffectRecipe& r : effectRecipes(kind, size)) {
        if (r.spread > 0) {  // smoke: one particle at a time around the point
            EffectRecipe one = r;
            one.count = 1;
            for (int i = 0; i < r.count; i++)
                fire(one, x - r.spread + PS.random(2 * r.spread), y - r.spread + PS.random(2 * r.spread));
        } else {
            fire(r, x, y);
        }
        part++;
    }
}

}  // namespace

GMFN(effect_create_above) {  // (kind, x, y, size, colour)
    effectCreate(rt, effectSystem(rt, true), I(0), D(1), D(2), I(3), C(4));
    return Value();
}
GMFN(effect_create_below) {
    effectCreate(rt, effectSystem(rt, false), I(0), D(1), D(2), I(3), C(4));
    return Value();
}
GMFN(effect_create_depth) {  // (depth, kind, x, y, size, colour): above or below by the sign
    effectCreate(rt, effectSystem(rt, D(0) < 0), I(1), D(2), D(3), I(4), C(5));
    return Value();
}
GMFN(effect_create_layer) {  // (layer, kind, x, y, size, colour)
    Layer* L = rt.layerFromValue(ARG(0));
    effectCreate(rt, effectSystem(rt, !L || L->depth < 0), I(1), D(2), D(3), I(4), C(5));
    return Value();
}
GMFN(effect_clear) {
    for (int id : {gEffects.below, gEffects.above})
        if (ParticleSystem* s = PS.system(id)) {
            s->particles.clear();
            for (auto& e : s->emitters) e.particles.clear();
        }
    return Value();
}

void registerParticleBuiltins(Runner& rt) {
    gEffects = EffectState();
    GMREG(effect_create_above); GMREG(effect_create_below); GMREG(effect_create_depth); GMREG(effect_create_layer);
    GMREG(effect_clear);
    GMREG(part_system_create); GMREG(part_system_create_layer); GMREG(part_system_destroy); GMREG(part_system_angle);
    GMREG(part_system_exists); GMREG(part_system_clear); GMREG(part_system_depth); GMREG(part_system_layer);
    GMREG(part_system_position); GMREG(part_system_color); GMALIAS(part_system_colour, part_system_color);
    GMREG(part_system_automatic_update); GMREG(part_system_automatic_draw); GMREG(part_system_draw_order);
    GMREG(part_system_drawit); GMREG(part_system_update); GMREG(part_particles_create);
    GMREG(part_particles_create_color); GMALIAS(part_particles_create_colour, part_particles_create_color);
    GMREG(part_particles_clear); GMREG(part_particles_count);
    GMREG(part_emitter_create); GMREG(part_emitter_destroy); GMREG(part_emitter_destroy_all);
    GMREG(part_emitter_exists); GMREG(part_emitter_clear); GMREG(part_emitter_region); GMREG(part_emitter_burst);
    GMREG(part_emitter_stream); GMREG(part_emitter_enable); GMREG(part_emitter_relative); GMREG(part_emitter_delay);
    GMREG(part_emitter_interval);
    GMREG(part_type_create); GMREG(part_type_destroy); GMREG(part_type_exists); GMREG(part_type_clear);
    GMREG(part_type_sprite); GMREG(part_type_subimage); GMREG(part_type_shape); GMREG(part_type_size);
    GMREG(part_type_size_x); GMREG(part_type_size_y); GMREG(part_type_scale); GMREG(part_type_life);
    GMREG(part_type_speed); GMREG(part_type_direction); GMREG(part_type_gravity); GMREG(part_type_orientation);
    GMREG(part_type_color1); GMALIAS(part_type_colour1, part_type_color1); GMREG(part_type_color2);
    GMALIAS(part_type_colour2, part_type_color2); GMREG(part_type_color3); GMALIAS(part_type_colour3, part_type_color3);
    GMREG(part_type_color_mix); GMALIAS(part_type_colour_mix, part_type_color_mix); GMREG(part_type_color_rgb);
    GMALIAS(part_type_colour_rgb, part_type_color_rgb); GMREG(part_type_color_hsv);
    GMALIAS(part_type_colour_hsv, part_type_color_hsv); GMREG(part_type_alpha1); GMREG(part_type_alpha2);
    GMREG(part_type_alpha3); GMREG(part_type_blend); GMREG(part_type_death); GMREG(part_type_step);
}

// ---------------------------------------------------------------------------------------------
// Drag-and-drop actions compiled by GMS 1.4 into calls to action_* functions.

static bool g_relative = false;

GMFN(action_set_relative) { g_relative = B(0); return Value(); }
GMFN(action_kill_object) { rt.destroyInstance(self); return Value(); }
static bool compareOp(double a, double b, int op) {
    switch (op) {
        case 1: return a < b;
        case 2: return a > b;
        case 3: return a <= b;
        case 4: return a >= b;
        default: return std::fabs(a - b) < 1e-5;
    }
}
GMFN(action_if_variable) {
    if (ARG(0).isString() || ARG(1).isString()) {
        bool eq = ARG(0).toString() == ARG(1).toString();
        return boolV(I(2) == 0 ? eq : false);
    }
    return boolV(compareOp(D(0), D(1), I(2)));
}
GMFN(action_if_number) { return boolV(compareOp(rt.instanceCountOf(I(0)), D(1), I(2))); }
GMFN(action_if) { return boolV(B(0)); }
static Instance* createRel(Runner& rt, Instance* self, int obj, double x, double y) {
    if (g_relative && self) {
        x += self->x;
        y += self->y;
    }
    Instance* inst = rt.createInstance(x, y, obj, -1, false);
    if (inst) rt.performEvent(inst, EV_CREATE, 0, self);
    return inst;
}
GMFN(action_create_object) { createRel(rt, self, I(0), D(1), D(2)); return Value(); }
GMFN(action_create_object_motion) {
    if (Instance* i = createRel(rt, self, I(0), D(1), D(2))) i->setSpeedDir(D(3), D(4));
    return Value();
}
GMFN(action_set_alarm) {
    int a = I(1);
    if (self && a >= 0 && a < 12) self->alarm[a] = g_relative ? self->alarm[a] + D(0) : D(0);
    return Value();
}
GMFN(action_sprite_set) {
    if (!self) return Value();
    self->sprite = I(0);
    if (D(1) >= 0) self->imageIndex = D(1);
    self->imageSpeed = D(2);
    return Value();
}
GMFN(action_set_hspeed) {
    if (self) self->setHVSpeed(g_relative ? self->hspeed + D(0) : D(0), self->vspeed);
    return Value();
}
GMFN(action_set_vspeed) {
    if (self) self->setHVSpeed(self->hspeed, g_relative ? self->vspeed + D(0) : D(0));
    return Value();
}
GMFN(action_set_friction) {
    if (self) self->friction = g_relative ? self->friction + D(0) : D(0);
    return Value();
}
GMFN(action_set_gravity) {
    if (!self) return Value();
    self->gravityDirection = g_relative ? self->gravityDirection + D(0) : D(0);
    self->gravity = g_relative ? self->gravity + D(1) : D(1);
    return Value();
}
GMFN(action_set_motion) {
    if (!self) return Value();
    if (g_relative) {
        double r = D(0) * M_PI / 180.0;
        self->setHVSpeed(self->hspeed + D(1) * std::cos(r), self->vspeed - D(1) * std::sin(r));
    } else {
        self->setSpeedDir(D(1), D(0));
    }
    return Value();
}
// "Move fixed": one of the directions marked in a 9-character string laid out like a number pad
// (down-left, down, down-right, left, stop, right, up-left, up, up-right), picked at random.
GMFN(action_move) {
    if (!self) return Value();
    static const double kDirs[9] = {225, 270, 315, 180, -1, 0, 135, 90, 45};
    std::string dirs = S(0);
    int marked[9], n = 0;
    for (int i = 0; i < 9 && i < static_cast<int>(dirs.size()); i++)
        if (dirs[i] == '1') marked[n++] = i;
    if (n == 0) return Value();
    int pick = marked[std::min(n - 1, static_cast<int>(rt.rng.next() * n))];
    if (kDirs[pick] < 0) self->setSpeedDir(0, self->direction);
    else self->setSpeedDir(g_relative ? self->speed + D(1) : D(1), kDirs[pick]);
    return Value();
}
// "Move towards": head for (x, y) at a speed; relative takes the point from the instance.
GMFN(action_move_point) {
    if (!self) return Value();
    double tx = g_relative ? self->x + D(0) : D(0), ty = g_relative ? self->y + D(1) : D(1);
    self->setSpeedDir(D(2), std::atan2(-(ty - self->y), tx - self->x) * 180.0 / M_PI);
    return Value();
}
GMFN(action_move_to) {
    if (!self) return Value();
    self->x = g_relative ? self->x + D(0) : D(0);
    self->y = g_relative ? self->y + D(1) : D(1);
    return Value();
}
GMFN(action_reverse_xdir) { if (self) self->setHVSpeed(-self->hspeed, self->vspeed); return Value(); }
GMFN(action_reverse_ydir) { if (self) self->setHVSpeed(self->hspeed, -self->vspeed); return Value(); }
GMFN(action_sound) {
    rt.audio->play(I(0), 0, B(1));
    return Value();
}
GMFN(action_another_room) { rt.gotoRoom(I(0)); return Value(); }
GMFN(action_next_room) {
    int n = rt.roomNext(rt.room);
    if (n >= 0) rt.gotoRoom(n);
    return Value();
}
GMFN(action_previous_room) {
    int n = rt.roomPrevious(rt.room);
    if (n >= 0) rt.gotoRoom(n);
    return Value();
}
GMFN(action_current_room) { rt.restartPending = true; return Value(); }
GMFN(action_end_game) { rt.gameEnded = true; return Value(); }
GMFN(action_restart_game) { rt.gameRestartPending = true; return Value(); }
GMFN(action_execute_script) {
    int code = rt.vm.scriptCode(I(0));
    if (code >= 0) rt.vm.call(code, self, other, argc - 1, args + 1);
    return Value();
}

void registerActionBuiltins(Runner& rt) {
    GMREG(action_set_relative); GMREG(action_kill_object); GMREG(action_if_variable); GMREG(action_if_number);
    GMREG(action_if); GMREG(action_create_object); GMREG(action_create_object_motion); GMREG(action_set_alarm);
    GMREG(action_sprite_set); GMREG(action_set_hspeed); GMREG(action_set_vspeed); GMREG(action_set_friction);
    GMREG(action_set_gravity); GMREG(action_set_motion); GMREG(action_move); GMREG(action_move_point); GMREG(action_move_to); GMREG(action_reverse_xdir);
    GMREG(action_reverse_ydir); GMREG(action_sound); GMREG(action_another_room); GMREG(action_next_room);
    GMREG(action_previous_room); GMREG(action_current_room); GMREG(action_end_game); GMREG(action_restart_game);
    GMREG(action_execute_script);
}

}  // namespace gm
