// The in-game menu: resume, the game's controls (what each button presses, the on-screen
// controls, the menu button) and exit. Drawn by the runner over the paused game, so it is the
// same on every target; changes apply at once and are saved in the game's gmrunner.ini.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "platform/sdl/Host.h"
#include "platform/sdl/Target.h"
#include "runtime/Runner.h"

namespace gm::host {

namespace {

enum Page { MAIN, CONTROLS, PICK };
enum Kind { RESUME, OPEN_CONTROLS, TOUCH, SHORTCUT, EXIT, BACK, BUTTON, RESET, AUTO, NOTHING, PAD, KEY };

struct Row {
    Kind kind;
    int arg;
    std::string label, value;
    bool check = false, checked = false, changed = false;
    Row(Kind k, int a = 0, std::string l = "", std::string v = "") : kind(k), arg(a), label(std::move(l)), value(std::move(v)) {}
};

const char* const kLabels[GP_BUTTON_COUNT] = {"A", "B", "X", "Y", "L", "R", "L2", "R2",
                                              "SELECT", "START", "L3", "R3", "UP", "DOWN", "LEFT", "RIGHT"};
// The controls page lists the buttons in this order: the on-screen ones first.
const int kButtonOrder[GP_BUTTON_COUNT] = {GP_FACE1, GP_FACE2, GP_FACE3, GP_FACE4, GP_SHOULDERL, GP_SHOULDERR,
                                           GP_SELECT, GP_START, GP_PADU, GP_PADD, GP_PADL, GP_PADR,
                                           GP_SHOULDERLB, GP_SHOULDERRB, GP_STICKL, GP_STICKR};
// Keys offered for any game, most used first (offeredKeys mixes in the game's own).
const int kCommonKeys[] = {'Z', 'X', 'C', VK_SPACE, VK_ENTER, VK_ESCAPE, VK_SHIFT, VK_CONTROL, VK_ALT, 'A', 'S', 'D',
                           'Q', 'W', 'E', 'V', 'P', VK_TAB, VK_BACKSPACE, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};

// The keys to offer for a button: the usual ones the game checks, its other keys, then the
// usual ones it doesn't (they may be checked through variables the scan can't follow).
std::vector<int> offeredKeys(const ButtonMap& m) {
    auto inGame = [&](int vk) { return std::find(m.gameKeys.begin(), m.gameKeys.end(), vk) != m.gameKeys.end(); };
    std::vector<int> keys;
    for (int vk : kCommonKeys)
        if (inGame(vk)) keys.push_back(vk);
    for (int vk : m.gameKeys)
        if (std::find(keys.begin(), keys.end(), vk) == keys.end()) keys.push_back(vk);
    for (int vk : kCommonKeys)
        if (!inGame(vk)) keys.push_back(vk);
    return keys;
}

std::string titleCase(const std::string& s) {  // "SELECT" -> "Select", "L2" stays
    std::string t = s;
    for (size_t i = 1; i < t.size(); i++) t[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(t[i])));
    return t;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string showKey(int vk) {
    std::string n = keyName(vk);
    return upper(n.rfind("vk_", 0) == 0 ? n.substr(3) : n);
}
std::string showPad(int gp) { return std::string("PAD ") + kLabels[gp]; }
// What button `b` presses; its own gamepad button only when that is all (it is the usual case).
std::string showTargets(int b, const std::vector<int>& pad, const std::vector<int>& keys) {
    std::string s;
    for (int vk : keys) s += (s.empty() ? "" : " + ") + showKey(vk);
    for (int gp : pad)
        if (gp != b || (keys.empty() && pad.size() == 1)) s += (s.empty() ? "" : " + ") + showPad(gp);
    return s.empty() ? "NOTHING" : s;
}

const char* const kShortcutNames[GameMenu::SHORTCUT_COUNT] = {"button", "fingers", "back", "none"};
const char* const kShortcutLabels[GameMenu::SHORTCUT_COUNT] = {"FLOATING BUTTON", "THREE FINGERS", "BACK BUTTON",
                                                               "START + SELECT"};

// The shortcuts a target offers: Android has a back button.
bool shortcutOffered(int s) {
#ifdef __ANDROID__
    return s >= 0 && s < GameMenu::SHORTCUT_COUNT;
#else
    return s >= 0 && s < GameMenu::SHORTCUT_COUNT && s != GameMenu::BACK_BUTTON;
#endif
}

// Sets `key = value` in an ini file, keeping its other lines; an empty value removes the key.
void setIni(const std::string& path, const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        for (std::string l; std::getline(in, l);) lines.push_back(l);
    }
    bool found = false;
    for (size_t i = 0; i < lines.size(); i++) {
        size_t eq = lines[i].find('=');
        if (eq == std::string::npos) continue;
        std::string k = lines[i].substr(0, eq);
        while (!k.empty() && std::isspace(static_cast<unsigned char>(k.back()))) k.pop_back();
        while (!k.empty() && std::isspace(static_cast<unsigned char>(k.front()))) k.erase(0, 1);
        if (k != key) continue;
        if (value.empty()) lines.erase(lines.begin() + static_cast<long>(i--));
        else lines[i] = key + " = " + value;
        found = true;
    }
    if (!found && !value.empty()) lines.push_back(key + " = " + value);
    std::ofstream out(path, std::ios::trunc);
    for (auto& l : lines) out << l << "\n";
    if (!out) fprintf(stderr, "[menu] cannot write %s\n", path.c_str());
}

struct Geometry {
    double px, rowH, titleH, x, y, w, h;
    int visibleRows;
};

}  // namespace

GameMenu::~GameMenu() = default;

const char* GameMenu::shortcutName(int s) { return s >= 0 && s < SHORTCUT_COUNT ? kShortcutNames[s] : "button"; }
int GameMenu::shortcutFromName(const std::string& name) {
    for (int s = 0; s < SHORTCUT_COUNT; s++)
        if (name == kShortcutNames[s]) return shortcutOffered(s) ? s : FLOATING_BUTTON;
    return FLOATING_BUTTON;
}

namespace {

std::vector<Row> buildRows(const GameMenu& menu, const HostInput& in) {
    std::vector<Row> rows;
    const ButtonMap& m = in.buttons.map;
    const ButtonMap& d = in.buttons.defaults;
    if (menu.page == MAIN) {
        rows.push_back({RESUME, 0, "Resume"});
        rows.push_back({OPEN_CONTROLS, 0, "Controls"});
        if (target().touchControls) {
            static const char* const modes[3] = {"AUTO", "ON", "OFF"};
            rows.push_back({TOUCH, 0, "On-screen controls", modes[in.touchMode]});
            rows.push_back({SHORTCUT, 0, "Menu shortcut", kShortcutLabels[menu.shortcut]});
        }
        rows.push_back({EXIT, 0, "Exit game"});
    } else if (menu.page == CONTROLS) {
        rows.push_back({BACK, 0, "< Back"});
        for (int b : kButtonOrder) {
            Row r{BUTTON, b, kLabels[b], showTargets(b, m.pad[b], m.keys[b])};
            r.changed = m.set[b];
            rows.push_back(r);
        }
        rows.push_back({RESET, 0, "Reset all to auto"});
    } else {
        int b = menu.editing;
        rows.push_back({BACK, 0, "< Back"});
        Row autoRow{AUTO, 0, "Auto", showTargets(b, d.pad[b], d.keys[b])};
        autoRow.check = true, autoRow.checked = !m.set[b];
        rows.push_back(autoRow);
        Row none{NOTHING, 0, "Nothing"};
        none.check = true, none.checked = m.set[b] && m.pad[b].empty() && m.keys[b].empty();
        rows.push_back(none);
        auto addPad = [&]() {
            for (int gp = 0; gp < GP_BUTTON_COUNT; gp++) {
                Row r{PAD, gp, "Gamepad " + std::string(kLabels[gp])};
                r.check = true, r.checked = std::find(m.pad[b].begin(), m.pad[b].end(), gp) != m.pad[b].end();
                rows.push_back(r);
            }
        };
        bool padFirst = m.readsGamepad && !m.readsKeyboard;
        if (padFirst) addPad();
        std::vector<int> keys = offeredKeys(m);
        for (int vk : m.keys[b])
            if (std::find(keys.begin(), keys.end(), vk) == keys.end()) keys.push_back(vk);
        for (int vk : keys) {
            bool inGame = std::find(m.gameKeys.begin(), m.gameKeys.end(), vk) != m.gameKeys.end();
            Row r{KEY, vk, "Key " + showKey(vk), inGame ? "IN GAME" : ""};
            r.check = true, r.checked = std::find(m.keys[b].begin(), m.keys[b].end(), vk) != m.keys[b].end();
            rows.push_back(r);
        }
        if (!padFirst) addPad();
    }
    return rows;
}

Geometry geometry(int w, int h, const double inset[4], size_t rows) {
    Geometry g;
    double s = std::min(w, h);
    g.px = std::max(2.0, std::floor(s / 190));
    g.rowH = 13 * g.px;
    g.titleH = 17 * g.px;
    double x0 = inset[0], y0 = inset[1], x1 = w - inset[2], y1 = h - inset[3], m = 0.04 * s;
    g.w = std::min(x1 - x0 - 2 * m, 62 * 6 * g.px);
    double maxH = y1 - y0 - 2 * m;
    g.visibleRows = std::max(1, static_cast<int>((maxH - g.titleH) / g.rowH));
    int shown = std::min(g.visibleRows, static_cast<int>(rows));
    g.h = g.titleH + shown * g.rowH;
    g.x = (x0 + x1 - g.w) / 2;
    g.y = (y0 + y1 - g.h) / 2;
    return g;
}

// The floating menu button: top centre, clear of the safe-area insets.
void buttonPlace(int w, int h, const double inset[4], double& cx, double& cy, double& r) {
    double s = std::min(w, h);
    r = 0.045 * s;
    cx = (inset[0] + w - inset[2]) / 2;
    cy = inset[1] + r + 0.025 * s;
}

}  // namespace

namespace {

std::string jsonString(const std::string& v) {
    std::string s = "\"";
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') s += '\\', s += static_cast<char>(c);
        else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", c);
            s += buf;
        } else s += static_cast<char>(c);
    }
    return s + "\"";
}

std::string jsonList(const std::vector<std::string>& items) {
    std::string s = "[";
    for (size_t i = 0; i < items.size(); i++) s += (i ? "," : "") + jsonString(items[i]);
    return s + "]";
}

std::vector<std::string> targetIds(const std::vector<int>& pad, const std::vector<int>& keys) {
    std::vector<std::string> ids;
    for (int gp : pad) ids.push_back(padTargetName(gp));
    for (int vk : keys) ids.push_back(keyName(vk));
    return ids;
}

std::string niceKey(int vk) {  // "Space", "Z", "F1", "Numpad 1"
    std::string n = keyName(vk);
    if (n.rfind("vk_", 0) != 0) return n;
    n = n.substr(3);
    if (n.rfind("numpad", 0) == 0) return "Numpad " + n.substr(6);
    if (n[0] == 'f' && n.size() <= 3 && std::isdigit(static_cast<unsigned char>(n[1]))) return upper(n);
    n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
    return n;
}

// What a native menu (targetShowMenu) needs: every button's targets now and by default, the keys
// and gamepad buttons to offer (ids as gmrunner.ini writes them), and the menu's settings.
std::string menuJson(const GameMenu& menu, const HostInput& in) {
    const ButtonMap& m = in.buttons.map;
    const ButtonMap& d = in.buttons.defaults;
    static const char* const touchModes[3] = {"auto", "on", "off"};
    std::string s = "{\"ini\":" + jsonString(in.iniPath);
    s += ",\"readsKeyboard\":" + std::string(m.readsKeyboard ? "true" : "false");
    s += ",\"readsGamepad\":" + std::string(m.readsGamepad ? "true" : "false");
    s += ",\"touchMode\":" + jsonString(touchModes[in.touchMode]);
    s += ",\"shortcut\":" + jsonString(GameMenu::shortcutName(menu.shortcut));
    s += ",\"buttons\":[";
    for (int i = 0; i < GP_BUTTON_COUNT; i++) {
        int b = kButtonOrder[i];
        s += std::string(i ? "," : "") + "{\"id\":" + jsonString(buttonName(b)) + ",\"label\":" + jsonString(titleCase(kLabels[b])) +
             ",\"pad\":" + jsonString(padTargetName(b)) +
             ",\"set\":" + (m.set[b] ? "true" : "false") + ",\"targets\":" + jsonList(targetIds(m.pad[b], m.keys[b])) +
             ",\"auto\":" + jsonList(targetIds(d.pad[b], d.keys[b])) + "}";
    }
    s += "],\"keys\":[";
    std::vector<int> keys = offeredKeys(m);
    for (size_t i = 0; i < keys.size(); i++) {
        bool inGame = std::find(m.gameKeys.begin(), m.gameKeys.end(), keys[i]) != m.gameKeys.end();
        s += std::string(i ? "," : "") + "{\"id\":" + jsonString(keyName(keys[i])) + ",\"name\":" + jsonString(niceKey(keys[i])) +
             ",\"inGame\":" + (inGame ? "true" : "false") + "}";
    }
    s += "],\"pads\":[";
    for (int gp = 0; gp < GP_BUTTON_COUNT; gp++)
        s += std::string(gp ? "," : "") + "{\"id\":" + jsonString(padTargetName(gp)) + ",\"name\":" +
             jsonString("Gamepad " + titleCase(kLabels[gp])) + ",\"inGame\":false}";
    return s + "]}";
}

}  // namespace

void GameMenu::poll() {
    if (!native) return;
    int r = targetMenuResult();
    if (r < 0) return;
    native = open = false;
    input->reloadSettings();
    if (r == 1) exitRequested = true;
    fprintf(stderr, "[menu] %s\n", exitRequested ? "exit" : "closed");
}

void GameMenu::show(Runner& rt) {
    open = true;
    page = MAIN;
    sel = 0;
    scroll = 0;
    tracking = onButton = false;
    // Whatever the game was being given lets go: buttons, keys, the on-screen pad, the mouse.
    input->buttons.releaseAll(rt);
    input->touchPad.releaseAll(rt);
    for (bool& u : input->fingerUsed) u = false;
    rt.input.mouseButton(0, false);
    native = targetShowMenu(menuJson(*this, *input));
    fprintf(stderr, "[menu] open%s\n", native ? " (native)" : "");
}

namespace {

void save(HostInput& in, int b) {
    const ButtonMap& m = in.buttons.map;
    setIni(in.iniPath, std::string("map_") + buttonName(b), m.set[b] ? targetsText(m.pad[b], m.keys[b]) : "");
}

void activate(GameMenu& menu, HostInput& in, Runner& rt, const Row& row) {
    ButtonMap& m = in.buttons.map;
    const ButtonMap& d = in.buttons.defaults;
    auto changeButton = [&](int b, auto&& change) {
        in.buttons.releaseAll(rt);
        change();
        in.decideTouchControls();
        save(in, b);
    };
    int b = menu.editing;
    switch (row.kind) {
        case RESUME: menu.open = false; break;
        case OPEN_CONTROLS: menu.page = CONTROLS, menu.sel = 1, menu.scroll = 0; break;
        case TOUCH:
            in.touchMode = (in.touchMode + 1) % 3;
            in.decideTouchControls();
            setIni(in.iniPath, "touch_controls", in.touchMode == 1 ? "on" : in.touchMode == 2 ? "off" : "");
            break;
        case SHORTCUT:
            do menu.shortcut = (menu.shortcut + 1) % GameMenu::SHORTCUT_COUNT;
            while (!shortcutOffered(menu.shortcut));
            setIni(in.iniPath, "menu_shortcut", menu.shortcut == GameMenu::FLOATING_BUTTON ? "" : kShortcutNames[menu.shortcut]);
            break;
        case EXIT: menu.exitRequested = true, menu.open = false; break;
        case BACK:
            if (menu.page == PICK) {
                menu.page = CONTROLS;
                menu.sel = 1 + static_cast<int>(std::find(kButtonOrder, kButtonOrder + GP_BUTTON_COUNT, b) - kButtonOrder);
            } else {
                menu.page = MAIN, menu.sel = 1;
            }
            menu.scroll = 0;
            break;
        case BUTTON: menu.editing = row.arg, menu.page = PICK, menu.sel = 1, menu.scroll = 0; break;
        case RESET:
            in.buttons.releaseAll(rt);
            for (int i = 0; i < GP_BUTTON_COUNT; i++)
                if (m.set[i]) m.set[i] = false, m.pad[i] = d.pad[i], m.keys[i] = d.keys[i], save(in, i);
            in.decideTouchControls();
            break;
        case AUTO: changeButton(b, [&] { m.set[b] = false, m.pad[b] = d.pad[b], m.keys[b] = d.keys[b]; }); break;
        case NOTHING: changeButton(b, [&] { m.set[b] = true, m.pad[b].clear(), m.keys[b].clear(); }); break;
        case PAD: case KEY:
            changeButton(b, [&] {
                std::vector<int>& list = row.kind == PAD ? m.pad[b] : m.keys[b];
                m.set[b] = true;
                auto it = std::find(list.begin(), list.end(), row.arg);
                if (it == list.end()) list.push_back(row.arg);
                else list.erase(it);
            });
            break;
    }
    if (!menu.open) fprintf(stderr, "[menu] %s\n", menu.exitRequested ? "exit" : "closed");
}

void keepVisible(GameMenu& menu, int visibleRows, int rows) {
    if (menu.sel < menu.scroll) menu.scroll = menu.sel;
    if (menu.sel >= menu.scroll + visibleRows) menu.scroll = menu.sel - visibleRows + 1;
    menu.scroll = std::clamp(menu.scroll, 0.0, std::max(0.0, static_cast<double>(rows - visibleRows)));
}

}  // namespace

bool GameMenu::handleButton(Runner& rt, int source, int button, bool down) {
    if (native) return true;  // the platform's menu has the controller now
    const auto& held = input->buttons.held[source];
    if (!open) {
        // Start + Select held together open the menu (the second of them doesn't reach the game).
        if (down && (button == GP_START || button == GP_SELECT) && held[GP_START] && held[GP_SELECT]) {
            show(rt);
            return true;
        }
        return false;
    }
    if (!down) return true;
    auto rows = buildRows(*this, *input);
    int n = static_cast<int>(rows.size());
    switch (button) {
        case GP_PADU: sel = (sel + n - 1) % n; break;
        case GP_PADD: sel = (sel + 1) % n; break;
        case GP_FACE1: activate(*this, *input, rt, rows[static_cast<size_t>(sel)]); break;
        case GP_FACE2:
            if (page == MAIN) open = false;
            else activate(*this, *input, rt, Row{BACK});
            break;
        case GP_START: open = false; break;
        default: break;
    }
    if (lastW > 0) {
        Geometry g = geometry(lastW, lastH, input->touchPad.inset, buildRows(*this, *input).size());
        keepVisible(*this, g.visibleRows, static_cast<int>(buildRows(*this, *input).size()));
    }
    return true;
}

void GameMenu::tick(Runner& rt, double now) {
    if (!open || native) {
        repeatDir = 0;
        return;
    }
    bool padUp = false, padDown = false;
    for (auto& held : input->buttons.held) padUp |= held[GP_PADU], padDown |= held[GP_PADD];
    float stick = rt.input.pads[0].axis[GP_AXISLV];
    int dir = padUp ? -1 : padDown ? 1 : stick < -0.5f ? -1 : stick > 0.5f ? 1 : 0;
    auto step = [&] { handleButton(rt, 0, dir < 0 ? GP_PADU : GP_PADD, true); };
    if (dir == 0) {
        repeatDir = 0;
    } else if (dir != repeatDir) {
        if (!padUp && !padDown) step();  // a d-pad press has moved it already
        repeatDir = dir;
        repeatAt = now + 0.4;
    } else if (now >= repeatAt) {
        step();
        repeatAt = now + 0.1;
    }
}

bool GameMenu::handleEvent(const SDL_Event& e, Runner& rt, SdlPlatform& platform) {
    if (native) return e.type != SDL_CONTROLLERDEVICEADDED && e.type != SDL_CONTROLLERDEVICEREMOVED &&
                       e.type != SDL_JOYDEVICEADDED && e.type != SDL_JOYDEVICEREMOVED;
    int w, h;
    platform.drawableSize(w, h);
    lastW = w, lastH = h;
    // One pointer for fingers and the mouse (desktop testing): id, kind, position in pixels.
    bool isTouch = e.type == SDL_FINGERDOWN || e.type == SDL_FINGERUP || e.type == SDL_FINGERMOTION;
    bool isMouse = (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEMOTION) &&
                   (e.type == SDL_MOUSEMOTION ? e.motion.which : e.button.which) != SDL_TOUCH_MOUSEID;
    if (isTouch) {
#if SDL_VERSION_ATLEAST(2, 0, 10)
        if (SDL_GetTouchDeviceType(e.tfinger.touchId) != SDL_TOUCH_DEVICE_DIRECT) return open;
#endif
        auto it = std::find(touches.begin(), touches.end(), e.tfinger.fingerId);
        if (e.type == SDL_FINGERDOWN && it == touches.end()) touches.push_back(e.tfinger.fingerId);
        if (e.type == SDL_FINGERUP && it != touches.end()) touches.erase(it);
        // Three fingers at once (also the way in when the floating button is hidden).
        if (!open && e.type == SDL_FINGERDOWN && touches.size() >= 3 && target().touchControls &&
            shortcut != CONTROLLER_ONLY) {
            show(rt);
            return true;
        }
    }
    if (!isTouch && !isMouse) {
        if (!open) {
            // Android's back button, when it is the shortcut (otherwise it is the game's Escape).
            bool back = shortcut == BACK_BUTTON && (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) &&
                        e.key.keysym.sym == SDLK_AC_BACK;
            if (back && e.type == SDL_KEYDOWN && !e.key.repeat) show(rt);
            return back;
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            int b = -1;
            switch (e.key.keysym.sym) {
                case SDLK_UP: b = GP_PADU; break;
                case SDLK_DOWN: b = GP_PADD; break;
                case SDLK_RETURN: case SDLK_SPACE: case SDLK_z: b = GP_FACE1; break;
                case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_AC_BACK: case SDLK_x: b = GP_FACE2; break;
                default: break;
            }
            if (b >= 0) handleButton(rt, 0, b, true);
        }
        return e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_TEXTINPUT || e.type == SDL_MOUSEWHEEL;
    }
    if (isMouse && e.type == SDL_MOUSEMOTION && !(e.motion.state & SDL_BUTTON_LMASK)) return open;
    if (isMouse && e.type != SDL_MOUSEMOTION && e.button.button != SDL_BUTTON_LEFT) return open;

    SDL_FingerID id;
    double x, y;
    int kind;  // 0 down, 1 move, 2 up
    if (isTouch) {
        id = e.tfinger.fingerId;
        x = e.tfinger.x * w, y = e.tfinger.y * h;
        kind = e.type == SDL_FINGERDOWN ? 0 : e.type == SDL_FINGERMOTION ? 1 : 2;
    } else {
        int ww, wh;
        SDL_GetWindowSize(platform.window, &ww, &wh);
        int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x, my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
        id = -2;
        x = ww ? mx * static_cast<double>(w) / ww : mx, y = wh ? my * static_cast<double>(h) / wh : my;
        kind = e.type == SDL_MOUSEBUTTONDOWN ? 0 : e.type == SDL_MOUSEMOTION ? 1 : 2;
    }

    if (!open) {  // only the floating button
        bool visible = shortcut == FLOATING_BUTTON && (target().touchControls || input->touchPad.visible);
        double cx, cy, r;
        buttonPlace(w, h, input->touchPad.inset, cx, cy, r);
        bool over = std::hypot(x - cx, y - cy) < r * 1.4;
        if (kind == 0 && visible && over) {
            onButton = true, trackId = id;
            return true;
        }
        if (onButton && id == trackId) {
            if (kind == 2) {
                onButton = false;
                if (over) show(rt);
            }
            return true;
        }
        return false;
    }

    auto rows = buildRows(*this, *input);
    Geometry g = geometry(w, h, input->touchPad.inset, rows.size());
    if (kind == 0 && !tracking) {
        tracking = true, dragged = false, trackId = id, trackX = x, trackY = y, scroll0 = scroll;
    } else if (tracking && id == trackId && kind == 1) {
        double dy = y - trackY;
        if (std::fabs(dy) > g.rowH / 3) dragged = true;
        if (dragged) {
            double maxScroll = std::max(0.0, static_cast<double>(rows.size()) - g.visibleRows);
            scroll = std::clamp(scroll0 - dy / g.rowH, 0.0, maxScroll);
        }
    } else if (tracking && id == trackId && kind == 2) {
        tracking = false;
        if (!dragged) {
            if (x < g.x || x > g.x + g.w || y < g.y || y > g.y + g.h) {
                open = false;  // a tap beside the menu resumes
                fprintf(stderr, "[menu] closed\n");
            } else if (y >= g.y + g.titleH) {
                int i = static_cast<int>((y - g.y - g.titleH) / g.rowH + std::floor(scroll));
                if (i >= 0 && i < static_cast<int>(rows.size())) {
                    sel = i;
                    activate(*this, *input, rt, rows[static_cast<size_t>(i)]);
                }
            }
        } else {
            scroll = std::round(scroll);
        }
    }
    return true;
}

void GameMenu::draw(Renderer& r, int w, int h) {
    lastW = w, lastH = h;
    if (native) return;
    bool buttonVisible = !open && shortcut == FLOATING_BUTTON && (target().touchControls || input->touchPad.visible);
    if (!open && !buttonVisible) return;
    if (!painter || painterOwner != &r) painter = std::make_unique<Painter>(r), painterOwner = &r;
    Painter& p = *painter;
    const uint32_t white = rgb(255, 255, 255), black = rgb(0, 0, 0), panel = rgb(28, 30, 36),
                   hi = rgb(70, 110, 200), dim = rgb(150, 155, 165), accent = rgb(255, 200, 80);
    if (!open) {
        double cx, cy, rad;
        buttonPlace(w, h, input->touchPad.inset, cx, cy, rad);
        p.circle(cx, cy, rad, black, onButton ? 0.55 : 0.3);
        p.ring(cx, cy, rad, std::max(1.0, rad * 0.06), white, 0.45);
        double bw = rad * 0.9, bh = std::max(1.0, rad * 0.12);
        for (int i = -1; i <= 1; i++) p.rect(cx - bw / 2, cy + i * rad * 0.3 - bh / 2, bw, bh, white, 0.7);
        return;
    }
    auto rows = buildRows(*this, *input);
    Geometry g = geometry(w, h, input->touchPad.inset, rows.size());
    p.rect(0, 0, w, h, black, 0.55);
    p.rect(g.x, g.y, g.w, g.h, panel, 0.94);
    std::string title = page == MAIN ? "Paused" : page == CONTROLS ? "Controls" : std::string(kLabels[editing]) + " presses";
    p.text(g.x + 4 * g.px, g.y + 4 * g.px, g.px * 1.25, title, white, 1.0);
    p.rect(g.x, g.y + g.titleH - g.px, g.w, g.px * 0.5, white, 0.2);
    int first = static_cast<int>(std::floor(scroll));
    double off = (scroll - first) * g.rowH;
    for (int i = first; i < static_cast<int>(rows.size()) && i - first <= g.visibleRows; i++) {
        double y = g.y + g.titleH + (i - first) * g.rowH - off;
        if (y < g.y + g.titleH - 1 || y + g.rowH > g.y + g.h + 1) continue;
        const Row& row = rows[static_cast<size_t>(i)];
        if (i == sel) p.rect(g.x, y, g.w, g.rowH, hi, 0.55);
        double tx = g.x + 4 * g.px, ty = y + (g.rowH - 7 * g.px) / 2;
        if (row.check) {
            double c = 7 * g.px;
            p.frame(tx, ty, c, c, g.px, white, 0.8);
            if (row.checked) p.rect(tx + 2 * g.px, ty + 2 * g.px, c - 4 * g.px, c - 4 * g.px, accent, 1.0);
            tx += c + 4 * g.px;
        }
        p.text(tx, ty, g.px, row.label, white, 1.0);
        if (!row.value.empty()) {
            double room = g.x + g.w - 4 * g.px - (tx + Painter::textWidth(row.label, g.px) + 6 * g.px);
            std::string v = row.value;
            while (!v.empty() && Painter::textWidth(v, g.px) > room) v.pop_back();
            p.text(g.x + g.w - 4 * g.px - Painter::textWidth(v, g.px), ty, g.px, v, row.changed ? accent : dim, 1.0);
        }
    }
    if (rows.size() > static_cast<size_t>(g.visibleRows)) {  // scroll position
        double track = g.h - g.titleH, frac = g.visibleRows / static_cast<double>(rows.size());
        double maxScroll = static_cast<double>(rows.size()) - g.visibleRows;
        double barH = track * frac, barY = g.y + g.titleH + (track - barH) * (scroll / maxScroll);
        p.rect(g.x + g.w - 1.5 * g.px, barY, g.px, barH, white, 0.35);
    }
}

}  // namespace gm::host
