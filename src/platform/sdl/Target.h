// What a build target does differently. Exactly one Target*.cpp is compiled in:
//   TargetDesktop.cpp   macOS, Windows, Linux desktops (a window; --smoke and other tools)
//   TargetSwitch.cpp    Nintendo Switch homebrew (__SWITCH__)
//   TargetHandheld.cpp  Linux handhelds through PortMaster (GMR_HANDHELD)
#pragma once

#include <string>
#include <vector>

struct SDL_Window;

namespace gm {

class Runner;

namespace host {

struct RunOptions;
struct Buttons;

struct TargetInfo {
    const char* osName;
    bool fixedScreen;      // fullscreen at the display's size, no window to move or resize
    int screenW, screenH;  // fixedScreen: this size, or 0 for the display's current mode
    bool gles;             // OpenGL ES 2 instead of desktop OpenGL 2.1
    bool sdlControllers;   // controllers through SDL's game controller API
    int mouseStick;        // default stick for the virtual mouse (0 off, 1 left, 2 right)
    bool devTools;         // --smoke and its temporary save folder
    bool picker;           // no game named: choose one on screen (targetGameList)
    bool touchControls;    // on-screen controls on touchscreens unless a controller is connected
    bool orientFromGame;   // landscape or portrait screen to suit the game's window
    bool returnToPicker;   // a game's end brings back the game list instead of quitting (iOS)
};
const TargetInfo& target();

void targetStart(int argc, char** argv);  // before anything else (logging, file systems)
void targetFinish(long frame, bool withError);  // after SDL has shut down
// The game folder to run, from the command line or the target's own places; "" if none.
std::string targetFindGame(int argc, char** argv, const std::vector<std::string>& positional);
std::string targetNoGameMessage();
// picker targets: the game folders to offer, and what to say when there are none.
std::vector<std::string> targetGameList();
std::vector<std::string> targetPickerHelp();
// A launcher of the platform's own (iOS); false to use the drawn picker.
bool targetNativePick(std::string& chosen);
std::string targetSaveDir(const std::string& gameName);
void targetShowError(const std::string& msg);  // stderr, plus whatever screen the target has
bool targetFrame();  // once per frame; false when the system asks the program to exit
// Controllers the target reads itself instead of through SDL (the Switch's libnx pads).
void targetPollInput(Runner& rt, Buttons& buttons, const RunOptions& opt, bool& quit);
void targetGameChanged();  // a new Runner: controllers are announced to it again
void targetWindowCreated(SDL_Window* window);  // iOS: the window joins the app's scene
// Edges of the window the on-screen controls keep clear of (a phone's notch, camera cutout and
// home indicator), in drawable pixels: left, top, right, bottom. Zero where there are none.
void targetSafeArea(SDL_Window* window, double inset[4]);
// The in-game menu in the platform's own design (iOS, Android), from a JSON description of the
// game's controls and settings (GameMenu.cpp); false where the runner draws the menu itself. The
// native menu saves its changes in the game's gmrunner.ini.
bool targetShowMenu(const std::string& json);
int targetMenuResult();  // -1 while that menu is open, then 0 to resume or 1 to exit the game

}  // namespace host
}  // namespace gm
