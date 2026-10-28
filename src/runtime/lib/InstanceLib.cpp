// Instances, collisions, motion, events, rooms, objects, sprites, backgrounds, tiles and paths.
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

#include "runtime/Builtins.h"
#include "runtime/Graphics.h"

namespace gm {

static constexpr double kNoone = -4;

static Value idOf(Instance* i) { return Value(i ? static_cast<double>(i->id) : kNoone); }

// Iterates instances matching a GML object/instance argument (object index, instance id or all).
template <typename F> static void eachTarget(Runner& rt, Instance* self, Instance* other, int target, F&& f) {
    std::vector<Instance*> list;
    rt.collectTargets(target, self, other, list);
    for (Instance* i : list)
        if (!f(i)) return;
}

// ---- creation / destruction
GMFN(instance_create) {
    Instance* inst = rt.createInstance(D(0), D(1), I(2), -1, false);
    if (!inst) return Value(kNoone);
    rt.performEvent(inst, EV_CREATE, 0, self);
    return Value(static_cast<double>(inst->id));
}
GMFN(instance_destroy) {
    if (argc > 0) {
        eachTarget(rt, self, other, I(0), [&](Instance* i) {
            rt.destroyInstance(i, argc < 2 || B(1));
            return true;
        });
    } else {
        rt.destroyInstance(self);
    }
    return Value();
}
GMFN(instance_exists) {
    int t = I(0);
    if (t == INST_SELF) return boolV(self && Runner::live(self));
    if (t == INST_OTHER) return boolV(other && Runner::live(other));
    if (t >= 100000) return boolV(rt.findById(t) != nullptr);
    if (t == INST_ALL) return boolV(rt.firstOf(-1) != nullptr);
    if (t < 0 || static_cast<size_t>(t) >= rt.dw.objects.size()) return boolV(false);
    return boolV(rt.firstOf(t) != nullptr);
}
GMFN(instance_number) {
    int t = I(0);
    if (t == INST_ALL) t = -1;
    else if (t < 0 || static_cast<size_t>(t) >= rt.dw.objects.size()) return Value(0.0);
    return Value(static_cast<double>(rt.instanceCountOf(t)));
}
GMFN(instance_find) {
    int t = I(0), n = I(1);
    std::vector<Instance*> list;
    rt.collectTargets(t == INST_ALL ? INST_ALL : t, self, other, list);
    if (n < 0 || static_cast<size_t>(n) >= list.size()) return Value(kNoone);
    return idOf(list[n]);
}
static Value nearestFurthest(Runner& rt, Instance* self, Instance* other, double x, double y, int obj, bool nearest) {
    Instance* best = nullptr;
    double bestD = nearest ? std::numeric_limits<double>::max() : -1;
    eachTarget(rt, self, other, obj, [&](Instance* i) {
        double d = std::hypot(i->x - x, i->y - y);
        // GMS2 keeps the last instance found on a tie (YoYo's runner); 1.4 keeps the first.
        bool better = nearest ? d < bestD : d > bestD;
        if (rt.dw.gen.gms2 && d == bestD) better = true;
        if (better) {
            bestD = d;
            best = i;
        }
        return true;
    });
    return idOf(best);
}
GMFN(instance_nearest) { return nearestFurthest(rt, self, other, D(0), D(1), I(2), true); }
GMFN(instance_furthest) { return nearestFurthest(rt, self, other, D(0), D(1), I(2), false); }
GMFN(instance_change) {
    int obj = I(0);
    bool perf = B(1);
    if (!self || obj < 0 || static_cast<size_t>(obj) >= rt.dw.objects.size()) return Value();
    if (perf) {
        rt.performEvent(self, EV_DESTROY, 0);
        if (rt.dw.gen.gms2) rt.performEvent(self, EV_CLEANUP, 0);
    }
    auto& oldList = rt.byObject[self->object];
    oldList.erase(std::remove(oldList.begin(), oldList.end(), self), oldList.end());
    self->object = obj;
    rt.byObject[obj].push_back(self);
    const GameObject& o = rt.dw.objects[obj];
    self->sprite = o.sprite;
    self->mask = o.mask;
    self->visible = o.visible;
    self->solid = o.solid;
    self->persistent = o.persistent;
    self->depth = o.depth;
    if (perf) {
        if (rt.dw.gen.gms2) rt.performEvent(self, EV_PRECREATE, 0);
        rt.performEvent(self, EV_CREATE, 0);
    }
    return Value();
}
GMFN(instance_copy) {
    if (!self) return Value(kNoone);
    Instance* inst = rt.createInstance(self->x, self->y, self->object, -1, false);
    int id = inst->id;
    uint64_t order = inst->createOrder, step = inst->createdStep;
    *inst = *self;
    inst->id = id;
    inst->createOrder = order;
    inst->createdStep = step;
    rt.byId[id] = inst;
    if (B(0)) {
        if (rt.dw.gen.gms2) rt.performEvent(inst, EV_PRECREATE, 0, self);
        rt.performEvent(inst, EV_CREATE, 0, self);
    }
    return Value(static_cast<double>(id));
}

// ---- collisions
static Instance* placeFirst(Runner& rt, Instance* self, Instance* other, double x, double y, int obj) {
    Instance* hit = nullptr;
    if (!self) return nullptr;
    eachTarget(rt, self, other, obj, [&](Instance* i) {
        if (i != self && rt.instancesCollide(self, x, y, i)) {
            hit = i;
            return false;
        }
        return true;
    });
    return hit;
}
// A collision target (2023+): an object, instance, all/other, a tilemap element or an array of
// these. Returns what self at (x, y) meets first (instance id or the tilemap), or noone.
static Value placeTarget(Runner& rt, Instance* self, Instance* other, double x, double y, const Value& target) {
    if (!self) return Value(kNoone);
    if (const GMArray* a = target.arr()) {
        if (!a->rows.empty())
            for (const Value& v : a->rows[0]) {
                Value hit = placeTarget(rt, self, other, x, y, v);
                if (hit.num() != kNoone) return hit;
            }
        return Value(kNoone);
    }
    double t = target.num();
    if (t >= Runner::kElementIdBase) {
        if (const LayerElement* e = rt.elementById(static_cast<int32_t>(t)); e && e->type == ElementType::Tilemap)
            return rt.tilemapPlace(self, x, y, e->id) ? target : Value(kNoone);
    }
    return idOf(placeFirst(rt, self, other, x, y, static_cast<int>(t)));
}
GMFN(place_meeting) { return boolV(placeTarget(rt, self, other, D(0), D(1), ARG(2)).num() != kNoone); }
GMFN(instance_place) { return placeTarget(rt, self, other, D(0), D(1), ARG(2)); }

// move_and_collide(dx, dy, obj, [iterations=4], [xoff, yoff], [max_x_move, max_y_move]), as YoYo's:
// moves in steps; when blocked, tries sideways steps (perpendicular, or along (xoff, yoff)) of
// growing size; returns the array of what was hit.
GMFN(move_and_collide) {
    Value result = Value::newArray();
    GMArray* out = result.writableArray();
    out->rows.resize(1);
    auto& hits = out->rows[0];
    auto note = [&](const Value& v) {
        for (const Value& h : hits)
            if (h.num() == v.num()) return;
        hits.push_back(v);
    };
    if (!self) return result;
    Value target = ARG(2);
    if (!target.arr()) {
        if (target.num() == kNoone) return result;
        if (target.num() == -1) target = Value(static_cast<double>(self->id));  // self
    }
    auto test = [&](double x, double y) { return placeTarget(rt, self, other, x, y, target); };
    Value res = test(self->x, self->y);
    if (res.num() != kNoone) {
        note(res);
        return result;
    }
    double dx = D(0), dy = D(1);
    if (dx == 0 && dy == 0) return result;
    double xc = argc > 6 && !ARG(6).isUndefined() ? D(6) : -1, yc = argc > 7 && !ARG(7).isUndefined() ? D(7) : -1;
    double minx = self->x - xc, maxx = self->x + xc, miny = self->y - yc, maxy = self->y + yc;
    auto clampXY = [&](double& x, double& y) {
        if (xc >= 0) x = std::clamp(x, minx, maxx);
        if (yc >= 0) y = std::clamp(y, miny, maxy);
    };
    int steps = argc > 3 && !ARG(3).isUndefined() ? I(3) : 4;
    double xoff = argc > 4 && !ARG(4).isUndefined() ? D(4) : 0, yoff = argc > 5 && !ARG(5).isUndefined() ? D(5) : 0;
    bool perpendicular = xoff == 0 && yoff == 0;
    double lx = 0, ly = 0;
    if (!perpendicular) {
        double len = std::sqrt(xoff * xoff + yoff * yoff);
        lx = xoff / len;
        ly = yoff / len;
    }
    double dist = std::sqrt(dx * dx + dy * dy), ndx = dx / dist, ndy = dy / dist;
    const double r2 = 0.70710678118654;
    double stepDist = dist / steps, left = dist;
    auto tryMove = [&](double x, double y) {
        clampXY(x, y);
        Value h = test(x, y);
        if (h.num() == kNoone) {
            self->x = x;
            self->y = y;
            return true;
        }
        note(h);
        return false;
    };
    for (int i = 0; i < steps; i++) {
        double d = std::min(stepDist, left);
        if (d <= 0) break;
        if (tryMove(self->x + ndx * d, self->y + ndy * d)) {
            left -= d;
            continue;
        }
        bool moved = false;
        for (int j = 1; j < steps - i + 1 && !moved; j++) {
            if (perpendicular) {
                moved = tryMove(self->x + r2 * (ndx + j * ndy) * d, self->y + r2 * (ndy - j * ndx) * d) ||
                        tryMove(self->x + r2 * (ndx - j * ndy) * d, self->y + r2 * (ndy + j * ndx) * d);
            } else {
                moved = tryMove(self->x + r2 * (ndx + j * lx) * d, self->y + r2 * (ndy + j * ly) * d);
            }
            if (moved) left -= d * j;
        }
        if (!moved) break;
    }
    return result;
}
GMFN(place_free) {
    bool free = true;
    rt.forEachInstanceOf(-1, [&](Instance* i) {
        if (free && i != self && i->solid && rt.instancesCollide(self, D(0), D(1), i)) free = false;
    });
    return boolV(free);
}
GMFN(place_empty) {
    bool empty = true;
    rt.forEachInstanceOf(-1, [&](Instance* i) {
        if (empty && i != self && rt.instancesCollide(self, D(0), D(1), i)) empty = false;
    });
    return boolV(empty);
}
GMFN(position_meeting) {
    bool hit = false;
    eachTarget(rt, self, other, I(2), [&](Instance* i) {
        hit = rt.pointInInstance(i, D(0), D(1), true);
        return !hit;
    });
    return boolV(hit);
}
GMFN(instance_position) {
    Instance* hit = nullptr;
    eachTarget(rt, self, other, I(2), [&](Instance* i) {
        if (rt.pointInInstance(i, D(0), D(1), true)) hit = i;
        return !hit;
    });
    return idOf(hit);
}
GMFN(position_empty) {
    bool empty = true;
    rt.forEachInstanceOf(-1, [&](Instance* i) {
        if (empty && rt.pointInInstance(i, D(0), D(1), true)) empty = false;
    });
    return boolV(empty);
}
GMFN(position_destroy) {
    std::vector<Instance*> hits;
    rt.forEachInstanceOf(-1, [&](Instance* i) {
        if (rt.pointInInstance(i, D(0), D(1), true)) hits.push_back(i);
    });
    for (Instance* i : hits) rt.destroyInstance(i);
    return Value();
}
template <typename Test>
static Value collisionFirst(Runner& rt, Instance* self, Instance* other, int obj, bool notme, Test test) {
    Instance* hit = nullptr;
    eachTarget(rt, self, other, obj, [&](Instance* i) {
        if (notme && i == self) return true;
        if (test(i)) {
            hit = i;
            return false;
        }
        return true;
    });
    return idOf(hit);
}
GMFN(collision_rectangle) {
    return collisionFirst(rt, self, other, I(4), B(6),
                          [&](Instance* i) { return rt.rectInInstance(i, D(0), D(1), D(2), D(3), B(5)); });
}
GMFN(collision_point) {
    return collisionFirst(rt, self, other, I(2), B(4), [&](Instance* i) { return rt.pointInInstance(i, D(0), D(1), B(3)); });
}
GMFN(collision_line) {
    return collisionFirst(rt, self, other, I(4), B(6),
                          [&](Instance* i) { return rt.lineInInstance(i, D(0), D(1), D(2), D(3), B(5)); });
}
GMFN(collision_circle) {
    return collisionFirst(rt, self, other, I(3), B(5),
                          [&](Instance* i) { return rt.circleInInstance(i, D(0), D(1), D(2), D(2), B(4)); });
}
GMFN(collision_ellipse) {
    double cx = (D(0) + D(2)) / 2, cy = (D(1) + D(3)) / 2, rx = std::fabs(D(2) - D(0)) / 2, ry = std::fabs(D(3) - D(1)) / 2;
    return collisionFirst(rt, self, other, I(4), B(6),
                          [&](Instance* i) { return rt.circleInInstance(i, cx, cy, rx, ry, B(5)); });
}

// ---- distances and motion
static double boxDistance(const Runner::BBox& b, double x, double y) {
    double dx = x < b.left ? b.left - x : (x > b.right ? x - b.right : 0);
    double dy = y < b.top ? b.top - y : (y > b.bottom ? y - b.bottom : 0);
    return std::sqrt(dx * dx + dy * dy);
}
GMFN(distance_to_point) {
    if (!self) return Value(0.0);
    Runner::BBox b = rt.bbox(self);
    if (!b.valid) return Value(std::hypot(D(0) - self->x, D(1) - self->y));
    return Value(boxDistance(b, D(0), D(1)));
}
GMFN(distance_to_object) {
    if (!self) return Value(0.0);
    double best = 1e10;
    Runner::BBox a = rt.bbox(self);
    eachTarget(rt, self, other, I(0), [&](Instance* i) {
        if (i == self) return true;
        Runner::BBox b = rt.bbox(i);
        double dx = std::max({0.0, b.left - a.right, a.left - b.right});
        double dy = std::max({0.0, b.top - a.bottom, a.top - b.bottom});
        best = std::min(best, std::sqrt(dx * dx + dy * dy));
        return true;
    });
    return Value(best);
}
GMFN(motion_set) {
    if (self) self->setSpeedDir(D(1), D(0));
    return Value();
}
GMFN(motion_add) {
    if (!self) return Value();
    self->addSpeed(D(0), D(1));
    return Value();
}
GMFN(move_towards_point) {
    if (!self) return Value();
    double dir = std::atan2(-(D(1) - self->y), D(0) - self->x) * 180.0 / M_PI;
    self->setSpeedDir(D(2), dir);
    return Value();
}
// ---- classic motion (YoYo's Function_Movement): free = no solid instance there (or no
// instance at all for the _all variants) at the instance's mask placed at x, y.
static bool freeAt(Runner& rt, Instance* self, double x, double y, bool useAll) {
    bool free = true;
    rt.forEachInstanceOf(-1, [&](Instance* i) {
        if (free && i != self && (useAll || i->solid) && rt.instancesCollide(self, x, y, i)) free = false;
    });
    return free;
}
static void moveContact(Runner& rt, Instance* self, double dir, double maxDist, bool useAll) {
    if (!self) return;
    int steps = maxDist <= 0 ? 1000 : static_cast<int>(std::lround(maxDist));
    double dx = std::cos(dir * M_PI / 180), dy = -std::sin(dir * M_PI / 180);
    if (!freeAt(rt, self, self->x, self->y, useAll)) return;
    for (int i = 1; i <= steps && freeAt(rt, self, self->x + dx, self->y + dy, useAll); i++) {
        self->x += dx;
        self->y += dy;
    }
}
static void moveOutside(Runner& rt, Instance* self, double dir, double maxDist, bool useAll) {
    if (!self) return;
    int steps = maxDist <= 0 ? 1000 : static_cast<int>(std::lround(maxDist));
    double dx = std::cos(dir * M_PI / 180), dy = -std::sin(dir * M_PI / 180);
    for (int i = 1; i <= steps && !freeAt(rt, self, self->x, self->y, useAll); i++) {
        self->x += dx;
        self->y += dy;
    }
}
static void moveBounce(Runner& rt, Instance* self, bool advanced, bool useAll) {
    if (!self) return;
    bool bounced = false;
    if (!freeAt(rt, self, self->x, self->y, useAll)) {
        self->x = self->xprevious;
        self->y = self->yprevious;
        bounced = true;
    }
    if (advanced) {
        const int n = 18;
        double dir = 10.0 * std::round(self->direction / 10.0), ldir = dir, rdir = dir;
        auto blocked = [&](double d) {
            double r = d * M_PI / 180;
            return !freeAt(rt, self, self->x + self->speed * std::cos(r), self->y - self->speed * std::sin(r), useAll);
        };
        for (int i = 1; i < 2 * n; i++) {
            ldir -= 180.0 / n;
            if (!blocked(ldir)) break;
            bounced = true;
        }
        for (int i = 1; i < 2 * n; i++) {
            rdir += 180.0 / n;
            if (!blocked(rdir)) break;
            bounced = true;
        }
        if (bounced) self->setSpeedDir(self->speed, 180 + (ldir + rdir) - dir);
        return;
    }
    bool bh = freeAt(rt, self, self->x + self->hspeed, self->y, useAll);
    bool bv = freeAt(rt, self, self->x, self->y + self->vspeed, useAll);
    bool ba = freeAt(rt, self, self->x + self->hspeed, self->y + self->vspeed, useAll);
    if ((!bh && !bv) || (bh && bv && !ba)) self->setHVSpeed(-self->hspeed, -self->vspeed);
    else if (!bh) self->setHVSpeed(-self->hspeed, self->vspeed);
    else if (!bv) self->setHVSpeed(self->hspeed, -self->vspeed);
}
GMFN(move_contact_solid) { moveContact(rt, self, D(0), D(1), false); return Value(); }
GMFN(move_contact_all) { moveContact(rt, self, D(0), D(1), true); return Value(); }
GMFN(move_outside_solid) { moveOutside(rt, self, D(0), D(1), false); return Value(); }
GMFN(move_outside_all) { moveOutside(rt, self, D(0), D(1), true); return Value(); }
GMFN(move_bounce_solid) { moveBounce(rt, self, B(0), false); return Value(); }
GMFN(move_bounce_all) { moveBounce(rt, self, B(0), true); return Value(); }
GMFN(place_snapped) {
    if (!self) return boolV(false);
    double h = D(0), v = D(1);
    if (h > 0 && std::fabs(self->x - h * std::round(self->x / h)) >= 0.001) return boolV(false);
    if (v > 0 && std::fabs(self->y - v * std::round(self->y / v)) >= 0.001) return boolV(false);
    return boolV(true);
}
GMFN(move_random) {  // a free position snapped to (hsnap, vsnap), at most 100 tries
    if (!self) return Value();
    double xmin = 0, xmax = rt.roomWidth, ymin = 0, ymax = rt.roomHeight;
    Runner::BBox b = rt.bbox(self);
    if (b.valid) {
        xmin = std::round(self->x - b.left);
        xmax = std::round(xmax + self->x - b.right);
        ymin = std::round(self->y - b.top);
        ymax = std::round(ymax + self->y - b.bottom);
    }
    double sx = std::round(D(0)), sy = std::round(D(1));
    for (int i = 1; i < 100; i++) {
        double x = xmin + rt.rng.next() * (xmax - xmin);
        if (sx > 0) x = sx * std::floor(x / sx);
        double y = ymin + rt.rng.next() * (ymax - ymin);
        if (sy > 0) y = sy * std::floor(y / sy);
        if (freeAt(rt, self, x, y, false)) {
            self->x = x;
            self->y = y;
            return Value();
        }
    }
    return Value();
}
GMFN(move_snap) {
    if (!self) return Value();
    double h = D(0), v = D(1);
    if (h > 0) self->x = std::round(self->x / h) * h;
    if (v > 0) self->y = std::round(self->y / v) * v;
    return Value();
}
GMFN(move_wrap) {
    if (!self) return Value();
    double m = D(2);
    if (B(0)) {
        if (self->x < -m) self->x += rt.roomWidth + 2 * m;
        else if (self->x > rt.roomWidth + m) self->x -= rt.roomWidth + 2 * m;
    }
    if (B(1)) {
        if (self->y < -m) self->y += rt.roomHeight + 2 * m;
        else if (self->y > rt.roomHeight + m) self->y -= rt.roomHeight + 2 * m;
    }
    return Value();
}

// ---- activation
GMFN(instance_deactivate_all) {
    bool notme = B(0);
    for (Instance* i : rt.instances)
        if (!i->destroyed && !(notme && i == self)) rt.setActive(i, false);
    return Value();
}
GMFN(instance_activate_all) {
    for (Instance* i : rt.instances)
        if (!i->destroyed) rt.setActive(i, true);
    return Value();
}
static void setActiveFor(Runner& rt, int target, bool active) {
    for (Instance* i : rt.instances) {
        if (i->destroyed) continue;
        bool match = target == INST_ALL || (target >= 100000 ? i->id == target : rt.objectIs(i->object, target));
        if (match) rt.setActive(i, active);
    }
}
GMFN(instance_activate_object) { setActiveFor(rt, I(0), true); return Value(); }
GMFN(instance_deactivate_object) { setActiveFor(rt, I(0), false); return Value(); }
static void regionActivate(Runner& rt, Instance* self, double l, double t, double w, double h, bool inside,
                           bool active, bool notme) {
    for (Instance* i : rt.instances) {
        if (i->destroyed || (notme && i == self)) continue;
        Runner::BBox b = rt.bbox(i);
        double bl = b.valid ? b.left : i->x, br = b.valid ? b.right : i->x;
        double bt = b.valid ? b.top : i->y, bb = b.valid ? b.bottom : i->y;
        bool in = br >= l && bl <= l + w && bb >= t && bt <= t + h;
        if (in == inside) rt.setActive(i, active);
    }
}
GMFN(instance_activate_region) {
    regionActivate(rt, self, D(0), D(1), D(2), D(3), B(4), true, false);
    return Value();
}
GMFN(instance_deactivate_region) {
    regionActivate(rt, self, D(0), D(1), D(2), D(3), B(4), false, B(5));
    return Value();
}

// ---- events
GMFN(event_perform) {
    if (self) rt.performEvent(self, static_cast<uint32_t>(I(0)), static_cast<uint32_t>(I(1)), other);
    return Value();
}
GMFN(event_perform_object) {
    // `other` carries through, e.g. a collision handler forwarding to a parent's handler.
    if (self) rt.performEventAs(self, I(0), static_cast<uint32_t>(I(1)), static_cast<uint32_t>(I(2)), other);
    return Value();
}
GMFN(event_user) {
    if (self) rt.performEvent(self, EV_OTHER, OTHER_USER0 + static_cast<uint32_t>(I(0)), other);
    return Value();
}
GMFN(event_inherited) {
    if (self) rt.eventInherited(self, other);
    return Value();
}

// ---- objects
static const GameObject* obj(Runner& rt, int o) {
    return o >= 0 && static_cast<size_t>(o) < rt.dw.objects.size() ? &rt.dw.objects[o] : nullptr;
}
GMFN(object_exists) { return boolV(obj(rt, I(0)) != nullptr); }
GMFN(object_get_name) { auto* o = obj(rt, I(0)); return Value(o ? std::string(o->name) : "<undefined>"); }
GMFN(object_get_parent) {
    auto* o = obj(rt, I(0));
    return Value(o ? static_cast<double>(o->parent < 0 ? -100 : o->parent) : -1.0);
}
GMFN(object_is_ancestor) {
    auto* o = obj(rt, I(0));
    if (!o || o->parent < 0) return boolV(false);
    return boolV(rt.objectIs(o->parent, I(1)));
}
GMFN(object_get_sprite) { auto* o = obj(rt, I(0)); return Value(o ? static_cast<double>(o->sprite) : -1.0); }
GMFN(object_get_depth) { auto* o = obj(rt, I(0)); return Value(o ? static_cast<double>(o->depth) : 0.0); }
GMFN(object_get_visible) { auto* o = obj(rt, I(0)); return boolV(o && o->visible); }
GMFN(object_get_solid) { auto* o = obj(rt, I(0)); return boolV(o && o->solid); }
GMFN(object_get_persistent) { auto* o = obj(rt, I(0)); return boolV(o && o->persistent); }
GMFN(object_get_mask) { auto* o = obj(rt, I(0)); return Value(o ? static_cast<double>(o->mask) : -1.0); }

// object_set_*: change the object's defaults for instances created afterwards.
static GameObject* objMut(Runner& rt, int o) {
    return o >= 0 && static_cast<size_t>(o) < rt.dw.objects.size() ? &rt.dw.objects[o] : nullptr;
}
GMFN(object_set_sprite) { if (auto* o = objMut(rt, I(0))) o->sprite = I(1); return Value(); }
GMFN(object_set_visible) { if (auto* o = objMut(rt, I(0))) o->visible = B(1); return Value(); }
GMFN(object_set_solid) { if (auto* o = objMut(rt, I(0))) o->solid = B(1); return Value(); }
GMFN(object_set_persistent) { if (auto* o = objMut(rt, I(0))) o->persistent = B(1); return Value(); }
GMFN(object_set_mask) { if (auto* o = objMut(rt, I(0))) o->mask = I(1); return Value(); }
GMFN(object_get_physics) { auto* o = obj(rt, I(0)); return boolV(o && o->physics); }
GMFN(instance_id_get) {  // the n-th active instance (instance_id[n])
    int64_t n = 0, index = I(0);
    for (Instance* i : rt.instances)
        if (Runner::live(i) && n++ == index) return Value(static_cast<double>(i->id));
    return Value(kNoone);
}
// @@GetInstance@@(object or instance): what obj.member resolves against when calling a method
// on an object (its first instance) or an instance id.
GMFN(gml_get_instance) {
    if (!ARG(0).isNumeric()) return ARG(0);
    int t = I(0);
    if (t >= 0 && static_cast<size_t>(t) < rt.dw.objects.size()) return idOf(rt.firstOf(t));
    return ARG(0);
}
static void layerActivate(Runner& rt, const Value& layer, bool active) {
    Layer* L = rt.layerFromValue(layer);
    if (!L) return;
    for (int32_t id : L->elements)
        if (const LayerElement* e = rt.elementById(id); e && e->type == ElementType::Instance)
            for (Instance* i : rt.instances)
                if (i->id == e->instance && !i->destroyed) rt.setActive(i, active);
}
GMFN(instance_activate_layer) { layerActivate(rt, ARG(0), true); return Value(); }
GMFN(instance_deactivate_layer) { layerActivate(rt, ARG(0), false); return Value(); }

// ---- rooms
static bool roomValid(Runner& rt, int r) { return r >= 0 && static_cast<size_t>(r) < rt.dw.rooms.size(); }
GMFN(room_goto) {
    if (roomValid(rt, I(0))) rt.gotoRoom(I(0));
    return Value();
}
GMFN(room_goto_next) {
    int n = rt.roomNext(rt.room);
    if (n >= 0) rt.gotoRoom(n);
    return Value();
}
GMFN(room_goto_previous) {
    int n = rt.roomPrevious(rt.room);
    if (n >= 0) rt.gotoRoom(n);
    return Value();
}
GMFN(room_restart) {
    rt.restartPending = true;
    return Value();
}
GMFN(room_next) { return Value(static_cast<double>(rt.roomNext(I(0)))); }
GMFN(room_previous) { return Value(static_cast<double>(rt.roomPrevious(I(0)))); }
GMFN(room_exists) { return boolV(roomValid(rt, I(0))); }
GMFN(room_get_name) { return Value(roomValid(rt, I(0)) ? std::string(rt.dw.rooms[I(0)].name) : "<undefined>"); }

// Editing rooms (YoYo's Function_Room): changes apply to the stored room, so they show the next
// time it is entered. New rooms are empty and outside the room order.
static Room* roomMut(Runner& rt, int r) { return roomValid(rt, r) ? &rt.dw.rooms[r] : nullptr; }
static std::deque<std::string> gRoomNames;  // names of rooms made at run time
static Value newRoom(Runner& rt, Room room, const char* prefix) {
    gRoomNames.push_back(prefix + std::to_string(rt.dw.rooms.size()));
    room.name = gRoomNames.back();
    rt.dw.rooms.push_back(std::move(room));
    return Value(static_cast<double>(rt.dw.rooms.size() - 1));
}
GMFN(room_add) {
    Room r;
    r.width = 1024;
    r.height = 768;
    r.bgColor = 0xC0C0C0;
    r.views.resize(8, RoomView{false, 0, 0, 640, 480, 0, 0, 640, 480, 32, 32, -1, -1, -1});
    return newRoom(rt, std::move(r), "__newroom");
}
GMFN(room_duplicate) {
    Room* src = roomMut(rt, I(0));
    if (!src) return Value(0.0);
    Room copy = *src;
    return newRoom(rt, std::move(copy), "__newroom");
}
GMFN(room_assign) {  // (target, source): target becomes a copy of source, keeping its name
    Room* dst = roomMut(rt, I(0));
    Room* src = roomMut(rt, I(1));
    if (!dst || !src || dst == src) return Value();
    std::string_view name = dst->name;
    *dst = *src;
    dst->name = name;
    return Value();
}
GMFN(room_set_width) { if (Room* r = roomMut(rt, I(0))) r->width = static_cast<uint32_t>(std::max(0, I(1))); return Value(); }
GMFN(room_set_height) { if (Room* r = roomMut(rt, I(0))) r->height = static_cast<uint32_t>(std::max(0, I(1))); return Value(); }
GMFN(room_set_persistent) { if (Room* r = roomMut(rt, I(0))) r->persistent = B(1); return Value(); }
GMFN(room_set_view_enabled) {
    if (Room* r = roomMut(rt, I(0))) r->flags = B(1) ? (r->flags | 1u) : (r->flags & ~1u);
    return Value();
}
GMFN(room_set_background_color) {  // (room, colour, show)
    if (Room* r = roomMut(rt, I(0))) {
        r->bgColor = static_cast<uint32_t>(C(1));
        r->drawBgColor = B(2);
    }
    return Value();
}
GMFN(room_set_viewport) {  // (room, view, visible, x, y, w, h)
    Room* r = roomMut(rt, I(0));
    int v = I(1);
    if (!r || v < 0 || v >= 8) return Value();
    if (r->views.size() < 8) r->views.resize(8, RoomView{false, 0, 0, 640, 480, 0, 0, 640, 480, 32, 32, -1, -1, -1});
    RoomView& rv = r->views[v];
    rv.enabled = B(2);
    rv.portX = I(3);
    rv.portY = I(4);
    rv.portW = I(5);
    rv.portH = I(6);
    return Value();
}
GMFN(room_get_viewport) {  // [visible, x, y, w, h]
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    Room* r = roomMut(rt, I(0));
    int v = I(1);
    if (r && v >= 0 && static_cast<size_t>(v) < r->views.size()) {
        const RoomView& rv = r->views[v];
        a->rows[0] = {Value::boolean(rv.enabled), Value(double(rv.portX)), Value(double(rv.portY)),
                      Value(double(rv.portW)), Value(double(rv.portH))};
    } else {
        a->rows[0] = {Value(0.0), Value(0.0), Value(0.0), Value(0.0), Value(0.0)};
    }
    return arr;
}
GMFN(room_instance_add) {  // (room, x, y, object): the new instance's id
    Room* r = roomMut(rt, I(0));
    if (!r) return Value(0.0);
    int32_t id = rt.nextInstanceId++;
    r->instances.push_back(RoomInstance{static_cast<int32_t>(D(1)), static_cast<int32_t>(D(2)), I(3),
                                        static_cast<uint32_t>(id), -1, 1, 1, 1, 0, 0xFFFFFFFF, 0, -1});
    return Value(static_cast<double>(id));
}
GMFN(room_instance_clear) {
    if (Room* r = roomMut(rt, I(0))) {
        r->instances.clear();
        for (auto& L : r->layers) L.instanceIds.clear();
    }
    return Value();
}

// ---- sprites and backgrounds
static const Sprite* spr(Runner& rt, int s) {
    return s >= 0 && static_cast<size_t>(s) < rt.dw.sprites.size() && !rt.dw.sprites[s].deleted ? &rt.dw.sprites[s]
                                                                                                  : nullptr;
}
GMFN(sprite_exists) { return boolV(spr(rt, I(0)) != nullptr); }
GMFN(sprite_get_width) { auto* s = spr(rt, I(0)); return Value(s ? static_cast<double>(s->width) : 0.0); }
GMFN(sprite_get_height) { auto* s = spr(rt, I(0)); return Value(s ? static_cast<double>(s->height) : 0.0); }
GMFN(sprite_get_number) { auto* s = spr(rt, I(0)); return Value(s ? static_cast<double>(s->frames.size()) : 0.0); }
GMFN(sprite_get_xoffset) { auto* s = spr(rt, I(0)); return Value(s ? static_cast<double>(s->originX) : 0.0); }
GMFN(sprite_get_yoffset) { auto* s = spr(rt, I(0)); return Value(s ? static_cast<double>(s->originY) : 0.0); }
GMFN(sprite_get_name) { auto* s = spr(rt, I(0)); return Value(s ? std::string(s->name) : "<undefined>"); }
GMFN(sprite_get_bbox_left) { auto* s = spr(rt, I(0)); return Value(s ? static_cast<double>(s->marginLeft) : 0.0); }
GMFN(sprite_get_bbox_right) { auto* s = spr(rt, I(0)); return Value(s ? static_cast<double>(s->marginRight) : 0.0); }
GMFN(sprite_get_bbox_top) { auto* s = spr(rt, I(0)); return Value(s ? static_cast<double>(s->marginTop) : 0.0); }
GMFN(sprite_get_bbox_bottom) { auto* s = spr(rt, I(0)); return Value(s ? static_cast<double>(s->marginBottom) : 0.0); }
GMFN(background_exists) {
    int b = I(0);
    return boolV(b >= 0 && static_cast<size_t>(b) < rt.dw.backgrounds.size());
}
GMFN(background_get_width) { int w, h; rt.gfx->backgroundSize(I(0), w, h); return Value(static_cast<double>(w)); }
GMFN(background_get_height) { int w, h; rt.gfx->backgroundSize(I(0), w, h); return Value(static_cast<double>(h)); }
// Asset name getters: GameMaker returns "<undefined>" for an index that isn't an asset, and
// games loop over assets until they see it.
template <typename T> static Value assetName(const std::vector<T>& list, int i) {
    if (i < 0 || static_cast<size_t>(i) >= list.size()) return Value("<undefined>");
    return Value(std::string(list[i].name));
}
GMFN(tileset_get_name) { return assetName(rt.dw.backgrounds, I(0)); }
GMFN(font_get_name) { return assetName(rt.dw.fonts, I(0)); }
GMFN(path_get_name) { return assetName(rt.dw.paths, I(0)); }
GMFN(shader_get_name) { return assetName(rt.dw.shaders, I(0)); }
// Timelines made or changed at run time (timeline_add, timeline_moment_add_script, ...).
static Timeline* timelineAt(Runner& rt, int i) {
    return i >= 0 && static_cast<size_t>(i) < rt.dw.timelines.size() && !rt.dw.timelines[i].deleted ? &rt.dw.timelines[i]
                                                                                                    : nullptr;
}
GMFN(timeline_add) {
    static std::deque<std::string> names;  // runtime names outlive the timelines that point at them
    names.push_back("__newtimeline" + std::to_string(rt.dw.timelines.size()));
    Timeline t;
    t.name = names.back();
    rt.dw.timelines.push_back(std::move(t));
    return Value(static_cast<double>(rt.dw.timelines.size() - 1));
}
GMFN(timeline_delete) {
    if (Timeline* t = timelineAt(rt, I(0))) {
        t->moments.clear();
        t->deleted = true;
    }
    return Value();
}
GMFN(timeline_exists) { return boolV(timelineAt(rt, I(0)) != nullptr); }
GMFN(timeline_clear) { if (Timeline* t = timelineAt(rt, I(0))) t->moments.clear(); return Value(); }
GMFN(timeline_moment_clear) {
    if (Timeline* t = timelineAt(rt, I(0))) {
        uint32_t step = static_cast<uint32_t>(std::max(0, I(1)));
        t->moments.erase(std::remove_if(t->moments.begin(), t->moments.end(),
                                        [&](const TimelineMoment& m) { return m.step == step; }),
                         t->moments.end());
    }
    return Value();
}
GMFN(timeline_moment_add_script) {  // (timeline, step, script)
    Timeline* t = timelineAt(rt, I(0));
    if (!t) return Value();
    uint32_t step = static_cast<uint32_t>(std::max(0, I(1)));
    int script = I(2);
    if (script >= 100000) script -= 100000;  // a script function id
    auto it = std::lower_bound(t->moments.begin(), t->moments.end(), step,
                               [](const TimelineMoment& m, uint32_t s) { return m.step < s; });
    if (it == t->moments.end() || it->step != step) it = t->moments.insert(it, TimelineMoment{step, {}, {}});
    it->scripts.push_back(script);
    return Value();
}
GMFN(timeline_size) { Timeline* t = timelineAt(rt, I(0)); return Value(t ? static_cast<double>(t->moments.size()) : 0.0); }
GMFN(timeline_max_moment) {
    Timeline* t = timelineAt(rt, I(0));
    return Value(t && !t->moments.empty() ? static_cast<double>(t->moments.back().step) : 0.0);
}
GMFN(timeline_get_name) { return assetName(rt.dw.timelines, I(0)); }
GMFN(background_get_name) {
    int b = I(0);
    if (b < 0 || static_cast<size_t>(b) >= rt.dw.backgrounds.size()) return Value("<undefined>");
    return Value(std::string(rt.dw.backgrounds[b].name));
}

// ---- tiles
static Tile* tile(Runner& rt, int id) {
    for (auto& t : rt.tiles)
        if (t.id == id) return &t;
    return nullptr;
}
GMFN(tile_add) {
    Tile t;
    t.id = rt.nextTileId++;
    t.background = I(0);
    t.left = I(1);
    t.top = I(2);
    t.width = I(3);
    t.height = I(4);
    t.x = D(5);
    t.y = D(6);
    t.depth = D(7);
    rt.tiles.push_back(t);
    return Value(static_cast<double>(t.id));
}
GMFN(tile_delete) {
    int id = I(0);
    rt.tiles.erase(std::remove_if(rt.tiles.begin(), rt.tiles.end(), [&](const Tile& t) { return t.id == id; }),
                   rt.tiles.end());
    return Value();
}
GMFN(tile_exists) { return boolV(tile(rt, I(0)) != nullptr); }
// GMS2 layer_tile_* (legacy tiles on asset layers, kept with the room's tiles at the layer's depth).
GMFN(layer_tile_create) {  // (layer, x, y, sprite, left, top, width, height)
    Layer* L = rt.layerFromValue(ARG(0));
    Tile t;
    t.id = rt.nextTileId++;
    t.background = I(3);
    t.x = D(1);
    t.y = D(2);
    t.left = I(4);
    t.top = I(5);
    t.width = I(6);
    t.height = I(7);
    t.depth = L ? L->depth : 0;
    t.layer = L ? L->id : -1;
    rt.tiles.push_back(t);
    return Value(static_cast<double>(t.id));
}
GMFN(layer_tile_destroy) { return fn_tile_delete(rt, self, other, argc, args); }
GMFN(layer_tile_exists) { return boolV(tile(rt, I(argc > 1 ? 1 : 0)) != nullptr); }  // ([layer], tile)
#define TILE_SET(name, body) \
    GMFN(name) {             \
        if (Tile* t = tile(rt, I(0))) { body; } \
        return Value();      \
    }
#define TILE_GET(name, expr, def) \
    GMFN(name) {                  \
        Tile* t = tile(rt, I(0)); \
        return t ? Value(expr) : Value(def); \
    }
TILE_SET(layer_tile_x, t->x = D(1))
TILE_SET(layer_tile_y, t->y = D(1))
TILE_SET(layer_tile_xscale, t->xscale = D(1))
TILE_SET(layer_tile_yscale, t->yscale = D(1))
TILE_SET(layer_tile_blend, t->blend = C(1))
TILE_SET(layer_tile_alpha, t->alpha = D(1))
TILE_SET(layer_tile_visible, t->visible = B(1))
TILE_SET(layer_tile_change, t->background = I(1))
TILE_SET(layer_tile_region, t->left = I(1); t->top = I(2); t->width = I(3); t->height = I(4))
TILE_GET(layer_tile_get_x, t->x, 0.0)
TILE_GET(layer_tile_get_y, t->y, 0.0)
TILE_GET(layer_tile_get_xscale, t->xscale, 1.0)
TILE_GET(layer_tile_get_yscale, t->yscale, 1.0)
TILE_GET(layer_tile_get_blend, static_cast<double>(t->blend), 0.0)
TILE_GET(layer_tile_get_alpha, t->alpha, 0.0)
TILE_GET(layer_tile_get_visible, Value::boolean(t->visible), Value::boolean(false))
TILE_GET(layer_tile_get_sprite, static_cast<double>(t->background), -1.0)
GMFN(layer_tile_get_region) {
    Tile* t = tile(rt, I(0));
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    if (t) a->rows[0] = {Value(double(t->left)), Value(double(t->top)), Value(double(t->width)), Value(double(t->height))};
    return arr;
}
GMFN(tile_get_ids) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    for (size_t i = 0; i < rt.tiles.size(); i++) a->set(static_cast<int64_t>(i), Value(static_cast<double>(rt.tiles[i].id)));
    return arr;
}
GMFN(tile_get_ids_at_depth) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    int64_t n = 0;
    for (auto& t : rt.tiles)
        if (t.depth == D(0)) a->set(n++, Value(static_cast<double>(t.id)));
    return arr;
}
GMFN(tile_layer_find) {
    double depth = D(0), x = D(1), y = D(2);
    for (auto& t : rt.tiles)
        if (t.depth == depth && x >= t.x && y >= t.y && x < t.x + t.width * t.xscale && y < t.y + t.height * t.yscale)
            return Value(static_cast<double>(t.id));
    return Value(-1.0);
}
GMFN(tile_layer_depth) {
    for (auto& t : rt.tiles)
        if (t.depth == D(0)) t.depth = D(1);
    return Value();
}
GMFN(tile_layer_delete) {
    double d = D(0);
    rt.tiles.erase(std::remove_if(rt.tiles.begin(), rt.tiles.end(), [&](const Tile& t) { return t.depth == d; }),
                   rt.tiles.end());
    return Value();
}
GMFN(tile_layer_hide) {
    for (auto& t : rt.tiles)
        if (t.depth == D(0)) t.visible = false;
    return Value();
}
GMFN(tile_layer_show) {
    for (auto& t : rt.tiles)
        if (t.depth == D(0)) t.visible = true;
    return Value();
}
GMFN(tile_layer_shift) {
    for (auto& t : rt.tiles)
        if (t.depth == D(0)) {
            t.x += D(1);
            t.y += D(2);
        }
    return Value();
}
GMFN(tile_set_background) { if (Tile* t = tile(rt, I(0))) t->background = I(1); return Value(); }
GMFN(tile_set_position) { if (Tile* t = tile(rt, I(0))) { t->x = D(1); t->y = D(2); } return Value(); }
GMFN(tile_set_depth) { if (Tile* t = tile(rt, I(0))) t->depth = D(1); return Value(); }
GMFN(tile_set_visible) { if (Tile* t = tile(rt, I(0))) t->visible = B(1); return Value(); }
GMFN(tile_set_alpha) { if (Tile* t = tile(rt, I(0))) t->alpha = D(1); return Value(); }
GMFN(tile_set_blend) { if (Tile* t = tile(rt, I(0))) t->blend = C(1); return Value(); }
GMFN(tile_set_scale) { if (Tile* t = tile(rt, I(0))) { t->xscale = D(1); t->yscale = D(2); } return Value(); }
GMFN(tile_set_region) {
    if (Tile* t = tile(rt, I(0))) {
        t->left = I(1);
        t->top = I(2);
        t->width = I(3);
        t->height = I(4);
    }
    return Value();
}
GMFN(tile_get_x) { Tile* t = tile(rt, I(0)); return Value(t ? t->x : 0.0); }
GMFN(tile_get_y) { Tile* t = tile(rt, I(0)); return Value(t ? t->y : 0.0); }
GMFN(tile_get_depth) { Tile* t = tile(rt, I(0)); return Value(t ? t->depth : 0.0); }
GMFN(tile_get_background) { Tile* t = tile(rt, I(0)); return Value(t ? static_cast<double>(t->background) : -1.0); }
GMFN(tile_get_width) { Tile* t = tile(rt, I(0)); return Value(t ? static_cast<double>(t->width) : 0.0); }
GMFN(tile_get_height) { Tile* t = tile(rt, I(0)); return Value(t ? static_cast<double>(t->height) : 0.0); }
GMFN(tile_get_left) { Tile* t = tile(rt, I(0)); return Value(t ? static_cast<double>(t->left) : 0.0); }
GMFN(tile_get_top) { Tile* t = tile(rt, I(0)); return Value(t ? static_cast<double>(t->top) : 0.0); }
GMFN(tile_get_xscale) { Tile* t = tile(rt, I(0)); return Value(t ? t->xscale : 1.0); }
GMFN(tile_get_yscale) { Tile* t = tile(rt, I(0)); return Value(t ? t->yscale : 1.0); }
GMFN(tile_get_alpha) { Tile* t = tile(rt, I(0)); return Value(t ? t->alpha : 1.0); }
GMFN(tile_get_blend) { Tile* t = tile(rt, I(0)); return Value(t ? static_cast<double>(t->blend) : 16777215.0); }
GMFN(tile_get_visible) { Tile* t = tile(rt, I(0)); return Value::boolean(t && t->visible); }

// ---- paths
GMFN(path_start) {
    if (!self) return Value();
    int p = I(0);
    if (!rt.pathAt(p)) return Value();
    // As GameMaker (Assign_Path): negative speeds start at the end; an absolute path moves the
    // instance onto the path, a relative one anchors the path's first point at the instance.
    self->pathIndex = p;
    self->pathSpeed = D(1);
    self->pathEndAction = I(2);
    self->pathPosition = self->pathPositionPrevious = self->pathSpeed >= 0 ? 0 : 1;
    self->pathScale = 1;
    self->pathOrientation = 0;
    if (B(3)) {
        double x0, y0, x1, y1, sp;
        pathPoint(rt, p, 0, x0, y0, sp);
        pathPoint(rt, p, 1, x1, y1, sp);
        self->x = self->pathSpeed >= 0 ? x0 : x1;
        self->y = self->pathSpeed >= 0 ? y0 : y1;
        self->pathXStart = x0;
        self->pathYStart = y0;
    } else {
        self->pathXStart = self->x;
        self->pathYStart = self->y;
    }
    return Value();
}
GMFN(path_end) {
    if (self) {
        self->pathIndex = -1;
        self->setHVSpeed(0, 0);
    }
    return Value();
}
GMFN(path_exists) { return boolV(rt.pathAt(I(0)) != nullptr); }
GMFN(path_get_length) { return Value(pathLength(rt, I(0))); }
GMFN(path_get_x) { double x = 0, y = 0, s = 0; pathPoint(rt, I(0), D(1), x, y, s); return Value(x); }
GMFN(path_get_y) { double x = 0, y = 0, s = 0; pathPoint(rt, I(0), D(1), x, y, s); return Value(y); }

void registerInstanceBuiltins(Runner& rt) {
    GMREG(object_set_sprite); GMREG(object_set_visible); GMREG(object_set_solid); GMREG(object_set_persistent);
    GMREG(object_set_mask); GMREG(object_get_physics); GMREG(instance_id_get); GMREG(instance_activate_layer);
    GMREG(instance_deactivate_layer); GMREG(room_add); GMREG(room_duplicate); GMREG(room_assign);
    GMREG(room_set_width); GMREG(room_set_height); GMREG(room_set_persistent); GMREG(room_set_view_enabled);
    GMREG(room_set_background_color); GMALIAS(room_set_background_colour, room_set_background_color);
    GMREG(room_set_viewport); GMREG(room_get_viewport); GMREG(room_instance_add); GMREG(room_instance_clear); GMREG(timeline_add); GMREG(timeline_delete); GMREG(timeline_exists);
    GMREG(timeline_clear); GMREG(timeline_moment_clear); GMREG(timeline_moment_add_script); GMREG(timeline_size);
    GMREG(timeline_max_moment); GMALIAS(timeline_name, timeline_get_name);
    rt.vm.registerBuiltin("@@GetInstance@@", fn_gml_get_instance);
    GMREG(instance_create); GMREG(instance_destroy); GMREG(instance_exists); GMREG(instance_number);
    GMREG(instance_find); GMREG(instance_nearest); GMREG(instance_furthest); GMREG(instance_change);
    GMREG(instance_copy); GMREG(place_meeting); GMREG(move_and_collide); GMREG(instance_place); GMREG(place_free); GMREG(place_empty);
    GMREG(position_meeting); GMREG(instance_position); GMREG(position_empty); GMREG(position_destroy);
    GMREG(collision_rectangle); GMREG(collision_point); GMREG(collision_line); GMREG(collision_circle);
    GMREG(collision_ellipse); GMREG(distance_to_point); GMREG(distance_to_object); GMREG(motion_set);
    GMREG(motion_add); GMREG(move_towards_point); GMREG(move_snap); GMREG(move_wrap);
    GMREG(move_contact_solid); GMREG(move_contact_all); GMREG(move_outside_solid); GMREG(move_outside_all);
    GMREG(move_bounce_solid); GMREG(move_bounce_all); GMREG(place_snapped); GMREG(move_random);
    GMREG(instance_deactivate_all); GMREG(instance_activate_all); GMREG(instance_activate_object);
    GMREG(instance_deactivate_object); GMREG(instance_activate_region); GMREG(instance_deactivate_region);
    GMREG(event_perform); GMREG(event_perform_object); GMREG(event_user); GMREG(event_inherited);
    GMREG(object_exists); GMREG(object_get_name); GMREG(object_get_parent); GMREG(object_is_ancestor);
    GMREG(object_get_sprite); GMREG(object_get_depth); GMREG(object_get_visible); GMREG(object_get_solid);
    GMREG(object_get_persistent); GMREG(object_get_mask);
    GMREG(room_goto); GMREG(room_goto_next); GMREG(room_goto_previous); GMREG(room_restart); GMREG(room_next);
    GMREG(room_previous); GMREG(room_exists); GMREG(room_get_name);
    GMREG(sprite_exists); GMREG(sprite_get_width); GMREG(sprite_get_height); GMREG(sprite_get_number);
    GMREG(sprite_get_xoffset); GMREG(sprite_get_yoffset); GMREG(sprite_get_name); GMREG(sprite_get_bbox_left);
    GMREG(sprite_get_bbox_right); GMREG(sprite_get_bbox_top); GMREG(sprite_get_bbox_bottom);
    GMREG(background_exists); GMREG(background_get_width); GMREG(background_get_height); GMREG(background_get_name);
    GMREG(tileset_get_name); GMREG(font_get_name); GMREG(path_get_name); GMREG(shader_get_name);
    GMREG(timeline_get_name);
    GMREG(layer_tile_create); GMREG(layer_tile_destroy); GMREG(layer_tile_exists); GMREG(layer_tile_x);
    GMREG(layer_tile_y); GMREG(layer_tile_xscale); GMREG(layer_tile_yscale); GMREG(layer_tile_blend);
    GMREG(layer_tile_alpha); GMREG(layer_tile_visible); GMREG(layer_tile_change); GMREG(layer_tile_region);
    GMREG(layer_tile_get_x); GMREG(layer_tile_get_y); GMREG(layer_tile_get_xscale); GMREG(layer_tile_get_yscale);
    GMREG(layer_tile_get_blend); GMREG(layer_tile_get_alpha); GMREG(layer_tile_get_visible);
    GMREG(layer_tile_get_sprite); GMREG(layer_tile_get_region);
    GMREG(tile_add); GMREG(tile_delete); GMREG(tile_exists); GMREG(tile_get_ids); GMREG(tile_get_ids_at_depth);
    GMREG(tile_layer_find); GMREG(tile_layer_depth); GMREG(tile_layer_delete); GMREG(tile_layer_hide);
    GMREG(tile_layer_show); GMREG(tile_layer_shift); GMREG(tile_set_background); GMREG(tile_set_position);
    GMREG(tile_set_depth); GMREG(tile_set_visible); GMREG(tile_set_alpha); GMREG(tile_set_blend);
    GMREG(tile_set_scale); GMREG(tile_set_region); GMREG(tile_get_x); GMREG(tile_get_y); GMREG(tile_get_left); GMREG(tile_get_top); GMREG(tile_get_xscale);
    GMREG(tile_get_yscale); GMREG(tile_get_alpha); GMREG(tile_get_blend); GMREG(tile_get_visible); GMREG(tile_get_depth);
    GMREG(tile_get_background); GMREG(tile_get_width); GMREG(tile_get_height);
    GMREG(path_start); GMREG(path_end); GMREG(path_exists); GMREG(path_get_length); GMREG(path_get_x);
    GMREG(path_get_y);
}

}  // namespace gm
