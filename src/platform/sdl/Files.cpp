// Game folders and paths.
#include <sstream>

#include "data/DataWin.h"
#include "platform/sdl/Host.h"
#include "util/Fs.h"

namespace gm::host {

std::string findDataFile(const std::string& dir) {
    // Windows, Linux (assets/), Android, iOS/macOS (app bundle) builds.
    for (const char* n : {"data.win", "game.unx", "game.droid", "game.ios", "assets/game.unx", "assets/game.droid",
                          "Contents/Resources/game.ios"}) {
        std::string p = dir + "/" + n;
        if (isFile(p)) return p;
    }
    return {};
}

std::string normalizePath(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    for (std::string seg; std::getline(ss, seg, '/');) {
        if (seg.empty() || seg == ".") continue;
        if (seg == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
        else parts.push_back(seg);
    }
    std::string out = !path.empty() && path[0] == '/' ? "/" : "";
    for (size_t i = 0; i < parts.size(); i++) out += (i ? "/" : "") + parts[i];
    return out.empty() ? "." : out;
}

bool gameUsesMouse(const DataWin& dw) {
    for (auto& f : dw.functions) {
        std::string_view n = f.name;
        if (n.rfind("mouse_check_button", 0) == 0 || n.rfind("device_mouse_", 0) == 0 ||
            n.rfind("mouse_wheel", 0) == 0 || n.rfind("window_mouse_get", 0) == 0)
            return true;
    }
    for (auto& v : dw.variables)
        if (v.name == "mouse_x" || v.name == "mouse_y" || v.name == "mouse_button" || v.name == "mouse_lastbutton")
            return true;
    return false;
}

}  // namespace gm::host
