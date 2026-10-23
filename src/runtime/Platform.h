// Services the runtime needs from the host (window, display, clock, paths).
#pragma once

#include <string>

namespace gm {

class Platform {
public:
    virtual ~Platform() = default;
    virtual int displayWidth() const = 0;
    virtual int displayHeight() const = 0;
    virtual bool fullscreen() const { return false; }
    virtual void setFullscreen(bool) {}
    virtual void setWindowSize(int, int) {}
    virtual void setWindowCaption(const std::string&) {}
    // Desktop window extras (window_set_position, window_set_min_width, ...); no-ops elsewhere.
    enum class WindowFlag { Border, Resizable, OnTop, Visible };
    virtual void windowPosition(int& x, int& y) const { x = y = 0; }
    virtual void setWindowPosition(int, int) {}
    virtual void setWindowSizeLimits(int /*minW*/, int /*minH*/, int /*maxW*/, int /*maxH*/) {}
    virtual void setWindowFlag(WindowFlag, bool) {}
    virtual void setCursorVisible(bool) {}
    virtual void warpMouse(int, int) {}       // window coordinates
    virtual void warpMouseGlobal(int, int) {} // display coordinates
    virtual double displayDpi() const { return 96; }
    virtual double now() const = 0;  // seconds, monotonic
    virtual bool hasGraphics() const { return true; }
    virtual const char* osName() const { return "unknown"; }
};

}  // namespace gm
