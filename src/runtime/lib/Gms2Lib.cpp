// GameMaker Studio 2 / 2.3 built-ins: structs and methods, arrays, layers, cameras, gpu_*,
// game speed, motion-planning grids, ds serialisation and assorted newer functions.
#include <algorithm>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <queue>
#include <sstream>

#include "runtime/Audio.h"
#include "runtime/Builtins.h"
#include "runtime/BuiltinVars.h"
#include "runtime/Graphics.h"
#include "runtime/Stores.h"
#include "util/Fs.h"

namespace gm {

#define G (*rt.gfx)

// ---------------------------------------------------------------------------------------------
// Language support (compiler-emitted helpers, structs, methods)

GMFN(gml_this) { return rt.vm.selfValue(self); }
GMFN(gml_other) { return rt.vm.selfValue(other); }
GMFN(gml_global) { return Value(static_cast<double>(INST_GLOBAL)); }
GMFN(gml_new_array) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    a->rows[0].assign(args, args + argc);
    return arr;
}
GMFN(gml_new_object) {
    // @@NewGMLObject@@(constructor, args...): build a struct and run the constructor on it.
    Value s = rt.vm.newStruct(self);
    if (argc > 0 && (args[0].isMethod() || args[0].isNumeric())) {
        Instance* inst = s.obj()->inst;
        GMMethod* m = args[0].method();
        Value fn = args[0];
        if (m && !m->self.isUndefined()) {  // constructors run on the new struct, not the bound self
            auto* copy = new GMMethod(*m);
            copy->rc = 0;
            copy->self = Value();
            fn = Value::fromMethod(copy);
        }
        // The struct falls back to the constructor's static variables.
        int32_t code = rt.vm.codeOf(fn);
        if (code >= 0) inst->staticParent = rt.vm.staticScope(code);
        rt.vm.callValue(fn, inst, self, argc - 1, args + 1);
    }
    return s;
}
GMFN(gml_noop) { return Value(); }
GMFN(gml_null_object) { return Value(); }  // method(@@NullObject@@, f): an unbound method
GMFN(gml_throw) { throw GMLThrow(ARG(0), "uncaught exception: " + ARG(0).toString()); }
GMFN(gml_try_hook) {
    rt.vm.tryHook(argc, args);
    return Value();
}
GMFN(gml_try_unhook) {
    rt.vm.tryUnhook();
    return Value();
}
GMFN(gml_finish_finally) {
    rt.vm.finishFinally();
    return Value();
}
GMFN(gml_copy_static) {
    // Child constructor: its static scope falls back to the parent constructor's.
    int32_t parent = rt.vm.codeOf(ARG(0)), code = rt.vm.currentCode();
    if (parent >= 0 && code >= 0 && parent != code) rt.vm.staticScope(code)->staticParent = rt.vm.staticScope(parent);
    return Value();
}
GMFN(method) { return rt.vm.bindMethod(ARG(0), ARG(1), self); }
GMFN(method_get_self) {
    GMMethod* m = ARG(0).method();
    return m ? m->self : Value();
}
GMFN(method_get_index) {
    GMMethod* m = ARG(0).method();
    if (!m) return Value();
    auto* copy = new GMMethod(*m);
    copy->rc = 0;
    copy->self = Value();
    return Value::fromMethod(copy);
}
GMFN(is_method) { return boolV(ARG(0).isMethod()); }
GMFN(is_struct) { return boolV(ARG(0).isObject()); }
GMFN(bool) { return Value::boolean(ARG(0).truthy()); }
// 2024 compilers index a call's result through this: f()[i].
GMFN(gml_array_get) {
    const GMArray* a = ARG(0).arr();
    if (!a) throw GMLError("trying to index a variable which is not an array");
    int64_t i = ARG(1).toInt64();
    if (a->rows.empty() || i < 0 || i >= static_cast<int64_t>(a->rows[0].size()))
        throw GMLError("array index out of range: " + std::to_string(i));
    return a->rows[0][static_cast<size_t>(i)];
}
GMFN(is_callable) { return boolV(ARG(0).isMethod()); }
GMFN(is_numeric) { return boolV(ARG(0).isNumeric()); }
GMFN(is_nan) { return boolV(ARG(0).isNumeric() && std::isnan(ARG(0).num())); }
GMFN(is_infinity) { return boolV(ARG(0).isNumeric() && std::isinf(ARG(0).num())); }
GMFN(typeof) {
    switch (ARG(0).type()) {
        case VT::Real: return Value("number");
        case VT::String: return Value("string");
        case VT::Array: return Value("array");
        case VT::Undefined: return Value("undefined");
        case VT::Int32: return Value("int32");
        case VT::Int64: return Value("int64");
        case VT::Bool: return Value("bool");
        case VT::Ptr: return Value("ptr");
        case VT::Object: return Value("struct");
        case VT::Method: return Value("method");
    }
    return Value("unknown");
}
GMFN(script_execute) {
    if (argc == 0) return Value();
    return rt.vm.callValue(args[0], self, other, argc - 1, args + 1);
}
GMFN(script_execute_ext) {
    GMArray* a = ARG(1).arr();
    std::vector<Value> list;
    if (a && !a->rows.empty()) list = a->rows[0];
    return rt.vm.callValue(ARG(0), self, other, static_cast<int>(list.size()), list.data());
}

GMFN(method_call) {  // (method, args array, [offset], [count])
    std::vector<Value> list;
    if (GMArray* a = ARG(1).arr())
        if (!a->rows.empty()) {
            auto& r = a->rows[0];
            int n = static_cast<int>(r.size());
            int off = argc > 2 ? I(2) : 0;
            if (off < 0) off = std::max(0, n + off);
            off = std::min(off, n);
            int count = argc > 3 ? std::max(0, std::min(I(3), n - off)) : n - off;
            list.assign(r.begin() + off, r.begin() + off + count);
        }
    return rt.vm.callValue(ARG(0), self, other, static_cast<int>(list.size()), list.data());
}

// variable_clone(value, [depth]): deep copy of arrays and structs (as YoYo's runner: shared
// references stay shared, methods bound to a cloned struct are bound to its clone).
static Value cloneValue(Runner& rt, const Value& v, int depth, std::unordered_map<const void*, Value>& seen) {
    if (depth <= 0) return v;
    if (GMArray* a = v.arr()) {
        if (auto it = seen.find(a); it != seen.end()) return it->second;
        Value out = Value::newArray();
        GMArray* o = out.writableArray();
        seen[a] = out;
        o->rows.resize(a->rows.size());
        for (size_t r = 0; r < a->rows.size(); r++)
            for (const Value& e : std::vector<Value>(a->rows[r])) o->rows[r].push_back(cloneValue(rt, e, depth - 1, seen));
        return out;
    }
    GMObject* src = v.obj();
    if (!src || src->weakRef) return v;
    if (auto it = seen.find(src); it != seen.end()) return it->second;
    Value out = rt.vm.newStruct(nullptr);
    seen[src] = out;
    Instance* di = out.obj()->inst;
    di->staticParent = src->inst->staticParent;
    std::vector<std::pair<int32_t, Value>> vars;
    src->inst->vars.forEachInOrder([&](int32_t id, const Value& x) { vars.push_back({id, x}); });
    for (auto& [id, x] : vars) {
        Value c = cloneValue(rt, x, depth - 1, seen);
        if (GMMethod* m = c.method(); m && m->self.obj() == src) {
            auto* bound = new GMMethod(*m);
            bound->rc = 0;
            bound->self = out;
            c = Value::fromMethod(bound);
        }
        di->vars.getOrCreate(id) = c;
    }
    return out;
}
GMFN(variable_clone) {
    std::unordered_map<const void*, Value> seen;
    return cloneValue(rt, ARG(0), argc > 1 ? std::max(0, I(1)) : 128, seen);
}

// handle_parse("ref sprite 3") -> the handle (a plain id here); an asset name may replace the number.
GMFN(handle_parse) {
    std::string s = S(0);
    if (s.rfind("ref ", 0) != 0) return Value();
    size_t sp = s.find(' ', 4);
    if (sp == std::string::npos) return Value();
    std::string type = s.substr(4, sp - 4), rest = s.substr(sp + 1);
    rest.erase(0, rest.find_first_not_of(' '));
    if (rest.empty()) return Value();
    char* end = nullptr;
    double n = std::strtod(rest.c_str(), &end);
    if (end && *end == 0) return Value(n);
    auto find = [&](const auto& list) -> Value {
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].name == rest) return Value(static_cast<double>(i));
        return Value(-1.0);
    };
    if (type == "object") return find(rt.dw.objects);
    if (type == "sprite") return find(rt.dw.sprites);
    if (type == "sound") return find(rt.dw.sounds);
    if (type == "room") return find(rt.dw.rooms);
    if (type == "path") return find(rt.dw.paths);
    if (type == "script") return find(rt.dw.scripts);
    if (type == "font") return find(rt.dw.fonts);
    if (type == "timeline") return find(rt.dw.timelines);
    if (type == "shader") return find(rt.dw.shaders);
    if (type == "tileset") return find(rt.dw.backgrounds);
    return Value();
}

static Instance* structOrInstance(Runner& rt, Instance* self, Instance* other, const Value& v) {
    return rt.vm.targetOf(v, self, other);
}
GMFN(variable_struct_get) {
    Instance* i = structOrInstance(rt, self, other, ARG(0));
    return i ? rt.vm.memberGet(i, rt.vm.varIdForName(S(1))) : Value();
}
GMFN(variable_struct_set) {
    if (Instance* i = structOrInstance(rt, self, other, ARG(0))) rt.vm.memberSet(i, rt.vm.varIdForName(S(1)), ARG(2));
    return Value();
}
GMFN(variable_struct_set_post) {  // `s[$ k] = v` as an expression: returns v
    if (Instance* i = structOrInstance(rt, self, other, ARG(0))) rt.vm.memberSet(i, rt.vm.varIdForName(S(1)), ARG(2));
    return ARG(2);
}
// @@SetStatic@@ (GameMaker 2024+ compilers emit it at the start of constructors): the struct
// being built falls back to the constructor's static variables (`new` normally did this already).
GMFN(gml_set_static) {
    GM_UNUSED_FN_ARGS;
    int32_t code = rt.vm.currentCode();
    if (self && self->structObj && !self->staticParent && code >= 0) self->staticParent = rt.vm.staticScope(code);
    return Value();
}
// The function name of a code entry ("gml_Script_Foo@Bar@scr" -> "Foo").
static std::string functionName(Runner& rt, int32_t code) {
    std::string n(rt.dw.code[code].name);
    if (n.rfind("gml_Script_", 0) == 0) n = n.substr(11);
    size_t at = n.find('@');
    return at == std::string::npos ? n : n.substr(0, at);
}
// instanceof(struct): the name of the constructor that made it, "Object" for a struct literal.
GMFN(instanceof) {
    GMObject* o = ARG(0).obj();
    if (!o) return Value();
    if (!o->inst->staticParent) return Value("Object");
    int32_t code = rt.vm.staticOwner(o->inst->staticParent);
    return code >= 0 ? Value(functionName(rt, code)) : Value("Object");
}
// is_instanceof(struct, constructor): made by it or by a constructor that inherits from it.
GMFN(is_instanceof) {
    GMObject* o = ARG(0).obj();
    int32_t want = rt.vm.codeOf(ARG(1));
    if (!o || want < 0) return boolV(false);
    Instance* target = rt.vm.staticScope(want);
    for (Instance* s = o->inst->staticParent; s; s = s->staticParent)
        if (s == target) return boolV(true);
    return boolV(false);
}
// static_get(struct or function): its static struct; static_set(struct, static struct).
GMFN(static_get) {
    if (GMObject* o = ARG(0).obj()) {
        Instance* s = o->inst->staticParent;
        return s && s->structObj ? Value::fromObject(s->structObj) : Value();
    }
    int32_t code = rt.vm.codeOf(ARG(0));
    if (code < 0) return Value();
    Instance* s = rt.vm.staticScope(code);
    return s->structObj ? Value::fromObject(s->structObj) : Value();
}
GMFN(static_set) {
    GMObject* o = ARG(0).obj();
    GMObject* st = ARG(1).obj();
    if (o) o->inst->staticParent = st ? st->inst : nullptr;
    return Value();
}

// Variable hashes (GameMaker 2023+): our variable ids.
GMFN(variable_get_hash) { return Value(static_cast<double>(rt.vm.varIdForName(S(0)))); }
GMFN(struct_get_from_hash) {
    Instance* i = structOrInstance(rt, self, other, ARG(0));
    return i ? rt.vm.memberGet(i, I(1)) : Value();
}
GMFN(struct_set_from_hash) {
    if (Instance* i = structOrInstance(rt, self, other, ARG(0))) rt.vm.memberSet(i, I(1), ARG(2));
    return Value();
}
GMFN(struct_exists_from_hash) {
    Instance* i = structOrInstance(rt, self, other, ARG(0));
    return boolV(i && rt.vm.memberExists(i, I(1)));
}
GMFN(struct_remove_from_hash) {
    if (Instance* i = structOrInstance(rt, self, other, ARG(0)))
        i->vars.erase(I(1));
    return Value();
}
GMFN(variable_struct_exists) {
    Instance* i = structOrInstance(rt, self, other, ARG(0));
    return boolV(i && rt.vm.memberExists(i, rt.vm.varIdForName(S(1))));
}
GMFN(variable_struct_remove) {
    if (Instance* i = structOrInstance(rt, self, other, ARG(0)))
        i->vars.erase(rt.vm.varIdForName(S(1)));
    return Value();
}
GMFN(variable_struct_get_names) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    if (Instance* i = structOrInstance(rt, self, other, ARG(0)))
        i->vars.forEachInOrder([&](int32_t id, const Value&) { a->rows[0].push_back(Value(std::string(rt.vm.varName(id)))); });
    return arr;
}
GMFN(variable_struct_names_count) {
    int n = 0;
    if (Instance* i = structOrInstance(rt, self, other, ARG(0)))
        n = static_cast<int>(i->vars.size());
    return Value(static_cast<double>(n));
}

// ---------------------------------------------------------------------------------------------
// Arrays (GMS 2.3: one-dimensional, nested for more dimensions)

static std::vector<Value>* row(const Value& v) {
    GMArray* a = v.arr();
    if (!a) return nullptr;
    if (a->rows.empty()) a->rows.resize(1);
    return &a->rows[0];
}
// Arrays passed to functions are modified in place (they're references in GMS2).
GMFN(array_length) { auto* r = row(ARG(0)); return Value(r ? static_cast<double>(r->size()) : 0.0); }
GMFN(array_get) {
    auto* r = row(ARG(0));
    int i = I(1);
    return r && i >= 0 && static_cast<size_t>(i) < r->size() ? (*r)[i] : Value();
}
GMFN(array_set) {
    auto* r = row(ARG(0));
    int i = I(1);
    if (r && i >= 0) {
        if (static_cast<size_t>(i) >= r->size()) r->resize(i + 1, Value(0.0));
        (*r)[i] = ARG(2);
    }
    return Value();
}
GMFN(array_set_post) {  // `a[i] = v` as an expression: returns v
    fn_array_set(rt, self, other, argc, args);
    return ARG(2);
}
GMFN(array_push) {
    if (auto* r = row(ARG(0)))
        for (int i = 1; i < argc; i++) r->push_back(args[i]);
    return Value();
}
GMFN(array_pop) {
    auto* r = row(ARG(0));
    if (!r || r->empty()) return Value();
    Value v = r->back();
    r->pop_back();
    return v;
}
GMFN(array_insert) {
    auto* r = row(ARG(0));
    int i = I(1);
    if (r && i >= 0) {
        if (static_cast<size_t>(i) > r->size()) r->resize(i, Value(0.0));
        r->insert(r->begin() + i, args + 2, args + std::max(2, argc));
    }
    return Value();
}
GMFN(array_delete) {
    auto* r = row(ARG(0));
    int i = I(1), n = I(2);
    if (r && i >= 0 && static_cast<size_t>(i) < r->size()) r->erase(r->begin() + i, r->begin() + std::min<size_t>(r->size(), i + n));
    return Value();
}
GMFN(array_resize) {
    if (auto* r = row(ARG(0))) r->resize(std::max(0, I(1)), Value(0.0));
    return Value();
}
GMFN(array_copy) {
    auto* dst = row(ARG(0));
    auto* src = row(ARG(2));
    if (!dst || !src) return Value();
    int di = I(1), si = I(3), n = I(4);
    for (int k = 0; k < n && si + k < static_cast<int>(src->size()); k++) {
        if (di + k >= static_cast<int>(dst->size())) dst->resize(di + k + 1, Value(0.0));
        (*dst)[di + k] = (*src)[si + k];
    }
    return Value();
}
static bool valuesEqual(const Value& a, const Value& b) {
    if (a.isString() || b.isString()) return a.isString() && b.isString() && a.str() == b.str();
    if (a.isArray() && b.isArray()) {
        auto *x = row(a), *y = row(b);
        if (x->size() != y->size()) return false;
        for (size_t i = 0; i < x->size(); i++)
            if (!valuesEqual((*x)[i], (*y)[i])) return false;
        return true;
    }
    if (a.isNumeric() && b.isNumeric()) return std::fabs(a.num() - b.num()) <= 0.00001;
    return a.type() == b.type() && a.rawInt() == b.rawInt();
}
GMFN(array_equals) {
    auto *x = row(ARG(0)), *y = row(ARG(1));
    if (!x || !y) return boolV(false);
    if (x->size() != y->size()) return boolV(false);
    for (size_t i = 0; i < x->size(); i++)
        if (!valuesEqual((*x)[i], (*y)[i])) return boolV(false);
    return boolV(true);
}
GMFN(array_contains) {
    if (auto* r = row(ARG(0)))
        for (auto& v : *r)
            if (valuesEqual(v, ARG(1))) return boolV(true);
    return boolV(false);
}
GMFN(array_sort) {
    auto* r = row(ARG(0));
    if (!r) return Value();
    if (ARG(1).isMethod()) {
        Value fn = ARG(1);
        std::stable_sort(r->begin(), r->end(), [&](const Value& a, const Value& b) {
            Value pair[2] = {a, b};
            return rt.vm.callValue(fn, self, other, 2, pair).num() < 0;
        });
    } else {
        bool asc = argc < 2 || B(1);
        std::stable_sort(r->begin(), r->end(), [&](const Value& a, const Value& b) {
            if (a.isString() && b.isString()) return asc ? a.str() < b.str() : a.str() > b.str();
            return asc ? a.num() < b.num() : a.num() > b.num();
        });
    }
    return Value();
}
GMFN(array_first) { auto* r = row(ARG(0)); return r && !r->empty() ? r->front() : Value(); }
GMFN(array_last) { auto* r = row(ARG(0)); return r && !r->empty() ? r->back() : Value(); }

// ---------------------------------------------------------------------------------------------
// Instances on layers

// Creating instances (as GameMaker's instance_create_depth/layer): Pre-Create, then the
// optional struct's variables are copied in (built-ins through their setters, methods bound to
// the struct rebound to the instance), then Create.
static void createWithVars(Runner& rt, Instance* inst, Instance* creator, const Value& vars) {
    rt.performEvent(inst, EV_PRECREATE, 0, creator);
    if (GMObject* o = vars.obj()) {
        o->inst->vars.forEachInOrder([&](int32_t id, const Value& v) {
            Value val = v;
            if (GMMethod* m = v.method(); m && m->self.obj() == o) {
                auto* copy = new GMMethod(*m);
                copy->rc = 0;
                copy->self = rt.vm.selfValue(inst);
                val = Value::fromMethod(copy);
            }
            int16_t b = builtinVarId(rt.vm.varName(id));
            if (b >= 0) setBuiltinVar(rt, inst, b, -1, val, nullptr, 0);
            else rt.vm.setInstanceVar(inst, id, val);
        });
    }
    if (!inst->destroyed) rt.performEvent(inst, EV_CREATE, 0, creator);
}
GMFN(instance_create_depth) {
    Instance* inst = rt.createInstance(D(0), D(1), I(3), -1, false);
    if (!inst) return Value(-4.0);
    inst->depth = D(2);
    createWithVars(rt, inst, self, ARG(4));
    return Value(static_cast<double>(inst->id));
}
GMFN(instance_create_layer) {
    Instance* inst = rt.createInstance(D(0), D(1), I(3), -1, false);
    if (!inst) return Value(-4.0);
    if (Layer* L = rt.layerFromValue(ARG(2))) {
        rt.addInstanceToLayer(inst, *L);
    } else {
        rt.vm.warnOnce("layer:" + ARG(2).toString(), "instance_create_layer: no layer " + ARG(2).toString());
    }
    createWithVars(rt, inst, self, ARG(4));
    return Value(static_cast<double>(inst->id));
}

// ---------------------------------------------------------------------------------------------
// gpu_* (GMS2 names for the draw_set_* state)

GMFN(gpu_set_fog) {
    if (GMArray* a = ARG(0).arr()) {
        auto* r = row(ARG(0));
        G.setFog(r->size() > 0 && (*r)[0].truthy(), r->size() > 1 ? static_cast<uint32_t>((*r)[1].toInt64()) & 0xFFFFFF : 0);
        (void)a;
    } else {
        G.setFog(B(0), C(1));
    }
    return Value();
}
GMFN(gpu_set_blendmode) { G.setBlendMode(I(0)); return Value(); }
GMFN(gpu_set_blendmode_ext) {
    if (auto* r = row(ARG(0))) {
        if (r->size() >= 2) G.setBlendModeExt((*r)[0].toInt(), (*r)[1].toInt());
    } else {
        G.setBlendModeExt(I(0), I(1));
    }
    return Value();
}
GMFN(gpu_set_blendenable) { G.setAlphaBlend(B(0)); return Value(); }
GMFN(gpu_set_colorwriteenable) {
    if (auto* r = row(ARG(0))) {
        if (r->size() >= 4) G.setColorWrite((*r)[0].truthy(), (*r)[1].truthy(), (*r)[2].truthy(), (*r)[3].truthy());
    } else {
        G.setColorWrite(B(0), B(1), B(2), B(3));
    }
    return Value();
}
GMFN(gpu_set_alphatestenable) { G.setAlphaTest(B(0)); return Value(); }
GMFN(gpu_set_alphatestref) { G.setAlphaTestRef(D(0)); return Value(); }
GMFN(gpu_set_texfilter) { G.setTexFilter(B(0)); return Value(); }

// ---------------------------------------------------------------------------------------------
// Cameras and matrices

static Value matrixValue(const double (&m)[16]) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    for (double d : m) a->rows[0].push_back(Value(d));
    return arr;
}
static bool matrixArg(const Value& v, double (&m)[16]) {
    auto* r = row(v);
    if (!r || r->size() < 16) return false;
    for (int i = 0; i < 16; i++) m[i] = (*r)[i].num();
    return true;
}
GMFN(matrix_build_identity) {
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return matrixValue(m);
}
GMFN(matrix_build_lookat) {
    // D3D-style left-handed look-at, stored as GameMaker arrays (rows of the D3D matrix).
    double fx = D(0), fy = D(1), fz = D(2), tx = D(3), ty = D(4), tz = D(5), ux = D(6), uy = D(7), uz = D(8);
    double zx = tx - fx, zy = ty - fy, zz = tz - fz;
    double zl = std::sqrt(zx * zx + zy * zy + zz * zz);
    if (zl > 0) zx /= zl, zy /= zl, zz /= zl;
    double xx = uy * zz - uz * zy, xy = uz * zx - ux * zz, xz = ux * zy - uy * zx;
    double xl = std::sqrt(xx * xx + xy * xy + xz * xz);
    if (xl > 0) xx /= xl, xy /= xl, xz /= xl;
    double yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
    double m[16] = {xx, yx, zx, 0, xy, yy, zy, 0, xz, yz, zz, 0,
                    -(xx * fx + xy * fy + xz * fz), -(yx * fx + yy * fy + yz * fz), -(zx * fx + zy * fy + zz * fz), 1};
    return matrixValue(m);
}
GMFN(matrix_build_projection_ortho) {
    double w = D(0), h = D(1), zn = D(2), zf = D(3);
    double m[16] = {2 / w, 0, 0, 0, 0, 2 / h, 0, 0, 0, 0, 1 / (zf - zn), 0, 0, 0, zn / (zn - zf), 1};
    return matrixValue(m);
}
static Camera* cam(Runner& rt, int id) {
    if (id < 0 || static_cast<size_t>(id) >= rt.cameras.size() || !rt.cameras[id].live) return nullptr;
    return &rt.cameras[id];
}
GMFN(camera_create) { return Value(static_cast<double>(rt.createCamera(Camera()))); }
GMFN(camera_create_view) {
    Camera c;
    c.x = D(0);
    c.y = D(1);
    c.w = D(2);
    c.h = D(3);
    c.angle = D(4);
    c.object = argc > 5 ? I(5) : -1;
    c.hspeed = argc > 6 ? D(6) : -1;
    c.vspeed = argc > 7 ? D(7) : -1;
    c.hborder = argc > 8 ? D(8) : 0;
    c.vborder = argc > 9 ? D(9) : 0;
    return Value(static_cast<double>(rt.createCamera(c)));
}
GMFN(camera_destroy) {
    if (Camera* c = cam(rt, I(0))) {
        c->live = false;
        if (rt.activeCamera == I(0)) rt.activeCamera = -1;
    }
    return Value();
}
GMFN(camera_get_active) { return Value(static_cast<double>(cam(rt, rt.activeCamera) ? rt.activeCamera : -1)); }
GMFN(camera_get_default) { return Value(static_cast<double>(cam(rt, rt.defaultCamera) ? rt.defaultCamera : -1)); }
GMFN(camera_set_default) { if (cam(rt, I(0))) rt.defaultCamera = I(0); return Value(); }
// room_get_camera / room_set_camera(room, view[, camera]): the current room's views directly;
// another room's view gets a copy of the camera when that room is next entered.
GMFN(room_get_camera) {
    int r = I(0), v = I(1);
    if (v < 0 || v >= 8) return Value(-1.0);
    if (r == rt.room) return Value(static_cast<double>(rt.views[v].camera));
    auto it = rt.roomCameraSet.find(r);
    return Value(it != rt.roomCameraSet.end() ? static_cast<double>(it->second[v]) : -1.0);
}
GMFN(room_set_camera) {
    int r = I(0), v = I(1);
    if (v < 0 || v >= 8 || r < 0 || static_cast<size_t>(r) >= rt.dw.rooms.size()) return Value();
    if (r == rt.room) {
        rt.views[v].camera = I(2);
    } else {
        auto [it, added] = rt.roomCameraSet.try_emplace(r);
        if (added) it->second.fill(-1);
        it->second[v] = I(2);
    }
    return Value();
}
GMFN(camera_get_view_x) { Camera* c = cam(rt, I(0)); return Value(c ? c->x : 0.0); }
GMFN(camera_get_view_y) { Camera* c = cam(rt, I(0)); return Value(c ? c->y : 0.0); }
GMFN(camera_get_view_width) { Camera* c = cam(rt, I(0)); return Value(c ? c->w : 0.0); }
GMFN(camera_get_view_height) { Camera* c = cam(rt, I(0)); return Value(c ? c->h : 0.0); }
GMFN(camera_get_view_angle) { Camera* c = cam(rt, I(0)); return Value(c ? c->angle : 0.0); }
GMFN(camera_get_view_target) { Camera* c = cam(rt, I(0)); return Value(c ? static_cast<double>(c->object) : -1.0); }
GMFN(camera_set_view_pos) { if (Camera* c = cam(rt, I(0))) c->customView = c->customProj = false, c->x = D(1), c->y = D(2); return Value(); }
GMFN(camera_set_view_size) { if (Camera* c = cam(rt, I(0))) c->customView = c->customProj = false, c->w = D(1), c->h = D(2); return Value(); }
GMFN(camera_set_view_angle) { if (Camera* c = cam(rt, I(0))) c->customView = c->customProj = false, c->angle = D(1); return Value(); }
GMFN(camera_set_view_target) { if (Camera* c = cam(rt, I(0))) c->object = I(1); return Value(); }
GMFN(camera_set_view_border) { if (Camera* c = cam(rt, I(0))) c->hborder = D(1), c->vborder = D(2); return Value(); }
GMFN(camera_set_view_speed) { if (Camera* c = cam(rt, I(0))) c->hspeed = D(1), c->vspeed = D(2); return Value(); }
// The 2D renderer keeps cameras as rectangles: derive the centre from the view matrix and the
// size from the orthographic projection.
GMFN(camera_set_view_mat) {
    Camera* c = cam(rt, I(0));
    double m[16];
    if (!c || !matrixArg(ARG(1), m)) return Value();
    double cx = -(m[12] * m[0] + m[13] * m[1]), cy = -(m[12] * m[4] + m[13] * m[5]);
    c->x = cx - c->w / 2;
    c->y = cy - c->h / 2;
    c->viewSet = true;
    c->cx = cx;
    c->cy = cy;
    c->customView = true;
    std::copy(m, m + 16, c->viewMat);
    return Value();
}
GMFN(camera_set_proj_mat) {
    Camera* c = cam(rt, I(0));
    double m[16];
    if (!c || !matrixArg(ARG(1), m)) return Value();
    c->customProj = true;
    std::copy(m, m + 16, c->projMat);
    if (m[0] == 0 || m[5] == 0 || m[11] != 0) return Value();  // perspective: keep the 2D rect
    double w = std::fabs(2 / m[0]), h = std::fabs(2 / m[5]);
    double cx = c->viewSet ? c->cx : c->x + c->w / 2;
    double cy = c->viewSet ? c->cy : c->y + c->h / 2;
    c->w = w;
    c->h = h;
    c->x = cx - w / 2;
    c->y = cy - h / 2;
    return Value();
}
GMFN(view_get_camera) { int v = I(0); return Value(v >= 0 && v < 8 ? static_cast<double>(rt.views[v].camera) : -1.0); }
GMFN(view_set_camera) { int v = I(0); if (v >= 0 && v < 8) rt.views[v].camera = I(1); return Value(); }
GMFN(view_get_visible) { int v = I(0); return boolV(v >= 0 && v < 8 && rt.views[v].visible); }
GMFN(view_set_visible) { int v = I(0); if (v >= 0 && v < 8) rt.views[v].visible = B(1); return Value(); }
GMFN(view_get_wport) { int v = I(0); return Value(v >= 0 && v < 8 ? rt.views[v].wport : 0.0); }
GMFN(view_get_hport) { int v = I(0); return Value(v >= 0 && v < 8 ? rt.views[v].hport : 0.0); }
GMFN(view_set_wport) { int v = I(0); if (v >= 0 && v < 8) rt.views[v].wport = D(1); return Value(); }
GMFN(view_set_hport) { int v = I(0); if (v >= 0 && v < 8) rt.views[v].hport = D(1); return Value(); }
GMFN(view_get_xport) { int v = I(0); return Value(v >= 0 && v < 8 ? rt.views[v].xport : 0.0); }
GMFN(view_get_yport) { int v = I(0); return Value(v >= 0 && v < 8 ? rt.views[v].yport : 0.0); }
GMFN(view_set_xport) { int v = I(0); if (v >= 0 && v < 8) rt.views[v].xport = D(1); return Value(); }
GMFN(view_set_yport) { int v = I(0); if (v >= 0 && v < 8) rt.views[v].yport = D(1); return Value(); }
GMFN(view_get_surface_id) { int v = I(0); return Value(v >= 0 && v < 8 ? static_cast<double>(rt.views[v].surface) : -1.0); }
GMFN(view_set_surface_id) { int v = I(0); if (v >= 0 && v < 8) rt.views[v].surface = I(1); return Value(); }
GMFN(camera_get_view_border_x) { Camera* c = cam(rt, I(0)); return Value(c ? c->hborder : 0.0); }
GMFN(camera_get_view_border_y) { Camera* c = cam(rt, I(0)); return Value(c ? c->vborder : 0.0); }
GMFN(camera_get_view_speed_x) { Camera* c = cam(rt, I(0)); return Value(c ? c->hspeed : 0.0); }
GMFN(camera_get_view_speed_y) { Camera* c = cam(rt, I(0)); return Value(c ? c->vspeed : 0.0); }

// ---------------------------------------------------------------------------------------------
// Game speed

GMFN(game_set_speed) {
    double v = D(0);
    double fps = I(1) == 1 ? (v > 0 ? 1000000.0 / v : 60) : v;  // gamespeed_microseconds = 1
    if (fps > 0) rt.gameSpeed = rt.roomSpeed = fps;
    return Value();
}
GMFN(game_get_speed) {
    return Value(I(0) == 1 ? 1000000.0 / (rt.gameSpeed > 0 ? rt.gameSpeed : 60) : rt.gameSpeed);
}

// ---------------------------------------------------------------------------------------------
// Assets

GMFN(asset_get_index) {
    std::string n = S(0);
    auto find = [&](const auto& list) -> int {
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].name == n) return static_cast<int>(i);
        return -1;
    };
    int i;
    if ((i = find(rt.dw.objects)) >= 0) return Value(static_cast<double>(i));
    if ((i = find(rt.dw.sprites)) >= 0) return Value(static_cast<double>(i));
    if ((i = find(rt.dw.sounds)) >= 0) return Value(static_cast<double>(i));
    if ((i = find(rt.dw.rooms)) >= 0) return Value(static_cast<double>(i));
    if ((i = find(rt.dw.backgrounds)) >= 0) return Value(static_cast<double>(i));
    if ((i = find(rt.dw.paths)) >= 0) return Value(static_cast<double>(i));
    if ((i = find(rt.dw.scripts)) >= 0) return Value(static_cast<double>(i));
    if ((i = find(rt.dw.fonts)) >= 0) return Value(static_cast<double>(i));
    if ((i = find(rt.dw.timelines)) >= 0) return Value(static_cast<double>(i));
    if ((i = find(rt.dw.shaders)) >= 0) return Value(static_cast<double>(i));
    return Value(-1.0);
}
// asset_get_type(name): asset_* constants are 0 object, 1 sprite, 2 sound, 3 room, then (GameMaker
// 2024+, as YoYo's AT_ values) 4 path, 5 script, 6 font, 7 timeline, 8 shader, 13 tileset, or
// (older) 4 background/tiles, 5 path, 6 script, 7 font, 8 timeline, 10 shader; -1 unknown. The
// compiled constants show the switch: 2023.x games pass 7 for asset_font, 2024 ones 6 (the UILR
// chunk is the 2024 sign the data.win carries).
GMFN(asset_get_type) {
    std::string n = S(0);
    auto has = [&](const auto& list) {
        for (auto& a : list)
            if (a.name == n) return true;
        return false;
    };
    bool modern = rt.dw.hasChunk("UILR");
    if (has(rt.dw.objects)) return Value(0.0);
    if (has(rt.dw.sprites)) return Value(1.0);
    if (has(rt.dw.sounds)) return Value(2.0);
    if (has(rt.dw.rooms)) return Value(3.0);
    if (has(rt.dw.backgrounds)) return Value(modern ? 13.0 : 4.0);
    if (has(rt.dw.paths)) return Value(modern ? 4.0 : 5.0);
    if (has(rt.dw.scripts)) return Value(modern ? 5.0 : 6.0);
    if (has(rt.dw.fonts)) return Value(modern ? 6.0 : 7.0);
    if (has(rt.dw.timelines)) return Value(modern ? 7.0 : 8.0);
    if (has(rt.dw.shaders)) return Value(modern ? 8.0 : 10.0);
    return Value(-1.0);
}
GMFN(font_get_size) {
    int f = I(0);
    if (f >= 0 && static_cast<size_t>(f) < rt.dw.fonts.size()) return Value(static_cast<double>(rt.dw.fonts[f].emSize));
    return Value(G.fontPoints(f));  // added with font_add
}

// ---------------------------------------------------------------------------------------------
// Drawing variants

GMFN(draw_text_transformed_color) {
    G.drawText(D(0), D(1), ARG(2).toString(), -1, -1, D(3), D(4), D(5), C(6), C(7), C(8), C(9), D(10));
    return Value();
}
GMFN(draw_text_ext_transformed_color) {
    G.drawText(D(0), D(1), ARG(2).toString(), D(3), D(4), D(5), D(6), D(7), C(8), C(9), C(10), C(11), D(12));
    return Value();
}
static void spriteTiled(Runner& rt, int spr, double sub, double x, double y, double xs, double ys, uint32_t col, double a) {
    if (spr < 0 || static_cast<size_t>(spr) >= rt.dw.sprites.size()) return;
    const Sprite& sp = rt.dw.sprites[spr];
    double tw = sp.width * std::fabs(xs), th = sp.height * std::fabs(ys);
    if (tw < 1 || th < 1) return;
    double vx, vy, vw, vh;
    // Tile across the whole room (cheap enough for the rooms games use this in).
    vx = 0;
    vy = 0;
    vw = rt.roomWidth;
    vh = rt.roomHeight;
    double x0 = x - std::ceil((x - vx) / tw) * tw, y0 = y - std::ceil((y - vy) / th) * th;
    for (double ty = y0; ty < vy + vh; ty += th)
        for (double tx = x0; tx < vx + vw; tx += tw)
            G.drawSprite(spr, sub, tx + sp.originX * xs, ty + sp.originY * ys, xs, ys, 0, col, a);
}
GMFN(draw_sprite_tiled) {
    spriteTiled(rt, I(0), D(1) < 0 && self ? self->imageIndex : D(1), D(2), D(3), 1, 1, 0xFFFFFF, G.alpha);
    return Value();
}
GMFN(draw_sprite_tiled_ext) {
    spriteTiled(rt, I(0), D(1) < 0 && self ? self->imageIndex : D(1), D(2), D(3), D(4), D(5), C(6), D(7));
    return Value();
}
GMFN(draw_triangle_color) {
    if (B(9)) {
        G.drawLine(D(0), D(1), D(2), D(3), 1, C(6), C(7), G.alpha);
        G.drawLine(D(2), D(3), D(4), D(5), 1, C(7), C(8), G.alpha);
        G.drawLine(D(4), D(5), D(0), D(1), 1, C(8), C(6), G.alpha);
    } else {
        G.primitiveBegin(4);
        G.primitiveVertex(D(0), D(1), C(6), G.alpha);
        G.primitiveVertex(D(2), D(3), C(7), G.alpha);
        G.primitiveVertex(D(4), D(5), C(8), G.alpha);
        G.primitiveEnd();
    }
    return Value();
}

// ---------------------------------------------------------------------------------------------
// Collision lists

template <typename Test>
static Value collisionList(Runner& rt, Instance* self, Instance* other, int obj, bool notme, int list, bool ordered,
                           double ox, double oy, Test test) {
    std::vector<std::pair<double, Instance*>> hits;
    std::vector<Instance*> cands;
    rt.collectTargets(obj, self, other, cands);
    for (Instance* i : cands) {
        if (notme && i == self) continue;
        if (test(i)) hits.push_back({std::hypot(i->x - ox, i->y - oy), i});
    }
    if (ordered) std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first < b.first; });
    if (auto* l = rt.ds->lists.get(list))
        for (auto& h : hits) l->push_back(Value(static_cast<double>(h.second->id)));
    return Value(static_cast<double>(hits.size()));
}
GMFN(collision_circle_list) {
    return collisionList(rt, self, other, I(3), B(5), I(6), B(7), D(0), D(1),
                         [&](Instance* i) { return rt.circleInInstance(i, D(0), D(1), D(2), D(2), B(4)); });
}
GMFN(collision_rectangle_list) {
    return collisionList(rt, self, other, I(4), B(6), I(7), B(8), (D(0) + D(2)) / 2, (D(1) + D(3)) / 2,
                         [&](Instance* i) { return rt.rectInInstance(i, D(0), D(1), D(2), D(3), B(5)); });
}
GMFN(collision_point_list) {
    return collisionList(rt, self, other, I(2), B(4), I(5), B(6), D(0), D(1),
                         [&](Instance* i) { return rt.pointInInstance(i, D(0), D(1), B(3)); });
}
GMFN(collision_line_list) {  // ordered by distance from (x1, y1)
    return collisionList(rt, self, other, I(4), B(6), I(7), B(8), D(0), D(1),
                         [&](Instance* i) { return rt.lineInInstance(i, D(0), D(1), D(2), D(3), B(5)); });
}
GMFN(collision_ellipse_list) {
    double cx = (D(0) + D(2)) / 2, cy = (D(1) + D(3)) / 2, rx = std::fabs(D(2) - D(0)) / 2, ry = std::fabs(D(3) - D(1)) / 2;
    return collisionList(rt, self, other, I(4), B(6), I(7), B(8), cx, cy,
                         [&](Instance* i) { return rt.circleInInstance(i, cx, cy, rx, ry, B(5)); });
}
GMFN(instance_position_list) {  // (x, y, obj, list, ordered)
    return collisionList(rt, self, other, I(2), false, I(3), B(4), D(0), D(1),
                         [&](Instance* i) { return rt.pointInInstance(i, D(0), D(1), true); });
}
GMFN(instance_place_list) {
    if (!self) return Value(0.0);
    return collisionList(rt, self, other, I(2), true, I(3), B(4), D(0), D(1),
                         [&](Instance* i) { return rt.instancesCollide(self, D(0), D(1), i); });
}

// ---------------------------------------------------------------------------------------------
// ds_* additions and serialisation

GMFN(ds_exists) {
    int id = I(0);
    switch (I(1)) {
        case 1: return boolV(rt.ds->maps.get(id) != nullptr);
        case 2: return boolV(rt.ds->lists.get(id) != nullptr);
        case 3: return boolV(rt.ds->stacks.get(id) != nullptr);
        case 4: return boolV(rt.ds->queues.get(id) != nullptr);
        case 5: return boolV(rt.ds->grids.get(id) != nullptr);
        case 6: return boolV(rt.ds->priorities.get(id) != nullptr);
    }
    return boolV(false);
}
static Value* gridCell(Runner& rt, int g, int x, int y) {
    DsGrid* grid = rt.ds->grids.get(g);
    if (!grid || x < 0 || y < 0 || x >= grid->w || y >= grid->h) return nullptr;
    return &grid->cells[static_cast<size_t>(y) * grid->w + x];
}
GMFN(ds_grid_add) {
    if (Value* c = gridCell(rt, I(0), I(1), I(2))) {
        if (c->isString() || ARG(3).isString()) *c = Value(c->toString() + ARG(3).toString());
        else *c = Value(c->num() + D(3));
    }
    return Value();
}
GMFN(ds_grid_set_region) {
    for (int y = std::min(I(2), I(4)); y <= std::max(I(2), I(4)); y++)
        for (int x = std::min(I(1), I(3)); x <= std::max(I(1), I(3)); x++)
            if (Value* c = gridCell(rt, I(0), x, y)) *c = ARG(5);
    return Value();
}

// Serialised form: a printable string the runner reads back ("GMR" + typed values).
static void writeValue(std::string& out, const Value& v) {
    if (v.isString()) {
        out += "s" + std::to_string(v.str().size()) + ":" + v.str();
    } else if (v.isUndefined()) {
        out += "u;";
    } else {
        char buf[40];
        snprintf(buf, sizeof buf, "r%.17g;", v.num());
        out += buf;
    }
}
static bool readValue(const std::string& in, size_t& p, Value& v) {
    if (p >= in.size()) return false;
    char t = in[p++];
    if (t == 'u') {
        p++;
        v = Value();
        return true;
    }
    if (t == 'r') {
        size_t e = in.find(';', p);
        if (e == std::string::npos) return false;
        v = Value(std::strtod(in.substr(p, e - p).c_str(), nullptr));
        p = e + 1;
        return true;
    }
    if (t == 's') {
        size_t c = in.find(':', p);
        if (c == std::string::npos) return false;
        size_t n = std::strtoul(in.substr(p, c - p).c_str(), nullptr, 10);
        v = Value(in.substr(c + 1, n));
        p = c + 1 + n;
        return true;
    }
    return false;
}
GMFN(ds_list_write) {
    std::string out = "GMRL";
    if (auto* l = rt.ds->lists.get(I(0))) {
        out += std::to_string(l->size()) + ";";
        for (auto& v : *l) writeValue(out, v);
    }
    return Value(out);
}
GMFN(ds_list_read) {
    auto* l = rt.ds->lists.get(I(0));
    std::string in = S(1);
    if (!l || in.compare(0, 4, "GMRL") != 0) return Value();
    l->clear();
    size_t p = in.find(';', 4);
    size_t n = std::strtoul(in.substr(4, p - 4).c_str(), nullptr, 10);
    p++;
    for (size_t i = 0; i < n; i++) {
        Value v;
        if (!readValue(in, p, v)) break;
        l->push_back(v);
    }
    return Value();
}
GMFN(ds_grid_write) {
    std::string out = "GMRG";
    if (DsGrid* g = rt.ds->grids.get(I(0))) {
        out += std::to_string(g->w) + "," + std::to_string(g->h) + ";";
        for (auto& v : g->cells) writeValue(out, v);
    }
    return Value(out);
}
GMFN(ds_grid_read) {
    DsGrid* g = rt.ds->grids.get(I(0));
    std::string in = S(1);
    if (!g || in.compare(0, 4, "GMRG") != 0) return Value();
    size_t c = in.find(',', 4), p = in.find(';', 4);
    g->w = std::atoi(in.substr(4, c - 4).c_str());
    g->h = std::atoi(in.substr(c + 1, p - c - 1).c_str());
    g->cells.assign(static_cast<size_t>(g->w) * g->h, Value(0.0));
    p++;
    for (auto& cell : g->cells)
        if (!readValue(in, p, cell)) break;
    return Value();
}
static std::string mapToString(DsMap* m) {
    std::string out = "GMRM" + std::to_string(m->order.size()) + ";";
    for (auto& k : m->order) {
        writeValue(out, k.toValue());
        writeValue(out, m->values[k]);
    }
    return out;
}
static void mapFromString(DsMap* m, const std::string& in) {
    if (in.compare(0, 4, "GMRM") != 0) return;
    m->values.clear();
    m->order.clear();
    size_t p = in.find(';', 4);
    size_t n = std::strtoul(in.substr(4, p - 4).c_str(), nullptr, 10);
    p++;
    for (size_t i = 0; i < n; i++) {
        Value k, v;
        if (!readValue(in, p, k) || !readValue(in, p, v)) break;
        ValueKey key = ValueKey::of(k);
        if (!m->values.count(key)) m->order.push_back(key);
        m->values[key] = v;
    }
}
GMFN(ds_map_write) { DsMap* m = rt.ds->maps.get(I(0)); return Value(m ? mapToString(m) : std::string()); }
GMFN(ds_map_read) { if (DsMap* m = rt.ds->maps.get(I(0))) mapFromString(m, S(1)); return Value(); }
GMFN(ds_map_secure_save) {
    DsMap* m = rt.ds->maps.get(I(0));
    if (!m) return Value();
    std::ofstream f(rt.files->writePath(S(1)), std::ios::binary);
    f << mapToString(m);
    return Value();
}
GMFN(ds_map_secure_load) {
    std::ifstream f(rt.files->readPath(S(0)), std::ios::binary);
    if (!f) return Value(-1.0);
    std::stringstream ss;
    ss << f.rdbuf();
    int id = rt.ds->maps.create();
    mapFromString(rt.ds->maps.get(id), ss.str());
    return Value(static_cast<double>(id));
}

// ---------------------------------------------------------------------------------------------
// Paths and motion-planning grids

GMFN(path_add) {
    rt.runtimePaths.push_back(Path());
    rt.runtimePathLive.push_back(true);
    rt.pathVersion++;
    return Value(static_cast<double>(rt.dw.paths.size() + rt.runtimePaths.size() - 1));
}
// Any path can be edited, the game's own included (as GameMaker); only path_add ones can be deleted.
static Path* rtPath(Runner& rt, int id) {
    if (id < 0) return nullptr;
    if (static_cast<size_t>(id) < rt.dw.paths.size()) return &rt.dw.paths[id];
    size_t r = static_cast<size_t>(id) - rt.dw.paths.size();
    return r < rt.runtimePaths.size() && rt.runtimePathLive[r] ? &rt.runtimePaths[r] : nullptr;
}
GMFN(path_delete) {
    if (I(0) >= static_cast<int>(rt.dw.paths.size()) && rtPath(rt, I(0))) {
        rt.runtimePathLive[I(0) - rt.dw.paths.size()] = false;
        rt.pathVersion++;
    }
    return Value();
}
GMFN(path_add_point) {
    if (Path* p = rtPath(rt, I(0))) {
        p->points.push_back({static_cast<float>(D(1)), static_cast<float>(D(2)), static_cast<float>(D(3))});
        rt.pathVersion++;
    }
    return Value();
}
GMFN(path_clear_points) {
    if (Path* p = rtPath(rt, I(0))) {
        p->points.clear();
        rt.pathVersion++;
    }
    return Value();
}
GMFN(path_set_closed) { if (Path* p = rtPath(rt, I(0))) p->closed = B(1), rt.pathVersion++; return Value(); }
GMFN(path_set_kind) { if (Path* p = rtPath(rt, I(0))) p->smooth = B(1), rt.pathVersion++; return Value(); }
GMFN(path_get_number) { const Path* p = rt.pathAt(I(0)); return Value(p ? static_cast<double>(p->points.size()) : 0.0); }
GMFN(path_get_point_x) {
    const Path* p = rt.pathAt(I(0));
    int n = I(1);
    return Value(p && n >= 0 && static_cast<size_t>(n) < p->points.size() ? p->points[n].x : 0.0);
}
GMFN(path_get_point_y) {
    const Path* p = rt.pathAt(I(0));
    int n = I(1);
    return Value(p && n >= 0 && static_cast<size_t>(n) < p->points.size() ? p->points[n].y : 0.0);
}

// Path editing (YoYo's yyPath): mirror, flip, rotate and rescale work about the centre of the
// points' bounding box.
template <typename F> static void editPath(Runner& rt, int id, F&& f) {
    if (Path* p = rtPath(rt, id)) {
        f(*p);
        rt.pathVersion++;
    }
}
static void pathAbout(Path& p, const std::function<void(float&, float&)>& f) {
    if (p.points.empty()) return;
    float x0 = 1e8f, x1 = -1e8f, y0 = 1e8f, y1 = -1e8f;
    for (auto& q : p.points) {
        x0 = std::min(x0, q.x), x1 = std::max(x1, q.x);
        y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
    }
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    for (auto& q : p.points) {
        float x = q.x - cx, y = q.y - cy;
        f(x, y);
        q.x = x + cx;
        q.y = y + cy;
    }
}
GMFN(path_shift) {
    float dx = static_cast<float>(D(1)), dy = static_cast<float>(D(2));
    editPath(rt, I(0), [&](Path& p) { for (auto& q : p.points) q.x += dx, q.y += dy; });
    return Value();
}
GMFN(path_mirror) { editPath(rt, I(0), [&](Path& p) { pathAbout(p, [](float& x, float&) { x = -x; }); }); return Value(); }
GMFN(path_flip) { editPath(rt, I(0), [&](Path& p) { pathAbout(p, [](float&, float& y) { y = -y; }); }); return Value(); }
GMFN(path_rotate) {
    double th = D(1) * M_PI / 180, c = std::cos(th), sn = std::sin(th);
    editPath(rt, I(0), [&](Path& p) {
        pathAbout(p, [&](float& x, float& y) {
            double xx = x, yy = y;
            x = static_cast<float>(xx * c + yy * sn);
            y = static_cast<float>(yy * c - xx * sn);
        });
    });
    return Value();
}
GMFN(path_rescale) {
    float xs = static_cast<float>(D(1)), ys = static_cast<float>(D(2));
    editPath(rt, I(0), [&](Path& p) { pathAbout(p, [&](float& x, float& y) { x *= xs, y *= ys; }); });
    return Value();
}
GMFN(path_reverse) { editPath(rt, I(0), [](Path& p) { std::reverse(p.points.begin(), p.points.end()); }); return Value(); }
GMFN(path_change_point) {  // (path, n, x, y, speed)
    int n = I(1);
    editPath(rt, I(0), [&](Path& p) {
        if (n >= 0 && static_cast<size_t>(n) < p.points.size())
            p.points[n] = {static_cast<float>(D(2)), static_cast<float>(D(3)), static_cast<float>(D(4))};
    });
    return Value();
}
GMFN(path_insert_point) {  // (path, n, x, y, speed)
    int n = I(1);
    editPath(rt, I(0), [&](Path& p) {
        if (n >= 0 && static_cast<size_t>(n) <= p.points.size())
            p.points.insert(p.points.begin() + n, {static_cast<float>(D(2)), static_cast<float>(D(3)), static_cast<float>(D(4))});
    });
    return Value();
}
GMFN(path_delete_point) {
    int n = I(1);
    editPath(rt, I(0), [&](Path& p) {
        if (n >= 0 && static_cast<size_t>(n) < p.points.size()) p.points.erase(p.points.begin() + n);
    });
    return Value();
}
GMFN(path_append) {  // (path, path2): path2's points added to path
    const Path* src = rt.pathAt(I(1));
    if (!src) return Value();
    std::vector<PathPoint> pts = src->points;
    editPath(rt, I(0), [&](Path& p) { p.points.insert(p.points.end(), pts.begin(), pts.end()); });
    return Value();
}
GMFN(path_assign) {  // (target, source): target becomes a copy (keeping its name)
    const Path* src = rt.pathAt(I(1));
    if (!src || I(0) == I(1)) return Value();
    Path copy = *src;
    editPath(rt, I(0), [&](Path& p) {
        copy.name = p.name;
        p = copy;
    });
    return Value();
}
GMFN(path_duplicate) {
    const Path* src = rt.pathAt(I(0));
    if (!src) return Value(-1.0);
    Path copy = *src;
    rt.runtimePaths.push_back(std::move(copy));
    rt.runtimePathLive.push_back(true);
    rt.pathVersion++;
    return Value(static_cast<double>(rt.dw.paths.size() + rt.runtimePaths.size() - 1));
}
GMFN(path_set_precision) {
    int prec = std::clamp(I(1), 1, 8);
    editPath(rt, I(0), [&](Path& p) { p.precision = static_cast<uint32_t>(prec); });
    return Value();
}
GMFN(path_get_precision) { const Path* p = rt.pathAt(I(0)); return Value(p ? static_cast<double>(p->precision) : -1.0); }
GMFN(path_get_kind) { const Path* p = rt.pathAt(I(0)); return Value(p ? (p->smooth ? 1.0 : 0.0) : -1.0); }
GMFN(path_get_closed) { const Path* p = rt.pathAt(I(0)); return boolV(p && p->closed); }
GMFN(path_get_point_speed) {
    const Path* p = rt.pathAt(I(0));
    int n = I(1);
    return Value(p && n >= 0 && static_cast<size_t>(n) < p->points.size() ? p->points[n].speed : 0.0);
}
GMFN(path_get_speed) {
    double x = 0, y = 0, sp = 0;
    pathPoint(rt, I(0), D(1), x, y, sp);
    return Value(sp);
}
// draw_path(path, x, y, absolute): lines through round(length / 4) samples, offset so the path
// starts at (x, y) unless absolute.
GMFN(draw_path) {
    const Path* p = rt.pathAt(I(0));
    if (!p) return Value();
    double x0 = 0, y0 = 0, sp = 0;
    pathPoint(rt, I(0), 0, x0, y0, sp);
    double ox = B(3) ? 0 : I(1) - x0, oy = B(3) ? 0 : I(2) - y0;
    int steps = static_cast<int>(std::round(pathLength(rt, I(0)) / 4.0));
    for (int i = 0; i < steps; i++) {
        double ax, ay, bx, by;
        pathPoint(rt, I(0), static_cast<double>(i) / steps, ax, ay, sp);
        pathPoint(rt, I(0), static_cast<double>(i + 1) / steps, bx, by, sp);
        G.drawLine(std::trunc(ox + ax), std::trunc(oy + ay), std::trunc(ox + bx), std::trunc(oy + by), 1, G.color,
                   G.color, G.alpha);
    }
    return Value();
}

struct MpGrid {
    double left, top;
    int hcells, vcells;
    double cw, ch;
    std::vector<uint8_t> blocked;
};
static std::vector<std::unique_ptr<MpGrid>> g_grids;
static MpGrid* grid(int id) { return id >= 0 && static_cast<size_t>(id) < g_grids.size() ? g_grids[id].get() : nullptr; }
GMFN(mp_grid_create) {
    auto g = std::make_unique<MpGrid>();
    g->left = D(0);
    g->top = D(1);
    g->hcells = std::max(1, I(2));
    g->vcells = std::max(1, I(3));
    g->cw = D(4) > 0 ? D(4) : 1;
    g->ch = D(5) > 0 ? D(5) : 1;
    g->blocked.assign(static_cast<size_t>(g->hcells) * g->vcells, 0);
    g_grids.push_back(std::move(g));
    return Value(static_cast<double>(g_grids.size() - 1));
}
GMFN(mp_grid_destroy) { if (grid(I(0))) g_grids[I(0)].reset(); return Value(); }
static void setCell(MpGrid* g, int x, int y, uint8_t v) {
    if (g && x >= 0 && y >= 0 && x < g->hcells && y < g->vcells) g->blocked[static_cast<size_t>(y) * g->hcells + x] = v;
}
GMFN(mp_grid_add_cell) { setCell(grid(I(0)), I(1), I(2), 1); return Value(); }
GMFN(mp_grid_clear_cell) { setCell(grid(I(0)), I(1), I(2), 0); return Value(); }
GMFN(mp_grid_clear_all) {
    if (MpGrid* g = grid(I(0))) std::fill(g->blocked.begin(), g->blocked.end(), 0);
    return Value();
}
GMFN(mp_grid_get_cell) {
    MpGrid* g = grid(I(0));
    int x = I(1), y = I(2);
    if (!g || x < 0 || y < 0 || x >= g->hcells || y >= g->vcells) return Value(-1.0);
    return Value(g->blocked[static_cast<size_t>(y) * g->hcells + x] ? -1.0 : 0.0);
}
GMFN(mp_grid_add_rectangle) {
    MpGrid* g = grid(I(0));
    if (!g) return Value();
    int x0 = static_cast<int>(std::floor((std::min(D(1), D(3)) - g->left) / g->cw));
    int x1 = static_cast<int>(std::floor((std::max(D(1), D(3)) - g->left) / g->cw));
    int y0 = static_cast<int>(std::floor((std::min(D(2), D(4)) - g->top) / g->ch));
    int y1 = static_cast<int>(std::floor((std::max(D(2), D(4)) - g->top) / g->ch));
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) setCell(g, x, y, 1);
    return Value();
}
GMFN(mp_grid_add_instances) {
    MpGrid* g = grid(I(0));
    if (!g) return Value();
    std::vector<Instance*> list;
    rt.collectTargets(I(1), self, other, list);
    for (Instance* inst : list) {
        Runner::BBox b = rt.bbox(inst);
        if (!b.valid) continue;
        for (int y = 0; y < g->vcells; y++)
            for (int x = 0; x < g->hcells; x++) {
                double cx0 = g->left + x * g->cw, cy0 = g->top + y * g->ch;
                if (rt.rectInInstance(inst, cx0, cy0, cx0 + g->cw - 1, cy0 + g->ch - 1, B(2))) setCell(g, x, y, 1);
            }
    }
    return Value();
}
GMFN(mp_grid_clear_rectangle) {
    MpGrid* g = grid(I(0));
    if (!g) return Value();
    int x0 = static_cast<int>(std::floor((std::min(D(1), D(3)) - g->left) / g->cw));
    int x1 = static_cast<int>(std::floor((std::max(D(1), D(3)) - g->left) / g->cw));
    int y0 = static_cast<int>(std::floor((std::min(D(2), D(4)) - g->top) / g->ch));
    int y1 = static_cast<int>(std::floor((std::max(D(2), D(4)) - g->top) / g->ch));
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) setCell(g, x, y, 0);
    return Value();
}
GMFN(mp_grid_draw) {  // free cells green, blocked cells red, at the draw alpha
    MpGrid* g = grid(I(0));
    if (!g) return Value();
    for (int y = 0; y < g->vcells; y++)
        for (int x = 0; x < g->hcells; x++) {
            uint32_t c = g->blocked[static_cast<size_t>(y) * g->hcells + x] ? 0x0000FF : 0x00FF00;
            double x1 = std::trunc(g->left + x * g->cw), y1 = std::trunc(g->top + y * g->ch);
            G.drawRect(x1, y1, x1 + g->cw - 1, y1 + g->ch - 1, false, c, c, c, c, G.alpha);
        }
    return Value();
}
GMFN(mp_grid_to_ds_grid) {  // (mp_grid, ds_grid): 0 free, -1 blocked
    MpGrid* g = grid(I(0));
    DsGrid* d = rt.ds->grids.get(I(1));
    if (!g || !d) return boolV(false);
    for (int y = 0; y < std::min(g->vcells, d->h); y++)
        for (int x = 0; x < std::min(g->hcells, d->w); x++)
            d->cells[static_cast<size_t>(y) * d->w + x] = Value(g->blocked[static_cast<size_t>(y) * g->hcells + x] ? -1.0 : 0.0);
    return boolV(true);
}

// mp_linear_* / mp_potential_* (YoYo's Function_MotionPlanning): steps towards a goal that only
// move where the instance would be free of solid instances (all instances with checkall, or
// of one object for the _object variants).
static struct { double maxRot = 30, step = 10, ahead = 3; bool onSpot = true; } g_mpPot;
static bool mpFree(Runner& rt, Instance* self, Instance* other, double x, double y, int obj, bool checkAll) {
    if (obj == INST_ALL) {
        bool free = true;
        rt.forEachInstanceOf(-1, [&](Instance* i) {
            if (free && i != self && (checkAll || i->solid) && rt.instancesCollide(self, x, y, i)) free = false;
        });
        return free;
    }
    std::vector<Instance*> list;
    rt.collectTargets(obj, self, other, list);
    for (Instance* i : list)
        if (i != self && rt.instancesCollide(self, x, y, i)) return false;
    return true;
}
static double pointDir(double x1, double y1, double x2, double y2) {
    double d = std::atan2(-(y2 - y1), x2 - x1) * 180 / M_PI;
    return d < 0 ? d + 360 : d;
}
static bool mpLinearStep(Runner& rt, Instance* self, Instance* other, double gx, double gy, double speed, int obj, bool all) {
    if (self->x == gx && self->y == gy) return true;
    double dist = std::hypot(self->x - gx, self->y - gy), nx = gx, ny = gy;
    bool reached = dist <= speed;
    if (!reached) {
        nx = self->x + speed * (gx - self->x) / dist;
        ny = self->y + speed * (gy - self->y) / dist;
    }
    if (!mpFree(rt, self, other, nx, ny, obj, all)) return reached;
    self->setSpeedDir(self->speed, pointDir(self->x, self->y, nx, ny));
    self->x = nx;
    self->y = ny;
    return reached;
}
static double diffDir(double a, double b) {
    a = std::fmod(a, 360.0);
    if (a <= 0) a += 360;
    b = std::fmod(b, 360.0);
    if (b < 0) b += 360;
    double r = std::fabs(b - a);
    return r > 180 ? 360 - r : r;
}
static bool mpTryDir(Runner& rt, Instance* self, Instance* other, double dir, double speed, int obj, bool all) {
    if (diffDir(dir, self->direction) > g_mpPot.maxRot) return false;
    double c = std::cos(dir * M_PI / 180), sn = std::sin(dir * M_PI / 180);
    if (!mpFree(rt, self, other, self->x + speed * g_mpPot.ahead * c, self->y - speed * g_mpPot.ahead * sn, obj, all)) return false;
    double nx = self->x + speed * c, ny = self->y - speed * sn;
    if (!mpFree(rt, self, other, nx, ny, obj, all)) return false;
    self->setSpeedDir(self->speed, dir);
    self->x = nx;
    self->y = ny;
    return true;
}
static bool mpPotentialStep(Runner& rt, Instance* self, Instance* other, double gx, double gy, double speed, int obj, bool all) {
    if (self->x == gx && self->y == gy) return true;
    if (std::hypot(self->x - gx, self->y - gy) <= speed) {
        if (mpFree(rt, self, other, gx, gy, obj, all)) {
            self->setSpeedDir(self->speed, pointDir(self->x, self->y, gx, gy));
            self->x = gx;
            self->y = gy;
        }
        return true;
    }
    double goal = pointDir(self->x, self->y, gx, gy);
    for (double cur = 0; cur < 180; cur += g_mpPot.step)
        if (mpTryDir(rt, self, other, goal - cur, speed, obj, all) || mpTryDir(rt, self, other, goal + cur, speed, obj, all))
            return false;
    if (g_mpPot.onSpot) self->setSpeedDir(self->speed, self->direction + g_mpPot.maxRot);
    return false;
}
// Runs the steps from the instance's position into a path, then puts the instance back.
template <typename Step>
static bool mpPathFrom(Runner& rt, Instance* self, int pathId, double gx, double gy, double maxLen, Step step) {
    Path* path = rtPath(rt, pathId);
    if (!path || !self) return false;
    double x0 = self->x, y0 = self->y, d0 = self->direction, sp0 = self->speed;
    path->points.clear();
    path->smooth = false;
    path->closed = false;
    path->points.push_back({static_cast<float>(x0), static_cast<float>(y0), 100});
    bool ok = true;
    double len = 0, lastLen = 0;
    int still = 0;
    while (true) {
        double ox = self->x, oy = self->y;
        if (step()) break;
        if (maxLen < 0 && self->x == ox && self->y == oy) {  // linear: blocked
            ok = false;
            break;
        }
        len += std::hypot(self->x - ox, self->y - oy);
        path->points.push_back({static_cast<float>(self->x), static_cast<float>(self->y), 100});
        if (maxLen >= 0) {  // potential: give up past the length limit or after 100 steps in place
            if (len > maxLen) {
                ok = false;
                break;
            }
            still = len == lastLen ? still + 1 : 0;
            if (still > 100) {
                ok = false;
                break;
            }
            lastLen = len;
        }
    }
    if (ok) path->points.push_back({static_cast<float>(gx), static_cast<float>(gy), 100});
    self->x = x0;
    self->y = y0;
    self->setSpeedDir(sp0, d0);
    rt.pathVersion++;
    return ok;
}
GMFN(mp_linear_step) { return boolV(self && mpLinearStep(rt, self, other, D(0), D(1), D(2), INST_ALL, B(3))); }
GMFN(mp_linear_step_object) { return boolV(self && mpLinearStep(rt, self, other, D(0), D(1), D(2), I(3), true)); }
GMFN(mp_potential_step) { return boolV(self && mpPotentialStep(rt, self, other, D(0), D(1), D(2), INST_ALL, B(3))); }
GMFN(mp_potential_step_object) { return boolV(self && mpPotentialStep(rt, self, other, D(0), D(1), D(2), I(3), true)); }
GMFN(mp_potential_settings) {
    g_mpPot.maxRot = std::max(1.0, D(0));
    g_mpPot.step = std::max(1.0, D(1));
    g_mpPot.ahead = std::max(1.0, D(2));
    g_mpPot.onSpot = B(3);
    return Value();
}
static Value mpLinearPath(Runner& rt, Instance* self, Instance* other, Value* args, int argc, int obj, bool all) {
    double gx = D(1), gy = D(2), speed = D(3);
    if (speed <= 0) return boolV(false);
    return boolV(mpPathFrom(rt, self, I(0), gx, gy, -1, [&] { return mpLinearStep(rt, self, other, gx, gy, speed, obj, all); }));
}
static Value mpPotentialPath(Runner& rt, Instance* self, Instance* other, Value* args, int argc, int obj, bool all) {
    double gx = D(1), gy = D(2), speed = D(3), factor = D(4);
    if (!self || speed <= 0 || factor < 1) return boolV(false);
    double maxLen = std::hypot(gx - self->x, gy - self->y) * factor;
    return boolV(mpPathFrom(rt, self, I(0), gx, gy, maxLen, [&] { return mpPotentialStep(rt, self, other, gx, gy, speed, obj, all); }));
}
GMFN(mp_linear_path) { return mpLinearPath(rt, self, other, args, argc, INST_ALL, B(4)); }
GMFN(mp_linear_path_object) { return mpLinearPath(rt, self, other, args, argc, I(4), true); }
GMFN(mp_potential_path) { return mpPotentialPath(rt, self, other, args, argc, INST_ALL, B(5)); }
GMFN(mp_potential_path_object) { return mpPotentialPath(rt, self, other, args, argc, I(5), true); }
// A* over the grid; diagonal moves can't cut past blocked corners.
GMFN(mp_grid_path) {
    MpGrid* g = grid(I(0));
    Path* path = rtPath(rt, I(1));
    if (!g || !path) return boolV(false);
    double sx = D(2), sy = D(3), gx = D(4), gy = D(5);
    bool diag = B(6);
    auto cellOf = [&](double x, double y, int& cx, int& cy) {
        cx = static_cast<int>(std::floor((x - g->left) / g->cw));
        cy = static_cast<int>(std::floor((y - g->top) / g->ch));
        return cx >= 0 && cy >= 0 && cx < g->hcells && cy < g->vcells;
    };
    int scx, scy, gcx, gcy;
    if (!cellOf(sx, sy, scx, scy) || !cellOf(gx, gy, gcx, gcy)) return boolV(false);
    auto blocked = [&](int x, int y) {
        return x < 0 || y < 0 || x >= g->hcells || y >= g->vcells || g->blocked[static_cast<size_t>(y) * g->hcells + x];
    };
    if (blocked(scx, scy) || blocked(gcx, gcy)) return boolV(false);
    size_t n = static_cast<size_t>(g->hcells) * g->vcells;
    std::vector<double> cost(n, 1e300);
    std::vector<int> from(n, -1);
    using Node = std::pair<double, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    auto idx = [&](int x, int y) { return y * g->hcells + x; };
    auto h = [&](int x, int y) { return std::hypot(x - gcx, y - gcy); };
    cost[idx(scx, scy)] = 0;
    open.push({h(scx, scy), idx(scx, scy)});
    bool found = false;
    while (!open.empty()) {
        int cur = open.top().second;
        open.pop();
        int cx = cur % g->hcells, cy = cur / g->hcells;
        if (cx == gcx && cy == gcy) {
            found = true;
            break;
        }
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                if (dx && dy && (!diag || blocked(cx + dx, cy) || blocked(cx, cy + dy))) continue;
                int nx = cx + dx, ny = cy + dy;
                if (blocked(nx, ny)) continue;
                double nc = cost[cur] + (dx && dy ? M_SQRT2 : 1.0);
                int ni = idx(nx, ny);
                if (nc < cost[ni]) {
                    cost[ni] = nc;
                    from[ni] = cur;
                    open.push({nc + h(nx, ny), ni});
                }
            }
    }
    if (!found) return boolV(false);
    std::vector<int> cells;
    for (int c = idx(gcx, gcy); c != -1; c = from[c]) cells.push_back(c);
    std::reverse(cells.begin(), cells.end());
    path->points.clear();
    path->points.push_back({static_cast<float>(sx), static_cast<float>(sy), 100});
    for (size_t i = 1; i + 1 < cells.size(); i++) {
        int cx = cells[i] % g->hcells, cy = cells[i] / g->hcells;
        path->points.push_back({static_cast<float>(g->left + (cx + 0.5) * g->cw),
                                static_cast<float>(g->top + (cy + 0.5) * g->ch), 100});
    }
    path->points.push_back({static_cast<float>(gx), static_cast<float>(gy), 100});
    path->smooth = false;
    path->closed = false;
    rt.pathVersion++;
    return boolV(true);
}


// ---------------------------------------------------------------------------------------------
// Misc

GMFN(audio_group_stop_all) {
    int group = I(0);
    for (size_t s = 0; s < rt.dw.sounds.size(); s++)
        if (rt.dw.sounds[s].group == group) rt.audio->stop(static_cast<int>(s));
    return Value();
}
GMFN(file_copy) {
    std::ifstream in(rt.files->readPath(S(0)), std::ios::binary);
    if (!in) return Value();
    std::ofstream out(rt.files->writePath(S(1)), std::ios::binary);
    out << in.rdbuf();
    return Value();
}
GMFN(string_trim) {
    std::string s = S(0);
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return Value(a == std::string::npos ? std::string() : s.substr(a, b - a + 1));
}
GMFN(string_starts_with) { std::string s = S(0), p = S(1); return boolV(s.compare(0, p.size(), p) == 0); }
GMFN(string_ends_with) {
    std::string s = S(0), p = S(1);
    return boolV(s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0);
}
GMFN(string_split) {
    std::string s = S(0), d = S(1);
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    if (d.empty()) {
        a->rows[0].push_back(Value(s));
        return arr;
    }
    size_t p = 0;
    for (;;) {
        size_t q = s.find(d, p);
        a->rows[0].push_back(Value(s.substr(p, q == std::string::npos ? std::string::npos : q - p)));
        if (q == std::string::npos) break;
        p = q + d.size();
    }
    return arr;
}
GMFN(zero_fn) { return Value(0.0); }
GMFN(false_fn) { return boolV(false); }

void registerGms2Builtins(Runner& rt) {
    rt.vm.registerBuiltin("@@This@@", fn_gml_this);
    rt.vm.registerBuiltin("@@Other@@", fn_gml_other);
    rt.vm.registerBuiltin("@@Global@@", fn_gml_global);
    rt.vm.registerBuiltin("@@NewGMLArray@@", fn_gml_new_array);
    rt.vm.registerBuiltin("@@SetStatic@@", fn_gml_set_static);
    // @@string@@(format, args...): the 2024+ compilers' name for string() with {0} placeholders
    if (BuiltinFn str = rt.vm.findBuiltin("string")) rt.vm.registerBuiltin("@@string@@", str);
    GMREG(variable_struct_set_post); GMREG(array_set_post);
    rt.vm.registerBuiltin("@@NewGMLObject@@", fn_gml_new_object);
    rt.vm.registerBuiltin("@@NullObject@@", fn_gml_null_object);
    rt.vm.registerBuiltin("@@throw@@", fn_gml_throw);
    rt.vm.registerBuiltin("@@CopyStatic@@", fn_gml_copy_static);
    rt.vm.registerBuiltin("@@try_hook@@", fn_gml_try_hook);
    rt.vm.registerBuiltin("@@try_unhook@@", fn_gml_try_unhook);
    rt.vm.registerBuiltin("@@finish_catch@@", fn_gml_noop);
    rt.vm.registerBuiltin("@@finish_finally@@", fn_gml_finish_finally);
    GMREG(method); GMREG(method_get_self); GMREG(method_get_index); GMREG(is_method); GMREG(is_struct); GMREG(bool);
    rt.vm.registerBuiltin("@@array_get@@", fn_gml_array_get);
    GMREG(is_callable); GMREG(is_numeric); GMREG(is_nan); GMREG(is_infinity); GMREG(typeof); GMREG(script_execute);
    GMREG(script_execute_ext); GMREG(variable_get_hash); GMREG(instanceof); GMREG(is_instanceof); GMREG(static_get);
    GMREG(static_set); GMREG(struct_get_from_hash);
    GMREG(struct_set_from_hash); GMREG(struct_exists_from_hash); GMREG(struct_remove_from_hash); GMREG(method_call); GMREG(variable_clone); GMREG(handle_parse);
    GMREG(variable_struct_get); GMREG(variable_struct_set); GMREG(variable_struct_exists);
    GMREG(variable_struct_remove); GMREG(variable_struct_get_names); GMREG(variable_struct_names_count);
    GMALIAS(struct_get, variable_struct_get); GMALIAS(struct_set, variable_struct_set);
    GMALIAS(struct_exists, variable_struct_exists); GMALIAS(struct_remove, variable_struct_remove);
    GMALIAS(struct_get_names, variable_struct_get_names); GMALIAS(struct_names_count, variable_struct_names_count);
    GMREG(array_length); GMREG(array_get); GMREG(array_set); GMREG(array_push); GMREG(array_pop); GMREG(array_insert);
    GMREG(array_delete); GMREG(array_resize); GMREG(array_copy); GMREG(array_equals); GMREG(array_contains);
    GMREG(array_sort); GMREG(array_first); GMREG(array_last);
    GMREG(instance_create_depth); GMREG(instance_create_layer);
    GMREG(gpu_set_fog); GMREG(gpu_set_blendmode); GMREG(gpu_set_blendmode_ext); GMREG(gpu_set_blendenable);
    GMREG(gpu_set_colorwriteenable); GMALIAS(gpu_set_colourwriteenable, gpu_set_colorwriteenable); GMREG(gpu_set_alphatestenable); GMREG(gpu_set_alphatestref);
    GMREG(gpu_set_texfilter);
    GMREG(matrix_build_identity); GMREG(matrix_build_lookat); GMREG(matrix_build_projection_ortho);
    GMREG(camera_create); GMREG(camera_create_view); GMREG(camera_destroy); GMREG(camera_get_active); GMREG(camera_get_default); GMREG(camera_set_default);
    GMREG(room_get_camera); GMREG(room_set_camera); GMREG(camera_get_view_x);
    GMREG(camera_get_view_y); GMREG(camera_get_view_width); GMREG(camera_get_view_height);
    GMREG(camera_get_view_angle); GMREG(camera_get_view_target); GMREG(camera_set_view_pos);
    GMREG(camera_set_view_size); GMREG(camera_set_view_angle); GMREG(camera_set_view_target);
    GMREG(camera_set_view_border); GMREG(camera_set_view_speed); GMREG(camera_set_view_mat);
    GMREG(camera_set_proj_mat); GMREG(view_get_camera); GMREG(view_set_camera); GMREG(view_get_visible);
    GMREG(view_set_visible); GMREG(view_get_wport); GMREG(view_get_hport); GMREG(view_set_wport);
    GMREG(view_set_hport); GMREG(view_get_xport); GMREG(view_get_yport); GMREG(view_set_xport);
    GMREG(view_set_yport); GMREG(view_get_surface_id); GMREG(view_set_surface_id); GMREG(camera_get_view_border_x);
    GMREG(camera_get_view_border_y); GMREG(camera_get_view_speed_x); GMREG(camera_get_view_speed_y);
    GMREG(game_set_speed); GMREG(game_get_speed); GMREG(asset_get_index); GMREG(asset_get_type); GMREG(font_get_size);
    GMREG(draw_text_transformed_color); GMALIAS(draw_text_transformed_colour, draw_text_transformed_color);
    GMREG(draw_text_ext_transformed_color); GMALIAS(draw_text_ext_transformed_colour, draw_text_ext_transformed_color);
    GMREG(draw_sprite_tiled); GMREG(draw_sprite_tiled_ext); GMREG(draw_triangle_color);
    GMALIAS(draw_triangle_colour, draw_triangle_color);
    GMREG(collision_circle_list); GMREG(collision_rectangle_list); GMREG(collision_point_list);
    GMREG(instance_place_list); GMREG(collision_line_list); GMREG(collision_ellipse_list);
    GMREG(instance_position_list); GMALIAS(variable_instance_get_names, variable_struct_get_names);
    GMREG(ds_exists); GMREG(ds_grid_add); GMREG(ds_grid_set_region); GMREG(ds_list_write);
    GMREG(ds_list_read); GMREG(ds_grid_write); GMREG(ds_grid_read); GMREG(ds_map_write); GMREG(ds_map_read);
    GMREG(ds_map_secure_save); GMREG(ds_map_secure_load);

    GMREG(path_add); GMREG(path_delete); GMREG(path_add_point); GMREG(path_clear_points); GMREG(path_set_closed);
    GMREG(path_set_kind); GMREG(path_get_number); GMREG(path_get_point_x); GMREG(path_get_point_y);
    GMREG(path_shift); GMREG(path_mirror); GMREG(path_flip); GMREG(path_rotate); GMREG(path_rescale);
    GMREG(path_reverse); GMREG(path_change_point); GMREG(path_insert_point); GMREG(path_delete_point);
    GMREG(path_append); GMREG(path_assign); GMREG(path_duplicate); GMREG(path_set_precision);
    GMREG(path_get_precision); GMREG(path_get_kind); GMREG(path_get_closed); GMREG(path_get_point_speed);
    GMREG(path_get_speed); GMREG(draw_path);
    GMREG(mp_grid_create); GMREG(mp_grid_clear_rectangle); GMREG(mp_grid_draw); GMREG(mp_grid_to_ds_grid);
    GMREG(mp_linear_step); GMREG(mp_linear_step_object); GMREG(mp_potential_step); GMREG(mp_potential_step_object);
    GMREG(mp_potential_settings); GMREG(mp_linear_path); GMREG(mp_linear_path_object); GMREG(mp_potential_path);
    GMREG(mp_potential_path_object); GMREG(mp_grid_destroy); GMREG(mp_grid_add_cell); GMREG(mp_grid_clear_cell);
    GMREG(mp_grid_clear_all); GMREG(mp_grid_get_cell); GMREG(mp_grid_add_rectangle); GMREG(mp_grid_add_instances);
    GMREG(mp_grid_path);
   
   
   
    GMREG(audio_group_stop_all); GMREG(file_copy); GMREG(string_trim); GMREG(string_starts_with);
    GMREG(string_ends_with); GMREG(string_split);
    for (const char* n : {"steam_update", "steam_shutdown", "ds_grid_set_post", "font_enable_effects", "gc_collect",
                          "font_enable_sdf", "font_sdf_spread"})
        rt.vm.registerBuiltin(n, fn_gml_noop);
    // Texture pages are always resident: prefetching and flushing succeed (0).
    for (const char* n : {"gamepad_hat_value", "gamepad_hat_count", "sprite_prefetch", "sprite_prefetch_multi",
                          "texture_prefetch", "sprite_flush", "sprite_flush_multi", "texture_flush"})
        rt.vm.registerBuiltin(n, fn_zero_fn);
    for (const char* n : {"steam_utils_is_steam_running_on_steam_deck", "steam_is_cloud_enabled_for_app"})
        rt.vm.registerBuiltin(n, fn_false_fn);
}

}  // namespace gm
