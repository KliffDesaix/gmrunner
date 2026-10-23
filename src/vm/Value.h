// GML runtime values.
//
// GMS 1.4 semantics: reals are doubles, strings are immutable values, arrays are
// reference types with copy-on-write when written through a second reference.
// Strings and arrays are intrusively ref-counted so a Value stays 16 bytes.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gm {

enum class VT : uint8_t { Real, String, Array, Undefined, Int32, Int64, Bool, Ptr, Object, Method };

struct GMString;
struct GMArray;
struct GMObject;  // GMS 2.3 struct
struct GMMethod;  // GMS 2.3 function value / bound method
struct Instance;

class Value {
public:
    Value() : t_(VT::Undefined) { u_.i = 0; }
    Value(double d) : t_(VT::Real) { u_.r = d; }
    Value(int v) : t_(VT::Real) { u_.r = v; }
    Value(std::string_view s);
    Value(const std::string& s) : Value(std::string_view(s)) {}
    Value(const char* s) : Value(std::string_view(s)) {}

    static Value i32(int32_t v) { Value x; x.t_ = VT::Int32; x.u_.i = v; return x; }
    static Value i64(int64_t v) { Value x; x.t_ = VT::Int64; x.u_.i = v; return x; }
    static Value boolean(bool v) { Value x; x.t_ = VT::Bool; x.u_.i = v ? 1 : 0; return x; }
    static Value ptr(void* p) { Value x; x.t_ = VT::Ptr; x.u_.p = p; return x; }
    static Value fromString(GMString* s);  // takes a new reference
    static Value newArray();
    static Value fromArray(GMArray* a);  // shares the array (adds a reference)
    static Value fromObject(GMObject* o);  // takes a new reference
    static Value fromMethod(GMMethod* m);  // takes a new reference

    Value(const Value& o) : t_(o.t_), u_(o.u_) { retain(); }
    Value(Value&& o) noexcept : t_(o.t_), u_(o.u_) { o.t_ = VT::Undefined; }
    Value& operator=(const Value& o) {
        if (this != &o) {
            o.retain();
            release();
            t_ = o.t_;
            u_ = o.u_;
        }
        return *this;
    }
    Value& operator=(Value&& o) noexcept {
        if (this != &o) {
            release();
            t_ = o.t_;
            u_ = o.u_;
            o.t_ = VT::Undefined;
        }
        return *this;
    }
    ~Value() { release(); }

    VT type() const { return t_; }
    bool isString() const { return t_ == VT::String; }
    bool isArray() const { return t_ == VT::Array; }
    bool isUndefined() const { return t_ == VT::Undefined; }
    bool isReal() const { return t_ == VT::Real; }
    bool isNumeric() const { return t_ == VT::Real || t_ == VT::Int32 || t_ == VT::Int64 || t_ == VT::Bool; }
    bool isIntegral() const { return t_ == VT::Int32 || t_ == VT::Int64 || t_ == VT::Bool; }

    double num() const {
        switch (t_) {
            case VT::Real: return u_.r;
            case VT::Int32: case VT::Int64: case VT::Bool: return static_cast<double>(u_.i);
            default: return 0.0;
        }
    }
    int64_t toInt64() const;
    int32_t toInt() const { return static_cast<int32_t>(toInt64()); }
    bool truthy() const {
        switch (t_) {
            case VT::Real: return u_.r > 0.5;
            case VT::Int32: case VT::Int64: case VT::Bool: return u_.i > 0;
            case VT::Ptr: return u_.p != nullptr;
            case VT::Object: case VT::Method: return true;
            default: return false;
        }
    }
    const std::string& str() const;  // empty string unless isString()
    GMArray* arr() const { return t_ == VT::Array ? u_.a : nullptr; }
    GMObject* obj() const { return t_ == VT::Object ? u_.o : nullptr; }
    GMMethod* method() const { return t_ == VT::Method ? u_.m : nullptr; }
    bool isObject() const { return t_ == VT::Object; }
    bool isMethod() const { return t_ == VT::Method; }
    void* rawPtr() const { return u_.p; }
    int64_t rawInt() const { return u_.i; }

    // GML string() conversion.
    std::string toString() const;
    // Debug representation (strings quoted).
    std::string repr() const;

    // Makes this value's array uniquely owned (copy-on-write) and returns it.
    // Turns a non-array into a fresh empty array.
    GMArray* writableArray();

private:
    // Only strings, arrays, structs and methods are reference counted; the check is inline
    // because values are copied and destroyed constantly (most of them numbers).
    static bool counted(VT t) { return (0x306u >> static_cast<unsigned>(t)) & 1u; }  // 1, 2, 8, 9
    void retain() const {
        if (counted(t_)) retainCounted();
    }
    void release() {
        if (counted(t_)) releaseCounted();
        t_ = VT::Undefined;
    }
    void retainCounted() const;
    void releaseCounted();

    VT t_;
    union U {
        double r;
        int64_t i;
        GMString* s;
        GMArray* a;
        GMObject* o;
        GMMethod* m;
        void* p;
    } u_;
};

struct GMString {
    int32_t rc = 0;
    std::string s;
};

// When a write through one reference copies a shared array (copy-on-write) differs by version:
// GameMaker Studio 1.4 copies an array other references share; GameMaker Studio 2 copies only
// with the project's "copy on write" option, and then only when the writing code isn't the
// array's owner (the compiler's setowner names the running function; YoYo's yyVariable.js
// array_check); with the option off arrays are never copied.
enum class ArrayCopy : uint8_t { WhenShared, OtherOwner, Never };
struct ArrayOwnership {
    static inline ArrayCopy mode = ArrayCopy::WhenShared;
    static inline int64_t current = 0;  // setowner's value: the code writing arrays now
};

struct GMArray {
    int32_t rc = 0;
    int64_t owner = 0;  // ArrayCopy::OtherOwner: the code that made or last wrote it
    std::vector<std::vector<Value>> rows;

    Value get(int64_t index) const;  // flattened 1.4 index: row * 32000 + col
    void set(int64_t index, Value v);
    Value& at(int64_t index);  // element storage, padded with 0 like set()
    int64_t length1d() const { return rows.empty() ? 0 : static_cast<int64_t>(rows[0].size()); }
};

// A struct: its variables live in a heap Instance that isn't part of any room.
// weak_ref_create: the referenced struct (or instance id) and a flag cleared when it is freed.
struct GMWeakRef {
    std::shared_ptr<bool> targetAlive;
    GMObject* target = nullptr;  // valid while *targetAlive (not a counted reference)
    int32_t instanceId = -1;
};

struct GMObject {
    int32_t rc = 0;
    Instance* inst = nullptr;
    std::shared_ptr<bool> alive;         // created on demand for weak references to this struct
    std::unique_ptr<GMWeakRef> weakRef;  // set when this struct is a weak reference
    ~GMObject();
};

// A function value: a script/code entry or a built-in, optionally bound to a `self`.
struct GMMethod {
    int32_t rc = 0;
    int32_t code = -1;     // code entry to run, or -1
    int32_t builtin = -1;  // FUNC index of a built-in, or -1
    int32_t script = -1;   // script asset index (for script_get_name / comparisons)
    Value self;            // bound struct / instance id; undefined = use the caller's self
};

constexpr int64_t kArrayRowStride = 32000;

std::string formatReal(double d);

// How values turn into text in GML (string(), drawing, concatenation). Set by the VM:
// variable names for struct output, and GMS 2.3+ rules (booleans print as true/false).
extern std::string_view (*gVarNameForString)(int32_t id);
extern bool gGms2Strings;

}  // namespace gm
