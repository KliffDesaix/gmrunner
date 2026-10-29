// audio_*, legacy sound_*, keyboard_*, mouse_*, gamepad_* and joystick_*.
#include <algorithm>
#include <cmath>

#include "runtime/Audio.h"
#include "runtime/Builtins.h"
#include "runtime/Stores.h"

namespace gm {

#define A (*rt.audio)

// ---- audio
// Optional trailing (gain, offset, pitch[, listener_mask]) arguments from `first` on.
static Audio::PlayParams playExtras(Value* args, int argc, int first) {
    Audio::PlayParams p;
    if (argc > first && !args[first].isUndefined()) p.gain = argNum(args[first]);
    if (argc > first + 1 && !args[first + 1].isUndefined()) p.offset = argNum(args[first + 1]);
    if (argc > first + 2 && !args[first + 2].isUndefined()) p.pitch = argNum(args[first + 2]);
    return p;
}
GMFN(audio_play_sound) {  // (sound, priority, loop, [gain], [offset], [pitch], [listener_mask])
    return Value(static_cast<double>(A.play(I(0), D(1), B(2), playExtras(args, argc, 3))));
}
GMFN(audio_play_sound_at) {  // (sound, x, y, z, falloff_ref, falloff_max, falloff_factor, loop, priority, ...)
    Audio::PlayParams p = playExtras(args, argc, 9);
    p.positional = true;
    p.x = D(1), p.y = D(2), p.z = D(3), p.ref = D(4), p.max = D(5), p.factor = D(6);
    return Value(static_cast<double>(A.play(I(0), D(8), B(7), p)));
}
GMFN(audio_play_sound_on) {  // (emitter, sound, loop, priority, [gain], [offset], [pitch], [listener_mask])
    if (!A.emitter(I(0))) return Value(-1.0);
    Audio::PlayParams p = playExtras(args, argc, 4);
    p.emitter = I(0);
    return Value(static_cast<double>(A.play(I(1), D(3), B(2), p)));
}
static Value member(Runner& rt, const Value& st, const char* name) {
    GMObject* o = st.obj();
    return o ? rt.vm.getInstanceVar(o->inst, rt.vm.varIdForName(name)) : Value();
}
GMFN(audio_play_sound_ext) {  // ({sound, priority, loop, gain, offset, pitch, emitter, position{...}})
    const Value& st = ARG(0);
    Audio::PlayParams p;
    auto num = [&](const char* n, double def) {
        Value v = member(rt, st, n);
        return v.isUndefined() ? def : v.num();
    };
    p.gain = std::max(0.0, num("gain", 1));
    p.offset = std::max(0.0, num("offset", 0));
    p.pitch = num("pitch", 1);
    Value em = member(rt, st, "emitter");
    if (!em.isUndefined()) p.emitter = em.toInt();
    Value pos = member(rt, st, "position");
    if (pos.obj() && p.emitter < 0) {
        p.positional = true;
        auto pn = [&](const char* n, double def) {
            Value v = member(rt, pos, n);
            return v.isUndefined() ? def : v.num();
        };
        p.x = pn("x", 0), p.y = pn("y", 0), p.z = pn("z", 0);
        p.ref = pn("falloff_ref", 0), p.max = pn("falloff_max", 1), p.factor = pn("falloff_factor", 1);
    }
    Value loop = member(rt, st, "loop");
    return Value(static_cast<double>(A.play(member(rt, st, "sound").toInt(), num("priority", 0), loop.truthy(), p)));
}

// ---- emitters and the listener
#define EMITTER_SET(name, body) \
    GMFN(name) {                \
        if (Audio::Emitter* e = A.emitter(I(0))) { body; } \
        return Value();         \
    }
GMFN(audio_emitter_create) { return Value(static_cast<double>(A.emitterCreate())); }
GMFN(audio_emitter_free) { A.emitterFree(I(0)); return Value(); }
GMFN(audio_emitter_exists) { return boolV(A.emitter(I(0)) != nullptr); }
EMITTER_SET(audio_emitter_position, e->x = D(1); e->y = D(2); e->z = D(3))
EMITTER_SET(audio_emitter_velocity, e->vx = D(1); e->vy = D(2); e->vz = D(3))
EMITTER_SET(audio_emitter_falloff, e->ref = D(1); e->max = D(2); e->factor = D(3))
EMITTER_SET(audio_emitter_gain, e->gain = std::max(0.0, D(1)))
EMITTER_SET(audio_emitter_pitch, e->pitch = std::max(0.01, D(1)))
EMITTER_SET(audio_emitter_set_listener_mask, e->listenerMask = static_cast<uint32_t>(I(1)))
#define EMITTER_GET(name, expr) \
    GMFN(name) {                \
        Audio::Emitter* e = A.emitter(I(0)); \
        return Value(e ? static_cast<double>(expr) : 0.0); \
    }
EMITTER_GET(audio_emitter_get_gain, e->gain)
EMITTER_GET(audio_emitter_get_pitch, e->pitch)
EMITTER_GET(audio_emitter_get_x, e->x)
EMITTER_GET(audio_emitter_get_y, e->y)
EMITTER_GET(audio_emitter_get_z, e->z)
EMITTER_GET(audio_emitter_get_vx, e->vx)
EMITTER_GET(audio_emitter_get_vy, e->vy)
EMITTER_GET(audio_emitter_get_vz, e->vz)
EMITTER_GET(audio_emitter_get_listener_mask, e->listenerMask)
GMFN(audio_listener_position) { A.listener.x = D(0); A.listener.y = D(1); A.listener.z = D(2); return Value(); }
GMFN(audio_listener_velocity) { A.listener.vx = D(0); A.listener.vy = D(1); A.listener.vz = D(2); return Value(); }
GMFN(audio_listener_orientation) {
    auto& l = A.listener;
    l.atX = D(0), l.atY = D(1), l.atZ = D(2), l.upX = D(3), l.upY = D(4), l.upZ = D(5);
    return Value();
}
// Multi-listener forms (index first); one listener is mixed.
GMFN(audio_listener_set_position) { A.listener.x = D(1); A.listener.y = D(2); A.listener.z = D(3); return Value(); }
GMFN(audio_listener_set_velocity) { A.listener.vx = D(1); A.listener.vy = D(2); A.listener.vz = D(3); return Value(); }
GMFN(audio_listener_set_orientation) {
    auto& l = A.listener;
    l.atX = D(1), l.atY = D(2), l.atZ = D(3), l.upX = D(4), l.upY = D(5), l.upZ = D(6);
    return Value();
}
GMFN(audio_get_listener_count) { return Value(1.0); }
GMFN(audio_falloff_set_model) { A.falloffModel = I(0); return Value(); }

// ---- groups, loop points, sync groups
GMFN(audio_group_set_gain) { A.setGroupGain(I(0), D(1), D(2)); return Value(); }
GMFN(audio_group_get_gain) { return Value(A.groupGain(I(0))); }
GMFN(audio_group_stop_all) { A.stopGroup(I(0)); return Value(); }
GMFN(audio_group_load_progress) { return Value(100.0); }
// Sounds are always playable here, but loading still answers as YoYo's runners do: a group
// (other than the default, 0) becoming loaded sends an Async Save/Load event with
// type "audiogroup_load"; loading it again does nothing and returns false.
static std::vector<bool> gGroupLoaded;
GMFN(audio_group_load) {
    int g = I(0);
    if (g <= 0 || static_cast<size_t>(g) >= rt.dw.audioGroups.size()) return boolV(false);
    if (gGroupLoaded.size() <= static_cast<size_t>(g)) gGroupLoaded.resize(g + 1, false);
    if (gGroupLoaded[g]) return boolV(false);
    gGroupLoaded[g] = true;
    rt.queueAsync(72, {{"type", Value("audiogroup_load")}, {"group_id", Value(static_cast<double>(g))}});
    return boolV(true);
}
GMFN(audio_group_unload) {
    int g = I(0);
    if (g <= 0 || static_cast<size_t>(g) >= gGroupLoaded.size() || !gGroupLoaded[g]) return boolV(false);
    gGroupLoaded[g] = false;
    return boolV(true);
}
GMFN(audio_sound_loop) { A.setLoop(I(0), B(1)); return Value(); }
GMFN(audio_sound_get_loop) { return boolV(A.getLoop(I(0))); }
GMFN(audio_sound_loop_start) { A.setLoopPoint(I(0), false, D(1)); return Value(); }
GMFN(audio_sound_loop_end) { A.setLoopPoint(I(0), true, D(1)); return Value(); }
GMFN(audio_sound_get_loop_start) { return Value(A.getLoopPoint(I(0), false)); }
GMFN(audio_sound_get_loop_end) { return Value(A.getLoopPoint(I(0), true)); }
GMFN(audio_create_sync_group) { return Value(static_cast<double>(A.syncCreate(B(0)))); }
GMFN(audio_play_in_sync_group) { A.syncAdd(I(0), I(1)); return Value(); }
GMFN(audio_start_sync_group) { A.syncStart(I(0)); return Value(); }
GMFN(audio_stop_sync_group) { A.syncStop(I(0)); return Value(); }
GMFN(audio_pause_sync_group) { A.syncPause(I(0), true); return Value(); }
GMFN(audio_resume_sync_group) { A.syncPause(I(0), false); return Value(); }
GMFN(audio_sync_group_is_playing) { return boolV(A.syncPlaying(I(0))); }
GMFN(audio_sync_group_get_track_pos) { return Value(A.syncPosition(I(0))); }
GMFN(audio_destroy_sync_group) { A.syncDestroy(I(0)); return Value(); }

// ---- buses and effects (GameMaker 2023). A bus is a struct with `bypass`, `gain` and 8 effect
// slots; voices take the gain of their emitter's bus (or the main bus). Effects are kept but not
// processed.
static std::vector<Value> g_buses;  // [0] = audio_bus_main
static Value makeBus(Runner& rt) {
    Value b = rt.vm.newStruct(nullptr);
    Instance* bi = b.obj()->inst;
    rt.vm.setInstanceVar(bi, rt.vm.varIdForName("bypass"), Value::boolean(false));
    rt.vm.setInstanceVar(bi, rt.vm.varIdForName("gain"), Value(1.0));
    Value fx = Value::newArray();
    GMArray* a = fx.writableArray();
    a->rows.resize(1);
    a->rows[0].assign(8, Value());
    rt.vm.setInstanceVar(bi, rt.vm.varIdForName("effects"), fx);
    return b;
}
static void ensureBuses(Runner& rt) {
    if (!g_buses.empty()) return;
    g_buses.push_back(makeBus(rt));
    Runner* r = &rt;
    A.beforeUpdate = [r] {  // bus gains for the mixer, read from the structs each step
        std::vector<double> gains;
        for (const Value& b : g_buses) {
            Value g = r->vm.getInstanceVar(b.obj()->inst, r->vm.varIdForName("gain"));
            gains.push_back(g.isUndefined() ? 1.0 : std::max(0.0, g.num()));
        }
        r->audio->busGains = gains;
    };
}
Value audioMainBus(Runner& rt) {
    ensureBuses(rt);
    return g_buses[0];
}
GMFN(audio_bus_create) {
    ensureBuses(rt);
    g_buses.push_back(makeBus(rt));
    return g_buses.back();
}
GMFN(audio_emitter_bus) {  // (emitter, bus)
    ensureBuses(rt);
    Audio::Emitter* e = A.emitter(I(0));
    if (!e) return Value();
    e->bus = -1;
    for (size_t i = 0; i < g_buses.size(); i++)
        if (g_buses[i].obj() == ARG(1).obj()) e->bus = static_cast<int>(i);
    return Value();
}
GMFN(audio_emitter_get_bus) {
    ensureBuses(rt);
    Audio::Emitter* e = A.emitter(I(0));
    return g_buses[e && e->bus > 0 && static_cast<size_t>(e->bus) < g_buses.size() ? e->bus : 0];
}
GMFN(audio_effect_create) {  // (type, [params struct]): kept as a struct with its settings
    Value fx = rt.vm.newStruct(nullptr);
    Instance* fi = fx.obj()->inst;
    rt.vm.setInstanceVar(fi, rt.vm.varIdForName("type"), Value(D(0)));
    rt.vm.setInstanceVar(fi, rt.vm.varIdForName("bypass"), Value::boolean(false));
    if (GMObject* params = ARG(1).obj())
        params->inst->vars.forEachInOrder([&](int32_t id, const Value& v) { rt.vm.setInstanceVar(fi, id, v); });
    return fx;
}
GMFN(audio_bus_get_emitters) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    return arr;
}
GMFN(audio_bus_clear_emitters) { return Value(); }
GMFN(audio_stop_sound) { A.stop(I(0)); return Value(); }
GMFN(audio_stop_all) { A.stopAll(); return Value(); }
GMFN(audio_is_playing) { return boolV(A.isPlaying(I(0))); }
GMFN(audio_is_paused) { return boolV(A.isPaused(I(0))); }
GMFN(audio_exists) { return boolV(A.exists(I(0))); }
// audio_sound_is_playable(sound): whether the sound can be played now (its audio group loaded).
// Groups load on demand here, so any existing sound can.
GMFN(audio_sound_is_playable) { return boolV(A.exists(I(0))); }
GMFN(audio_pause_sound) { A.pause(I(0), true); return Value(); }
GMFN(audio_resume_sound) { A.pause(I(0), false); return Value(); }
GMFN(audio_pause_all) { A.pauseAll(true); return Value(); }
GMFN(audio_resume_all) { A.pauseAll(false); return Value(); }
GMFN(audio_sound_gain) { A.setGain(I(0), D(1), D(2)); return Value(); }
GMFN(audio_sound_get_gain) { return Value(A.getGain(I(0))); }
GMFN(audio_sound_pitch) { A.setPitch(I(0), D(1)); return Value(); }
GMFN(audio_sound_get_pitch) { return Value(A.getPitch(I(0))); }
GMFN(audio_sound_get_track_position) { return Value(A.getPosition(I(0))); }
GMFN(audio_sound_set_track_position) { A.setPosition(I(0), D(1)); return Value(); }
GMFN(audio_sound_length) { return Value(A.soundLength(I(0))); }
GMFN(audio_master_gain) { A.setMasterGain(D(0)); return Value(); }
GMFN(audio_set_master_gain) { A.setMasterGain(D(1)); return Value(); }  // (listener index, gain)
GMFN(audio_get_master_gain) { return Value(A.masterGain()); }
GMFN(audio_create_stream) { return Value(static_cast<double>(A.createStream(rt.files->readPath(S(0))))); }
GMFN(audio_destroy_stream) { A.destroyStream(I(0)); return Value(); }
GMFN(audio_get_name) {
    int s = I(0);
    if (std::string n = A.streamName(s); !n.empty()) return Value(n);
    if (s < 0 || static_cast<size_t>(s) >= rt.dw.sounds.size()) return Value("<undefined>");
    return Value(std::string(rt.dw.sounds[s].name));
}
GMFN(sound_play) { A.play(I(0), 0, false); return Value(); }
GMFN(sound_loop) { A.play(I(0), 0, true); return Value(); }
GMFN(sound_stop) { A.stop(I(0)); return Value(); }
GMFN(sound_stop_all) { A.stopAll(); return Value(); }
GMFN(sound_isplaying) { return boolV(A.isPlaying(I(0))); }
GMFN(sound_volume) { A.setGain(I(0), D(1), 0); return Value(); }
GMFN(audio_noop) { return Value(); }
GMFN(audio_true) { return boolV(true); }

// ---- keyboard
// Key codes outside 0-255 (games store unbound controls as -4, noone) are never down, as in
// GameMaker; mapping them to 0 (vk_nokey) made them "pressed" whenever every key was released.
static bool validKey(int k) { return k >= 0 && k < Input::kKeys; }
GMFN(keyboard_check) { return boolV(validKey(I(0)) && rt.input.down[I(0)]); }
GMFN(keyboard_check_pressed) { return boolV(validKey(I(0)) && rt.input.pressed[I(0)]); }
GMFN(keyboard_check_released) { return boolV(validKey(I(0)) && rt.input.released[I(0)]); }
GMFN(keyboard_check_direct) {  // the keys as they are now; vk_anykey / vk_nokey: whether any key is down
    int k = I(0);
    if (k == VK_ANYKEY || k == VK_NOKEY) {
        bool any = std::any_of(rt.input.raw + 2, rt.input.raw + Input::kKeys, [](bool d) { return d; });
        return boolV(k == VK_ANYKEY ? any : !any);
    }
    return boolV(validKey(k) && rt.input.raw[k]);
}
GMFN(keyboard_clear) {
    if (int k = I(0); validKey(k)) rt.input.down[k] = rt.input.pressed[k] = rt.input.released[k] = false;
    return Value();
}
GMFN(keyboard_key_press) {
    if (validKey(I(0))) rt.input.keyEvent(I(0), true);
    return Value();
}
GMFN(keyboard_key_release) {
    if (validKey(I(0))) rt.input.keyEvent(I(0), false);
    return Value();
}
GMFN(keyboard_set_map) {
    int a = I(0), b = I(1);
    if (a > 1 && validKey(a) && validKey(b)) rt.input.map[a] = b;
    return Value();
}
GMFN(keyboard_get_map) { return Value(static_cast<double>(validKey(I(0)) ? rt.input.map[I(0)] : I(0))); }
GMFN(keyboard_unset_map) {
    for (int i = 0; i < Input::kKeys; i++) rt.input.map[i] = i;
    return Value();
}
GMFN(io_clear) { rt.input.clear(); return Value(); }

// ---- mouse (mb_left = 1, mb_right = 2, mb_middle = 3, mb_any = -1, mb_none = 0)
static bool mouseQuery(const bool* st, int b) {
    if (b == -1) return st[0] || st[1] || st[2];
    if (b == 0) return !(st[0] || st[1] || st[2]);
    int i = b == 1 ? 0 : (b == 2 ? 1 : (b == 3 ? 2 : -1));
    return i >= 0 && st[i];
}
GMFN(mouse_check_button) { return boolV(mouseQuery(rt.input.mouseDown, I(0))); }
GMFN(mouse_check_button_pressed) { return boolV(mouseQuery(rt.input.mousePressed, I(0))); }
GMFN(mouse_check_button_released) { return boolV(mouseQuery(rt.input.mouseReleased, I(0))); }
// mouse_clear(button): the button reads as up until it is pressed again.
GMFN(mouse_clear) {
    Input& in = rt.input;
    int b = I(0);
    for (int i = 0; i < 3; i++)
        if (b == -1 || b == i + 1) {
            in.mouseRaw[i] = in.mouseDown[i] = in.mousePressed[i] = in.mouseReleased[i] = false;
            in.mouseLatchPress[i] = in.mouseLatchRelease[i] = false;
        }
    return Value();
}

// ---- gamepads (gp_face1 = 32769 ... gp_padr = 32784, gp_axislh = 32785 ...)
static Input::Pad* pad(Runner& rt, int d) { return d >= 0 && d < Input::kPads ? &rt.input.pads[d] : nullptr; }
static int button(int b) {
    int i = b - kGpBase;
    return i >= 0 && i < GP_BUTTON_COUNT ? i : -1;
}
static float shoulderValue(const Input::Pad& p, int b) {
    // Analog triggers report through the axis-less button value in GameMaker.
    return p.down[b] ? 1.f : 0.f;
}
GMFN(gamepad_is_supported) { return boolV(true); }
GMFN(gamepad_get_device_count) { return Value(static_cast<double>(Input::kPads)); }
GMFN(gamepad_is_connected) { Input::Pad* p = pad(rt, I(0)); return boolV(p && p->connected); }
GMFN(gamepad_button_check) {
    Input::Pad* p = pad(rt, I(0));
    int b = button(I(1));
    return boolV(p && b >= 0 && p->down[b]);
}
GMFN(gamepad_button_check_pressed) {
    Input::Pad* p = pad(rt, I(0));
    int b = button(I(1));
    return boolV(p && b >= 0 && p->pressed[b]);
}
GMFN(gamepad_button_check_released) {
    Input::Pad* p = pad(rt, I(0));
    int b = button(I(1));
    return boolV(p && b >= 0 && p->released[b]);
}
GMFN(gamepad_button_value) {
    Input::Pad* p = pad(rt, I(0));
    int b = button(I(1));
    return Value(p && b >= 0 ? static_cast<double>(shoulderValue(*p, b)) : 0.0);
}
GMFN(gamepad_axis_value) {
    Input::Pad* p = pad(rt, I(0));
    int a = I(1) - kGpBase - GP_BUTTON_COUNT;
    if (!p || a < 0 || a >= GP_AXIS_COUNT) return Value(0.0);
    double v = p->axis[a], dz = p->deadzone;
    // Inside the dead zone: 0; outside it the rest of the range is stretched back to 0..1.
    if (dz > 0) v = std::fabs(v) < dz ? 0.0 : (std::fabs(v) - dz) / (1.0 - dz) * (v < 0 ? -1 : 1);
    return Value(v);
}
GMFN(gamepad_set_axis_deadzone) {
    if (Input::Pad* p = pad(rt, I(0))) p->deadzone = static_cast<float>(std::clamp(D(1), 0.0, 0.99));
    return Value();
}
GMFN(gamepad_get_axis_deadzone) {
    Input::Pad* p = pad(rt, I(0));
    return Value(p ? static_cast<double>(p->deadzone) : 0.0);
}
GMFN(gamepad_set_vibration) {
    rt.platform.padRumble(I(0), D(1), D(2));
    return Value();
}
GMFN(gamepad_set_colour) {  // GameMaker colours are 0xBBGGRR
    uint32_t c = static_cast<uint32_t>(I(1));
    rt.platform.padLight(I(0), ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF));
    return Value();
}
// The d-pad as one hat: a mask of up 1, right 2, down 4, left 8.
GMFN(gamepad_hat_count) {
    Input::Pad* p = pad(rt, I(0));
    return Value(p && p->connected ? 1.0 : 0.0);
}
GMFN(gamepad_hat_value) {
    Input::Pad* p = pad(rt, I(0));
    if (!p || !p->connected || I(1) != 0) return Value(0.0);
    return Value(static_cast<double>((p->down[GP_PADU] ? 1 : 0) | (p->down[GP_PADR] ? 2 : 0) |
                                     (p->down[GP_PADD] ? 4 : 0) | (p->down[GP_PADL] ? 8 : 0)));
}
GMFN(gamepad_set_button_threshold) { rt.input.buttonThreshold = static_cast<float>(D(1)); return Value(); }
GMFN(gamepad_get_description) {
    Input::Pad* p = pad(rt, I(0));
    return Value(p && p->connected ? (p->name.empty() ? "Controller" : p->name) : "");
}
GMFN(gamepad_get_guid) {
    Input::Pad* p = pad(rt, I(0));
    return Value(p && p->connected && !p->guid.empty() ? p->guid : "none");
}
GMFN(gamepad_get_mapping) {  // SDL's own mappings are used; there is no GameMaker mapping string
    Input::Pad* p = pad(rt, I(0));
    return Value(p && p->connected ? "no mapping" : "device not connected");
}
GMFN(gamepad_test_mapping) { return Value(); }    // (device, mapping): SDL maps the pad itself
GMFN(gamepad_remove_mapping) { return Value(); }
GMFN(gamepad_axis_count) { return Value(4.0); }
GMFN(gamepad_button_count) { return Value(16.0); }

// ---- legacy joystick_* (1-based ids and buttons)
GMFN(joystick_exists) { Input::Pad* p = pad(rt, I(0) - 1); return boolV(p && p->connected); }
GMFN(joystick_check_button) {
    Input::Pad* p = pad(rt, I(0) - 1);
    int b = I(1) - 1;
    return boolV(p && b >= 0 && b < GP_BUTTON_COUNT && p->down[b]);
}
GMFN(joystick_xpos) { Input::Pad* p = pad(rt, I(0) - 1); return Value(p ? static_cast<double>(p->axis[0]) : 0.0); }
GMFN(joystick_ypos) { Input::Pad* p = pad(rt, I(0) - 1); return Value(p ? static_cast<double>(p->axis[1]) : 0.0); }

void registerAudioBuiltins(Runner& rt) {
    GMREG(audio_play_sound); GMREG(audio_play_sound_at); GMREG(audio_play_sound_on); GMREG(audio_stop_sound);
    GMREG(audio_stop_all); GMREG(audio_is_playing); GMREG(audio_is_paused); GMREG(audio_exists); GMREG(audio_sound_is_playable);
    GMREG(audio_pause_sound); GMREG(audio_resume_sound); GMREG(audio_pause_all); GMREG(audio_resume_all);
    GMREG(audio_sound_gain); GMREG(audio_sound_get_gain); GMREG(audio_sound_pitch); GMREG(audio_sound_get_pitch);
    GMREG(audio_sound_get_track_position); GMREG(audio_sound_set_track_position); GMREG(audio_sound_length);
    GMREG(audio_master_gain); GMREG(audio_set_master_gain); GMREG(audio_get_master_gain); GMREG(audio_get_name);
    GMREG(sound_play); GMREG(sound_loop); GMREG(sound_stop); GMREG(sound_stop_all); GMREG(sound_isplaying);
    GMREG(sound_volume);
    GMREG(audio_play_sound_ext); GMREG(audio_create_stream); GMREG(audio_destroy_stream); GMREG(audio_emitter_create); GMREG(audio_emitter_free); GMREG(audio_emitter_exists);
    GMREG(audio_emitter_position); GMREG(audio_emitter_velocity); GMREG(audio_emitter_falloff);
    GMREG(audio_emitter_gain); GMREG(audio_emitter_pitch); GMREG(audio_emitter_set_listener_mask);
    GMREG(audio_emitter_get_gain); GMREG(audio_emitter_get_pitch); GMREG(audio_emitter_get_x);
    GMREG(audio_emitter_get_y); GMREG(audio_emitter_get_z); GMREG(audio_emitter_get_vx); GMREG(audio_emitter_get_vy);
    GMREG(audio_emitter_get_vz); GMREG(audio_emitter_get_listener_mask); GMREG(audio_listener_position);
    GMREG(audio_listener_velocity); GMREG(audio_listener_orientation); GMREG(audio_listener_set_position);
    GMREG(audio_listener_set_velocity); GMREG(audio_listener_set_orientation); GMREG(audio_get_listener_count);
    GMREG(audio_falloff_set_model); GMREG(audio_group_set_gain); GMREG(audio_group_get_gain);
    GMREG(audio_group_stop_all); GMREG(audio_group_load_progress); GMREG(audio_sound_loop); GMREG(audio_sound_get_loop);
    GMREG(audio_sound_loop_start); GMREG(audio_sound_loop_end); GMREG(audio_sound_get_loop_start);
    GMREG(audio_sound_get_loop_end); GMREG(audio_create_sync_group); GMREG(audio_play_in_sync_group);
    GMREG(audio_start_sync_group); GMREG(audio_stop_sync_group); GMREG(audio_pause_sync_group);
    GMREG(audio_resume_sync_group); GMREG(audio_sync_group_is_playing); GMREG(audio_sync_group_get_track_pos);
    GMREG(audio_destroy_sync_group); GMREG(audio_bus_create); GMREG(audio_emitter_bus); GMREG(audio_emitter_get_bus);
    GMREG(audio_effect_create); GMREG(audio_bus_get_emitters); GMREG(audio_bus_clear_emitters);
    gGroupLoaded.clear();
    GMREG(audio_group_load); GMREG(audio_group_unload);
    rt.vm.registerBuiltin("audio_channel_num", fn_audio_noop);
    for (const char* n : {"audio_group_is_loaded", "audio_system_is_available"}) rt.vm.registerBuiltin(n, fn_audio_true);
}

void registerInputBuiltins(Runner& rt) {
    GMREG(keyboard_check); GMREG(keyboard_check_pressed); GMREG(keyboard_check_released);
    GMREG(keyboard_check_direct); GMREG(keyboard_clear); GMREG(keyboard_key_press); GMREG(keyboard_key_release);
    GMREG(keyboard_set_map); GMREG(keyboard_get_map); GMREG(keyboard_unset_map); GMREG(io_clear);
    GMREG(mouse_check_button); GMREG(mouse_check_button_pressed); GMREG(mouse_check_button_released);
    GMREG(mouse_clear);
    GMREG(gamepad_is_supported); GMREG(gamepad_get_device_count); GMREG(gamepad_is_connected);
    GMREG(gamepad_button_check); GMREG(gamepad_button_check_pressed); GMREG(gamepad_button_check_released);
    GMREG(gamepad_button_value); GMREG(gamepad_axis_value); GMREG(gamepad_set_button_threshold);
    GMREG(gamepad_get_description); GMREG(gamepad_get_guid); GMREG(gamepad_get_mapping);
    GMREG(gamepad_test_mapping); GMREG(gamepad_remove_mapping); GMREG(gamepad_axis_count); GMREG(gamepad_button_count);
    GMREG(joystick_exists); GMREG(joystick_check_button); GMREG(joystick_xpos); GMREG(joystick_ypos);
    GMREG(gamepad_set_axis_deadzone); GMREG(gamepad_get_axis_deadzone); GMREG(gamepad_set_vibration);
    GMREG(gamepad_set_colour); GMALIAS(gamepad_set_color, gamepad_set_colour); GMREG(gamepad_hat_count);
    GMREG(gamepad_hat_value);
}

}  // namespace gm
