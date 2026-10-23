// Helpers for implementing GML built-in functions.
#pragma once

#include <cstdlib>
#include <string>

#include "Instance.h"
#include "Runner.h"
#include "vm/VM.h"

namespace gm {

void registerAllBuiltins(Runner& rt);
void registerCoreBuiltins(Runner& rt);
void registerDsBuiltins(Runner& rt);
void registerInstanceBuiltins(Runner& rt);
void registerDrawBuiltins(Runner& rt);
void registerAudioBuiltins(Runner& rt);
void registerInputBuiltins(Runner& rt);
void registerFileBuiltins(Runner& rt);
void registerParticleBuiltins(Runner& rt);
void registerActionBuiltins(Runner& rt);

// Geometry helpers shared between libraries (Collision.cpp).
double pathLength(const Runner& rt, int32_t index);
bool pathPoint(const Runner& rt, int32_t index, double pos, double& x, double& y, double& speed);
void registerGms2Builtins(Runner& rt);
void registerJsonBuiltins(Runner& rt);
void registerBufferBuiltins(Runner& rt);
void registerLayerBuiltins(Runner& rt);
void registerMiscBuiltins(Runner& rt);
void registerArrayBuiltins(Runner& rt);
void registerTimeBuiltins(Runner& rt);
void registerVertexBuiltins(Runner& rt);
void registerSpriteBuiltins(Runner& rt);
void registerAnimCurveBuiltins(Runner& rt);
void registerSequenceBuiltins(Runner& rt);
// AnimCurveLib, for sequences: a curve struct for curve data (embedded in a keyframe), a curve
// given as struct or asset id (undefined if none), and a channel's sample points (x 0..1, value)
// as the runner evaluates them.
Value animCurveFromData(Runner& rt, const AnimCurve& c);
Value animCurveArg(Runner& rt, const Value& v);
std::vector<std::pair<double, double>> animCurveSamples(Runner& rt, const Value& channel);
void registerDateBuiltins(Runner& rt);
void registerVideoBuiltins(Runner& rt);
void registerExtensionEmulation(Runner& rt);  // FMODGMS & co., before the extension stubs
bool extensionStubbed(Runner& rt, std::string_view name);  // a native function that only returns 0 or ""
// LayerLib: the fx struct for a room layer's effect (undefined if it has none).
Value roomLayerFx(Runner& rt, const RoomLayer& L);
// Texture handles (sprite_get_texture, surface_get_texture, ...) are pointers in GameMaker, and
// libraries check is_numeric / is_ptr on them, so they travel as ptr values holding handle + 1.
inline Value texValue(int handle) {
    return handle < 0 ? Value::ptr(nullptr) : Value::ptr(reinterpret_cast<void*>(static_cast<intptr_t>(handle) + 1));
}
inline double texArg(const Value& v) {
    if (v.type() == VT::Ptr) return v.rawPtr() ? static_cast<double>(reinterpret_cast<intptr_t>(v.rawPtr()) - 1) : -1.0;
    return v.isNumeric() ? v.num() : -1.0;
}
// BufferLib, for vertex buffers: a buffer's bytes (nullptr if it doesn't exist) and a new buffer.
std::vector<uint8_t>* bufferBytes(int id);
int bufferCreateWith(std::vector<uint8_t> data, int type, int align);
// JsonLib: JSON text as GML values (structs and arrays), as json_parse.
Value jsonToValue(Runner& rt, const std::string& text, bool& ok);
// LayerLib: an fx struct's filter name and whether it affects only its own layer.
bool fxInfo(const Value& fx, std::string& name, bool& singleLayer);
// "{0}" placeholders (string(), string_ext); "{{" and "}}" are literal braces.
std::string formatPlaceholders(const std::string& fmt, Value* vals, int n);

#define GMFN(name) static Value fn_##name(Runner& rt, Instance* self, Instance* other, int argc, Value* args)
#define GMREG(name) rt.vm.registerBuiltin(#name, fn_##name)
#define GMALIAS(alias, name) rt.vm.registerBuiltin(#alias, fn_##name)
#define GM_UNUSED_FN_ARGS (void)rt, (void)self, (void)other, (void)argc, (void)args

inline const Value& argv(Value* args, int argc, int i) {
    static const Value undef;
    return i < argc ? args[i] : undef;
}
// GMS2 converts numeric strings passed to built-ins that expect numbers ("1" -> 1); 1.4 reads them as 0.
inline bool gStringArgsAsNumbers = false;
inline double argNum(const Value& v) {
    if (gStringArgsAsNumbers && v.isString()) {
        const std::string& s = v.str();
        char* end = nullptr;
        double d = strtod(s.c_str(), &end);
        return end != s.c_str() ? d : 0.0;
    }
    return v.num();
}
inline double argD(Value* args, int argc, int i, double def = 0) {
    return i < argc ? argNum(args[i]) : def;
}
inline int argI(Value* args, int argc, int i, int def = 0) {
    if (i >= argc) return def;
    if (gStringArgsAsNumbers && args[i].isString()) return Value(argNum(args[i])).toInt();
    return args[i].toInt();
}
inline bool argB(Value* args, int argc, int i, bool def = false) { return i < argc ? args[i].truthy() : def; }
inline std::string argS(Value* args, int argc, int i) { return i < argc ? args[i].toString() : std::string(); }
inline uint32_t argC(Value* args, int argc, int i, uint32_t def = 0xFFFFFF) {
    return i < argc ? static_cast<uint32_t>(args[i].toInt64()) & 0xFFFFFF : def;
}

#define D(i) argD(args, argc, i)
#define I(i) argI(args, argc, i)
#define B(i) argB(args, argc, i)
#define S(i) argS(args, argc, i)
#define C(i) argC(args, argc, i)
#define ARG(i) argv(args, argc, i)

inline Value boolV(bool b) { return Value(b ? 1.0 : 0.0); }

}  // namespace gm
