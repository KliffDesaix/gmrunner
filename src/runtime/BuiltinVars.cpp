#include "BuiltinVars.h"

#include <cmath>
#include <ctime>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "Graphics.h"
#include "Runner.h"

namespace gm {

static const char* kNames[] = {
#define GM_NAME(n) #n,
    GM_BUILTIN_VARS(GM_NAME)
#undef GM_NAME
};

int16_t builtinVarId(std::string_view name) {
    static std::unordered_map<std::string_view, int16_t> map = [] {
        std::unordered_map<std::string_view, int16_t> m;
        for (int16_t i = 0; i < BV_COUNT; i++) m[kNames[i]] = i;
        return m;
    }();
    auto it = map.find(name);
    return it == map.end() ? -1 : it->second;
}

const char* builtinVarName(int16_t id) { return id >= 0 && id < BV_COUNT ? kNames[id] : "?"; }

static int slot(int64_t index, int size) {
    if (index < 0) return 0;
    return index < size ? static_cast<int>(index) : -1;
}

static Value bboxValue(Runner& rt, Instance* self, int which) {
    Runner::BBox b = rt.bbox(self);
    if (!b.valid) return Value(which == 0 || which == 1 ? self->x : self->y);
    switch (which) {
        case 0: return Value(static_cast<double>(std::lround(b.left)));
        case 1: return Value(static_cast<double>(std::lround(b.right) - 1));
        case 2: return Value(static_cast<double>(std::lround(b.top)));
        default: return Value(static_cast<double>(std::lround(b.bottom) - 1));
    }
}

Value getBuiltinVar(Runner& rt, Instance* self, int16_t id, int64_t index, Value* args, int argc) {
    const DataWin& dw = rt.dw;
    const Sprite* spr = self->sprite >= 0 && static_cast<size_t>(self->sprite) < dw.sprites.size()
                            ? &dw.sprites[self->sprite] : nullptr;
    int vi = slot(index, 8);
    switch (id) {
        case BV_x: return Value(self->x);
        case BV_y: return Value(self->y);
        case BV_xprevious: return Value(self->xprevious);
        case BV_yprevious: return Value(self->yprevious);
        case BV_xstart: return Value(self->xstart);
        case BV_ystart: return Value(self->ystart);
        case BV_hspeed: return Value(self->hspeed);
        case BV_vspeed: return Value(self->vspeed);
        case BV_speed: return Value(self->speed);
        case BV_direction: return Value(self->direction);
        case BV_friction: return Value(self->friction);
        case BV_gravity: return Value(self->gravity);
        case BV_gravity_direction: return Value(self->gravityDirection);
        case BV_sprite_index: return Value(static_cast<double>(self->sprite));
        case BV_image_index: return Value(self->imageIndex);
        case BV_image_speed: return Value(self->imageSpeed);
        case BV_image_xscale: return Value(self->imageXscale);
        case BV_image_yscale: return Value(self->imageYscale);
        case BV_image_angle: return Value(self->imageAngle);
        case BV_image_alpha: return Value(self->imageAlpha);
        case BV_image_blend: return Value(static_cast<double>(self->imageBlend));
        case BV_image_number: return Value(static_cast<double>(rt.imageNumber(self)));
        case BV_image_single: return Value(self->imageSpeed != 0 ? -1.0 : self->imageIndex);
        case BV_mask_index: return Value(static_cast<double>(self->mask));
        case BV_depth: return Value(self->depth);
        case BV_visible: return Value::boolean(self->visible);
        case BV_solid: return Value::boolean(self->solid);
        case BV_persistent: return Value::boolean(self->persistent);
        case BV_id: return Value(static_cast<double>(self->id));
        case BV_object_index: return Value(static_cast<double>(self->object));
        case BV_alarm: {
            int a = slot(index, 12);
            return Value(a < 0 ? -1.0 : self->alarm[a]);
        }
        case BV_bbox_left: return bboxValue(rt, self, 0);
        case BV_bbox_right: return bboxValue(rt, self, 1);
        case BV_bbox_top: return bboxValue(rt, self, 2);
        case BV_bbox_bottom: return bboxValue(rt, self, 3);
        case BV_sprite_width: return Value(spr ? spr->width * self->imageXscale : 0.0);
        case BV_sprite_height: return Value(spr ? spr->height * self->imageYscale : 0.0);
        case BV_sprite_xoffset: return Value(spr ? spr->originX * self->imageXscale : 0.0);
        case BV_sprite_yoffset: return Value(spr ? spr->originY * self->imageYscale : 0.0);
        case BV_path_index: return Value(static_cast<double>(self->pathIndex));
        case BV_path_position: return Value(self->pathPosition);
        case BV_path_positionprevious: return Value(self->pathPositionPrevious);
        case BV_path_speed: return Value(self->pathSpeed);
        case BV_path_scale: return Value(self->pathScale);
        case BV_path_orientation: return Value(self->pathOrientation);
        case BV_path_endaction: return Value(static_cast<double>(self->pathEndAction));
        case BV_timeline_index: return Value(static_cast<double>(self->timelineIndex));
        case BV_timeline_position: return Value(self->timelinePosition);
        case BV_timeline_speed: return Value(self->timelineSpeed);
        case BV_timeline_running: return Value::boolean(self->timelineRunning);
        case BV_timeline_loop: return Value::boolean(self->timelineLoop);

        case BV_argument: {
            if (index < 0) index = 0;
            return index < argc ? args[index] : Value();
        }
        case BV_argument_count: return Value(static_cast<double>(argc));
        case BV_argument0: case BV_argument1: case BV_argument2: case BV_argument3: case BV_argument4:
        case BV_argument5: case BV_argument6: case BV_argument7: case BV_argument8: case BV_argument9:
        case BV_argument10: case BV_argument11: case BV_argument12: case BV_argument13: case BV_argument14:
        case BV_argument15: {
            int n = id - BV_argument0;
            if (n >= argc) return Value();
            // argumentN[i]: an element of the array passed as argument N
            if (index >= 0 && args[n].arr()) return args[n].arr()->get(index);
            return args[n];
        }

        case BV_room: return Value(static_cast<double>(rt.room));
        case BV_room_speed: return Value(rt.roomSpeed);
        case BV_room_width: return Value(rt.roomWidth);
        case BV_room_height: return Value(rt.roomHeight);
        case BV_room_caption: return Value(rt.roomCaption);
        case BV_room_persistent: return Value::boolean(rt.roomPersistent);
        case BV_room_first: return Value(dw.gen.roomOrder.empty() ? -1.0 : dw.gen.roomOrder.front());
        case BV_room_last: return Value(dw.gen.roomOrder.empty() ? -1.0 : dw.gen.roomOrder.back());
        case BV_fps: return Value(rt.fps);
        case BV_fps_real: return Value(rt.fpsReal);
        case BV_current_time: return Value(std::floor(rt.currentTimeMs()));
        case BV_instance_count: {
            int n = 0;
            for (Instance* i : rt.instances) n += Runner::live(i);
            return Value(static_cast<double>(n));
        }
        case BV_instance_id: {
            int64_t n = 0;
            for (Instance* i : rt.instances)
                if (Runner::live(i) && n++ == index) return Value(static_cast<double>(i->id));
            return Value(-4.0);
        }
        case BV_mouse_x: case BV_mouse_y: {
            double rx, ry;
            rt.gfx->windowToRoom(rt.input.mouseX, rt.input.mouseY, rx, ry);
            return Value(id == BV_mouse_x ? rx : ry);
        }
        case BV_mouse_button: {
            for (int b = 0; b < 3; b++)
                if (rt.input.mouseDown[b]) return Value(static_cast<double>(b + 1));
            return Value(0.0);
        }
        case BV_mouse_lastbutton: return Value(0.0);
        case BV_keyboard_key: {
            for (int k = 2; k < Input::kKeys; k++)
                if (rt.input.down[k]) return Value(static_cast<double>(k));
            return Value(0.0);
        }
        case BV_keyboard_lastkey: return Value(static_cast<double>(rt.input.lastKey));
        case BV_keyboard_lastchar: return Value(rt.input.lastChar);
        case BV_keyboard_string: return Value(rt.input.keyboardString);
        case BV_background_color: case BV_background_colour: return Value(static_cast<double>(rt.bgColor));
        case BV_background_showcolor: case BV_background_showcolour: return Value::boolean(rt.bgShowColor);
        case BV_view_enabled: return Value::boolean(rt.viewsEnabled);
        case BV_view_current: return Value(static_cast<double>(rt.viewCurrent));
        case BV_application_surface: return Value(static_cast<double>(rt.gfx->applicationSurface()));
        case BV_working_directory: return Value(rt.saveDir + "/");
        case BV_program_directory: return Value(rt.gameDir + "/");
        case BV_temp_directory: return Value(rt.saveDir + "/temp/");
        case BV_game_save_id: return Value(rt.saveDir + "/");
        case BV_game_id: return Value(static_cast<double>(dw.gen.gameId));
        case BV_game_display_name: return Value(rt.displayName);
        case BV_async_load: return Value(static_cast<double>(rt.asyncLoad));
        case BV_score: return Value(rt.score);
        case BV_lives: return Value(rt.lives);
        case BV_health: return Value(rt.health);
        case BV_delta_time: return Value(1000000.0 / (rt.roomSpeed > 0 ? rt.roomSpeed : 60));
        case BV_os_type: return Value(0.0);  // os_windows: the game was built for it
        case BV_os_device: return Value(0.0);
        case BV_debug_mode: return Value::boolean(false);
        case BV_cursor_sprite: return Value(-1.0);
        case BV_error_occurred: return Value::boolean(false);
        case BV_error_last: return Value("");
        case BV_show_score: case BV_show_lives: case BV_show_health: case BV_secure_mode: return Value::boolean(false);
        case BV_undefined: return Value();
        case BV_pointer_null: return Value::ptr(nullptr);
        case BV_audio_bus_main: return audioMainBus(rt);
        case BV_event_type: return Value(static_cast<double>(rt.ctx.type));
        case BV_event_number: return Value(static_cast<double>(rt.ctx.subtype));
        case BV_event_object: return Value(static_cast<double>(rt.ctx.object));
        case BV_pointer_invalid: return Value::ptr(reinterpret_cast<void*>(-1));
        case BV_infinity: return Value(std::numeric_limits<double>::infinity());
        case BV_NaN: return Value(std::numeric_limits<double>::quiet_NaN());
        case BV_layer: return Value(static_cast<double>(self->layer));
        case BV_in_sequence: return Value::boolean(self->inSequence);
        case BV_event_data: return Value(static_cast<double>(rt.eventData));
        case BV_sequence_instance: return sequenceInstanceStruct(rt, self->seqElement);
        case BV_drawn_by_sequence: return Value::boolean(self->drawnBySequence);
        case BV_view_camera: {
            int v = slot(index, 8);
            return Value(v < 0 ? -1.0 : static_cast<double>(rt.views[v].camera));
        }
        case BV_view_surface_id: {
            int v = slot(index, 8);
            return Value(v < 0 ? -1.0 : static_cast<double>(rt.views[v].surface));
        }
        case BV_current_year: case BV_current_month: case BV_current_day: case BV_current_weekday:
        case BV_current_hour: case BV_current_minute: case BV_current_second: {
            std::time_t t = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            switch (id) {
                case BV_current_year: return Value(static_cast<double>(tm.tm_year + 1900));
                case BV_current_month: return Value(static_cast<double>(tm.tm_mon + 1));
                case BV_current_day: return Value(static_cast<double>(tm.tm_mday));
                case BV_current_weekday: return Value(static_cast<double>(tm.tm_wday));
                case BV_current_hour: return Value(static_cast<double>(tm.tm_hour));
                case BV_current_minute: return Value(static_cast<double>(tm.tm_min));
                default: return Value(static_cast<double>(tm.tm_sec));
            }
        }
        case BV_browser_width: return Value(static_cast<double>(rt.platform.displayWidth()));
        case BV_browser_height: return Value(static_cast<double>(rt.platform.displayHeight()));
        default: break;
    }

    if (id >= BV_background_visible && id <= BV_background_height) {
        if (vi < 0) return Value(0.0);
        const RoomBackgroundState& b = rt.backgrounds[vi];
        switch (id) {
            case BV_background_visible: return Value::boolean(b.visible);
            case BV_background_foreground: return Value::boolean(b.foreground);
            case BV_background_index: return Value(static_cast<double>(b.index));
            case BV_background_x: return Value(b.x);
            case BV_background_y: return Value(b.y);
            case BV_background_hspeed: return Value(b.hspeed);
            case BV_background_vspeed: return Value(b.vspeed);
            case BV_background_htiled: return Value::boolean(b.htiled);
            case BV_background_vtiled: return Value::boolean(b.vtiled);
            case BV_background_xscale: return Value(b.xscale);
            case BV_background_yscale: return Value(b.yscale);
            case BV_background_alpha: return Value(b.alpha);
            case BV_background_blend: return Value(static_cast<double>(b.blend));
            case BV_background_width: case BV_background_height: {
                int w = 0, h = 0;
                rt.gfx->backgroundSize(b.index, w, h);
                return Value(static_cast<double>(id == BV_background_width ? w : h));
            }
        }
    }
    if (id >= BV_view_visible && id <= BV_view_object) {
        if (vi < 0) return Value(0.0);
        const View& v = rt.views[vi];
        if (const Camera* c = rt.cameraForView(vi)) {  // GMS2: view rect lives in the camera
            switch (id) {
                case BV_view_xview: return Value(c->x);
                case BV_view_yview: return Value(c->y);
                case BV_view_wview: return Value(c->w);
                case BV_view_hview: return Value(c->h);
                case BV_view_angle: return Value(c->angle);
                case BV_view_object: return Value(static_cast<double>(c->object));
                default: break;
            }
        }
        switch (id) {
            case BV_view_visible: return Value::boolean(v.visible);
            case BV_view_xview: return Value(v.xview);
            case BV_view_yview: return Value(v.yview);
            case BV_view_wview: return Value(v.wview);
            case BV_view_hview: return Value(v.hview);
            case BV_view_xport: return Value(v.xport);
            case BV_view_yport: return Value(v.yport);
            case BV_view_wport: return Value(v.wport);
            case BV_view_hport: return Value(v.hport);
            case BV_view_angle: return Value(v.angle);
            case BV_view_hborder: return Value(v.hborder);
            case BV_view_vborder: return Value(v.vborder);
            case BV_view_hspeed: return Value(v.hspeed);
            case BV_view_vspeed: return Value(v.vspeed);
            case BV_view_object: return Value(static_cast<double>(v.object));
        }
    }
    rt.vm.warnOnce(std::string("bvget:") + builtinVarName(id),
                   std::string("read of unsupported built-in ") + builtinVarName(id));
    return Value(0.0);
}

void setBuiltinVar(Runner& rt, Instance* self, int16_t id, int64_t index, Value v, Value* args, int argc) {
    double d = v.num();
    static const int32_t traced = [] {
        const char* e = getenv("GMR_TRACE_ID");
        return e ? atoi(e) : -1;
    }();
    if (self->id == traced && (id == BV_visible || id == BV_sprite_index || id == BV_mask_index || id == BV_image_alpha))
        rt.log("[trace] %d.%s = %s in %s (step %llu)", self->id, builtinVarName(id), v.repr().c_str(),
               rt.vm.callStack().c_str(), static_cast<unsigned long long>(rt.stepCount));
    int vi = slot(index, 8);
    switch (id) {
        case BV_x: self->x = d; return;
        case BV_y: self->y = d; return;
        case BV_xprevious: self->xprevious = d; return;
        case BV_yprevious: self->yprevious = d; return;
        case BV_xstart: self->xstart = d; return;
        case BV_ystart: self->ystart = d; return;
        case BV_hspeed: self->setHVSpeed(d, self->vspeed); return;
        case BV_vspeed: self->setHVSpeed(self->hspeed, d); return;
        case BV_speed: self->setSpeedDir(d, self->direction); return;
        case BV_direction: self->setSpeedDir(self->speed, d); return;
        case BV_friction: self->friction = d; return;
        case BV_gravity: self->gravity = d; return;
        case BV_gravity_direction: self->gravityDirection = d; return;
        case BV_sprite_index: self->sprite = v.toInt(); return;
        case BV_image_index: self->imageIndex = d; return;
        case BV_image_speed: self->imageSpeed = d; return;
        case BV_image_xscale: self->imageXscale = d; return;
        case BV_image_yscale: self->imageYscale = d; return;
        case BV_image_angle: self->imageAngle = d; return;
        case BV_image_alpha: self->imageAlpha = d; return;
        case BV_image_blend: self->imageBlend = static_cast<uint32_t>(v.toInt64()) & 0xFFFFFF; return;
        case BV_image_single:
            if (d < 0) self->imageSpeed = 1;
            else {
                self->imageIndex = d;
                self->imageSpeed = 0;
            }
            return;
        case BV_mask_index: self->mask = v.toInt(); return;
        case BV_depth:
            // GMS2 moves the instance off its layer to one at that depth; it is drawn by depth.
            if (rt.dw.gen.gms2 && self->layerElement >= 0 && d != self->depth) rt.removeInstanceFromLayer(self);
            self->depth = d;
            return;
        case BV_visible: self->visible = v.truthy(); return;
        case BV_solid: self->solid = v.truthy(); return;
        case BV_persistent: self->persistent = v.truthy(); return;
        case BV_alarm: {
            int a = slot(index, 12);
            if (a >= 0) self->alarm[a] = std::floor(d);
            return;
        }
        case BV_path_position: self->pathPosition = d; return;
        case BV_path_speed: self->pathSpeed = d; return;
        case BV_path_scale: self->pathScale = d; return;
        case BV_path_orientation: self->pathOrientation = d; return;
        case BV_path_endaction: self->pathEndAction = v.toInt(); return;
        case BV_timeline_index: self->timelineIndex = v.toInt(); return;
        case BV_timeline_position: self->timelinePosition = d; return;
        case BV_timeline_speed: self->timelineSpeed = d; return;
        case BV_timeline_running: self->timelineRunning = v.truthy(); return;
        case BV_timeline_loop: self->timelineLoop = v.truthy(); return;

        case BV_argument:
            if (index < 0) index = 0;
            if (index < argc) args[index] = std::move(v);
            return;
        case BV_argument0: case BV_argument1: case BV_argument2: case BV_argument3: case BV_argument4:
        case BV_argument5: case BV_argument6: case BV_argument7: case BV_argument8: case BV_argument9:
        case BV_argument10: case BV_argument11: case BV_argument12: case BV_argument13: case BV_argument14:
        case BV_argument15: {
            int n = id - BV_argument0;
            if (n >= argc) return;
            if (index >= 0) {  // argumentN[i] = v writes into the passed array (shared, as in GMS 2.3)
                if (!args[n].isArray()) args[n] = Value::newArray();
                args[n].arr()->set(index, std::move(v));
            } else {
                args[n] = std::move(v);
            }
            return;
        }

        case BV_room: rt.gotoRoom(v.toInt()); return;
        case BV_room_speed:
            rt.roomSpeed = d;
            if (rt.dw.gen.gms2) rt.gameSpeed = d;
            return;
        case BV_view_camera: {
            int v = slot(index, 8);
            if (v >= 0) rt.views[v].camera = v < 0 ? -1 : static_cast<int32_t>(d);
            return;
        }
        case BV_view_surface_id: {
            int v = slot(index, 8);
            if (v >= 0) rt.views[v].surface = static_cast<int32_t>(d);
            return;
        }
        case BV_layer: {
            if (Layer* L = rt.layerById(static_cast<int32_t>(d))) rt.addInstanceToLayer(self, *L);
            return;
        }
        case BV_drawn_by_sequence: self->drawnBySequence = v.truthy(); return;
        case BV_in_sequence: case BV_sequence_instance: case BV_event_data: return;
        case BV_room_width: rt.roomWidth = d; return;
        case BV_room_height: rt.roomHeight = d; return;
        case BV_room_caption:
            rt.roomCaption = v.toString();
            rt.platform.setWindowCaption(rt.roomCaption);
            return;
        case BV_room_persistent: rt.roomPersistent = v.truthy(); return;
        case BV_keyboard_lastkey: rt.input.lastKey = v.toInt(); return;
        case BV_keyboard_lastchar: rt.input.lastChar = v.toString(); return;
        case BV_keyboard_string: rt.input.keyboardString = v.toString(); return;
        case BV_background_color: case BV_background_colour: rt.bgColor = static_cast<uint32_t>(v.toInt64()) & 0xFFFFFF; return;
        case BV_background_showcolor: case BV_background_showcolour: rt.bgShowColor = v.truthy(); return;
        case BV_view_enabled: rt.viewsEnabled = v.truthy(); return;
        case BV_view_current: return;
        case BV_score: rt.score = d; return;
        case BV_lives: rt.lives = d; return;
        case BV_health: rt.health = d; return;
        case BV_cursor_sprite: return;
        default: break;
    }
    if (id >= BV_background_visible && id <= BV_background_blend) {
        if (vi < 0) return;
        RoomBackgroundState& b = rt.backgrounds[vi];
        switch (id) {
            case BV_background_visible: b.visible = v.truthy(); return;
            case BV_background_foreground: b.foreground = v.truthy(); return;
            case BV_background_index: b.index = v.toInt(); return;
            case BV_background_x: b.x = d; return;
            case BV_background_y: b.y = d; return;
            case BV_background_hspeed: b.hspeed = d; return;
            case BV_background_vspeed: b.vspeed = d; return;
            case BV_background_htiled: b.htiled = v.truthy(); return;
            case BV_background_vtiled: b.vtiled = v.truthy(); return;
            case BV_background_xscale: b.xscale = d; return;
            case BV_background_yscale: b.yscale = d; return;
            case BV_background_alpha: b.alpha = d; return;
            case BV_background_blend: b.blend = static_cast<uint32_t>(v.toInt64()) & 0xFFFFFF; return;
        }
    }
    if (id >= BV_view_visible && id <= BV_view_object) {
        if (vi < 0) return;
        View& w = rt.views[vi];
        if (Camera* c = rt.cameraForView(vi)) {
            switch (id) {
                case BV_view_xview: c->x = d; c->viewSet = false; return;
                case BV_view_yview: c->y = d; c->viewSet = false; return;
                case BV_view_wview: c->w = d; return;
                case BV_view_hview: c->h = d; return;
                case BV_view_angle: c->angle = d; return;
                case BV_view_object: c->object = v.toInt(); return;
                default: break;
            }
        }
        switch (id) {
            case BV_view_visible: w.visible = v.truthy(); return;
            case BV_view_xview: w.xview = d; return;
            case BV_view_yview: w.yview = d; return;
            case BV_view_wview: w.wview = d; return;
            case BV_view_hview: w.hview = d; return;
            case BV_view_xport: w.xport = d; return;
            case BV_view_yport: w.yport = d; return;
            case BV_view_wport: w.wport = d; return;
            case BV_view_hport: w.hport = d; return;
            case BV_view_angle: w.angle = d; return;
            case BV_view_hborder: w.hborder = d; return;
            case BV_view_vborder: w.vborder = d; return;
            case BV_view_hspeed: w.hspeed = d; return;
            case BV_view_vspeed: w.vspeed = d; return;
            case BV_view_object: w.object = v.toInt(); return;
        }
    }
    rt.vm.warnOnce(std::string("bvset:") + builtinVarName(id),
                   std::string("write to read-only/unsupported built-in ") + builtinVarName(id));
}

}  // namespace gm
