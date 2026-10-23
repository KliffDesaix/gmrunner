// Bounding boxes, precise mask collisions and path following.
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "Runner.h"

namespace gm {

int32_t Runner::maskSprite(const Instance* inst) const {
    int32_t s = inst->mask >= 0 ? inst->mask : inst->sprite;
    if (s < 0 || static_cast<size_t>(s) >= dw.sprites.size()) return -1;
    return s;
}

int Runner::imageNumber(const Instance* inst) const {
    if (inst->sprite < 0 || static_cast<size_t>(inst->sprite) >= dw.sprites.size()) return 0;
    if (dw.sprites[inst->sprite].skeleton >= 0) {  // a skeleton: the frames of its animation
        Runner& self = const_cast<Runner&>(*this);
        if (isSkeletonSprite(self, inst->sprite)) return skeletonFrameCount(self, inst);
    }
    return static_cast<int>(dw.sprites[inst->sprite].frames.size());
}

Runner::BBox Runner::bboxAt(const Instance* inst, double x, double y) const {
    int32_t s = maskSprite(inst);
    if (s < 0) return {x, y, x, y, false};
    const Sprite& sp = dw.sprites[s];
    double l = sp.marginLeft - sp.originX, r = sp.marginRight + 1 - sp.originX;
    double t = sp.marginTop - sp.originY, b = sp.marginBottom + 1 - sp.originY;
    double xs = inst->imageXscale, ys = inst->imageYscale;
    double cx[4] = {l * xs, r * xs, r * xs, l * xs};
    double cy[4] = {t * ys, t * ys, b * ys, b * ys};
    if (inst->imageAngle != 0) {
        double a = inst->imageAngle * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
        for (int i = 0; i < 4; i++) {
            double px = cx[i], py = cy[i];
            cx[i] = px * ca + py * sa;
            cy[i] = -px * sa + py * ca;
        }
    }
    BBox bb{x + *std::min_element(cx, cx + 4), y + *std::min_element(cy, cy + 4),
            x + *std::max_element(cx, cx + 4), y + *std::max_element(cy, cy + 4), true};
    return bb;
}

Runner::BBox Runner::bbox(const Instance* inst) const { return bboxAt(inst, inst->x, inst->y); }

bool Runner::tilemapPlace(const Instance* inst, double x, double y, int32_t element) {
    const LayerElement* e = elementById(element);
    if (!e || e->type != ElementType::Tilemap || maskSprite(inst) < 0) return false;
    if (e->tileset < 0 || static_cast<size_t>(e->tileset) >= dw.backgrounds.size()) return false;
    const Background& ts = dw.backgrounds[e->tileset];
    if (!ts.tileWidth || !ts.tileHeight || !e->tilesX || !e->tilesY) return false;
    double ox = e->x, oy = e->y;
    if (const Layer* L = layerById(e->layer)) ox += L->x, oy += L->y;
    const double tw = ts.tileWidth, th = ts.tileHeight;
    BBox bb = bboxAt(inst, x, y);
    double l = std::max(bb.left, ox), t = std::max(bb.top, oy);
    double r = std::min(bb.right, ox + e->tilesX * tw), b = std::min(bb.bottom, oy + e->tilesY * th);
    if (r <= l || b <= t) return false;
    int c0 = std::max(0, static_cast<int>((l - ox) / tw)), r0 = std::max(0, static_cast<int>((t - oy) / th));
    int c1 = std::min(static_cast<int>(e->tilesX) - 1, static_cast<int>((r - ox) / tw));
    int r1 = std::min(static_cast<int>(e->tilesY) - 1, static_cast<int>((b - oy) / th));
    for (int cx = c0; cx <= c1; cx++)
        for (int cy = r0; cy <= r1; cy++) {
            uint32_t data = e->tiles[static_cast<size_t>(cy) * e->tilesX + cx] & e->tileMask;
            if ((data & 0x7FFFF) == 0) continue;
            // As precise collisions: the overlap must cross a pixel centre.
            double tl = ox + cx * tw, tt = oy + cy * th;
            double il = std::max(bb.left, tl), ir = std::min(bb.right, tl + tw);
            double it = std::max(bb.top, tt), ib = std::min(bb.bottom, tt + th);
            if (std::floor(il + 0.49999) == std::floor(ir + 0.5)) continue;
            if (std::floor(it + 0.49999) == std::floor(ib + 0.5)) continue;
            return true;
        }
    return false;
}

namespace {

// GameMaker rounds bounding boxes to whole pixels (right/bottom inclusive).
struct IBox {
    int l, t, r, b;
};
IBox toInt(const Runner::BBox& bb) {
    return {static_cast<int>(std::lround(bb.left)), static_cast<int>(std::lround(bb.top)),
            static_cast<int>(std::lround(bb.right)) - 1, static_cast<int>(std::lround(bb.bottom)) - 1};
}

// Tests whether world point (wx, wy) hits the instance's collision mask.
bool maskHit(const DataWin& dw, int32_t spr, const Instance* inst, double ix, double iy, double wx, double wy) {
    const Sprite& sp = dw.sprites[spr];
    if (sp.masks.empty()) return true;
    double dx = wx - ix, dy = wy - iy;
    if (inst->imageAngle != 0) {
        double a = inst->imageAngle * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
        double rx = dx * ca - dy * sa, ry = dx * sa + dy * ca;
        dx = rx;
        dy = ry;
    }
    if (inst->imageXscale == 0 || inst->imageYscale == 0) return false;
    double lx = dx / inst->imageXscale + sp.originX, ly = dy / inst->imageYscale + sp.originY;
    int px = static_cast<int>(std::floor(lx)), py = static_cast<int>(std::floor(ly));
    if (px < 0 || py < 0 || px >= static_cast<int>(sp.width) || py >= static_cast<int>(sp.height)) return false;
    size_t frame = 0;
    if (sp.masks.size() > 1) {
        int n = static_cast<int>(sp.masks.size());
        int f = static_cast<int>(std::floor(inst->imageIndex)) % n;
        if (f < 0) f += n;
        frame = static_cast<size_t>(f);
    }
    const uint8_t* m = sp.masks[frame];
    size_t stride = (sp.width + 7) / 8;
    return (m[py * stride + px / 8] >> (7 - (px & 7))) & 1;
}

}  // namespace

bool Runner::instancesCollide(const Instance* a, double ax, double ay, const Instance* b) const {
    int32_t sa = maskSprite(a), sb = maskSprite(b);
    if (sa < 0 || sb < 0) return false;
    IBox A = toInt(bboxAt(a, ax, ay)), B = toInt(bbox(b));
    if (A.l > B.r || B.l > A.r || A.t > B.b || B.t > A.b) return false;
    bool aRect = dw.sprites[sa].masks.empty(), bRect = dw.sprites[sb].masks.empty();
    if (aRect && bRect) return true;
    int l = std::max(A.l, B.l), r = std::min(A.r, B.r), t = std::max(A.t, B.t), bt = std::min(A.b, B.b);
    for (int y = t; y <= bt; y++)
        for (int x = l; x <= r; x++) {
            double wx = x + 0.5, wy = y + 0.5;
            if (maskHit(dw, sa, a, ax, ay, wx, wy) && maskHit(dw, sb, b, b->x, b->y, wx, wy)) return true;
        }
    return false;
}

bool Runner::pointInInstance(const Instance* inst, double px, double py, bool precise) const {
    int32_t s = maskSprite(inst);
    if (s < 0) return false;
    IBox B = toInt(bbox(inst));
    int ix = static_cast<int>(std::floor(px)), iy = static_cast<int>(std::floor(py));
    if (ix < B.l || ix > B.r || iy < B.t || iy > B.b) return false;
    if (!precise) return true;
    return maskHit(dw, s, inst, inst->x, inst->y, ix + 0.5, iy + 0.5);
}

bool Runner::rectInInstance(const Instance* inst, double x1, double y1, double x2, double y2, bool precise) const {
    int32_t s = maskSprite(inst);
    if (s < 0) return false;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    IBox B = toInt(bbox(inst));
    int l = static_cast<int>(std::lround(x1)), r = static_cast<int>(std::lround(x2));
    int t = static_cast<int>(std::lround(y1)), b = static_cast<int>(std::lround(y2));
    if (l > B.r || B.l > r || t > B.b || B.t > b) return false;
    if (!precise) return true;
    for (int y = std::max(t, B.t); y <= std::min(b, B.b); y++)
        for (int x = std::max(l, B.l); x <= std::min(r, B.r); x++)
            if (maskHit(dw, s, inst, inst->x, inst->y, x + 0.5, y + 0.5)) return true;
    return false;
}

bool Runner::circleInInstance(const Instance* inst, double cx, double cy, double rx, double ry, bool precise) const {
    int32_t s = maskSprite(inst);
    if (s < 0 || rx <= 0 || ry <= 0) return false;
    IBox B = toInt(bbox(inst));
    int l = static_cast<int>(std::floor(cx - rx)), r = static_cast<int>(std::ceil(cx + rx));
    int t = static_cast<int>(std::floor(cy - ry)), b = static_cast<int>(std::ceil(cy + ry));
    if (l > B.r || B.l > r || t > B.b || B.t > b) return false;
    for (int y = std::max(t, B.t); y <= std::min(b, B.b); y++)
        for (int x = std::max(l, B.l); x <= std::min(r, B.r); x++) {
            double dx = (x + 0.5 - cx) / rx, dy = (y + 0.5 - cy) / ry;
            if (dx * dx + dy * dy > 1.0) continue;
            if (!precise || maskHit(dw, s, inst, inst->x, inst->y, x + 0.5, y + 0.5)) return true;
        }
    return false;
}

bool Runner::lineInInstance(const Instance* inst, double x1, double y1, double x2, double y2, bool precise) const {
    int32_t s = maskSprite(inst);
    if (s < 0) return false;
    BBox bb = bbox(inst);
    // Liang-Barsky clip of the segment against the bounding box.
    double t0 = 0, t1 = 1, dx = x2 - x1, dy = y2 - y1;
    double p[4] = {-dx, dx, -dy, dy};
    double q[4] = {x1 - bb.left, bb.right - x1, y1 - bb.top, bb.bottom - y1};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
        } else {
            double r = q[i] / p[i];
            if (p[i] < 0) t0 = std::max(t0, r);
            else t1 = std::min(t1, r);
        }
    }
    if (t0 > t1) return false;
    if (!precise) return true;
    double len = std::sqrt(dx * dx + dy * dy) * (t1 - t0);
    int steps = std::max(1, static_cast<int>(std::ceil(len)));
    for (int i = 0; i <= steps; i++) {
        double t = t0 + (t1 - t0) * i / steps;
        if (maskHit(dw, s, inst, inst->x, inst->y, x1 + dx * t, y1 + dy * t)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Paths

namespace {

struct PathPolyline {
    std::vector<PathPoint> pts;
    std::vector<double> cum;  // cumulative length at each point
    double length = 0;
};

PathPolyline buildPolyline(const Path& p) {
    PathPolyline out;
    const auto& src = p.points;
    if (src.empty()) return out;
    if (!p.smooth || src.size() < 3) {
        out.pts = src;
        if (p.closed && src.size() > 1) out.pts.push_back(src[0]);
    } else {
        // Quadratic curves between segment midpoints, with each point as the control point.
        std::vector<PathPoint> s = src;
        if (p.closed) {
            s.push_back(src[0]);
            s.push_back(src[1]);
        }
        int sub = 4 * static_cast<int>(std::max<uint32_t>(1, p.precision));
        auto mid = [](const PathPoint& a, const PathPoint& b) {
            return PathPoint{(a.x + b.x) / 2, (a.y + b.y) / 2, (a.speed + b.speed) / 2};
        };
        PathPoint start = p.closed ? mid(s[0], s[1]) : s[0];
        out.pts.push_back(start);
        for (size_t i = 1; i + 1 < s.size(); i++) {
            PathPoint a = (i == 1 && !p.closed) ? s[0] : mid(s[i - 1], s[i]);
            PathPoint c = (i + 2 == s.size() && !p.closed) ? s[i + 1] : mid(s[i], s[i + 1]);
            const PathPoint& ctl = s[i];
            for (int k = 1; k <= sub; k++) {
                float t = static_cast<float>(k) / sub, u = 1 - t;
                out.pts.push_back({u * u * a.x + 2 * u * t * ctl.x + t * t * c.x,
                                   u * u * a.y + 2 * u * t * ctl.y + t * t * c.y,
                                   u * u * a.speed + 2 * u * t * ctl.speed + t * t * c.speed});
            }
        }
    }
    out.cum.resize(out.pts.size(), 0);
    for (size_t i = 1; i < out.pts.size(); i++) {
        double dx = out.pts[i].x - out.pts[i - 1].x, dy = out.pts[i].y - out.pts[i - 1].y;
        out.cum[i] = out.cum[i - 1] + std::sqrt(dx * dx + dy * dy);
    }
    out.length = out.cum.empty() ? 0 : out.cum.back();
    return out;
}

const PathPolyline& polyline(const Runner& rt, int32_t index) {
    static std::unordered_map<int32_t, PathPolyline> cache;
    static uint64_t version = ~0ull;
    if (version != rt.pathVersion) {
        cache.clear();
        version = rt.pathVersion;
    }
    auto it = cache.find(index);
    if (it != cache.end()) return it->second;
    const Path* p = rt.pathAt(index);
    return cache[index] = p ? buildPolyline(*p) : PathPolyline();
}

PathPoint pointAt(const PathPolyline& pl, double pos) {
    if (pl.pts.empty()) return {0, 0, 100};
    if (pl.pts.size() == 1 || pl.length <= 0) return pl.pts[0];
    double d = std::max(0.0, std::min(1.0, pos)) * pl.length;
    size_t i = std::upper_bound(pl.cum.begin(), pl.cum.end(), d) - pl.cum.begin();
    if (i == 0) return pl.pts[0];
    if (i >= pl.pts.size()) return pl.pts.back();
    double seg = pl.cum[i] - pl.cum[i - 1];
    float t = seg > 0 ? static_cast<float>((d - pl.cum[i - 1]) / seg) : 0.f;
    const PathPoint &a = pl.pts[i - 1], &b = pl.pts[i];
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.speed + (b.speed - a.speed) * t};
}

}  // namespace

double pathLength(const Runner& rt, int32_t index) {
    if (!rt.pathAt(index)) return 0;
    return polyline(rt, index).length;
}

bool pathPoint(const Runner& rt, int32_t index, double pos, double& x, double& y, double& speed) {
    if (!rt.pathAt(index)) return false;
    PathPoint p = pointAt(polyline(rt, index), pos);
    x = p.x;
    y = p.y;
    speed = p.speed;
    return true;
}

// Path following for one instance (as GameMaker's Adapt_Path). Returns true when it reached
// an end of the path while moving (Path End event). The instance is placed on the path; hspeed
// and vspeed are set to that step's movement (so direction follows the path) and speed to 0.
bool Runner::adaptPath(Instance* inst) {
    if (inst->pathIndex < 0 || !pathAt(inst->pathIndex)) return false;
    const PathPolyline& pl = polyline(*this, inst->pathIndex);
    if (pl.length <= 0) return false;
    double orient = inst->pathOrientation * M_PI / 180.0;
    PathPoint cur = pointAt(pl, inst->pathPosition);
    double sp = cur.speed / (100.0 * inst->pathScale);
    inst->pathPosition += inst->pathSpeed * sp / pl.length;
    PathPoint p0 = pointAt(pl, 0);
    bool atEnd = false;
    if (inst->pathPosition >= 1 || inst->pathPosition <= 0) {
        atEnd = inst->pathSpeed != 0;
        switch (inst->pathEndAction) {
            case 1:  // continue from the start
                inst->pathPosition += inst->pathPosition < 0 ? 1 : -1;
                break;
            case 2: {  // continue from the current position
                PathPoint p1 = pointAt(pl, 1);
                double xx = p1.x - p0.x, yy = p1.y - p0.y;
                double xd = inst->pathScale * (xx * std::cos(orient) + yy * std::sin(orient));
                double yd = inst->pathScale * (yy * std::cos(orient) - xx * std::sin(orient));
                if (inst->pathPosition < 0) {
                    inst->pathXStart -= xd;
                    inst->pathYStart -= yd;
                    inst->pathPosition += 1;
                } else {
                    inst->pathXStart += xd;
                    inst->pathYStart += yd;
                    inst->pathPosition -= 1;
                }
                break;
            }
            case 3:  // reverse
                if (inst->pathPosition < 0) {
                    inst->pathPosition = -inst->pathPosition;
                    inst->pathSpeed = std::fabs(inst->pathSpeed);
                } else {
                    inst->pathPosition = 2 - inst->pathPosition;
                    inst->pathSpeed = -std::fabs(inst->pathSpeed);
                }
                break;
            default:  // stop
                if (inst->pathSpeed != 0) {
                    inst->pathPosition = 1;
                    inst->pathIndex = -1;
                }
                break;
        }
    }
    PathPoint p = pointAt(pl, inst->pathPosition);
    double xx = p.x - p0.x, yy = p.y - p0.y;
    double nx = inst->pathXStart + inst->pathScale * (xx * std::cos(orient) + yy * std::sin(orient));
    double ny = inst->pathYStart + inst->pathScale * (yy * std::cos(orient) - xx * std::sin(orient));
    inst->setHVSpeed(nx - inst->x, ny - inst->y);
    inst->setSpeedDir(0, inst->direction);
    inst->x = nx;
    inst->y = ny;
    return atEnd;
}

}  // namespace gm
