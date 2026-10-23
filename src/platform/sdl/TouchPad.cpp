// On-screen controller for touchscreens: an 8-way d-pad bottom left, the four face buttons in
// GameMaker's positions bottom right (gp_face1 at the bottom), L and R in the top corners, and
// Select / Start at the bottom centre (above the rest in portrait). Fingers can slide between buttons.
#include <algorithm>
#include <cmath>

#include "platform/sdl/Host.h"
#include "platform/sdl/Ui.h"
#include "runtime/Runner.h"

namespace gm::host {

namespace {

struct Control {
    int button;  // GP_*, or -1 for the d-pad
    double x, y, r;
    const char* label;
};

// inset: left, top, right, bottom edges the controls stay clear of (notch, camera, home indicator).
// Only the buttons that do something in the game (`shown`, by GP_*) are laid out.
std::vector<Control> layout(int w, int h, const double inset[4], const bool* shown) {
    double s = std::min(w, h), m = 0.05 * s;
    double R = 0.15 * s, rb = 0.068 * s, d = 0.115 * s;
    double x0 = inset[0], y0 = inset[1], x1 = w - inset[2], y1 = h - inset[3];
    double fx = x1 - m - d - rb, fy = y1 - m - d - rb;
    // Select / Start at the bottom centre, or in portrait (no room between the thumbs) above them.
    double sy = x1 - x0 < y1 - y0 ? y1 - m - 0.43 * s : y1 - m - 0.035 * s;
    std::vector<Control> all = {
        {-1, x0 + m + R, y1 - m - R, R, ""},
        {GP_FACE1, fx, fy + d, rb, "A"},
        {GP_FACE2, fx + d, fy, rb, "B"},
        {GP_FACE3, fx - d, fy, rb, "X"},
        {GP_FACE4, fx, fy - d, rb, "Y"},
        {GP_SHOULDERL, x0 + m + 0.06 * s, y0 + m + 0.06 * s, 0.06 * s, "L"},
        {GP_SHOULDERR, x1 - m - 0.06 * s, y0 + m + 0.06 * s, 0.06 * s, "R"},
        {GP_SELECT, (x0 + x1) / 2 - 0.09 * s, sy, 0.045 * s, "SEL"},
        {GP_START, (x0 + x1) / 2 + 0.09 * s, sy, 0.045 * s, "START"},
    };
    if (!shown) return all;
    std::vector<Control> cs;
    for (auto& c : all)
        if (c.button >= 0 ? shown[c.button] : shown[GP_PADU] || shown[GP_PADD] || shown[GP_PADL] || shown[GP_PADR])
            cs.push_back(c);
    return cs;
}

// Which control a point is on (generous margins: thumbs are imprecise), or -1.
int hit(const std::vector<Control>& cs, double x, double y) {
    int best = -1;
    double bestD = 1e9;
    for (size_t i = 0; i < cs.size(); i++) {
        double dist = std::hypot(x - cs[i].x, y - cs[i].y);
        double reach = cs[i].r * (cs[i].button < 0 ? 1.3 : 1.35);
        if (dist < reach && dist / cs[i].r < bestD) best = static_cast<int>(i), bestD = dist / cs[i].r;
    }
    return best;
}

// The d-pad directions a point presses: 8 sectors of 45 degrees, nothing in the middle.
void dpadDirs(const Control& c, double x, double y, bool out[4]) {
    double dx = x - c.x, dy = y - c.y, dist = std::hypot(dx, dy);
    out[0] = out[1] = out[2] = out[3] = false;
    if (dist < 0.2 * c.r) return;
    out[0] = dx < -0.38 * dist;  // left
    out[1] = dx > 0.38 * dist;   // right
    out[2] = dy < -0.38 * dist;  // up
    out[3] = dy > 0.38 * dist;   // down
}

const int kDpadButtons[4] = {GP_PADL, GP_PADR, GP_PADU, GP_PADD};

}  // namespace

TouchPad::~TouchPad() = default;

bool TouchPad::handleFinger(const SDL_TouchFingerEvent& e, Runner& rt, int w, int h) {
    if (!visible) return false;
    auto cs = layout(w, h, inset, buttons ? buttons->map.onScreen : nullptr);
    double x = e.x * w, y = e.y * h;
    Finger* f = nullptr;
    for (auto& fi : fingers)
        if (fi.used && fi.id == e.fingerId) f = &fi;
    if (e.type == SDL_FINGERDOWN) {
        int c = hit(cs, x, y);
        if (c < 0) return false;
        for (auto& fi : fingers)
            if (!fi.used) {
                f = &fi;
                break;
            }
        if (!f) return true;
        *f = {e.fingerId, c, true, x, y};
    }
    if (!f) return false;  // a touch that started outside the pad stays the game's
    f->x = x, f->y = y;
    if (e.type == SDL_FINGERUP) f->used = false;
    else if (e.type == SDL_FINGERMOTION && cs[static_cast<size_t>(f->control)].button >= 0) {
        int c = hit(cs, x, y);  // slide from one button onto another
        if (c >= 0 && cs[static_cast<size_t>(c)].button >= 0) f->control = c;
    }
    bool want[GP_BUTTON_COUNT] = {};
    for (size_t i = 0; i < 10; i++) {
        if (!fingers[i].used) continue;
        const Control& c = cs[static_cast<size_t>(fingers[i].control)];
        if (c.button >= 0) {
            want[c.button] = true;
        } else {
            bool dirs[4];
            dpadDirs(c, fingers[i].x, fingers[i].y, dirs);
            for (int k = 0; k < 4; k++)
                if (dirs[k]) want[kDpadButtons[k]] = true;
        }
    }
    for (int b = 0; b < GP_BUTTON_COUNT; b++)
        if (want[b] != down[b]) {
            down[b] = want[b];
            if (buttons) buttons->press(rt, Buttons::kOnScreen, b, want[b]);
        }
    return true;
}

void TouchPad::releaseAll(Runner& rt) {
    for (auto& f : fingers) f.used = false;
    for (int b = 0; b < GP_BUTTON_COUNT; b++)
        if (down[b]) {
            down[b] = false;
            if (buttons) buttons->press(rt, Buttons::kOnScreen, b, false);
        }
}

void TouchPad::draw(Renderer& r, int w, int h) {
    if (!visible) return;
    if (!painter || painterOwner != &r) painter = std::make_unique<Painter>(r), painterOwner = &r;
    Painter& p = *painter;
    const uint32_t white = rgb(255, 255, 255), dark = rgb(0, 0, 0);
    for (auto& c : layout(w, h, inset, buttons ? buttons->map.onScreen : nullptr)) {
        if (c.button < 0) {  // d-pad: a cross on a disc, the pressed arms lit
            p.circle(c.x, c.y, c.r, dark, 0.25);
            double a = c.r * 0.34, l = c.r * 0.86;
            p.rect(c.x - l, c.y - a / 2, 2 * l, a, white, 0.30);
            p.rect(c.x - a / 2, c.y - l, a, 2 * l, white, 0.30);
            const double ax[4] = {-1, 1, 0, 0}, ay[4] = {0, 0, -1, 1};
            for (int k = 0; k < 4; k++)
                if (down[kDpadButtons[k]])
                    p.rect(c.x + ax[k] * l * 0.55 - a / 2, c.y + ay[k] * l * 0.55 - a / 2, a, a, white, 0.6);
            continue;
        }
        bool on = down[c.button];
        p.circle(c.x, c.y, c.r, on ? white : dark, on ? 0.45 : 0.25);
        p.ring(c.x, c.y, c.r, std::max(1.0, c.r * 0.08), white, 0.45);
        size_t n = std::string(c.label).size();
        double px = std::max(1.0, std::floor(std::min(c.r * 0.1, 1.5 * c.r / (6.0 * n))));
        p.textCentered(c.x, c.y, px, c.label, white, on ? 0.9 : 0.6);
    }
}

}  // namespace gm::host
