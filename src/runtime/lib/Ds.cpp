// ds_list, ds_map, ds_queue, ds_stack, ds_grid, ds_priority.
#include <algorithm>
#include <cmath>

#include "runtime/Builtins.h"
#include "runtime/Stores.h"
#include "util/Fs.h"

namespace gm {

static double gDsPrecision = 0.00001;  // ds_set_precision

static bool valueEq(const Value& a, const Value& b) {
    if (a.isString() || b.isString()) return a.isString() && b.isString() && a.str() == b.str();
    if (a.isArray() || b.isArray()) return a.arr() == b.arr();
    return std::fabs(a.num() - b.num()) <= gDsPrecision;
}
GMFN(ds_set_precision) { gDsPrecision = D(0); return Value(); }

static std::vector<Value>* list(Runner& rt, const Value& id) {
    auto* l = rt.ds->lists.get(id.toInt());
    if (!l) rt.vm.warnOnce("dslist" + id.toString(), "ds_list " + id.toString() + " does not exist");
    return l;
}

// ---- ds_list
GMFN(ds_list_create) { return Value(static_cast<double>(rt.ds->lists.create())); }
// Destroying a list or map also destroys the lists and maps marked as nested in it.
void dsDestroyMap(Runner& rt, int id);
void dsDestroyList(Runner& rt, int id) {
    auto* l = rt.ds->lists.get(id);
    if (!l) return;
    auto marks = rt.ds->listMarks.find(id);
    if (marks != rt.ds->listMarks.end()) {
        auto m = std::move(marks->second);
        rt.ds->listMarks.erase(marks);
        for (auto& [i, kind] : m)
            if (i < l->size()) {
                int child = (*l)[i].toInt();
                if (kind == DS_MARK_LIST) dsDestroyList(rt, child);
                else if (kind == DS_MARK_MAP) dsDestroyMap(rt, child);
            }
    }
    rt.ds->lists.destroy(id);
}
void dsDestroyMap(Runner& rt, int id) {
    auto* m = rt.ds->maps.get(id);
    if (!m) return;
    auto marks = std::move(m->marks);
    m->marks.clear();
    for (auto& [k, kind] : marks) {
        auto it = m->values.find(k);
        if (it == m->values.end()) continue;
        int child = it->second.toInt();
        if (kind == DS_MARK_LIST) dsDestroyList(rt, child);
        else if (kind == DS_MARK_MAP) dsDestroyMap(rt, child);
    }
    rt.ds->maps.destroy(id);
}
GMFN(ds_list_destroy) { dsDestroyList(rt, I(0)); return Value(); }
GMFN(ds_list_exists) { return boolV(rt.ds->lists.get(I(0)) != nullptr); }
GMFN(ds_list_clear) { if (auto* l = list(rt, ARG(0))) l->clear(); return Value(); }
GMFN(ds_list_add) {
    if (auto* l = list(rt, ARG(0)))
        for (int i = 1; i < argc; i++) l->push_back(args[i]);
    return Value();
}
GMFN(ds_list_size) { auto* l = list(rt, ARG(0)); return Value(l ? static_cast<double>(l->size()) : 0.0); }
GMFN(ds_list_empty) { auto* l = list(rt, ARG(0)); return boolV(!l || l->empty()); }
GMFN(ds_list_find_value) {
    auto* l = list(rt, ARG(0));
    int i = I(1);
    if (!l || i < 0 || static_cast<size_t>(i) >= l->size()) return Value();
    return (*l)[i];
}
GMFN(ds_list_find_index) {
    auto* l = list(rt, ARG(0));
    if (!l) return Value(-1.0);
    for (size_t i = 0; i < l->size(); i++)
        if (valueEq((*l)[i], ARG(1))) return Value(static_cast<double>(i));
    return Value(-1.0);
}
GMFN(ds_list_delete) {
    auto* l = list(rt, ARG(0));
    int i = I(1);
    if (l && i >= 0 && static_cast<size_t>(i) < l->size()) l->erase(l->begin() + i);
    return Value();
}
GMFN(ds_list_insert) {
    auto* l = list(rt, ARG(0));
    int i = I(1);
    if (l && i >= 0 && static_cast<size_t>(i) <= l->size()) l->insert(l->begin() + i, ARG(2));
    return Value();
}
GMFN(ds_list_replace) {
    auto* l = list(rt, ARG(0));
    int i = I(1);
    if (l && i >= 0 && static_cast<size_t>(i) < l->size()) (*l)[i] = ARG(2);
    return Value();
}
GMFN(ds_list_set) {
    auto* l = list(rt, ARG(0));
    int i = I(1);
    if (l && i >= 0) {
        if (static_cast<size_t>(i) >= l->size()) l->resize(i + 1, Value(0.0));
        (*l)[i] = ARG(2);
    }
    return Value();
}
// The compiler's form of `list[| i] = v` in some GameMaker versions: like ds_list_set, returns v.
GMFN(ds_list_set_post) {
    fn_ds_list_set(rt, self, other, argc, args);
    return ARG(2);
}
GMFN(ds_list_copy) {
    auto* dst = list(rt, ARG(0));
    auto* src = list(rt, ARG(1));
    if (dst && src) *dst = *src;
    return Value();
}
GMFN(ds_list_shuffle) {
    if (auto* l = list(rt, ARG(0))) {
        for (size_t i = l->size(); i > 1; i--)
            std::swap((*l)[i - 1], (*l)[std::min(i - 1, static_cast<size_t>(rt.rng.next() * i))]);
    }
    return Value();
}
GMFN(ds_list_sort) {
    auto* l = list(rt, ARG(0));
    if (!l) return Value();
    bool asc = B(1);
    std::stable_sort(l->begin(), l->end(), [&](const Value& a, const Value& b) {
        bool lt;
        if (a.isString() && b.isString()) lt = a.str() < b.str();
        else if (a.isString() != b.isString()) lt = !a.isString();  // numbers before strings
        else lt = a.num() < b.num();
        if (asc) return lt;
        bool gt;
        if (a.isString() && b.isString()) gt = a.str() > b.str();
        else if (a.isString() != b.isString()) gt = a.isString();
        else gt = a.num() > b.num();
        return gt;
    });
    return Value();
}

// ---- ds_map
static DsMap* map(Runner& rt, const Value& id) {
    auto* m = rt.ds->maps.get(id.toInt());
    if (!m) rt.vm.warnOnce("dsmap" + id.toString(), "ds_map " + id.toString() + " does not exist");
    return m;
}
GMFN(ds_map_create) { return Value(static_cast<double>(rt.ds->maps.create())); }
GMFN(ds_map_copy) {  // (dest, source): dest's contents are replaced by source's
    DsMap* d = map(rt, ARG(0));
    DsMap* src = map(rt, ARG(1));
    if (d && src && d != src) *d = *src;
    return Value();
}
GMFN(ds_map_destroy) { dsDestroyMap(rt, I(0)); return Value(); }
static Value mapAddMarked(Runner& rt, Value* args, int argc, uint8_t kind, bool replace) {
    DsMap* m = map(rt, ARG(0));
    if (!m) return boolV(false);
    ValueKey k = ValueKey::of(ARG(1));
    bool exists = m->values.count(k) != 0;
    if (exists && !replace) return boolV(false);
    if (!exists) m->order.push_back(k);
    m->values[k] = ARG(2);
    m->marks[k] = kind;
    return boolV(true);
}
GMFN(ds_map_add_list) { return mapAddMarked(rt, args, argc, DS_MARK_LIST, false); }
GMFN(ds_map_add_map) { return mapAddMarked(rt, args, argc, DS_MARK_MAP, false); }
GMFN(ds_map_replace_list) { return mapAddMarked(rt, args, argc, DS_MARK_LIST, true); }
GMFN(ds_map_replace_map) { return mapAddMarked(rt, args, argc, DS_MARK_MAP, true); }
static bool mapMarked(Runner& rt, Value* args, int argc, uint8_t kind) {
    DsMap* m = map(rt, ARG(0));
    if (!m) return false;
    auto it = m->marks.find(ValueKey::of(ARG(1)));
    return it != m->marks.end() && it->second == kind;
}
GMFN(ds_map_is_list) { return boolV(mapMarked(rt, args, argc, DS_MARK_LIST)); }
GMFN(ds_map_is_map) { return boolV(mapMarked(rt, args, argc, DS_MARK_MAP)); }
GMFN(ds_list_mark_as_list) {
    if (list(rt, ARG(0))) rt.ds->listMarks[I(0)][static_cast<size_t>(std::max(0, I(1)))] = DS_MARK_LIST;
    return Value();
}
GMFN(ds_list_mark_as_map) {
    if (list(rt, ARG(0))) rt.ds->listMarks[I(0)][static_cast<size_t>(std::max(0, I(1)))] = DS_MARK_MAP;
    return Value();
}
static bool listMarked(Runner& rt, Value* args, int argc, uint8_t kind) {
    auto it = rt.ds->listMarks.find(I(0));
    if (it == rt.ds->listMarks.end()) return false;
    auto m = it->second.find(static_cast<size_t>(std::max(0, I(1))));
    return m != it->second.end() && m->second == kind;
}
GMFN(ds_list_is_list) { return boolV(listMarked(rt, args, argc, DS_MARK_LIST)); }
GMFN(ds_list_is_map) { return boolV(listMarked(rt, args, argc, DS_MARK_MAP)); }
GMFN(ds_map_exists) {
    DsMap* m = map(rt, ARG(0));
    return boolV(m && m->values.count(ValueKey::of(ARG(1))));
}
GMFN(ds_map_add) {
    DsMap* m = map(rt, ARG(0));
    if (!m) return boolV(false);
    ValueKey k = ValueKey::of(ARG(1));
    if (m->values.count(k)) return boolV(false);
    m->values.emplace(k, ARG(2));
    m->order.push_back(k);
    return boolV(true);
}
GMFN(ds_map_replace) {
    DsMap* m = map(rt, ARG(0));
    if (!m) return Value();
    ValueKey k = ValueKey::of(ARG(1));
    if (!m->values.count(k)) m->order.push_back(k);
    m->values[k] = ARG(2);
    return Value();
}
GMFN(ds_map_set_post) {  // `map[? k] = v`, returning v
    fn_ds_map_replace(rt, self, other, argc, args);
    return ARG(2);
}
// ds_map_keys_to_array / ds_map_values_to_array(map, [array]): appended to the array if given.
static Value mapToArray(Runner& rt, Value* args, int argc, bool keys) {
    DsMap* m = map(rt, ARG(0));
    Value arr = argc > 1 && ARG(1).isArray() ? ARG(1) : Value::newArray();
    GMArray* a = arr.arr();  // the caller's array itself (GMS 2.3+ arrays are references)
    if (a->rows.empty()) a->rows.resize(1);
    if (m)
        for (const ValueKey& k : m->order) {
            auto it = m->values.find(k);
            if (it == m->values.end()) continue;
            a->rows[0].push_back(keys ? (k.isString ? Value(k.str) : Value(k.num)) : it->second);
        }
    return arr;
}
GMFN(ds_map_keys_to_array) { return mapToArray(rt, args, argc, true); }
GMFN(ds_map_values_to_array) { return mapToArray(rt, args, argc, false); }
GMFN(ds_map_find_value) {
    DsMap* m = map(rt, ARG(0));
    if (!m) return Value();
    auto it = m->values.find(ValueKey::of(ARG(1)));
    return it == m->values.end() ? Value() : it->second;
}
GMFN(ds_map_delete) {
    DsMap* m = map(rt, ARG(0));
    if (!m) return Value();
    ValueKey k = ValueKey::of(ARG(1));
    if (m->values.erase(k)) m->order.erase(std::remove(m->order.begin(), m->order.end(), k), m->order.end());
    return Value();
}
GMFN(ds_map_size) { DsMap* m = map(rt, ARG(0)); return Value(m ? static_cast<double>(m->values.size()) : 0.0); }
GMFN(ds_map_empty) { DsMap* m = map(rt, ARG(0)); return boolV(!m || m->values.empty()); }
GMFN(ds_map_clear) {
    if (DsMap* m = map(rt, ARG(0))) {
        m->values.clear();
        m->order.clear();
    }
    return Value();
}
GMFN(ds_map_find_first) {
    DsMap* m = map(rt, ARG(0));
    if (!m || m->order.empty()) return Value();
    return m->order.front().toValue();
}
GMFN(ds_map_find_next) {
    DsMap* m = map(rt, ARG(0));
    if (!m) return Value();
    ValueKey k = ValueKey::of(ARG(1));
    for (size_t i = 0; i + 1 < m->order.size(); i++)
        if (m->order[i] == k) return m->order[i + 1].toValue();
    return Value();
}
GMFN(ds_map_find_last) {
    DsMap* m = map(rt, ARG(0));
    if (!m || m->order.empty()) return Value();
    return m->order.back().toValue();
}
GMFN(ds_map_find_previous) {
    DsMap* m = map(rt, ARG(0));
    if (!m) return Value();
    ValueKey k = ValueKey::of(ARG(1));
    for (size_t i = 1; i < m->order.size(); i++)
        if (m->order[i] == k) return m->order[i - 1].toValue();
    return Value();
}

// ---- ds_queue
GMFN(ds_queue_create) { return Value(static_cast<double>(rt.ds->queues.create())); }
GMFN(ds_queue_destroy) { rt.ds->queues.destroy(I(0)); return Value(); }
GMFN(ds_queue_clear) { if (auto* q = rt.ds->queues.get(I(0))) q->clear(); return Value(); }
GMFN(ds_queue_enqueue) {
    if (auto* q = rt.ds->queues.get(I(0)))
        for (int i = 1; i < argc; i++) q->push_back(args[i]);
    return Value();
}
GMFN(ds_queue_dequeue) {
    auto* q = rt.ds->queues.get(I(0));
    if (!q || q->empty()) return Value();
    Value v = q->front();
    q->pop_front();
    return v;
}
GMFN(ds_queue_head) { auto* q = rt.ds->queues.get(I(0)); return q && !q->empty() ? q->front() : Value(); }
GMFN(ds_queue_tail) { auto* q = rt.ds->queues.get(I(0)); return q && !q->empty() ? q->back() : Value(); }
GMFN(ds_queue_size) { auto* q = rt.ds->queues.get(I(0)); return Value(q ? static_cast<double>(q->size()) : 0.0); }
GMFN(ds_queue_empty) { auto* q = rt.ds->queues.get(I(0)); return boolV(!q || q->empty()); }

// ---- ds_stack
GMFN(ds_stack_create) { return Value(static_cast<double>(rt.ds->stacks.create())); }
GMFN(ds_stack_destroy) { rt.ds->stacks.destroy(I(0)); return Value(); }
GMFN(ds_stack_clear) { if (auto* s = rt.ds->stacks.get(I(0))) s->clear(); return Value(); }
GMFN(ds_stack_push) {
    if (auto* s = rt.ds->stacks.get(I(0)))
        for (int i = 1; i < argc; i++) s->push_back(args[i]);
    return Value();
}
GMFN(ds_stack_pop) {
    auto* s = rt.ds->stacks.get(I(0));
    if (!s || s->empty()) return Value();
    Value v = s->back();
    s->pop_back();
    return v;
}
GMFN(ds_stack_top) { auto* s = rt.ds->stacks.get(I(0)); return s && !s->empty() ? s->back() : Value(); }
GMFN(ds_stack_size) { auto* s = rt.ds->stacks.get(I(0)); return Value(s ? static_cast<double>(s->size()) : 0.0); }
GMFN(ds_stack_empty) { auto* s = rt.ds->stacks.get(I(0)); return boolV(!s || s->empty()); }

// ---- ds_grid
GMFN(ds_grid_create) {
    int id = rt.ds->grids.create();
    DsGrid* g = rt.ds->grids.get(id);
    g->w = std::max(0, I(0));
    g->h = std::max(0, I(1));
    g->cells.assign(static_cast<size_t>(g->w) * g->h, Value(0.0));
    return Value(static_cast<double>(id));
}
GMFN(ds_grid_copy) {  // (dest, source): dest becomes a copy, size included
    DsGrid* d = rt.ds->grids.get(I(0));
    DsGrid* src = rt.ds->grids.get(I(1));
    if (d && src && d != src) *d = *src;
    return Value();
}
GMFN(ds_grid_destroy) { rt.ds->grids.destroy(I(0)); return Value(); }
GMFN(ds_grid_width) { DsGrid* g = rt.ds->grids.get(I(0)); return Value(g ? g->w : 0); }
GMFN(ds_grid_height) { DsGrid* g = rt.ds->grids.get(I(0)); return Value(g ? g->h : 0); }
GMFN(ds_grid_set) {
    DsGrid* g = rt.ds->grids.get(I(0));
    int x = I(1), y = I(2);
    if (g && x >= 0 && y >= 0 && x < g->w && y < g->h) g->cells[static_cast<size_t>(y) * g->w + x] = ARG(3);
    return Value();
}
GMFN(ds_grid_get) {
    DsGrid* g = rt.ds->grids.get(I(0));
    int x = I(1), y = I(2);
    if (g && x >= 0 && y >= 0 && x < g->w && y < g->h) return g->cells[static_cast<size_t>(y) * g->w + x];
    return Value();
}
GMFN(ds_grid_clear) {
    if (DsGrid* g = rt.ds->grids.get(I(0))) std::fill(g->cells.begin(), g->cells.end(), ARG(1));
    return Value();
}
GMFN(ds_grid_resize) {
    DsGrid* g = rt.ds->grids.get(I(0));
    if (!g) return Value();
    int nw = std::max(0, I(1)), nh = std::max(0, I(2));
    if (nw == g->w) {  // rows are stored one after another: adding or dropping rows is a resize
        g->cells.resize(static_cast<size_t>(nw) * nh, Value(0.0));
        g->h = nh;
        return Value();
    }
    std::vector<Value> cells(static_cast<size_t>(nw) * nh, Value(0.0));
    for (int y = 0; y < std::min(nh, g->h); y++)
        for (int x = 0; x < std::min(nw, g->w); x++)
            cells[static_cast<size_t>(y) * nw + x] = std::move(g->cells[static_cast<size_t>(y) * g->w + x]);
    g->w = nw;
    g->h = nh;
    g->cells.swap(cells);
    return Value();
}

// Region and disk queries (YoYo's ds_grid): region corners are swapped into order and clamped
// into the grid; a disk covers the cells whose centres lie within r of (x, y).
template <typename F> static void gridRegion(DsGrid& g, int x1, int y1, int x2, int y2, F&& f) {
    if (g.w <= 0 || g.h <= 0) return;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::clamp(x1, 0, g.w - 1);
    x2 = std::clamp(x2, 0, g.w - 1);
    y1 = std::clamp(y1, 0, g.h - 1);
    y2 = std::clamp(y2, 0, g.h - 1);
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++)
            if (!f(x, y, g.cells[static_cast<size_t>(y) * g.w + x])) return;
}
template <typename F> static void gridDisk(DsGrid& g, double cx, double cy, double r, F&& f) {
    int x1 = static_cast<int>(std::max(0.0, std::floor(cx - r)));
    int x2 = static_cast<int>(std::min<double>(g.w - 1, std::ceil(cx + r)));
    int y1 = static_cast<int>(std::max(0.0, std::floor(cy - r)));
    int y2 = static_cast<int>(std::min<double>(g.h - 1, std::ceil(cy + r)));
    for (int x = x1; x <= x2; x++)
        for (int y = y1; y <= y2; y++)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r)
                if (!f(x, y, g.cells[static_cast<size_t>(y) * g.w + x])) return;
}
struct GridStats {
    Value min, max;
    double sum = 0;
    int count = 0;
    bool any = false;
    bool operator()(int, int, const Value& v) {
        if (!any) {
            min = max = v;
            any = true;
        } else {
            if (valueLess(v, min)) min = v;
            if (valueLess(max, v)) max = v;
        }
        if (!v.isString()) sum += v.num();
        count++;
        return true;
    }
    static bool valueLess(const Value& a, const Value& b) {
        if (a.isString() && b.isString()) return a.str() < b.str();
        if (a.isString() != b.isString()) return !a.isString();
        return a.num() < b.num();
    }
};
enum GridStat { GS_SUM, GS_MAX, GS_MIN, GS_MEAN };
static Value gridStat(const GridStats& s, GridStat what) {
    switch (what) {
        case GS_SUM: return Value(s.sum);
        case GS_MAX: return s.any ? s.max : Value(0.0);
        case GS_MIN: return s.any ? s.min : Value(0.0);
        default: return Value(s.count ? s.sum / s.count : 0.0);
    }
}
static Value regionStat(Runner& rt, Value* args, int argc, GridStat what) {
    GridStats s;
    if (DsGrid* g = rt.ds->grids.get(I(0))) gridRegion(*g, I(1), I(2), I(3), I(4), s);
    return gridStat(s, what);
}
static Value diskStat(Runner& rt, Value* args, int argc, GridStat what) {
    GridStats s;
    if (DsGrid* g = rt.ds->grids.get(I(0))) gridDisk(*g, D(1), D(2), D(3), s);
    return gridStat(s, what);
}
GMFN(ds_grid_get_sum) { return regionStat(rt, args, argc, GS_SUM); }
GMFN(ds_grid_get_max) { return regionStat(rt, args, argc, GS_MAX); }
GMFN(ds_grid_get_min) { return regionStat(rt, args, argc, GS_MIN); }
GMFN(ds_grid_get_mean) { return regionStat(rt, args, argc, GS_MEAN); }
GMFN(ds_grid_get_disk_sum) { return diskStat(rt, args, argc, GS_SUM); }
GMFN(ds_grid_get_disk_max) { return diskStat(rt, args, argc, GS_MAX); }
GMFN(ds_grid_get_disk_min) { return diskStat(rt, args, argc, GS_MIN); }
GMFN(ds_grid_get_disk_mean) { return diskStat(rt, args, argc, GS_MEAN); }
// value_exists / value_x / value_y over a region (args 1-4) or a disk (args 1-3); 0 exists, 1 x, 2 y.
static Value gridFind(Runner& rt, Value* args, int argc, bool disk, int what) {
    int fx = -1, fy = -1;
    const Value& want = ARG(disk ? 4 : 5);
    auto test = [&](int x, int y, const Value& v) {
        if (!valueEq(v, want)) return true;
        fx = x;
        fy = y;
        return false;
    };
    if (DsGrid* g = rt.ds->grids.get(I(0))) {
        if (disk) gridDisk(*g, D(1), D(2), D(3), test);
        else gridRegion(*g, I(1), I(2), I(3), I(4), test);
    }
    if (what == 0) return boolV(fx >= 0);
    return Value(static_cast<double>(what == 1 ? fx : fy));
}
GMFN(ds_grid_value_exists) { return gridFind(rt, args, argc, false, 0); }
GMFN(ds_grid_value_x) { return gridFind(rt, args, argc, false, 1); }
GMFN(ds_grid_value_y) { return gridFind(rt, args, argc, false, 2); }
GMFN(ds_grid_value_disk_exists) { return gridFind(rt, args, argc, true, 0); }
GMFN(ds_grid_value_disk_x) { return gridFind(rt, args, argc, true, 1); }
GMFN(ds_grid_value_disk_y) { return gridFind(rt, args, argc, true, 2); }
// set / add / multiply over a disk (add concatenates strings, as ds_grid_add).
static void diskApply(Runner& rt, Value* args, int argc, int op) {
    DsGrid* g = rt.ds->grids.get(I(0));
    if (!g) return;
    const Value& val = ARG(4);
    gridDisk(*g, D(1), D(2), D(3), [&](int, int, Value& c) {
        if (op == 0) c = val;
        else if (op == 1) c = c.isString() || val.isString() ? Value(c.toString() + val.toString()) : Value(c.num() + val.num());
        else if (!c.isString()) c = Value(c.num() * val.num());
        return true;
    });
}
GMFN(ds_grid_set_disk) { diskApply(rt, args, argc, 0); return Value(); }
GMFN(ds_grid_add_disk) { diskApply(rt, args, argc, 1); return Value(); }
GMFN(ds_grid_multiply_disk) { diskApply(rt, args, argc, 2); return Value(); }
GMFN(ds_grid_shuffle) {
    if (DsGrid* g = rt.ds->grids.get(I(0)))
        for (size_t i = g->cells.size(); i > 1; i--)
            std::swap(g->cells[i - 1], g->cells[static_cast<size_t>(rt.rng.next() * i)]);
    return Value();
}
GMFN(ds_grid_sort) {  // (grid, column, ascending): rows reordered by that column, stably
    DsGrid* g = rt.ds->grids.get(I(0));
    int col = I(1);
    if (!g || col < 0 || col >= g->w) return Value();
    bool asc = B(2);
    std::vector<int> rows(static_cast<size_t>(g->h));
    for (int y = 0; y < g->h; y++) rows[y] = asc ? y : g->h - 1 - y;  // descending ties: bottom row first
    auto key = [&](int y) -> const Value& { return g->cells[static_cast<size_t>(y) * g->w + col]; };
    std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
        return asc ? GridStats::valueLess(key(a), key(b)) : GridStats::valueLess(key(b), key(a));
    });
    std::vector<Value> cells;
    cells.reserve(g->cells.size());
    for (int y : rows)
        for (int x = 0; x < g->w; x++) cells.push_back(g->cells[static_cast<size_t>(y) * g->w + x]);
    g->cells.swap(cells);
    return Value();
}

// ---- ds_priority
GMFN(ds_priority_create) { return Value(static_cast<double>(rt.ds->priorities.create())); }
GMFN(ds_priority_destroy) { rt.ds->priorities.destroy(I(0)); return Value(); }
GMFN(ds_priority_clear) { if (auto* p = rt.ds->priorities.get(I(0))) p->clear(); return Value(); }
GMFN(ds_priority_add) {
    if (auto* p = rt.ds->priorities.get(I(0))) p->push_back({ARG(1), D(2)});
    return Value();
}
GMFN(ds_priority_size) { auto* p = rt.ds->priorities.get(I(0)); return Value(p ? static_cast<double>(p->size()) : 0.0); }
GMFN(ds_priority_empty) { auto* p = rt.ds->priorities.get(I(0)); return boolV(!p || p->empty()); }
static Value priorityTake(Runner& rt, int id, bool max, bool remove) {
    auto* p = rt.ds->priorities.get(id);
    if (!p || p->empty()) return Value();
    auto it = max ? std::max_element(p->begin(), p->end(), [](auto& a, auto& b) { return a.second < b.second; })
                  : std::min_element(p->begin(), p->end(), [](auto& a, auto& b) { return a.second < b.second; });
    Value v = it->first;
    if (remove) p->erase(it);
    return v;
}
GMFN(ds_priority_delete_max) { return priorityTake(rt, I(0), true, true); }
GMFN(ds_priority_delete_min) { return priorityTake(rt, I(0), false, true); }
GMFN(ds_priority_find_max) { return priorityTake(rt, I(0), true, false); }
GMFN(ds_priority_find_min) { return priorityTake(rt, I(0), false, false); }
GMFN(ds_priority_copy) {  // (dest, source)
    auto* d = rt.ds->priorities.get(I(0));
    auto* src = rt.ds->priorities.get(I(1));
    if (d && src && d != src) *d = *src;
    return Value();
}
GMFN(ds_priority_change_priority) {  // (id, value, priority)
    if (auto* p = rt.ds->priorities.get(I(0)))
        for (auto& e : *p)
            if (valueEq(e.first, ARG(1))) {
                e.second = D(2);
                break;
            }
    return Value();
}
GMFN(ds_priority_find_priority) {
    if (auto* p = rt.ds->priorities.get(I(0)))
        for (auto& e : *p)
            if (valueEq(e.first, ARG(1))) return Value(e.second);
    return Value();
}
GMFN(ds_priority_delete_value) {
    if (auto* p = rt.ds->priorities.get(I(0)))
        for (auto it = p->begin(); it != p->end(); ++it)
            if (valueEq(it->first, ARG(1))) {
                p->erase(it);
                break;
            }
    return Value();
}
GMFN(ds_stack_copy) {
    auto* d = rt.ds->stacks.get(I(0));
    auto* src = rt.ds->stacks.get(I(1));
    if (d && src && d != src) *d = *src;
    return Value();
}
GMFN(ds_queue_copy) {
    auto* d = rt.ds->queues.get(I(0));
    auto* src = rt.ds->queues.get(I(1));
    if (d && src && d != src) *d = *src;
    return Value();
}

// load_csv(filename): a ds_grid of strings, one row per line and one column per field (as
// RFC 4180: quoted fields may hold commas, line breaks and doubled quotes); -1 if unreadable.
GMFN(load_csv) {
    std::vector<char> text;
    if (!readFile(rt.files->readPath(S(0)), text)) return Value(-1.0);
    std::vector<std::vector<std::string>> rows(1);
    std::string field;
    bool quoted = false, fieldStarted = false;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') field += '"', i++;
            else if (c == '"') quoted = false;
            else field += c;
            continue;
        }
        if (c == '"' && !fieldStarted) {
            quoted = fieldStarted = true;
        } else if (c == ',') {
            rows.back().push_back(field);
            field.clear();
            fieldStarted = false;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') i++;
            rows.back().push_back(field);
            field.clear();
            fieldStarted = false;
            rows.emplace_back();
        } else {
            field += c;
            fieldStarted = true;
        }
    }
    if (fieldStarted || !field.empty() || !rows.back().empty()) rows.back().push_back(field);
    if (rows.back().empty()) rows.pop_back();
    size_t w = 0;
    for (auto& r : rows) w = std::max(w, r.size());
    int id = rt.ds->grids.create();
    DsGrid* g = rt.ds->grids.get(id);
    g->w = static_cast<int>(w);
    g->h = static_cast<int>(rows.size());
    g->cells.assign(w * rows.size(), Value(0.0));
    for (size_t y = 0; y < rows.size(); y++)
        for (size_t x = 0; x < rows[y].size(); x++) g->cells[y * w + x] = Value(rows[y][x]);
    return Value(static_cast<double>(id));
}

void registerDsBuiltins(Runner& rt) {
    GMREG(load_csv);
    GMREG(ds_set_precision); GMREG(ds_map_find_last); GMREG(ds_map_find_previous);
    GMREG(ds_grid_get_sum); GMREG(ds_grid_get_max); GMREG(ds_grid_get_min); GMREG(ds_grid_get_mean);
    GMREG(ds_grid_get_disk_sum); GMREG(ds_grid_get_disk_max); GMREG(ds_grid_get_disk_min); GMREG(ds_grid_get_disk_mean);
    GMREG(ds_grid_value_exists); GMREG(ds_grid_value_x); GMREG(ds_grid_value_y); GMREG(ds_grid_value_disk_exists);
    GMREG(ds_grid_value_disk_x); GMREG(ds_grid_value_disk_y); GMREG(ds_grid_set_disk); GMREG(ds_grid_add_disk);
    GMREG(ds_grid_multiply_disk); GMREG(ds_grid_shuffle); GMREG(ds_grid_sort);
    GMREG(ds_priority_copy); GMREG(ds_priority_change_priority); GMREG(ds_priority_find_priority);
    GMREG(ds_priority_delete_value); GMREG(ds_stack_copy); GMREG(ds_queue_copy);
    GMREG(ds_list_create); GMREG(ds_list_destroy); GMREG(ds_list_exists); GMREG(ds_list_clear); GMREG(ds_list_add);
    GMREG(ds_list_size); GMREG(ds_list_empty); GMREG(ds_list_find_value); GMREG(ds_list_find_index);
    GMREG(ds_list_delete); GMREG(ds_list_insert); GMREG(ds_list_replace); GMREG(ds_list_set); GMREG(ds_list_copy);
    GMREG(ds_list_shuffle); GMREG(ds_list_sort);
    GMREG(ds_map_add_list); GMREG(ds_map_add_map); GMREG(ds_map_replace_list); GMREG(ds_map_replace_map);
    GMREG(ds_map_is_list); GMREG(ds_map_is_map); GMREG(ds_list_mark_as_list); GMREG(ds_list_mark_as_map);
    GMREG(ds_list_is_list); GMREG(ds_list_is_map);
    GMREG(ds_map_create); GMREG(ds_map_destroy); GMREG(ds_map_exists); GMREG(ds_map_add); GMREG(ds_map_replace);
    GMALIAS(ds_map_set, ds_map_replace); GMREG(ds_map_set_post); GMALIAS(ds_map_set_pre, ds_map_set_post); GMREG(ds_map_keys_to_array); GMREG(ds_map_values_to_array); GMREG(ds_list_set_post); GMREG(ds_map_find_value); GMREG(ds_map_delete); GMREG(ds_map_size);
    GMREG(ds_map_empty); GMREG(ds_map_clear); GMREG(ds_map_find_first); GMREG(ds_map_find_next);
    GMREG(ds_queue_create); GMREG(ds_queue_destroy); GMREG(ds_queue_clear); GMREG(ds_queue_enqueue);
    GMREG(ds_queue_dequeue); GMREG(ds_queue_head); GMREG(ds_queue_tail); GMREG(ds_queue_size); GMREG(ds_queue_empty);
    GMREG(ds_stack_create); GMREG(ds_stack_destroy); GMREG(ds_stack_clear); GMREG(ds_stack_push); GMREG(ds_stack_pop);
    GMREG(ds_stack_top); GMREG(ds_stack_size); GMREG(ds_stack_empty);
    GMREG(ds_grid_create); GMREG(ds_grid_copy); GMREG(ds_map_copy); GMREG(ds_grid_destroy); GMREG(ds_grid_width); GMREG(ds_grid_height); GMREG(ds_grid_set);
    GMREG(ds_grid_get); GMREG(ds_grid_clear); GMREG(ds_grid_resize);
    GMREG(ds_priority_create); GMREG(ds_priority_destroy); GMREG(ds_priority_clear); GMREG(ds_priority_add);
    GMREG(ds_priority_size); GMREG(ds_priority_empty); GMREG(ds_priority_delete_max); GMREG(ds_priority_delete_min);
    GMREG(ds_priority_find_max); GMREG(ds_priority_find_min);
}

}  // namespace gm
