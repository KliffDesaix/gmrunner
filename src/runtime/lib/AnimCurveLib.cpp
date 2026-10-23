// Animation curves (animcurve_*). Curves, channels and points are GML structs, as in GameMaker,
// so games can read and edit them; evaluation samples a channel the way YoYo's runner does
// (yyAnimCurve.js): straight lines, centripetal Catmull-Rom or bezier segments, then linear
// interpolation between the samples.
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

#include "runtime/Builtins.h"

namespace gm {

namespace {

std::vector<Value> g_curves;  // curve structs by id (data curves first, then animcurve_create)
std::vector<bool> g_curveLive;
std::unordered_map<const GMObject*, std::array<float, 4>> g_handles;  // point struct -> bezier handles

void setField(Runner& rt, const Value& s, const char* name, Value v) {
    rt.vm.setInstanceVar(s.obj()->inst, rt.vm.varIdForName(name), std::move(v));
}
Value field(Runner& rt, const Value& s, const char* name) {
    return s.obj() ? rt.vm.getInstanceVar(s.obj()->inst, rt.vm.varIdForName(name)) : Value();
}
Value newArray(std::vector<Value> items) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    a->rows[0] = std::move(items);
    return arr;
}
std::vector<Value> items(const Value& arr) {
    GMArray* a = arr.arr();
    return a && !a->rows.empty() ? a->rows[0] : std::vector<Value>{};
}

Value makePoint(Runner& rt, double x, double v) {
    Value p = rt.vm.newStruct(nullptr);
    setField(rt, p, "posx", Value(x));
    setField(rt, p, "value", Value(v));
    return p;
}
Value makeChannel(Runner& rt, const std::string& name, int type, int iterations, std::vector<Value> points) {
    Value c = rt.vm.newStruct(nullptr);
    setField(rt, c, "name", Value(name));
    setField(rt, c, "type", Value(static_cast<double>(type)));
    setField(rt, c, "iterations", Value(static_cast<double>(iterations)));
    setField(rt, c, "points", newArray(std::move(points)));
    return c;
}
Value makeCurve(Runner& rt, const std::string& name, int graphType, std::vector<Value> channels) {
    Value c = rt.vm.newStruct(nullptr);
    setField(rt, c, "name", Value(name));
    setField(rt, c, "graphType", Value(static_cast<double>(graphType)));
    setField(rt, c, "channels", newArray(std::move(channels)));
    return c;
}

Value fromData(Runner& rt, const AnimCurve& ac) {
    std::vector<Value> channels;
    for (const AnimCurveChannel& ch : ac.channels) {
        std::vector<Value> points;
        for (const AnimCurvePoint& p : ch.points) {
            Value pv = makePoint(rt, p.x, p.value);
            g_handles[pv.obj()] = {p.tx0, p.ty0, p.tx1, p.ty1};
            points.push_back(pv);
        }
        channels.push_back(makeChannel(rt, std::string(ch.name), ch.type, static_cast<int>(ch.iterations), points));
    }
    return makeCurve(rt, std::string(ac.name), ac.graphType, channels);
}

void ensureLoaded(Runner& rt) {
    if (!g_curves.empty() || rt.dw.animCurves.empty()) return;
    for (const AnimCurve& ac : rt.dw.animCurves) {
        g_curves.push_back(fromData(rt, ac));
        g_curveLive.push_back(true);
    }
}

// A curve given as a struct or an id (asset index).
Value curveArg(Runner& rt, const Value& v) {
    ensureLoaded(rt);
    if (v.obj()) return v;
    int id = v.toInt();
    if (id >= 0 && static_cast<size_t>(id) < g_curves.size() && g_curveLive[id]) return g_curves[id];
    return Value();
}

struct Sample {
    double x, v;
};

double catmullTime(double t, double x1, double y1, double x2, double y2) {
    double a = (x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1);
    if (a == 0.0) a = 0.0001;
    return std::pow(std::sqrt(a), 0.5) + t;
}

std::vector<Sample> sampleChannel(Runner& rt, const Value& channel) {
    struct Pt {
        double x, v;
        std::array<float, 4> h;
    };
    std::vector<Pt> pts;
    for (const Value& p : items(field(rt, channel, "points"))) {
        auto it = g_handles.find(p.obj());
        pts.push_back({field(rt, p, "posx").num(), field(rt, p, "value").num(),
                       it != g_handles.end() ? it->second : std::array<float, 4>{}});
    }
    int type = field(rt, channel, "type").toInt();
    int iterations = std::max(1, field(rt, channel, "iterations").toInt());
    std::vector<Sample> out;
    size_t n = pts.size();
    if (type == 1 && n >= 2) {  // centripetal Catmull-Rom, y normalised to 0..1 while sampling
        double miny = pts[0].v, maxy = miny;
        for (auto& p : pts) miny = std::min(miny, p.v), maxy = std::max(maxy, p.v);
        double scale = maxy - miny > 1.0 ? maxy - miny : 1.0, offset = miny;
        std::vector<double> sx, sy;
        sx.push_back(pts[0].x);
        sy.push_back((pts[0].v - offset) / scale);
        for (auto& p : pts) sx.push_back(p.x), sy.push_back((p.v - offset) / scale);
        sx.push_back(pts[n - 1].x);
        sy.push_back((pts[n - 1].v - offset) / scale);
        for (size_t i = 0; i + 3 < sx.size(); i++) {
            double p0x = sx[i], p1x = sx[i + 1], p2x = sx[i + 2], p3x = sx[i + 3];
            double p0y = sy[i], p1y = sy[i + 1], p2y = sy[i + 2], p3y = sy[i + 3];
            double t1 = catmullTime(0, p0x, p0y, p1x, p1y), t2 = catmullTime(t1, p1x, p1y, p2x, p2y);
            double t3 = catmullTime(t2, p2x, p2y, p3x, p3y);
            double step = (t2 - t1) / iterations, minX = p1x, maxX = p2x;
            for (double t = t1; t <= t2; t += step) {
                double a1x = (t1 - t) / t1 * p0x + t / t1 * p1x, a1y = (t1 - t) / t1 * p0y + t / t1 * p1y;
                double a2x = (t2 - t) / (t2 - t1) * p1x + (t - t1) / (t2 - t1) * p2x;
                double a2y = (t2 - t) / (t2 - t1) * p1y + (t - t1) / (t2 - t1) * p2y;
                double a3x = (t3 - t) / (t3 - t2) * p2x + (t - t2) / (t3 - t2) * p3x;
                double a3y = (t3 - t) / (t3 - t2) * p2y + (t - t2) / (t3 - t2) * p3y;
                double b1x = (t2 - t) / t2 * a1x + t / t2 * a2x, b1y = (t2 - t) / t2 * a1y + t / t2 * a2y;
                double b2x = (t3 - t) / (t3 - t1) * a2x + (t - t1) / (t3 - t1) * a3x;
                double b2y = (t3 - t) / (t3 - t1) * a2y + (t - t1) / (t3 - t1) * a3y;
                double cx = (t2 - t) / (t2 - t1) * b1x + (t - t1) / (t2 - t1) * b2x;
                double cy = (t2 - t) / (t2 - t1) * b1y + (t - t1) / (t2 - t1) * b2y;
                cx = std::min(std::max(cx, minX), maxX);
                minX = cx;
                out.push_back({cx, cy * scale + offset});
                if (step <= 0) break;
            }
        }
        out.push_back({pts[n - 1].x, pts[n - 1].v});
    } else if (type == 2 && n >= 2) {  // bezier segments through each point's handles
        int it = iterations * 2;
        for (size_t i = 0; i + 1 < n; i++) {
            const Pt &a = pts[i], &b = pts[i + 1];
            double p0x = a.x, p0y = a.v, p1x = a.x + a.h[2], p1y = a.v + a.h[3];
            double p2x = b.x + b.h[0], p2y = b.v + b.h[1], p3x = b.x, p3y = b.v;
            for (int j = 0; j < it; j++) {
                double t = static_cast<double>(j) / it, mt = 1 - t;
                out.push_back({p0x * mt * mt * mt + 3 * p1x * mt * mt * t + 3 * p2x * mt * t * t + p3x * t * t * t,
                               p0y * mt * mt * mt + 3 * p1y * mt * mt * t + 3 * p2y * mt * t * t + p3y * t * t * t});
            }
        }
        out.push_back({pts[n - 1].x, pts[n - 1].v});
    } else {
        for (auto& p : pts) out.push_back({p.x, p.v});
    }
    return out;
}

double evaluate(const std::vector<Sample>& s, double x) {
    if (s.empty()) return 0;
    if (s.size() == 1) return s[0].v;
    x = std::min(1.0, std::max(0.0, x));
    size_t start = 0, end = s.size() - 1, mid = end >> 1;
    while (mid != start) {
        if (s[mid].x > x) end = mid;
        else start = mid;
        mid = (start + end) >> 1;
    }
    double x1 = s[mid].x, x2 = s[mid + 1].x;
    if (x1 == x2) return s[mid].v;
    return s[mid].v + (s[mid + 1].v - s[mid].v) * (x - x1) / (x2 - x1);
}

}  // namespace

Value animCurveFromData(Runner& rt, const AnimCurve& c) { return fromData(rt, c); }
Value animCurveArg(Runner& rt, const Value& v) { return curveArg(rt, v); }
std::vector<std::pair<double, double>> animCurveSamples(Runner& rt, const Value& channel) {
    std::vector<std::pair<double, double>> out;
    if (!channel.obj()) return out;
    for (const Sample& s : sampleChannel(rt, channel)) out.push_back({s.x, s.v});
    return out;
}

GMFN(animcurve_get) { return curveArg(rt, ARG(0)); }
GMFN(animcurve_exists) { return boolV(!curveArg(rt, ARG(0)).isUndefined()); }
GMFN(animcurve_get_channel_index) {
    Value c = curveArg(rt, ARG(0));
    auto chans = items(field(rt, c, "channels"));
    for (size_t i = 0; i < chans.size(); i++)
        if (field(rt, chans[i], "name").toString() == S(1)) return Value(static_cast<double>(i));
    return Value(-1.0);
}
GMFN(animcurve_get_channel) {  // (curve, index or name)
    Value c = curveArg(rt, ARG(0));
    auto chans = items(field(rt, c, "channels"));
    if (ARG(1).isString()) {
        for (auto& ch : chans)
            if (field(rt, ch, "name").toString() == S(1)) return ch;
        return Value();
    }
    int i = I(1);
    return i >= 0 && static_cast<size_t>(i) < chans.size() ? chans[i] : Value();
}
GMFN(animcurve_channel_evaluate) {  // (channel, x 0..1)
    if (!ARG(0).obj()) return Value(0.0);
    return Value(evaluate(sampleChannel(rt, ARG(0)), D(1)));
}
GMFN(animcurve_create) {
    ensureLoaded(rt);
    Value c = makeCurve(rt, "", 0, {});
    g_curves.push_back(c);
    g_curveLive.push_back(true);
    return c;
}
GMFN(animcurve_destroy) {
    for (size_t i = 0; i < g_curves.size(); i++)
        if (g_curves[i].obj() && (g_curves[i].obj() == ARG(0).obj() || (!ARG(0).obj() && static_cast<int>(i) == I(0))))
            g_curveLive[i] = false;
    return Value();
}
GMFN(animcurve_channel_new) { return makeChannel(rt, "", 0, 16, {}); }
GMFN(animcurve_point_new) { return makePoint(rt, 0, 0); }

void registerAnimCurveBuiltins(Runner& rt) {
    GMREG(animcurve_get); GMREG(animcurve_exists); GMREG(animcurve_get_channel_index); GMREG(animcurve_get_channel);
    GMREG(animcurve_channel_evaluate); GMREG(animcurve_create); GMREG(animcurve_destroy);
    GMREG(animcurve_channel_new); GMREG(animcurve_point_new);
}

}  // namespace gm
