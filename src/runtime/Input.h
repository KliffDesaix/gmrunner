// Keyboard, gamepad and mouse state with GameMaker's per-step pressed/released semantics.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace gm {

// GameMaker virtual key codes used by the runtime itself.
enum Vk : int {
    VK_NOKEY = 0, VK_ANYKEY = 1, VK_BACKSPACE = 8, VK_TAB = 9, VK_ENTER = 13, VK_SHIFT = 16, VK_CONTROL = 17,
    VK_ALT = 18, VK_PAUSE = 19, VK_ESCAPE = 27, VK_SPACE = 32, VK_PAGEUP = 33, VK_PAGEDOWN = 34, VK_END = 35,
    VK_HOME = 36, VK_LEFT = 37, VK_UP = 38, VK_RIGHT = 39, VK_DOWN = 40, VK_INSERT = 45, VK_DELETE = 46,
    VK_F1 = 112, VK_LSHIFT = 160, VK_RSHIFT = 161, VK_LCONTROL = 162, VK_RCONTROL = 163, VK_LALT = 164,
    VK_RALT = 165,
};

// GameMaker gamepad constants (gp_face1 = 32769 ...).
enum GpButton : int {
    GP_FACE1 = 0, GP_FACE2, GP_FACE3, GP_FACE4, GP_SHOULDERL, GP_SHOULDERR, GP_SHOULDERLB, GP_SHOULDERRB,
    GP_SELECT, GP_START, GP_STICKL, GP_STICKR, GP_PADU, GP_PADD, GP_PADL, GP_PADR, GP_BUTTON_COUNT
};
enum GpAxis : int { GP_AXISLH = 0, GP_AXISLV, GP_AXISRH, GP_AXISRV, GP_AXIS_COUNT };
constexpr int kGpBase = 32769;

struct Input {
    static constexpr int kKeys = 256;
    static constexpr int kPads = 4;

    // Raw state written by the platform layer.
    bool raw[kKeys] = {};
    bool rawPressLatch[kKeys] = {};
    bool rawReleaseLatch[kKeys] = {};

    // Per-step state seen by GML (after keyboard_set_map remapping).
    bool down[kKeys] = {}, pressed[kKeys] = {}, released[kKeys] = {};
    int map[kKeys];
    int lastKey = 0;
    std::string lastChar, keyboardString;

    struct Pad {
        bool connected = false;
        bool raw[GP_BUTTON_COUNT] = {};
        bool latchPress[GP_BUTTON_COUNT] = {}, latchRelease[GP_BUTTON_COUNT] = {};
        bool down[GP_BUTTON_COUNT] = {}, pressed[GP_BUTTON_COUNT] = {}, released[GP_BUTTON_COUNT] = {};
        float axis[GP_AXIS_COUNT] = {};
        std::string name, guid;  // gamepad_get_description / gamepad_get_guid (set by the host)
    } pads[kPads];
    float buttonThreshold = 0.5f;

    double mouseX = 0, mouseY = 0;  // in window (backbuffer) pixels
    bool mouseRaw[3] = {}, mouseDown[3] = {}, mousePressed[3] = {}, mouseReleased[3] = {};
    bool mouseLatchPress[3] = {}, mouseLatchRelease[3] = {};
    bool wheelUp = false, wheelDown = false;  // mouse_wheel_up/down: the wheel moved since the last step
    int wheelLatch = 0;                       // accumulated wheel movement (positive = up)
    double mouseDeltaX = 0, mouseDeltaY = 0;  // window_mouse_get_delta_x/y: movement during the last step
    double mouseLatchDX = 0, mouseLatchDY = 0;
    // Extra touch points for device_mouse_*(1..4) (device 0 is the mouse); window pixels.
    static constexpr int kTouches = 5;
    struct Touch {
        double x = 0, y = 0;
        bool raw = false, down = false, pressed = false, released = false, latchPress = false, latchRelease = false;
    } touches[kTouches];

    Input() {
        for (int i = 0; i < kKeys; i++) map[i] = i;
    }

    void keyEvent(int vk, bool isDown) {
        if (vk <= 1 || vk >= kKeys) return;
        if (isDown && !raw[vk]) rawPressLatch[vk] = true;
        if (!isDown && raw[vk]) rawReleaseLatch[vk] = true;
        raw[vk] = isDown;
        if (isDown) lastKey = vk;
    }
    void textEvent(const std::string& s) {
        lastChar = s;
        keyboardString += s;
        if (keyboardString.size() > 1024) keyboardString.erase(0, keyboardString.size() - 1024);
    }
    void padButton(int pad, int button, bool isDown) {
        if (pad < 0 || pad >= kPads || button < 0 || button >= GP_BUTTON_COUNT) return;
        Pad& p = pads[pad];
        if (isDown && !p.raw[button]) p.latchPress[button] = true;
        if (!isDown && p.raw[button]) p.latchRelease[button] = true;
        p.raw[button] = isDown;
    }
    void mouseButton(int b, bool isDown) {
        if (b < 0 || b >= 3) return;
        if (isDown && !mouseRaw[b]) mouseLatchPress[b] = true;
        if (!isDown && mouseRaw[b]) mouseLatchRelease[b] = true;
        mouseRaw[b] = isDown;
    }
    void touchEvent(int device, double x, double y, bool isDown) {
        if (device <= 0 || device >= kTouches) return;
        Touch& t = touches[device];
        t.x = x;
        t.y = y;
        if (isDown && !t.raw) t.latchPress = true;
        if (!isDown && t.raw) t.latchRelease = true;
        t.raw = isDown;
    }

    // Called once at the start of every game step.
    void beginStep() {
        bool anyDown = false, anyPressed = false, anyReleased = false;
        bool nd[kKeys] = {}, np[kKeys] = {}, nr[kKeys] = {};
        for (int k = 2; k < kKeys; k++) {
            int m = map[k];
            if (m < 0 || m >= kKeys) continue;
            // A key that was tapped and released within one frame still counts as down for that step.
            nd[m] |= raw[k] || rawPressLatch[k];
            np[m] |= rawPressLatch[k];
            nr[m] |= rawReleaseLatch[k];
            rawPressLatch[k] = rawReleaseLatch[k] = false;
        }
        for (int k = 2; k < kKeys; k++) {
            down[k] = nd[k];
            pressed[k] = np[k];
            released[k] = nr[k];
            anyDown |= nd[k];
            anyPressed |= np[k];
            anyReleased |= nr[k];
        }
        down[VK_ANYKEY] = anyDown;
        pressed[VK_ANYKEY] = anyPressed;
        released[VK_ANYKEY] = anyReleased;
        down[VK_NOKEY] = !anyDown;
        pressed[VK_NOKEY] = released[VK_NOKEY] = false;

        for (auto& p : pads) {
            for (int b = 0; b < GP_BUTTON_COUNT; b++) {
                p.down[b] = p.raw[b] || p.latchPress[b];
                p.pressed[b] = p.latchPress[b];
                p.released[b] = p.latchRelease[b];
                p.latchPress[b] = p.latchRelease[b] = false;
            }
        }
        for (int b = 0; b < 3; b++) {
            mouseDown[b] = mouseRaw[b] || mouseLatchPress[b];
            mousePressed[b] = mouseLatchPress[b];
            mouseReleased[b] = mouseLatchRelease[b];
            mouseLatchPress[b] = mouseLatchRelease[b] = false;
        }
        for (auto& t : touches) {
            t.down = t.raw || t.latchPress;
            t.pressed = t.latchPress;
            t.released = t.latchRelease;
            t.latchPress = t.latchRelease = false;
        }
        wheelUp = wheelLatch > 0;
        wheelDown = wheelLatch < 0;
        wheelLatch = 0;
        mouseDeltaX = mouseLatchDX;
        mouseDeltaY = mouseLatchDY;
        mouseLatchDX = mouseLatchDY = 0;
    }

    void clear() {  // io_clear
        std::memset(down, 0, sizeof down);
        std::memset(pressed, 0, sizeof pressed);
        std::memset(released, 0, sizeof released);
        std::memset(raw, 0, sizeof raw);
        for (auto& p : pads) {
            std::memset(p.down, 0, sizeof p.down);
            std::memset(p.pressed, 0, sizeof p.pressed);
            std::memset(p.released, 0, sizeof p.released);
        }
        keyboardString.clear();
    }
};

}  // namespace gm
