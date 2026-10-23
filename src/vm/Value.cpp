#include "Value.h"

#include <cmath>
#include <cstdio>

#include "runtime/Instance.h"

namespace gm {

static const std::string kEmpty;

Value::Value(std::string_view s) : t_(VT::String) {
    u_.s = new GMString{1, std::string(s)};
}

Value Value::fromString(GMString* s) {
    Value v;
    v.t_ = VT::String;
    v.u_.s = s;
    s->rc++;
    return v;
}

Value Value::newArray() {
    Value v;
    v.t_ = VT::Array;
    v.u_.a = new GMArray();
    v.u_.a->rc = 1;
    v.u_.a->owner = ArrayOwnership::current;
    return v;
}

Value Value::fromArray(GMArray* a) {
    Value v;
    v.t_ = VT::Array;
    v.u_.a = a;
    a->rc++;
    return v;
}

Value Value::fromObject(GMObject* o) {
    Value v;
    v.t_ = VT::Object;
    v.u_.o = o;
    o->rc++;
    return v;
}

Value Value::fromMethod(GMMethod* m) {
    Value v;
    v.t_ = VT::Method;
    v.u_.m = m;
    m->rc++;
    return v;
}

GMObject::~GMObject() {
    if (alive) *alive = false;
    delete inst;
}

static_assert(static_cast<int>(VT::String) == 1 && static_cast<int>(VT::Array) == 2 &&
                  static_cast<int>(VT::Object) == 8 && static_cast<int>(VT::Method) == 9,
              "Value::counted() assumes these type numbers");

void Value::retainCounted() const {
    switch (t_) {
        case VT::String: u_.s->rc++; break;
        case VT::Array: u_.a->rc++; break;
        case VT::Object: u_.o->rc++; break;
        case VT::Method: u_.m->rc++; break;
        default: break;
    }
}

void Value::releaseCounted() {
    switch (t_) {
        case VT::String:
            if (--u_.s->rc == 0) delete u_.s;
            break;
        case VT::Array:
            if (--u_.a->rc == 0) delete u_.a;
            break;
        case VT::Object:
            if (--u_.o->rc == 0) delete u_.o;
            break;
        case VT::Method:
            if (--u_.m->rc == 0) delete u_.m;
            break;
        default: break;
    }
    t_ = VT::Undefined;
}

int64_t Value::toInt64() const {
    switch (t_) {
        case VT::Real: {
            double d = u_.r;
            if (!std::isfinite(d)) return 0;
            // Snap values that are within float noise of an integer (e.g. 2.9999999) before truncating.
            double r = std::round(d);
            if (std::fabs(d - r) < 1e-7) return static_cast<int64_t>(r);
            return static_cast<int64_t>(d);
        }
        case VT::Int32: case VT::Int64: case VT::Bool: return u_.i;
        default: return 0;
    }
}

const std::string& Value::str() const { return t_ == VT::String ? u_.s->s : kEmpty; }

std::string formatReal(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    char buf[64];
    double r = std::round(d);
    if (std::fabs(d - r) < 1e-9 && std::fabs(r) < 1e15) {
        snprintf(buf, sizeof buf, "%.0f", r == 0 ? 0.0 : r);
    } else {
        snprintf(buf, sizeof buf, "%.2f", d);
        if (std::string(buf) == "-0.00") return "0.00";
    }
    return buf;
}

std::string_view (*gVarNameForString)(int32_t id) = nullptr;
bool gGms2Strings = false;

// GML text for a value inside an array or struct: strings are quoted there.
static std::string nestedString(const Value& v, int depth);

static std::string containerString(const Value& v, int depth) {
    if (depth > 16) return "...";
    if (GMArray* a = v.arr()) {
        std::string out = "[ ";
        if (!a->rows.empty()) {
            auto& row = a->rows[0];
            for (size_t i = 0; i < row.size(); i++) out += (i ? "," : "") + nestedString(row[i], depth + 1);
        }
        return out + " ]";
    }
    GMObject* o = v.obj();
    std::string out = "{ ";
    bool first = true;
    o->inst->vars.forEachInOrder([&](int32_t id, const Value& x) {
        if (!first) out += ", ";
        first = false;
        out += gVarNameForString ? std::string(gVarNameForString(id)) : std::to_string(id);
        out += " : " + nestedString(x, depth + 1);
    });
    return out + (first ? "}" : " }");
}

static std::string nestedString(const Value& v, int depth) {
    if (v.isString()) return "\"" + v.str() + "\"";
    if (v.isArray() || v.isObject()) return containerString(v, depth);
    return v.toString();
}

std::string Value::toString() const {
    switch (t_) {
        case VT::Real: return formatReal(u_.r);
        case VT::String: return u_.s->s;
        case VT::Int32: case VT::Int64: return std::to_string(u_.i);
        case VT::Bool: return gGms2Strings ? (u_.i ? "true" : "false") : std::to_string(u_.i);
        case VT::Undefined: return "undefined";
        case VT::Array: case VT::Object: return containerString(*this, 0);
        case VT::Method: return "function";
        case VT::Ptr: {
            char buf[32];
            snprintf(buf, sizeof buf, "%p", u_.p);
            return buf;
        }
    }
    return "";
}

std::string Value::repr() const {
    if (t_ == VT::String) return "\"" + u_.s->s + "\"";
    if (t_ == VT::Array) {
        std::string s = "[";
        if (!u_.a->rows.empty()) {
            auto& row = u_.a->rows[0];
            for (size_t i = 0; i < row.size() && i < 16; i++) s += (i ? "," : "") + row[i].repr();
            if (row.size() > 16) s += ",...";
        }
        return s + "]";
    }
    return toString();
}

GMArray* Value::writableArray() {
    if (t_ != VT::Array) {
        *this = newArray();
        return u_.a;
    }
    bool copy = false;
    switch (ArrayOwnership::mode) {
        case ArrayCopy::WhenShared: copy = u_.a->rc > 1; break;
        case ArrayCopy::OtherOwner: copy = u_.a->rc > 1 && u_.a->owner != ArrayOwnership::current; break;
        case ArrayCopy::Never: break;
    }
    if (copy) {
        GMArray* c = new GMArray();
        c->rc = 1;
        c->rows = u_.a->rows;
        u_.a->rc--;
        u_.a = c;
    }
    u_.a->owner = ArrayOwnership::current;
    return u_.a;
}

Value GMArray::get(int64_t index) const {
    if (index < 0) return Value();
    size_t row = static_cast<size_t>(index / kArrayRowStride);
    size_t col = static_cast<size_t>(index % kArrayRowStride);
    if (row >= rows.size() || col >= rows[row].size()) return Value();
    return rows[row][col];
}

Value& GMArray::at(int64_t index) {
    if (index < 0) index = 0;
    size_t row = static_cast<size_t>(index / kArrayRowStride);
    size_t col = static_cast<size_t>(index % kArrayRowStride);
    if (row >= rows.size()) rows.resize(row + 1);
    auto& r = rows[row];
    if (col >= r.size()) r.resize(col + 1, Value(0.0));
    return r[col];
}

void GMArray::set(int64_t index, Value v) {
    if (index < 0) return;
    size_t row = static_cast<size_t>(index / kArrayRowStride);
    size_t col = static_cast<size_t>(index % kArrayRowStride);
    if (row >= rows.size()) rows.resize(row + 1);
    auto& r = rows[row];
    if (col >= r.size()) r.resize(col + 1, Value(0.0));  // GML pads new array slots with 0
    r[col] = std::move(v);
}

}  // namespace gm
