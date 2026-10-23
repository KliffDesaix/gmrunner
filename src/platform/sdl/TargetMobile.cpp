// Phone and tablet target: iOS and Android (GMR_MOBILE). Games are folders the player copies
// into the app's own folder, as emulators do:
//   iOS      the app's Documents folder, shown in the Files app as On My iPhone > gmRunner
//   Android  gmrunner on internal storage (/storage/emulated/0/gmrunner; the app asks for
//            All files access), or the app's own external folder without that permission
// A picker lists them; saves go in Saves/<game> next to them. Fullscreen OpenGL ES, the screen
// turned to suit each game, on-screen controls unless a controller is connected.
#ifdef GMR_MOBILE

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

#include "platform/sdl/Host.h"
#include "platform/sdl/Target.h"
#include "util/Fs.h"

#ifdef __ANDROID__
#include <SDL_system.h>
#include <jni.h>
#else
#include <SDL_syswm.h>
#endif

namespace gm::host {

const TargetInfo& target() {
    static const TargetInfo info = {
#ifdef __ANDROID__
        "android",
#else
        "ios",
#endif
        true,   // fixedScreen
        0, 0,   // screen size: the display's
        true,   // gles
        true,   // sdlControllers
        2,      // mouseStick: right stick, with a controller
        false,  // devTools
        true,   // picker
        true,   // touchControls
        true,   // orientFromGame
#ifdef __ANDROID__
        false,  // returnToPicker: the game activity ends and the launcher activity is back
#else
        true,   // returnToPicker
#endif
    };
    return info;
}

namespace {

#ifdef __ANDROID__
const char* const kSharedRoot = "/storage/emulated/0/gmrunner";

// The shared folder when the app may use it (All files access), else the app's own folder.
std::string gamesRoot() {
    makeDirs(kSharedRoot);
    if (access(kSharedRoot, R_OK | W_OK) == 0) return kSharedRoot;
    const char* own = SDL_AndroidGetExternalStoragePath();
    return own ? own : ".";
}
#else
std::string gamesRoot() {
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/Documents";
}
#endif

}  // namespace

void targetStart(int, char**) {
    // The log goes next to the games, where the player (or adb) can find it.
    std::string root = gamesRoot();
    makeDirs(root);
    if (freopen((root + "/gmrunner.log").c_str(), "w", stderr)) setvbuf(stderr, nullptr, _IOLBF, 0);
    fprintf(stderr, "[mobile] gmrunner started, games in %s\n", root.c_str());
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // back is Escape, not "quit"
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "1");
}

void targetFinish(long frame, bool withError) {
    fprintf(stderr, "[mobile] exiting (frame %ld)%s\n", frame, withError ? ", with an error" : "");
}

std::string targetFindGame(int, char**, const std::vector<std::string>& positional) {
    return positional.empty() ? std::string() : positional[0];
}

std::string targetNoGameMessage() { return "No game found."; }

std::vector<std::string> targetGameList() {
    std::string root = gamesRoot();
    std::vector<std::string> found;
    if (DIR* d = opendir(root.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string path = root + "/" + e->d_name;
            if (e->d_name[0] != '.' && isDir(path) && !findDataFile(path).empty()) found.push_back(path);
        }
        closedir(d);
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::vector<std::string> targetPickerHelp() {
#ifdef __ANDROID__
    std::string root = gamesRoot();
    if (root != kSharedRoot)
        return {"gmrunner needs All files access to read games from the gmrunner folder on internal storage. "
                "Allow it in Settings > Apps > gmrunner, then come back.",
                "Or copy each game's folder (the one with data.win) into:", root, "Tap to look again."};
    return {"Copy each game's folder (the one with data.win) into the gmrunner folder on internal storage:", root,
            "Tap to look again."};
#else
    return {"Open the Files app, go to On My iPhone (or iPad) > gmRunner, and copy each game's folder "
            "(the one with data.win) there.",
            "Then come back here, or tap to look again."};
#endif
}

#ifdef __ANDROID__
bool targetNativePick(std::string&) { return false; }  // the launcher activity passes the game
#else
// The SwiftUI launcher (ios/Launcher.swift): shows the games until one is chosen.
extern "C" int gmr_ios_run_launcher(const char* root, char* out, int outSize);
bool targetNativePick(std::string& chosen) {
    char buf[4096] = {};
    if (!gmr_ios_run_launcher(gamesRoot().c_str(), buf, sizeof buf)) return false;
    chosen = buf;
    return true;
}
#endif

std::string targetSaveDir(const std::string& gameName) { return gamesRoot() + "/Saves/" + gameName; }

void targetShowError(const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "gmrunner", msg.c_str(), nullptr);
}

bool targetFrame() { return true; }
void targetPollInput(Runner&, Buttons&, const RunOptions&, bool&) {}
void targetGameChanged() {}

#ifdef __ANDROID__
void targetWindowCreated(SDL_Window*) {}

// GameActivity.safeInsets(): the display cutout and system bars, in the surface's pixels.
void targetSafeArea(SDL_Window*, double inset[4]) {
    inset[0] = inset[1] = inset[2] = inset[3] = 0;
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, "safeInsets", "()[I");
    auto arr = m ? static_cast<jintArray>(env->CallObjectMethod(activity, m)) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (arr && env->GetArrayLength(arr) == 4) {
        jint v[4];
        env->GetIntArrayRegion(arr, 0, 4, v);
        for (int i = 0; i < 4; i++) inset[i] = v[i];
    }
    if (arr) env->DeleteLocalRef(arr);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

// The in-game menu: GameActivity.showMenu starts MenuActivity (Compose) over the game, which
// reports back through nativeMenuResult when it closes.
static std::atomic<int> gMenuResult{-1};
extern "C" JNIEXPORT void JNICALL Java_org_gmrunner_app_GameActivity_nativeMenuResult(JNIEnv*, jclass, jint result) {
    gMenuResult = result;
}

bool targetShowMenu(const std::string& json) {
    gMenuResult = -1;
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return false;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, "showMenu", "(Ljava/lang/String;)Z");
    jstring text = env->NewStringUTF(json.c_str());
    bool shown = m && env->CallBooleanMethod(activity, m, text);
    if (env->ExceptionCheck()) env->ExceptionClear(), shown = false;
    env->DeleteLocalRef(text);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return shown;
}

int targetMenuResult() { return gMenuResult.load(); }
#else
// iOS shows only windows that belong to a scene; SDL2 makes its window without one.
extern "C" void gmr_ios_attach_window(void* uiWindow);  // ios/Launcher.swift
void targetWindowCreated(SDL_Window* window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_UIKIT)
        gmr_ios_attach_window(static_cast<void*>(info.info.uikit.window));
}

// The window's safe area (notch or Dynamic Island, home indicator), in points, scaled to pixels.
extern "C" void gmr_ios_safe_area(void* uiWindow, double* insets);  // ios/Launcher.swift
void targetSafeArea(SDL_Window* window, double inset[4]) {
    inset[0] = inset[1] = inset[2] = inset[3] = 0;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_UIKIT) return;
    gmr_ios_safe_area(static_cast<void*>(info.info.uikit.window), inset);
    int ww, wh, dw, dh;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    double sx = ww > 0 ? double(dw) / ww : 1, sy = wh > 0 ? double(dh) / wh : 1;
    inset[0] *= sx, inset[2] *= sx, inset[1] *= sy, inset[3] *= sy;
}

// The in-game menu: a SwiftUI sheet over the game (ios/GameMenu.swift).
extern "C" int gmr_ios_show_menu(const char* json);
extern "C" int gmr_ios_menu_result(void);
bool targetShowMenu(const std::string& json) { return gmr_ios_show_menu(json.c_str()) != 0; }
int targetMenuResult() { return gmr_ios_menu_result(); }
#endif

}  // namespace gm::host

#endif
