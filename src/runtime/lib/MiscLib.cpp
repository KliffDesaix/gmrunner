// Assorted engine functions: geometry tests, file names and file searching, alarm access,
// device mouse, clipboard, URLs, date strings, rounded rectangles, GPU state, grid regions,
// texture handles, sprite/asset info and weak references.
#include <dirent.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>

#include "runtime/Builtins.h"
#include "runtime/Graphics.h"
#include "render/Renderer.h"
#include "runtime/Stores.h"
#include "util/Fs.h"
#include "util/Utf8.h"

namespace gm {

#define G (*rt.gfx)

namespace {

// ---- geometry (as YoYo's runner, Function_Collision)
bool pointInTriangle(double px, double py, double x1, double y1, double x2, double y2, double x3, double y3) {
    double v0x = x3 - x1, v1x = x2 - x1, v2x = px - x1;
    double v0y = y3 - y1, v1y = y2 - y1, v2y = py - y1;
    double d00 = v0x * v0x + v0y * v0y, d01 = v0x * v1x + v0y * v1y, d02 = v0x * v2x + v0y * v2y;
    double d11 = v1x * v1x + v1y * v1y, d12 = v1x * v2x + v1y * v2y;
    double inv = 1.0 / (d00 * d11 - d01 * d01);
    double u = (d11 * d02 - d01 * d12) * inv, v = (d00 * d12 - d01 * d02) * inv;
    return u >= 0 && v >= 0 && u + v < 1;
}
bool pointInCircleSq(double px, double py, double cx, double cy, double r2) {
    return (px - cx) * (px - cx) + (py - cy) * (py - cy) <= r2;
}
int cornersIn(double ax1, double ay1, double ax2, double ay2, double bx1, double by1, double bx2, double by2) {
    auto in = [&](double x, double y) { return x >= bx1 && x <= bx2 && y >= by1 && y <= by2; };
    return (in(ax1, ay1) ? 1 : 0) | (in(ax2, ay1) ? 2 : 0) | (in(ax2, ay2) ? 4 : 0) | (in(ax1, ay2) ? 8 : 0);
}

// ---- file names
size_t lastSep(const std::string& s) {
    size_t a = s.find_last_of('/'), b = s.find_last_of('\\');
    if (a == std::string::npos) return b;
    if (b == std::string::npos) return a;
    return std::max(a, b);
}

// ---- file_find_*: matches in the save area and the game's own files
struct FindState {
    std::vector<std::string> results;
    size_t next = 0;
} g_find;
bool wildMatch(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') return wildMatch(p + 1, s) || (*s && wildMatch(p, s + 1));
    if (*s && (*p == '?' || std::tolower(static_cast<unsigned char>(*p)) == std::tolower(static_cast<unsigned char>(*s))))
        return wildMatch(p + 1, s + 1);
    return false;
}

std::string g_clipboard;

}  // namespace

// A field of a struct built here (sprite_get_info, gpu_get_scissor, ...).
static void structSet(Runner& rt, Value& st, const char* name, Value v) {
    rt.vm.setInstanceVar(st.obj()->inst, rt.vm.varIdForName(name), v);
}
static Value nineSliceStruct(Runner& rt, const NineSlice& n) {
    Value st = rt.vm.newStruct(nullptr);
    structSet(rt, st, "enabled", Value::boolean(n.enabled));
    structSet(rt, st, "left", Value(double(n.left)));
    structSet(rt, st, "top", Value(double(n.top)));
    structSet(rt, st, "right", Value(double(n.right)));
    structSet(rt, st, "bottom", Value(double(n.bottom)));
    Value modes = Value::newArray();
    GMArray* a = modes.writableArray();
    a->rows.resize(1);
    for (int m : n.modes) a->rows[0].push_back(Value(double(m)));
    structSet(rt, st, "tilemode", modes);
    return st;
}
GMFN(point_in_rectangle) { return boolV(D(0) >= D(2) && D(0) <= D(4) && D(1) >= D(3) && D(1) <= D(5)); }
GMFN(point_in_circle) { return boolV(pointInCircleSq(D(0), D(1), D(2), D(3), D(4) * D(4))); }
GMFN(point_in_triangle) { return boolV(pointInTriangle(D(0), D(1), D(2), D(3), D(4), D(5), D(6), D(7))); }
// 0 = apart, 1 = the first rectangle is inside the second, 2 = they overlap.
GMFN(rectangle_in_rectangle) {
    double sx1 = std::min(D(0), D(2)), sx2 = std::max(D(0), D(2)), sy1 = std::min(D(1), D(3)), sy2 = std::max(D(1), D(3));
    double dx1 = std::min(D(4), D(6)), dx2 = std::max(D(4), D(6)), dy1 = std::min(D(5), D(7)), dy2 = std::max(D(5), D(7));
    int in = cornersIn(sx1, sy1, sx2, sy2, dx1, dy1, dx2, dy2);
    if (in == 15) return Value(1.0);
    if (in != 0) return Value(2.0);
    if (cornersIn(dx1, dy1, dx2, dy2, sx1, sy1, sx2, sy2)) return Value(2.0);
    // Crossing without any corner inside: one spans the other along x or y.
    auto inX = [&](double x) { return x >= sx1 && x <= sx2; };
    auto inY = [&](double y) { return y >= dy1 && y <= dy2; };
    if ((inX(dx1) || inX(dx2)) && (inY(sy1) || inY(sy2))) return Value(2.0);
    auto inX2 = [&](double x) { return x >= dx1 && x <= dx2; };
    auto inY2 = [&](double y) { return y >= sy1 && y <= sy2; };
    if ((inX2(sx1) || inX2(sx2)) && (inY2(dy1) || inY2(dy2))) return Value(2.0);
    return Value(0.0);
}
GMFN(rectangle_in_circle) {
    double x1 = D(0), y1 = D(1), x2 = D(2), y2 = D(3), cx = D(4), cy = D(5), r2 = D(6) * D(6);
    double ix = std::max(x1, std::min(cx, x2)), iy = std::max(y1, std::min(cy, y2));
    if ((ix - cx) * (ix - cx) + (iy - cy) * (iy - cy) > r2) return Value(0.0);
    bool all = pointInCircleSq(x1, y1, cx, cy, r2) && pointInCircleSq(x2, y1, cx, cy, r2) &&
               pointInCircleSq(x2, y2, cx, cy, r2) && pointInCircleSq(x1, y2, cx, cy, r2);
    return Value(all ? 1.0 : 2.0);
}
GMFN(rectangle_in_triangle) {
    double sx1 = D(0), sy1 = D(1), sx2 = D(2), sy2 = D(3);
    double tx[3] = {D(4), D(6), D(8)}, ty[3] = {D(5), D(7), D(9)};
    int in = 0;
    double cx[4] = {sx1, sx2, sx2, sx1}, cy[4] = {sy1, sy1, sy2, sy2};
    for (int i = 0; i < 4; i++)
        if (pointInTriangle(cx[i], cy[i], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2])) in |= 1 << i;
    if (in == 15) return Value(1.0);
    if (in) return Value(2.0);
    // Separating axis test between the box and the triangle.
    double axes[5][2] = {{0, 1}, {1, 0}, {-(ty[1] - ty[0]), tx[1] - tx[0]}, {-(ty[2] - ty[1]), tx[2] - tx[1]},
                         {-(ty[0] - ty[2]), tx[0] - tx[2]}};
    for (auto& a : axes) {
        double tmin = 1e300, tmax = -1e300, bmin = 1e300, bmax = -1e300;
        for (int i = 0; i < 3; i++) {
            double d = tx[i] * a[0] + ty[i] * a[1];
            tmin = std::min(tmin, d);
            tmax = std::max(tmax, d);
        }
        for (int i = 0; i < 4; i++) {
            double d = cx[i] * a[0] + cy[i] * a[1];
            bmin = std::min(bmin, d);
            bmax = std::max(bmax, d);
        }
        if (bmax <= tmin || tmax <= bmin) return Value(0.0);
    }
    return Value(2.0);
}

// ---- strings and file names

// "#" becomes a newline; "\#" stays a literal "#".
GMFN(string_hash_to_newline) {
    std::string s = S(0), out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '#') {
            if (!out.empty() && i > 0 && s[i - 1] == '\\') out.back() = '#';
            else out += '\n';
        } else {
            out += s[i];
        }
    }
    return Value(out);
}
GMFN(filename_name) {
    std::string s = S(0);
    size_t p = lastSep(s);
    return Value(p == std::string::npos ? s : s.substr(p + 1));
}
GMFN(filename_path) {
    std::string s = S(0);
    size_t p = lastSep(s);
    return Value(p == std::string::npos ? std::string() : s.substr(0, p + 1));
}
GMFN(filename_dir) {
    std::string s = S(0);
    size_t p = lastSep(s);
    return Value(p == std::string::npos ? std::string() : s.substr(0, p));
}
GMFN(filename_drive) {
    std::string s = S(0);
    return Value(s.size() >= 2 && s[1] == ':' ? s.substr(0, 2) : std::string());
}
GMFN(filename_ext) {
    std::string s = S(0);
    size_t dot = s.find_last_of('.'), sep = lastSep(s);
    if (dot == std::string::npos || (sep != std::string::npos && sep > dot)) return Value("");
    return Value(s.substr(dot));
}
GMFN(filename_change_ext) {
    std::string s = S(0);
    size_t dot = s.find_last_of('.'), sep = lastSep(s);
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) s.erase(dot);
    return Value(s + S(1));
}

// file_find_first(mask, attributes): files matching the mask (directories only with
// fa_directory = 16), from the save area and the game's files; "" when there are none.
GMFN(file_find_first) {
    g_find = FindState();
    std::string mask = S(0);
    for (char& c : mask)
        if (c == '\\') c = '/';
    size_t p = mask.find_last_of('/');
    std::string dir = p == std::string::npos ? std::string() : mask.substr(0, p);
    std::string pattern = p == std::string::npos ? mask : mask.substr(p + 1);
    bool dirs = (I(1) & 16) != 0;
    std::set<std::string> seen;
    for (const std::string& base : {rt.saveDir, rt.gameDir}) {
        std::string path = base + "/" + rt.files->relative(dir);
        DIR* d = opendir(path.c_str());
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            bool isDirectory = isDir(path + "/" + name);
            if (isDirectory && !dirs) continue;
            if (wildMatch(pattern.c_str(), name.c_str()) && seen.insert(name).second) g_find.results.push_back(name);
        }
        closedir(d);
    }
    std::sort(g_find.results.begin(), g_find.results.end());
    return Value(g_find.results.empty() ? std::string() : g_find.results[g_find.next++]);
}
GMFN(file_find_next) {
    return Value(g_find.next < g_find.results.size() ? g_find.results[g_find.next++] : std::string());
}
GMFN(file_find_close) {
    g_find = FindState();
    return Value();
}

// ---- instance alarms
GMFN(alarm_get) {
    int a = I(0);
    return Value(self && a >= 0 && a < 12 ? self->alarm[a] : -1.0);
}
GMFN(alarm_set) {
    int a = I(0);
    if (self && a >= 0 && a < 12) self->alarm[a] = D(1);
    return Value();
}

// ---- device mouse (device 0 is the mouse; GUI coordinates are window coordinates here)
static int mouseButtonIndex(int b) { return b == 1 ? 0 : b == 2 ? 1 : b == 3 ? 2 : -1; }
static bool deviceButton(const bool* st, int b) {
    if (b == -1) return st[0] || st[1] || st[2];
    if (b == 0) return !(st[0] || st[1] || st[2]);
    int i = mouseButtonIndex(b);
    return i >= 0 && st[i];
}
// Device n's position in window pixels: the mouse for 0, touch points 1..4 after that.
static bool devicePos(Runner& rt, int dev, double& x, double& y) {
    if (dev == 0) {
        x = rt.input.mouseX;
        y = rt.input.mouseY;
        return true;
    }
    if (dev < 0 || dev >= Input::kTouches) return false;
    x = rt.input.touches[dev].x;
    y = rt.input.touches[dev].y;
    return true;
}
// Touch points only have the left "button".
static bool deviceState(Runner& rt, int dev, const bool* mouse, bool Input::Touch::*field, int b) {
    if (dev == 0) return deviceButton(mouse, b);
    if (dev < 0 || dev >= Input::kTouches) return false;
    bool on = rt.input.touches[dev].*field;
    return b == -1 || b == 1 ? on : b == 0 ? !on : false;
}
GMFN(device_mouse_x) {
    double wx, wy, x = 0, y = 0;
    if (devicePos(rt, I(0), wx, wy)) G.windowToRoom(wx, wy, x, y);
    return Value(x);
}
GMFN(device_mouse_y) {
    double wx, wy, x = 0, y = 0;
    if (devicePos(rt, I(0), wx, wy)) G.windowToRoom(wx, wy, x, y);
    return Value(y);
}
GMFN(device_mouse_raw_x) {
    double x = 0, y = 0;
    devicePos(rt, I(0), x, y);
    return Value(x);
}
GMFN(device_mouse_raw_y) {
    double x = 0, y = 0;
    devicePos(rt, I(0), x, y);
    return Value(y);
}
GMFN(device_mouse_x_to_gui) {
    double wx, wy, gx = 0, gy = 0;
    if (devicePos(rt, I(0), wx, wy)) G.windowToGui(wx, wy, gx, gy);
    return Value(gx);
}
GMFN(device_mouse_y_to_gui) {
    double wx, wy, gx = 0, gy = 0;
    if (devicePos(rt, I(0), wx, wy)) G.windowToGui(wx, wy, gx, gy);
    return Value(gy);
}
GMFN(device_mouse_check_button) { return boolV(deviceState(rt, I(0), rt.input.mouseDown, &Input::Touch::down, I(1))); }
GMFN(device_mouse_check_button_pressed) {
    return boolV(deviceState(rt, I(0), rt.input.mousePressed, &Input::Touch::pressed, I(1)));
}
GMFN(device_mouse_check_button_released) {
    return boolV(deviceState(rt, I(0), rt.input.mouseReleased, &Input::Touch::released, I(1)));
}
GMFN(device_mouse_dbclick_enable) { return Value(); }

// ---- clipboard (kept inside the runner) and URLs (not opened on a console)
GMFN(clipboard_set_text) {
    g_clipboard = S(0);
    return Value();
}
GMFN(clipboard_get_text) { return Value(g_clipboard); }
GMFN(clipboard_has_text) { return boolV(!g_clipboard.empty()); }
GMFN(url_open) {
    rt.log("[url] %s (not opened)", S(0).c_str());
    return Value();
}
GMFN(audio_system) { return Value(1.0); }  // audio_new_system

// ---- dates (date values are kept in UTC, as the runner's other date functions)

// ---- rounded rectangles: col1 at the centre, col2 at the edge; radius 10 unless given
static void roundRect(Runner& rt, double x1, double y1, double x2, double y2, double rx, double ry, uint32_t c1,
                      uint32_t c2, bool outline) {
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);
    rx = std::min(rx, (x2 - x1) / 2);
    ry = std::min(ry, (y2 - y1) / 2);
    std::vector<std::pair<double, double>> pts;
    int seg = std::max(2, G.circlePrecision / 4);
    double cxs[4] = {x2 - rx, x1 + rx, x1 + rx, x2 - rx}, cys[4] = {y1 + ry, y1 + ry, y2 - ry, y2 - ry};
    for (int c = 0; c < 4; c++)
        for (int i = 0; i <= seg; i++) {
            double a = (c * 90.0 + 90.0 * i / seg) * M_PI / 180.0;
            pts.push_back({cxs[c] + std::cos(a) * rx, cys[c] - std::sin(a) * ry});
        }
    if (outline) {
        G.primitiveBegin(3);  // pr_linestrip
        for (auto& p : pts) G.primitiveVertex(p.first, p.second, c2, G.alpha);
        G.primitiveVertex(pts[0].first, pts[0].second, c2, G.alpha);
    } else {
        G.primitiveBegin(6);  // pr_trianglefan
        G.primitiveVertex((x1 + x2) / 2, (y1 + y2) / 2, c1, G.alpha);
        for (auto& p : pts) G.primitiveVertex(p.first, p.second, c2, G.alpha);
        G.primitiveVertex(pts[0].first, pts[0].second, c2, G.alpha);
    }
    G.primitiveEnd();
}
GMFN(draw_roundrect) {
    roundRect(rt, D(0), D(1), D(2), D(3), 10, 10, G.color, G.color, B(4));
    return Value();
}
GMFN(draw_roundrect_ext) {
    roundRect(rt, D(0), D(1), D(2), D(3), D(4), D(5), G.color, G.color, B(6));
    return Value();
}
GMFN(draw_roundrect_color) {
    roundRect(rt, D(0), D(1), D(2), D(3), 10, 10, C(4), C(5), B(6));
    return Value();
}
GMFN(draw_roundrect_color_ext) {
    roundRect(rt, D(0), D(1), D(2), D(3), D(4), D(5), C(6), C(7), B(8));
    return Value();
}

// ---- stencil (gpu_set_stencil_*): kept by the renderer, applied to render targets
template <typename F> static Value stencilSet(Runner& rt, F&& f) {
    Renderer::Stencil st = G.renderer().stencil();
    f(st);
    G.renderer().setStencil(st);
    return Value();
}
GMFN(gpu_set_stencil_enable) { return stencilSet(rt, [&](Renderer::Stencil& s) { s.enabled = B(0); }); }
GMFN(gpu_set_stencil_func) { return stencilSet(rt, [&](Renderer::Stencil& s) { s.func = I(0); }); }
GMFN(gpu_set_stencil_ref) { return stencilSet(rt, [&](Renderer::Stencil& s) { s.ref = I(0) & 0xFF; }); }
GMFN(gpu_set_stencil_read_mask) { return stencilSet(rt, [&](Renderer::Stencil& s) { s.readMask = I(0) & 0xFF; }); }
GMFN(gpu_set_stencil_write_mask) { return stencilSet(rt, [&](Renderer::Stencil& s) { s.writeMask = I(0) & 0xFF; }); }
GMFN(gpu_set_stencil_fail) { return stencilSet(rt, [&](Renderer::Stencil& s) { s.fail = I(0); }); }
GMFN(gpu_set_stencil_depth_fail) { return stencilSet(rt, [&](Renderer::Stencil& s) { s.depthFail = I(0); }); }
GMFN(gpu_set_stencil_pass) { return stencilSet(rt, [&](Renderer::Stencil& s) { s.pass = I(0); }); }
GMFN(gpu_get_stencil_enable) { return boolV(G.renderer().stencil().enabled); }
GMFN(gpu_get_stencil_func) { return Value(static_cast<double>(G.renderer().stencil().func)); }
GMFN(gpu_get_stencil_ref) { return Value(static_cast<double>(G.renderer().stencil().ref)); }
GMFN(gpu_get_stencil_read_mask) { return Value(static_cast<double>(G.renderer().stencil().readMask)); }
GMFN(gpu_get_stencil_write_mask) { return Value(static_cast<double>(G.renderer().stencil().writeMask)); }
GMFN(gpu_get_stencil_fail) { return Value(static_cast<double>(G.renderer().stencil().fail)); }
GMFN(gpu_get_stencil_depth_fail) { return Value(static_cast<double>(G.renderer().stencil().depthFail)); }
GMFN(gpu_get_stencil_pass) { return Value(static_cast<double>(G.renderer().stencil().pass)); }
GMFN(draw_clear_ext) {  // (colour, alpha, depth, stencil): undefined parts are left alone
    if (!ARG(0).isUndefined()) G.drawClear(C(0), ARG(1).isUndefined() ? 1.0 : D(1));
    if (!ARG(2).isUndefined()) G.renderer().clearDepth();
    if (!ARG(3).isUndefined()) G.renderer().clearStencil(I(3));
    return Value();
}

// ---- GPU state
GMFN(gpu_push_state) {
    G.gpuStack.push_back(G.gpuState());
    return Value();
}
GMFN(gpu_pop_state) {
    if (!G.gpuStack.empty()) {
        G.setGpuState(G.gpuStack.back());
        G.gpuStack.pop_back();
    }
    return Value();
}
GMFN(gpu_get_blendmode) { return Value(static_cast<double>(G.blendMode())); }
GMFN(gpu_get_blendmode_src) { return Value(static_cast<double>(G.blendSrc())); }
GMFN(gpu_get_blendmode_dest) { return Value(static_cast<double>(G.blendDst())); }
GMFN(gpu_get_blendenable) { return boolV(G.alphaBlendOn()); }
GMFN(gpu_get_alphatestenable) { return boolV(G.alphaTestOn()); }
GMFN(gpu_get_alphatestref) { return Value(G.alphaTestRef()); }
GMFN(gpu_get_texfilter) { return boolV(G.texFilter()); }
GMFN(gpu_get_colorwriteenable) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    for (int i = 0; i < 4; i++) a->rows[0].push_back(Value::boolean(G.colorMask()[i]));
    return arr;
}
GMFN(gpu_get_fog) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    a->rows[0] = {Value::boolean(G.fogOn()), Value(static_cast<double>(G.fogColor())), Value(0.0), Value(1.0)};
    return arr;
}

// ---- grid regions: ds_grid_*_grid_region(dest, source, x1, y1, x2, y2, xpos, ypos)
static Value gridRegion(Runner& rt, Value* args, int argc, int op) {
    DsGrid* d = rt.ds->grids.get(I(0));
    DsGrid* s = rt.ds->grids.get(I(1));
    if (!d || !s) return Value();
    int x1 = std::max(0, std::min(I(2), I(4))), x2 = std::min(s->w - 1, std::max(I(2), I(4)));
    int y1 = std::max(0, std::min(I(3), I(5))), y2 = std::min(s->h - 1, std::max(I(3), I(5)));
    std::vector<Value> copy;  // source and destination may be the same grid
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++) copy.push_back(s->cells[static_cast<size_t>(y) * s->w + x]);
    size_t k = 0;
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++, k++) {
            int dx = I(6) + (x - x1), dy = I(7) + (y - y1);
            if (dx < 0 || dy < 0 || dx >= d->w || dy >= d->h) continue;
            Value& cell = d->cells[static_cast<size_t>(dy) * d->w + dx];
            const Value& v = copy[k];
            if (op == 0) cell = v;
            else if (op == 1) cell = cell.isString() || v.isString() ? Value(cell.toString() + v.toString()) : Value(cell.num() + v.num());
            else cell = Value(cell.num() * v.num());
        }
    return Value();
}
GMFN(ds_grid_set_grid_region) { return gridRegion(rt, args, argc, 0); }
GMFN(ds_grid_add_grid_region) { return gridRegion(rt, args, argc, 1); }
GMFN(ds_grid_multiply_grid_region) { return gridRegion(rt, args, argc, 2); }


// ---- more GPU state
GMFN(gpu_set_blendmode_ext_sepalpha) {  // (src, dst, srcalpha, dstalpha) or one array
    if (GMArray* a = ARG(0).arr()) {
        if (!a->rows.empty() && a->rows[0].size() >= 4) {
            auto& r = a->rows[0];
            G.setBlendModeExtSepAlpha(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt());
        }
    } else {
        G.setBlendModeExtSepAlpha(I(0), I(1), I(2), I(3));
    }
    return Value();
}
static Value numArray(std::initializer_list<double> v) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    for (double d : v) a->rows[0].push_back(Value(d));
    return arr;
}
GMFN(gpu_get_blendmode_ext) { return numArray({double(G.blendSrc()), double(G.blendDst())}); }
GMFN(gpu_get_blendmode_ext_sepalpha) {
    return numArray({double(G.blendSrc()), double(G.blendDst()), double(G.blendSrcAlpha()), double(G.blendDstAlpha())});
}
GMFN(gpu_get_blendmode_srcalpha) { return Value(static_cast<double>(G.blendSrcAlpha())); }
GMFN(gpu_get_blendmode_destalpha) { return Value(static_cast<double>(G.blendDstAlpha())); }
GMFN(gpu_set_tex_filter) { G.setTexFilter(B(0)); return Value(); }
GMFN(gpu_set_tex_repeat) { G.setTexRepeat(B(0)); return Value(); }
// The _ext forms take a sampler: stage 0 is the base texture; other stages keep the defaults.
GMFN(gpu_set_tex_repeat_ext) { if (I(0) <= 0) G.setTexRepeat(B(1)); return Value(); }
GMFN(gpu_get_tex_repeat_ext) { return boolV(I(0) <= 0 && G.texRepeat); }
GMFN(gpu_set_tex_filter_ext) { if (I(0) <= 0) G.setTexFilter(B(1)); return Value(); }
GMFN(gpu_get_tex_filter_ext) { return boolV(I(0) <= 0 && G.texFilter()); }
GMFN(gpu_set_scissor) {  // (x, y, w, h) or a struct with x, y, w, h
    if (ARG(0).isObject()) {
        Instance* st = ARG(0).obj()->inst;
        auto get = [&](const char* k) { return rt.vm.getInstanceVar(st, rt.vm.varIdForName(k)).num(); };
        G.setScissor(static_cast<int>(get("x")), static_cast<int>(get("y")), static_cast<int>(get("w")),
                     static_cast<int>(get("h")));
    } else {
        G.setScissor(I(0), I(1), I(2), I(3));
    }
    return Value();
}
GMFN(gpu_get_scissor) {
    int x, y, w, h;
    G.scissor(x, y, w, h);
    Value st = rt.vm.newStruct(nullptr);
    structSet(rt, st, "x", Value(static_cast<double>(x)));
    structSet(rt, st, "y", Value(static_cast<double>(y)));
    structSet(rt, st, "w", Value(static_cast<double>(w)));
    structSet(rt, st, "h", Value(static_cast<double>(h)));
    return st;
}
GMFN(texture_set_blending) { G.setAlphaBlend(B(0)); return Value(); }
GMFN(shader_set_uniform_matrix) { G.shaderUniformWorldMatrix(I(0)); return Value(); }
GMFN(shader_set_uniform_f_buffer) {  // (uniform, buffer, offset, count): floats read from the buffer
    std::vector<uint8_t>* b = bufferBytes(I(1));
    int off = std::max(0, I(2)), n = std::max(0, I(3));
    if (!b || n == 0) return Value();
    std::vector<float> v;
    for (int i = 0; i < n && static_cast<size_t>(off) + (i + 1) * 4 <= b->size(); i++) {
        float f;
        std::memcpy(&f, b->data() + off + i * 4, 4);
        v.push_back(f);
    }
    if (!v.empty()) G.shaderUniformArray(I(0), v.data(), static_cast<int>(v.size()));
    return Value();
}
GMFN(gpu_get_tex_repeat) { return boolV(G.texRepeat); }
static Value floatArrayUniform(Runner& rt, Value* args, int argc) {
    std::vector<float> v;
    if (GMArray* a = ARG(1).arr())
        if (!a->rows.empty())
            for (auto& x : a->rows[0]) v.push_back(static_cast<float>(x.num()));
    G.shaderUniformArray(I(0), v.data(), static_cast<int>(v.size()));
    return Value();
}
GMFN(shader_set_uniform_f_array) { return floatArrayUniform(rt, args, argc); }
GMFN(shader_set_uniform_i_array) { return floatArrayUniform(rt, args, argc); }

// ---- textures
GMFN(sprite_get_texture) {
    return texValue(G.spriteTexture(I(0), D(1)));
}
GMFN(sprite_get_uvs) {
    double uv[8];
    if (!G.spriteUVs(I(0), D(1), uv)) return Value();
    return numArray({uv[0], uv[1], uv[2], uv[3], uv[4], uv[5], uv[6], uv[7]});
}
GMFN(surface_get_texture) {
    return texValue(G.surfaceExists(I(0)) ? Graphics::kSurfaceTexBase + I(0) : -1);
}
GMFN(texture_get_width) {
    uint32_t t;
    int w, h;
    return Value(G.textureInfo(texArg(ARG(0)), t, w, h) ? 1.0 : 0.0);
}
// Texture groups: every texture page is loaded at start, so a group lists all pages and every
// texture is ready (texturegroup_status_fetched).
GMFN(texturegroup_get_textures) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    for (size_t i = 0; i < G.pages().size(); i++) a->rows[0].push_back(Value(static_cast<double>(i)));
    return arr;
}
GMFN(texturegroup_get_sprites) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    for (size_t i = 0; i < rt.dw.sprites.size(); i++) a->rows[0].push_back(Value(static_cast<double>(i)));
    return arr;
}
GMFN(texturegroup_get_fonts) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    for (size_t i = 0; i < rt.dw.fonts.size(); i++) a->rows[0].push_back(Value(static_cast<double>(i)));
    return arr;
}
GMFN(texturegroup_get_tilesets) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    for (size_t i = 0; i < rt.dw.backgrounds.size(); i++) a->rows[0].push_back(Value(static_cast<double>(i)));
    return arr;
}
GMFN(texturegroup_get_status) { return Value(3.0); }  // texturegroup_status_fetched
GMFN(texturegroup_load) { return Value(0.0); }
GMFN(texturegroup_unload) { return Value(); }
GMFN(texturegroup_exists) { return boolV(true); }
GMFN(texture_is_ready) { return boolV(true); }
GMFN(texture_get_uvs) {  // a whole texture page: left, top, right, bottom, then offsets and ratios
    return numArray({0, 0, 1, 1, 0, 0, 1, 1});
}
GMFN(texture_get_texel_width) {
    uint32_t t;
    int w = 0, h = 0;
    return Value(G.textureInfo(texArg(ARG(0)), t, w, h) && w > 0 ? 1.0 / w : 1.0);
}
GMFN(texture_get_texel_height) {
    uint32_t t;
    int w = 0, h = 0;
    return Value(G.textureInfo(texArg(ARG(0)), t, w, h) && h > 0 ? 1.0 / h : 1.0);
}
GMFN(draw_sprite_pos) {  // (sprite, subimg, x1, y1, x2, y2, x3, y3, x4, y4, alpha)
    double sub = D(1);
    if (sub < 0 && self) sub = self->imageIndex;
    double px[4] = {D(2), D(4), D(6), D(8)}, py[4] = {D(3), D(5), D(7), D(9)};
    G.drawSpritePos(I(0), sub, px, py, D(10));
    return Value();
}

// ---- sprite and asset info
GMFN(sprite_get_info) {
    int si = I(0);
    if (si < 0 || static_cast<size_t>(si) >= rt.dw.sprites.size()) return Value();
    const Sprite& sp = rt.dw.sprites[si];
    Value st = rt.vm.newStruct(nullptr);
    structSet(rt, st, "width", Value(double(sp.width)));
    structSet(rt, st, "height", Value(double(sp.height)));
    structSet(rt, st, "xoffset", Value(double(sp.originX)));
    structSet(rt, st, "yoffset", Value(double(sp.originY)));
    structSet(rt, st, "transparent", Value::boolean(sp.transparent));
    structSet(rt, st, "smooth", Value::boolean(sp.smooth));
    structSet(rt, st, "preload", Value::boolean(sp.preload));
    structSet(rt, st, "type", Value(0.0));
    structSet(rt, st, "bbox_left", Value(double(sp.marginLeft)));
    structSet(rt, st, "bbox_right", Value(double(sp.marginRight)));
    structSet(rt, st, "bbox_top", Value(double(sp.marginTop)));
    structSet(rt, st, "bbox_bottom", Value(double(sp.marginBottom)));
    structSet(rt, st, "name", Value(std::string(sp.name)));
    structSet(rt, st, "num_subimages", Value(double(sp.frames.size())));
    structSet(rt, st, "frame_speed", Value(double(sp.playbackSpeed)));
    structSet(rt, st, "frame_type", Value(double(sp.playbackSpeedType)));
    structSet(rt, st, "use_mask", Value::boolean(!sp.masks.empty()));
    structSet(rt, st, "num_masks", Value(double(sp.masks.size())));
    Value frames = Value::newArray();
    GMArray* fa = frames.writableArray();
    fa->rows.resize(1);
    for (int32_t tp : sp.frames) {
        if (tp < 0 || static_cast<size_t>(tp) >= rt.dw.tpag.size()) continue;
        const TexturePageItem& t = rt.dw.tpag[tp];
        Value f = rt.vm.newStruct(nullptr);
        structSet(rt, f, "x", Value(double(t.srcX)));
        structSet(rt, f, "y", Value(double(t.srcY)));
        structSet(rt, f, "w", Value(double(t.srcW)));
        structSet(rt, f, "h", Value(double(t.srcH)));
        structSet(rt, f, "x_offset", Value(double(t.tgtX)));
        structSet(rt, f, "y_offset", Value(double(t.tgtY)));
        structSet(rt, f, "crop_width", Value(double(t.tgtW)));
        structSet(rt, f, "crop_height", Value(double(t.tgtH)));
        structSet(rt, f, "original_width", Value(double(t.boundW)));
        structSet(rt, f, "original_height", Value(double(t.boundH)));
        structSet(rt, f, "texture", texValue(t.texture));
        fa->rows[0].push_back(f);
    }
    structSet(rt, st, "frames", frames);
    structSet(rt, st, "nineslice", nineSliceStruct(rt, sp.nineSlice));
    return st;
}

// Nine-slice settings as GameMaker's struct (enabled, left, top, right, bottom, tilemode[5]).
// sprite_get_nineslice returns a copy; sprite_set_nineslice applies one.
GMFN(sprite_nineslice_create) {
    GM_UNUSED_FN_ARGS;
    return nineSliceStruct(rt, NineSlice());
}
GMFN(sprite_get_nineslice) {
    int si = I(0);
    if (si < 0 || static_cast<size_t>(si) >= rt.dw.sprites.size()) return Value();
    return nineSliceStruct(rt, rt.dw.sprites[si].nineSlice);
}
GMFN(sprite_set_nineslice) {
    int si = I(0);
    Instance* o = ARG(1).obj() ? ARG(1).obj()->inst : nullptr;
    if (si < 0 || static_cast<size_t>(si) >= rt.dw.sprites.size() || !o) return Value();
    NineSlice& n = rt.dw.sprites[si].nineSlice;
    auto num = [&](const char* name) { return rt.vm.getInstanceVar(o, rt.vm.varIdForName(name)).toInt(); };
    n.enabled = rt.vm.getInstanceVar(o, rt.vm.varIdForName("enabled")).truthy();
    n.left = num("left");
    n.top = num("top");
    n.right = num("right");
    n.bottom = num("bottom");
    Value modes = rt.vm.getInstanceVar(o, rt.vm.varIdForName("tilemode"));
    if (GMArray* a = modes.arr(); a && !a->rows.empty())
        for (size_t i = 0; i < 5 && i < a->rows[0].size(); i++) n.modes[i] = a->rows[0][i].toInt();
    return Value();
}

// Tilesets: size, tile layout and the texture they live on.
static const Background* tilesetOf(Runner& rt, int i) {
    return i >= 0 && static_cast<size_t>(i) < rt.dw.backgrounds.size() ? &rt.dw.backgrounds[i] : nullptr;
}
GMFN(tileset_get_texture) {
    const Background* b = tilesetOf(rt, I(0));
    if (!b || b->tpag < 0 || static_cast<size_t>(b->tpag) >= rt.dw.tpag.size()) return texValue(-1);
    return texValue(rt.dw.tpag[b->tpag].texture);
}
GMFN(tileset_get_uvs) {  // [left, top, right, bottom] in the texture, 0..1
    const Background* b = tilesetOf(rt, I(0));
    std::vector<Value> uv;
    if (b && b->tpag >= 0 && static_cast<size_t>(b->tpag) < rt.dw.tpag.size()) {
        const TexturePageItem& t = rt.dw.tpag[b->tpag];
        int w = 0, h = 0;
        uint32_t glTex = 0;
        G.textureInfo(t.texture, glTex, w, h);
        double tw = w > 0 ? w : 1, th = h > 0 ? h : 1;
        uv = {Value(t.srcX / tw), Value(t.srcY / th), Value((t.srcX + t.srcW) / tw), Value((t.srcY + t.srcH) / th),
              Value(double(t.tgtX)), Value(double(t.tgtY)), Value(t.boundW ? double(t.tgtW) / t.boundW : 1.0),
              Value(t.boundH ? double(t.tgtH) / t.boundH : 1.0)};
    }
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    a->rows[0] = std::move(uv);
    return arr;
}
GMFN(tileset_get_info) {
    const Background* b = tilesetOf(rt, I(0));
    if (!b) return Value();
    Value st = rt.vm.newStruct(nullptr);
    double w = 0, h = 0;
    int tex = -1;
    if (b->tpag >= 0 && static_cast<size_t>(b->tpag) < rt.dw.tpag.size()) {
        const TexturePageItem& t = rt.dw.tpag[b->tpag];
        w = t.boundW, h = t.boundH, tex = t.texture;
    }
    structSet(rt, st, "width", Value(w));
    structSet(rt, st, "height", Value(h));
    structSet(rt, st, "texture", texValue(tex));
    structSet(rt, st, "tile_width", Value(double(b->tileWidth)));
    structSet(rt, st, "tile_height", Value(double(b->tileHeight)));
    structSet(rt, st, "tile_horizontal_separator", Value(double(b->borderX)));
    structSet(rt, st, "tile_vertical_separator", Value(double(b->borderY)));
    structSet(rt, st, "tile_columns", Value(double(b->columns)));
    structSet(rt, st, "tile_count", Value(double(b->tileCount)));
    structSet(rt, st, "frame_count", Value(double(b->framesPerTile)));
    structSet(rt, st, "frame_length_ms", Value(b->frameTime / 1000.0));
    // frames: tile index -> its animation frames (only tiles that animate)
    Value frames = rt.vm.newStruct(nullptr);
    if (b->framesPerTile > 1)
        for (uint32_t i = 0; i < b->tileCount; i++) {
            std::vector<Value> f;
            for (uint32_t k = 0; k < b->framesPerTile && i * b->framesPerTile + k < b->tileIds.size(); k++)
                f.push_back(Value(double(b->tileIds[i * b->framesPerTile + k])));
            bool animated = false;
            for (auto& v : f) animated |= v.toInt() != f[0].toInt();
            if (!animated) continue;
            Value arr = Value::newArray();
            GMArray* a = arr.writableArray();
            a->rows.resize(1);
            a->rows[0] = std::move(f);
            structSet(rt, frames, std::to_string(i).c_str(), arr);
        }
    structSet(rt, st, "frames", frames);
    return st;
}

// room_get_info(room, [views], [instances], [layers], [layer_elements], [tilemap_data]): the
// room as designed (not its current state), with YoYo's field names.
static Value arrayValue(std::vector<Value> items) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    a->rows[0] = std::move(items);
    return arr;
}
GMFN(room_get_info) {
    int r = I(0);
    if (r < 0 || static_cast<size_t>(r) >= rt.dw.rooms.size()) return Value();
    const Room& rm = rt.dw.rooms[r];
    auto opt = [&](int i) { return argc <= i || ARG(i).isUndefined() || ARG(i).truthy(); };
    bool wantViews = opt(1), wantInstances = opt(2), wantLayers = opt(3), wantElements = opt(4), wantTiles = opt(5);
    auto num = [](double v) { return Value(v); };
    Value st = rt.vm.newStruct(nullptr);
    structSet(rt, st, "width", num(rm.width));
    structSet(rt, st, "height", num(rm.height));
    structSet(rt, st, "persistent", Value::boolean(rm.persistent));
    structSet(rt, st, "colour", num(rm.bgColor & 0xFFFFFF));
    structSet(rt, st, "creationCode", num(rm.creationCode));  // the code entry (YoYo's runner: the function)
    structSet(rt, st, "physicsWorld", Value::boolean(false));
    structSet(rt, st, "enableViews", Value::boolean(rm.flags & 1));
    structSet(rt, st, "clearDisplayBuffer", Value::boolean((rm.flags & 4) == 0));
    structSet(rt, st, "clearViewportBackground", Value::boolean(rm.drawBgColor));
    if (wantViews) {
        std::vector<Value> views;
        for (const RoomView& v : rm.views) {
            Value s = rt.vm.newStruct(nullptr);
            structSet(rt, s, "visible", Value::boolean(v.enabled));
            structSet(rt, s, "xview", num(v.viewX));
            structSet(rt, s, "yview", num(v.viewY));
            structSet(rt, s, "wview", num(v.viewW));
            structSet(rt, s, "hview", num(v.viewH));
            structSet(rt, s, "xport", num(v.portX));
            structSet(rt, s, "yport", num(v.portY));
            structSet(rt, s, "wport", num(v.portW));
            structSet(rt, s, "hport", num(v.portH));
            structSet(rt, s, "hborder", num(v.borderX));
            structSet(rt, s, "vborder", num(v.borderY));
            structSet(rt, s, "hspeed", num(v.speedX));
            structSet(rt, s, "vspeed", num(v.speedY));
            structSet(rt, s, "object", num(v.follow));
            structSet(rt, s, "cameraID", num(-1));
            views.push_back(s);
        }
        structSet(rt, st, "views", arrayValue(std::move(views)));
    }
    if (wantInstances) {
        std::vector<Value> insts;
        for (const RoomInstance& in : rm.instances) {
            Value s = rt.vm.newStruct(nullptr);
            structSet(rt, s, "x", num(in.x));
            structSet(rt, s, "y", num(in.y));
            bool validObj = in.object >= 0 && static_cast<size_t>(in.object) < rt.dw.objects.size();
            structSet(rt, s, "object_index", Value(validObj ? std::string(rt.dw.objects[in.object].name) : std::string()));
            structSet(rt, s, "id", num(in.id));
            structSet(rt, s, "angle", num(in.rotation));
            structSet(rt, s, "xscale", num(in.scaleX));
            structSet(rt, s, "yscale", num(in.scaleY));
            structSet(rt, s, "image_speed", num(in.imageSpeed));
            structSet(rt, s, "image_index", num(in.imageIndex));
            structSet(rt, s, "colour", num(in.color & 0xFFFFFF));
            structSet(rt, s, "creation_code", num(in.creationCode));
            structSet(rt, s, "pre_creation_code", num(in.preCreateCode));
            insts.push_back(s);
        }
        structSet(rt, st, "instances", arrayValue(std::move(insts)));
    }
    if (wantLayers) {
        std::vector<Value> layers;
        for (const RoomLayer& L : rm.layers) {
            Value s = rt.vm.newStruct(nullptr);
            structSet(rt, s, "name", Value(std::string(L.name)));
            structSet(rt, s, "id", num(L.id));
            structSet(rt, s, "type", num(static_cast<double>(L.type)));
            structSet(rt, s, "depth", num(L.depth));
            structSet(rt, s, "xoffset", num(L.xOffset));
            structSet(rt, s, "yoffset", num(L.yOffset));
            structSet(rt, s, "hspeed", num(L.hSpeed));
            structSet(rt, s, "vspeed", num(L.vSpeed));
            structSet(rt, s, "visible", Value::boolean(L.visible));
            structSet(rt, s, "effectEnabled", Value::boolean(L.fxEnabled));
            structSet(rt, s, "effectType", L.fxType.empty() ? num(-1) : Value(std::string(L.fxType)));
            std::vector<Value> params;
            for (auto& [name, value] : L.fxParams) {
                Value p = rt.vm.newStruct(nullptr);
                structSet(rt, p, "name", Value(std::string(name)));
                structSet(rt, p, "value", Value(std::string(value)));
                params.push_back(p);
            }
            structSet(rt, s, "effectParams", arrayValue(std::move(params)));
            if (wantElements) {
                std::vector<Value> els;
                auto element = [&](int type) {
                    Value e = rt.vm.newStruct(nullptr);
                    structSet(rt, e, "type", num(type));
                    els.push_back(e);
                    return e;
                };
                auto blendAlpha = [&](Value& e, uint32_t c, const char* blend, const char* alpha) {
                    structSet(rt, e, blend, num(c & 0xFFFFFF));
                    structSet(rt, e, alpha, num(((c >> 24) & 0xFF) / 255.0));
                };
                switch (L.type) {
                    case LayerType::Background: {
                        Value e = element(1);
                        structSet(rt, e, "visible", Value::boolean(L.bgVisible));
                        structSet(rt, e, "foreground", Value::boolean(L.bgForeground));
                        structSet(rt, e, "sprite_index", num(L.bgSprite));
                        structSet(rt, e, "htiled", Value::boolean(L.bgHTiled));
                        structSet(rt, e, "vtiled", Value::boolean(L.bgVTiled));
                        structSet(rt, e, "stretch", Value::boolean(L.bgStretch));
                        blendAlpha(e, L.bgColor, "blendColour", "blendAlpha");
                        structSet(rt, e, "image_speed", num(L.bgAnimSpeed));
                        structSet(rt, e, "image_index", num(L.bgFirstFrame));
                        structSet(rt, e, "speed_type", num(L.bgAnimSpeedType));
                        break;
                    }
                    case LayerType::Instances:
                        for (uint32_t id : L.instanceIds) {
                            Value e = element(2);
                            structSet(rt, e, "inst_id", num(id));
                        }
                        break;
                    case LayerType::Tiles: {
                        Value e = element(5);
                        structSet(rt, e, "x", num(0));
                        structSet(rt, e, "y", num(0));
                        structSet(rt, e, "width", num(L.tilesX));
                        structSet(rt, e, "height", num(L.tilesY));
                        structSet(rt, e, "tileset_index", num(L.tileset));
                        if (wantTiles) {
                            std::vector<Value> tiles;
                            for (uint32_t t : L.tiles) tiles.push_back(num(t));
                            structSet(rt, e, "tiles", arrayValue(std::move(tiles)));
                        }
                        break;
                    }
                    case LayerType::Assets: {
                        for (const RoomTile& t : L.legacyTiles) {
                            Value e = element(7);
                            structSet(rt, e, "x", num(t.x));
                            structSet(rt, e, "y", num(t.y));
                            structSet(rt, e, "sprite_index", num(t.bg));
                            structSet(rt, e, "xo", num(t.srcX));
                            structSet(rt, e, "yo", num(t.srcY));
                            structSet(rt, e, "width", num(t.w));
                            structSet(rt, e, "height", num(t.h));
                            structSet(rt, e, "id", num(t.id));
                            structSet(rt, e, "image_xscale", num(t.scaleX));
                            structSet(rt, e, "image_yscale", num(t.scaleY));
                            blendAlpha(e, t.color, "image_blend", "image_alpha");
                            structSet(rt, e, "visible", Value::boolean(true));
                        }
                        auto assets = [&](const std::vector<RoomSpriteAsset>& list, int type) {
                            for (const RoomSpriteAsset& a : list) {
                                Value e = element(type);
                                structSet(rt, e, "id", Value(std::string(a.name)));
                                structSet(rt, e, type == 8 ? "seq_id" : "sprite_index", num(a.sprite));
                                structSet(rt, e, "x", num(a.x));
                                structSet(rt, e, "y", num(a.y));
                                structSet(rt, e, "image_xscale", num(a.scaleX));
                                structSet(rt, e, "image_yscale", num(a.scaleY));
                                blendAlpha(e, a.color, "image_blend", "image_alpha");
                                structSet(rt, e, "speed_type", num(a.animSpeedType));
                                structSet(rt, e, "image_speed", num(a.animSpeed));
                                structSet(rt, e, type == 8 ? "head_position" : "image_index", num(a.frameIndex));
                                structSet(rt, e, type == 8 ? "angle" : "image_angle", num(a.rotation));
                            }
                        };
                        assets(L.sprites, 4);
                        assets(L.sequences, 8);
                        break;
                    }
                    default: break;
                }
                structSet(rt, s, "elements", arrayValue(std::move(els)));
            }
            layers.push_back(s);
        }
        structSet(rt, st, "layers", arrayValue(std::move(layers)));
    }
    return st;
}

// draw_getpixel(x, y): the colour at a pixel of the current target (the application surface,
// a surface, ...); _ext adds the alpha in the top byte.
GMFN(draw_getpixel) {
    std::vector<uint8_t> px;
    if (!G.readCurrentTarget(I(0), I(1), 1, 1, px)) return Value(0.0);
    return Value(static_cast<double>(px[0] | px[1] << 8 | px[2] << 16));
}
GMFN(draw_getpixel_ext) {
    std::vector<uint8_t> px;
    if (!G.readCurrentTarget(I(0), I(1), 1, 1, px)) return Value(0.0);
    return Value(static_cast<double>(static_cast<uint32_t>(px[0] | px[1] << 8 | px[2] << 16) |
                                     static_cast<uint32_t>(px[3]) << 24));
}
GMFN(gpu_set_blendequation) {
    G.setBlendEquation(I(0), I(0));
    return Value();
}
GMFN(gpu_set_blendequation_sepalpha) {
    G.setBlendEquation(I(0), I(1));
    return Value();
}
GMFN(gpu_get_blendequation) { return Value(static_cast<double>(G.blendEquation())); }
GMFN(gpu_get_blendequation_sepalpha) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    a->rows[0] = {Value(static_cast<double>(G.blendEquation())), Value(static_cast<double>(G.blendEquationAlpha()))};
    return arr;
}
// asset_get_ids(type): asset_object 0, sprite 1, sound 2, room 3, path 4, script 5, font 6,
// timeline 7, shader 8, sequence 9, animation curve 10, particle system 11, tileset 13.
// An asset_* constant from the game in GameMaker 2024's numbering, which the runner uses inside
// (asset_get_type in Gms2Lib.cpp): older games number 4 background/tiles, 5 path, 6 script,
// 7 font, 8 timeline, 10 shader.
static int modernAssetType(Runner& rt, int type) {
    if (rt.dw.hasChunk("UILR")) return type;
    static const int toModern[11] = {0, 1, 2, 3, 13, 4, 5, 6, 7, -1, 8};
    return type >= 0 && type < 11 ? toModern[type] : -1;
}

GMFN(asset_get_ids) {
    size_t n = 0;
    switch (modernAssetType(rt, I(0))) {
        case 0: n = rt.dw.objects.size(); break;
        case 1: n = rt.dw.sprites.size(); break;
        case 2: n = rt.dw.sounds.size(); break;
        case 3: n = rt.dw.rooms.size(); break;
        case 4: n = rt.dw.paths.size(); break;
        case 5: n = rt.dw.scripts.size(); break;
        case 6: n = rt.dw.fonts.size(); break;
        case 7: n = rt.dw.timelines.size(); break;
        case 8: n = rt.dw.shaders.size(); break;
        case 13: n = rt.dw.backgrounds.size(); break;
        default: break;
    }
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    for (size_t i = 0; i < n; i++) a->rows[0].push_back(Value(static_cast<double>(i)));
    return arr;
}

// ---- window, surfaces, misc
GMFN(application_get_position) {  // the application surface fills the game window
    return numArray({0, 0, double(G.windowWidth()), double(G.windowHeight())});
}
static bool gSurfaceDepthDisabled = false;
GMFN(surface_depth_disable) { gSurfaceDepthDisabled = B(0); return Value(); }
GMFN(surface_get_depth_disable) { return boolV(gSurfaceDepthDisabled); }
GMFN(surface_format_is_supported) { return boolV(I(0) == 0); }  // surface_rgba8unorm only
// Surfaces are always 8-bit RGBA (surface_rgba8unorm = 0), whatever format was asked for.
GMFN(surface_get_format) { return Value(G.surfaceExists(I(0)) ? 0.0 : -1.0); }
GMFN(surface_create_ext) {  // (name, w, h): an HTML5 canvas-backed surface elsewhere; a plain one here
    return Value(static_cast<double>(G.surfaceCreate(I(1), I(2))));
}
// Extension options (2022.2+) aren't loaded from the data: no option has a value.
GMFN(extension_get_option_value) { GM_UNUSED_FN_ARGS; return Value(); }
GMFN(extension_get_options) { GM_UNUSED_FN_ARGS; return rt.vm.newStruct(nullptr); }
// Handles (typed references, 2023.8+) are plain numbers in this runner.
GMFN(is_handle) { GM_UNUSED_FN_ARGS; return Value::boolean(false); }
GMFN(audio_system_is_initialised) { return boolV(true); }
static bool gMouseLocked = false;
GMFN(window_mouse_set_locked) { gMouseLocked = B(0); return Value(); }
GMFN(window_mouse_get_locked) { return boolV(gMouseLocked); }
GMFN(mouse_wheel_up) { return boolV(rt.input.wheelUp); }
GMFN(mouse_wheel_down) { return boolV(rt.input.wheelDown); }
GMFN(draw_get_halign) { return Value(static_cast<double>(G.halign)); }
GMFN(draw_get_valign) { return Value(static_cast<double>(G.valign)); }
GMFN(math_set_epsilon) { gCompareEpsilon = std::max(0.0, D(0)); return Value(); }
GMFN(math_get_epsilon) { return Value(gCompareEpsilon); }
GMFN(point_distance_3d) {
    double dx = D(3) - D(0), dy = D(4) - D(1), dz = D(5) - D(2);
    return Value(std::sqrt(dx * dx + dy * dy + dz * dz));
}

// ---- ds_grid_add_region / ds_grid_multiply_region(grid, x1, y1, x2, y2, value)
static Value gridValueRegion(Runner& rt, Value* args, int argc, bool multiply) {
    DsGrid* g = rt.ds->grids.get(I(0));
    if (!g) return Value();
    int x1 = std::max(0, std::min(I(1), I(3))), x2 = std::min(g->w - 1, std::max(I(1), I(3)));
    int y1 = std::max(0, std::min(I(2), I(4))), y2 = std::min(g->h - 1, std::max(I(2), I(4)));
    const Value& v = ARG(5);
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++) {
            Value& cell = g->cells[static_cast<size_t>(y) * g->w + x];
            if (multiply) cell = Value(cell.num() * v.num());
            else cell = cell.isString() || v.isString() ? Value(cell.toString() + v.toString()) : Value(cell.num() + v.num());
        }
    return Value();
}
GMFN(ds_grid_add_region) { return gridValueRegion(rt, args, argc, false); }
GMFN(ds_grid_multiply_region) { return gridValueRegion(rt, args, argc, true); }

// ---- weak references: alive while the struct has other (strong) references, or the instance exists
GMFN(weak_ref_create) {
    const Value& target = ARG(0);
    auto ref = std::make_unique<GMWeakRef>();
    if (GMObject* o = target.obj()) {
        if (!o->alive) o->alive = std::make_shared<bool>(true);
        ref->targetAlive = o->alive;
        ref->target = o;
    } else if (target.isNumeric() && rt.byId.count(target.toInt())) {
        ref->instanceId = target.toInt();
    } else {
        rt.log("weak_ref_create: argument is not a struct or instance");
        return Value();
    }
    Value w = rt.vm.newStruct(nullptr);
    w.obj()->weakRef = std::move(ref);
    return w;
}
static Value weakAlive(Runner& rt, const Value& v) {
    GMObject* o = v.obj();
    if (!o || !o->weakRef) return Value();
    const GMWeakRef& r = *o->weakRef;
    if (r.targetAlive) return Value::boolean(*r.targetAlive);
    auto it = rt.byId.find(r.instanceId);
    return Value::boolean(it != rt.byId.end() && !it->second->destroyed);
}
GMFN(weak_ref_alive) { return weakAlive(rt, ARG(0)); }
GMFN(weak_ref_any_alive) {  // (array, [offset], [length])
    GMArray* a = ARG(0).arr();
    if (!a || a->rows.empty()) return boolV(false);
    auto& r = a->rows[0];
    int n = static_cast<int>(r.size());
    int off = argc > 1 ? std::max(0, std::min(I(1), n)) : 0;
    int len = argc > 2 ? I(2) : n - off;
    for (int i = off; i < std::min(n, off + len); i++)
        if (weakAlive(rt, r[i]).truthy()) return boolV(true);
    return boolV(false);
}

// ---- HTTP and dialogs. The runner makes no network requests, so HTTP calls fail the way they
// do without a connection (Async - HTTP, status -1); dialogs finish at once with their default.
static Value httpFail(Runner& rt, const std::string& url) {
    int id = rt.nextAsyncId++;
    rt.queueAsync(62, {{"id", Value(static_cast<double>(id))}, {"status", Value(-1.0)}, {"result", Value("")},
                       {"url", Value(url)}, {"http_status", Value(0.0)}});
    return Value(static_cast<double>(id));
}
GMFN(http_get) { return httpFail(rt, S(0)); }
GMFN(http_get_file) { return httpFail(rt, S(0)); }
GMFN(http_post_string) { return httpFail(rt, S(0)); }
GMFN(http_request) { return httpFail(rt, S(0)); }
static Value dialogDone(Runner& rt, std::vector<std::pair<std::string, Value>> fields) {
    int id = rt.nextAsyncId++;
    fields.insert(fields.begin(), {"id", Value(static_cast<double>(id))});
    rt.queueAsync(63, std::move(fields));
    return Value(static_cast<double>(id));
}
// The blocking dialogs answer at once: the default text or number, and "no" to a question.
GMFN(get_string) { return Value(S(1)); }
GMFN(get_integer) { return Value(D(1)); }
GMFN(show_question) { return boolV(false); }
// File pickers answer as if cancelled: an empty path.
GMFN(get_open_filename) { GM_UNUSED_FN_ARGS; return Value(""); }
GMFN(show_message_async) { return dialogDone(rt, {{"status", Value::boolean(true)}}); }
GMFN(show_question_async) { return dialogDone(rt, {{"status", Value::boolean(true)}}); }
GMFN(get_string_async) { return dialogDone(rt, {{"status", Value::boolean(true)}, {"result", Value(S(1))}}); }
GMFN(get_integer_async) { return dialogDone(rt, {{"status", Value::boolean(true)}, {"value", Value(D(1))}}); }
GMFN(get_login_async) {
    return dialogDone(rt, {{"status", Value::boolean(false)}, {"username", Value(S(0))}, {"password", Value("")}});
}

// ---- fonts
static const Font* fontAt(Runner& rt, int f) {
    return f >= 0 && static_cast<size_t>(f) < rt.dw.fonts.size() ? &rt.dw.fonts[f] : nullptr;
}
GMFN(font_get_texture) {
    const Font* f = fontAt(rt, I(0));
    return texValue(f && f->tpag >= 0 ? rt.dw.tpag[f->tpag].texture : -1);
}
GMFN(font_get_uvs) {
    const Font* f = fontAt(rt, I(0));
    if (!f || f->tpag < 0) return Value();
    const TexturePageItem& t = rt.dw.tpag[f->tpag];
    uint32_t tex;
    int w = 0, h = 0;
    if (!G.textureInfo(t.texture, tex, w, h) || w <= 0 || h <= 0) return Value();
    return numArray({double(t.srcX) / w, double(t.srcY) / h, double(t.srcX + t.srcW) / w, double(t.srcY + t.srcH) / h});
}
// font_get_info for a font made with font_add_sprite* or font_add: sprite fonts report each
// glyph's sprite frame as its "char" (GameMaker does; text libraries such as Scribble rely on it).
static Value runtimeFontInfo(Runner& rt, int font) {
    Graphics::RuntimeFontInfo info;
    if (!G.runtimeFontInfo(font, info)) return Value();
    Value st = rt.vm.newStruct(nullptr);
    structSet(rt, st, "ascenderOffset", Value(0.0));
    structSet(rt, st, "ascender", Value(0.0));
    structSet(rt, st, "sdfSpread", Value(0.0));
    structSet(rt, st, "sdfEnabled", Value::boolean(false));
    structSet(rt, st, "freetype", Value::boolean(info.trueType));
    structSet(rt, st, "size", Value(info.points));
    structSet(rt, st, "spriteIndex", Value(static_cast<double>(info.sprite)));
    structSet(rt, st, "texture", texValue(-1));
    structSet(rt, st, "name", Value(""));
    structSet(rt, st, "bold", Value::boolean(false));
    structSet(rt, st, "italic", Value::boolean(false));
    structSet(rt, st, "effectsEnabled", Value::boolean(false));
    Value glyphs = rt.vm.newStruct(nullptr);
    for (const auto& g : info.glyphs) {
        Value ge = rt.vm.newStruct(nullptr);
        structSet(rt, ge, "char", Value(static_cast<double>(info.sprite >= 0 ? g.frame : static_cast<int>(g.ch))));
        structSet(rt, ge, "x", Value(static_cast<double>(g.x)));
        structSet(rt, ge, "y", Value(static_cast<double>(g.y)));
        structSet(rt, ge, "w", Value(static_cast<double>(g.w)));
        structSet(rt, ge, "h", Value(static_cast<double>(g.h)));
        structSet(rt, ge, "shift", Value(g.advance));
        structSet(rt, ge, "offset", Value(g.offset));
        rt.vm.setInstanceVar(glyphs.obj()->inst, rt.vm.varIdForName(utf8Encode(std::u32string(1, g.ch))), ge);
    }
    structSet(rt, st, "glyphs", glyphs);
    return st;
}

GMFN(font_get_info) {
    const Font* f = fontAt(rt, I(0));
    if (!f) return runtimeFontInfo(rt, I(0));
    Value st = rt.vm.newStruct(nullptr);
    structSet(rt, st, "ascenderOffset", Value(double(f->ascenderOffset)));
    structSet(rt, st, "ascender", Value(double(f->ascender)));
    structSet(rt, st, "sdfSpread", Value(double(f->sdfSpread)));
    structSet(rt, st, "sdfEnabled", Value::boolean(f->sdfSpread > 0));
    structSet(rt, st, "freetype", Value::boolean(false));
    structSet(rt, st, "size", Value(double(f->emSize)));
    structSet(rt, st, "spriteIndex", Value(-1.0));
    structSet(rt, st, "texture", texValue(f->tpag >= 0 ? rt.dw.tpag[f->tpag].texture : -1));
    structSet(rt, st, "name", Value(std::string(f->name)));
    structSet(rt, st, "bold", Value::boolean(f->bold));
    structSet(rt, st, "italic", Value::boolean(f->italic));
    structSet(rt, st, "effectsEnabled", Value::boolean(false));
    Value glyphs = rt.vm.newStruct(nullptr);
    // Glyph positions are on the whole texture page (data.win stores them within the font's entry).
    double px = f->tpag >= 0 ? rt.dw.tpag[f->tpag].srcX : 0, py = f->tpag >= 0 ? rt.dw.tpag[f->tpag].srcY : 0;
    for (const Glyph& g : f->glyphs) {
        Value ge = rt.vm.newStruct(nullptr);
        structSet(rt, ge, "char", Value(double(g.ch)));
        structSet(rt, ge, "x", Value(double(g.x) + px));
        structSet(rt, ge, "y", Value(double(g.y) + py));
        structSet(rt, ge, "w", Value(double(g.w)));
        structSet(rt, ge, "h", Value(double(g.h)));
        structSet(rt, ge, "shift", Value(double(g.shift)));
        structSet(rt, ge, "offset", Value(double(g.offset)));
        rt.vm.setInstanceVar(glyphs.obj()->inst, rt.vm.varIdForName(utf8Encode(std::u32string(1, g.ch))), ge);
    }
    structSet(rt, st, "glyphs", glyphs);
    return st;
}
GMFN(shader_current) { return Value(static_cast<double>(G.currentShader())); }
GMFN(window_handle) { return Value(0.0); }  // no native window handle is exposed
GMFN(window_mouse_get_delta_x) { return Value(rt.input.mouseDeltaX); }
GMFN(window_mouse_get_delta_y) { return Value(rt.input.mouseDeltaY); }

// ---- asset tags (TAGS chunk), keyed by (asset type << 24) | index; types as asset_get_type (2023+)
static std::map<int32_t, std::vector<std::string>>& tagTable(Runner& rt) {
    static std::map<int32_t, std::vector<std::string>> table;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        for (auto& [key, tags] : rt.dw.assetTags)
            for (auto t : tags) table[key].push_back(std::string(t));
    }
    return table;
}
template <typename L> static int findIn(const L& list, const std::string& n) {
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].name == n) return static_cast<int>(i);
    return -1;
}
// The tag key for an asset given by name, or by index plus asset type.
static int32_t tagKey(Runner& rt, const Value& asset, int type) {
    if (asset.isString()) {
        std::string n = asset.toString();
        int i;
        if ((i = findIn(rt.dw.objects, n)) >= 0) return (0 << 24) | i;
        if ((i = findIn(rt.dw.sprites, n)) >= 0) return (1 << 24) | i;
        if ((i = findIn(rt.dw.sounds, n)) >= 0) return (2 << 24) | i;
        if ((i = findIn(rt.dw.rooms, n)) >= 0) return (3 << 24) | i;
        if ((i = findIn(rt.dw.paths, n)) >= 0) return (4 << 24) | i;
        if ((i = findIn(rt.dw.scripts, n)) >= 0) return (5 << 24) | i;
        if ((i = findIn(rt.dw.fonts, n)) >= 0) return (6 << 24) | i;
        if ((i = findIn(rt.dw.timelines, n)) >= 0) return (7 << 24) | i;
        if ((i = findIn(rt.dw.shaders, n)) >= 0) return (8 << 24) | i;
        if ((i = findIn(rt.dw.backgrounds, n)) >= 0) return (13 << 24) | i;
        return -1;
    }
    type = modernAssetType(rt, type);
    if (type < 0) return -1;
    return (type << 24) | (asset.toInt() & 0xFFFFFF);
}
static std::string assetNameForKey(Runner& rt, int32_t key) {
    int type = key >> 24, i = key & 0xFFFFFF;
    auto get = [&](const auto& list) -> std::string {
        return static_cast<size_t>(i) < list.size() ? std::string(list[i].name) : std::string();
    };
    switch (type) {
        case 0: return get(rt.dw.objects);
        case 1: return get(rt.dw.sprites);
        case 2: return get(rt.dw.sounds);
        case 3: return get(rt.dw.rooms);
        case 4: return get(rt.dw.paths);
        case 5: return get(rt.dw.scripts);
        case 6: return get(rt.dw.fonts);
        case 7: return get(rt.dw.timelines);
        case 8: return get(rt.dw.shaders);
        case 13: return get(rt.dw.backgrounds);
        default: return {};
    }
}
static std::vector<std::string> tagList(const Value& v) {  // a tag name or an array of them
    std::vector<std::string> out;
    if (GMArray* a = v.arr()) {
        if (!a->rows.empty())
            for (auto& t : a->rows[0]) out.push_back(t.toString());
    } else {
        out.push_back(v.toString());
    }
    return out;
}
static Value stringArray(const std::vector<std::string>& v) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    for (auto& s : v) a->rows[0].push_back(Value(s));
    return arr;
}
GMFN(asset_get_tags) {  // (name or id, [asset type])
    auto& t = tagTable(rt);
    auto it = t.find(tagKey(rt, ARG(0), argc > 1 ? I(1) : -1));
    return stringArray(it == t.end() ? std::vector<std::string>{} : it->second);
}
static bool hasTags(Runner& rt, Value* args, int argc, bool all) {  // (asset, tags, [type])
    auto& t = tagTable(rt);
    auto it = t.find(tagKey(rt, ARG(0), argc > 2 ? I(2) : -1));
    if (it == t.end()) return false;
    auto want = tagList(ARG(1));
    for (auto& w : want) {
        bool has = std::find(it->second.begin(), it->second.end(), w) != it->second.end();
        if (all && !has) return false;
        if (!all && has) return true;
    }
    return all;
}
GMFN(asset_has_tags) { return boolV(hasTags(rt, args, argc, true)); }
GMFN(asset_has_any_tag) { return boolV(hasTags(rt, args, argc, false)); }
GMFN(asset_add_tags) {  // (asset, tags, [type])
    int32_t key = tagKey(rt, ARG(0), argc > 2 ? I(2) : -1);
    if (key < 0) return boolV(false);
    auto& list = tagTable(rt)[key];
    for (auto& w : tagList(ARG(1)))
        if (std::find(list.begin(), list.end(), w) == list.end()) list.push_back(w);
    return boolV(true);
}
GMFN(asset_remove_tags) {
    auto& t = tagTable(rt);
    auto it = t.find(tagKey(rt, ARG(0), argc > 2 ? I(2) : -1));
    if (it == t.end()) return boolV(false);
    for (auto& w : tagList(ARG(1))) it->second.erase(std::remove(it->second.begin(), it->second.end(), w), it->second.end());
    return boolV(true);
}
GMFN(asset_clear_tags) {
    auto& t = tagTable(rt);
    auto it = t.find(tagKey(rt, ARG(0), argc > 1 ? I(1) : -1));
    if (it != t.end()) it->second.clear();
    return boolV(it != t.end());
}
// tag_get_assets(tags) -> asset names; tag_get_asset_ids(tags, type) -> indices of that type.
static std::vector<int32_t> keysWithTags(Runner& rt, const Value& tags) {
    std::vector<int32_t> out;
    auto want = tagList(tags);
    for (auto& [key, list] : tagTable(rt))
        for (auto& w : want)
            if (std::find(list.begin(), list.end(), w) != list.end()) {
                out.push_back(key);
                break;
            }
    return out;
}
GMFN(tag_get_assets) {
    std::vector<std::string> names;
    for (int32_t key : keysWithTags(rt, ARG(0))) names.push_back(assetNameForKey(rt, key));
    return stringArray(names);
}
GMFN(tag_get_asset_ids) {
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    for (int32_t key : keysWithTags(rt, ARG(0)))
        if ((key >> 24) == modernAssetType(rt, I(1))) a->rows[0].push_back(Value(static_cast<double>(key & 0xFFFFFF)));
    return arr;
}

// ---- os_* queries: a desktop (or Switch) machine with no network, permissions or debugger.
GMFN(os_get_info) {  // a ds_map of platform details, as on Windows (no D3D handles to share)
    int id = rt.ds->maps.create();
    DsMap* m = rt.ds->maps.get(id);
    auto put = [&](const char* k, Value v) {
        ValueKey key = ValueKey::of(Value(k));
        m->values.emplace(key, std::move(v));
        m->order.push_back(key);
    };
    put("is_debugger_attached", Value(0.0));
    put("video_adapter_description", Value("gmrunner"));
    put("video_adapter_vendorid", Value(0.0));
    put("video_adapter_deviceid", Value(0.0));
    put("video_adapter_subsysid", Value(0.0));
    put("video_adapter_revision", Value(0.0));
    put("video_adapter_dedicatedvideomemory", Value(0.0));
    put("video_adapter_dedicatedsystemmemory", Value(0.0));
    put("video_adapter_sharedsystemmemory", Value(0.0));
    return Value(static_cast<double>(id));
}
GMFN(os_get_config) { return Value("Default"); }
GMFN(os_get_region) { return Value("GB"); }
GMFN(os_is_paused) { return boolV(false); }
GMFN(os_is_network_connected) { return boolV(false); }
GMFN(os_check_permission) { return Value(0.0); }
// ---- garbage collector: values are reference counted, so these only report and remember.
static struct { bool enabled = true; int64_t targetMicros = 100; } gGc;
GMFN(gc_enable) { gGc.enabled = B(0); return Value(); }
GMFN(gc_is_enabled) { return boolV(gGc.enabled); }
GMFN(gc_collect) { return Value(); }
GMFN(gc_target_frame_time) { gGc.targetMicros = static_cast<int64_t>(D(0)); return Value(); }
GMFN(gc_get_target_frame_time) { return Value(static_cast<double>(gGc.targetMicros)); }
GMFN(gc_get_stats) {
    Value v = rt.vm.newStruct(nullptr);
    for (const char* k : {"objects_touched", "objects_collected", "traversal_time", "collection_time",
                          "gc_frame", "generation_collected", "num_generations", "num_objects_in_generation"})
        structSet(rt, v, k, Value(0.0));
    return v;
}
// ---- audio recording: there is no microphone to record from.
GMFN(audio_get_recorder_count) { return Value(0.0); }
GMFN(audio_get_recorder_info) { return Value(-1.0); }
GMFN(audio_start_recording) { return Value(-1.0); }
GMFN(audio_stop_recording) { return Value(); }
// ---- GIF capture: frames are accepted but no GIF encoder exists, so saving reports failure.
static int gGifNext = 0;
GMFN(gif_open) { return Value(static_cast<double>(gGifNext++)); }
GMFN(gif_add_surface) { return Value(0.0); }
GMFN(gif_save) { return Value(-1.0); }
GMFN(gif_save_buffer) { return Value(-1.0); }

// event_perform_async(type, map): runs that async event now with `map` as async_load, then
// destroys the map (YoYo's runner).
GMFN(event_perform_async) {
    int32_t map = I(1);
    int32_t saved = rt.asyncLoad;
    rt.asyncLoad = map;
    uint32_t sub = static_cast<uint32_t>(I(0));
    for (Instance* i : std::vector<Instance*>(rt.instances))
        if (Runner::live(i) && rt.findHandler(i->object, EV_OTHER, sub)) rt.performEvent(i, EV_OTHER, sub);
    rt.asyncLoad = saved;
    rt.ds->maps.destroy(map);
    return Value();
}
// ptr(value): a pointer value (from a number, or a hex string).
GMFN(ptr) {
    const Value& v = ARG(0);
    if (v.type() == VT::Ptr) return v;
    int64_t bits = v.isString() ? static_cast<int64_t>(std::strtoull(v.str().c_str(), nullptr, 16)) : v.toInt64();
    return Value::ptr(reinterpret_cast<void*>(static_cast<intptr_t>(bits)));
}
// ref_create(context, name[, index]): references feed the debug overlay's controls (dbg_slider
// & co.), which do nothing here; keep the parts in a struct.
GMFN(ref_create) {
    GM_UNUSED_FN_ARGS;
    Value s = rt.vm.newStruct(nullptr);
    rt.vm.setInstanceVar(s.obj()->inst, rt.vm.varIdForName("context"), ARG(0));
    rt.vm.setInstanceVar(s.obj()->inst, rt.vm.varIdForName("name"), ARG(1));
    if (argc > 2) rt.vm.setInstanceVar(s.obj()->inst, rt.vm.varIdForName("index"), ARG(2));
    return s;
}

void registerMiscBuiltins(Runner& rt) {
    GMREG(event_perform_async); GMREG(ptr); GMREG(ref_create);
    GMREG(sprite_nineslice_create); GMREG(sprite_get_nineslice); GMREG(sprite_set_nineslice);
    GMREG(tileset_get_texture); GMREG(tileset_get_uvs); GMREG(tileset_get_info);
    GMREG(draw_getpixel); GMREG(draw_getpixel_ext); GMREG(room_get_info);
    GMREG(gpu_set_blendequation); GMREG(gpu_set_blendequation_sepalpha); GMREG(gpu_get_blendequation);
    GMREG(gpu_get_blendequation_sepalpha);
    GMREG(get_string); GMREG(get_integer); GMREG(show_question);
    for (const char* n : {"get_open_filename", "get_open_filename_ext", "get_save_filename", "get_save_filename_ext"})
        rt.vm.registerBuiltin(n, fn_get_open_filename);
    GMREG(os_get_info); GMREG(os_get_config);
    GMREG(os_get_region); GMREG(os_is_paused); GMREG(os_is_network_connected); GMREG(os_check_permission);
    GMREG(gc_enable); GMREG(gc_is_enabled); GMREG(gc_collect); GMREG(gc_target_frame_time);
    GMREG(gc_get_target_frame_time); GMREG(gc_get_stats); GMREG(audio_get_recorder_count);
    GMREG(audio_get_recorder_info); GMREG(audio_start_recording); GMREG(audio_stop_recording); GMREG(gif_open);
    GMREG(gif_add_surface); GMREG(gif_save); GMREG(gif_save_buffer);
    GMALIAS(gpu_get_colourwriteenable, gpu_get_colorwriteenable);
    GMREG(point_in_rectangle); GMREG(point_in_circle); GMREG(point_in_triangle); GMREG(rectangle_in_rectangle);
    GMREG(rectangle_in_circle); GMREG(rectangle_in_triangle); GMREG(string_hash_to_newline); GMREG(filename_name);
    GMREG(filename_path); GMREG(filename_dir); GMREG(filename_drive); GMREG(filename_ext); GMREG(filename_change_ext);
    GMREG(file_find_first); GMREG(file_find_next); GMREG(file_find_close); GMREG(alarm_get); GMREG(alarm_set);
    GMREG(device_mouse_x); GMREG(device_mouse_y); GMREG(device_mouse_raw_x); GMREG(device_mouse_raw_y);
    GMREG(device_mouse_x_to_gui); GMREG(device_mouse_y_to_gui); GMREG(device_mouse_check_button);
    GMREG(device_mouse_check_button_pressed); GMREG(device_mouse_check_button_released);
    GMREG(device_mouse_dbclick_enable); GMREG(clipboard_set_text); GMREG(clipboard_get_text);
    GMREG(clipboard_has_text); GMREG(url_open); GMALIAS(url_open_ext, url_open); GMALIAS(url_open_full, url_open);
    GMREG(audio_system); GMREG(draw_roundrect);
    GMREG(draw_roundrect_ext); GMREG(draw_roundrect_color); GMALIAS(draw_roundrect_colour, draw_roundrect_color);
    GMREG(draw_roundrect_color_ext); GMALIAS(draw_roundrect_colour_ext, draw_roundrect_color_ext);
    GMREG(gpu_push_state); GMREG(gpu_pop_state); GMREG(gpu_get_blendmode); GMREG(gpu_get_blendmode_src);
    GMREG(gpu_get_blendmode_dest); GMREG(gpu_get_blendenable); GMREG(gpu_get_alphatestenable);
    GMREG(gpu_get_alphatestref); GMREG(gpu_get_texfilter); GMREG(gpu_get_colorwriteenable); GMREG(gpu_get_fog);
    GMREG(ds_grid_set_grid_region); GMREG(ds_grid_add_grid_region); GMREG(ds_grid_multiply_grid_region);
    GMREG(gpu_set_blendmode_ext_sepalpha); GMREG(gpu_get_blendmode_ext); GMREG(gpu_get_blendmode_ext_sepalpha);
    GMREG(gpu_get_blendmode_srcalpha); GMREG(gpu_get_blendmode_destalpha); GMREG(gpu_set_tex_filter);
    GMALIAS(gpu_get_tex_filter, gpu_get_texfilter); GMREG(gpu_set_tex_repeat); GMREG(gpu_get_tex_repeat);
    GMALIAS(gpu_set_texrepeat, gpu_set_tex_repeat); GMALIAS(gpu_get_texrepeat, gpu_get_tex_repeat);
    GMALIAS(texture_set_repeat, gpu_set_tex_repeat); GMALIAS(texture_get_repeat, gpu_get_tex_repeat);
    GMREG(gpu_set_tex_repeat_ext); GMREG(gpu_get_tex_repeat_ext); GMALIAS(gpu_set_texrepeat_ext, gpu_set_tex_repeat_ext);
    GMALIAS(gpu_get_texrepeat_ext, gpu_get_tex_repeat_ext); GMALIAS(texture_set_repeat_ext, gpu_set_tex_repeat_ext);
    GMREG(gpu_set_tex_filter_ext); GMREG(gpu_get_tex_filter_ext); GMALIAS(gpu_set_texfilter_ext, gpu_set_tex_filter_ext);
    GMALIAS(gpu_get_texfilter_ext, gpu_get_tex_filter_ext); GMALIAS(texture_set_interpolation_ext, gpu_set_tex_filter_ext);
    GMREG(texture_set_blending); GMREG(gpu_set_scissor); GMREG(gpu_get_scissor);
    GMREG(gpu_set_stencil_enable); GMREG(gpu_set_stencil_func); GMREG(gpu_set_stencil_ref);
    GMREG(gpu_set_stencil_read_mask); GMREG(gpu_set_stencil_write_mask); GMREG(gpu_set_stencil_fail);
    GMREG(gpu_set_stencil_depth_fail); GMREG(gpu_set_stencil_pass); GMREG(gpu_get_stencil_enable);
    GMREG(gpu_get_stencil_func); GMREG(gpu_get_stencil_ref); GMREG(gpu_get_stencil_read_mask);
    GMREG(gpu_get_stencil_write_mask); GMREG(gpu_get_stencil_fail); GMREG(gpu_get_stencil_depth_fail);
    GMREG(gpu_get_stencil_pass); GMREG(draw_clear_ext); GMREG(shader_set_uniform_matrix); GMREG(shader_set_uniform_f_buffer);
    GMALIAS(shader_set_uniform_matrix_array, shader_set_uniform_f_array);
    GMREG(shader_set_uniform_f_array); GMREG(shader_set_uniform_i_array); GMREG(sprite_get_texture);
    GMREG(sprite_get_uvs); GMREG(surface_get_texture); GMREG(texture_get_width);
    GMALIAS(texture_get_height, texture_get_width); GMREG(texture_get_texel_width); GMREG(texture_get_texel_height);
    GMREG(draw_sprite_pos); GMREG(sprite_get_info); GMREG(asset_get_ids); GMREG(application_get_position);
    GMREG(surface_depth_disable); GMREG(surface_get_depth_disable); GMREG(surface_format_is_supported);
    GMREG(surface_get_format); GMREG(surface_create_ext); GMREG(extension_get_option_value);
    GMREG(extension_get_options); GMREG(is_handle);
    GMREG(audio_system_is_initialised); GMREG(window_mouse_set_locked); GMREG(window_mouse_get_locked);
    GMREG(mouse_wheel_up); GMREG(mouse_wheel_down); GMREG(draw_get_halign); GMREG(draw_get_valign);
    GMREG(math_set_epsilon); GMREG(math_get_epsilon); GMREG(point_distance_3d); GMREG(ds_grid_add_region);
    GMREG(ds_grid_multiply_region); GMREG(weak_ref_create); GMREG(weak_ref_alive); GMREG(weak_ref_any_alive);
    GMREG(font_get_texture); GMREG(font_get_uvs); GMREG(font_get_info); GMREG(shader_current); GMREG(window_handle);
    GMREG(window_mouse_get_delta_x); GMREG(window_mouse_get_delta_y);
    GMREG(http_get); GMREG(http_get_file); GMREG(http_post_string); GMREG(http_request);
    GMREG(show_message_async); GMREG(show_question_async); GMREG(get_string_async); GMREG(get_integer_async);
    GMREG(get_login_async);
    GMREG(texturegroup_get_textures); GMREG(texturegroup_get_sprites); GMREG(texturegroup_get_fonts);
    GMREG(texturegroup_get_tilesets); GMREG(texturegroup_get_status); GMREG(texturegroup_load);
    GMREG(texturegroup_unload); GMREG(texturegroup_exists); GMREG(texture_is_ready); GMREG(texture_get_uvs);
    GMREG(asset_get_tags); GMREG(asset_has_tags); GMREG(asset_has_any_tag); GMREG(asset_add_tags);
    GMREG(asset_remove_tags); GMREG(asset_clear_tags); GMREG(tag_get_assets); GMREG(tag_get_asset_ids);
}

}  // namespace gm
