// The Switch game list, in the console's own look (Borealis). gmrunner.nro calls
// gmr_switch_launcher when it was started without a game: the games in
// sdmc:/switch/gmrunner/games appear as a grid or a list (Y switches); A starts one, X (or a
// long touch) offers to rename it or give it an icon from the pictures in its folder. The title
// and icon live in the game's gmrunner.ini (title=, icon=), as the Android and iOS launchers keep
// them. Borealis (and with it SDL) is shut down again before the game starts.
#include <dirent.h>
#ifdef __SWITCH__
#include <switch.h>
#else
#include <glad/glad.h>  // the desktop preview's screenshots

#include "util/Png.h"
#endif
#include <sys/stat.h>

#include <algorithm>
#include <borealis.hpp>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Game {
    std::string dir, folder, title, icon;  // icon: a file in dir, or "" for the standard one
};

bool isFile(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
std::string trim(std::string s) {
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    return s;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream f(path);
    for (std::string l; std::getline(f, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}

std::string iniValue(const std::string& dir, const std::string& key) {
    for (auto& l : readLines(dir + "/gmrunner.ini")) {
        size_t eq = l.find('=');
        if (eq == std::string::npos || l[0] == '#' || l[0] == ';') continue;
        if (trim(l.substr(0, eq)) == key) return trim(l.substr(eq + 1));
    }
    return {};
}

// Sets (or with "" removes) one key, keeping the file's other lines as they are.
void setIniValue(const std::string& dir, const std::string& key, const std::string& value) {
    std::vector<std::string> lines = readLines(dir + "/gmrunner.ini");
    auto it = std::find_if(lines.begin(), lines.end(), [&](const std::string& l) {
        size_t eq = l.find('=');
        return eq != std::string::npos && trim(l.substr(0, eq)) == key;
    });
    if (value.empty()) {
        if (it == lines.end()) return;
        lines.erase(it);
    } else if (it != lines.end()) {
        *it = key + " = " + value;
    } else {
        lines.push_back(key + " = " + value);
    }
    std::ofstream f(dir + "/gmrunner.ini", std::ios::trunc);
    for (auto& l : lines) f << l << "\n";
}

Game loadGame(const std::string& dir) {
    Game g;
    g.dir = dir;
    g.folder = dir.substr(dir.find_last_of('/') + 1);
    g.title = iniValue(dir, "title");
    if (g.title.empty()) g.title = g.folder;
    std::string icon = iniValue(dir, "icon");
    if (!icon.empty() && isFile(dir + "/" + icon)) g.icon = icon;
    return g;
}

std::vector<Game> scanGames(const std::string& root) {
    static const char* const kData[] = {"data.win", "game.unx", "game.droid", "assets/game.unx", "assets/game.droid"};
    std::vector<Game> games;
    if (DIR* d = opendir(root.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string path = root + "/" + e->d_name;
            if (e->d_name[0] == '.' || !isDir(path)) continue;
            for (const char* f : kData)
                if (isFile(path + "/" + f)) {
                    games.push_back(loadGame(path));
                    break;
                }
        }
        closedir(d);
    }
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return lower(a.title) < lower(b.title); });
    return games;
}

// Pictures in a game's folder that can be its icon.
std::vector<std::string> picturesIn(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = lower(e->d_name);
            if (n.size() > 4 && (n.rfind(".png") == n.size() - 4 || n.rfind(".jpg") == n.size() - 4 ||
                                 (n.size() > 5 && n.rfind(".jpeg") == n.size() - 5)))
                out.push_back(e->d_name);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The third-party software in gmrunner.nro and its licenses (romfs:/licenses, from the
// repository's licenses/ folder).
struct Component {
    const char *name, *license, *file;
};
const Component kComponents[] = {
    {"gmRunner", "GNU GPL version 3 or later", "GPL-3.0.txt"},
    {"SDL2", "zlib License", "SDL2.txt"},
    {"GameMaker HTML5 runner (YoYo Games)", "Apache License 2.0", "gamemaker-html5.txt"},
    {"stb_image, stb_truetype, stb_vorbis", "MIT License or public domain", "stb.txt"},
    {"Liberation Fonts", "SIL Open Font License 1.1", "liberation-fonts.txt"},
    {"libnx", "ISC License", "libnx.txt"},
    {"Mesa", "MIT License and others", "mesa.txt"},
    {"libdrm", "MIT License", "libdrm.txt"},
    {"FFmpeg", "GNU GPL version 2 or later", "ffmpeg.txt"},
    {"dav1d", "BSD 2-Clause License", "dav1d.txt"},
    {"bzip2", "bzip2 License", "bzip2.txt"},
    {"zlib", "zlib License", "zlib.txt"},
    {"Borealis", "Apache License 2.0", "borealis.txt"},
    {"NanoVG", "zlib License", "nanovg.txt"},
    {"Yoga", "MIT License", "yoga.txt"},
    {"{fmt}", "MIT License", "fmt.txt"},
    {"TinyXML-2", "zlib License", "tinyxml2.txt"},
    {"Tweeny", "MIT License", "tweeny.txt"},
    {"libretro-common", "MIT License", "libretro-common.txt"},
    {"switch-libpulsar", "MIT License", "switch-libpulsar.txt"},
    {"glad", "MIT License", "glad.txt"},
    {"Material Icons font", "Apache License 2.0", "material-icons-font.txt"},
    {"GNU General Public License 2.0", "Full text", "GPL-2.0.txt"},
    {"Apache License 2.0", "Full text", "Apache-2.0.txt"},
};

// License files are wrapped for 80-100 columns: join each paragraph's lines so the text wraps to
// the screen. Numbered and bulleted items, tab-indented lines (lists of names) and Debian
// copyright fields ("Files:", "License:") keep lines of their own; " ." is a paragraph break.
std::string reflow(const std::string& text) {
    std::string out, para;
    std::istringstream in(text);
    auto flush = [&]() {
        if (para.empty()) return;
        if (!out.empty()) out += "\n\n";
        out += para;
        para.clear();
    };
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty() || t == ".") {
            flush();
            continue;
        }
        size_t d = 0;
        while (d < t.size() && isdigit(static_cast<unsigned char>(t[d]))) d++;
        bool item = (d > 0 && d < t.size() && (t[d] == '.' || t[d] == ')')) || t.rfind("- ", 0) == 0 ||
                    t.rfind("* ", 0) == 0 || t[0] == '(';
        size_t colon = line.find(':');
        bool field = !isspace(static_cast<unsigned char>(line[0])) && colon != std::string::npos && colon < 24 &&
                     line.find(' ') > colon;
        if (!para.empty()) para += item || field || line[0] == '\t' ? "\n" : " ";
        para += t;
    }
    flush();
    return out;
}

std::string readText(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A touch held still on a tile for half a second: the tile's long-press action. A shorter touch
// is a tap (the tile's A action), as Borealis's own tap gesture would do.
class HoldGesture : public brls::GestureRecognizer {
  public:
    HoldGesture(brls::View* view, std::function<void()> tap, std::function<void()> hold)
        : view_(view), tap_(std::move(tap)), hold_(std::move(hold)) {}

    brls::GestureState recognitionLoop(brls::TouchState touch, brls::MouseState mouse, brls::View* view,
                                       brls::Sound* sound) override {
        brls::TouchPhase phase = touch.phase;
        brls::Point pos = touch.position;
        if (phase == brls::TouchPhase::NONE) phase = mouse.leftButton, pos = mouse.position;
        if (!enabled || phase == brls::TouchPhase::NONE) return brls::GestureState::FAILED;
        switch (phase) {
            case brls::TouchPhase::START:
                start_ = pos;
                since_ = brls::getCPUTimeUsec();
                fired_ = false;
                state = brls::GestureState::UNSURE;
                brls::Application::giveFocus(view_);
                break;
            case brls::TouchPhase::STAY:
                if (state != brls::GestureState::UNSURE) break;
                if (std::abs(pos.x - start_.x) > 20 || std::abs(pos.y - start_.y) > 20) {
                    state = brls::GestureState::FAILED;  // a scroll, not a press
                } else if (!fired_ && brls::getCPUTimeUsec() - since_ > 500000) {
                    fired_ = true;
                    state = brls::GestureState::END;
                    *sound = brls::SOUND_CLICK;
                    hold_();
                }
                break;
            case brls::TouchPhase::END:
                if (state == brls::GestureState::UNSURE && !fired_) {
                    state = brls::GestureState::END;
                    *sound = brls::SOUND_CLICK;
                    view_->playClickAnimation();
                    tap_();
                }
                break;
            default: break;
        }
        return state;
    }

  private:
    brls::View* view_;
    std::function<void()> tap_, hold_;
    brls::Point start_{};
    brls::Time since_ = 0;
    bool fired_ = false;
};

class Launcher {
  public:
    // The game last started is kept next to the games folder, so the list opens on it again
    // when the player comes back from it.
    explicit Launcher(std::string root) : root_(std::move(root)) {
        std::ifstream f(lastPath());
        std::getline(f, last_);
    }

    std::string chosen;
    bool grid = true;

    // After the list is shown: the last game takes the focus (the first one otherwise).
    void focusLast() {
        if (lastView_) brls::Application::giveFocus(lastView_);
    }

    brls::View* build() {
        frame_ = new brls::AppletFrame(content());
        frame_->setTitle("gmRunner");
        frame_->setIcon(std::string(BRLS_RESOURCES) + "img/game.png");
        frame_->registerAction(grid ? "List" : "Grid", brls::BUTTON_Y, [this](brls::View*) {
            grid = !grid;
            refresh();
            return true;
        });
        frame_->registerAction("Quit", brls::BUTTON_START, [](brls::View*) {
            brls::Application::quit();
            return true;
        });
        frame_->registerAction("", brls::BUTTON_B, [](brls::View*) { return true; }, true);  // nothing to go back to
        frame_->registerAction("Licenses", brls::BUTTON_BACK, [this](brls::View*) {
            showLicenses();
            return true;
        });
        return frame_;
    }

    void optionsForFirst() {
        std::vector<Game> games = scanGames(root_);
        if (!games.empty()) showOptions(games[0]);
    }

    void refresh() {
        // A new frame with the other layout replaces the current one (keeps actions in sync).
        brls::Application::popActivity(brls::TransitionAnimation::NONE);
        brls::Application::pushActivity(new brls::Activity(build()), brls::TransitionAnimation::NONE);
        focusLast();
    }

  private:
    std::string root_;
    std::string last_;  // the folder of the game started last
    brls::View* lastView_ = nullptr;
    brls::AppletFrame* frame_ = nullptr;

    std::string lastPath() const {
        size_t slash = root_.find_last_of('/');
        return (slash == std::string::npos ? std::string(".") : root_.substr(0, slash)) + "/last_game.txt";
    }

    brls::View* content() {
        lastView_ = nullptr;
        std::vector<Game> games = scanGames(root_);
        if (games.empty()) {
            auto* box = new brls::Box(brls::Axis::COLUMN);
            box->setJustifyContent(brls::JustifyContent::CENTER);
            box->setAlignItems(brls::AlignItems::CENTER);
            box->setGrow(1);
            box->setFocusable(true);
            box->setHideHighlight(true);
            auto* title = new brls::Label();
            title->setText("No games yet");
            title->setFontSize(28);
            auto* help = new brls::Label();
            help->setText("Copy each game's folder (the one with data.win) to\n" + root_ + "/");
            help->setHorizontalAlign(brls::HorizontalAlign::CENTER);
            help->setMarginTop(20);
            box->addView(title);
            box->addView(help);
            return box;
        }
        auto* list = new brls::Box(brls::Axis::COLUMN);
        list->setPadding(30, 60, 30, 60);
        if (grid) {
            const int perRow = 5;
            brls::Box* row = nullptr;
            for (size_t i = 0; i < games.size(); i++) {
                if (i % perRow == 0) {
                    row = new brls::Box(brls::Axis::ROW);
                    row->setMarginBottom(24);
                    list->addView(row);
                }
                row->addView(tile(games[i]));
            }
        } else {
            for (auto& g : games) list->addView(rowView(g));
        }
        auto* scroll = new brls::ScrollingFrame();
        scroll->setContentView(list);
        scroll->setGrow(1);
        return scroll;
    }

    brls::Image* icon(const Game& g, float size) {
        auto* img = new brls::Image();
        img->setWidth(size);
        img->setHeight(size);
        img->setScalingType(brls::ImageScalingType::FILL);
        img->setCornerRadius(size * 0.12f);
        if (!g.icon.empty()) img->setImageFromFile(g.dir + "/" + g.icon);
        else img->setImageFromRes("img/game.png");
        return img;
    }

    // Shared by both layouts: A plays, X and a long touch open the options.
    void makeInteractive(brls::Box* v, const Game& g) {
        v->setFocusable(true);
        v->setHighlightCornerRadius(12);
        if (g.dir == last_) lastView_ = v;
        auto play = [this, g]() {
            chosen = g.dir;
            std::ofstream(lastPath()) << g.dir << "\n";
            brls::Application::quit();
        };
        auto options = [this, g]() { showOptions(g); };
        v->registerClickAction([play](brls::View*) {
            play();
            return true;
        });
        v->registerAction("Options", brls::BUTTON_X, [options](brls::View*) {
            options();
            return true;
        });
        v->addGestureRecognizer(new HoldGesture(v, play, options));
    }

    brls::View* tile(const Game& g) {
        auto* t = new brls::Box(brls::Axis::COLUMN);
        t->setWidth(208);
        t->setPadding(12, 12, 12, 12);
        t->setMarginRight(16);
        t->setAlignItems(brls::AlignItems::CENTER);
        t->addView(icon(g, 184));
        auto* label = new brls::Label();
        label->setText(g.title);
        label->setFontSize(18);
        label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        label->setSingleLine(true);
        label->setMarginTop(12);
        label->setWidth(184);
        t->addView(label);
        makeInteractive(t, g);
        return t;
    }

    brls::View* rowView(const Game& g) {
        auto* r = new brls::Box(brls::Axis::ROW);
        r->setHeight(96);
        r->setPadding(12, 20, 12, 20);
        r->setMarginBottom(8);
        r->setAlignItems(brls::AlignItems::CENTER);
        r->addView(icon(g, 72));
        auto* text = new brls::Box(brls::Axis::COLUMN);
        text->setMarginLeft(24);
        auto* title = new brls::Label();
        title->setText(g.title);
        title->setFontSize(22);
        text->addView(title);
        if (g.title != g.folder) {
            auto* sub = new brls::Label();
            sub->setText(g.folder);
            sub->setFontSize(16);
            sub->setTextColor(brls::Application::getTheme()["brls/text_disabled"]);
            text->addView(sub);
        }
        r->addView(text);
        makeInteractive(r, g);
        return r;
    }

  public:
    static void showLicenseText(const std::string& title, const std::string& file) {
        auto* text = new brls::Label();
        text->setText(reflow(readText(std::string(BRLS_RESOURCES) + "licenses/" + file)));
        text->setFontSize(16);
        auto* box = new brls::Box(brls::Axis::COLUMN);
        box->setPadding(30, 60, 30, 60);
        box->addView(text);
        // Something on this screen must take the focus, or the license list's highlight stays
        // drawn over a text short enough not to scroll.
        box->setFocusable(true);
        box->setHideHighlight(true);
        auto* scroll = new brls::ScrollingFrame();
        scroll->setContentView(box);
        auto* frame = new brls::AppletFrame(scroll);
        frame->setTitle(title);
        brls::Application::pushActivity(new brls::Activity(frame));
    }

    void showLicenses() {
        auto* list = new brls::Box(brls::Axis::COLUMN);
        list->setPadding(30, 60, 30, 60);
        auto* intro = new brls::Label();
        intro->setText("gmRunner is free software under the GNU General Public License, version 3 or later, and comes with NO WARRANTY. Source: github.com/KliffDesaix/gmrunner. It includes the following open-source software.");
        intro->setMarginBottom(20);
        list->addView(intro);
        for (const Component& c : kComponents) {
            auto* row = new brls::Box(brls::Axis::COLUMN);
            row->setPadding(14, 20, 14, 20);
            row->setMarginBottom(6);
            row->setFocusable(true);
            row->setHighlightCornerRadius(8);
            auto* name = new brls::Label();
            name->setText(c.name);
            name->setFontSize(20);
            auto* license = new brls::Label();
            license->setText(c.license);
            license->setFontSize(16);
            license->setTextColor(brls::Application::getTheme()["brls/text_disabled"]);
            row->addView(name);
            row->addView(license);
            std::string title = c.name, file = c.file;
            row->registerClickAction([title, file](brls::View*) {
                showLicenseText(title, file);
                return true;
            });
            row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
            list->addView(row);
        }
        auto* scroll = new brls::ScrollingFrame();
        scroll->setContentView(list);
        auto* frame = new brls::AppletFrame(scroll);
        frame->setTitle("Licenses");
        brls::Application::pushActivity(new brls::Activity(frame));
    }

  private:
    void showOptions(const Game& g) {
        std::vector<std::string> items = {"Rename", "Change icon"};
        if (!g.icon.empty()) items.push_back("Standard icon");
        auto* dropdown = new brls::Dropdown(g.title, items, [this, g](int sel) {
            if (sel == 0) rename(g);
            if (sel == 1) chooseIcon(g);
            if (sel == 2) {
                setIniValue(g.dir, "icon", "");
                refreshLater();
            }
        }, -1);  // a list of actions: none is "selected"
        brls::Application::pushActivity(new brls::Activity(dropdown));
    }

    void rename(const Game& g) {
        brls::Application::getImeManager()->openForText(
            [this, g](std::string text) {
                text = trim(text);
                setIniValue(g.dir, "title", text == g.folder ? "" : text);
                refreshLater();
            },
            "Rename", g.folder, 64, g.title);
    }

    void chooseIcon(const Game& g) {
        std::vector<std::string> pics = picturesIn(g.dir);
        if (pics.empty()) {
            auto* dialog = new brls::Dialog("There are no pictures in this game's folder.\nCopy a .png or .jpg into\n" +
                                            g.dir + "/\nto use it as the icon.");
            dialog->addButton("OK", []() {});
            dialog->open();
            return;
        }
        auto* dropdown = new brls::Dropdown("Icon for " + g.title, pics, [this, g, pics](int sel) {
            if (sel < 0 || sel >= static_cast<int>(pics.size())) return;
            setIniValue(g.dir, "icon", pics[static_cast<size_t>(sel)]);
            refreshLater();
        });
        brls::Application::pushActivity(new brls::Activity(dropdown));
    }

    // After a dropdown or the keyboard has closed: rebuild the list.
    void refreshLater() {
        brls::sync([this]() { refresh(); });
    }
};

}  // namespace

// Shows the game list until a game is chosen (1, its folder in `out`) or + is pressed (0);
// -1 if Borealis could not start.
extern "C" int gmr_switch_launcher(const char* root, char* out, int outSize) {
#ifdef __SWITCH__
    plInitialize(PlServiceType_User);  // the system font
    setsysInitialize();                // the system language and theme
#endif
    brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_AUTO;
    int ok = -1;
    if (brls::Application::init()) {
        ok = 0;
        brls::Application::createWindow("gmRunner");
        brls::Application::setGlobalQuit(false);
        Launcher launcher(root);
#ifndef __SWITCH__
        // Desktop preview aids: GMR_LAUNCHER_LIST=1 starts in list view, GMR_LAUNCHER_OPTIONS=1 opens
        // the first game's options, GMR_LAUNCHER_LICENSES=1 the licenses, GMR_LAUNCHER_LICENSE=file.txt
        // one license, GMR_LAUNCHER_SHOT=file.png saves a frame and closes.
        if (getenv("GMR_LAUNCHER_LIST")) launcher.grid = false;
        const char* shot = getenv("GMR_LAUNCHER_SHOT");
        int frames = 0;
#endif
        brls::Application::pushActivity(new brls::Activity(launcher.build()));
        launcher.focusLast();
        while (brls::Application::mainLoop()) {
#ifndef __SWITCH__
            frames++;
            if (frames == 30 && getenv("GMR_LAUNCHER_OPTIONS")) launcher.optionsForFirst();
            if (frames == 30 && getenv("GMR_LAUNCHER_LICENSES")) launcher.showLicenses();
            if (frames == 30 && getenv("GMR_LAUNCHER_LICENSE")) Launcher::showLicenseText("License", getenv("GMR_LAUNCHER_LICENSE"));
            if (shot && frames == 90) {
                GLint vp[4];
                glGetIntegerv(GL_VIEWPORT, vp);
                std::vector<uint8_t> px(static_cast<size_t>(vp[2]) * vp[3] * 4), flipped(px.size());
                glReadBuffer(GL_BACK);
                glReadPixels(0, 0, vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                size_t row = static_cast<size_t>(vp[2]) * 4;
                for (int y = 0; y < vp[3]; y++) memcpy(&flipped[y * row], &px[(vp[3] - 1 - y) * row], row);
                for (size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
                gm::writePng(shot, flipped.data(), vp[2], vp[3]);
                brls::Application::quit();
            }
#endif
        }
        if (!launcher.chosen.empty()) {
            snprintf(out, static_cast<size_t>(outSize), "%s", launcher.chosen.c_str());
            ok = 1;
        }
    }
#ifdef __SWITCH__
    setsysExit();
    plExit();
#endif
    return ok;
}
