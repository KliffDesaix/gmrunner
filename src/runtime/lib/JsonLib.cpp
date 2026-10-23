// JSON: json_parse / json_stringify (GMS 2.3 structs and arrays) and json_decode / json_encode
// (ds_maps and ds_lists, with nested structures marked as GameMaker does).
#include <cmath>
#include <cstdio>
#include <string>

#include "../Builtins.h"
#include "../Stores.h"

namespace gm {

void dsDestroyMap(Runner& rt, int id);

namespace {

// ---- reading

struct JsonReader {
    const std::string& s;
    size_t p = 0;
    bool ok = true;

    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
    }
    bool eat(char c) {
        ws();
        if (p < s.size() && s[p] == c) {
            p++;
            return true;
        }
        return false;
    }
    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    uint32_t hex4() {
        uint32_t v = 0;
        for (int i = 0; i < 4 && p < s.size(); i++, p++) {
            char c = s[p];
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
        }
        return v;
    }
    std::string str() {  // after the opening quote
        std::string out;
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c != '\\' || p >= s.size()) {
                out += c;
                continue;
            }
            char e = s[p++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                        p += 2;
                        uint32_t lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += e; break;
            }
        }
        if (p < s.size()) p++;
        else ok = false;
        return out;
    }
    double number() {
        const char* start = s.c_str() + p;
        char* end = nullptr;
        double d = strtod(start, &end);
        if (end == start) {
            ok = false;
            p++;
            return 0;
        }
        p += static_cast<size_t>(end - start);
        return d;
    }
    bool word(const char* w) {
        size_t n = strlen(w);
        if (s.compare(p, n, w) == 0) {
            p += n;
            return true;
        }
        return false;
    }
};

// Builds GML values (structs/arrays) or ds structures, depending on the reader mode.
struct JsonBuilder {
    Runner& rt;
    bool ds;  // json_decode: objects -> ds_map, arrays -> ds_list

    Value parse(JsonReader& r, uint8_t* mark) {
        r.ws();
        if (mark) *mark = DS_MARK_NONE;
        if (r.p >= r.s.size()) {
            r.ok = false;
            return Value();
        }
        char c = r.s[r.p];
        if (c == '{') {
            r.p++;
            if (ds) {
                int id = rt.ds->maps.create();
                if (mark) *mark = DS_MARK_MAP;
                if (r.eat('}')) return Value(static_cast<double>(id));
                do {
                    if (r.eat('}')) return Value(static_cast<double>(id));  // trailing comma
                    if (!r.eat('"')) { r.ok = false; break; }
                    std::string key = r.str();
                    if (!r.eat(':')) { r.ok = false; break; }
                    uint8_t m = 0;
                    Value v = parse(r, &m);
                    DsMap* map = rt.ds->maps.get(id);
                    ValueKey k{true, 0, key};
                    if (!map->values.count(k)) map->order.push_back(k);
                    map->values[k] = v;
                    if (m) map->marks[k] = m;
                } while (r.ok && r.eat(','));
                if (!r.eat('}')) r.ok = false;
                return Value(static_cast<double>(id));
            }
            Value st = rt.vm.newStruct(nullptr);
            Instance* inst = st.obj()->inst;
            if (r.eat('}')) return st;
            // GameMaker's parser takes a comma before the closing brace or bracket, as the Windows
            // runner does (games ship such files).
            do {
                if (r.eat('}')) return st;
                if (!r.eat('"')) { r.ok = false; break; }
                std::string key = r.str();
                if (!r.eat(':')) { r.ok = false; break; }
                Value v = parse(r, nullptr);
                rt.vm.setInstanceVar(inst, rt.vm.varIdForName(key), v);
            } while (r.ok && r.eat(','));
            if (!r.eat('}')) r.ok = false;
            return st;
        }
        if (c == '[') {
            r.p++;
            if (ds) {
                int id = rt.ds->lists.create();
                if (mark) *mark = DS_MARK_LIST;
                if (r.eat(']')) return Value(static_cast<double>(id));
                do {
                    if (r.eat(']')) return Value(static_cast<double>(id));  // trailing comma
                    uint8_t m = 0;
                    Value v = parse(r, &m);
                    auto* l = rt.ds->lists.get(id);
                    if (m) rt.ds->listMarks[id][l->size()] = m;
                    l->push_back(v);
                } while (r.ok && r.eat(','));
                if (!r.eat(']')) r.ok = false;
                return Value(static_cast<double>(id));
            }
            Value arr = Value::newArray();
            GMArray* a = arr.writableArray();
            a->rows.resize(1);
            if (r.eat(']')) return arr;
            do {
                if (r.eat(']')) return arr;  // trailing comma
                a->rows[0].push_back(parse(r, nullptr));
            } while (r.ok && r.eat(','));
            if (!r.eat(']')) r.ok = false;
            return arr;
        }
        if (c == '"') {
            r.p++;
            return Value(r.str());
        }
        if (r.word("true")) return Value::boolean(true);
        if (r.word("false")) return Value::boolean(false);
        if (r.word("null")) return Value();
        return Value(r.number());
    }
};

// ---- writing

void escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

// pointZero: whole numbers as "3.0", as GameMaker 2024+ writes reals in json_stringify and
// json_encode (int64s stay "3").
void number(std::string& out, double d, bool pointZero = false) {
    if (!std::isfinite(d)) {
        out += "0";
        return;
    }
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        char buf[32];
        snprintf(buf, sizeof buf, pointZero ? "%.1f" : "%.0f", d);
        out += buf;
        return;
    }
    // Shortest form that reads back as the same double.
    char buf[40];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (strtod(buf, nullptr) == d) break;
    }
    out += buf;
}

struct JsonWriter {
    Runner& rt;
    bool pretty;
    int depth = 0;
    bool pointZero = rt.dw.hasChunk("UILR");

    void newline(std::string& out) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<size_t>(depth) * 2, ' ');
    }
    void value(std::string& out, const Value& v) {
        if (v.isString()) escape(out, v.str());
        else if (v.type() == VT::Bool) out += v.truthy() ? "true" : "false";
        else if (v.isUndefined()) out += "null";
        else if (v.type() == VT::Int64) out += std::to_string(v.toInt64());
        else if (v.isNumeric()) number(out, v.num(), pointZero);
        else if (GMArray* a = v.arr()) {
            out += '[';
            depth++;
            size_t n = a->rows.empty() ? 0 : a->rows[0].size();
            for (size_t i = 0; i < n; i++) {
                if (i) out += ',';
                newline(out);
                value(out, a->rows[0][i]);
            }
            depth--;
            if (n) newline(out);
            out += ']';
        } else if (GMObject* o = v.obj()) {
            out += '{';
            depth++;
            bool first = true;
            o->inst->vars.forEachInOrder([&](int32_t id, const Value& x) {
                if (x.isMethod()) return;  // functions aren't data
                if (!first) out += ',';
                first = false;
                newline(out);
                escape(out, std::string(rt.vm.varName(id)));
                out += pretty ? ": " : ":";
                value(out, x);
            });
            depth--;
            if (!first) newline(out);
            out += '}';
        } else {
            out += "null";
        }
    }
    // json_encode: ds_map with marked nested lists/maps.
    void dsValue(std::string& out, const Value& v, uint8_t mark) {
        if (mark == DS_MARK_MAP) dsMap(out, v.toInt());
        else if (mark == DS_MARK_LIST) dsList(out, v.toInt());
        else value(out, v);
    }
    void dsMap(std::string& out, int id) {
        DsMap* m = rt.ds->maps.get(id);
        if (!m) {
            out += "null";
            return;
        }
        out += '{';
        bool first = true;
        for (auto& k : m->order) {
            auto it = m->values.find(k);
            if (it == m->values.end()) continue;
            if (!first) out += ',';
            first = false;
            escape(out, k.isString ? k.str : formatReal(k.num));
            out += ':';
            auto mk = m->marks.find(k);
            dsValue(out, it->second, mk == m->marks.end() ? DS_MARK_NONE : mk->second);
        }
        out += '}';
    }
    void dsList(std::string& out, int id) {
        auto* l = rt.ds->lists.get(id);
        if (!l) {
            out += "null";
            return;
        }
        auto marks = rt.ds->listMarks.find(id);
        out += '[';
        for (size_t i = 0; i < l->size(); i++) {
            if (i) out += ',';
            uint8_t mark = DS_MARK_NONE;
            if (marks != rt.ds->listMarks.end()) {
                auto m = marks->second.find(i);
                if (m != marks->second.end()) mark = m->second;
            }
            dsValue(out, (*l)[i], mark);
        }
        out += ']';
    }
};

}  // namespace

Value jsonToValue(Runner& rt, const std::string& text, bool& ok) {
    JsonReader r{text};
    JsonBuilder b{rt, false};
    Value v = b.parse(r, nullptr);
    ok = r.ok;
    return v;
}

GMFN(json_parse) {
    std::string text = S(0);
    JsonReader r{text};
    JsonBuilder b{rt, false};
    Value v = b.parse(r, nullptr);
    if (!r.ok) throw GMLError("json_parse: invalid JSON");
    return v;
}
GMFN(json_stringify) {
    JsonWriter w{rt, B(1)};
    std::string out;
    w.value(out, ARG(0));
    return Value(out);
}
GMFN(json_decode) {
    std::string text = S(0);
    JsonReader r{text};
    JsonBuilder b{rt, true};
    uint8_t mark = 0;
    Value v = b.parse(r, &mark);
    if (!r.ok) {
        if (mark == DS_MARK_MAP) dsDestroyMap(rt, v.toInt());
        return Value(-1.0);
    }
    if (mark == DS_MARK_MAP) return v;
    // A top-level array or value is wrapped in a map under "default", as GameMaker does.
    int id = rt.ds->maps.create();
    DsMap* m = rt.ds->maps.get(id);
    ValueKey k{true, 0, "default"};
    m->order.push_back(k);
    m->values[k] = v;
    if (mark) m->marks[k] = mark;
    return Value(static_cast<double>(id));
}
GMFN(json_encode) {
    JsonWriter w{rt, false};
    std::string out;
    w.dsMap(out, I(0));
    return Value(out);
}

void registerJsonBuiltins(Runner& rt) {
    GMREG(json_parse); GMREG(json_stringify); GMREG(json_decode); GMREG(json_encode);
}

}  // namespace gm
