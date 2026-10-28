#include "Runner.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <map>
#include <unordered_map>

#include "Audio.h"
#include "Builtins.h"
#include "Graphics.h"
#include "Sequences.h"
#include "Stores.h"

namespace gm {

static uint64_t eventKey(uint32_t type, uint32_t subtype) { return (static_cast<uint64_t>(type) << 32) | subtype; }

// GameMaker's motion maths (YoYo's runner: Compute_Speed1/2, AddTo_Speed): sin/cos are cut to
// 6 decimals, degrees use 0.0174532925, and results within 0.0001 of a whole number snap to it.
// Matching this keeps long movements landing exactly where they do in GameMaker.
static double clampFloat(double f) { return static_cast<double>(static_cast<int32_t>(f * 1000000.0)) / 1000000.0; }
static double snapWhole(double v) { return std::fabs(v - std::round(v)) < 0.0001 ? std::round(v) : v; }
static constexpr double kDegToRad = 0.0174532925;

void Instance::setHVSpeed(double h, double v) {
    hspeed = h;
    vspeed = v;
    if (h == 0) {
        if (v > 0) direction = 270;
        else if (v < 0) direction = 90;
    } else {
        double dd = clampFloat(180.0 * std::atan2(v, h) / M_PI);
        direction = dd <= 0 ? -dd : 360.0 - dd;
    }
    direction = std::fmod(snapWhole(direction), 360.0);
    speed = snapWhole(std::sqrt(h * h + v * v));
}

void Instance::setSpeedDir(double s, double d) {
    speed = s;
    direction = std::fmod(d, 360.0);
    if (direction < 0) direction += 360;
    hspeed = snapWhole(s * clampFloat(std::cos(direction * kDegToRad)));
    vspeed = snapWhole(-s * clampFloat(std::sin(direction * kDegToRad)));
}

void Instance::addSpeed(double dir, double amount) {
    setHVSpeed(hspeed + amount * clampFloat(std::cos(dir * kDegToRad)),
               vspeed - amount * clampFloat(std::sin(dir * kDegToRad)));
}

static int32_t traceId() {
    static int32_t id = [] {
        const char* e = getenv("GMR_TRACE_ID");
        return e ? atoi(e) : -1;
    }();
    return id;
}

Runner::Runner(DataWin& d, Platform& p, std::string gd, std::string sd)
    : dw(d), platform(p), vm(*this), gameDir(std::move(gd)), saveDir(std::move(sd)), rng(static_cast<int32_t>(std::random_device{}())) {
    displayName = std::string(dw.gen.displayName);
    // Writing to an array another variable shares: GameMaker before 2.3 copies it; 2.3+ copies only
    // with "enable copy on write" set, and then only arrays made in another function (setowner).
    ArrayOwnership::mode = !dw.gen.gms23                               ? ArrayCopy::WhenShared
                           : (dw.options.flags & OPT_COPY_ON_WRITE) != 0 ? ArrayCopy::OtherOwner
                                                                       : ArrayCopy::Never;
    ArrayOwnership::current = 0;
    byObject.resize(dw.objects.size());
    family.resize(dw.objects.size());
    for (size_t o = 0; o < dw.objects.size(); o++) {
        int32_t cur = static_cast<int32_t>(o);
        int guard = 0;
        while (cur >= 0 && static_cast<size_t>(cur) < dw.objects.size() && guard++ < 64) {
            family[cur].push_back(static_cast<int32_t>(o));
            cur = dw.objects[cur].parent;
        }
    }
    globalSelf.id = -1;
    globalSelf.object = -1;
    buildEventTables();
    buildCollisionPairs();
}

Runner::~Runner() {
    for (Instance* i : instances) delete i;
    for (auto& [r, s] : savedRooms)
        for (Instance* i : s.instances) delete i;
}

void Runner::log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void Runner::report(DiagKind kind, const std::string& msg) {
    std::string roomName = room >= 0 && static_cast<size_t>(room) < dw.rooms.size() ? std::string(dw.rooms[room].name) : "?";
    if (kind == DiagKind::Error || kind == DiagKind::Compile)
        fprintf(stderr, "%s %s\n", kind == DiagKind::Compile ? "[vm]" : "[gml]", msg.c_str());
    else
        fprintf(stderr, "[gml] %s (step %llu, room %s)\n", msg.c_str(), static_cast<unsigned long long>(stepCount),
                roomName.c_str());
    if (!collectDiagnostics) return;
    DiagEntry& e = diagnostics[msg];
    if (e.count++ == 0) {
        e.kind = kind;
        e.phase = diagPhase;
    }
    if (std::find(e.rooms.begin(), e.rooms.end(), roomName) == e.rooms.end()) e.rooms.push_back(roomName);
}

double Runner::currentTimeMs() const {
    if (fixedClock) return clockMs_ += 0.001;
    return (platform.now() - startTime_) * 1000.0;
}

// ---------------------------------------------------------------------------------------------
// Event tables

void Runner::buildEventTables() {
    events.assign(dw.objects.size(), ObjectEvents());
    std::vector<int> state(dw.objects.size(), 0);  // 0 = todo, 1 = in progress, 2 = done
    std::function<void(int32_t)> resolve = [&](int32_t o) {
        if (state[o] == 2) return;
        if (state[o] == 1) {
            log("warning: parent cycle at object %d", o);
            return;
        }
        state[o] = 1;
        ObjectEvents& ev = events[o];
        int32_t parent = dw.objects[o].parent;
        if (parent >= 0 && static_cast<size_t>(parent) < dw.objects.size()) {
            resolve(parent);
            ev.handlers = events[parent].handlers;
        }
        // An event the object defines overrides its parent's even when it has no code (an empty
        // child event, or an empty Draw that turns off the default drawing).
        for (uint32_t t = 0; t < EV_COUNT; t++)
            for (auto& h : dw.objects[o].events[t])
                ev.handlers[eventKey(t, h.subtype)] = {!h.code.empty() && h.code[0] >= 0 ? h.code[0] : -1, o};
        state[o] = 2;
    };
    for (size_t o = 0; o < dw.objects.size(); o++) resolve(static_cast<int32_t>(o));

    for (auto& ev : events) {
        ev.hasDraw = ev.handlers.count(eventKey(EV_DRAW, DRAW_NORMAL)) != 0;
        for (auto& [key, h] : ev.handlers) ev.hasMouse |= (key >> 32) == EV_MOUSE;
    }
}

const Handler* Runner::findHandler(int32_t object, uint32_t type, uint32_t subtype) const {
    if (object < 0 || static_cast<size_t>(object) >= events.size()) return nullptr;
    auto& h = events[object].handlers;
    auto it = h.find(eventKey(type, subtype));
    return it == h.end() ? nullptr : &it->second;
}

// A collision event is matched up the other object's parent chain as well (YoYo's
// PerformEvent): colliding with a child of the object the event names runs that event, whether
// the collision was found by the runner or sent with event_perform.
const Handler* Runner::findEvent(int32_t object, uint32_t type, uint32_t& subtype) const {
    if (type != EV_COLLISION) return findHandler(object, type, subtype);
    int depth = 0;
    for (int64_t c = subtype; c >= 0 && static_cast<size_t>(c) < dw.objects.size() && depth < 64; depth++) {
        if (const Handler* h = findHandler(object, type, static_cast<uint32_t>(c))) {
            subtype = static_cast<uint32_t>(c);
            return h;
        }
        c = dw.objects[c].parent;
    }
    return nullptr;
}

bool Runner::runHandler(Instance* inst, const Handler& h, uint32_t type, uint32_t subtype, Instance* other) {
    EventCtx saved = ctx;
    ctx = {h.owner, static_cast<int32_t>(type), static_cast<int32_t>(subtype)};
    if (h.code >= 0) vm.run(h.code, inst, other ? other : inst);
    ctx = saved;
    return true;
}

bool Runner::performEvent(Instance* inst, uint32_t type, uint32_t subtype, Instance* other) {
    const Handler* h = findEvent(inst->object, type, subtype);
    if (!h) return false;
    Handler copy = *h;
    return runHandler(inst, copy, type, subtype, other);
}

bool Runner::performEventAs(Instance* inst, int32_t object, uint32_t type, uint32_t subtype, Instance* other) {
    const Handler* h = findEvent(object, type, subtype);
    if (!h) return false;
    Handler copy = *h;
    return runHandler(inst, copy, type, subtype, other ? other : inst);
}

bool Runner::eventInherited(Instance* self, Instance* other) {
    if (ctx.object < 0 || ctx.type < 0) return false;
    int32_t parent = dw.objects[ctx.object].parent;
    if (parent < 0) return false;
    uint32_t subtype = static_cast<uint32_t>(ctx.subtype);
    const Handler* h = findEvent(parent, static_cast<uint32_t>(ctx.type), subtype);
    if (!h) return false;
    Handler copy = *h;
    return runHandler(self, copy, static_cast<uint32_t>(ctx.type), subtype, other);
}

// ---------------------------------------------------------------------------------------------
// Instances

Instance* Runner::createInstance(double x, double y, int32_t object, int32_t id, bool runCreate) {
    if (object < 0 || static_cast<size_t>(object) >= dw.objects.size()) {
        log("instance_create: bad object %d", object);
        return nullptr;
    }
    const GameObject& o = dw.objects[object];
    Instance* inst = new Instance();
    inst->id = id >= 0 ? id : nextInstanceId++;
    if (id >= nextInstanceId) nextInstanceId = id + 1;
    inst->object = object;
    inst->x = inst->xstart = inst->xprevious = x;
    inst->y = inst->ystart = inst->yprevious = y;
    inst->sprite = o.sprite;
    inst->mask = o.mask;
    inst->visible = o.visible;
    inst->solid = o.solid;
    inst->persistent = o.persistent;
    inst->depth = o.depth;
    inst->createOrder = createCounter++;
    inst->createdStep = stepCount;
    instances.push_back(inst);
    byId[inst->id] = inst;
    byObject[object].push_back(inst);
    if (inst->id == traceId())
        log("[trace] create %d (%.*s) step %llu in %s", inst->id, int(o.name.size()), o.name.data(),
            (unsigned long long)stepCount, vm.currentCodeName().c_str());
    if (runCreate) {
        if (dw.gen.gms2) performEvent(inst, EV_PRECREATE, 0);
        if (!inst->destroyed) performEvent(inst, EV_CREATE, 0);
    }
    return inst;
}

void Runner::destroyInstance(Instance* inst, bool runEvent) {
    if (!inst || inst->destroyed || inst == &globalSelf) return;
    if (inst->id == traceId())
        log("[trace] destroy %d step %llu in %s", inst->id, (unsigned long long)stepCount, vm.currentCodeName().c_str());
    inst->destroyed = true;
    if (runEvent) performEvent(inst, EV_DESTROY, 0);
    performEvent(inst, EV_CLEANUP, 0);
}

Instance* Runner::findById(int32_t id) {
    auto it = byId.find(id);
    if (it == byId.end() || !live(it->second)) return nullptr;
    return it->second;
}

Instance* Runner::findForAccess(int32_t id) {
    auto it = byId.find(id);
    return it == byId.end() ? nullptr : it->second;
}

Instance* Runner::firstOf(int32_t object) {
    Instance* found = nullptr;
    if (object < 0) {
        for (Instance* i : instances)
            if (live(i)) return i;
        return nullptr;
    }
    if (static_cast<size_t>(object) >= family.size()) return nullptr;
    // GameMaker returns the earliest-created instance among the object and its children.
    for (int32_t o : family[object])
        for (Instance* i : byObject[o])
            if (live(i) && (!found || i->createOrder < found->createOrder)) {
                found = i;
                break;
            }
    return found;
}

Instance* Runner::firstOfIncludingInactive(int32_t object) {
    if (Instance* i = firstOf(object)) return i;
    if (object < 0 || static_cast<size_t>(object) >= family.size()) return nullptr;
    Instance* found = nullptr;
    for (int32_t o : family[object])
        for (Instance* i : byObject[o])
            if (!i->destroyed && (!found || i->createOrder < found->createOrder)) found = i;
    return found;
}

void Runner::collectTargets(int32_t target, Instance* self, Instance* other, std::vector<Instance*>& out) {
    switch (target) {
        case INST_SELF:
            if (self) out.push_back(self);
            return;
        case INST_OTHER:
            if (other) out.push_back(other);
            return;
        case INST_ALL:
            forEachInstanceOf(-1, [&](Instance* i) { out.push_back(i); });
            return;
        case INST_GLOBAL:
            out.push_back(&globalSelf);
            return;
        default:
            break;
    }
    if (target >= 100000) {
        if (Instance* i = findById(target)) out.push_back(i);
        return;
    }
    if (target >= 0 && static_cast<size_t>(target) < dw.objects.size()) {
        forEachInstanceOf(target, [&](Instance* i) { out.push_back(i); });
        if (family[target].size() > 1)
            std::sort(out.begin(), out.end(), [](Instance* a, Instance* b) { return a->createOrder < b->createOrder; });
    }
}

bool Runner::objectIs(int32_t object, int32_t ancestor) const {
    int guard = 0;
    while (object >= 0 && static_cast<size_t>(object) < dw.objects.size() && guard++ < 64) {
        if (object == ancestor) return true;
        object = dw.objects[object].parent;
    }
    return false;
}

int Runner::instanceCountOf(int32_t object) {
    int n = 0;
    forEachInstanceOf(object, [&](Instance*) { n++; });
    return n;
}

void Runner::setActive(Instance* inst, bool active) {
    if (inst->id == traceId() && inst->active != active)
        log("[trace] %s %d step %llu in %s", active ? "activate" : "deactivate", inst->id,
            (unsigned long long)stepCount, vm.currentCodeName().c_str());
    inst->active = active;
}

// Instances in the order GameMaker dispatches events: object by object (ascending object
// index), and by creation within an object. Games depend on it, e.g. a level
// controller clears global.levelcontrol in its Room End after the player's Room End used it.
std::vector<Instance*> Runner::snapshot() {
    std::vector<Instance*> out;
    out.reserve(instances.size());
    for (Instance* i : instances)
        if (live(i)) out.push_back(i);
    std::stable_sort(out.begin(), out.end(), [](Instance* a, Instance* b) {
        return a->object != b->object ? a->object < b->object : a->createOrder < b->createOrder;
    });
    return out;
}

void Runner::freeInstance(Instance* inst) {
    if (inst->id == traceId()) log("[trace] free %d step %llu", inst->id, (unsigned long long)stepCount);
    removeInstanceFromLayer(inst);
    auto& list = byObject[inst->object];
    list.erase(std::remove(list.begin(), list.end(), inst), list.end());
    auto it = byId.find(inst->id);
    if (it != byId.end() && it->second == inst) byId.erase(it);
    delete inst;
}

void Runner::sweepDestroyed() {
    bool any = false;
    for (Instance* i : instances)
        if (i->destroyed) {
            any = true;
            break;
        }
    if (!any) return;
    std::vector<Instance*> keep;
    keep.reserve(instances.size());
    for (Instance* i : instances) {
        if (i->destroyed) freeInstance(i);
        else keep.push_back(i);
    }
    instances.swap(keep);
}

// ---------------------------------------------------------------------------------------------
// Rooms

int32_t Runner::roomOrderIndex(int32_t r) const {
    auto& order = dw.gen.roomOrder;
    for (size_t i = 0; i < order.size(); i++)
        if (order[i] == r) return static_cast<int32_t>(i);
    return -1;
}

int32_t Runner::roomNext(int32_t r) const {
    int32_t i = roomOrderIndex(r);
    if (i < 0 || static_cast<size_t>(i + 1) >= dw.gen.roomOrder.size()) return -1;
    return dw.gen.roomOrder[i + 1];
}

int32_t Runner::roomPrevious(int32_t r) const {
    int32_t i = roomOrderIndex(r);
    if (i <= 0) return -1;
    return dw.gen.roomOrder[i - 1];
}

void Runner::runInstanceCreate(Instance* inst, int32_t creationCode, int32_t preCreateCode) {
    if (dw.gen.gms2) {
        // GMS2 (as YoYo's runner, StartRoom): Pre-Create event (object variable definitions),
        // the instance's pre-create code (its variable overrides), Create, then creation code.
        performEvent(inst, EV_PRECREATE, 0);
        if (preCreateCode >= 0 && !inst->destroyed) vm.run(preCreateCode, inst, inst);
        if (!inst->destroyed) performEvent(inst, EV_CREATE, 0);
        if (creationCode >= 0 && !inst->destroyed) vm.run(creationCode, inst, inst);
        return;
    }
    if (dw.options.flags & OPT_CREATE_BEFORE_CODE) {
        performEvent(inst, EV_CREATE, 0);
        if (creationCode >= 0 && !inst->destroyed) vm.run(creationCode, inst, inst);
    } else {
        if (creationCode >= 0) vm.run(creationCode, inst, inst);
        if (!inst->destroyed) performEvent(inst, EV_CREATE, 0);
    }
}

void Runner::leaveRoom() {
    for (Instance* i : snapshot()) performEvent(i, EV_OTHER, OTHER_ROOM_END);
    if (dw.gen.gms2 && !roomPersistent)
        for (Instance* i : snapshot())
            if (!i->persistent) performEvent(i, EV_CLEANUP, 0);
    std::vector<Instance*> keep, saved;
    for (Instance* i : instances) {
        if (i->persistent && !i->destroyed) {
            // Carried to the next room: remember the layer by name (GameMaker rejoins it there).
            Layer* L = layerById(i->layer);
            i->persistLayer = L && !L->dynamic ? L->name : std::string();
            removeInstanceFromLayer(i);
            keep.push_back(i);
            continue;
        }
        if (roomPersistent && !i->destroyed) saved.push_back(i);
        else freeInstance(i);
    }
    if (roomPersistent) {
        SavedRoom& s = savedRooms[room];
        for (Instance* i : saved) {
            auto& list = byObject[i->object];
            list.erase(std::remove(list.begin(), list.end(), i), list.end());
            byId.erase(i->id);
        }
        s.instances = std::move(saved);
        s.tiles = tiles;
        s.layers = layers;
        s.layerElements = layerElements;
        s.roomCameras = std::move(roomCameras);
        s.views = views;
        s.backgrounds = backgrounds;
    }
    if (!roomPersistent)
        for (int32_t c : roomCameras)
            if (c >= 0 && static_cast<size_t>(c) < cameras.size()) cameras[c].live = false;
    roomCameras.clear();
    instances.swap(keep);
    if (particles) particles->roomEnd();
    if (sequences) sequences->roomEnd(roomPersistent);
}

int32_t Runner::createCamera(const Camera& c) {
    cameras.push_back(c);
    cameras.back().live = true;
    return static_cast<int32_t>(cameras.size() - 1);
}

void Runner::loadRoom(int32_t r, bool first) {
    if (r < 0 || static_cast<size_t>(r) >= dw.rooms.size()) {
        log("room_goto: invalid room %d", r);
        return;
    }
    const Room& rm = dw.rooms[r];
    room = r;
    roomWidth = rm.width;
    roomHeight = rm.height;
    roomSpeed = dw.gen.gms2 ? gameSpeed : (rm.speed ? rm.speed : 30);
    roomPersistent = rm.persistent;
    roomCaption = std::string(rm.caption);
    bgColor = rm.bgColor & 0xFFFFFF;
    bgShowColor = rm.drawBgColor;
    viewsEnabled = (rm.flags & 1) != 0;
    if (debugTrace) log("[room] -> %.*s", int(rm.name.size()), rm.name.data());

    auto saved = savedRooms.find(r);
    if (saved != savedRooms.end()) {
        for (Instance* i : saved->second.instances) {
            instances.push_back(i);
            byId[i->id] = i;
            byObject[i->object].push_back(i);
        }
        tiles = saved->second.tiles;
        layers = saved->second.layers;
        layerElements = saved->second.layerElements;
        roomCameras = saved->second.roomCameras;
        views = saved->second.views;
        backgrounds = saved->second.backgrounds;
        savedRooms.erase(saved);
        if (gfx) gfx->onRoomStart();
        for (Instance* i : snapshot()) performEvent(i, EV_OTHER, OTHER_ROOM_START);
        prefetchRoomTextures();
        return;
    }

    for (size_t i = 0; i < views.size(); i++) {
        View v;
        if (i < rm.views.size()) {
            auto& s = rm.views[i];
            v.visible = s.enabled;
            v.xview = s.viewX;
            v.yview = s.viewY;
            v.wview = s.viewW;
            v.hview = s.viewH;
            v.xport = s.portX;
            v.yport = s.portY;
            v.wport = s.portW;
            v.hport = s.portH;
            v.hborder = s.borderX;
            v.vborder = s.borderY;
            v.hspeed = s.speedX;
            v.vspeed = s.speedY;
            v.object = s.follow;
        }
        views[i] = v;
    }
    if (dw.gen.gms2) {
        if (defaultCamera < 0) defaultCamera = createCamera(Camera());
        // GMS2 rooms give every view its own camera, initialised from the view rect.
        for (size_t i = 0; i < views.size(); i++) {
            View& v = views[i];
            Camera c;
            c.x = v.xview;
            c.y = v.yview;
            c.w = v.wview;
            c.h = v.hview;
            c.hborder = v.hborder;
            c.vborder = v.vborder;
            c.hspeed = v.hspeed;
            c.vspeed = v.vspeed;
            c.object = v.object;
            auto set = roomCameraSet.find(r);
            if (set != roomCameraSet.end() && set->second[i] >= 0 &&
                static_cast<size_t>(set->second[i]) < cameras.size() && cameras[set->second[i]].live)
                c = cameras[set->second[i]];  // room_set_camera: the room gets a copy of that camera
            v.camera = createCamera(c);
            roomCameras.push_back(v.camera);
        }
        // Without views enabled, view 0 still renders the whole room through its camera.
        if (!viewsEnabled) {
            Camera& c0 = cameras[views[0].camera];
            c0.x = c0.y = 0;
            c0.w = rm.width;
            c0.h = rm.height;
        }
        Camera& def = cameras[defaultCamera];  // YoYo's UpdateDefaultCamera: the whole room
        def.x = def.y = 0;
        def.w = rm.width;
        def.h = rm.height;
        def.angle = 0;
    }
    for (size_t i = 0; i < backgrounds.size(); i++) {
        RoomBackgroundState b;
        if (i < rm.backgrounds.size()) {
            auto& s = rm.backgrounds[i];
            b.visible = s.enabled;
            b.foreground = s.foreground;
            b.index = s.bg;
            b.x = s.x;
            b.y = s.y;
            b.htiled = s.tileX;
            b.vtiled = s.tileY;
            b.hspeed = s.speedX;
            b.vspeed = s.speedY;
            b.stretch = s.stretch;
        }
        backgrounds[i] = b;
    }
    tiles.clear();
    for (auto& t : rm.tiles) {
        Tile tile;
        tile.id = static_cast<int32_t>(t.id);
        tile.background = t.bg;
        tile.x = t.x;
        tile.y = t.y;
        tile.left = static_cast<int32_t>(t.srcX);
        tile.top = static_cast<int32_t>(t.srcY);
        tile.width = static_cast<int32_t>(t.w);
        tile.height = static_cast<int32_t>(t.h);
        tile.depth = t.depth;
        tile.xscale = t.scaleX;
        tile.yscale = t.scaleY;
        tile.blend = t.color & 0xFFFFFF;
        tile.alpha = ((t.color >> 24) & 0xFF) / 255.0;
        tiles.push_back(tile);
        if (tile.id >= nextTileId) nextTileId = tile.id + 1;
    }
    layers.clear();
    layerElements.clear();
    std::unordered_map<uint32_t, const RoomLayer*> layerOfInstance;
    for (auto& L : rm.layers) {
        Layer rl;
        rl.id = static_cast<int32_t>(L.id);
        rl.name = std::string(L.name);
        rl.depth = L.depth;
        rl.visible = L.visible;
        rl.x = L.xOffset;
        rl.y = L.yOffset;
        rl.hspeed = L.hSpeed;
        rl.vspeed = L.vSpeed;
        rl.order = layerOrder++;
        rl.fx = roomLayerFx(*this, L);
        rl.fxEnabled = L.fxEnabled;
        layers.push_back(std::move(rl));
        Layer& lay = layers.back();
        // The layer's content becomes elements (instances are added once they exist).
        if (L.type == LayerType::Background) {
            LayerElement e;
            e.type = ElementType::Background;
            e.visible = L.bgVisible;
            e.foreground = L.bgForeground;
            e.sprite = L.bgSprite;
            e.htiled = L.bgHTiled;
            e.vtiled = L.bgVTiled;
            e.stretch = L.bgStretch;
            e.blend = L.bgColor & 0xFFFFFF;
            e.alpha = ((L.bgColor >> 24) & 0xFF) / 255.0;
            e.index = L.bgFirstFrame;
            e.bgPlayback = L.bgAnimSpeed;
            e.bgPlaybackType = L.bgAnimSpeedType;
            addLayerElement(lay, std::move(e));
        } else if (L.type == LayerType::Tiles) {
            LayerElement e;
            e.type = ElementType::Tilemap;
            e.tileset = L.tileset;
            e.tilesX = L.tilesX;
            e.tilesY = L.tilesY;
            e.tiles = L.tiles;
            addLayerElement(lay, std::move(e));
        }
        for (auto& sp : L.sprites) {
            LayerElement e;
            e.type = ElementType::Sprite;
            e.sprite = sp.sprite;
            e.x = sp.x;
            e.y = sp.y;
            e.xscale = sp.scaleX;
            e.yscale = sp.scaleY;
            e.angle = sp.rotation;
            e.blend = sp.color & 0xFFFFFF;
            e.alpha = ((sp.color >> 24) & 0xFF) / 255.0;
            e.index = sp.frameIndex;
            e.speed = sp.animSpeed;
            addLayerElement(lay, std::move(e));
        }
        for (auto& sq : L.sequences) {  // as layer_sequence_create, with the asset's settings
            int32_t id = sequences->createElement(lay, sq.sprite, sq.x, sq.y);
            if (LayerElement* e = elementById(id)) {
                e->xscale = sq.scaleX;
                e->yscale = sq.scaleY;
                e->angle = sq.rotation;
                e->blend = sq.color & 0xFFFFFF;
                e->alpha = ((sq.color >> 24) & 0xFF) / 255.0;
                e->speed = sq.animSpeed;
            }
            sequences->setElementStart(id, sq.frameIndex, sq.animSpeed);
        }
        for (auto& ps : L.particleSystems) {  // as part_system_create_layer with the element's settings
            if (!particles) break;
            int id = particles->instantiate(ps.system);
            if (ParticleSystem* sys = particles->system(id)) {
                sys->x = ps.x;
                sys->y = ps.y;
                sys->xscale = ps.scaleX;
                sys->yscale = ps.scaleY;
                sys->angle = ps.rotation;
                sys->color = ps.color & 0xFFFFFF;
                sys->alpha = ((ps.color >> 24) & 0xFF) / 255.0;
                sys->depth = L.depth;
                sys->layer = lay.id;
                sys->persistent = false;
            }
        }
        for (auto& t : L.legacyTiles) {
            Tile tile;
            tile.id = static_cast<int32_t>(t.id);
            tile.background = t.bg;
            tile.x = t.x;
            tile.y = t.y;
            tile.left = static_cast<int32_t>(t.srcX);
            tile.top = static_cast<int32_t>(t.srcY);
            tile.width = static_cast<int32_t>(t.w);
            tile.height = static_cast<int32_t>(t.h);
            tile.depth = L.depth;
            tile.layer = lay.id;
            tile.xscale = t.scaleX;
            tile.yscale = t.scaleY;
            tile.blend = t.color & 0xFFFFFF;
            tile.alpha = ((t.color >> 24) & 0xFF) / 255.0;
            tiles.push_back(tile);
        }
        for (uint32_t id : L.instanceIds) layerOfInstance[id] = &L;
        if (lay.id >= nextLayerId) nextLayerId = lay.id + 1;
    }
    if (gfx) gfx->onRoomStart();

    struct Created {
        Instance* inst;
        int32_t code, preCreate;
    };
    std::vector<Created> created;
    for (auto& ri : rm.instances) {
        if (ri.object < 0) continue;
        auto existing = byId.find(static_cast<int32_t>(ri.id));
        if (existing != byId.end() && !existing->second->destroyed) continue;  // persistent instance already here
        Instance* inst = createInstance(ri.x, ri.y, ri.object, static_cast<int32_t>(ri.id), false);
        if (!inst) continue;
        inst->imageXscale = ri.scaleX;
        inst->imageYscale = ri.scaleY;
        inst->imageAngle = ri.rotation;
        inst->imageBlend = ri.color & 0xFFFFFF;
        inst->imageAlpha = ((ri.color >> 24) & 0xFF) / 255.0;
        if (dw.gen.gms2) {
            inst->imageSpeed = ri.imageSpeed;
            inst->imageIndex = ri.imageIndex;
            auto lit = layerOfInstance.find(ri.id);
            if (lit != layerOfInstance.end()) {
                inst->depth = lit->second->depth;
                inst->layer = static_cast<int32_t>(lit->second->id);
            }
        }
        created.push_back({inst, ri.creationCode, ri.preCreateCode});
    }
    // Room instances join their layers in each layer's own order; persistent instances carried
    // over rejoin the layer with the same name, if this room has one.
    for (auto& L : rm.layers)
        if (Layer* lay = layerById(static_cast<int32_t>(L.id)))
            for (uint32_t id : L.instanceIds) {
                auto it = byId.find(static_cast<int32_t>(id));
                if (it != byId.end() && !it->second->destroyed && it->second->layerElement < 0)
                    addInstanceToLayer(it->second, *lay);
            }
    for (Instance* i : instances)
        if (i->persistent && i->layerElement < 0 && !i->persistLayer.empty())
            if (Layer* lay = layerByName(i->persistLayer)) addInstanceToLayer(i, *lay);
    for (auto& [inst, cc, pc] : created) {
        if (inst->destroyed) continue;
        // GMS2: once a Create event asks for another room, the remaining instances are still
        // created but skip Create and creation code (unless persistent). A title
        // room relies on this to stop reloading itself.
        bool roomChangeRequested = pendingRoom >= 0 || restartPending || gameRestartPending || gameEnded;
        if (dw.gen.gms2 && roomChangeRequested && !inst->persistent && !rm.persistent) continue;
        runInstanceCreate(inst, cc, pc);
    }
    if (first)
        for (Instance* i : snapshot()) performEvent(i, EV_OTHER, OTHER_GAME_START);
    if (rm.creationCode >= 0) vm.run(rm.creationCode, &globalSelf, &globalSelf);
    for (Instance* i : snapshot()) performEvent(i, EV_OTHER, OTHER_ROOM_START);
    prefetchRoomTextures();
    if (const char* g = getenv("GMR_TRACE_GLOBAL")) {
        Value* v = vm.findGlobal(vm.varIdForName(g));
        log("[trace] after room start: global.%s = %s", g, v ? v->repr().c_str() : "<none>");
    }
}

// The texture pages the room shows at its start (instances' sprites, tiles, backgrounds and layer
// elements), loaded together before its first frame rather than one by one as they're drawn.
void Runner::prefetchRoomTextures() {
    if (!gfx) return;
    std::vector<int> pages;
    auto addTpag = [&](int32_t t) {
        if (t >= 0 && static_cast<size_t>(t) < dw.tpag.size()) pages.push_back(dw.tpag[t].texture);
    };
    auto addSprite = [&](int32_t s) {
        if (s < 0 || static_cast<size_t>(s) >= dw.sprites.size() || dw.sprites[s].deleted) return;
        for (int32_t t : dw.sprites[s].frames) addTpag(t);
    };
    auto addBackground = [&](int32_t b) {
        if (b >= 0 && static_cast<size_t>(b) < dw.backgrounds.size()) addTpag(dw.backgrounds[b].tpag);
    };
    for (Instance* i : instances)
        if (!i->destroyed) addSprite(i->sprite);
    for (const Tile& t : tiles)  // GMS2's legacy tiles take a sprite (Graphics::drawTile)
        if (t.visible) dw.gen.gms2 ? addSprite(t.background) : addBackground(t.background);
    for (const RoomBackgroundState& b : backgrounds)
        if (b.visible) addBackground(b.index);
    for (const auto& [id, e] : layerElements) {
        if (e.type == ElementType::Background || e.type == ElementType::Sprite) addSprite(e.sprite);
        else if (e.type == ElementType::Tilemap) addBackground(e.tileset);
    }
    gfx->prefetchPages(std::move(pages));
}

// ---------------------------------------------------------------------------------------------
// Main loop

void Runner::prepare() {
    if (prepared_) return;
    prepared_ = true;
    registerAllBuiltins(*this);
    vm.init();
}

bool Runner::printCompatibilityReport() {
    auto& g = dw.gen;
    printf("Game:       %s\n", displayName.c_str());
    printf("GameMaker:  %s, bytecode %u\n", dw.engineVersion().c_str(), g.bytecodeVersion);
    if (!dw.supported()) {
        printf("PROBLEM:    %s\n", dw.unsupportedReason().c_str());
        printf("Verdict:    cannot run\n");
        return false;
    }
    prepare();
    printf("Content:    %zu rooms, %zu objects, %zu scripts, %zu sprites, %zu sounds, %zu shaders\n",
           dw.rooms.size(), dw.objects.size(), dw.scripts.size(), dw.sprites.size(), dw.sounds.size(),
           dw.shaders.size());
    bool ok = true;
    if (dw.code.empty()) {
        printf("PROBLEM:    no bytecode - the game was compiled with YYC (native code) and can't run.\n");
        printf("Verdict:    cannot run\n");
        return false;
    }
    // Native extension functions: emulated ones (FMODGMS) work, the rest return 0 or "".
    size_t stubbed = 0, emulated = 0;
    for (auto& f : dw.extensionFunctions) (extensionStubbed(*this, f.name) ? stubbed : emulated)++;
    if (stubbed)
        printf("WARNING:    the game uses extensions (DLLs); %zu of their %zu functions will do nothing.\n", stubbed,
               stubbed + emulated);
    // Engine functions the game calls that the runner doesn't implement, with call-site counts.
    std::vector<std::pair<uint32_t, std::string>> missing;
    for (auto& name : vm.unknownFunctions()) {
        for (auto& f : dw.functions)
            if (f.name == name) missing.push_back({f.occurrences, name});
    }
    std::sort(missing.rbegin(), missing.rend());
    if (missing.empty()) {
        // FUNC lists scripts and GML functions too; count the built-ins only.
        size_t builtins = 0;
        for (auto& f : dw.functions)
            if (vm.hasBuiltin(f.name) && !extensionStubbed(*this, f.name)) builtins++;
        printf("Functions:  all %zu engine functions the game calls are implemented\n", builtins);
    } else {
        printf("Functions:  %zu not implemented (calls do nothing and return 0):\n", missing.size());
        for (auto& [n, name] : missing) printf("              %-40s %u call sites\n", name.c_str(), n);
    }
    int missingFiles = 0;
    for (auto& s : dw.sounds) {
        bool embedded = (s.flags & 0x3) != 0;
        if (embedded || s.file.empty()) continue;
        std::string path = gameDir + "/" + std::string(s.file);
        if (FILE* f = fopen(path.c_str(), "rb")) fclose(f);
        else {
            if (missingFiles++ < 5) printf("WARNING:    missing audio file %s\n", std::string(s.file).c_str());
        }
    }
    if (missingFiles > 5) printf("WARNING:    ... %d missing audio files in total\n", missingFiles);
    printf("Verdict:    %s\n", !ok ? "cannot run" : missing.empty() ? "should run - try it"
                                                    : "may run - unimplemented functions could break parts of it");
    return ok;
}

void Runner::start(bool graphics) {
    double t0 = platform.now();
    prepare();
    double t1 = platform.now();
    gfx = std::make_unique<Graphics>(*this);
    gfx->init(graphics);
    double t2 = platform.now();
    audio = std::make_unique<Audio>(*this);
    double t3 = platform.now();
    ds = std::make_unique<DsStore>();
    files = std::make_unique<FileStore>(*this);
    particles = std::make_unique<ParticleStore>(*this);
    sequences = std::make_unique<SequenceStore>(*this);
    timeSources = std::make_unique<TimeSourceStore>(*this);
    nextInstanceId = static_cast<int32_t>(dw.gen.lastObj) + 1;
    if (nextInstanceId < 100000) nextInstanceId = 100000;
    nextTileId = std::max<int32_t>(10000000, static_cast<int32_t>(dw.gen.lastTile) + 1);
    startTime_ = platform.now();
    clockMs_ = 0;
    if (dw.gen.gms2 && dw.gen.gms2Fps > 0) gameSpeed = dw.gen.gms2Fps;
    fps = fpsReal = gameSpeed > 0 ? gameSpeed : 30;  // until the first second has been measured
    gStringArgsAsNumbers = dw.gen.gms2;
    // GMS2: the default camera is the first one made, before any game code, so it is camera 0
    // (games size surfaces with camera_get_view_width(0)); cameras the game creates
    // early come after it.
    if (dw.gen.gms2 && defaultCamera < 0) defaultCamera = createCamera(Camera());

    if (!dw.extensionFunctions.empty()) {
        std::string stubbed, emulated;
        for (auto& f : dw.extensionFunctions) (extensionStubbed(*this, f.name) ? stubbed : emulated) += " " + std::string(f.name);
        if (!emulated.empty()) log("[runner] extension functions provided by the runner:%s", emulated.c_str());
        if (!stubbed.empty()) log("[runner] extension functions (no native library; they return 0 or \"\"):%s", stubbed.c_str());
    }
    auto unknown = vm.unknownFunctions();
    if (!unknown.empty()) {
        std::string list;
        for (auto& u : unknown) list += " " + u;
        log("[runner] %zu functions are not implemented yet:%s", unknown.size(), list.c_str());
    }

    for (int32_t code : dw.globalInit) vm.run(code, &globalSelf, &globalSelf);
    double t4 = platform.now();
    log("[load] code %.2fs, textures %.2fs (%zu pages), audio %.2fs, global scripts %.2fs", t1 - t0, t2 - t1,
        dw.textures.size(), t3 - t2, t4 - t3);
    if (dw.gen.roomOrder.empty()) {
        log("game has no rooms");
        gameEnded = true;
        return;
    }
    double t5 = platform.now();
    loadRoom(dw.gen.roomOrder[0], true);
    log("[load] first room %.2fs", platform.now() - t5);
    started_ = true;
}

// Alarms (as YoYo's runner, HandleAlarm): only for objects with that alarm event; the value is
// truncated, counted down while >= 0, and the event fires when it reaches exactly 0 (so an
// alarm set to 0 never fires, and it reads 0 inside its own event).
void Runner::doAlarms() {
    for (Instance* inst : snapshot()) {
        // Instances created during this step start counting on the next one (GMS2).
        if (inst->createdStep == stepCount && stepCount > 0 && dw.gen.gms2) continue;
        for (int a = 0; a < 12; a++) {
            const Handler* h = findHandler(inst->object, EV_ALARM, a);
            if (!h) continue;
            double al = std::trunc(inst->alarm[a]);
            if (al >= 0) {
                al -= 1;
                inst->alarm[a] = al;
            }
            if (al == 0) {
                Handler copy = *h;
                runHandler(inst, copy, EV_ALARM, a, inst);
                if (inst->destroyed) break;
            }
        }
    }
}

// Timelines (as YoYo's runner, HandleTimeLine): the position moves by timeline_speed and every
// moment passed fires: [old, new) going forward, (new, old] going backward. Looping wraps past
// the last moment back to 0 (or below 0 to the last moment).
void Runner::doTimelines() {
    for (Instance* inst : snapshot()) {
        if (!inst->timelineRunning || inst->timelineIndex < 0 ||
            static_cast<size_t>(inst->timelineIndex) >= dw.timelines.size() || dw.timelines[inst->timelineIndex].deleted)
            continue;
        if (inst->createdStep == stepCount && stepCount > 0 && dw.gen.gms2) continue;
        const int32_t tlIndex = inst->timelineIndex;  // by index: moments may add timelines
        auto tl = [&]() -> const Timeline& { return dw.timelines[tlIndex]; };
        double from = inst->timelinePosition;
        double to = from + inst->timelineSpeed;
        inst->timelinePosition = to;
        std::vector<size_t> due;
        for (size_t m = 0; m < tl().moments.size(); m++) {
            double t = tl().moments[m].step;
            if (inst->timelineSpeed > 0 ? (t >= from && t < to) : (t <= from && t > to)) due.push_back(m);
        }
        if (inst->timelineSpeed < 0) std::reverse(due.begin(), due.end());
        for (size_t m : due) {
            // Copies: a moment's code may edit the timeline (timeline_moment_add_script, ...).
            std::vector<int32_t> codes = tl().moments[m].code, scripts = tl().moments[m].scripts;
            for (int32_t code : codes) {
                EventCtx saved = ctx;
                ctx = {-1, -1, -1};
                vm.run(code, inst, inst);
                ctx = saved;
                if (inst->destroyed) break;
            }
            for (int32_t script : scripts) {
                if (inst->destroyed) break;
                vm.callValue(vm.scriptValue(script), inst, inst, 0, nullptr);
            }
            if (inst->destroyed || m >= tl().moments.size()) break;
        }
        if (inst->destroyed || !inst->timelineLoop || tl().moments.empty()) continue;
        if (inst->timelineSpeed > 0 && inst->timelinePosition > tl().moments.back().step) inst->timelinePosition = 0;
        else if (inst->timelineSpeed < 0 && inst->timelinePosition < 0) inst->timelinePosition = tl().moments.back().step;
    }
}

// Keyboard events (as YoYo's runner, HandleKeyboard): held keys, then pressed, then released.
// Each pass goes key by key (every instance's event for that key), then vk_anykey (1) if any
// key was in that state, otherwise vk_nokey (0).
void Runner::doKeyboard() {
    auto pass = [&](uint32_t type, const auto& state) {
        std::vector<Instance*> list = snapshot();
        auto fire = [&](uint32_t key) {
            for (Instance* inst : list)
                if (live(inst))
                    if (const Handler* h = findHandler(inst->object, type, key)) {
                        Handler copy = *h;
                        runHandler(inst, copy, type, key, inst);
                    }
        };
        bool any = false;
        for (uint32_t key = 2; key < Input::kKeys; key++)
            if (state[key]) {
                any = true;
                fire(key);
            }
        fire(any ? 1 : 0);
    };
    pass(EV_KEYBOARD, input.down);
    pass(EV_KEYPRESS, input.pressed);
    pass(EV_KEYRELEASE, input.released);
}

// Mouse events, after the keyboard's. Those of an instance under the mouse (its mask at the
// mouse's room position): buttons held (0-2) or none (3), pressed (4-6), released (7-9), and Mouse
// Enter / Leave (10, 11) as the mouse moves onto or off it. Then the global ones, wherever the
// mouse is: held (50-52), pressed (53-55), released (56-58), and the wheel (60 up, 61 down).
void Runner::doMouse() {
    enum : uint32_t { HELD = 0, NONE = 3, PRESSED = 4, RELEASED = 7, ENTER = 10, LEAVE = 11, GLOBAL = 50,
                      WHEEL_UP = 60, WHEEL_DOWN = 61 };
    std::vector<Instance*> list;
    for (Instance* inst : snapshot())
        if (inst->object >= 0 && events[inst->object].hasMouse) list.push_back(inst);
    if (list.empty()) return;
    auto fire = [&](Instance* inst, uint32_t sub) {
        if (const Handler* h = live(inst) ? findHandler(inst->object, EV_MOUSE, sub) : nullptr) {
            Handler copy = *h;
            runHandler(inst, copy, EV_MOUSE, sub, inst);
        }
    };
    double mx, my;
    gfx->windowToRoom(input.mouseX, input.mouseY, mx, my);
    bool anyDown = input.mouseDown[0] || input.mouseDown[1] || input.mouseDown[2];
    for (Instance* inst : list) {
        if (!live(inst)) continue;
        bool over = pointInInstance(inst, mx, my, true);
        if (over) {
            for (uint32_t b = 0; b < 3; b++)
                if (input.mouseDown[b]) fire(inst, HELD + b);
            if (!anyDown) fire(inst, NONE);
            for (uint32_t b = 0; b < 3; b++)
                if (input.mousePressed[b]) fire(inst, PRESSED + b);
            for (uint32_t b = 0; b < 3; b++)
                if (input.mouseReleased[b]) fire(inst, RELEASED + b);
        }
        if (!live(inst)) continue;
        if (over != inst->mouseOver) {
            inst->mouseOver = over;
            fire(inst, over ? ENTER : LEAVE);
        }
    }
    for (Instance* inst : list) {
        for (uint32_t b = 0; b < 3; b++)
            if (input.mouseDown[b]) fire(inst, GLOBAL + b);
        for (uint32_t b = 0; b < 3; b++)
            if (input.mousePressed[b]) fire(inst, GLOBAL + 3 + b);
        for (uint32_t b = 0; b < 3; b++)
            if (input.mouseReleased[b]) fire(inst, GLOBAL + 6 + b);
        if (input.wheelUp) fire(inst, WHEEL_UP);
        if (input.wheelDown) fire(inst, WHEEL_DOWN);
    }
}

// Motion (as YoYo's runner, UpdatePositions), per instance: friction and gravity, then path
// following (Path End event), then x += hspeed, y += vspeed.
void Runner::doMotion() {
    for (Instance* inst : snapshot()) {
        if (!live(inst)) continue;
        // Friction moves speed towards 0 without crossing it; gravity adds along its direction.
        if (inst->friction != 0 && inst->speed != 0) {
            double s = inst->speed;
            double ns = s > 0 ? s - inst->friction : s + inst->friction;
            if ((s > 0 && ns < 0) || (s < 0 && ns > 0)) ns = 0;
            inst->setSpeedDir(ns, inst->direction);
        }
        if (inst->gravity != 0) inst->addSpeed(inst->gravityDirection, inst->gravity);
        if (adaptPath(inst)) performEvent(inst, EV_OTHER, OTHER_PATH_END);
        if (!live(inst)) continue;
        if (inst->hspeed != 0 || inst->vspeed != 0) {
            inst->x += inst->hspeed;
            inst->y += inst->vspeed;
        }
    }
}

// Outside/boundary events (as YoYo's runner, HandleOther), after instances have moved:
// Outside Room fires once when the instance leaves the room; Intersect Boundary fires every step
// its bounding box crosses a room edge; Outside View n fires every step it's outside view n, and
// Intersect View n Boundary every step its box crosses that view's edge. Without a sprite or
// mask the position is used (and the view boundary event doesn't apply).
void Runner::doOutsideRoom() {
    auto newThisStep = [&](const Instance* i) { return dw.gen.gms2 && i->createdStep == stepCount && stepCount > 0; };
    for (Instance* inst : snapshot()) {
        if (!live(inst) || newThisStep(inst)) continue;
        bool outsideEv = findHandler(inst->object, EV_OTHER, OTHER_OUTSIDE) != nullptr;
        bool boundaryEv = findHandler(inst->object, EV_OTHER, OTHER_BOUNDARY) != nullptr;
        bool anyView = false;
        if (viewsEnabled)
            for (int v = 0; v < 8 && !anyView; v++)
                anyView = findHandler(inst->object, EV_OTHER, 40 + v) || findHandler(inst->object, EV_OTHER, 50 + v);
        if (!outsideEv && !boundaryEv && !anyView) continue;
        BBox b = bbox(inst);
        double l = b.valid ? b.left : inst->x, r = b.valid ? b.right : inst->x;
        double t = b.valid ? b.top : inst->y, bt = b.valid ? b.bottom : inst->y;
        if (outsideEv) {
            bool outside = r < 0 || l > roomWidth || bt < 0 || t > roomHeight;
            if (outside && !inst->wasOutside) performEvent(inst, EV_OTHER, OTHER_OUTSIDE);
            inst->wasOutside = outside;
        }
        if (boundaryEv && live(inst)) {
            bool cross = b.valid ? (l < 0 || r > roomWidth || t < 0 || bt > roomHeight)
                                 : (inst->x < 0 || inst->x > roomWidth || inst->y < 0 || inst->y > roomHeight);
            if (cross) performEvent(inst, EV_OTHER, OTHER_BOUNDARY);
        }
        if (!anyView) continue;
        for (int v = 0; v < 8 && live(inst); v++) {
            const View& view = views[v];
            if (!view.visible) continue;
            double vx = view.xview, vy = view.yview, vr = view.xview + view.wview, vb = view.yview + view.hview;
            if (const Camera* c = cameraForView(v)) {
                vx = c->x;
                vy = c->y;
                vr = c->x + c->w;
                vb = c->y + c->h;
            }
            bool outside = r < vx || l > vr || bt < vy || t > vb;
            if (outside && findHandler(inst->object, EV_OTHER, 40 + v)) performEvent(inst, EV_OTHER, 40 + v);
            if (b.valid && live(inst) && findHandler(inst->object, EV_OTHER, 50 + v)) {
                bool inside = r < vr && l > vx && bt < vb && t > vy;
                if (!outside && !inside) performEvent(inst, EV_OTHER, 50 + v);
            }
        }
    }
}

// The object pairs tested for collisions, as GameMaker builds them (YoYo's runner,
// AddCollision): an object's own collision events, parents first, each pair only once - not
// when the other object (or a parent of either) already registered the reverse pair.
void Runner::buildCollisionPairs() {
    collisionPairs.clear();
    std::vector<char> done(dw.objects.size(), 0);
    auto parentOf = [&](int32_t o) {
        int32_t p = dw.objects[o].parent;
        return p >= 0 && static_cast<size_t>(p) < dw.objects.size() && p != o ? p : -1;
    };
    auto has = [&](int32_t a, int32_t b) {
        auto it = collisionPairs.find(a);
        return it != collisionPairs.end() && it->second.count(b);
    };
    std::function<void(int32_t, int)> add = [&](int32_t o, int depth) {
        if (done[o] || depth > 64) return;
        if (int32_t p = parentOf(o); p >= 0) add(p, depth + 1);
        for (auto& h : dw.objects[o].events[EV_COLLISION]) {
            int32_t other = static_cast<int32_t>(h.subtype);
            if (other < 0 || static_cast<size_t>(other) >= dw.objects.size()) continue;
            if (has(other, o)) continue;
            bool found = false;
            for (int32_t a = o; a >= 0 && !found; a = parentOf(a))
                for (int32_t b = other; b >= 0 && !found; b = parentOf(b))
                    if (has(b, a)) found = true;
            if (!found) collisionPairs[o].insert(other);
        }
        done[o] = 1;
    };
    for (size_t o = 0; o < dw.objects.size(); o++) add(static_cast<int32_t>(o), 0);
}

// Collisions (as YoYo's runner, HandleCollision): every registered pair is tested once per
// step. On contact, if either is solid both go back to their previous position; then both
// instances get their collision event (matched up the other object's parent chain first).
void Runner::doCollisions() {
    auto collisionEvent = [&](Instance* self, Instance* other) {
        performEvent(self, EV_COLLISION, static_cast<uint32_t>(other->object), other);
    };
    auto newThisStep = [&](const Instance* i) { return dw.gen.gms2 && i->createdStep == stepCount && stepCount > 0; };
    std::vector<Instance*> pool1, pool2;
    for (auto& [id1, targets] : collisionPairs) {
        pool1.clear();
        forEachInstanceOf(id1, [&](Instance* i) { pool1.push_back(i); });
        for (size_t a = 0; a < pool1.size(); a++) {
            Instance* inst1 = pool1[a];
            for (int32_t id2 : targets) {
                if (!live(inst1) || newThisStep(inst1)) break;
                pool2.clear();
                forEachInstanceOf(id2, [&](Instance* i) { pool2.push_back(i); });
                for (size_t b = 0; b < pool2.size(); b++) {
                    Instance* inst2 = pool2[b];
                    if (!live(inst1)) break;
                    if (inst2 == inst1 || !live(inst2) || newThisStep(inst2)) continue;
                    if (inst1->object == inst2->object && b < a) continue;  // same pair, other way round
                    if (!instancesCollide(inst1, inst1->x, inst1->y, inst2)) continue;
                    if (inst1->solid || inst2->solid) {
                        for (Instance* i : {inst1, inst2}) {
                            i->x = i->xprevious;
                            i->y = i->yprevious;
                            i->pathPosition = i->pathPositionPrevious;
                        }
                    }
                    // Both events run even if the first destroys an instance (it's only marked).
                    collisionEvent(inst1, inst2);
                    collisionEvent(inst2, inst1);
                }
            }
        }
    }
}

// image_index += image_speed, scaled in GMS2 by the sprite's playback speed (frames per
// game frame, or frames per second divided by the game speed). Without a sprite image_index
// still advances, as in GameMaker's runners.
void Runner::advanceImage(Instance* inst) {
    if (inst->sprite < 0 || static_cast<size_t>(inst->sprite) >= dw.sprites.size() ||
        dw.sprites[inst->sprite].frames.empty()) {
        inst->imageIndex += inst->imageSpeed;
        return;
    }
    const Sprite& sp = dw.sprites[inst->sprite];
    double rate = 1;
    if (dw.gen.gms2)
        rate = sp.playbackSpeedType == 0 ? sp.playbackSpeed / (roomSpeed > 0 ? roomSpeed : 60) : sp.playbackSpeed;
    inst->imageIndex += inst->imageSpeed * rate;
}

// Past the last frame (or before the first): wrap by one cycle and run Animation End.
void Runner::wrapImage(Instance* inst) {
    if (inst->sprite < 0 || static_cast<size_t>(inst->sprite) >= dw.sprites.size()) return;
    int n = imageNumber(inst);
    if (n <= 0) return;
    if (inst->imageIndex >= n) inst->imageIndex -= n;
    else if (inst->imageIndex < 0) inst->imageIndex += n;
    else return;
    performEvent(inst, EV_OTHER, OTHER_ANIMATION_END);
}

void Runner::doAnimation() {
    for (Instance* inst : snapshot()) {
        advanceImage(inst);
        wrapImage(inst);
    }
}

// One axis of a view following its target (YoYo's CCamera::Update): a border of half the view or
// more centres the target, otherwise the view moves just enough to keep it that far from the edge.
static void followTarget(double at, double pos, double size, double border, double& out) {
    if (2 * border >= size) out = at - size / 2;
    else if (at - border < pos) out = at - border;
    else if (at + border > pos + size) out = at + border - size;
}

void Runner::updateViews() {
    if (!viewsEnabled) return;
    // Only the cameras of visible views follow their target (YoYo: pCam.Update() per view).
    std::vector<int32_t> updated;
    for (const View& v : views) {
        if (!v.visible || v.camera < 0 || static_cast<size_t>(v.camera) >= cameras.size()) continue;
        if (std::find(updated.begin(), updated.end(), v.camera) != updated.end()) continue;
        updated.push_back(v.camera);
        Camera& c = cameras[v.camera];
        if (!c.live || c.object < 0) continue;
        Instance* t = c.object >= 100000 ? findById(c.object) : firstOf(c.object);
        if (!t) continue;
        double nx = c.x, ny = c.y;
        followTarget(std::floor(t->x), c.x, c.w, c.hborder, nx);
        followTarget(std::floor(t->y), c.y, c.h, c.vborder, ny);
        nx = std::max(0.0, std::min(nx, roomWidth - c.w));
        ny = std::max(0.0, std::min(ny, roomHeight - c.h));
        if (c.hspeed >= 0) nx = c.x + std::max(-c.hspeed, std::min(c.hspeed, nx - c.x));
        if (c.vspeed >= 0) ny = c.y + std::max(-c.vspeed, std::min(c.vspeed, ny - c.y));
        c.x = nx;
        c.y = ny;
    }
    for (View& v : views) {
        if (v.camera >= 0) continue;
        if (!v.visible || v.object < 0) continue;
        Instance* t = v.object >= 100000 ? findById(v.object) : firstOf(v.object);
        if (!t) continue;
        double nx = v.xview, ny = v.yview;
        followTarget(std::floor(t->x), v.xview, v.wview, v.hborder, nx);
        followTarget(std::floor(t->y), v.yview, v.hview, v.vborder, ny);
        nx = std::max(0.0, std::min(nx, roomWidth - v.wview));
        ny = std::max(0.0, std::min(ny, roomHeight - v.hview));
        if (v.hspeed >= 0) nx = v.xview + std::max(-v.hspeed, std::min(v.hspeed, nx - v.xview));
        if (v.vspeed >= 0) ny = v.yview + std::max(-v.vspeed, std::min(v.vspeed, ny - v.yview));
        v.xview = nx;
        v.yview = ny;
    }
}

bool Runner::step() {
    if (!started_ || gameEnded) return false;
    if (stepCount > 0) {
        double speed = dw.gen.gms2 ? gameSpeed : roomSpeed;
        clockMs_ += 1000.0 / (speed > 0 ? speed : 60);
    }
    stepCount++;
    vm.resetLoopCounter();
    input.beginStep();
    if (audio) audio->update();
    if (videoUpdate) videoUpdate();
    if (audio && !audioOutput) {
        // No device pulls samples: mix one frame's worth and drop it, so sounds still advance
        // (track positions, audio_is_playing and music-synced scenes behave as when heard).
        silentMixCarry += static_cast<double>(Audio::kRate) / (roomSpeed > 0 ? roomSpeed : 60);
        int n = static_cast<int>(silentMixCarry);
        silentMixCarry -= n;
        silentMixBuffer.resize(static_cast<size_t>(n) * 2);
        if (n > 0) audio->mix(silentMixBuffer.data(), n);
    }

    // GMS2 (YoYo's runner: RememberOldPositions, UpdateImages): remember positions and
    // advance animations, then wrap them and run Animation End, all before Begin Step.
    // GMS 1.4 animates after End Step (doAnimation below).
    for (Instance* i : instances) {
        i->xprevious = i->x;
        i->yprevious = i->y;
        i->pathPositionPrevious = i->pathPosition;
        if (dw.gen.gms2 && live(i)) advanceImage(i);
    }
    // Once a room change (or restart / game end) is requested, the rest of the step is
    // skipped, drawing included, as in YoYo's runner (GameMaker_DoAStep).
    auto changing = [&] { return pendingRoom >= 0 || restartPending || gameRestartPending || gameEnded; };
    [&] {
        if (dw.gen.gms2)
            for (Instance* i : snapshot()) wrapImage(i);
        if (changing()) return;
        if (dw.gen.gms2) updateLayers();
        if (sequences) sequences->stepEvent(STEP_BEGIN);
        for (Instance* i : snapshot()) performEvent(i, EV_STEP, STEP_BEGIN);
        if (changing()) return;
        processAsync();  // YoYo's runner handles async events right after Begin Step
        if (changing()) return;
        doTimelines();
        if (changing()) return;
        timeSources->update(static_cast<int64_t>(1000000.0 / (roomSpeed > 0 ? roomSpeed : 60)));
        if (changing()) return;
        doAlarms();
        if (changing()) return;
        doKeyboard();
        if (changing()) return;
        doMouse();
        if (changing()) return;
        if (sequences) sequences->update();  // also runs the sequences' own step events
        if (changing()) return;
        for (Instance* i : snapshot()) performEvent(i, EV_STEP, STEP_NORMAL);
        if (changing()) return;
        doMotion();
        doOutsideRoom();
        if (changing()) return;
        doCollisions();
        if (changing()) return;
        if (sequences) sequences->stepEvent(STEP_END);
        for (Instance* i : snapshot()) performEvent(i, EV_STEP, STEP_END);
        if (changing()) return;
        if (particles) particles->update();
        if (!dw.gen.gms2) doAnimation();
        updateViews();
        if (gfx) gfx->drawFrame();
    }();
    debugDump();
    sweepDestroyed();

    if (gameRestartPending) {
        gameRestartPending = false;
        leaveRoom();
        for (Instance* i : instances) freeInstance(i);
        instances.clear();
        for (auto& [r, s] : savedRooms)
            for (Instance* i : s.instances) delete i;
        savedRooms.clear();
        pendingRoom = -1;
        loadRoom(dw.gen.roomOrder[0], true);
    } else if (pendingRoom >= 0 || restartPending) {
        int32_t target = pendingRoom >= 0 ? pendingRoom : room;
        if (debugTrace && pendingRoom < 0) log("[room] restart (step %llu)", static_cast<unsigned long long>(stepCount));
        pendingRoom = -1;
        restartPending = false;
        if (target >= 0 && static_cast<size_t>(target) < dw.rooms.size()) {
            leaveRoom();
            sweepDestroyed();
            loadRoom(target, false);
        }
    }
    if (gameEnded) {
        for (Instance* i : snapshot()) performEvent(i, EV_OTHER, OTHER_GAME_END);
    }
    return !gameEnded;
}

// GMR_DUMP_OBJ=name[,name] and GMR_DUMP_EVERY=N: print every instance of those objects
// (built-in state and all variables) every N steps. A debugging aid for runtime bugs.
void Runner::debugDump() {
    static std::vector<int32_t> objs;
    static int every = 0;
    static bool init = false;
    if (!init) {
        init = true;
        const char* e = getenv("GMR_DUMP_OBJ");
        const char* n = getenv("GMR_DUMP_EVERY");
        every = n ? atoi(n) : 30;
        if (e) {
            std::string list = e;
            size_t p = 0;
            while (p <= list.size()) {
                size_t q = list.find(',', p);
                if (q == std::string::npos) q = list.size();
                std::string name = list.substr(p, q - p);
                for (size_t o = 0; o < dw.objects.size(); o++)
                    if (dw.objects[o].name == name) objs.push_back(static_cast<int32_t>(o));
                p = q + 1;
            }
        }
    }
    // GMR_LIST_AT=step: list the live instances at that step.
    static const long listAt = getenv("GMR_LIST_AT") ? atol(getenv("GMR_LIST_AT")) : -1;
    if (static_cast<long>(stepCount) == listAt) {
        std::map<std::string, int> counts;
        for (Instance* i : instances)
            if (live(i)) counts[std::string(dw.objects[i->object].name)]++;
        std::string out;
        for (auto& [n, c] : counts) out += " " + n + (c > 1 ? "x" + std::to_string(c) : "");
        log("[list] step %ld:%s", listAt, out.c_str());
    }
    // GMR_WATCH_GLOBAL=name: print a global whenever its value changes.
    static const char* watch = getenv("GMR_WATCH_GLOBAL");
    if (watch) {
        static std::string last;
        Value* v = vm.findGlobal(vm.varIdForName(watch));
        std::string now = v ? v->repr() : "<unset>";
        if (now != last) {
            log("[watch] step %llu global.%s = %s", static_cast<unsigned long long>(stepCount), watch, now.c_str());
            last = now;
        }
    }
    if (objs.empty() || every <= 0 || stepCount % every) return;
    for (Instance* i : instances) {
        if (std::find(objs.begin(), objs.end(), i->object) == objs.end()) continue;
        std::string vars;
        i->vars.forEach([&](int32_t id, const Value& v) {
            std::string r = v.repr();
            if (r.size() > 40) r = r.substr(0, 40) + "...";
            vars += " " + std::string(vm.varName(id)) + "=" + r;
        });
        std::string alarms;
        for (int a = 0; a < 12; a++)
            if (i->alarm[a] >= 0) alarms += " alarm" + std::to_string(a) + "=" + std::to_string(static_cast<int>(i->alarm[a]));
        vars = alarms + vars;
        log("[dump] step %llu %.*s #%d%s%s x=%.1f y=%.1f spr=%d img=%.2f alpha=%.2f vis=%d depth=%.0f%s",
            static_cast<unsigned long long>(stepCount), int(dw.objects[i->object].name.size()),
            dw.objects[i->object].name.data(), i->id, i->active ? "" : " INACTIVE", i->destroyed ? " DESTROYED" : "",
            i->x, i->y, i->sprite, i->imageIndex, i->imageAlpha, i->visible, i->depth, vars.c_str());
    }
}

void Runner::cameraMatrices(const Camera& c, float* view, float* proj) const {
    double cx = c.x + c.w / 2, cy = c.y + c.h / 2;
    double a = c.angle * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
    double v2[16] = {ca, -sa, 0, 0, sa, ca, 0, 0, 0, 0, 1, 0, -(ca * cx + sa * cy), -(-sa * cx + ca * cy), 16000, 1};
    double w = c.w ? c.w : 1, h = c.h ? c.h : 1;
    double p2[16] = {2 / w, 0, 0, 0, 0, 2 / h, 0, 0, 0, 0, 1 / (32000.0 - 1), 0, 0, 0, 1 / (1 - 32000.0), 1};
    for (int i = 0; i < 16; i++) {
        view[i] = static_cast<float>(c.customView ? c.viewMat[i] : v2[i]);
        proj[i] = static_cast<float>(c.customProj ? c.projMat[i] : p2[i]);
    }
}

Camera* Runner::cameraForView(int view) {
    if (view < 0 || view >= static_cast<int>(views.size())) return nullptr;
    int32_t c = views[view].camera;
    if (c < 0 || static_cast<size_t>(c) >= cameras.size() || !cameras[c].live) return nullptr;
    return &cameras[c];
}

void Runner::queueAsync(uint32_t subtype, std::vector<std::pair<std::string, Value>> fields) {
    asyncQueue.push_back({subtype, std::move(fields)});
}

void Runner::processAsync() {
    if (asyncQueue.empty()) return;
    std::vector<AsyncEvent> events;
    events.swap(asyncQueue);
    for (AsyncEvent& e : events) {
        int32_t id = ds->maps.create();
        DsMap* m = ds->maps.get(id);
        for (auto& [k, v] : e.fields) {
            ValueKey key = ValueKey::of(Value(k));
            m->order.push_back(key);
            m->values[key] = v;
        }
        int32_t saved = asyncLoad;
        asyncLoad = id;
        for (Instance* i : snapshot())
            if (live(i) && findHandler(i->object, EV_OTHER, e.subtype)) performEvent(i, EV_OTHER, e.subtype);
        asyncLoad = saved;
        ds->maps.destroy(id);
    }
}

void Runner::runLayerScript(int32_t layerId, const Value& fn, uint32_t drawSubtype) {
    if (fn.isUndefined()) return;
    EventCtx saved = ctx;
    ctx = {-1, EV_DRAW, static_cast<int32_t>(drawSubtype)};
    layerScriptSelf.layer = layerId;
    Value f = fn;
    vm.callValue(f, &layerScriptSelf, &layerScriptSelf, 0, nullptr);
    ctx = saved;
}

Layer* Runner::layerById(int32_t id) {
    for (auto& l : layers)
        if (l.id == id) return &l;
    return nullptr;
}

// Layer names match case-insensitively (YoYo's GetLayerFromName lower-cases both sides).
Layer* Runner::layerByName(const std::string& name) {
    auto same = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    };
    if (name.empty()) return nullptr;
    for (auto& l : layers)
        if (same(l.name, name)) return &l;
    return nullptr;
}

Layer* Runner::layerFromValue(const Value& v) {
    return v.isString() ? layerByName(v.str()) : layerById(v.toInt());
}

LayerElement* Runner::elementById(int32_t id) {
    auto it = layerElements.find(id);
    return it == layerElements.end() ? nullptr : &it->second;
}

int32_t Runner::addLayerElement(Layer& L, LayerElement e) {
    e.id = nextElementId++;
    e.layer = L.id;
    // Instances stay at the front in the order they were added; anything else goes right after
    // them (the intent of YoYo's AddNewElement; the C++ runner keeps instances first too).
    size_t at = 0;
    while (at < L.elements.size()) {
        const LayerElement* x = elementById(L.elements[at]);
        if (!x || x->type != ElementType::Instance) break;
        at++;
    }
    L.elements.insert(L.elements.begin() + static_cast<long>(at), e.id);
    int32_t id = e.id;
    layerElements.emplace(id, std::move(e));
    return id;
}

void Runner::removeLayerElement(int32_t id, bool destroyInstances) {
    LayerElement* e = elementById(id);
    if (!e) return;
    if (Layer* L = layerById(e->layer)) L->elements.erase(std::remove(L->elements.begin(), L->elements.end(), id), L->elements.end());
    if (e->type == ElementType::Instance) {
        if (Instance* inst = findById(e->instance)) {
            inst->layer = -1;
            inst->layerElement = -1;
            if (destroyInstances) destroyInstance(inst, true);
        }
    } else if (e->type == ElementType::Sequence && sequences) {
        sequences->elementRemoved(id);
    }
    layerElements.erase(id);
}

void Runner::addInstanceToLayer(Instance* inst, Layer& L) {
    removeInstanceFromLayer(inst);
    LayerElement e;
    e.type = ElementType::Instance;
    e.instance = inst->id;
    inst->layer = L.id;
    inst->depth = L.depth;
    inst->layerElement = addLayerElement(L, std::move(e));
}

void Runner::removeInstanceFromLayer(Instance* inst) {
    if (inst->layerElement >= 0) {
        if (LayerElement* e = elementById(inst->layerElement)) {
            if (Layer* L = layerById(e->layer))
                L->elements.erase(std::remove(L->elements.begin(), L->elements.end(), e->id), L->elements.end());
            layerElements.erase(e->id);
        }
    }
    inst->layerElement = -1;
    inst->layer = -1;
}

Layer& Runner::createLayer(double depth, const std::string& name, bool dynamic) {
    Layer L;
    L.id = nextLayerId++;
    L.depth = depth;
    L.name = name;
    L.dynamic = dynamic;
    L.order = layerOrder++;
    layers.push_back(std::move(L));
    return layers.back();
}

void Runner::destroyLayer(int32_t id) {
    Layer* L = layerById(id);
    if (!L) return;
    // Its elements go with it, instances included (as GameMaker's layer_destroy).
    std::vector<int32_t> els = L->elements;
    for (int32_t e : els) removeLayerElement(e, true);
    layers.erase(std::remove_if(layers.begin(), layers.end(), [&](const Layer& l) { return l.id == id; }), layers.end());
}

// Per step, before Begin Step (YoYo's runner, UpdateLayers): layers scroll by their speed;
// backgrounds advance by image_speed x their playback speed, sprite elements by image_speed x
// the sprite's playback speed (per second ones divided by the game speed).
void Runner::updateLayers() {
    double fps = roomSpeed > 0 ? roomSpeed : 60;
    for (Layer& L : layers) {
        L.x += L.hspeed;
        L.y += L.vspeed;
        for (int32_t id : L.elements) {
            LayerElement* e = elementById(id);
            if (!e) continue;
            if (e->type == ElementType::Background) {
                e->index += e->bgPlaybackType == 1 ? e->speed * e->bgPlayback : e->speed * e->bgPlayback / fps;
            } else if (e->type == ElementType::Sprite && e->sprite >= 0 &&
                       static_cast<size_t>(e->sprite) < dw.sprites.size()) {
                const Sprite& sp = dw.sprites[e->sprite];
                e->index += sp.playbackSpeedType == 1 ? e->speed * sp.playbackSpeed : e->speed * sp.playbackSpeed / fps;
            }
        }
    }
}

const Path* Runner::pathAt(int32_t index) const {
    if (index < 0) return nullptr;
    if (static_cast<size_t>(index) < dw.paths.size()) return &dw.paths[index];
    size_t r = static_cast<size_t>(index) - dw.paths.size();
    if (r < runtimePaths.size() && runtimePathLive[r]) return &runtimePaths[r];
    return nullptr;
}

void Runner::present(int screenW, int screenH) {
    if (gfx) gfx->present(screenW, screenH);
}

}  // namespace gm
