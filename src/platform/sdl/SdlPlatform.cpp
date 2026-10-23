// The runtime's Platform on SDL2: window, display and clock. On a fixed screen (console,
// handheld) the window calls do nothing.
#include <algorithm>

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

void SdlPlatform::drawableSize(int& w, int& h) const {
    w = h = 0;
    if (window) SDL_GL_GetDrawableSize(window, &w, &h);
}

}  // namespace gm::host
