// Keyboard, mouse, touch and SDL game controllers into the runtime's Input (controller buttons
// through the game's button map, Buttons.cpp); the stick-driven mouse pointer.
#include <algorithm>
#include <cmath>

#include "platform/sdl/Host.h"
#include "platform/sdl/Target.h"
#include "runtime/Graphics.h"
#include "runtime/Runner.h"

namespace gm::host {

namespace {

int vkFromSdl(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return VK_F1 + (k - SDLK_F1);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 97 + (k - SDLK_KP_1);
    switch (k) {
        case SDLK_LEFT: return VK_LEFT;
        case SDLK_RIGHT: return VK_RIGHT;
        case SDLK_UP: return VK_UP;
        case SDLK_DOWN: return VK_DOWN;
        case SDLK_RETURN: case SDLK_KP_ENTER: return VK_ENTER;
        case SDLK_ESCAPE: case SDLK_AC_BACK: return VK_ESCAPE;  // Android's back button
        case SDLK_SPACE: return VK_SPACE;
        case SDLK_BACKSPACE: return VK_BACKSPACE;
        case SDLK_TAB: return VK_TAB;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return VK_SHIFT;
        case SDLK_LCTRL: case SDLK_RCTRL: return VK_CONTROL;
        case SDLK_LALT: case SDLK_RALT: return VK_ALT;
        case SDLK_PAUSE: return VK_PAUSE;
        case SDLK_INSERT: return VK_INSERT;
        case SDLK_DELETE: return VK_DELETE;
        case SDLK_HOME: return VK_HOME;
        case SDLK_END: return VK_END;
        case SDLK_PAGEUP: return VK_PAGEUP;
        case SDLK_PAGEDOWN: return VK_PAGEDOWN;
        case SDLK_KP_0: return 96;
        case SDLK_KP_MULTIPLY: return 106;
        case SDLK_KP_PLUS: return 107;
        case SDLK_KP_MINUS: return 109;
        case SDLK_KP_PERIOD: return 110;
        case SDLK_KP_DIVIDE: return 111;
        case SDLK_PRINTSCREEN: return 44;
        default: return 0;
    }
}

int sideKey(SDL_Keycode k) {
    switch (k) {
        case SDLK_LSHIFT: return VK_LSHIFT;
        case SDLK_RSHIFT: return VK_RSHIFT;
        case SDLK_LCTRL: return VK_LCONTROL;
        case SDLK_RCTRL: return VK_RCONTROL;
        case SDLK_LALT: return VK_LALT;
        case SDLK_RALT: return VK_RALT;
        default: return 0;
    }
}

bool isTouchscreen(SDL_TouchID id) {
#if SDL_VERSION_ATLEAST(2, 0, 10)
    return SDL_GetTouchDeviceType(id) == SDL_TOUCH_DEVICE_DIRECT;
#else
    (void)id;
    return true;
#endif
}

void describePad(Input::Pad& p, SDL_GameController* c) {
    p.connected = true;
    const char* nm = SDL_GameControllerName(c);
    p.name = nm ? nm : "Controller";
    char guid[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(c)), guid, sizeof guid);
    p.guid = guid;
}

}  // namespace

void HostInput::handleEvent(const SDL_Event& e, Runner& rt, SdlPlatform& platform, const RunOptions& opt, bool& quit) {
    auto padSlot = [&](SDL_JoystickID id) {
        for (int i = 0; i < Input::kPads; i++)
            if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) return i;
        return -1;
    };
    // A hidden or headless run is driven only by its --input/--press script: the real mouse,
    // keyboard and pads would make it depend on what the desk is doing.
    if ((opt.hidden || opt.headless) &&
        (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_TEXTINPUT || e.type == SDL_MOUSEMOTION ||
         e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEWHEEL ||
         e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP ||
         e.type == SDL_CONTROLLERAXISMOTION || e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP ||
         e.type == SDL_JOYAXISMOTION || e.type == SDL_JOYHATMOTION))
        return;
    if (e.type == SDL_QUIT) {
        quit = true;
        return;
    }
    if (!opt.hidden && !opt.headless && menu.handleEvent(e, rt, platform)) return;
    switch (e.type) {
        case SDL_KEYDOWN: case SDL_KEYUP: {
            if (e.key.repeat) break;
            bool down = e.type == SDL_KEYDOWN;
            if (down && e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
                platform.setFullscreen(!platform.fullscreen());
                break;
            }
            rt.input.keyEvent(vkFromSdl(e.key.keysym.sym), down);
            // Text input stays off (it wakes macOS's input method system for nothing);
            // keyboard_lastchar / keyboard_string come from printable key presses.
            if (down) {
                SDL_Keycode k = e.key.keysym.sym;
                bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
                if (k >= SDLK_a && k <= SDLK_z) rt.input.textEvent(std::string(1, static_cast<char>((shift ? 'A' : 'a') + (k - SDLK_a))));
                else if (k >= SDLK_SPACE && k < 127 && !shift) rt.input.textEvent(std::string(1, static_cast<char>(k)));
            }
            if (int s = sideKey(e.key.keysym.sym)) rt.input.keyEvent(s, down);
            break;
        }
        case SDL_TEXTINPUT: rt.input.textEvent(e.text.text); break;
        case SDL_MOUSEWHEEL:
            rt.input.wheelLatch += e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            break;
        case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP: {
            // Touchscreens only (trackpads also send finger events): the on-screen controls
            // first, then the first finger is the mouse, the next ones device_mouse_* 1..4.
            if (!opt.touch || !isTouchscreen(e.tfinger.touchId)) break;
            {
                int sw, sh;
                platform.drawableSize(sw, sh);
                if (touchPad.handleFinger(e.tfinger, rt, sw, sh)) break;
            }
            int slot = -1;
            for (int i = 0; i < Input::kTouches; i++)
                if (fingerUsed[i] && fingers[i] == e.tfinger.fingerId) slot = i;
            if (slot < 0 && e.type == SDL_FINGERDOWN)
                for (int i = 0; i < Input::kTouches && slot < 0; i++)
                    if (!fingerUsed[i]) {
                        slot = i;
                        fingerUsed[i] = true;
                        fingers[i] = e.tfinger.fingerId;
                    }
            if (slot < 0) break;
            int dw, dh;
            platform.drawableSize(dw, dh);
            double wx, wy;
            rt.gfx->screenToWindow(e.tfinger.x * dw, e.tfinger.y * dh, wx, wy);
            bool down = e.type != SDL_FINGERUP;
            if (slot == 0) {
                rt.input.mouseX = wx;
                rt.input.mouseY = wy;
                if (e.type != SDL_FINGERMOTION) rt.input.mouseButton(0, down);
                vmActive = false;  // touch takes over from the stick pointer
            } else {
                rt.input.touchEvent(slot, wx, wy, down);
            }
            if (!down) fingerUsed[slot] = false;
            break;
        }
        case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            // SDL also turns touches into mouse events; the finger events above handle those.
            if (opt.touch && (e.type == SDL_MOUSEMOTION ? e.motion.which : e.button.which) == SDL_TOUCH_MOUSEID) break;
            vmActive = false;
            int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
            int my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
            int ww, wh, dw, dh;
            SDL_GetWindowSize(platform.window, &ww, &wh);
            platform.drawableSize(dw, dh);
            // Without a touchscreen (desktop, --touch-controls) the left button works the
            // on-screen controls as one finger.
            if (touchPad.visible && (e.type != SDL_MOUSEMOTION || (e.motion.state & SDL_BUTTON_LMASK)) &&
                (e.type == SDL_MOUSEMOTION || e.button.button == SDL_BUTTON_LEFT)) {
                SDL_TouchFingerEvent f{};
                f.type = e.type == SDL_MOUSEBUTTONDOWN ? SDL_FINGERDOWN : e.type == SDL_MOUSEBUTTONUP ? SDL_FINGERUP : SDL_FINGERMOTION;
                f.fingerId = 0x6d6f757365;  // "mouse"
                f.x = ww ? static_cast<float>(mx) / ww : 0;
                f.y = wh ? static_cast<float>(my) / wh : 0;
                if (touchPad.handleFinger(f, rt, dw, dh)) break;
            }
            double sx = ww ? mx * static_cast<double>(dw) / ww : mx, sy = wh ? my * static_cast<double>(dh) / wh : my;
            if (e.type == SDL_MOUSEMOTION) {
                rt.input.mouseLatchDX += e.motion.xrel;
                rt.input.mouseLatchDY += e.motion.yrel;
            }
            rt.gfx->screenToWindow(sx, sy, rt.input.mouseX, rt.input.mouseY);
            if (e.type != SDL_MOUSEMOTION) {
                int b = e.button.button == SDL_BUTTON_LEFT ? 0 : (e.button.button == SDL_BUTTON_RIGHT ? 1 : 2);
                rt.input.mouseButton(b, e.type == SDL_MOUSEBUTTONDOWN);
            }
            break;
        }
        case SDL_CONTROLLERDEVICEADDED: {
            if (padSlot(SDL_JoystickGetDeviceInstanceID(e.cdevice.which)) >= 0) break;  // opened by openExisting
            int slot = -1;
            for (int i = 0; i < Input::kPads; i++)
                if (!pads[i]) {
                    slot = i;
                    break;
                }
            if (slot >= 0 && (pads[slot] = SDL_GameControllerOpen(e.cdevice.which))) {
                describePad(rt.input.pads[slot], pads[slot]);
                fprintf(stderr, "[input] controller %d connected: %s\n", slot, rt.input.pads[slot].name.c_str());
            }
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {
            int slot = padSlot(e.cdevice.which);
            if (slot >= 0) {
                for (int b = 0; b < GP_BUTTON_COUNT; b++) buttons.press(rt, slot, b, false);
                SDL_GameControllerClose(pads[slot]);
                pads[slot] = nullptr;
                rt.input.pads[slot] = Input::Pad();
                fprintf(stderr, "[input] controller %d disconnected\n", slot);
            }
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
            int slot = padSlot(e.cbutton.which);
            static const int map[] = {
                GP_FACE1, GP_FACE2, GP_FACE3, GP_FACE4, GP_SELECT, -1, GP_START, GP_STICKL,
                GP_STICKR, GP_SHOULDERL, GP_SHOULDERR, GP_PADU, GP_PADD, GP_PADL, GP_PADR};
            if (slot >= 0 && e.cbutton.button < sizeof(map) / sizeof(map[0]) && map[e.cbutton.button] >= 0)
                buttons.press(rt, slot, map[e.cbutton.button], e.type == SDL_CONTROLLERBUTTONDOWN);
            break;
        }
        case SDL_CONTROLLERAXISMOTION: {
            int slot = padSlot(e.caxis.which);
            if (slot < 0) break;
            float v = e.caxis.value / 32767.f;
            v = std::max(-1.f, std::min(1.f, v));
            auto& p = rt.input.pads[slot];
            switch (e.caxis.axis) {
                case SDL_CONTROLLER_AXIS_LEFTX: p.axis[GP_AXISLH] = v; break;
                case SDL_CONTROLLER_AXIS_LEFTY: p.axis[GP_AXISLV] = v; break;
                case SDL_CONTROLLER_AXIS_RIGHTX: p.axis[GP_AXISRH] = v; break;
                case SDL_CONTROLLER_AXIS_RIGHTY: p.axis[GP_AXISRV] = v; break;
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT: buttons.press(rt, slot, GP_SHOULDERLB, v > rt.input.buttonThreshold); break;
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: buttons.press(rt, slot, GP_SHOULDERRB, v > rt.input.buttonThreshold); break;
            }
            break;
        }
        default: break;
    }
}

void HostInput::update(Runner& rt, SdlPlatform& platform, const RunOptions& opt) {
    // The on-screen controls give way to a real controller (and come back when it goes).
    bool show = touchControlsWanted && (touchControlsForced || !physicalPad()) && platform.window;
    if (show && platform.now() - insetCheck > 0.5) {  // a notch moves sides when the phone turns
        targetSafeArea(platform.window, touchPad.inset);
        insetCheck = platform.now();
    }
    if (show != touchPad.visible) {
        if (!show) touchPad.releaseAll(rt);
        touchPad.visible = show;
        if (show && !rt.input.pads[0].connected) rt.input.pads[0].connected = true, rt.input.pads[0].name = "Touch controls";
    }
    // Pad 0's left stick also presses the keys the d-pad does, for games that only read the keyboard.
    {
        auto& p0 = rt.input.pads[0];
        bool st[4] = {p0.axis[GP_AXISLH] < -0.5f, p0.axis[GP_AXISLH] > 0.5f, p0.axis[GP_AXISLV] < -0.5f,
                      p0.axis[GP_AXISLV] > 0.5f};
        static const int stickDir[4] = {GP_PADL, GP_PADR, GP_PADU, GP_PADD};
        for (int k = 0; k < 4; k++)
            if (st[k] != stickKey[k]) {
                for (int vk : buttons.map.keys[stickDir[k]]) buttons.key(rt, vk, st[k]);
                stickKey[k] = st[k];
            }
    }
    // Stick-driven pointer (games that read the mouse): the stick moves it, ZR is the left button
    // and ZL the right one; drawn over the frame unless the game hides the cursor.
    if (opt.mouseStick && usesMouse && rt.gfx && platform.window) {
        auto& p0 = rt.input.pads[0];
        float ax = p0.axis[opt.mouseStick == 1 ? GP_AXISLH : GP_AXISRH];
        float ay = p0.axis[opt.mouseStick == 1 ? GP_AXISLV : GP_AXISRV];
        int dw, dh;
        platform.drawableSize(dw, dh);
        if (vmX < 0) vmX = dw / 2.0, vmY = dh / 2.0;
        auto curve = [](float v) {  // dead zone, then a squared response for fine aiming
            float a = std::fabs(v);
            if (a < 0.2f) return 0.0;
            double t = (a - 0.2) / 0.8;
            return (v < 0 ? -1.0 : 1.0) * t * t;
        };
        double dt = 1.0 / (rt.roomSpeed > 0 ? rt.roomSpeed : 60);
        double vx = curve(ax) * opt.mouseSpeed * dh * dt, vy = curve(ay) * opt.mouseSpeed * dh * dt;
        if (vx != 0 || vy != 0) {
            vmActive = true;
            vmX = std::clamp(vmX + vx, 0.0, dw - 1.0);
            vmY = std::clamp(vmY + vy, 0.0, dh - 1.0);
            double ox = rt.input.mouseX, oy = rt.input.mouseY;
            rt.gfx->screenToWindow(vmX, vmY, rt.input.mouseX, rt.input.mouseY);
            rt.input.mouseLatchDX += rt.input.mouseX - ox;
            rt.input.mouseLatchDY += rt.input.mouseY - oy;
        }
        bool clicks[2] = {buttons.held[0][GP_SHOULDERRB], buttons.held[0][GP_SHOULDERLB]};
        for (int b = 0; b < 2; b++)
            if (clicks[b] != vmButton[b]) {
                vmButton[b] = clicks[b];
                vmActive = true;
                rt.input.mouseButton(b, clicks[b]);
            }
        rt.gfx->overlayCursor = vmActive && platform.cursorVisible;
        rt.gfx->overlayCursorX = rt.input.mouseX;
        rt.gfx->overlayCursorY = rt.input.mouseY;
    }
}

HostInput::HostInput() {
    touchPad.buttons = &buttons;
    menu.input = this;
    buttons.intercept = [this](Runner& rt, int source, int button, bool down) {
        return menu.handleButton(rt, source, button, down);
    };
}

void HostInput::setGame(const DataWin& dw, const RunOptions& opt) {
    usesMouse = gameUsesMouse(dw);
    iniPath = opt.gameDir + "/gmrunner.ini";
    buttons.reset();
    buttons.defaults = makeButtonMap(dw, opt);
    buttons.map = buttons.defaults;
    applyButtonChoices(buttons.map, opt.buttonMap);
    touchMode = opt.touchControls;
    decideTouchControls();
    menu.shortcut = opt.menuShortcut;
    menu.open = menu.exitRequested = false;
    for (bool& k : stickKey) k = false;
    if (!opt.hidden && !opt.headless) fprintf(stderr, "[input] %s\n", describeButtonMap(buttons.map).c_str());
}

void HostInput::reloadSettings() {
    RunOptions o;
    loadConfig(iniPath, o);
    buttons.map = buttons.defaults;
    applyButtonChoices(buttons.map, o.buttonMap);
    touchMode = o.touchControls;
    menu.shortcut = o.menuShortcut;
    decideTouchControls();
}

void HostInput::decideTouchControls() {
    ButtonMap& m = buttons.map;
    decideOnScreen(m);
    bool any = false;
    for (bool b : m.onScreen) any = any || b;
    if (touchMode == 1 && !any)  // asked for though nothing seems to need them: all that do something
        for (int b = 0; b < GP_BUTTON_COUNT; b++) m.onScreen[b] = !m.pad[b].empty() || !m.keys[b].empty();
    touchControlsWanted = touchMode == 1 || (touchMode == 0 && target().touchControls && any);
    touchControlsForced = touchMode == 1;
}

void HostInput::announcePads(Runner& rt) {
    for (int i = 0; i < Input::kPads; i++)
        if (pads[i]) describePad(rt.input.pads[i], pads[i]);
    for (bool& k : stickKey) k = false;
}

void HostInput::openExisting(Runner& rt) {
    for (int j = 0; j < SDL_NumJoysticks(); j++) {
        if (!SDL_IsGameController(j)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        bool open = false;
        for (auto* p : pads)
            if (p && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == id) open = true;
        if (open) continue;
        for (int i = 0; i < Input::kPads; i++)
            if (!pads[i]) {
                if ((pads[i] = SDL_GameControllerOpen(j))) describePad(rt.input.pads[i], pads[i]);
                break;
            }
    }
}

bool HostInput::physicalPad() const {
    for (auto* p : pads)
        if (p) return true;
    return false;
}

void HostInput::closeAll() {
    for (auto*& p : pads)
        if (p) SDL_GameControllerClose(p), p = nullptr;
}

}  // namespace gm::host
