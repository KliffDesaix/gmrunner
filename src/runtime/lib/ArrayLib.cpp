// GameMaker 2023 array, struct and string helpers (array_map, array_filter, string_join, ...),
// with GameMaker's optional offset/length arguments: a negative offset counts from the end and a
// negative length walks backwards (as YoYo's runner, computeIterationValues).
#include <algorithm>
#include <cmath>

#include "runtime/Builtins.h"
#include "util/Utf8.h"

namespace gm {
namespace {

struct Span {
    int64_t offset = 0, loops = 0, step = 1;
};

// The elements an (offset, length) pair selects in an array of size n.
Span span(int64_t n, Value* args, int argc, int offsetArg) {
    Span sp;
    if (n <= 0) return sp;
    double rawOffset = argc > offsetArg && !args[offsetArg].isUndefined() ? argNum(args[offsetArg]) : 0;
    double rawLength = argc > offsetArg + 1 ? argNum(args[offsetArg + 1]) : static_cast<double>(n);
    int64_t off = static_cast<int64_t>(std::min(std::max(rawOffset, -static_cast<double>(n)), static_cast<double>(n - 1)));
    if (off < 0) off += n;
    sp.offset = off;
    if (rawLength < 0) {
        sp.step = -1;
        sp.loops = std::min<int64_t>(off + 1, static_cast<int64_t>(std::fabs(rawLength)));
    } else {
        sp.step = 1;
        sp.loops = std::max<int64_t>(0, std::min<int64_t>(off + static_cast<int64_t>(rawLength), n) - off);
    }
    return sp;
}

std::vector<Value>* rowOf(const Value& v) {
    GMArray* a = v.arr();
    if (!a) return nullptr;
    if (a->rows.empty()) a->rows.resize(1);
    return &a->rows[0];
}

Value newArray(std::vector<Value> items) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    a->rows[0] = std::move(items);
    return arr;
}

// Writable element storage of an array argument (shared array, edited in place).
std::vector<Value>* writableRow(Value& v) {
    GMArray* a = v.arr();
    if (!a) return nullptr;
    if (a->rows.empty()) a->rows.resize(1);
    return &a->rows[0];
}

Value call(Runner& rt, Instance* self, Instance* other, const Value& fn, std::initializer_list<Value> list) {
    std::vector<Value> args(list);
    return rt.vm.callValue(fn, self, other, static_cast<int>(args.size()), args.data());
}

// Same value for array_unique / array_union / array_intersection (JavaScript Set semantics).
bool sameValue(const Value& a, const Value& b) {
    if (a.isString() || b.isString()) return a.isString() && b.isString() && a.str() == b.str();
    if (a.isNumeric() && b.isNumeric()) return a.num() == b.num();
    if (a.isArray() || b.isArray()) return a.arr() == b.arr();
    if (a.isObject() || b.isObject()) return a.obj() == b.obj();
    if (a.isMethod() || b.isMethod()) return a.method() == b.method();
    return a.type() == b.type();
}

bool looselyEqual(const Value& a, const Value& b) {  // array_get_index / array_contains
    if (a.isString() && b.isString()) return a.str() == b.str();
    if (a.isNumeric() && b.isNumeric()) return std::fabs(a.num() - b.num()) <= 0.00001;
    return sameValue(a, b);
}

}  // namespace

GMFN(array_create_ext) {
    std::vector<Value> out;
    int n = std::max(0, I(0));
    for (int i = 0; i < n; i++) out.push_back(call(rt, self, other, ARG(1), {Value(static_cast<double>(i))}));
    return newArray(std::move(out));
}
GMFN(array_find_index) {
    auto* r = rowOf(ARG(0));
    if (!r) return Value(-1.0);
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 2);
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--)
        if (call(rt, self, other, ARG(1), {(*r)[i], Value(static_cast<double>(i))}).truthy()) return Value(static_cast<double>(i));
    return Value(-1.0);
}
GMFN(array_get_index) {
    auto* r = rowOf(ARG(0));
    if (!r) return Value(-1.0);
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 2);
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--)
        if (looselyEqual((*r)[i], ARG(1))) return Value(static_cast<double>(i));
    return Value(-1.0);
}
GMFN(array_contains_ext) {  // (array, values, match_all, offset, length)
    auto* r = rowOf(ARG(0));
    auto* vals = rowOf(ARG(1));
    if (!r || !vals) return boolV(false);
    if (vals->empty()) return boolV(true);
    bool all = B(2);
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 3);
    size_t matches = 0;
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--) {
        bool m = std::any_of(vals->begin(), vals->end(), [&](const Value& v) { return looselyEqual((*r)[i], v); });
        if (m) {
            matches++;
            if (!all) return boolV(true);
        }
    }
    return boolV(matches == vals->size());
}
GMFN(array_any) {
    auto* r = rowOf(ARG(0));
    if (!r) return boolV(false);
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 2);
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--)
        if (call(rt, self, other, ARG(1), {(*r)[i], Value(static_cast<double>(i))}).truthy()) return boolV(true);
    return boolV(false);
}
GMFN(array_all) {
    auto* r = rowOf(ARG(0));
    if (!r) return boolV(false);
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 2);
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--)
        if (!call(rt, self, other, ARG(1), {(*r)[i], Value(static_cast<double>(i))}).truthy()) return boolV(false);
    return boolV(true);
}
GMFN(array_foreach) {
    Value arr = ARG(0);  // keeps the array alive while callbacks run
    auto* r = rowOf(arr);
    if (!r) return Value();
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 2);
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--) {
        if (i >= static_cast<int64_t>(r->size())) break;
        Value el = (*r)[i];
        call(rt, self, other, ARG(1), {el, Value(static_cast<double>(i))});
    }
    return Value();
}
GMFN(array_reduce) {  // (array, func, [init], [offset], [length])
    auto* r = rowOf(ARG(0));
    if (!r) return Value();
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 3);
    Value acc = argc > 2 ? ARG(2) : Value();
    int64_t i = sp.offset;
    if (acc.isUndefined() && sp.loops > 0) {
        acc = (*r)[i];
        i += sp.step;
        sp.loops--;
    }
    for (; sp.loops > 0; i += sp.step, sp.loops--)
        acc = call(rt, self, other, ARG(1), {acc, (*r)[i], Value(static_cast<double>(i))});
    return acc;
}
GMFN(array_filter) {
    auto* r = rowOf(ARG(0));
    if (!r) return newArray({});
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 2);
    std::vector<Value> out;
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--)
        if (call(rt, self, other, ARG(1), {(*r)[i], Value(static_cast<double>(i))}).truthy()) out.push_back((*r)[i]);
    return newArray(std::move(out));
}
GMFN(array_filter_ext) {  // keeps matches at the front of the span, returns how many
    Value arr = ARG(0);
    auto* r = writableRow(arr);
    if (!r) return Value(0.0);
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 2);
    int64_t write = sp.offset, kept = 0;
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--)
        if (call(rt, self, other, ARG(1), {(*r)[i], Value(static_cast<double>(i))}).truthy()) {
            (*r)[write] = (*r)[i];
            write += sp.step;
            kept++;
        }
    return Value(static_cast<double>(kept));
}
GMFN(array_map) {
    auto* r = rowOf(ARG(0));
    if (!r) return newArray({});
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 2);
    std::vector<Value> out;
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--)
        out.push_back(call(rt, self, other, ARG(1), {(*r)[i], Value(static_cast<double>(i))}));
    return newArray(std::move(out));
}
GMFN(array_map_ext) {
    Value arr = ARG(0);
    auto* r = writableRow(arr);
    if (!r) return Value(0.0);
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 2);
    int64_t n = 0;
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--, n++)
        (*r)[i] = call(rt, self, other, ARG(1), {(*r)[i], Value(static_cast<double>(i))});
    return Value(static_cast<double>(n));
}
GMFN(array_copy_while) {
    auto* r = rowOf(ARG(0));
    if (!r) return newArray({});
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 2);
    std::vector<Value> out;
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--) {
        if (!call(rt, self, other, ARG(1), {(*r)[i], Value(static_cast<double>(i))}).truthy()) break;
        out.push_back((*r)[i]);
    }
    return newArray(std::move(out));
}
GMFN(array_unique) {
    auto* r = rowOf(ARG(0));
    if (!r) return newArray({});
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 1);
    std::vector<Value> out;
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--)
        if (std::none_of(out.begin(), out.end(), [&](const Value& v) { return sameValue(v, (*r)[i]); })) out.push_back((*r)[i]);
    return newArray(std::move(out));
}
GMFN(array_unique_ext) {
    Value arr = ARG(0);
    auto* r = writableRow(arr);
    if (!r) return Value(0.0);
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 1);
    std::vector<Value> seen;
    int64_t write = sp.offset;
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--) {
        Value el = (*r)[i];
        if (std::none_of(seen.begin(), seen.end(), [&](const Value& v) { return sameValue(v, el); })) {
            (*r)[write] = el;
            write += sp.step;
            seen.push_back(el);
        }
    }
    return Value(static_cast<double>(seen.size()));
}
GMFN(array_reverse) {
    auto* r = rowOf(ARG(0));
    if (!r) return newArray({});
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 1);
    std::vector<Value> out;
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--) out.push_back((*r)[i]);
    std::reverse(out.begin(), out.end());
    return newArray(std::move(out));
}
GMFN(array_reverse_ext) {
    Value arr = ARG(0);
    auto* r = writableRow(arr);
    if (!r) return Value(0.0);
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 1);
    int64_t a = sp.offset, b = sp.offset + (sp.loops - 1) * sp.step;
    for (int64_t k = sp.loops / 2; k > 0; k--, a += sp.step, b -= sp.step) std::swap((*r)[a], (*r)[b]);
    return Value(static_cast<double>(sp.loops));
}
GMFN(array_shuffle) {
    auto* r = rowOf(ARG(0));
    if (!r) return Value();
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 1);
    int64_t start = sp.step < 0 ? sp.offset - (sp.loops - 1) : sp.offset;
    std::vector<Value> out(r->begin() + start, r->begin() + start + sp.loops);
    for (size_t i = out.size(); i > 1; i--) std::swap(out[i - 1], out[std::min(i - 1, static_cast<size_t>(rt.rng.next() * i))]);
    return newArray(std::move(out));
}
GMFN(array_shuffle_ext) {
    Value arr = ARG(0);
    auto* r = writableRow(arr);
    if (!r) return Value(0.0);
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 1);
    int64_t start = sp.step < 0 ? sp.offset - (sp.loops - 1) : sp.offset;
    for (int64_t i = sp.loops; i > 1; i--)
        std::swap((*r)[start + i - 1], (*r)[start + std::min(i - 1, static_cast<int64_t>(rt.rng.next() * i))]);
    return Value(static_cast<double>(sp.loops));
}
GMFN(array_shift) {  // removes and returns the first element
    Value arr = ARG(0);
    auto* r = writableRow(arr);
    if (!r || r->empty()) return Value();
    Value first = r->front();
    r->erase(r->begin());
    return first;
}
GMFN(array_concat) {
    std::vector<Value> out;
    for (int i = 0; i < argc; i++)
        if (auto* r = rowOf(args[i])) out.insert(out.end(), r->begin(), r->end());
    return newArray(std::move(out));
}
GMFN(array_union) {
    std::vector<Value> out;
    for (int i = 0; i < argc; i++)
        if (auto* r = rowOf(args[i]))
            for (auto& v : *r)
                if (std::none_of(out.begin(), out.end(), [&](const Value& x) { return sameValue(x, v); })) out.push_back(v);
    return newArray(std::move(out));
}
GMFN(array_intersection) {
    std::vector<Value> out;
    if (argc == 0) return newArray({});
    if (auto* first = rowOf(args[0]))
        for (auto& v : *first) {
            if (std::any_of(out.begin(), out.end(), [&](const Value& x) { return sameValue(x, v); })) continue;
            bool inAll = true;
            for (int i = 1; i < argc && inAll; i++) {
                auto* r = rowOf(args[i]);
                inAll = r && std::any_of(r->begin(), r->end(), [&](const Value& x) { return sameValue(x, v); });
            }
            if (inAll) out.push_back(v);
        }
    return newArray(std::move(out));
}
GMFN(struct_foreach) {  // (struct, func(name, value))
    GMObject* o = ARG(0).obj();
    if (!o) return Value();
    Value keep = ARG(0);
    std::vector<std::pair<std::string, Value>> entries;
    o->inst->vars.forEachInOrder([&](int32_t id, const Value& v) { entries.push_back({std::string(rt.vm.varName(id)), v}); });
    for (auto& [name, v] : entries) call(rt, self, other, ARG(1), {Value(name), v});
    return Value();
}

// ---- strings

GMFN(string_join) {  // (delimiter, values...)
    std::string out, sep = S(0);
    for (int i = 1; i < argc; i++) out += (i > 1 ? sep : std::string()) + args[i].toString();
    return Value(out);
}
GMFN(string_join_ext) {  // (delimiter, array, [offset], [length])
    std::string out, sep = S(0);
    auto* r = rowOf(ARG(1));
    if (!r) return Value("");
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 2);
    bool first = true;
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--, first = false)
        out += (first ? std::string() : sep) + (*r)[i].toString();
    return Value(out);
}
GMFN(string_concat) {
    std::string out;
    for (int i = 0; i < argc; i++) out += args[i].toString();
    return Value(out);
}
GMFN(string_concat_ext) {  // (array, [offset], [length])
    std::string out;
    auto* r = rowOf(ARG(0));
    if (!r) return Value("");
    Span sp = span(static_cast<int64_t>(r->size()), args, argc, 1);
    for (int64_t i = sp.offset; sp.loops > 0; i += sp.step, sp.loops--) out += (*r)[i].toString();
    return Value(out);
}
GMFN(string_pos_ext) {  // (substr, str, start position) -> 1-based position or 0
    auto sub = utf8Decode(S(0)), s = utf8Decode(S(1));
    if (sub.empty()) return Value(0.0);
    size_t from = static_cast<size_t>(std::max(0, I(2) - 1));
    size_t p = from <= s.size() ? s.find(sub, from) : std::u32string::npos;
    return Value(p == std::u32string::npos ? 0.0 : static_cast<double>(p + 1));
}
GMFN(string_last_pos_ext) {  // searches backwards from the start position
    if (I(2) <= 0) return Value(0.0);
    auto sub = utf8Decode(S(0)), s = utf8Decode(S(1));
    if (sub.empty()) return Value(0.0);
    size_t p = s.rfind(sub, static_cast<size_t>(I(2) - 1));
    return Value(p == std::u32string::npos ? 0.0 : static_cast<double>(p + 1));
}
static bool isSpace(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == 0xA0; }
// string_trim_start/end(str, [substrings]): whitespace, or any of the given substrings, repeatedly.
static std::u32string trimSide(Value* args, int argc, bool start) {
    auto s = utf8Decode(argS(args, argc, 0));
    std::vector<std::u32string> subs;
    if (argc > 1)
        if (auto* r = rowOf(args[1]))
            for (auto& v : *r)
                if (!v.toString().empty()) subs.push_back(utf8Decode(v.toString()));
    bool changed = true;
    while (changed && !s.empty()) {
        changed = false;
        if (subs.empty()) {
            if (isSpace(start ? s.front() : s.back())) {
                s.erase(start ? 0 : s.size() - 1, 1);
                changed = true;
            }
            continue;
        }
        for (auto& sub : subs) {
            if (sub.size() > s.size()) continue;
            if (start ? s.compare(0, sub.size(), sub) == 0 : s.compare(s.size() - sub.size(), sub.size(), sub) == 0) {
                start ? s.erase(0, sub.size()) : s.erase(s.size() - sub.size());
                changed = true;
                break;
            }
        }
    }
    return s;
}
GMFN(string_trim_start) { return Value(utf8Encode(trimSide(args, argc, true))); }
GMFN(string_trim_end) { return Value(utf8Encode(trimSide(args, argc, false))); }
// "{0}" placeholders from a list of values; "{{" and "}}" are literal braces.
std::string formatPlaceholders(const std::string& fmt, Value* vals, int n) {
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt.compare(i, 2, "{{") == 0 || fmt.compare(i, 2, "}}") == 0) {
            out += fmt[i];
            i++;
            continue;
        }
        if (fmt[i] == '{') {
            size_t close = fmt.find('}', i);
            if (close != std::string::npos && close > i + 1) {
                std::string num = fmt.substr(i + 1, close - i - 1);
                if (num.find_first_not_of("0123456789") == std::string::npos) {
                    int idx = std::atoi(num.c_str());
                    if (idx >= 0 && idx < n) {
                        out += vals[idx].toString();
                        i = close;
                        continue;
                    }
                }
            }
        }
        out += fmt[i];
    }
    return out;
}
GMFN(string_ext) {  // (format, values array)
    auto* r = rowOf(ARG(1));
    if (!r) return Value(S(0));
    std::vector<Value> vals(*r);
    return Value(formatPlaceholders(S(0), vals.data(), static_cast<int>(vals.size())));
}
GMFN(string_split_ext) {  // (str, delimiters array, [remove_empty], [max_splits])
    std::string s = S(0);
    std::vector<std::string> delims;
    if (auto* r = rowOf(ARG(1)))
        for (auto& v : *r)
            if (!v.toString().empty()) delims.push_back(v.toString());
    bool removeEmpty = B(2);
    long maxSplits = argc > 3 ? static_cast<long>(D(3)) : static_cast<long>(s.size());
    std::vector<Value> out;
    size_t start = 0, pos = 0;
    long splits = 0;
    while (pos < s.size() && splits < maxSplits) {
        size_t len = 0;
        for (auto& d : delims)
            if (s.compare(pos, d.size(), d) == 0) {
                len = d.size();
                break;
            }
        if (len) {
            std::string part = s.substr(start, pos - start);
            if (!removeEmpty || !part.empty()) out.push_back(Value(part));
            pos += len;
            start = pos;
            splits++;
        } else {
            pos++;
        }
    }
    std::string last = s.substr(start);
    if (!removeEmpty || !last.empty()) out.push_back(Value(last));
    return newArray(std::move(out));
}

// string_foreach(string, func, [pos], [length]): func(character, position) for each character
// (positions from 1; a negative position counts from the end, a negative length walks back).
GMFN(string_foreach) {
    std::u32string s = utf8Decode(S(0));
    int64_t n = static_cast<int64_t>(s.size());
    if (n == 0) return Value();
    double pos = argc > 2 && !ARG(2).isUndefined() ? argNum(ARG(2)) : 1;
    Value spanArgs[2] = {Value(pos < 0 ? pos : pos > 0 ? pos - 1 : 0.0),
                         Value(argc > 3 ? argNum(ARG(3)) : static_cast<double>(n))};
    Span sp = span(n, spanArgs, 2, 0);
    for (int64_t i = sp.offset; sp.loops > 0 && i >= 0 && i < n; i += sp.step, sp.loops--)
        call(rt, self, other, ARG(1), {Value(utf8Encode(s[static_cast<size_t>(i)])), Value(static_cast<double>(i + 1))});
    return Value();
}

void registerArrayBuiltins(Runner& rt) {
    GMREG(string_foreach);
    GMREG(array_create_ext); GMREG(array_find_index); GMREG(array_get_index); GMREG(array_contains_ext);
    GMREG(array_any); GMREG(array_all); GMREG(array_foreach); GMREG(array_reduce); GMREG(array_filter);
    GMREG(array_filter_ext); GMREG(array_map); GMREG(array_map_ext); GMREG(array_copy_while); GMREG(array_unique);
    GMREG(array_unique_ext); GMREG(array_reverse); GMREG(array_reverse_ext); GMREG(array_shuffle);
    GMREG(array_shuffle_ext); GMREG(array_shift); GMREG(array_concat); GMREG(array_union); GMREG(array_intersection);
    GMREG(struct_foreach); GMREG(string_join); GMREG(string_join_ext); GMREG(string_concat); GMREG(string_concat_ext);
    GMREG(string_pos_ext); GMREG(string_last_pos_ext); GMREG(string_trim_start); GMREG(string_trim_end);
    GMREG(string_ext); GMREG(string_split_ext);
}

}  // namespace gm
