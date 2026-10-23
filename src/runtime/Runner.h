// The game runtime: instances, rooms, events and the per-step loop.
#pragma once

#include <array>
#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/SkeletonSprites.h"
#include "Input.h"
#include "Instance.h"
#include "Random.h"
#include "Platform.h"
#include "data/DataWin.h"
#include "vm/VM.h"

namespace gm {

class Graphics;
class Audio;
struct DsStore;
struct FileStore;
struct ParticleStore;
struct TimeSourceStore;
struct SequenceStore;

// Event sub-types used by the runtime.
enum StepKind : uint32_t { STEP_NORMAL = 0, STEP_BEGIN = 1, STEP_END = 2 };
enum DrawKind : uint32_t {
    DRAW_NORMAL = 0, DRAW_GUI = 64, DRAW_RESIZE = 65, DRAW_BEGIN = 72, DRAW_END = 73, DRAW_GUI_BEGIN = 74,
    DRAW_GUI_END = 75, DRAW_PRE = 76, DRAW_POST = 77
};
enum OtherKind : uint32_t {
    OTHER_OUTSIDE = 0, OTHER_BOUNDARY = 1, OTHER_GAME_START = 2, OTHER_GAME_END = 3, OTHER_ROOM_START = 4,
    OTHER_ROOM_END = 5, OTHER_NO_LIVES = 6, OTHER_ANIMATION_END = 7, OTHER_PATH_END = 8, OTHER_NO_HEALTH = 9,
    OTHER_USER0 = 10, OTHER_BROADCAST = 76
};

// GMS2 camera; views point at one. As YoYo's camera manager: ids are never reused, the default
// camera comes first, each room creates one camera per view (destroyed when a non-persistent
// room ends) and camera_create cameras last until camera_destroy.
struct Camera {
    bool live = true;
    double x = 0, y = 0, w = 0, h = 0, angle = 0;
    double hborder = 0, vborder = 0, hspeed = -1, vspeed = -1;
    int32_t object = -1;
    bool viewSet = false;  // centre came from camera_set_view_mat
    double cx = 0, cy = 0;
    // camera_set_view_mat / camera_set_proj_mat; cleared when the 2D rect is changed again
    // (YoYo's runner rebuilds the 2D view then).
    bool customView = false, customProj = false;
    double viewMat[16] = {}, projMat[16] = {};
};

struct View {
    int32_t camera = -1;  // GMS2: camera id, -1 = use the rect below (GMS 1.4)
    bool visible = false;
    double xview = 0, yview = 0, wview = 0, hview = 0;
    double xport = 0, yport = 0, wport = 0, hport = 0;
    double angle = 0;
    double hborder = 0, vborder = 0, hspeed = -1, vspeed = -1;
    int32_t object = -1;
    bool viewSet = false;  // centre came from camera_set_view_mat
    double cx = 0, cy = 0;
    int32_t surface = -1;  // view_surface_id: draw the view into this surface instead
};

struct RoomBackgroundState {
    bool visible = false, foreground = false;
    int32_t index = -1;
    double x = 0, y = 0;
    bool htiled = false, vtiled = false;
    double xscale = 1, yscale = 1, hspeed = 0, vspeed = 0;
    uint32_t blend = 0xFFFFFF;
    double alpha = 1;
    bool stretch = false;
};

struct Tile {
    int32_t id = 0;
    int32_t background = -1;
    double x = 0, y = 0;
    int32_t left = 0, top = 0, width = 0, height = 0;
    double depth = 0, xscale = 1, yscale = 1;
    uint32_t blend = 0xFFFFFF;
    double alpha = 1;
    bool visible = true;
};

// A GMS2 room layer at runtime.
// GMS2 layer elements (GameMaker's layerelementtype_* values).
enum class ElementType : int32_t {
    None = 0, Background = 1, Instance = 2, Sprite = 4, Tilemap = 5, ParticleSystem = 6, Tile = 7, Sequence = 8
};

struct LayerElement {
    int32_t id = 0, layer = -1;
    ElementType type = ElementType::None;
    // background and sprite elements
    int32_t sprite = -1;
    double index = 0, speed = 1;  // image index and image speed (multiplies the playback speed)
    double xscale = 1, yscale = 1, angle = 0;
    uint32_t blend = 0xFFFFFF;
    double alpha = 1;
    // background
    bool visible = true, foreground = false, htiled = false, vtiled = false, stretch = false;
    double bgPlayback = 0;       // the background's own playback speed
    uint32_t bgPlaybackType = 0; // 0 = frames per second, 1 = per game frame
    // sprite and tile map position
    double x = 0, y = 0;
    // instance
    int32_t instance = -1;
    // tile map
    int32_t tileset = -1;
    uint32_t tilesX = 0, tilesY = 0;
    std::vector<uint32_t> tiles;
    uint32_t tileMask = 0xFFFFFFFF;  // tilemap_set_mask: ANDed with the tile data when drawn
};

struct Layer {
    int32_t id = 0;
    std::string name;
    double depth = 0;
    bool visible = true, dynamic = false;  // dynamic: created at run time (layer_create)
    double x = 0, y = 0, hspeed = 0, vspeed = 0;
    uint64_t order = 0;              // insertion order, for layers of equal depth
    std::vector<int32_t> elements;   // element ids, drawn in this order
    Value scriptBegin, scriptEnd;    // layer_script_begin/end: run around the layer in every draw pass
    int32_t shader = -1;             // layer_shader
    Value fx;                        // layer effect (fx struct) or undefined; recorded, not yet drawn
    bool fxEnabled = true;
};

struct Handler {
    int32_t code = -1;
    int32_t owner = -1;  // object that defines the handler (for event_inherited)
};

struct ObjectEvents {
    std::unordered_map<uint64_t, Handler> handlers;               // resolved through parents
    bool hasDraw = false;
    bool hasMouse = false;  // any Mouse event
};

class Runner {
public:
    Runner(DataWin& dw, Platform& platform, std::string gameDir, std::string saveDir);
    ~Runner();

    // Loads assets and enters the first room. Call once after the graphics context exists.
    void start(bool graphics);
    // Registers built-ins and links the bytecode without starting the game (start() calls it).
    void prepare();
    // Prints a compatibility report for this game; returns false if it can't run at all.
    bool printCompatibilityReport();
    // One game step: events, motion, collisions, drawing. Returns false once the game ends.
    bool step();
    // Draws the finished frame to the real screen (letterboxed).
    void present(int screenW, int screenH);

    DataWin& dw;
    Platform& platform;
    VM vm;
    Input input;
    std::unique_ptr<Graphics> gfx;
    std::unique_ptr<Audio> audio;
    std::unique_ptr<DsStore> ds;
    std::unique_ptr<FileStore> files;
    std::unique_ptr<ParticleStore> particles;
    std::unique_ptr<SkeletonStore, void (*)(SkeletonStore*)> skeletons{nullptr, destroySkeletonStore};
    std::unique_ptr<SequenceStore> sequences;
    std::unique_ptr<TimeSourceStore> timeSources;
    std::string gameDir, saveDir;
    GMRandom rng;

    // ---- execution context (for event_inherited, other, etc.)
    struct EventCtx {
        int32_t object = -1;
        int32_t type = -1;
        int32_t subtype = -1;
    } ctx;
    Instance globalSelf;  // `self` for room creation code and other instance-less code

    // ---- instances
    std::vector<Instance*> instances;  // creation order, includes deactivated, excludes freed
    std::unordered_map<int32_t, Instance*> byId;
    std::vector<std::vector<Instance*>> byObject;  // direct members only
    std::vector<std::vector<int32_t>> family;      // object -> itself + all descendants
    int32_t nextInstanceId = 100000;
    uint64_t createCounter = 0;

    Instance* createInstance(double x, double y, int32_t object, int32_t id = -1, bool runCreate = true);
    void destroyInstance(Instance* inst, bool runEvent = true);
    Instance* findById(int32_t id);
    // Like findById, but also returns deactivated instances and ones destroyed earlier this step:
    // GameMaker keeps their variables reachable by id (games rely on this during hit-stop effects).
    Instance* findForAccess(int32_t id);
    Instance* firstOf(int32_t object);  // -1 = any instance
    // Variable lookup fallback: the first instance of the object even if it's deactivated.
    Instance* firstOfIncludingInactive(int32_t object);
    // Expands a GML instance target (object, instance id, self/other/all) into live instances.
    void collectTargets(int32_t target, Instance* self, Instance* other, std::vector<Instance*>& out);
    template <typename F> void forEachInstanceOf(int32_t object, F&& f) {
        if (object < 0) {
            for (size_t i = 0; i < instances.size(); i++)
                if (live(instances[i])) f(instances[i]);
            return;
        }
        if (static_cast<size_t>(object) >= family.size()) return;
        for (int32_t o : family[object])
            for (size_t i = 0; i < byObject[o].size(); i++)
                if (live(byObject[o][i])) f(byObject[o][i]);
    }
    static bool live(const Instance* i) { return !i->destroyed && i->active; }
    bool objectIs(int32_t object, int32_t ancestor) const;
    int instanceCountOf(int32_t object);
    void setActive(Instance* inst, bool active);

    // ---- events
    const Handler* findHandler(int32_t object, uint32_t type, uint32_t subtype) const;
    // As findHandler, but a collision event also matches the parents of the object `subtype`
    // names; `subtype` becomes the object of the event found.
    const Handler* findEvent(int32_t object, uint32_t type, uint32_t& subtype) const;
    bool performEvent(Instance* inst, uint32_t type, uint32_t subtype, Instance* other = nullptr);
    // Asynchronous events (Async - HTTP 62, Dialog 63, Save/Load 72, ...): queued with the
    // contents of their async_load ds_map and dispatched after Begin Step.
    void queueAsync(uint32_t subtype, std::vector<std::pair<std::string, Value>> fields);
    void processAsync();
    int32_t asyncLoad = -1;   // the async_load map during an async event
    int32_t eventData = -1;   // event_data: the map during a broadcast message
    // Command line (parameter_string(0) is the program) and game_change(directory, parameters):
    // the platform layer then loads the other game in the same window.
    std::vector<std::string> parameters;
    bool gameChangeRequested = false;
    std::string gameChangeDir, gameChangeParams;
    int32_t nextAsyncId = 1;  // request ids handed out by async functions
    bool performEventAs(Instance* inst, int32_t object, uint32_t type, uint32_t subtype, Instance* other = nullptr);
    bool eventInherited(Instance* self, Instance* other);
    bool runHandler(Instance* inst, const Handler& h, uint32_t type, uint32_t subtype, Instance* other);
    std::vector<ObjectEvents> events;

    // ---- rooms
    int32_t room = -1;
    int32_t pendingRoom = -1;
    bool restartPending = false, gameEnded = false, gameRestartPending = false;
    double roomWidth = 0, roomHeight = 0, roomSpeed = 30;
    bool roomPersistent = false;
    std::string roomCaption;
    uint32_t bgColor = 0;
    bool bgShowColor = true;
    bool viewsEnabled = false;
    std::array<View, 8> views;
    std::array<RoomBackgroundState, 8> backgrounds;
    std::vector<Tile> tiles;
    std::vector<Camera> cameras;
    std::vector<int32_t> roomCameras;  // the cameras the current room created for its views
    int32_t defaultCamera = -1;        // camera_get_default: covers the room when views are off
    int32_t activeCamera = -1;         // camera_get_active: the camera being drawn through
    int32_t createCamera(const Camera& c);
    std::unordered_map<int32_t, std::array<int32_t, 8>> roomCameraSet;  // room_set_camera for other rooms
    std::vector<Layer> layers;
    std::unordered_map<int32_t, LayerElement> layerElements;
    // Element ids start high so a tilemap id passed to place_meeting & co. can't be mistaken for
    // an object index or instance id.
    static constexpr int32_t kElementIdBase = 0x10000000;
    int32_t nextLayerId = 1000000, nextElementId = kElementIdBase;
    uint64_t layerOrder = 0;
    double gameSpeed = 60;  // GMS2 game speed (frames per second)
    Camera* cameraForView(int view);
    // The camera's view and projection matrices (its own, or the 2D ones for its rect).
    void cameraMatrices(const Camera& c, float* view, float* proj) const;
    Layer* layerById(int32_t id);
    Layer* layerByName(const std::string& name);
    Layer* layerFromValue(const Value& v);  // layer id or name
    // Runs a layer's begin/end script (as YoYo's ExecuteLayerScript) with event_type set to the draw pass.
    void runLayerScript(int32_t layerId, const Value& fn, uint32_t drawSubtype);
    Instance layerScriptSelf;  // `self` for layer scripts
    struct AsyncEvent {
        uint32_t subtype;
        std::vector<std::pair<std::string, Value>> fields;
    };
    std::vector<AsyncEvent> asyncQueue;
    LayerElement* elementById(int32_t id);
    // Adds an element to a layer as GameMaker does (instances first, in order; other elements
    // right after them) and returns its id.
    int32_t addLayerElement(Layer& L, LayerElement e);
    void removeLayerElement(int32_t id, bool destroyInstances);
    void addInstanceToLayer(Instance* inst, Layer& L);
    void removeInstanceFromLayer(Instance* inst);
    Layer& createLayer(double depth, const std::string& name, bool dynamic);
    void destroyLayer(int32_t id);
    void updateLayers();  // per step: layer scrolling, background/sprite element animation
    // Paths created at runtime (path_add, mp_grid_path) follow the data.win paths.
    std::vector<Path> runtimePaths;
    std::vector<bool> runtimePathLive;
    const Path* pathAt(int32_t index) const;
    uint64_t pathVersion = 0;  // bumped when a runtime path changes
    int32_t nextTileId = 10000000;
    uint32_t tileGlobalMask = 0xFFFFFFFF;  // tilemap_set_global_mask
    std::function<void()> videoUpdate;  // VideoLib: advance the playing video once per step
    bool audioOutput = false;  // the host plays (or records) the mix; otherwise step() advances it silently
    double silentMixCarry = 0;
    std::vector<float> silentMixBuffer;
    int32_t viewCurrent = 0;
    uint64_t stepCount = 0;
    double fps = 0, fpsReal = 0;
    double score = 0, lives = -1, health = 100;

    struct SavedRoom {
        std::vector<Instance*> instances;
        std::vector<Tile> tiles;
        std::vector<Layer> layers;
        std::unordered_map<int32_t, LayerElement> layerElements;
        std::vector<int32_t> roomCameras;
        std::array<View, 8> views;
        std::array<RoomBackgroundState, 8> backgrounds;
    };
    std::unordered_map<int32_t, SavedRoom> savedRooms;

    void gotoRoom(int32_t r) {
        if (debugTrace && r >= 0 && static_cast<size_t>(r) < dw.rooms.size())
            log("[room] goto %.*s requested by %s (step %llu)", int(dw.rooms[r].name.size()), dw.rooms[r].name.data(),
                vm.currentCodeName().c_str(), static_cast<unsigned long long>(stepCount));
        pendingRoom = r;
    }
    int32_t roomOrderIndex(int32_t r) const;
    int32_t roomNext(int32_t r) const;
    int32_t roomPrevious(int32_t r) const;

    // ---- motion / geometry helpers
    struct BBox {
        double left, top, right, bottom;
        bool valid;
    };
    BBox bbox(const Instance* inst) const;
    // inst's bounding box at (x, y) over any set tile of a tilemap element (YoYo's
    // Tilemap_InstancePlace without per-pixel tile masks).
    bool tilemapPlace(const Instance* inst, double x, double y, int32_t element);
    BBox bboxAt(const Instance* inst, double x, double y) const;
    bool instancesCollide(const Instance* a, double ax, double ay, const Instance* b) const;
    bool pointInInstance(const Instance* inst, double px, double py, bool precise) const;
    bool rectInInstance(const Instance* inst, double x1, double y1, double x2, double y2, bool precise) const;
    bool lineInInstance(const Instance* inst, double x1, double y1, double x2, double y2, bool precise) const;
    bool circleInInstance(const Instance* inst, double cx, double cy, double rx, double ry, bool precise) const;
    int32_t maskSprite(const Instance* inst) const;
    int imageNumber(const Instance* inst) const;

    // ---- diagnostics: runtime errors, code that can't compile, unimplemented engine functions
    // and other warnings. Printed to stderr; also collected when collectDiagnostics is set
    // (--smoke), keyed by message.
    enum class DiagKind { Error, Compile, Unimplemented, Warning };
    struct DiagEntry {
        DiagKind kind = DiagKind::Warning;
        long count = 0;
        std::string phase;              // smoke phase when first seen ("boot", "rooms")
        std::vector<std::string> rooms;  // distinct rooms, in order seen
    };
    void report(DiagKind kind, const std::string& msg);
    bool collectDiagnostics = false;
    std::string diagPhase;
    std::map<std::string, DiagEntry> diagnostics;

    // ---- misc
    void log(const char* fmt, ...);
    double currentTimeMs() const;
    mutable double clockMs_ = 0;  // fixedClock time
    bool debugTrace = false;
    bool glES = false;
    // --fast testing: current_time / get_timer advance one frame per step, and 1 us per read so
    // games that busy-wait on the clock (hit-stop freezes) still get out of the loop.
    bool fixedClock = false;
    int64_t fixedSeed = -1;  // >= 0: deterministic RNG, also for randomize() (testing)  // shaders target GLSL ES (Switch) instead of desktop GLSL 1.20
    std::string displayName;

private:
    void buildEventTables();
    void loadRoom(int32_t r, bool first);
    void leaveRoom();
    void buildCollisionPairs();
    std::map<int32_t, std::set<int32_t>> collisionPairs;  // object -> objects it's tested against
    void advanceImage(Instance* inst);
    void wrapImage(Instance* inst);
    void runInstanceCreate(Instance* inst, int32_t creationCode, int32_t preCreateCode = -1);
    void doAlarms();
    void doTimelines();
    void doKeyboard();
    void doMouse();
    void doMotion();
    bool adaptPath(Instance* inst);  // path following; true at a path end
    void doCollisions();
    void doOutsideRoom();
    void doAnimation();
    void updateViews();
    void sweepDestroyed();
    void debugDump();
    void freeInstance(Instance* inst);
    std::vector<Instance*> snapshot();

    double startTime_ = 0;
    bool started_ = false;
    bool prepared_ = false;
};

}  // namespace gm
