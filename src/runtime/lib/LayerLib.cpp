// GMS2 room layers and their elements: layers, backgrounds, sprite elements, tile maps and the
// tile data helpers (behaviour as YoYo's runner, Function_Layers).
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <cstdio>

#include "runtime/Builtins.h"
#include "runtime/Graphics.h"

namespace gm {
namespace {

Layer* layerArg(Runner& rt, Value* args, int argc, int i) { return rt.layerFromValue(argv(args, argc, i)); }

LayerElement* elementOf(Runner& rt, Value* args, int argc, int i, ElementType type) {
    LayerElement* e = rt.elementById(argI(args, argc, i));
    return e && e->type == type ? e : nullptr;
}

const LayerElement* firstOfType(Runner& rt, const Layer& L, ElementType type) {
    for (int32_t id : L.elements)
        if (const LayerElement* e = rt.elementById(id); e && e->type == type) return e;
    return nullptr;
}

Value idList(const std::vector<int32_t>& ids) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    for (int32_t id : ids) a->rows[0].push_back(Value(static_cast<double>(id)));
    return arr;
}

constexpr uint32_t kTileIndexMask = 0x7FFFF, kTileMirror = 1u << 28, kTileFlip = 1u << 29, kTileRotate = 1u << 30;

}  // namespace

// ---- layers

GMFN(layer_get_id) {
    Layer* L = rt.layerByName(S(0));
    return Value(L ? static_cast<double>(L->id) : -1.0);
}
GMFN(layer_exists) { return boolV(layerArg(rt, args, argc, 0) != nullptr); }
GMFN(layer_create) {
    std::string name = argc > 1 && !ARG(1).isUndefined() ? S(1) : std::string();
    Layer& L = rt.createLayer(D(0), name, false);
    if (L.name.empty()) {
        char buf[32];
        snprintf(buf, sizeof buf, "_layer_%x", static_cast<unsigned>(L.id));
        L.name = buf;
    }
    return Value(static_cast<double>(L.id));
}
GMFN(layer_destroy) {
    if (Layer* L = layerArg(rt, args, argc, 0)) rt.destroyLayer(L->id);
    return Value();
}
GMFN(layer_get_all) {
    std::vector<const Layer*> sorted;
    for (auto& L : rt.layers)
        if (!L.dynamic) sorted.push_back(&L);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Layer* a, const Layer* b) { return a->depth < b->depth; });
    std::vector<int32_t> ids;
    for (const Layer* L : sorted) ids.push_back(L->id);
    return idList(ids);
}
GMFN(layer_get_name) {
    Layer* L = layerArg(rt, args, argc, 0);
    return Value(L ? L->name : std::string());
}
GMFN(layer_get_depth) { Layer* L = layerArg(rt, args, argc, 0); return Value(L ? L->depth : -1.0); }
GMFN(layer_depth) {
    if (Layer* L = layerArg(rt, args, argc, 0)) {
        L->depth = std::trunc(D(1));
        L->order = rt.layerOrder++;  // re-inserted into the sorted list
        for (int32_t id : L->elements)
            if (const LayerElement* e = rt.elementById(id); e && e->type == ElementType::Instance)
                if (Instance* inst = rt.findById(e->instance)) inst->depth = L->depth;
    }
    return Value();
}
GMFN(layer_set_visible) { if (Layer* L = layerArg(rt, args, argc, 0)) L->visible = B(1); return Value(); }
GMFN(layer_get_visible) { Layer* L = layerArg(rt, args, argc, 0); return boolV(L && L->visible); }
// layer_script_begin/end(layer, script): a function or script index; -1 removes it.
static Value layerScriptValue(Runner& rt, const Value& v) {
    if (v.isNumeric()) {
        int i = v.toInt();
        if (i >= 100000) i -= 100000;
        return i < 0 || static_cast<size_t>(i) >= rt.dw.scripts.size() ? Value() : rt.vm.scriptValue(i);
    }
    return v.isMethod() ? v : Value();
}
GMFN(layer_script_begin) { if (Layer* L = layerArg(rt, args, argc, 0)) L->scriptBegin = layerScriptValue(rt, ARG(1)); return Value(); }
GMFN(layer_script_end) { if (Layer* L = layerArg(rt, args, argc, 0)) L->scriptEnd = layerScriptValue(rt, ARG(1)); return Value(); }
GMFN(layer_get_script_begin) {
    Layer* L = layerArg(rt, args, argc, 0);
    return L && !L->scriptBegin.isUndefined() ? L->scriptBegin : Value(-1.0);
}
GMFN(layer_get_script_end) {
    Layer* L = layerArg(rt, args, argc, 0);
    return L && !L->scriptEnd.isUndefined() ? L->scriptEnd : Value(-1.0);
}
GMFN(layer_shader) { if (Layer* L = layerArg(rt, args, argc, 0)) L->shader = I(1); return Value(); }
GMFN(layer_get_shader) { Layer* L = layerArg(rt, args, argc, 0); return Value(L ? static_cast<double>(L->shader) : -1.0); }
// ---- effects (filters). fx structs hold the parameters as fields; the filter name is kept on
// the side. Built-in filters are not drawn yet.
static std::unordered_map<const GMObject*, std::string> g_fxNames;
static std::unordered_map<const GMObject*, bool> g_fxSingle;
static bool isFx(const Value& v) { return v.obj() && g_fxNames.count(v.obj()); }
static Value newFx(Runner& rt, const std::string& name) {
    Value fx = rt.vm.newStruct(nullptr);
    g_fxNames[fx.obj()] = name;
    return fx;
}
// "#AARRGGBB" colours become [r, g, b, a] in 0..1, numbers numbers, anything else stays text.
static Value fxParamValue(std::string_view text) {
    std::string t(text);
    if (t.size() == 9 && t[0] == '#') {
        uint32_t c = static_cast<uint32_t>(std::strtoul(t.c_str() + 1, nullptr, 16));
        Value arr = Value::newArray();
        GMArray* a = arr.writableArray();
        a->rows.resize(1);
        a->rows[0] = {Value(((c >> 16) & 0xFF) / 255.0), Value(((c >> 8) & 0xFF) / 255.0), Value((c & 0xFF) / 255.0),
                      Value(((c >> 24) & 0xFF) / 255.0)};
        return arr;
    }
    char* end = nullptr;
    double d = std::strtod(t.c_str(), &end);
    if (!t.empty() && end && *end == 0) return Value(d);
    return Value(t);
}
// A room layer's effect: an effect layer filters everything below it; an effect set on another
// kind of layer only that layer. Before 2022.1 effect layers were named "filter_x" / "effect_x".
Value roomLayerFx(Runner& rt, const RoomLayer& L) {
    std::string name(L.fxType);
    if (name.empty() && (L.name.rfind("filter_", 0) == 0 || L.name.rfind("effect_", 0) == 0)) name = "_" + std::string(L.name);
    if (name.empty()) return Value();
    Value fx = newFx(rt, name);
    for (auto& [pname, value] : L.fxParams) rt.vm.setInstanceVar(fx.obj()->inst, rt.vm.varIdForName(pname), fxParamValue(value));
    g_fxSingle[fx.obj()] = L.type != LayerType::Effect && !L.fxType.empty();
    return fx;
}
bool fxInfo(const Value& fx, std::string& name, bool& singleLayer) {
    if (!isFx(fx)) return false;
    name = g_fxNames[fx.obj()];
    auto it = g_fxSingle.find(fx.obj());
    singleLayer = it != g_fxSingle.end() && it->second;
    return true;
}
GMFN(fx_create) { return newFx(rt, S(0)); }
GMFN(fx_get_name) { return isFx(ARG(0)) ? Value(g_fxNames[ARG(0).obj()]) : Value(); }
GMFN(fx_get_parameter_names) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    if (isFx(ARG(0)))
        ARG(0).obj()->inst->vars.forEachInOrder([&](int32_t id, const Value&) { a->rows[0].push_back(Value(std::string(rt.vm.varName(id)))); });
    return arr;
}
GMFN(fx_get_parameter) {
    return isFx(ARG(0)) ? rt.vm.getInstanceVar(ARG(0).obj()->inst, rt.vm.varIdForName(S(1))) : Value();
}
GMFN(fx_get_parameters) {
    Value copy = rt.vm.newStruct(nullptr);
    if (isFx(ARG(0)))
        ARG(0).obj()->inst->vars.forEachInOrder([&](int32_t id, const Value& v) { rt.vm.setInstanceVar(copy.obj()->inst, id, v); });
    return copy;
}
GMFN(fx_set_parameter) {  // (fx, name, value, [more values...]): several values make an array
    if (!isFx(ARG(0))) return Value(-1.0);
    Value v = ARG(2);
    if (argc > 3) {
        v = Value::newArray();
        GMArray* a = v.writableArray();
        a->rows.resize(1);
        for (int i = 2; i < argc; i++) a->rows[0].push_back(args[i]);
    }
    rt.vm.setInstanceVar(ARG(0).obj()->inst, rt.vm.varIdForName(S(1)), v);
    return Value();
}
GMFN(fx_set_parameters) {
    if (!isFx(ARG(0)) || !ARG(1).obj()) return Value();
    Instance* dst = ARG(0).obj()->inst;
    ARG(1).obj()->inst->vars.forEachInOrder([&](int32_t id, const Value& v) { rt.vm.setInstanceVar(dst, id, v); });
    return Value();
}
GMFN(fx_set_single_layer) { if (isFx(ARG(0))) g_fxSingle[ARG(0).obj()] = B(1); return Value(); }
GMFN(fx_get_single_layer) { return boolV(isFx(ARG(0)) && g_fxSingle[ARG(0).obj()]); }
GMFN(layer_set_fx) { if (Layer* L = layerArg(rt, args, argc, 0)) L->fx = isFx(ARG(1)) ? ARG(1) : Value(); return Value(); }
GMFN(layer_get_fx) {
    Layer* L = layerArg(rt, args, argc, 0);
    return L && !L->fx.isUndefined() ? L->fx : Value(-1.0);
}
GMFN(layer_clear_fx) { if (Layer* L = layerArg(rt, args, argc, 0)) L->fx = Value(); return Value(); }
GMFN(layer_enable_fx) { if (Layer* L = layerArg(rt, args, argc, 0)) L->fxEnabled = B(1); return Value(); }
GMFN(layer_fx_is_enabled) { Layer* L = layerArg(rt, args, argc, 0); return boolV(L && L->fxEnabled); }
GMFN(layer_x) { if (Layer* L = layerArg(rt, args, argc, 0)) L->x = D(1); return Value(); }
GMFN(layer_y) { if (Layer* L = layerArg(rt, args, argc, 0)) L->y = D(1); return Value(); }
GMFN(layer_get_x) { Layer* L = layerArg(rt, args, argc, 0); return Value(L ? L->x : 0.0); }
GMFN(layer_get_y) { Layer* L = layerArg(rt, args, argc, 0); return Value(L ? L->y : 0.0); }
GMFN(layer_hspeed) { if (Layer* L = layerArg(rt, args, argc, 0)) L->hspeed = D(1); return Value(); }
GMFN(layer_vspeed) { if (Layer* L = layerArg(rt, args, argc, 0)) L->vspeed = D(1); return Value(); }
GMFN(layer_get_hspeed) { Layer* L = layerArg(rt, args, argc, 0); return Value(L ? L->hspeed : 0.0); }
GMFN(layer_get_vspeed) { Layer* L = layerArg(rt, args, argc, 0); return Value(L ? L->vspeed : 0.0); }
// Legacy tiles on asset layers are kept with the room's tiles; they are elements of their layer
// too (layerelementtype_tile), after its other elements.
static const Tile* legacyTile(Runner& rt, int32_t id) {
    for (const Tile& t : rt.tiles)
        if (t.id == id && t.layer >= 0) return &t;
    return nullptr;
}
GMFN(layer_get_all_elements) {
    Layer* L = layerArg(rt, args, argc, 0);
    if (!L) return idList({});
    std::vector<int32_t> ids = L->elements;
    for (const Tile& t : rt.tiles)
        if (t.layer == L->id) ids.push_back(t.id);
    return idList(ids);
}
GMFN(layer_get_element_type) {
    if (LayerElement* e = rt.elementById(I(0))) return Value(static_cast<double>(static_cast<int32_t>(e->type)));
    return Value(legacyTile(rt, I(0)) ? static_cast<double>(ElementType::Tile) : -1.0);
}
GMFN(layer_get_element_layer) {
    if (LayerElement* e = rt.elementById(I(0))) return Value(static_cast<double>(e->layer));
    const Tile* t = legacyTile(rt, I(0));
    return Value(t ? static_cast<double>(t->layer) : -1.0);
}
GMFN(layer_instance_get_instance) {
    LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Instance);
    return Value(e ? static_cast<double>(e->instance) : -4.0);
}
GMFN(layer_has_instance) {  // (layer, instance id or object)
    Layer* L = layerArg(rt, args, argc, 0);
    if (!L) return boolV(false);
    int32_t target = I(1);
    for (int32_t id : L->elements) {
        const LayerElement* e = rt.elementById(id);
        if (!e || e->type != ElementType::Instance) continue;
        Instance* inst = rt.findById(e->instance);
        if (!inst || !Runner::live(inst)) continue;
        if (inst->id == target || rt.objectIs(inst->object, target)) return boolV(true);
    }
    return boolV(false);
}
// Forced draw depth only changes the z used for 3D depth testing, which this runner doesn't use.
static bool g_depthForced = false;
static double g_forcedDepth = 0;
GMFN(layer_force_draw_depth) {
    g_depthForced = B(0);
    g_forcedDepth = D(1);
    return Value();
}
GMFN(layer_is_draw_depth_forced) { return boolV(g_depthForced); }
GMFN(layer_get_forced_depth) { return Value(g_forcedDepth); }

// ---- backgrounds

GMFN(layer_background_get_id) {
    Layer* L = layerArg(rt, args, argc, 0);
    const LayerElement* e = L ? firstOfType(rt, *L, ElementType::Background) : nullptr;
    return Value(e ? static_cast<double>(e->id) : -1.0);
}
GMFN(layer_background_exists) {  // (layer, element)
    LayerElement* e = elementOf(rt, args, argc, 1, ElementType::Background);
    Layer* L = layerArg(rt, args, argc, 0);
    return boolV(e && L && e->layer == L->id);
}
GMFN(layer_background_create) {  // (layer, sprite)
    Layer* L = layerArg(rt, args, argc, 0);
    if (!L) return Value(-1.0);
    LayerElement e;
    e.type = ElementType::Background;
    e.sprite = I(1);
    e.blend = 0xFFFFFF;
    e.alpha = 1;
    e.speed = 1;
    // Runtime backgrounds animate at the sprite's own playback speed.
    if (e.sprite >= 0 && static_cast<size_t>(e.sprite) < rt.dw.sprites.size()) {
        e.bgPlayback = rt.dw.sprites[e.sprite].playbackSpeed;
        e.bgPlaybackType = rt.dw.sprites[e.sprite].playbackSpeedType;
    }
    return Value(static_cast<double>(rt.addLayerElement(*L, std::move(e))));
}
GMFN(layer_background_destroy) {
    if (elementOf(rt, args, argc, 0, ElementType::Background)) rt.removeLayerElement(I(0), false);
    return Value();
}
#define BG_SET(fn, stmt)                                                                     \
    GMFN(fn) {                                                                               \
        if (LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Background)) { stmt; } \
        return Value();                                                                      \
    }
#define BG_GET(fn, expr, def)                                                    \
    GMFN(fn) {                                                                   \
        LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Background); \
        return e ? Value(expr) : Value(def);                                     \
    }
BG_SET(layer_background_visible, e->visible = B(1))
BG_SET(layer_background_htiled, e->htiled = B(1))
BG_SET(layer_background_vtiled, e->vtiled = B(1))
BG_SET(layer_background_stretch, e->stretch = B(1))
BG_SET(layer_background_xscale, e->xscale = D(1))
BG_SET(layer_background_yscale, e->yscale = D(1))
BG_SET(layer_background_blend, e->blend = C(1))
BG_SET(layer_background_alpha, e->alpha = D(1))
BG_SET(layer_background_index, e->index = D(1))
BG_SET(layer_background_speed, e->speed = D(1))
BG_SET(layer_background_sprite, e->sprite = I(1))
BG_SET(layer_background_change, e->sprite = I(1))
BG_GET(layer_background_get_visible, Value::boolean(e->visible), Value::boolean(true))
BG_GET(layer_background_get_htiled, Value::boolean(e->htiled), Value::boolean(false))
BG_GET(layer_background_get_vtiled, Value::boolean(e->vtiled), Value::boolean(false))
BG_GET(layer_background_get_stretch, Value::boolean(e->stretch), Value::boolean(false))
BG_GET(layer_background_get_xscale, e->xscale, 1.0)
BG_GET(layer_background_get_yscale, e->yscale, 1.0)
BG_GET(layer_background_get_blend, static_cast<double>(e->blend), 0.0)
BG_GET(layer_background_get_alpha, e->alpha, 0.0)
BG_GET(layer_background_get_index, e->index, -1.0)
BG_GET(layer_background_get_speed, e->speed, 0.0)
BG_GET(layer_background_get_sprite, static_cast<double>(e->sprite), -1.0)

// ---- sprite elements

GMFN(layer_sprite_get_id) {  // (layer, name) - elements don't keep names here; first sprite
    Layer* L = layerArg(rt, args, argc, 0);
    const LayerElement* e = L ? firstOfType(rt, *L, ElementType::Sprite) : nullptr;
    return Value(e ? static_cast<double>(e->id) : -1.0);
}
GMFN(layer_sprite_exists) {
    LayerElement* e = elementOf(rt, args, argc, 1, ElementType::Sprite);
    Layer* L = layerArg(rt, args, argc, 0);
    return boolV(e && L && e->layer == L->id);
}
GMFN(layer_sprite_create) {  // (layer, x, y, sprite)
    Layer* L = layerArg(rt, args, argc, 0);
    if (!L) return Value(-1.0);
    LayerElement e;
    e.type = ElementType::Sprite;
    e.x = D(1);
    e.y = D(2);
    e.sprite = I(3);
    e.speed = 1;
    return Value(static_cast<double>(rt.addLayerElement(*L, std::move(e))));
}
GMFN(layer_sprite_destroy) {
    if (elementOf(rt, args, argc, 0, ElementType::Sprite)) rt.removeLayerElement(I(0), false);
    return Value();
}
#define SPR_SET(fn, stmt)                                                                 \
    GMFN(fn) {                                                                            \
        if (LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Sprite)) { stmt; } \
        return Value();                                                                   \
    }
#define SPR_GET(fn, expr, def)                                               \
    GMFN(fn) {                                                               \
        LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Sprite); \
        return e ? Value(expr) : Value(def);                                 \
    }
SPR_SET(layer_sprite_change, e->sprite = I(1))
SPR_SET(layer_sprite_index, e->index = D(1))
SPR_SET(layer_sprite_speed, e->speed = D(1))
SPR_SET(layer_sprite_xscale, e->xscale = D(1))
SPR_SET(layer_sprite_yscale, e->yscale = D(1))
SPR_SET(layer_sprite_angle, e->angle = D(1))
SPR_SET(layer_sprite_blend, e->blend = C(1))
SPR_SET(layer_sprite_alpha, e->alpha = D(1))
SPR_SET(layer_sprite_x, e->x = D(1))
SPR_SET(layer_sprite_y, e->y = D(1))
SPR_GET(layer_sprite_get_sprite, static_cast<double>(e->sprite), -1.0)
SPR_GET(layer_sprite_get_index, e->index, -1.0)
SPR_GET(layer_sprite_get_speed, e->speed, 0.0)
SPR_GET(layer_sprite_get_xscale, e->xscale, 1.0)
SPR_GET(layer_sprite_get_yscale, e->yscale, 1.0)
SPR_GET(layer_sprite_get_angle, e->angle, 0.0)
SPR_GET(layer_sprite_get_blend, static_cast<double>(e->blend), 0.0)
SPR_GET(layer_sprite_get_alpha, e->alpha, 0.0)
SPR_GET(layer_sprite_get_x, e->x, 0.0)
SPR_GET(layer_sprite_get_y, e->y, 0.0)

// ---- tile maps

namespace {
const Background* tilesetOf(Runner& rt, const LayerElement& e) {
    return e.tileset >= 0 && static_cast<size_t>(e.tileset) < rt.dw.backgrounds.size() ? &rt.dw.backgrounds[e.tileset]
                                                                                        : nullptr;
}
// Cell under a room position, or false when outside the map.
bool cellAt(Runner& rt, const LayerElement& e, double px, double py, int& cx, int& cy) {
    const Background* ts = tilesetOf(rt, e);
    Layer* L = rt.layerById(e.layer);
    if (!ts || !ts->tileWidth || !ts->tileHeight || !L) return false;
    double x = px - (e.x + L->x), y = py - (e.y + L->y);
    if (x < 0 || y < 0) return false;
    cx = static_cast<int>(x / ts->tileWidth);
    cy = static_cast<int>(y / ts->tileHeight);
    return cx < static_cast<int>(e.tilesX) && cy < static_cast<int>(e.tilesY);
}
}  // namespace

GMFN(layer_tilemap_get_id) {
    Layer* L = layerArg(rt, args, argc, 0);
    const LayerElement* e = L ? firstOfType(rt, *L, ElementType::Tilemap) : nullptr;
    return Value(e ? static_cast<double>(e->id) : -1.0);
}
GMFN(layer_tilemap_exists) {
    LayerElement* e = elementOf(rt, args, argc, 1, ElementType::Tilemap);
    Layer* L = layerArg(rt, args, argc, 0);
    return boolV(e && L && e->layer == L->id);
}
GMFN(layer_tilemap_create) {  // (layer, x, y, tileset, width, height)
    Layer* L = layerArg(rt, args, argc, 0);
    if (!L) return Value(-1.0);
    LayerElement e;
    e.type = ElementType::Tilemap;
    e.x = D(1);
    e.y = D(2);
    e.tileset = I(3);
    e.tilesX = static_cast<uint32_t>(std::max(0, I(4)));
    e.tilesY = static_cast<uint32_t>(std::max(0, I(5)));
    e.tiles.assign(static_cast<size_t>(e.tilesX) * e.tilesY, 0);
    return Value(static_cast<double>(rt.addLayerElement(*L, std::move(e))));
}
GMFN(layer_tilemap_destroy) {
    if (elementOf(rt, args, argc, 0, ElementType::Tilemap)) rt.removeLayerElement(I(0), false);
    return Value();
}
#define TM_SET(fn, stmt)                                                                   \
    GMFN(fn) {                                                                             \
        if (LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Tilemap)) { stmt; } \
        return Value();                                                                    \
    }
// Getters return -1 for a missing tile map (YoYo's runner).
#define TM_GET(fn, expr, def)                                                 \
    GMFN(fn) {                                                                \
        LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Tilemap); \
        return e ? Value(expr) : Value(def);                                  \
    }
TM_SET(tilemap_tileset, e->tileset = I(1))
TM_SET(tilemap_x, e->x = D(1))
TM_SET(tilemap_y, e->y = D(1))
TM_GET(tilemap_get_tileset, static_cast<double>(e->tileset), -1.0)
TM_GET(tilemap_get_x, e->x, -1.0)
TM_GET(tilemap_get_y, e->y, -1.0)
TM_GET(tilemap_get_width, static_cast<double>(e->tilesX), -1.0)
TM_GET(tilemap_get_height, static_cast<double>(e->tilesY), -1.0)
TM_GET(tilemap_get_tile_width, tilesetOf(rt, *e) ? static_cast<double>(tilesetOf(rt, *e)->tileWidth) : -1.0, -1.0)
TM_GET(tilemap_get_tile_height, tilesetOf(rt, *e) ? static_cast<double>(tilesetOf(rt, *e)->tileHeight) : -1.0, -1.0)
TM_GET(tilemap_get_frame, static_cast<double>(rt.gfx->tilesetFrame(e->tileset)), -1.0)
TM_GET(tilemap_get_mask, static_cast<double>(e->tileMask), -1.0)
TM_SET(tilemap_set_mask, e->tileMask = static_cast<uint32_t>(ARG(1).toInt64()))
GMFN(tilemap_get_global_mask) { return Value(static_cast<double>(rt.tileGlobalMask)); }
GMFN(tilemap_set_global_mask) { rt.tileGlobalMask = static_cast<uint32_t>(ARG(0).toInt64()); return Value(); }
GMFN(draw_tilemap) {  // (tilemap, x, y): the whole tile map at that position
    if (LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Tilemap)) rt.gfx->drawTilemapAt(*e, D(1), D(2));
    return Value();
}
GMFN(draw_tile) {  // (tileset, tiledata, frame, x, y)
    rt.gfx->drawTileData(I(0), static_cast<uint32_t>(ARG(1).toInt64()) & rt.tileGlobalMask, I(2), D(3), D(4), 0xFFFFFF,
                         rt.gfx->alpha);
    return Value();
}
GMFN(layer_get_id_at_depth) {  // every layer at that depth, or [-1]
    std::vector<int32_t> ids;
    int32_t depth = I(0);
    for (const Layer& L : rt.layers)
        if (static_cast<int32_t>(L.depth) == depth) ids.push_back(L.id);
    if (ids.empty()) ids.push_back(-1);
    return idList(ids);
}
GMFN(layer_element_move) {  // (element, layer): appended to the target layer
    LayerElement* e = rt.elementById(I(0));
    if (!e) return Value(-1.0);
    Layer* to = layerArg(rt, args, argc, 1);
    if (!to || to->id == e->layer) return Value();
    if (Layer* from = rt.layerById(e->layer))
        from->elements.erase(std::remove(from->elements.begin(), from->elements.end(), e->id), from->elements.end());
    e->layer = to->id;
    to->elements.push_back(e->id);
    if (e->type == ElementType::Instance)
        if (Instance* inst = rt.findById(e->instance)) {
            inst->layer = to->id;
            inst->depth = to->depth;
        }
    return Value();
}
static void resizeTilemap(LayerElement& e, uint32_t w, uint32_t h) {
    std::vector<uint32_t> t(static_cast<size_t>(w) * h, 0);
    for (uint32_t y = 0; y < std::min(h, e.tilesY); y++)
        for (uint32_t x = 0; x < std::min(w, e.tilesX); x++) t[static_cast<size_t>(y) * w + x] = e.tiles[static_cast<size_t>(y) * e.tilesX + x];
    e.tiles = std::move(t);
    e.tilesX = w;
    e.tilesY = h;
}
TM_SET(tilemap_set_width, resizeTilemap(*e, static_cast<uint32_t>(std::max(0, I(1))), e->tilesY))
TM_SET(tilemap_set_height, resizeTilemap(*e, e->tilesX, static_cast<uint32_t>(std::max(0, I(1)))))
TM_SET(tilemap_clear, std::fill(e->tiles.begin(), e->tiles.end(), static_cast<uint32_t>(I(1))))
GMFN(tilemap_get) {  // (tilemap, cell x, cell y)
    LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Tilemap);
    if (!e) return Value(-1.0);
    int x = I(1), y = I(2);
    if (x < 0 || y < 0 || x >= static_cast<int>(e->tilesX) || y >= static_cast<int>(e->tilesY)) return Value();
    return Value(static_cast<double>(e->tiles[static_cast<size_t>(y) * e->tilesX + x]));
}
GMFN(tilemap_set) {  // (tilemap, tile data, cell x, cell y)
    LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Tilemap);
    if (!e) return boolV(false);
    int x = I(2), y = I(3);
    if (x < 0 || y < 0 || x >= static_cast<int>(e->tilesX) || y >= static_cast<int>(e->tilesY)) return boolV(false);
    e->tiles[static_cast<size_t>(y) * e->tilesX + x] = static_cast<uint32_t>(ARG(1).toInt64());
    return boolV(true);
}
GMFN(tilemap_get_at_pixel) {
    LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Tilemap);
    int cx, cy;
    if (!e || !cellAt(rt, *e, D(1), D(2), cx, cy)) return Value(-1.0);
    return Value(static_cast<double>(e->tiles[static_cast<size_t>(cy) * e->tilesX + cx]));
}
GMFN(tilemap_set_at_pixel) {  // (tilemap, tile data, x, y)
    LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Tilemap);
    int cx, cy;
    if (!e || !cellAt(rt, *e, D(2), D(3), cx, cy)) return boolV(false);
    e->tiles[static_cast<size_t>(cy) * e->tilesX + cx] = static_cast<uint32_t>(ARG(1).toInt64());
    return boolV(true);
}
GMFN(tilemap_get_cell_x_at_pixel) {
    LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Tilemap);
    int cx, cy;
    return Value(e && cellAt(rt, *e, D(1), D(2), cx, cy) ? static_cast<double>(cx) : -1.0);
}
GMFN(tilemap_get_cell_y_at_pixel) {
    LayerElement* e = elementOf(rt, args, argc, 0, ElementType::Tilemap);
    int cx, cy;
    return Value(e && cellAt(rt, *e, D(1), D(2), cx, cy) ? static_cast<double>(cy) : -1.0);
}

// ---- tile data (index in bits 0-18, mirror 28, flip 29, rotate 30)

static uint32_t tileData(Value* args, int argc, int i) { return static_cast<uint32_t>(argv(args, argc, i).toInt64()); }
GMFN(tile_get_index) { return Value(static_cast<double>(tileData(args, argc, 0) & kTileIndexMask)); }
GMFN(tile_get_empty) { return boolV((tileData(args, argc, 0) & kTileIndexMask) == 0); }
GMFN(tile_get_mirror) { return boolV(tileData(args, argc, 0) & kTileMirror); }
GMFN(tile_get_flip) { return boolV(tileData(args, argc, 0) & kTileFlip); }
GMFN(tile_get_rotate) { return boolV(tileData(args, argc, 0) & kTileRotate); }
GMFN(tile_set_index) {
    uint32_t d = tileData(args, argc, 0);
    return Value(static_cast<double>((d & ~kTileIndexMask) | (static_cast<uint32_t>(I(1)) & kTileIndexMask)));
}
GMFN(tile_set_empty) { return Value(static_cast<double>(tileData(args, argc, 0) & ~kTileIndexMask)); }
static Value setBit(Value* args, int argc, uint32_t bit) {
    uint32_t d = tileData(args, argc, 0);
    return Value(static_cast<double>(argB(args, argc, 1) ? (d | bit) : (d & ~bit)));
}
GMFN(tile_set_mirror) { return setBit(args, argc, kTileMirror); }
GMFN(tile_set_flip) { return setBit(args, argc, kTileFlip); }
GMFN(tile_set_rotate) { return setBit(args, argc, kTileRotate); }

void registerLayerBuiltins(Runner& rt) {
    GMREG(layer_get_id); GMREG(layer_exists); GMREG(layer_create); GMREG(layer_destroy); GMREG(layer_get_all);
    GMREG(layer_get_name); GMREG(layer_get_depth); GMREG(layer_depth); GMREG(layer_set_visible);
    GMREG(layer_get_visible); GMREG(fx_create); GMREG(fx_get_name); GMREG(fx_get_parameter_names);
    GMREG(fx_get_parameter); GMREG(fx_get_parameters); GMREG(fx_set_parameter); GMREG(fx_set_parameters);
    GMREG(fx_set_single_layer); GMREG(fx_get_single_layer); GMREG(layer_set_fx); GMREG(layer_get_fx);
    GMREG(layer_clear_fx); GMREG(layer_enable_fx); GMREG(layer_fx_is_enabled); GMREG(layer_script_begin); GMREG(layer_script_end); GMREG(layer_get_script_begin);
    GMREG(layer_get_script_end); GMREG(layer_shader); GMREG(layer_get_shader); GMREG(layer_x); GMREG(layer_y); GMREG(layer_get_x); GMREG(layer_get_y);
    GMREG(layer_hspeed); GMREG(layer_vspeed); GMREG(layer_get_hspeed); GMREG(layer_get_vspeed);
    GMREG(layer_get_all_elements); GMREG(layer_get_element_type); GMREG(layer_get_element_layer);
    GMREG(layer_instance_get_instance); GMREG(layer_has_instance); GMREG(layer_force_draw_depth);
    GMREG(layer_is_draw_depth_forced); GMREG(layer_get_forced_depth);
    GMREG(layer_background_get_id); GMREG(layer_background_exists); GMREG(layer_background_create);
    GMREG(layer_background_destroy); GMREG(layer_background_visible); GMREG(layer_background_htiled);
    GMREG(layer_background_vtiled); GMREG(layer_background_stretch); GMREG(layer_background_xscale);
    GMREG(layer_background_yscale); GMREG(layer_background_blend); GMREG(layer_background_alpha);
    GMREG(layer_background_index); GMREG(layer_background_speed); GMREG(layer_background_sprite);
    GMREG(layer_background_change); GMREG(layer_background_get_visible); GMREG(layer_background_get_htiled);
    GMREG(layer_background_get_vtiled); GMREG(layer_background_get_stretch); GMREG(layer_background_get_xscale);
    GMREG(layer_background_get_yscale); GMREG(layer_background_get_blend); GMREG(layer_background_get_alpha);
    GMREG(layer_background_get_index); GMREG(layer_background_get_speed); GMREG(layer_background_get_sprite);
    GMREG(layer_sprite_get_id); GMREG(layer_sprite_exists); GMREG(layer_sprite_create); GMREG(layer_sprite_destroy);
    GMREG(layer_sprite_change); GMREG(layer_sprite_index); GMREG(layer_sprite_speed); GMREG(layer_sprite_xscale);
    GMREG(layer_sprite_yscale); GMREG(layer_sprite_angle); GMREG(layer_sprite_blend); GMREG(layer_sprite_alpha);
    GMREG(layer_sprite_x); GMREG(layer_sprite_y); GMREG(layer_sprite_get_sprite); GMREG(layer_sprite_get_index);
    GMREG(layer_sprite_get_speed); GMREG(layer_sprite_get_xscale); GMREG(layer_sprite_get_yscale);
    GMREG(layer_sprite_get_angle); GMREG(layer_sprite_get_blend); GMREG(layer_sprite_get_alpha);
    GMREG(layer_sprite_get_x); GMREG(layer_sprite_get_y);
    GMREG(layer_tilemap_get_id); GMREG(layer_tilemap_exists); GMREG(layer_tilemap_create); GMREG(layer_tilemap_destroy);
    GMREG(tilemap_tileset); GMREG(tilemap_x); GMREG(tilemap_y); GMREG(tilemap_get_tileset); GMREG(tilemap_get_x);
    GMREG(tilemap_get_y); GMREG(tilemap_get_width); GMREG(tilemap_get_height); GMREG(tilemap_get_tile_width);
    GMREG(tilemap_get_tile_height); GMREG(tilemap_set_width); GMREG(tilemap_set_height); GMREG(tilemap_clear);
    GMREG(tilemap_get); GMREG(tilemap_set); GMREG(tilemap_get_at_pixel); GMREG(tilemap_set_at_pixel);
    GMREG(tilemap_get_cell_x_at_pixel); GMREG(tilemap_get_cell_y_at_pixel);
    GMREG(tile_get_index); GMREG(tile_get_empty); GMREG(tile_get_mirror); GMREG(tile_get_flip);
    GMREG(tile_get_rotate); GMREG(tile_set_index); GMREG(tile_set_empty); GMREG(tile_set_mirror);
    GMREG(tile_set_flip); GMREG(tile_set_rotate);
    GMREG(tilemap_get_frame); GMREG(tilemap_get_mask); GMREG(tilemap_set_mask); GMREG(tilemap_get_global_mask);
    GMREG(tilemap_set_global_mask); GMREG(draw_tilemap); GMREG(draw_tile); GMREG(layer_get_id_at_depth);
    GMREG(layer_element_move);
}

}  // namespace gm
