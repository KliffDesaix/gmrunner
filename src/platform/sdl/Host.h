// The SDL2 host, shared by every target: options, the SDL Platform, input, developer tools.
// What differs per target (finding the game, saves, error screen, controllers, the system's
// main loop) is in Target.h, implemented by one of TargetDesktop.cpp, TargetSwitch.cpp and
// TargetHandheld.cpp.
#pragma once

#include <SDL.h>

#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "runtime/Input.h"
#include "platform/sdl/Ui.h"
#include "runtime/Platform.h"

namespace gm {

class DataWin;
class Renderer;
class Runner;

namespace host {


struct RunOptions {
    std::string gameDir;
    bool headless = false;      // no window, no GL: logic only
    bool hidden = false;        // GL in a hidden window (for screenshots)
    long frames = -1;           // stop after N steps
    bool fast = false;          // no frame limiter
    bool mute = false;
    bool trace = false;
    std::vector<std::pair<long, std::string>> screenshots;
    std::string screenshotDir = ".";
    std::string sheet;  // tile all screenshots into one PNG
    std::string recordAudio;  // mix audio offline into a WAV instead of playing it
    std::vector<std::pair<long, std::string>> gotoRooms;  // debug: jump to a room at a frame
    // debug, each "SPEC@FRAME": --create OBJECT[:x:y], --set OBJECT.VAR=NUMBER or global.VAR=NUMBER,
    // --count OBJECT[,OBJECT...] (prints how many instances there are, "all" for every object)
    std::vector<std::pair<long, std::string>> debugCreate, debugSet, debugCount;
    std::vector<std::tuple<long, int, bool>> script;  // frame, vk (or kScriptMouse + button), down
    std::vector<std::tuple<long, double, double>> mouseScript;  // frame, window x, y ("F:AT:x;y")
    std::vector<std::tuple<long, int, double, double>> stickScript;  // frame, 0 left / 1 right, x, y ("F:RS:x;y")
    int scale = 0;
    bool linear = false;
    bool integerScale = false;
    int rotate = 0;
    bool padToKeys = true;
    bool showFps = false;
    bool stats = false;
    bool xboxLayout = false;
    // Virtual mouse for games that read the mouse: a stick moves a pointer, ZR/ZL click (the
    // target's default); touch drives the mouse and device_mouse_* 1..4.
    int mouseStick = 0;  // 0 off, 1 left stick, 2 right stick
    double mouseSpeed = 1.0;  // screen heights per second at full tilt
    bool touch = true;
    int touchControls = 0;  // on-screen controls: 0 auto (the target's default), 1 on, 2 off
    int menuShortcut = 0;  // how the in-game menu opens besides Start + Select: GameMenu::Shortcut
    // What buttons press (gmrunner.ini map_<button> = ..., --map BUTTON=TARGETS): button name, targets.
    std::vector<std::pair<std::string, std::string>> buttonMap;
    std::string pickerRoot;  // --picker DIR: choose a game folder in DIR on screen
    bool screenCapture = false;
    bool gles = false;  // desktop: request an OpenGL ES 2 context (the console code path)
    long seed = -1;
    bool check = false;
    bool smoke = false;      // --smoke: run every room headless and report problems
    long smokeBoot = 900;    // steps of normal start-up before the room tour
    long smokeFrames = 180;  // steps per room
    std::string smokeShots;  // --smoke-shots DIR: also save each room's last frame and contact sheets
    std::string saveDir;
};

// ---- Options.cpp
// Command line into `o` and `positional`; returns false when the program should exit (--help).
bool parseArgs(int argc, char** argv, RunOptions& o, std::vector<std::string>& positional);
// gmrunner.ini next to the game (applied after the command line has picked the game).
void loadConfig(const std::string& path, RunOptions& o);
constexpr int kScriptMouse = 100000;  // scripted mouse buttons: kScriptMouse + 0/1/2 (left/right/middle)
constexpr int kScriptPad = 200000;    // scripted pad 0 buttons: kScriptPad + GP_* button

// ---- Files.cpp
// The data file in a game folder (Windows, Linux, Android, macOS/iOS layouts), or "".
std::string findDataFile(const std::string& dir);
// "a/b/../c/./d" -> "a/c/d" (keeps a leading "/").
std::string normalizePath(const std::string& path);
// Whether the game reads the mouse at all (functions or built-in variables it references).
bool gameUsesMouse(const DataWin& dw);

// ---- SdlPlatform.cpp: the runtime's view of the window, display and clock.
class SdlPlatform : public Platform {
public:
    SDL_Window* window = nullptr;
    int dispW = 1280, dispH = 720;
    int rotate = 0;
    bool fs_ = false;
    bool fixedWindow = false;  // a console or handheld screen: always fullscreen, no window to move
    bool cursorVisible = true;  // window_set_cursor: cr_none hides it

    int displayWidth() const override { return dispW; }
    int displayHeight() const override { return dispH; }
    bool fullscreen() const override { return fs_; }
    void setFullscreen(bool on) override;
    void setWindowSize(int w, int h) override;
    void setWindowCaption(const std::string& c) override;
    void setCursorVisible(bool on) override;
    void windowPosition(int& x, int& y) const override;
    void setWindowPosition(int x, int y) override;
    void setWindowSizeLimits(int minW, int minH, int maxW, int maxH) override;
    void setWindowFlag(WindowFlag f, bool on) override;
    void warpMouse(int x, int y) override;
    void warpMouseGlobal(int x, int y) override;
    double displayDpi() const override;
    double now() const override;
    bool hasGraphics() const override { return window != nullptr; }
    const char* osName() const override;
    bool hasFocus() const override;
    void padRumble(int pad, double left, double right) override;
    void padLight(int pad, uint32_t rgb) override;
    bool showMessage(const std::string& text, bool question) override;
    void locale(std::string& language, std::string& region) const override;
    void drawableSize(int& w, int& h) const;
    std::vector<SDL_GameController*>* pads = nullptr;  // HostInput's, by GML device index
};

// ---- Picker.cpp: chooses a game on screen when none was named (phones, --picker). `scan`
// lists the game folders and is called again when the app comes back to the foreground;
// `help` is shown when there are none. Returns the folder, or "" if the app was closed.
// With `shot`, draws one frame into that PNG and returns "" (testing).
std::string runPicker(SdlPlatform& platform, bool gles, const std::function<std::vector<std::string>()>& scan,
                      const std::vector<std::string>& help, const std::string& shot = "");

// ---- Buttons.cpp: what each button does in the game. Controllers, the on-screen pad and scripted
// input press buttons by position (GP_*, gp_face1 being the bottom face button); a per-game map
// turns each into the game's own gamepad buttons and keyboard keys. Buttons the player hasn't set
// (gmrunner.ini map_<button> = "gp_face1 Z", "vk_space", "none") get defaults from the game's
// code: the keys it checks and whether it reads a gamepad.
struct ButtonMap {
    std::vector<int> pad[GP_BUTTON_COUNT];   // the game's gamepad buttons each one presses
    std::vector<int> keys[GP_BUTTON_COUNT];  // and keys (GameMaker virtual key codes)
    bool set[GP_BUTTON_COUNT] = {};          // chosen by the player rather than by default
    bool onScreen[GP_BUTTON_COUNT] = {};     // on the on-screen pad: does something in this game
    bool mouseOnly = false;                  // reads the mouse but neither keyboard nor gamepad
    std::vector<int> gameKeys;               // the keys found in the game's code
    bool gameKeysComplete = false;           // ... and no key the game checks is hidden in a variable
    bool readsKeyboard = false, readsGamepad = false, anyKey = false, readsUnknown = false;
};
ButtonMap makeButtonMap(const DataWin& dw, const RunOptions& opt);  // the game's defaults
// The player's choices (button name, targets: RunOptions::buttonMap) over the defaults.
void applyButtonChoices(ButtonMap& m, const std::vector<std::pair<std::string, std::string>>& choices);
void decideOnScreen(ButtonMap& m);  // again after a button changes
// map_<name>: a b x y (by position: a is the bottom one) l r l2 r2 select start l3 r3 up down left right.
const char* buttonName(int gp);
int buttonFromName(const std::string& name);  // -1 if unknown
// Targets as written in gmrunner.ini: "gp_face1 Z", "vk_space", "none".
std::string targetsText(const std::vector<int>& pad, const std::vector<int>& keys);
bool parseTargets(const std::string& text, std::vector<int>& pad, std::vector<int>& keys);
std::string keyName(int vk);  // "Z", "vk_space"
const char* padTargetName(int gp);  // "gp_face1"
std::string describeButtonMap(const ButtonMap& m);  // one line for the log

struct Buttons {
    static constexpr int kOnScreen = Input::kPads;  // the on-screen pad's source slot (game pad 0)
    ButtonMap map;
    ButtonMap defaults;  // the map without the player's choices ("auto")
    bool held[Input::kPads + 1][GP_BUTTON_COUNT] = {};  // what the player holds, per source, by position
    // Sees every press first (the in-game menu); true keeps it from the game.
    std::function<bool(Runner& rt, int source, int button, bool down)> intercept;
    void press(Runner& rt, int source, int button, bool down);
    void key(Runner& rt, int vk, bool down);  // a key outside the map (the stick as d-pad)
    void releaseAll(Runner& rt);              // before the map changes
    void reset();                             // a new Runner: nothing is held in it

private:
    void route(Runner& rt, int source, int button, bool down);
    int padCount[Input::kPads][GP_BUTTON_COUNT] = {};
    int keyCount[Input::kKeys] = {};
};

// ---- TouchPad.cpp: an on-screen controller (d-pad, four face buttons, L/R, Start/Select) on
// touchscreens, pressed like a real controller through the game's button map (as pad 0).
struct TouchPad {
    bool visible = false;
    double inset[4] = {};  // left, top, right, bottom: kept clear (targetSafeArea), in screen pixels
    struct Finger {
        SDL_FingerID id;
        int control;  // index into the layout's controls
        bool used;
        double x, y;  // where it is, in screen pixels
    };
    Finger fingers[10] = {};
    bool down[GP_BUTTON_COUNT] = {};
    Buttons* buttons = nullptr;  // where presses go; its map says which buttons are shown
    std::unique_ptr<Painter> painter;
    Renderer* painterOwner = nullptr;
    ~TouchPad();

    // Returns true when the touch belongs to the pad (the game doesn't see it as a mouse).
    bool handleFinger(const SDL_TouchFingerEvent& e, Runner& rt, int screenW, int screenH);
    void draw(Renderer& r, int screenW, int screenH);
    void releaseAll(Runner& rt);
};

// ---- GameMenu.cpp: the in-game menu (resume, controls, exit). Start + Select held together open
// it everywhere; on touchscreens also the shortcut chosen per game (gmrunner.ini menu_shortcut =
// button, fingers, back or none). The game pauses while it is open; what it changes is saved in
// the game's gmrunner.ini.
struct HostInput;
struct GameMenu {
    enum Shortcut { FLOATING_BUTTON, THREE_FINGERS, BACK_BUTTON, CONTROLLER_ONLY, SHORTCUT_COUNT };
    static const char* shortcutName(int s);  // as in gmrunner.ini
    static int shortcutFromName(const std::string& name);
    bool open = false;
    bool exitRequested = false;  // Exit game chosen
    int shortcut = FLOATING_BUTTON;
    HostInput* input = nullptr;

    // Every SDL event comes here first; true when the menu took it.
    bool handleEvent(const SDL_Event& e, Runner& rt, SdlPlatform& platform);
    // Controller buttons (through Buttons::intercept): navigation while open, Start + Select to open.
    bool handleButton(Runner& rt, int source, int button, bool down);
    void show(Runner& rt);
    void poll();  // each frame while open: a native menu (targetShowMenu) closing
    // Each frame while open: the left stick moves the selection too, and a held direction repeats.
    void tick(Runner& rt, double now);
    void draw(Renderer& r, int w, int h);
    ~GameMenu();
    bool native = false;  // shown by the platform rather than drawn

    // (GameMenu.cpp)
    int page = 0, sel = 0, editing = -1;
    double scroll = 0;
    std::vector<SDL_FingerID> touches;  // fingers down (three at once open the menu)
    bool tracking = false, dragged = false, onButton = false;
    SDL_FingerID trackId = 0;
    double trackX = 0, trackY = 0, scroll0 = 0;
    int lastW = 0, lastH = 0;
    int repeatDir = 0;      // -1 up, 1 down: the direction held, by the d-pad or the stick
    double repeatAt = 0;    // when it moves the selection again
    std::unique_ptr<Painter> painter;
    Renderer* painterOwner = nullptr;
};

// ---- HostInput.cpp: SDL events, controllers (through Buttons), touch, the stick mouse.
struct HostInput {
    std::vector<SDL_GameController*> pads = std::vector<SDL_GameController*>(Input::kPads, nullptr);
    Buttons buttons;
    bool stickKey[4] = {};
    bool usesMouse = false;
    double vmX = -1, vmY = -1;  // stick pointer in drawable pixels
    double insetCheck = -1;     // when the safe area was last read
    bool vmActive = false, vmButton[2] = {};
    SDL_FingerID fingers[Input::kTouches] = {};
    bool fingerUsed[Input::kTouches] = {};

    void handleEvent(const SDL_Event& e, Runner& rt, SdlPlatform& platform, const RunOptions& opt, bool& quit);
    // After the frame's events: the left stick as the d-pad's keys, the stick pointer.
    void update(Runner& rt, SdlPlatform& platform, const RunOptions& opt);
    TouchPad touchPad;
    GameMenu menu;
    std::string iniPath;  // the game's gmrunner.ini, where the menu saves
    int touchMode = 0;    // on-screen controls: 0 auto, 1 on, 2 off (as RunOptions::touchControls)
    void decideTouchControls();  // after touchMode or the map changed
    void reloadSettings();       // the game's gmrunner.ini again (a native menu changed it)
    HostInput();
    HostInput(const HostInput&) = delete;
    bool touchControlsWanted = false;  // decided by setGame: target, options and the game's buttons
    bool touchControlsForced = false;  // touch_controls = on: shown even with a controller connected

    // A game starts: its button map, and whether it gets on-screen controls (on touchscreen
    // targets, with the buttons that do something in it; touches that miss them are the mouse).
    void setGame(const DataWin& dw, const RunOptions& opt);
    void announcePads(Runner& rt);  // a new Runner (game_change) learns the open controllers
    void openExisting(Runner& rt);  // controllers already connected (their added events went to the picker)
    bool physicalPad() const;
    void closeAll();
};

// ---- DevTools.cpp: scripted input, debug commands, screenshots, audio recording, --smoke.
struct DevTools {
    size_t scriptPos = 0, mouseScriptPos = 0, stickScriptPos = 0;
    struct Shot {
        std::vector<uint8_t> px;
        int w, h;
    };
    std::vector<Shot> shots;
    std::vector<int16_t> recorded;
    double recordCarry = 0;

    explicit DevTools(RunOptions& opt);
    void beforeStep(Runner& rt, Buttons& buttons, const DataWin& dw, const RunOptions& opt, long frame);  // input script, --goto, ...
    void afterStep(Runner& rt, SdlPlatform& platform, const RunOptions& opt, long frame);  // screenshots, recording
    void finish(const RunOptions& opt);  // writes the sheet and the WAV
};
int runSmoke(Runner& rt, const DataWin& dw, const RunOptions& opt);

}  // namespace host
}  // namespace gm
