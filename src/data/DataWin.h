// In-memory model of a GameMaker data.win (a.k.a. game.droid / game.unx / game.ios).
// Supports GameMaker Studio 1.4 (bytecode 15/16) and GameMaker Studio 2 / GameMaker
// (bytecode 17). Version-dependent layouts are inferred from the file itself.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Reader.h"

namespace gm {

struct GeneralInfo {
    bool disableDebugger = false;
    uint8_t bytecodeVersion = 0;
    std::string_view fileName, config, name, displayName;
    uint32_t lastObj = 0, lastTile = 0, gameId = 0;
    uint32_t major = 0, minor = 0, release = 0, build = 0;
    uint32_t windowWidth = 0, windowHeight = 0;
    uint32_t infoFlags = 0;
    int32_t steamAppId = 0;
    std::vector<int32_t> roomOrder;
    bool gms2 = false;   // GEN8 major >= 2
    bool gms23 = false;  // GameMaker Studio 2.3+ (functions, structs, sequences)
    float gms2Fps = 0;   // GMS2 game speed (rooms no longer carry it)
};

// OPTN flags that change runtime semantics.
enum OptionFlags : uint64_t {
    OPT_FULLSCREEN = 0x1,
    OPT_INTERPOLATE = 0x2,
    OPT_SHOW_CURSOR = 0x10,
    OPT_SIZEABLE = 0x20,
    OPT_UNINIT_VARS_ZERO = 0x400000,     // "treat uninitialized variables as 0"
    OPT_CREATE_BEFORE_CODE = 0x800000,   // Create event runs before instance creation code
    OPT_COPY_ON_WRITE = 0x20000000,      // "enable copy on write" for arrays (GMS 2.3+)
};

struct Options {
    uint64_t flags = OPT_CREATE_BEFORE_CODE;
    int32_t scale = 0;
    uint32_t windowColor = 0;
};

struct TexturePageItem {
    uint16_t srcX, srcY, srcW, srcH;
    uint16_t tgtX, tgtY, tgtW, tgtH;
    uint16_t boundW, boundH;
    int16_t texture;
};

enum class TextureFormat : uint8_t { None, Png, Qoi, Bz2Qoi };

struct Texture {
    uint32_t scaled = 0;
    TextureFormat format = TextureFormat::None;  // None: loaded externally (texture groups)
    const uint8_t* data = nullptr;               // points into the file image
    size_t size = 0;
    uint32_t width = 0, height = 0;              // 0 if unknown until decoded
    uint32_t groupIndex = 0;                     // its place in its texture group (2022.9+)
    std::string externalFile;                    // dynamic texture groups: its file next to the data
};

// Nine-slice drawing (2.3.2+): fixed-size borders, and per slice (left, top, right, bottom,
// centre) a tile mode: 0 stretch, 1 repeat, 2 mirror, 3 blank repeat (once, then empty), 4 hide.
struct NineSlice {
    bool enabled = false;
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    int32_t modes[5] = {0, 0, 0, 0, 0};
};

struct Sprite {
    std::string_view name;
    uint32_t width = 0, height = 0;
    int32_t marginLeft = 0, marginRight = 0, marginBottom = 0, marginTop = 0;
    bool transparent = false, smooth = false, preload = false;
    uint32_t bboxMode = 0;
    bool sepMasks = false;
    int32_t originX = 0, originY = 0;
    bool unsupportedType = false;   // SWF sprites, and skeleton sprites in formats not read
    int32_t skeleton = -1;          // skeleton sprites: index into DataWin::skeletons
    bool deleted = false;           // sprite_delete
    float playbackSpeed = 1;        // GMS2: frames per game frame (type 1) or per second (type 0)
    uint32_t playbackSpeedType = 1;
    NineSlice nineSlice;
    std::vector<int32_t> frames;    // indices into DataWin::tpag
    std::vector<const uint8_t*> masks;  // 1bpp, ((width+7)/8) bytes per row
};

// A skeleton sprite's data as stored (read by the skeleton module, runtime/SkeletonSprites.h);
// its texture pages are the sprite's frames, in order.
struct SkeletonSprite {
    uint32_t version = 0;
    const uint8_t* skeleton = nullptr;
    uint32_t skeletonSize = 0;
    const uint8_t* atlas = nullptr;
    uint32_t atlasSize = 0;
};

struct Background {  // GMS2: tileset
    std::string_view name;
    bool transparent = false, smooth = false, preload = false;
    int32_t tpag = -1;
    // GMS2 tileset layout
    uint32_t tileWidth = 0, tileHeight = 0, borderX = 0, borderY = 0, columns = 0;
    uint32_t framesPerTile = 1, tileCount = 0;
    int64_t frameTime = 0;  // microseconds per animation frame
    std::vector<uint32_t> tileIds;  // tileCount * framesPerTile
};

struct Glyph {
    uint16_t ch, x, y, w, h;
    int16_t shift, offset;
};

struct Font {
    std::string_view name, displayName;
    float emSize = 0;
    bool bold = false, italic = false;
    uint16_t rangeStart = 0;
    uint8_t charset = 0, antiAlias = 0;
    uint32_t rangeEnd = 0;
    int32_t tpag = -1;
    float scaleX = 1, scaleY = 1;
    int32_t ascenderOffset = 0;            // GMS2 2.0.6+
    int32_t ascender = 0, lineHeight = 0;  // GMS2 2022+
    int32_t sdfSpread = 0;                 // 2023.2+: non-zero for SDF fonts
    std::vector<Glyph> glyphs;
};

struct Sound {
    std::string_view name, type, file;
    uint32_t flags = 0, effects = 0;
    float volume = 1, pitch = 0;
    int32_t group = 0;       // audio group: 0 = data.win, N = audiogroupN.dat
    int32_t audioFile = -1;  // index into that group's AUDO; only meaningful when flags & 0x3 (embedded/compressed)
};

struct AudioBlob {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct PathPoint { float x, y, speed; };
struct Path {
    std::string_view name;
    bool smooth = false, closed = false;
    uint32_t precision = 4;
    std::vector<PathPoint> points;
};

struct Script {
    std::string_view name;
    int32_t code = -1;
    bool constructor = false;  // GMS 2.3 constructor function
};

// GameMaker event types (index into GameObject::events).
enum EventType : uint32_t {
    EV_CREATE = 0, EV_DESTROY, EV_ALARM, EV_STEP, EV_COLLISION, EV_KEYBOARD,
    EV_MOUSE, EV_OTHER, EV_DRAW, EV_KEYPRESS, EV_KEYRELEASE, EV_TRIGGER,
    EV_CLEANUP, EV_GESTURE, EV_PRECREATE,
    EV_COUNT
};

struct EventHandler {
    uint32_t subtype = 0;
    std::vector<int32_t> code;  // code entry per action (normally exactly one)
};

struct GameObject {
    std::string_view name;
    int32_t sprite = -1;
    bool visible = true, solid = false;
    int32_t depth = 0;
    bool persistent = false;
    int32_t parent = -100;  // -100 = no parent
    int32_t mask = -1;
    bool physics = false;
    bool managed = true;  // GMS 2022.5+
    std::array<std::vector<EventHandler>, EV_COUNT> events;
};

struct RoomBackground {
    bool enabled, foreground;
    int32_t bg, x, y;
    bool tileX, tileY;
    int32_t speedX, speedY;
    bool stretch;
};

struct RoomView {
    bool enabled;
    int32_t viewX, viewY, viewW, viewH;
    int32_t portX, portY, portW, portH;
    uint32_t borderX, borderY;
    int32_t speedX, speedY;
    int32_t follow;
};

struct RoomInstance {
    int32_t x, y, object;
    uint32_t id;
    int32_t creationCode;
    float scaleX, scaleY;
    float imageSpeed;
    int32_t imageIndex;
    uint32_t color;
    float rotation;
    int32_t preCreateCode;
};

struct RoomTile {
    int32_t x, y, bg;
    uint32_t srcX, srcY, w, h;
    int32_t depth;
    uint32_t id;
    float scaleX, scaleY;
    uint32_t color;
};

enum class LayerType : uint32_t { Path = 0, Background = 1, Instances = 2, Assets = 3, Tiles = 4, Effect = 6 };

// A particle system placed on an asset layer in the room editor (2023.2+).
struct RoomParticleSystem {
    std::string_view name;
    int32_t id;      // element id (0x800000 and up)
    int32_t system;  // PSYS index
    int32_t x, y;
    float scaleX, scaleY;
    uint32_t color;  // ABGR
    float rotation;
};

struct RoomSpriteAsset {
    std::string_view name;
    int32_t sprite;
    int32_t x, y;
    float scaleX, scaleY;
    uint32_t color;
    float animSpeed;
    uint32_t animSpeedType;
    float frameIndex, rotation;
};

struct RoomLayer {
    std::string_view name;
    uint32_t id = 0;
    LayerType type = LayerType::Instances;
    int32_t depth = 0;
    float xOffset = 0, yOffset = 0, hSpeed = 0, vSpeed = 0;
    bool visible = true;
    // Instances
    std::vector<uint32_t> instanceIds;
    // Background
    bool bgVisible = true, bgForeground = false, bgHTiled = false, bgVTiled = false, bgStretch = false;
    int32_t bgSprite = -1;
    uint32_t bgColor = 0xFFFFFFFF;
    float bgFirstFrame = 0, bgAnimSpeed = 0;
    uint32_t bgAnimSpeedType = 0;
    // Tiles
    int32_t tileset = -1;
    uint32_t tilesX = 0, tilesY = 0;
    std::vector<uint32_t> tiles;  // GMS2 tile data: index | flip/mirror/rotate bits
    // Assets
    std::vector<struct RoomTile> legacyTiles;
    std::vector<RoomSpriteAsset> sprites;
    std::vector<RoomSpriteAsset> sequences;  // same layout; `sprite` is the sequence
    std::vector<RoomParticleSystem> particleSystems;  // 2023.2+
    // Layer effect (2022.1+): filter type (e.g. "_filter_tintfilter") and its parameters as text.
    bool fxEnabled = false;
    std::string_view fxType;
    std::vector<std::pair<std::string_view, std::string_view>> fxParams;  // name -> value
};

struct Room {
    std::string_view name, caption;
    uint32_t width = 0, height = 0, speed = 30;
    bool persistent = false;
    uint32_t bgColor = 0;
    bool drawBgColor = true;
    int32_t creationCode = -1;
    uint32_t flags = 0;
    std::vector<RoomBackground> backgrounds;
    std::vector<RoomView> views;
    std::vector<RoomInstance> instances;
    std::vector<RoomTile> tiles;
    std::vector<RoomLayer> layers;  // GMS2
};

struct TimelineMoment {
    uint32_t step;
    std::vector<int32_t> code;
    std::vector<int32_t> scripts;  // timeline_moment_add_script: script assets run after the code
};
struct Timeline {
    std::string_view name;
    std::vector<TimelineMoment> moments;  // in step order
    bool deleted = false;                 // timeline_delete
};

struct Shader {
    std::string_view name;
    uint32_t type = 0;
    std::string_view glslesVertex, glslesFragment;
    std::string_view glslVertex, glslFragment;
    std::vector<std::string_view> attributes;
};

struct CodeEntry {
    std::string_view name;
    uint32_t length = 0;      // bytes of bytecode
    uint16_t locals = 0;
    uint16_t args = 0;
    uint32_t bytecode = 0;    // absolute file offset of the first instruction
    uint32_t offset = 0;      // entry point inside the blob (shared blobs)
};

// VARI entry.
struct Variable {
    std::string_view name;
    int32_t instanceType = 0;
    int32_t varId = 0;
    uint32_t occurrences = 0;
    uint32_t firstAddress = 0;
};

// FUNC entry: a built-in function or script referenced by Call.
struct Function {
    std::string_view name;
    uint32_t occurrences = 0;
    uint32_t firstAddress = 0;
};

// ACRV (GMS 2.3+): animation curves. Points carry bezier handles since 2.3.1.
struct AnimCurvePoint {
    float x = 0, value = 0, tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
};
struct AnimCurveChannel {
    std::string_view name;
    int32_t type = 0;  // 0 linear, 1 smooth (centripetal Catmull-Rom), 2 bezier
    uint32_t iterations = 16;
    std::vector<AnimCurvePoint> points;
};
struct AnimCurve {
    std::string_view name;
    int32_t graphType = 0;
    std::vector<AnimCurveChannel> channels;
};

// SEQN (GMS 2.3+): sequences. A track's model name says what its keyframes hold.
struct SeqChannel {
    int32_t channel = 0;
    float real = 0;             // real tracks; colour tracks keep the colour's bits in `colour`
    uint32_t colour = 0;
    int32_t curve = -1;         // animation curve asset driving this key, or -1
    int32_t embeddedCurve = -1; // index into DataWin::seqCurves, or -1
    int32_t index = -1;         // graphic/instance/sequence/sprite-frames/bool/audio: asset or value
    int32_t mode = 0;           // audio: playback mode
    std::string_view text;      // string tracks, moments (function name)
    std::vector<std::string_view> messages;  // broadcast message keys
};
struct SeqKeyframe {
    float key = 0, length = 1;
    bool stretch = false, disabled = false;
    std::vector<SeqChannel> channels;
};
struct SeqTrack {
    std::string_view model, name;
    int32_t builtinName = 0, traits = 0, interpolation = 0;
    bool creation = false;
    std::vector<int32_t> tags;
    std::vector<SeqTrack> tracks;
    std::vector<SeqKeyframe> keys;
};
// PSEM (2023.2+): an emitter of a particle system asset, with the particle type it emits.
struct PsEmitter {
    std::string_view name;
    bool enabled = true;
    int32_t mode = 0;  // 0 stream, 1 burst
    float count = 0;
    bool relative = false;  // 2023.8+
    float delayMin = 0, delayMax = 0, intervalMin = 0, intervalMax = 0;
    int32_t delayUnit = 0, intervalUnit = 0;  // 0 seconds, 1 frames
    int32_t distribution = 0, shape = 0;
    float regionX = 0, regionY = 0, regionW = 0, regionH = 0, rotation = 0;
    int32_t sprite = -1, texture = 0;
    float frameIndex = 0;
    bool animate = false, stretch = false, randomFrame = false;
    uint32_t startColor = 0xFFFFFFFF, midColor = 0xFFFFFFFF, endColor = 0xFFFFFFFF;  // alpha in the top byte
    bool additive = false;
    float lifeMin = 0, lifeMax = 0, scaleX = 1, scaleY = 1;
    float sizeMinX = 1, sizeMaxX = 1, sizeMinY = 1, sizeMaxY = 1, sizeIncX = 0, sizeIncY = 0;
    float sizeWiggleX = 0, sizeWiggleY = 0;
    float speedMin = 0, speedMax = 0, speedInc = 0, speedWiggle = 0, gravity = 0, gravityDir = 270;
    float dirMin = 0, dirMax = 0, dirInc = 0, dirWiggle = 0;
    float angMin = 0, angMax = 0, angInc = 0, angWiggle = 0;
    bool angRelative = false;
    int32_t spawnOnDeath = -1, spawnOnDeathCount = 0, spawnOnUpdate = -1, spawnOnUpdateCount = 0;
};
// PSYS (2023.2+): a particle system asset, its emitters indexing PSEM.
struct PsSystem {
    std::string_view name;
    int32_t originX = 0, originY = 0, drawOrder = 0;
    bool globalSpace = false;
    std::vector<int32_t> emitters;
};

struct Sequence {
    std::string_view name;
    uint32_t playback = 0;  // 0 one-shot, 1 loop, 2 ping-pong
    float speed = 60;
    uint32_t speedType = 0;  // 0 frames per second, 1 frames per game frame
    float length = 0;
    int32_t originX = 0, originY = 0;
    float volume = 1;
    std::vector<SeqKeyframe> messages, moments;
    std::vector<SeqTrack> tracks;
    std::vector<std::pair<int32_t, std::string_view>> functions;
};

struct CodeLocals {
    std::string_view codeName;
    std::vector<std::pair<uint32_t, std::string_view>> locals;
};

class DataWin {
public:
    static std::unique_ptr<DataWin> load(const std::string& path);
    static std::unique_ptr<DataWin> fromBuffer(std::vector<uint8_t> buffer);
    ~DataWin();
    DataWin(const DataWin&) = delete;
    DataWin& operator=(const DataWin&) = delete;

    const Reader& reader() const { return r_; }
    // False when the file is from a GameMaker version the runner can't run; only the header,
    // strings and chunk list are loaded then, and unsupportedReason() says why.
    bool supported() const { return unsupported_.empty(); }
    const std::string& unsupportedReason() const { return unsupported_; }
    std::string engineVersion() const;
    bool hasChunk(const char* name) const { return chunks_.count(name) != 0; }
    struct ChunkInfo { uint32_t offset, size; };
    const std::vector<std::pair<std::string, ChunkInfo>>& chunkList() const { return chunkOrder_; }

    GeneralInfo gen;
    Options options;
    std::vector<std::string_view> strings;
    std::vector<Texture> textures;
    std::vector<TexturePageItem> tpag;
    // TAGS (GMS 2.3+): tag names per asset, keyed by (asset type << 24) | index.
    std::vector<std::pair<int32_t, std::vector<std::string_view>>> assetTags;
    std::vector<AnimCurve> animCurves;
    std::vector<Sequence> sequences;
    std::vector<SkeletonSprite> skeletons;
    std::vector<PsEmitter> psEmitters;
    std::vector<PsSystem> psSystems;
    std::vector<AnimCurve> seqCurves;  // curves embedded in sequence keyframes
    std::vector<Sprite> sprites;
    std::vector<Background> backgrounds;
    std::vector<Font> fonts;
    std::vector<Sound> sounds;
    std::vector<AudioBlob> audio;
    std::vector<std::string_view> audioGroups;
    std::vector<Path> paths;
    std::vector<Script> scripts;
    std::vector<int32_t> globalInit;
    std::vector<GameObject> objects;
    std::vector<Room> rooms;
    std::vector<Timeline> timelines;
    std::vector<Shader> shaders;
    std::vector<CodeEntry> code;
    std::vector<Variable> variables;
    std::vector<Function> functions;
    std::vector<CodeLocals> codeLocals;
    struct ExtensionFunction {
        std::string_view name;
        uint32_t returnType = 2;  // 1 string, 2 real
    };
    std::vector<ExtensionFunction> extensionFunctions;  // native (DLL) functions declared by extensions
    // FEDS (2022.1+): filter and effect definitions, name -> JSON (parameters, defaults, ...).
    std::vector<std::pair<std::string_view, std::string_view>> effectDefs;
    uint32_t varCount1 = 0, varCount2 = 0, maxLocalVars = 0;

    // Bytecode reference resolution: address of a Push/Pop/Call instruction -> VARI/FUNC index.
    std::unordered_map<uint32_t, int32_t> varRefs;
    std::unordered_map<uint32_t, int32_t> funcRefs;

    int32_t tpagIndexAt(uint32_t offset) const;
    int32_t codeIndex(std::string_view name) const;

private:
    void parse();
    void parseGEN8(uint32_t o, uint32_t len);
    void parseSTRG(uint32_t o);
    void parseOPTN(uint32_t o);
    void parseTXTR(uint32_t o, uint32_t len);
    void parseTPAG(uint32_t o);
    void parseSPRT(uint32_t o);
    void parseBGND(uint32_t o);
    void parseFONT(uint32_t o);
    void parseSOND(uint32_t o);
    void parseAUDO(uint32_t o);
    void parseAGRP(uint32_t o);
    void parsePATH(uint32_t o);
    void parseSCPT(uint32_t o);
    void parseGLOB(uint32_t o);
    void parseOBJT(uint32_t o);
    void parseROOM(uint32_t o);
    void parseTMLN(uint32_t o);
    void parseSHDR(uint32_t o, uint32_t len);
    void parseCODE(uint32_t o, uint32_t len);
    void parseVARI(uint32_t o, uint32_t len);
    void parseFUNC(uint32_t o, uint32_t len);
    void parseTAGS(uint32_t o);
    void parseACRV(uint32_t o, uint32_t len);
    void parseSEQN(uint32_t o, uint32_t len);
    void parsePSEM(uint32_t o, uint32_t len);
    bool parseSkeleton(uint32_t q, struct Sprite& s);
    void parseTGIN(uint32_t o);
    void parsePSYS(uint32_t o);
    void resolveReferences();

    uint32_t entrySize(uint32_t listOffset) const;  // spacing of a pointer list's first entries
    void parseTileLayer(RoomLayer& L, uint32_t q);
    void parseEXTN(uint32_t o);
    void parseFEDS(uint32_t o);

    template <typename F> void pointerList(uint32_t o, F&& f) const {
        uint32_t n = r_.u32(o);
        for (uint32_t i = 0; i < n; i++) f(i, r_.u32(o + 4 + i * 4));
    }

    DataWin() = default;
    std::vector<uint8_t> buf_;
    // Or the file mapped read-only (POSIX): the system can drop its pages under memory pressure
    // and read them back, where a copy would stay resident.
    void* mapped_ = nullptr;
    size_t mappedSize_ = 0;
    Reader r_;
    std::string unsupported_;
    std::unordered_map<std::string, ChunkInfo> chunks_;
    std::vector<std::pair<std::string, ChunkInfo>> chunkOrder_;
    std::unordered_map<uint32_t, int32_t> tpagByOffset_;
    std::unordered_map<std::string_view, int32_t> codeByName_;
};

}  // namespace gm
