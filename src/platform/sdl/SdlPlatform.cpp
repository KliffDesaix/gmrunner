// The runtime's Platform on SDL2: window, display and clock. On a fixed screen (console,
// handheld) the window calls do nothing.
#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "platform/sdl/Host.h"
#include "platform/sdl/Target.h"

namespace gm::host {

void SdlPlatform::setFullscreen(bool on) {
    if (fixedWindow) return;
    if (window) SDL_SetWindowFullscreen(window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    fs_ = on;
}

void SdlPlatform::setWindowSize(int w, int h) {
    if (fixedWindow || !window || fs_) return;
    if (rotate == 90 || rotate == 270) std::swap(w, h);
    double s = std::min({1.0, (dispW * 0.9) / w, (dispH * 0.9) / h});
    SDL_SetWindowSize(window, static_cast<int>(w * s), static_cast<int>(h * s));
}

void SdlPlatform::setWindowCaption(const std::string& c) {
    if (window && !c.empty()) SDL_SetWindowTitle(window, c.c_str());
}

void SdlPlatform::setCursorVisible(bool on) {
    cursorVisible = on;
    SDL_ShowCursor(on ? SDL_ENABLE : SDL_DISABLE);
}

void SdlPlatform::windowPosition(int& x, int& y) const {
    x = y = 0;
    if (window && !fixedWindow) SDL_GetWindowPosition(window, &x, &y);
}

void SdlPlatform::setWindowPosition(int x, int y) {
    if (window && !fixedWindow && !fs_) SDL_SetWindowPosition(window, x, y);
}

void SdlPlatform::setWindowSizeLimits(int minW, int minH, int maxW, int maxH) {
    if (!window || fixedWindow) return;
    SDL_SetWindowMinimumSize(window, std::max(0, minW), std::max(0, minH));
    if (maxW > 0 && maxH > 0) SDL_SetWindowMaximumSize(window, maxW, maxH);
}

void SdlPlatform::setWindowFlag(WindowFlag f, bool on) {
    if (!window || fixedWindow) return;
    switch (f) {
        case WindowFlag::Border: SDL_SetWindowBordered(window, on ? SDL_TRUE : SDL_FALSE); break;
        case WindowFlag::Resizable: SDL_SetWindowResizable(window, on ? SDL_TRUE : SDL_FALSE); break;
        case WindowFlag::OnTop:
#if SDL_VERSION_ATLEAST(2, 0, 16)
            SDL_SetWindowAlwaysOnTop(window, on ? SDL_TRUE : SDL_FALSE);
#endif
            break;
        case WindowFlag::Visible: on ? SDL_ShowWindow(window) : SDL_HideWindow(window); break;
    }
}

void SdlPlatform::warpMouse(int x, int y) {  // x, y in drawable pixels; SDL warps in window points
    if (!window || fixedWindow) return;
    int ww, wh, dw, dh;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    SDL_WarpMouseInWindow(window, dw ? x * ww / dw : x, dh ? y * wh / dh : y);
}

void SdlPlatform::warpMouseGlobal(int x, int y) {
    if (!fixedWindow) SDL_WarpMouseGlobal(x, y);
}

double SdlPlatform::displayDpi() const {
    float d = 0;
    if (fixedWindow) return 96;
    return window && SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(window), &d, nullptr, nullptr) == 0 && d > 0 ? d : 96;
}

double SdlPlatform::now() const {
    return SDL_GetPerformanceCounter() / static_cast<double>(SDL_GetPerformanceFrequency());
}

const char* SdlPlatform::osName() const { return target().osName; }

// SDL 2.0.14 functions: on Linux the SDL2 is the system's (PortMaster firmwares can be older), so
// they're looked up when needed instead of being required to start.
#if SDL_VERSION_ATLEAST(2, 0, 14)
#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
template <typename F> static F* newerSdl(F*, const char* name) {
    return reinterpret_cast<F*>(dlsym(RTLD_DEFAULT, name));
}
#else
template <typename F> static F* newerSdl(F* fn, const char*) { return fn; }
#endif
#define NEWER_SDL(fn) newerSdl(&fn, #fn)
#endif

bool SdlPlatform::hasFocus() const {
    return !window || fixedWindow || (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

void SdlPlatform::padRumble(int pad, double left, double right) {
    if (!pads || pad < 0 || pad >= static_cast<int>(pads->size()) || !(*pads)[pad]) return;
    auto motor = [](double v) { return static_cast<Uint16>(std::clamp(v, 0.0, 1.0) * 0xFFFF); };
    // GameMaker's vibration lasts until changed; SDL's runs for at most this long.
    SDL_GameControllerRumble((*pads)[pad], motor(left), motor(right), 0xFFFF);
}

void SdlPlatform::padLight(int pad, uint32_t rgb) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (!pads || pad < 0 || pad >= static_cast<int>(pads->size()) || !(*pads)[pad]) return;
    if (auto setLed = NEWER_SDL(SDL_GameControllerSetLED))
        setLed((*pads)[pad], (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
#else
    (void)pad, (void)rgb;
#endif
}

// Only over a visible desktop window: a hidden one (tests, --hidden) or a console screen has
// nobody to answer a dialog that would wait for them.
bool SdlPlatform::showMessage(const std::string& text, bool question) {
    if (!window || fixedWindow || (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN)) return false;
    const SDL_MessageBoxButtonData yesNo[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"},
    };
    const SDL_MessageBoxButtonData ok[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "OK"}};
    std::string title = SDL_GetWindowTitle(window);
    SDL_MessageBoxData box = {question ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION, window, title.c_str(),
                              text.c_str(), question ? 2 : 1, question ? yesNo : ok, nullptr};
    int pressed = 0;
    return SDL_ShowMessageBox(&box, &pressed) == 0 && pressed == 1;
}

void SdlPlatform::locale(std::string& language, std::string& region) const {
    Platform::locale(language, region);
    std::string tag;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    auto preferred = NEWER_SDL(SDL_GetPreferredLocales);
    if (SDL_Locale* l = preferred ? preferred() : nullptr) {
        if (l[0].language) tag = std::string(l[0].language) + (l[0].country ? "_" + std::string(l[0].country) : "");
        SDL_free(l);
    }
#endif
    if (tag.empty())
        for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"})
            if (const char* v = getenv(var); v && *v && std::string(v) != "C" && std::string(v) != "POSIX") {
                tag = v;
                break;
            }
    // "pt_BR.UTF-8", "en-US": language, then region
    size_t sep = tag.find_first_of("_-");
    std::string lang = tag.substr(0, std::min(sep, tag.find('.')));
    if (lang.size() >= 2) language = lang;
    if (sep != std::string::npos && tag.size() >= sep + 3) region = tag.substr(sep + 1, 2);
    for (char& c : language) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (char& c : region) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
}

void SdlPlatform::drawableSize(int& w, int& h) const {
    w = h = 0;
    if (window) SDL_GL_GetDrawableSize(window, &w, &h);
}

}  // namespace gm::host
