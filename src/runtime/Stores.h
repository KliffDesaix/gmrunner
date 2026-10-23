// Runtime-owned resource pools: ds_* structures, files/ini, particle systems, time sources.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "vm/Value.h"

namespace gm {

class Runner;

// ---- ds_*

struct ValueKey {
    bool isString;
    double num;
    std::string str;
    bool operator==(const ValueKey& o) const {
        return isString == o.isString && (isString ? str == o.str : num == o.num);
    }
    static ValueKey of(const Value& v) {
        if (v.isString()) return {true, 0, v.str()};
        return {false, v.num(), {}};
    }
    Value toValue() const { return isString ? Value(str) : Value(num); }
};
struct ValueKeyHash {
    size_t operator()(const ValueKey& k) const {
        return k.isString ? std::hash<std::string>()(k.str) : std::hash<double>()(k.num);
    }
};

// Values in a map or list can be marked as nested ds_lists/ds_maps (ds_map_add_map,
// ds_list_mark_as_list, json_decode): json_encode writes them out and destroying the parent
// destroys them too.
enum DsMark : uint8_t { DS_MARK_NONE = 0, DS_MARK_LIST = 1, DS_MARK_MAP = 2 };

struct DsMap {
    std::unordered_map<ValueKey, Value, ValueKeyHash> values;
    std::vector<ValueKey> order;  // insertion order for ds_map_find_first/next
    std::unordered_map<ValueKey, uint8_t, ValueKeyHash> marks;
};

struct DsGrid {
    int w = 0, h = 0;
    std::vector<Value> cells;
};

struct DsStore {
    template <typename T> struct Pool {
        std::vector<std::unique_ptr<T>> items;
        int create() {
            items.push_back(std::make_unique<T>());
            return static_cast<int>(items.size() - 1);
        }
        T* get(int id) { return id >= 0 && static_cast<size_t>(id) < items.size() ? items[id].get() : nullptr; }
        void destroy(int id) {
            if (get(id)) items[id].reset();
        }
    };
    Pool<std::vector<Value>> lists;
    std::unordered_map<int, std::unordered_map<size_t, uint8_t>> listMarks;  // list id -> index -> mark
    Pool<DsMap> maps;
    Pool<std::deque<Value>> queues;
    Pool<std::vector<Value>> stacks;
    Pool<DsGrid> grids;
    Pool<std::vector<std::pair<Value, double>>> priorities;
};

// ---- files

struct IniFile {
    std::string path;
    std::map<std::string, std::map<std::string, std::string>> sections;
    std::vector<std::string> sectionOrder;
    bool dirty = false;
};

struct TextFile {
    bool write = false;
    std::string path;
    std::vector<std::string> lines;  // read mode
    size_t line = 0, col = 0;
    std::string out;                 // write mode
};

struct FileStore {
    explicit FileStore(Runner& rt) : rt(rt) {}
    Runner& rt;
    std::unique_ptr<IniFile> ini;
    std::vector<std::unique_ptr<TextFile>> texts;

    // Resolves a GML path for reading: the save area first, then the game's own files.
    std::string readPath(const std::string& gmlPath) const;
    // Resolves a GML path for writing (always inside the save area).
    std::string writePath(const std::string& gmlPath) const;
    std::string relative(const std::string& gmlPath) const;
};

// ---- particles

// Particles follow YoYo's runner (yyParticle.js): types hold ranges, each particle draws its
// values once and then changes by the increments plus a deterministic wiggle.
struct ParticleType {
    bool live = true;
    int sprite = -1, spriteStart = 0;
    bool spriteAnim = true, spriteStretch = false, spriteRandom = false;
    int shape = 0;  // pt_shape_*
    double sizeMinX = 1, sizeMaxX = 1, sizeMinY = 1, sizeMaxY = 1;
    double sizeIncrX = 0, sizeIncrY = 0, sizeRandX = 0, sizeRandY = 0;
    double xscale = 1, yscale = 1;
    int lifeMin = 100, lifeMax = 100;
    int stepType = 0, stepNumber = 0, deathType = 0, deathNumber = 0;
    double spMin = 0, spMax = 0, spIncr = 0, spRand = 0;
    double dirMin = 0, dirMax = 0, dirIncr = 0, dirRand = 0;
    double angMin = 0, angMax = 0, angIncr = 0, angRand = 0;
    bool angDir = false;
    double grav = 0, gravDir = 270;
    int colMode = 0;  // one, two, three, rgb, hsv, mix
    double colPar[6] = {0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0, 0, 0};
    double alphaStart = 1, alphaMiddle = 1, alphaEnd = 1;
    bool additive = false;
};

struct Particle {
    int type = 0;
    double age = 0, lifetime = 0;
    double x = 0, y = 0, speed = 0, dir = 0, ang = 0;
    int colMode = 0;
    double colPar[6] = {};
    uint32_t color = 0xFFFFFF;
    double alpha = 1, xsize = 1, ysize = 1;
    double spriteStart = 0, subimg = 0;
    double ran = 0;  // per-particle phase for the wiggles
};

struct ParticleEmitter {
    bool created = true, zombie = false, enabled = true;
    bool burstMode = false;
    double number = 0;  // per step when streaming; negative = one particle with chance 1/-number
    bool relative = false;
    double delayMin = 0, delayMax = 0, delayCurrent = 0, intervalMin = 0, intervalMax = 0, intervalCurrent = 0;
    int delayUnit = 1, intervalUnit = 1;  // 0 seconds, 1 frames
    int partType = 0;
    double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    int shape = 0, distr = 0;  // ps_shape_*, ps_distr_*
    std::vector<Particle> particles;  // GameMaker 2023.2+: particles belong to their emitter
};

struct ParticleSystem {
    bool live = true;
    double depth = 0, x = 0, y = 0;
    // Systems from assets: drawn relative to their origin, turned and scaled (room editor elements).
    double originX = 0, originY = 0, angle = 0, xscale = 1, yscale = 1;
    int32_t layer = -1;       // drawn only while this layer is visible
    bool persistent = true;   // false: destroyed when the room ends (room editor elements)
    bool autoUpdate = true, autoDraw = true, oldToNew = true;
    uint32_t color = 0xFFFFFF;
    double alpha = 1;
    std::vector<ParticleEmitter> emitters;
    std::vector<Particle> particles;  // before 2023.2 all of a system's particles share one list
    bool empty() const {
        if (!particles.empty()) return false;
        for (auto& e : emitters)
            if (!e.particles.empty()) return false;
        return true;
    }
};

struct ParticleStore {
    explicit ParticleStore(Runner& rt);
    Runner& rt;
    std::vector<ParticleType> types;
    std::vector<ParticleSystem> systems;
    bool emitterLists = false;  // GameMaker 2023.2+ (PSEM chunk)
    std::vector<int> assetTypes;  // the particle type made for each PSEM emitter

    ParticleType* type(int id) {
        return id >= 0 && static_cast<size_t>(id) < types.size() && types[id].live ? &types[id] : nullptr;
    }
    ParticleSystem* system(int id) {
        return id >= 0 && static_cast<size_t>(id) < systems.size() && systems[id].live ? &systems[id] : nullptr;
    }
    ParticleEmitter* emitter(int sys, int em) {
        ParticleSystem* s = system(sys);
        if (!s || em < 0 || static_cast<size_t>(em) >= s->emitters.size() || !s->emitters[em].created) return nullptr;
        return &s->emitters[em];
    }
    int createEmitter(int sys);
    // part_particles_create(_color): into the first enabled emitter (2023.2+) or the system's list.
    void create(int sys, double x, double y, int type, int number, bool colored = false, uint32_t color = 0);
    void burst(int sys, int em, int type, double number);
    // A new system from a particle system asset (PSYS); -1 if there is no such asset.
    int instantiate(int asset);
    void destroy(int sys) {
        if (ParticleSystem* s = system(sys)) *s = ParticleSystem(), s->live = false;
    }
    void updateSystem(int sys);
    void update();
    // `world` (row-vector 4x4) places the system's space, as a sequence track does; color and
    // alpha multiply the system's own.
    void draw(int sys, const float* world = nullptr, uint32_t color = 0xFFFFFF, double alpha = 1);
    void clearAll() {
        for (auto& s : systems) {
            s.particles.clear();
            for (auto& e : s.emitters) e.particles.clear();
        }
    }
    // Room end: room editor systems go, the rest lose their particles.
    void roomEnd() {
        for (auto& s : systems)
            if (s.live && !s.persistent) s = ParticleSystem(), s.live = false;
        clearAll();
    }

private:
    std::vector<Particle>& listFor(ParticleSystem& s, int em);
    void emit(int sys, int em, double x, double y, int type, int number, bool colored = false, uint32_t color = 0);
    void burstRegion(int sys, int em, double x, double y, double w, double h, int shape, int distr, int type,
                     double number);
    void handle(int sys, int em);

public:
    double random(double range);  // [0, range), the particle system's own generator

private:
    double myRandom(double minv, double maxv, int distr);
    void computeColor(Particle& p);
};

// ---- time sources (GameMaker 2022+: time_source_*, call_later), a tree as in YoYo's runner.
// Ids 0 (time_source_global) and 1 (time_source_game) are built in; 2 is the hidden parent of
// call_later sources.

struct TimeSource {
    enum Type { Basic, Stateful, Configurable, SelfDestructing };
    enum State { Initial, Active, Paused, Stopped };
    int32_t id = 0, parent = -1;
    Type type = Basic;
    State state = Active;
    int units = 0;           // 0 seconds (period in microseconds), 1 frames
    int64_t period = 1, t = 0, dt = 0;
    int64_t repsRequested = 1, repsCompleted = 0, repsRemaining = 1;  // -1 repeats forever
    int expiryType = 1;      // 0 nearest, 1 after
    Value callback;
    std::vector<Value> args;
    bool locked = false, marked = false;
    std::vector<int32_t> children;
    bool configurable() const { return type == Configurable || type == SelfDestructing; }
    bool stateful() const { return type == Stateful || type == Configurable; }
    bool finished() const { return repsRemaining == 0; }
};

struct TimeSourceStore {
    explicit TimeSourceStore(Runner& rt);
    Runner& rt;
    std::map<int32_t, TimeSource> all;
    int32_t nextId = 3;

    TimeSource* get(int32_t id) {
        auto it = all.find(id);
        return it == all.end() ? nullptr : &it->second;
    }
    // Once per step, after timelines (YoYo's HandleTimeSources); frameMicros is the step's delta_time.
    void update(int64_t frameMicros);
    int32_t add(int32_t parent, TimeSource::Type type, double period, int units, const Value& callback,
                std::vector<Value> args, int64_t reps, int expiryType);
    void configure(TimeSource& ts, double period, int units, const Value& callback, std::vector<Value> args,
                   int64_t reps, int expiryType);
    void destroy(int32_t id);  // the source and its subtree
    void start(TimeSource& ts);
    void stop(TimeSource& ts);
    void pause(TimeSource& ts, bool pause);
    void reset(TimeSource& ts);
    void propagate(TimeSource& ts, TimeSource::State state);
    bool hasLockedSource(const TimeSource& ts);
    void markTree(TimeSource& ts);

private:
    void tick(TimeSource& ts, int64_t dt);
    void tickChildren(TimeSource& ts);
    void check(TimeSource& ts);
    void checkChildren(TimeSource& ts);
};

}  // namespace gm
