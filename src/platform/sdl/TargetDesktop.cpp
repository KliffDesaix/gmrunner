// Desktop target: macOS, Windows and Linux desktops. A window, SDL game controllers, and the
// developer tools (--smoke with its own throwaway saves).
#if !defined(__SWITCH__) && !defined(GMR_HANDHELD) && !defined(GMR_MOBILE)

#include <cstdio>

#include "platform/sdl/Host.h"
#include "platform/sdl/Target.h"

namespace gm::host {

const TargetInfo& target() {
    static const TargetInfo info = {
        "desktop",  // osName
        false,      // fixedScreen
        0, 0,       // screen size
        false,      // gles (--gles asks for it)
        true,       // sdlControllers
        0,          // mouseStick: games get the real mouse
        true,       // devTools
        false,  // picker
        false,  // touchControls
        false,  // orientFromGame
        false,  // returnToPicker
    };
    return info;
}

void targetStart(int, char**) {}
void targetFinish(long, bool) {}

std::string targetFindGame(int, char**, const std::vector<std::string>& positional) {
    return positional.empty() ? "." : positional[0];
}

std::string targetNoGameMessage() { return "No game found. Usage: gmrunner <game folder>"; }

std::vector<std::string> targetGameList() { return {}; }
std::vector<std::string> targetPickerHelp() { return {}; }
bool targetNativePick(std::string&) { return false; }

std::string targetSaveDir(const std::string& gameName) {
    char* pref = SDL_GetPrefPath("gmrunner", gameName.c_str());
    std::string p = pref ? pref : ("./saves/" + gameName);
    SDL_free(pref);
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    return p;
}

void targetShowError(const std::string& msg) { fprintf(stderr, "%s\n", msg.c_str()); }
bool targetFrame() { return true; }
void targetPollInput(Runner&, Buttons&, const RunOptions&, bool&) {}
void targetWindowCreated(SDL_Window*) {}
void targetSafeArea(SDL_Window*, double inset[4]) { inset[0] = inset[1] = inset[2] = inset[3] = 0; }
bool targetShowMenu(const std::string&) { return false; }
int targetMenuResult() { return 0; }
void targetGameChanged() {}

}  // namespace gm::host

#endif
