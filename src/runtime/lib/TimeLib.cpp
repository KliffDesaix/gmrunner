// Time sources (time_source_*, call_later, call_cancel), following YoYo's runner (yyTime.js):
// every source is ticked first, then expiries are checked and callbacks run, so sources created
// inside a callback don't fire in the step they were made.
#include <algorithm>
#include <cmath>

#include "runtime/Builtins.h"
#include "runtime/Stores.h"

namespace gm {

enum : int32_t { TS_GLOBAL = 0, TS_GAME = 1, TS_CALL_LATER = 2 };

TimeSourceStore::TimeSourceStore(Runner& r) : rt(r) {
    auto root = [](int32_t id, TimeSource::Type type) {
        TimeSource t;
        t.id = id;
        t.type = type;
        return t;
    };
    all[TS_GLOBAL] = root(TS_GLOBAL, TimeSource::Basic);
    all[TS_GAME] = root(TS_GAME, TimeSource::Stateful);
    all[TS_CALL_LATER] = root(TS_CALL_LATER, TimeSource::Basic);
}

void TimeSourceStore::update(int64_t frameMicros) {
    tick(all[TS_GLOBAL], frameMicros);
    tick(all[TS_CALL_LATER], frameMicros);
    tick(all[TS_GAME], frameMicros);
    for (int32_t root : {TS_GLOBAL, TS_CALL_LATER, TS_GAME}) checkChildren(all[root]);
}

void TimeSourceStore::tick(TimeSource& ts, int64_t dt) {
    ts.dt = dt;
    if (ts.configurable() && ts.state == TimeSource::Active) ts.t += ts.units == 0 ? dt : 1;
    tickChildren(ts);
}

void TimeSourceStore::tickChildren(TimeSource& ts) {
    int64_t dt = ts.dt;
    for (int32_t c : std::vector<int32_t>(ts.children))
        if (TimeSource* child = get(c)) tick(*child, dt);
}

void TimeSourceStore::check(TimeSource& ts) {
    int32_t id = ts.id;
    if (ts.configurable() && ts.state == TimeSource::Active) {
        bool expire = ts.t >= ts.period;
        if (!expire && ts.expiryType == 0 && ts.units == 0) {
            // "nearest": expire now if this tick is closer to the period than the next would be
            int64_t remaining = ts.period - ts.t;
            expire = remaining <= ts.dt && remaining < ts.t + ts.dt - ts.period;
        }
        if (expire) {
            ts.repsCompleted++;
            if (ts.repsRemaining > 0) ts.repsRemaining--;
            if (ts.finished()) {
                ts.t = ts.period;
                if (ts.type == TimeSource::SelfDestructing) ts.marked = true;
                else ts.state = TimeSource::Stopped;
            } else if (ts.t < ts.period) {
                ts.t -= ts.period;
            } else {
                ts.t %= ts.period;
            }
            Value fn = ts.callback;
            std::vector<Value> args = ts.args;
            ts.locked = true;
            rt.vm.callValue(fn, &rt.globalSelf, &rt.globalSelf, static_cast<int>(args.size()), args.data());
            TimeSource* again = get(id);  // the callback may have created sources (rehashing nothing: std::map)
            if (!again) return;
            again->locked = false;
        }
    }
    if (TimeSource* again = get(id)) checkChildren(*again);
}

void TimeSourceStore::checkChildren(TimeSource& ts) {
    int32_t id = ts.id;
    for (int32_t c : std::vector<int32_t>(ts.children)) {
        TimeSource* parent = get(id);
        if (!parent || std::find(parent->children.begin(), parent->children.end(), c) == parent->children.end()) continue;
        TimeSource* child = get(c);
        if (!child) continue;
        if (!child->marked) check(*child);
        if ((child = get(c)) && child->marked) destroy(c);
    }
}

void TimeSourceStore::configure(TimeSource& ts, double period, int units, const Value& callback,
                                std::vector<Value> args, int64_t reps, int expiryType) {
    ts.units = units == 1 ? 1 : 0;
    ts.period = ts.units == 0 ? static_cast<int64_t>(std::floor(std::max(period, 1e-6) * 1000000.0))
                              : static_cast<int64_t>(std::floor(std::max(period, 1.0)));
    ts.callback = callback;
    ts.args = std::move(args);
    ts.repsRequested = reps < -1 ? -1 : reps;
    ts.expiryType = expiryType == 0 ? 0 : 1;
    reset(ts);
}

int32_t TimeSourceStore::add(int32_t parent, TimeSource::Type type, double period, int units, const Value& callback,
                             std::vector<Value> args, int64_t reps, int expiryType) {
    TimeSource* p = get(parent);
    if (!p) return -1;
    int32_t id = nextId++;
    TimeSource& ts = all[id];
    ts.id = id;
    ts.parent = parent;
    ts.type = type;
    configure(ts, period, units, callback, std::move(args), reps, expiryType);
    if (type == TimeSource::SelfDestructing) ts.state = TimeSource::Active;
    get(parent)->children.push_back(id);
    return id;
}

void TimeSourceStore::destroy(int32_t id) {
    TimeSource* ts = get(id);
    if (!ts) return;
    for (int32_t c : std::vector<int32_t>(ts->children)) destroy(c);
    if (TimeSource* p = get(ts->parent)) p->children.erase(std::remove(p->children.begin(), p->children.end(), id), p->children.end());
    all.erase(id);
}

void TimeSourceStore::propagate(TimeSource& ts, TimeSource::State state) {
    if (ts.type != TimeSource::Basic) ts.state = state;
    for (int32_t c : ts.children)
        if (TimeSource* child = get(c)) propagate(*child, state);
}

void TimeSourceStore::start(TimeSource& ts) {
    if (ts.configurable() && ts.finished()) {
        ts.state = TimeSource::Initial;
        ts.t = 0;
        ts.repsRemaining = ts.repsRequested;
    }
    propagate(ts, TimeSource::Active);
}

void TimeSourceStore::stop(TimeSource& ts) {
    if (ts.configurable() && ts.state != TimeSource::Initial && !ts.finished()) {
        ts.state = TimeSource::Stopped;
        ts.t = 0;
    }
    for (int32_t c : ts.children)
        if (TimeSource* child = get(c)) stop(*child);
}

void TimeSourceStore::pause(TimeSource& ts, bool pauseIt) {
    if (pauseIt && ts.state == TimeSource::Active) ts.state = TimeSource::Paused;
    if (!pauseIt && ts.state == TimeSource::Paused) ts.state = TimeSource::Active;
    for (int32_t c : ts.children)
        if (TimeSource* child = get(c)) pause(*child, pauseIt);
}

void TimeSourceStore::reset(TimeSource& ts) {
    propagate(ts, TimeSource::Initial);
    ts.t = 0;
    ts.repsRemaining = ts.repsRequested;
    ts.repsCompleted = 0;
}

bool TimeSourceStore::hasLockedSource(const TimeSource& ts) {
    if (ts.locked) return true;
    for (int32_t c : ts.children)
        if (TimeSource* child = get(c); child && hasLockedSource(*child)) return true;
    return false;
}

void TimeSourceStore::markTree(TimeSource& ts) {
    ts.marked = true;
    for (int32_t c : ts.children)
        if (TimeSource* child = get(c)) markTree(*child);
}

namespace {

TimeSourceStore& store(Runner& rt) { return *rt.timeSources; }

std::vector<Value> argList(const Value& v) {
    std::vector<Value> out;
    if (GMArray* a = v.arr())
        if (!a->rows.empty()) out = a->rows[0];
    return out;
}

// A user source, or a built-in one when allowBuiltIn; logs like YoYo's runner otherwise.
TimeSource* userSource(Runner& rt, const Value& id, const char* fn, bool allowBuiltIn = false) {
    TimeSource* ts = store(rt).get(id.toInt());
    if (!ts || ts->id == TS_CALL_LATER) {
        rt.log("%s: time source %d does not exist", fn, id.toInt());
        return nullptr;
    }
    if (!allowBuiltIn && !ts->configurable()) {
        rt.log("%s: not allowed on built-in time source %d", fn, ts->id);
        return nullptr;
    }
    return ts;
}

}  // namespace

GMFN(time_source_create) {  // (parent, period, units, callback, [args], [reps], [expiry_type])
    TimeSource* parent = userSource(rt, ARG(0), "time_source_create", true);
    if (!parent) return Value(-1.0);
    return Value(static_cast<double>(store(rt).add(parent->id, TimeSource::Configurable, D(1), I(2), ARG(3),
                                                   argc > 4 ? argList(ARG(4)) : std::vector<Value>{},
                                                   argc > 5 ? static_cast<int64_t>(D(5)) : 1, argc > 6 ? I(6) : 1)));
}
GMFN(time_source_destroy) {  // (id, [destroy_tree])
    TimeSource* ts = userSource(rt, ARG(0), "time_source_destroy");
    if (!ts) return Value();
    if (!ts->children.empty() && !B(1)) {
        rt.log("time_source_destroy: time source %d still has children", ts->id);
        return Value();
    }
    if (store(rt).hasLockedSource(*ts)) store(rt).markTree(*ts);  // finishes after its callback
    else store(rt).destroy(ts->id);
    return Value();
}
GMFN(time_source_start) {
    if (TimeSource* ts = userSource(rt, ARG(0), "time_source_start", true)) {
        if (ts->type == TimeSource::Basic) rt.log("time_source_start: time source %d has no state", ts->id);
        else store(rt).start(*ts);
    }
    return Value();
}
GMFN(time_source_stop) {
    if (TimeSource* ts = userSource(rt, ARG(0), "time_source_stop")) store(rt).stop(*ts);
    return Value();
}
GMFN(time_source_pause) {
    if (TimeSource* ts = userSource(rt, ARG(0), "time_source_pause", true)) store(rt).pause(*ts, true);
    return Value();
}
GMFN(time_source_resume) {
    if (TimeSource* ts = userSource(rt, ARG(0), "time_source_resume", true)) store(rt).pause(*ts, false);
    return Value();
}
GMFN(time_source_reset) {
    if (TimeSource* ts = userSource(rt, ARG(0), "time_source_reset")) store(rt).reset(*ts);
    return Value();
}
GMFN(time_source_reconfigure) {  // (id, period, units, callback, [args], [reps], [expiry_type])
    TimeSource* ts = userSource(rt, ARG(0), "time_source_reconfigure");
    if (ts && ts->type == TimeSource::Configurable)
        store(rt).configure(*ts, D(1), I(2), ARG(3), argc > 4 ? argList(ARG(4)) : std::vector<Value>{},
                            argc > 5 ? static_cast<int64_t>(D(5)) : 1, argc > 6 ? I(6) : 1);
    return Value();
}
GMFN(time_source_get_period) {
    TimeSource* ts = userSource(rt, ARG(0), "time_source_get_period");
    if (!ts) return Value();
    return Value(ts->units == 0 ? ts->period / 1000000.0 : static_cast<double>(ts->period));
}
GMFN(time_source_get_reps_completed) {
    TimeSource* ts = userSource(rt, ARG(0), "time_source_get_reps_completed");
    return ts ? Value(static_cast<double>(ts->repsCompleted)) : Value();
}
GMFN(time_source_get_reps_remaining) {
    TimeSource* ts = userSource(rt, ARG(0), "time_source_get_reps_remaining");
    return ts ? Value(static_cast<double>(ts->repsRemaining)) : Value();
}
GMFN(time_source_get_units) {
    TimeSource* ts = userSource(rt, ARG(0), "time_source_get_units");
    return ts ? Value(static_cast<double>(ts->units)) : Value();
}
GMFN(time_source_get_time_remaining) {
    TimeSource* ts = userSource(rt, ARG(0), "time_source_get_time_remaining");
    if (!ts) return Value();
    int64_t left = std::max<int64_t>(ts->period - ts->t, 0);
    return Value(ts->units == 0 ? left / 1000000.0 : static_cast<double>(left));
}
GMFN(time_source_get_state) {
    TimeSource* ts = userSource(rt, ARG(0), "time_source_get_state", true);
    if (!ts) return Value();
    return Value(static_cast<double>(ts->type == TimeSource::Basic ? TimeSource::Active : ts->state));
}
GMFN(time_source_get_parent) {
    TimeSource* ts = userSource(rt, ARG(0), "time_source_get_parent", true);
    return ts && ts->parent >= 0 ? Value(static_cast<double>(ts->parent)) : Value();
}
GMFN(time_source_get_children) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    if (TimeSource* ts = userSource(rt, ARG(0), "time_source_get_children", true))
        for (int32_t c : ts->children) a->rows[0].push_back(Value(static_cast<double>(c)));
    return arr;
}
GMFN(time_source_exists) {
    TimeSource* ts = store(rt).get(I(0));
    return boolV(ts && ts->id != TS_CALL_LATER);
}
GMFN(time_seconds_to_bpm) { return Value(60.0 / D(0)); }
GMFN(time_bpm_to_seconds) { return Value(60.0 / D(0)); }
GMFN(call_later) {  // (period, units, callback, [repeat]) -> handle
    return Value(static_cast<double>(
        store(rt).add(TS_CALL_LATER, TimeSource::SelfDestructing, D(0), I(1), ARG(2), {}, B(3) ? -1 : 1, 1)));
}
GMFN(call_cancel) {
    TimeSource* ts = store(rt).get(I(0));
    if (!ts || ts->type != TimeSource::SelfDestructing) return Value();
    if (ts->locked) ts->marked = true;
    else store(rt).destroy(ts->id);
    return Value();
}

void registerTimeBuiltins(Runner& rt) {
    GMREG(time_source_create); GMREG(time_source_destroy); GMREG(time_source_start); GMREG(time_source_stop);
    GMREG(time_source_pause); GMREG(time_source_resume); GMREG(time_source_reset); GMREG(time_source_reconfigure);
    GMREG(time_source_get_period); GMREG(time_source_get_reps_completed); GMREG(time_source_get_reps_remaining);
    GMREG(time_source_get_units); GMREG(time_source_get_time_remaining); GMREG(time_source_get_state);
    GMREG(time_source_get_parent); GMREG(time_source_get_children); GMREG(time_source_exists);
    GMREG(time_seconds_to_bpm); GMREG(time_bpm_to_seconds); GMREG(call_later); GMREG(call_cancel);
}

}  // namespace gm
