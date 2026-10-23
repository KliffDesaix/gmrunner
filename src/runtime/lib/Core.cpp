// Math, strings, random numbers, colours, types and miscellaneous core functions.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "runtime/Builtins.h"
#include "util/Utf8.h"

namespace gm {

static double deg(double r) { return r * 180.0 / M_PI; }
static double rad(double d) { return d * M_PI / 180.0; }

// ---- type conversion
GMFN(real) {
    const Value& v = ARG(0);
    if (!v.isString()) return Value(v.num());
    const std::string& s = v.str();
    const char* p = s.c_str();
    while (*p == ' ' || *p == '\t') p++;
    // GameMaker also reads hexadecimal ("0x1F", "$1F").
    if ((p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) || p[0] == '$') {
        const char* h = p[0] == '$' ? p + 1 : p + 2;
        char* end = nullptr;
        unsigned long long x = std::strtoull(h, &end, 16);
        if (end != h) return Value(static_cast<double>(x));
    }
    char* end = nullptr;
    double d = std::strtod(p, &end);
    if (end == p) return Value(0.0);  // GameMaker raises an error here; 0 keeps events running
    return Value(d);
}
// GameMaker 2023: string("x = {0}", x) fills placeholders from the extra arguments.
GMFN(string) {
    if (argc > 1 && ARG(0).isString()) return Value(formatPlaceholders(S(0), args + 1, argc - 1));
    return Value(ARG(0).toString());
}
GMFN(int64) { return Value::i64(ARG(0).toInt64()); }
GMFN(is_string) { return boolV(ARG(0).isString()); }
// Any plain number, int32 included (variables written with pop.v.i hold int32s); not bools or int64s.
GMFN(is_real) { return boolV(ARG(0).isReal() || ARG(0).type() == VT::Int32); }
GMFN(is_array) { return boolV(ARG(0).isArray()); }
GMFN(is_undefined) { return boolV(ARG(0).isUndefined()); }
GMFN(is_bool) { return boolV(ARG(0).type() == VT::Bool); }
GMFN(is_int32) { return boolV(ARG(0).type() == VT::Int32); }
GMFN(is_int64) { return boolV(ARG(0).type() == VT::Int64); }
GMFN(is_ptr) { return boolV(ARG(0).type() == VT::Ptr); }

// ---- strings (indices are 1-based code points)
static std::u32string u32(const Value& v) { return utf8Decode(v.toString()); }

GMFN(string_length) { return Value(static_cast<double>(u32(ARG(0)).size())); }
GMFN(string_byte_length) { return Value(static_cast<double>(S(0).size())); }
GMFN(string_char_at) {
    auto s = u32(ARG(0));
    int i = I(1);
    if (i < 1) i = 1;  // GameMaker clamps index 0 to the first character
    if (static_cast<size_t>(i) > s.size()) return Value("");
    return Value(utf8Encode(s[i - 1]));
}
GMFN(string_byte_at) {
    std::string s = S(0);
    int i = I(1);
    if (i < 1 || static_cast<size_t>(i) > s.size()) return Value(0.0);
    return Value(static_cast<double>(static_cast<unsigned char>(s[i - 1])));
}
GMFN(string_ord_at) {  // the character code at a position (1-based, clamped like string_char_at)
    auto s = u32(ARG(0));
    int i = std::max(1, I(1));
    if (static_cast<size_t>(i) > s.size()) return Value(-1.0);
    return Value(static_cast<double>(s[i - 1]));
}
GMFN(string_set_byte_at) {  // (string, position, byte): a copy with that byte replaced
    std::string s = S(0);
    int i = I(1);
    if (i >= 1 && static_cast<size_t>(i) <= s.size()) s[i - 1] = static_cast<char>(I(2) & 0xFF);
    return Value(s);
}
GMFN(string_copy) {
    auto s = u32(ARG(0));
    int idx = std::max(1, I(1)), cnt = I(2);
    if (cnt <= 0 || static_cast<size_t>(idx) > s.size()) return Value("");
    return Value(utf8Encode(s.substr(idx - 1, cnt)));
}
GMFN(string_delete) {
    auto s = u32(ARG(0));
    int idx = std::max(1, I(1)), cnt = I(2);  // indexes below 1 mean the first character
    if (static_cast<size_t>(idx) > s.size() || cnt <= 0) return Value(utf8Encode(s));
    s.erase(idx - 1, cnt);
    return Value(utf8Encode(s));
}
GMFN(string_insert) {
    auto sub = u32(ARG(0)), s = u32(ARG(1));
    int idx = std::max(1, I(2));
    if (static_cast<size_t>(idx) > s.size() + 1) idx = static_cast<int>(s.size()) + 1;
    s.insert(idx - 1, sub);
    return Value(utf8Encode(s));
}
GMFN(string_pos) {
    auto sub = u32(ARG(0)), s = u32(ARG(1));
    if (sub.empty()) return Value(0.0);
    size_t p = s.find(sub);
    return Value(p == std::u32string::npos ? 0.0 : static_cast<double>(p + 1));
}
GMFN(string_last_pos) {
    auto sub = u32(ARG(0)), s = u32(ARG(1));
    if (sub.empty()) return Value(0.0);
    size_t p = s.rfind(sub);
    return Value(p == std::u32string::npos ? 0.0 : static_cast<double>(p + 1));
}
GMFN(string_count) {
    std::string sub = S(0), s = S(1);
    if (sub.empty()) return Value(0.0);
    int n = 0;
    for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + sub.size())) n++;
    return Value(static_cast<double>(n));
}
static std::string replace(const std::string& s, const std::string& from, const std::string& to, bool all) {
    if (from.empty()) return s;
    std::string out;
    size_t p = 0;
    while (true) {
        size_t q = s.find(from, p);
        if (q == std::string::npos) break;
        out += s.substr(p, q - p) + to;
        p = q + from.size();
        if (!all) break;
    }
    return out + s.substr(p);
}
GMFN(string_replace_all) { return Value(replace(S(0), S(1), S(2), true)); }
GMFN(string_replace) { return Value(replace(S(0), S(1), S(2), false)); }
GMFN(string_upper) {
    std::string s = S(0);
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return Value(s);
}
GMFN(string_lower) {
    std::string s = S(0);
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return Value(s);
}
template <typename Pred> static std::string filter(const std::string& s, Pred p) {
    std::string out;
    for (char c : s)
        if (p(c)) out += c;
    return out;
}
GMFN(string_letters) {
    return Value(filter(S(0), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }));
}
GMFN(string_digits) { return Value(filter(S(0), [](char c) { return c >= '0' && c <= '9'; })); }
GMFN(string_lettersdigits) {
    return Value(filter(S(0), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    }));
}
GMFN(string_repeat) {
    std::string s = S(0), out;
    for (int i = 0; i < I(1); i++) out += s;
    return Value(out);
}
GMFN(string_format) {
    double v = D(0);
    int tot = std::max(0, I(1)), dec = std::max(0, I(2));
    char buf[128];
    snprintf(buf, sizeof buf, "%*.*f", tot + (dec > 0 ? dec + 1 : 0), dec, v);
    return Value(std::string(buf));
}
GMFN(ord) {
    auto s = u32(ARG(0));
    return Value(s.empty() ? 0.0 : static_cast<double>(s[0]));
}
GMFN(chr) { return Value(utf8Encode(static_cast<char32_t>(I(0)))); }
GMFN(ansi_char) { return Value(std::string(1, static_cast<char>(I(0)))); }

// ---- math
GMFN(abs) { return Value(std::fabs(D(0))); }
GMFN(sign) { double d = D(0); return Value(d > 0 ? 1.0 : (d < 0 ? -1.0 : 0.0)); }
GMFN(floor) { return Value(std::floor(D(0))); }
GMFN(ceil) { return Value(std::ceil(D(0))); }
GMFN(round) {
    // GameMaker rounds halves to even, like the CPU default.
    return Value(std::nearbyint(D(0)));
}
GMFN(frac) { double d = D(0); return Value(d - std::trunc(d)); }
GMFN(sqr) { double d = D(0); return Value(d * d); }
GMFN(sqrt) { return Value(std::sqrt(std::max(0.0, D(0)))); }
GMFN(power) { return Value(std::pow(D(0), D(1))); }
GMFN(exp) { return Value(std::exp(D(0))); }
GMFN(ln) { return Value(std::log(D(0))); }
GMFN(log2) { return Value(std::log2(D(0))); }
GMFN(log10) { return Value(std::log10(D(0))); }
GMFN(logn) { return Value(std::log(D(1)) / std::log(D(0))); }
GMFN(sin) { return Value(std::sin(D(0))); }
GMFN(cos) { return Value(std::cos(D(0))); }
GMFN(tan) { return Value(std::tan(D(0))); }
GMFN(arcsin) { return Value(std::asin(D(0))); }
GMFN(arccos) { return Value(std::acos(D(0))); }
GMFN(arctan) { return Value(std::atan(D(0))); }
GMFN(arctan2) { return Value(std::atan2(D(0), D(1))); }
GMFN(dsin) { return Value(std::sin(rad(D(0)))); }
GMFN(dcos) { return Value(std::cos(rad(D(0)))); }
GMFN(dtan) { return Value(std::tan(rad(D(0)))); }
GMFN(darcsin) { return Value(deg(std::asin(D(0)))); }
GMFN(darccos) { return Value(deg(std::acos(D(0)))); }
GMFN(darctan) { return Value(deg(std::atan(D(0)))); }
GMFN(darctan2) { return Value(deg(std::atan2(D(0), D(1)))); }
GMFN(degtorad) { return Value(rad(D(0))); }
GMFN(radtodeg) { return Value(deg(D(0))); }
GMFN(min) {
    if (argc == 0) return Value(0.0);
    double m = D(0);
    for (int i = 1; i < argc; i++) m = std::min(m, D(i));
    return Value(m);
}
GMFN(max) {
    if (argc == 0) return Value(0.0);
    double m = D(0);
    for (int i = 1; i < argc; i++) m = std::max(m, D(i));
    return Value(m);
}
GMFN(mean) {
    if (argc == 0) return Value(0.0);
    double s = 0;
    for (int i = 0; i < argc; i++) s += D(i);
    return Value(s / argc);
}
GMFN(median) {
    if (argc == 0) return Value(0.0);
    std::vector<double> v;
    for (int i = 0; i < argc; i++) v.push_back(D(i));
    std::sort(v.begin(), v.end());
    return Value(v[(v.size() - 1) / 2]);
}
GMFN(clamp) { return Value(std::max(D(1), std::min(D(2), D(0)))); }
GMFN(lerp) { return Value(D(0) + (D(1) - D(0)) * D(2)); }
GMFN(lengthdir_x) { return Value(D(0) * std::cos(rad(D(1)))); }
GMFN(lengthdir_y) { return Value(-D(0) * std::sin(rad(D(1)))); }
GMFN(point_direction) {
    double d = deg(std::atan2(-(D(3) - D(1)), D(2) - D(0)));
    if (d < 0) d += 360;
    return Value(d);
}
GMFN(point_distance) { return Value(std::hypot(D(2) - D(0), D(3) - D(1))); }
GMFN(angle_difference) {
    double d = std::fmod(D(0) - D(1) + 540.0, 360.0);
    if (d < 0) d += 360;
    return Value(d - 180.0);
}
GMFN(dot_product) { return Value(D(0) * D(2) + D(1) * D(3)); }
GMFN(dot_product_3d) { return Value(D(0) * D(3) + D(1) * D(4) + D(2) * D(5)); }
GMFN(dot_product_normalised) {
    double l1 = std::hypot(D(0), D(1)), l2 = std::hypot(D(2), D(3));
    return Value(l1 > 0 && l2 > 0 ? (D(0) * D(2) + D(1) * D(3)) / (l1 * l2) : 0.0);
}
GMFN(dot_product_3d_normalised) {
    double l1 = std::sqrt(D(0) * D(0) + D(1) * D(1) + D(2) * D(2)), l2 = std::sqrt(D(3) * D(3) + D(4) * D(4) + D(5) * D(5));
    return Value(l1 > 0 && l2 > 0 ? (D(0) * D(3) + D(1) * D(4) + D(2) * D(5)) / (l1 * l2) : 0.0);
}
GMFN(lin_to_db) { return Value(D(0) > 0 ? 20 * std::log10(D(0)) : -INFINITY); }
GMFN(db_to_lin) { return Value(std::pow(10.0, D(0) / 20)); }

// ---- random
// GameMaker's own formulas (YoYo's HTML5 runner), including the extra draws in irandom and
// random_range, so seeded sequences match the real runner.
GMFN(random) { return Value(rt.rng.next() * D(0)); }
GMFN(random_range) {
    double a = D(0), b = D(1);
    if (a == b) return Value(a);
    if (a > b) std::swap(a, b);
    double r = a + rt.rng.next() * (b - a);
    rt.rng.next();
    return Value(r);
}
GMFN(irandom) {
    int32_t n = I(0);
    int32_t sign = n < 0 ? -1 : 1;
    double r = rt.rng.next() * (static_cast<double>(n) + sign);
    rt.rng.next();
    return Value(std::trunc(r));
}
GMFN(irandom_range) {
    int32_t a = I(0), b = I(1);
    if (a > b) std::swap(a, b);
    return Value(a + std::trunc(rt.rng.next() * (static_cast<double>(b) - a + 1)));
}
GMFN(choose) {
    if (argc == 0) return Value(0.0);
    int i = static_cast<int>(std::floor(rt.rng.next() * argc));
    return args[std::min(i, argc - 1)];
}
GMFN(randomize) {
    int32_t s = rt.fixedSeed >= 0 ? static_cast<int32_t>(rt.fixedSeed) : static_cast<int32_t>(std::random_device{}());
    rt.rng.seed(s);
    return Value(static_cast<double>(s));
}
GMFN(random_set_seed) {
    rt.rng.seed(static_cast<int32_t>(ARG(0).toInt64()));
    return Value();
}
GMFN(random_get_seed) { return Value(static_cast<double>(rt.rng.currentSeed())); }

// ---- colours (GameMaker colours are 0xBBGGRR)
GMFN(make_color_rgb) {
    return Value(static_cast<double>((I(0) & 0xFF) | ((I(1) & 0xFF) << 8) | ((I(2) & 0xFF) << 16)));
}
GMFN(color_get_red) { return Value(static_cast<double>(C(0) & 0xFF)); }
GMFN(color_get_green) { return Value(static_cast<double>((C(0) >> 8) & 0xFF)); }
GMFN(color_get_blue) { return Value(static_cast<double>((C(0) >> 16) & 0xFF)); }
GMFN(merge_color) {
    uint32_t a = C(0), b = C(1);
    double t = std::max(0.0, std::min(1.0, D(2)));
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        double ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= static_cast<uint32_t>(std::lround(ca + (cb - ca) * t)) << sh;
    }
    return Value(static_cast<double>(out));
}
static void rgbToHsv(uint32_t c, double& h, double& s, double& v) {
    double r = (c & 0xFF) / 255.0, g = ((c >> 8) & 0xFF) / 255.0, b = ((c >> 16) & 0xFF) / 255.0;
    double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d == 0) h = 0;
    else if (mx == r) h = std::fmod((g - b) / d, 6.0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
    if (h < 0) h += 360;
}
GMFN(make_color_hsv) {
    double h = D(0) / 255.0 * 360.0, s = D(1) / 255.0, v = D(2) / 255.0;
    double c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1)), m = v - c;
    double r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else { r = c; b = x; }
    auto to8 = [](double f) { return static_cast<uint32_t>(std::lround(std::max(0.0, std::min(1.0, f)) * 255)); };
    return Value(static_cast<double>(to8(r + m) | (to8(g + m) << 8) | (to8(b + m) << 16)));
}
GMFN(color_get_hue) { double h, s, v; rgbToHsv(C(0), h, s, v); return Value(h / 360.0 * 255.0); }
GMFN(color_get_saturation) { double h, s, v; rgbToHsv(C(0), h, s, v); return Value(s * 255.0); }
GMFN(color_get_value) { double h, s, v; rgbToHsv(C(0), h, s, v); return Value(v * 255.0); }

// ---- arrays
GMFN(array_length_1d) {
    GMArray* a = ARG(0).arr();
    return Value(a ? static_cast<double>(a->length1d()) : 0.0);
}
GMFN(array_height_2d) {
    GMArray* a = ARG(0).arr();
    return Value(a ? static_cast<double>(a->rows.size()) : 0.0);
}
GMFN(array_length_2d) {
    GMArray* a = ARG(0).arr();
    int r = I(1);
    if (!a || r < 0 || static_cast<size_t>(r) >= a->rows.size()) return Value(0.0);
    return Value(static_cast<double>(a->rows[r].size()));
}
GMFN(array_create) {
    Value v = Value::newArray();
    int n = I(0);
    GMArray* a = v.writableArray();
    a->rows.resize(1);
    a->rows[0].assign(std::max(0, n), argc > 1 ? args[1] : Value(0.0));
    return v;
}

// ---- debugging / scripts / variables
GMFN(show_debug_message) {
    // stderr, with the runner's own messages, so stdout stays clean for reports (--check/--smoke).
    fprintf(stderr, "%s\n", ARG(0).toString().c_str());
    return Value();
}
GMFN(show_message) {
    rt.log("[show_message] %s", ARG(0).toString().c_str());
    return Value();
}
GMFN(show_error) {
    rt.log("[show_error] %s", ARG(0).toString().c_str());
    if (B(1)) rt.gameEnded = true;
    return Value();
}
GMFN(get_timer) { return Value(std::floor(rt.currentTimeMs() * 1000.0)); }
GMFN(script_exists) {
    int s = I(0);
    return boolV(s >= 0 && static_cast<size_t>(s) < rt.dw.scripts.size());
}
GMFN(script_get_name) {
    int s = I(0);
    if (s < 0 || static_cast<size_t>(s) >= rt.dw.scripts.size()) return Value("<undefined>");
    return Value(std::string(rt.dw.scripts[s].name));
}
GMFN(script_execute) {
    int code = rt.vm.scriptCode(I(0));
    if (code < 0) throw GMLError("script_execute: invalid script " + ARG(0).toString());
    return rt.vm.call(code, self, other, argc - 1, args + 1);
}
GMFN(variable_global_get) {
    Value* v = rt.vm.findGlobal(rt.vm.varIdForName(S(0)));
    return v ? *v : Value();
}
GMFN(variable_global_set) {
    rt.vm.globalVar(rt.vm.varIdForName(S(0))) = ARG(1);
    return Value();
}
GMFN(variable_global_exists) {
    Value* v = rt.vm.findGlobal(rt.vm.varIdForName(S(0)));
    return boolV(v && !v->isUndefined());
}
static Instance* instArg(Runner& rt, Instance* self, Instance* other, const Value& v) {
    int t = v.toInt();
    if (t == INST_SELF) return self;
    if (t == INST_OTHER) return other;
    if (t >= 100000) return rt.findById(t);
    return rt.firstOf(t);
}
GMFN(variable_instance_get) {
    Instance* i = instArg(rt, self, other, ARG(0));
    return i ? rt.vm.memberGet(i, rt.vm.varIdForName(S(1))) : Value();
}
GMFN(variable_instance_set) {
    Instance* i = instArg(rt, self, other, ARG(0));
    if (i) rt.vm.memberSet(i, rt.vm.varIdForName(S(1)), ARG(2));
    return Value();
}
GMFN(variable_instance_exists) {
    Instance* i = instArg(rt, self, other, ARG(0));
    return boolV(i && rt.vm.memberExists(i, rt.vm.varIdForName(S(1))));
}
GMFN(game_end) {
    rt.gameEnded = true;
    return Value();
}
GMFN(game_restart) {
    rt.gameRestartPending = true;
    return Value();
}
GMFN(code_is_compiled) { return boolV(false); }
GMFN(debug_get_callstack) { return Value(rt.vm.currentCodeName()); }
GMFN(environment_get_variable) { return Value(""); }
GMFN(parameter_count) { return Value(static_cast<double>(rt.parameters.empty() ? 0 : rt.parameters.size() - 1)); }
GMFN(parameter_string) {
    int i = I(0);
    return i >= 0 && static_cast<size_t>(i) < rt.parameters.size() ? Value(rt.parameters[i]) : Value("");
}
// game_change(working directory, parameters): ends this game; the runner then starts the game in
// that directory (relative to this game's), with "-game <file>" naming its data file.
GMFN(game_change) {
    rt.gameChangeRequested = true;
    rt.gameChangeDir = S(0);
    rt.gameChangeParams = S(1);
    rt.gameEnded = true;
    return Value();
}

void registerCoreBuiltins(Runner& rt) {
    GMREG(real); GMREG(string); GMREG(int64); GMREG(is_string); GMREG(is_real); GMREG(is_array);
    GMREG(is_undefined); GMREG(is_bool); GMREG(is_int32); GMREG(is_int64); GMREG(is_ptr);
    GMREG(string_ord_at); GMREG(string_set_byte_at); GMREG(dot_product_3d); GMREG(dot_product_normalised);
    GMALIAS(dot_product_normalized, dot_product_normalised); GMREG(dot_product_3d_normalised);
    GMALIAS(dot_product_3d_normalized, dot_product_3d_normalised); GMREG(lin_to_db); GMREG(db_to_lin);
    GMREG(string_length); GMREG(string_byte_length); GMREG(string_char_at); GMREG(string_byte_at);
    GMREG(string_copy); GMREG(string_delete); GMREG(string_insert); GMREG(string_pos); GMREG(string_last_pos);
    GMREG(string_count); GMREG(string_replace_all); GMREG(string_replace); GMREG(string_upper);
    GMREG(string_lower); GMREG(string_letters); GMREG(string_digits); GMREG(string_lettersdigits);
    GMREG(string_repeat); GMREG(string_format); GMREG(ord); GMREG(chr); GMREG(ansi_char);
    GMREG(abs); GMREG(sign); GMREG(floor); GMREG(ceil); GMREG(round); GMREG(frac); GMREG(sqr); GMREG(sqrt);
    GMREG(power); GMREG(exp); GMREG(ln); GMREG(log2); GMREG(log10); GMREG(logn); GMREG(sin); GMREG(cos);
    GMREG(tan); GMREG(arcsin); GMREG(arccos); GMREG(arctan); GMREG(arctan2); GMREG(dsin); GMREG(dcos);
    GMREG(dtan); GMREG(darcsin); GMREG(darccos); GMREG(darctan); GMREG(darctan2); GMREG(degtorad);
    GMREG(radtodeg); GMREG(min); GMREG(max); GMREG(mean); GMREG(median); GMREG(clamp); GMREG(lerp);
    GMREG(lengthdir_x); GMREG(lengthdir_y); GMREG(point_direction); GMREG(point_distance);
    GMREG(angle_difference); GMREG(dot_product);
    GMREG(random); GMREG(random_range); GMREG(irandom); GMREG(irandom_range); GMREG(choose); GMREG(randomize); GMALIAS(randomise, randomize);
    GMREG(random_set_seed); GMREG(random_get_seed);
    GMREG(make_color_rgb); GMALIAS(make_colour_rgb, make_color_rgb); GMREG(color_get_red);
    GMALIAS(colour_get_red, color_get_red); GMREG(color_get_green); GMALIAS(colour_get_green, color_get_green);
    GMREG(color_get_blue); GMALIAS(colour_get_blue, color_get_blue); GMREG(merge_color);
    GMALIAS(merge_colour, merge_color); GMREG(make_color_hsv); GMALIAS(make_colour_hsv, make_color_hsv);
    GMREG(color_get_hue); GMALIAS(colour_get_hue, color_get_hue); GMREG(color_get_saturation);
    GMALIAS(colour_get_saturation, color_get_saturation); GMREG(color_get_value);
    GMALIAS(colour_get_value, color_get_value);
    GMREG(array_length_1d); GMREG(array_height_2d); GMREG(array_length_2d); GMREG(array_create);
    GMREG(show_debug_message); GMREG(show_message); GMREG(show_error); GMREG(get_timer); GMREG(script_exists);
    GMREG(script_get_name); GMREG(script_execute); GMREG(variable_global_get); GMREG(variable_global_set);
    GMREG(variable_global_exists); GMREG(variable_instance_get); GMREG(variable_instance_set);
    GMREG(variable_instance_exists); GMREG(game_end); GMREG(game_restart); GMREG(code_is_compiled);
    GMREG(debug_get_callstack); GMREG(environment_get_variable); GMREG(parameter_count); GMREG(game_change);
    GMREG(parameter_string);
}

// Extension (DLL) functions without native code here behave as if their library were missing:
// they return 0, or "" for string functions.
GMFN(extension_missing_real) { GM_UNUSED_FN_ARGS; return Value(0.0); }
GMFN(extension_missing_string) { GM_UNUSED_FN_ARGS; return Value(""); }

void registerAllBuiltins(Runner& rt) {
    registerCoreBuiltins(rt);
    registerDsBuiltins(rt);
    registerInstanceBuiltins(rt);
    registerDrawBuiltins(rt);
    registerAudioBuiltins(rt);
    registerInputBuiltins(rt);
    registerFileBuiltins(rt);
    registerParticleBuiltins(rt);
    registerActionBuiltins(rt);
    registerJsonBuiltins(rt);
    registerSkeletonBuiltins(rt);
    registerBufferBuiltins(rt);
    registerLayerBuiltins(rt);
    registerMiscBuiltins(rt);
    registerArrayBuiltins(rt);
    registerTimeBuiltins(rt);
    registerVertexBuiltins(rt);
    registerSpriteBuiltins(rt);
    registerAnimCurveBuiltins(rt);
    registerSequenceBuiltins(rt);
    registerDateBuiltins(rt);
    registerVideoBuiltins(rt);
    registerExtensionEmulation(rt);
    registerGms2Builtins(rt);  // last: overrides some 1.4 versions (script_execute)
    for (const auto& f : rt.dw.extensionFunctions)
        if (!rt.vm.hasBuiltin(f.name))
            rt.vm.registerBuiltin(std::string(f.name).c_str(),
                                  f.returnType == 1 ? fn_extension_missing_string : fn_extension_missing_real);
    // What the IDE compiles an extension call to on a platform the extension doesn't support.
    // (Also extensionStubbed below.)
    rt.vm.registerBuiltin("extension_stubfunc_real", fn_extension_missing_real);
    rt.vm.registerBuiltin("extension_stubfunc_string", fn_extension_missing_string);
}

bool extensionStubbed(Runner& rt, std::string_view name) {
    BuiltinFn f = rt.vm.findBuiltin(name);
    return f == fn_extension_missing_real || f == fn_extension_missing_string;
}

}  // namespace gm
