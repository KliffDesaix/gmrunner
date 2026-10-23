// Command line and gmrunner.ini.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "platform/sdl/Host.h"
#include "platform/sdl/Target.h"

#ifndef GMR_VERSION
#define GMR_VERSION "dev"
#endif

namespace gm::host {

namespace {

int vkFromName(const std::string& n) {
    static const std::map<std::string, int> names = {
        {"LEFT", VK_LEFT},   {"RIGHT", VK_RIGHT}, {"UP", VK_UP},         {"DOWN", VK_DOWN},
        {"ENTER", VK_ENTER}, {"ESC", VK_ESCAPE},  {"ESCAPE", VK_ESCAPE}, {"SPACE", VK_SPACE},
        {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL}, {"F1", VK_F1}};
    auto it = names.find(n);
    if (it != names.end()) return it->second;
    if (n.size() == 1 && isalnum(static_cast<unsigned char>(n[0]))) return toupper(n[0]);
    return atoi(n.c_str());
}

// "--input 120:Z:down,126:Z:up" or "--press 120:Z" (down at frame, up 4 frames later)
void parseScript(const std::string& s, RunOptions& o, bool press) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t a = item.find(':');
        if (a == std::string::npos) continue;
        long frame = atol(item.substr(0, a).c_str());
        size_t b = item.find(':', a + 1);
        std::string key = item.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        for (char& c : key) c = static_cast<char>(toupper(c));
        if ((key == "LS" || key == "RS") && b != std::string::npos) {  // tilt pad 0's stick: "frame:RS:x;y"
            std::string v = item.substr(b + 1);
            size_t semi = v.find(';');
            if (semi != std::string::npos)
                o.stickScript.emplace_back(frame, key == "RS", atof(v.substr(0, semi).c_str()), atof(v.substr(semi + 1).c_str()));
            continue;
        }
        if (key == "AT" && b != std::string::npos) {  // move the mouse: "frame:AT:x;y"
            std::string pos = item.substr(b + 1);
            size_t semi = pos.find(';');
            if (semi != std::string::npos)
                o.mouseScript.emplace_back(frame, atof(pos.substr(0, semi).c_str()), atof(pos.substr(semi + 1).c_str()));
            continue;
        }
        // Pad 0's buttons: ZL, ZR, or PAD0..PAD15 (gp_face1 = PAD0 ... gp_padr = PAD15).
        int vk = key == "LMB" ? kScriptMouse : key == "RMB" ? kScriptMouse + 1 : key == "MMB" ? kScriptMouse + 2
                 : key == "ZR"  ? kScriptPad + GP_SHOULDERRB : key == "ZL" ? kScriptPad + GP_SHOULDERLB
                 : key.rfind("PAD", 0) == 0 && key.size() > 3 && isdigit(static_cast<unsigned char>(key[3]))
                       ? kScriptPad + atoi(key.c_str() + 3)
                       : vkFromName(key);
        if (press) {
            o.script.emplace_back(frame, vk, true);
            o.script.emplace_back(frame + 4, vk, false);
        } else {
            std::string state = b == std::string::npos ? "down" : item.substr(b + 1);
            o.script.emplace_back(frame, vk, state != "up");
        }
    }
}

}  // namespace

bool parseArgs(int argc, char** argv, RunOptions& opt, std::vector<std::string>& positional) {
    opt.mouseStick = target().mouseStick;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--headless") opt.headless = true;
        else if (a == "--hidden") opt.hidden = true;
        else if (a == "--frames") opt.frames = atol(next().c_str());
        else if (a == "--fast") opt.fast = true;
        else if (a == "--mute") opt.mute = true;
        else if (a == "--trace") opt.trace = true;
        else if (a == "--stats") opt.stats = true;
        else if (a == "--capture-screen") opt.screenCapture = true;
        else if (a == "--gles") opt.gles = true;
        else if (a == "--check") opt.check = true;
        else if (a == "--smoke") opt.smoke = true;
        else if (a == "--smoke-boot") opt.smokeBoot = atol(next().c_str());
        else if (a == "--smoke-frames") opt.smokeFrames = atol(next().c_str());
        else if (a == "--smoke-shots") opt.smokeShots = next();
        else if (a == "--seed") opt.seed = atol(next().c_str());
        else if (a == "--save-dir") opt.saveDir = next();
        else if (a == "--scale") opt.scale = atoi(next().c_str());
        else if (a == "--linear") opt.linear = true;
        else if (a == "--rotate") opt.rotate = atoi(next().c_str());
        else if (a == "--input") parseScript(next(), opt, false);
        else if (a == "--mouse-stick") {
            std::string v = next();
            opt.mouseStick = v == "left" ? 1 : v == "right" ? 2 : 0;
        }
        else if (a == "--press") parseScript(next(), opt, true);
        else if (a == "--touch-controls") opt.touchControls = 1;
        else if (a == "--map") {  // BUTTON=TARGETS, as map_<button> in gmrunner.ini
            std::string v = next();
            size_t eq = v.find('=');
            if (eq != std::string::npos) opt.buttonMap.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        }
        else if (a == "--picker") opt.pickerRoot = next();
        else if (a == "--screenshot-dir") opt.screenshotDir = next();
        else if (a == "--sheet") opt.sheet = next();
        else if (a == "--record-audio") opt.recordAudio = next();
        else if (a == "--goto") {  // NAME@FRAME
            std::string v = next();
            size_t at = v.find('@');
            opt.gotoRooms.push_back({at == std::string::npos ? 1 : atol(v.c_str() + at + 1), v.substr(0, at)});
        }
        else if (a == "--create" || a == "--set" || a == "--count") {
            std::string v = next();
            size_t at = v.rfind('@');
            auto& list = a == "--create" ? opt.debugCreate : a == "--set" ? opt.debugSet : opt.debugCount;
            list.push_back({at == std::string::npos ? 1 : atol(v.c_str() + at + 1), v.substr(0, at)});
        }
        else if (a == "--screenshot") {
            std::string v = next();  // frame[,frame...]
            std::stringstream ss(v);
            std::string f;
            while (std::getline(ss, f, ',')) opt.screenshots.push_back({atol(f.c_str()), "shot_" + f + ".png"});
        } else if (a == "--help" || a == "-h") {
            printf("gmRunner %s\n", GMR_VERSION);
            printf("usage: gmrunner [game dir] [--check] [--smoke] [--headless|--hidden] [--frames N] [--fast] [--mute]\n"
                   "                [--screenshot F1,F2] [--screenshot-dir DIR] [--press F:KEY,...]\n"
                   "                [--input F:KEY:down|up,...] [--scale N] [--linear] [--rotate 90]\n"
                   "                [--save-dir DIR] [--picker GAMES_DIR] [--touch-controls] [--map BUTTON=TARGETS]\n"
                   "       gmrunner [game dir] --check     report whether a game is likely to run\n"
                   "debugging: --goto ROOM@F, --create OBJECT|SEQUENCE[:x:y]@F, --set OBJECT.VAR=N@F, --set global.VAR=N@F,\n"
                   "           --count OBJECT[,OBJECT...]@F or all@F; --press/--input take PAD0..PAD15 for pad buttons,\n"
                   "           LMB/RMB/MMB for mouse buttons, F:AT:X;Y to move the mouse and F:LS:X;Y / F:RS:X;Y for sticks\n"
                   "\ngmRunner is free software under the GNU GPL version 3 or later, with NO WARRANTY.\n"
                   "Source and third-party licenses: https://github.com/KliffDesaix/gmrunner\n");
            return false;
        } else positional.push_back(a);
    }
    return true;
}

void loadConfig(const std::string& path, RunOptions& o) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && isspace(static_cast<unsigned char>(k.back()))) k.pop_back();
        while (!v.empty() && isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
        while (!v.empty() && isspace(static_cast<unsigned char>(v.front()))) v.erase(0, 1);
        bool b = v == "1" || v == "true" || v == "yes";
        if (k == "scale") o.scale = atoi(v.c_str());
        else if (k == "filter") o.linear = v == "linear";
        else if (k == "integer_scale") o.integerScale = b;
        else if (k == "rotate") o.rotate = atoi(v.c_str());
        else if (k == "gamepad_to_keyboard") o.padToKeys = b;
        else if (k == "show_fps") o.showFps = b;
        else if (k == "button_layout") o.xboxLayout = v == "xbox";
        else if (k == "mute") o.mute = b;
        else if (k == "mouse_stick") o.mouseStick = v == "left" ? 1 : v == "right" ? 2 : 0;
        else if (k == "mouse_speed") o.mouseSpeed = atof(v.c_str());
        else if (k == "touch") o.touch = b;
        else if (k == "touch_controls") o.touchControls = v == "on" || v == "1" ? 1 : v == "off" || v == "0" ? 2 : 0;
        else if (k == "menu_shortcut") o.menuShortcut = GameMenu::shortcutFromName(v);
        // What a button presses: map_a = gp_face1 Z (see Buttons.cpp). The command line wins.
        else if (k.rfind("map_", 0) == 0) {
            bool given = false;
            for (auto& [b, t] : o.buttonMap) given = given || b == k.substr(4);
            if (!given) o.buttonMap.emplace_back(k.substr(4), v);
        }
        // Diagnostics, for the log file on a console: room changes and sounds, and every call to
        // the listed engine functions with arguments and result (as GMR_TRACE_CALL).
        else if (k == "trace") o.trace = b;
        else if (k == "trace_calls") setenv("GMR_TRACE_CALL", v.c_str(), 1);
    }
}

}  // namespace gm::host
