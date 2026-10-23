// Linux handheld target (PortMaster: ArkOS, ROCKNIX, muOS, Knulli and others). Fullscreen
// OpenGL ES 2 at the panel's resolution, controllers through SDL (PortMaster sets
// SDL_GAMECONTROLLERCONFIG for the device), Start + Select to quit. The launcher script names
// the game folder and the save folder (tools/make-portmaster.sh writes it).
#ifdef GMR_HANDHELD

#include <unistd.h>

#include <algorithm>
#include <cstdio>

#include "platform/sdl/Host.h"
#include "platform/sdl/Target.h"
#include "util/Fs.h"

namespace gm::host {

const TargetInfo& target() {
    static const TargetInfo info = {
        "linux-handheld",  // osName
        true,              // fixedScreen
        0, 0,              // screen size: the display's current mode
        true,              // gles
        true,              // sdlControllers
        2,                 // mouseStick: right stick
        true,              // devTools (--smoke works over SSH)
        false,  // picker
        false,  // touchControls
        false,  // orientFromGame
        false,  // returnToPicker
    };
    return info;
}

namespace {

// The folder the executable is in ("" if unknown).
std::string exeDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    return parentDir(std::string(buf, static_cast<size_t>(n)));
}

}  // namespace

void targetStart(int, char**) {
    SDL_version v;
    SDL_GetVersion(&v);
    fprintf(stderr, "[handheld] gmrunner started, SDL %d.%d.%d\n", v.major, v.minor, v.patch);
}

void targetFinish(long frame, bool withError) {
    fprintf(stderr, "[handheld] exiting (frame %ld)%s\n", frame, withError ? ", with an error" : "");
}

// The folder on the command line, or else a single game in games/ next to the executable.
std::string targetFindGame(int, char**, const std::vector<std::string>& positional) {
    if (!positional.empty()) {
        const std::string& a = positional[0];
        return isFile(a) ? parentDir(a) : a;
    }
    std::string root = exeDir() + "/games";
    std::vector<std::string> found;
    if (DIR* d = opendir(root.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string path = root + "/" + e->d_name;
            if (e->d_name[0] != '.' && isDir(path) && !findDataFile(path).empty()) found.push_back(path);
        }
        closedir(d);
    }
    std::sort(found.begin(), found.end());
    if (found.size() > 1) {
        fprintf(stderr, "[handheld] %zu games in %s, starting the first; name one on the command line:\n", found.size(),
                root.c_str());
        for (auto& f : found) fprintf(stderr, "  %s\n", f.c_str());
    }
    return found.empty() ? std::string() : found[0];
}

std::string targetNoGameMessage() {
    return "No game found. Copy the game's files (data.win and the rest) into the port's game folder.";
}

std::vector<std::string> targetGameList() { return {}; }
std::vector<std::string> targetPickerHelp() { return {}; }
bool targetNativePick(std::string&) { return false; }

std::string targetSaveDir(const std::string& gameName) {
    char* pref = SDL_GetPrefPath("gmrunner", gameName.c_str());
    std::string p = pref ? pref : ("./saves/" + gameName);
    SDL_free(pref);
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

// No message boxes on a KMS/DRM screen: the launcher keeps stderr in the port's log.txt.
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
