// GameMaker built-in variables (x, alarm[], view_xview[], room, ...).
#pragma once

#include <cstdint>
#include <string_view>

#include "vm/Value.h"

namespace gm {

class Runner;
struct Instance;

#define GM_BUILTIN_VARS(X)                                                                            \
    /* instance */                                                                                    \
    X(x) X(y) X(xprevious) X(yprevious) X(xstart) X(ystart) X(hspeed) X(vspeed) X(speed) X(direction) \
    X(friction) X(gravity) X(gravity_direction) X(sprite_index) X(image_index) X(image_speed)         \
    X(image_xscale) X(image_yscale) X(image_angle) X(image_alpha) X(image_blend) X(image_number)      \
    X(image_single) X(mask_index) X(depth) X(visible) X(solid) X(persistent) X(id) X(object_index)    \
    X(alarm) X(bbox_left) X(bbox_right) X(bbox_top) X(bbox_bottom) X(sprite_width) X(sprite_height)   \
    X(sprite_xoffset) X(sprite_yoffset) X(path_index) X(path_position) X(path_positionprevious)       \
    X(path_speed) X(path_scale) X(path_orientation) X(path_endaction) X(timeline_index)               \
    X(timeline_position) X(timeline_speed) X(timeline_running) X(timeline_loop)                       \
    /* script arguments */                                                                            \
    X(argument) X(argument_count) X(argument0) X(argument1) X(argument2) X(argument3) X(argument4)    \
    X(argument5) X(argument6) X(argument7) X(argument8) X(argument9) X(argument10) X(argument11)      \
    X(argument12) X(argument13) X(argument14) X(argument15)                                           \
    /* global */                                                                                      \
    X(room) X(room_speed) X(room_width) X(room_height) X(room_caption) X(room_persistent) X(room_first) \
    X(room_last) X(fps) X(fps_real) X(current_time) X(instance_count) X(instance_id) X(mouse_x)       \
    X(mouse_y) X(mouse_button) X(mouse_lastbutton) X(keyboard_key) X(keyboard_lastkey)                \
    X(keyboard_lastchar) X(keyboard_string) X(background_color) X(background_colour)                  \
    X(background_showcolor) X(background_showcolour) X(background_visible) X(background_foreground)   \
    X(background_index) X(background_x) X(background_y) X(background_hspeed) X(background_vspeed)     \
    X(background_htiled) X(background_vtiled) X(background_xscale) X(background_yscale)               \
    X(background_alpha) X(background_blend) X(background_width) X(background_height)                 \
    X(view_enabled) X(view_current) X(view_visible) X(view_xview) X(view_yview) X(view_wview)         \
    X(view_hview) X(view_xport) X(view_yport) X(view_wport) X(view_hport) X(view_angle)               \
    X(view_hborder) X(view_vborder) X(view_hspeed) X(view_vspeed) X(view_object)                      \
    X(application_surface) X(working_directory) X(program_directory) X(temp_directory)               \
    X(game_id) X(game_display_name) X(async_load) X(score) X(lives) X(health) X(delta_time)           \
    X(os_type) X(os_device) X(debug_mode) X(cursor_sprite) X(error_occurred) X(error_last)            \
    X(show_score) X(show_lives) X(show_health) X(secure_mode) X(browser_width) X(browser_height)               \
    /* GMS2 */                                                                                        \
    X(undefined) X(pointer_null) X(pointer_invalid) X(infinity) X(NaN) X(view_camera) X(view_surface_id) X(layer) X(game_save_id)          \
    X(current_year) X(current_month) X(current_day) X(current_weekday) X(current_hour)                  \
    X(current_minute) X(current_second) X(image_speed_fps) X(audio_bus_main) X(event_type) X(event_number) X(event_object) \
    X(in_sequence) X(sequence_instance) X(drawn_by_sequence) X(event_data)

enum BuiltinVar : int16_t {
#define GM_ENUM(n) BV_##n,
    GM_BUILTIN_VARS(GM_ENUM)
#undef GM_ENUM
    BV_COUNT
};

int16_t builtinVarId(std::string_view name);
const char* builtinVarName(int16_t id);

// Built-ins that are arrays (alarm, view_*, background_*) take an index; others ignore it.
Value getBuiltinVar(Runner& rt, Instance* self, int16_t id, int64_t index, Value* args, int argc);
Value audioMainBus(Runner& rt);  // AudioInputLib
Value sequenceInstanceStruct(Runner& rt, int32_t element);  // SequenceLib (undefined if none)
void setBuiltinVar(Runner& rt, Instance* self, int16_t id, int64_t index, Value v, Value* args, int argc);

}  // namespace gm
