// The game picker: a list of game folders, chosen by tapping, clicking, or with the d-pad and
// the bottom face button / Enter. Drawn with the host's own Painter before any game is loaded.
#include <algorithm>
#include <cmath>

#include "platform/sdl/Host.h"
#include "platform/sdl/Ui.h"
#include "render/Renderer.h"
#include "util/Png.h"

namespace gm::host {

namespace {

std::string baseName(const std::string& path) {
    size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

// Words into lines of at most `cols` characters (paths break at '/').
std::vector<std::string> wrap(const std::string& text, size_t cols) {
    std::vector<std::string> out;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find_first_of(" /", i);
        j = j == std::string::npos ? text.size() : j + 1;
        std::string word = text.substr(i, j - i);
        if (line.size() + word.size() > cols && !line.empty()) {
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out.push_back(line);
            line.clear();
        }
        line += word;
        i = j;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

}  // namespace

std::string runPicker(SdlPlatform& platform, bool gles, const std::function<std::vector<std::string>()>& scan,
                      const std::vector<std::string>& help, const std::string& shot) {
    Renderer r;
    if (!r.init(true, gles)) return {};
    Painter paint(r);
    std::vector<std::string> games = scan();
    std::vector<SDL_GameController*> pads;
    size_t sel = 0;
    double scroll = 0;            // list offset in pixels
    bool background = false;
    // A touch or click becomes a tap when it ends close to where it started.
    bool pressing = false;
    double pressX = 0, pressY = 0, pressScroll = 0;
    std::string chosen;
    bool done = false;
    while (!done) {
        int w, h;
        platform.drawableSize(w, h);
        double u = std::max(2.0, std::floor(std::min(w, h) / 200.0));  // one font pixel
        double rowH = 16 * u, listTop = 34 * u, listH = h - listTop - 14 * u;
        auto rowAt = [&](double y) -> int {
            double ly = y - listTop + scroll;
            if (y < listTop || y > listTop + listH || ly < 0) return -1;
            size_t i = static_cast<size_t>(ly / rowH);
            return i < games.size() ? static_cast<int>(i) : -1;
        };
        auto keepVisible = [&]() {
            double y = sel * rowH;
            if (y < scroll) scroll = y;
            if (y + rowH > scroll + listH) scroll = y + rowH - listH;
        };
        auto release = [&](double x, double y) {
            pressing = false;
            fprintf(stderr, "[picker] release at %.0f,%.0f (pressed at %.0f,%.0f), screen %dx%d, row %d\n", x, y, pressX,
                    pressY, w, h, rowAt(y));
            if (std::fabs(x - pressX) > 8 * u || std::fabs(y - pressY) > 8 * u) return;
            if (games.empty()) {
                games = scan();  // "tap to look again"
                return;
            }
            int i = rowAt(y);
            if (i >= 0) chosen = games[static_cast<size_t>(i)], done = true;
        };

        SDL_Event e;
        bool got = background ? SDL_WaitEvent(&e) : SDL_PollEvent(&e);
        while (got) {
            switch (e.type) {
                case SDL_QUIT: done = true; break;
                case SDL_APP_WILLENTERBACKGROUND: case SDL_APP_DIDENTERBACKGROUND: background = true; break;
                case SDL_APP_DIDENTERFOREGROUND:
                    background = false;
                    games = scan();  // files may have been copied in meanwhile
                    sel = std::min(sel, games.empty() ? 0 : games.size() - 1);
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    if (SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which)) pads.push_back(c);
                    break;
                case SDL_KEYDOWN: case SDL_CONTROLLERBUTTONDOWN: {
                    bool key = e.type == SDL_KEYDOWN;
                    int k = key ? e.key.keysym.sym : e.cbutton.button;
                    bool down = key ? k == SDLK_DOWN : k == SDL_CONTROLLER_BUTTON_DPAD_DOWN;
                    bool up = key ? k == SDLK_UP : k == SDL_CONTROLLER_BUTTON_DPAD_UP;
                    bool ok = key ? (k == SDLK_RETURN || k == SDLK_z || k == SDLK_SPACE) : k == SDL_CONTROLLER_BUTTON_A;
                    if (key && k == SDLK_ESCAPE) done = true;
                    if (games.empty()) {
                        if (ok) games = scan();
                        break;
                    }
                    if (down) sel = (sel + 1) % games.size();
                    if (up) sel = (sel + games.size() - 1) % games.size();
                    if (down || up) keepVisible();
                    if (ok) chosen = games[sel], done = true;
                    break;
                }
                case SDL_APP_WILLENTERFOREGROUND: break;
                case SDL_FINGERDOWN:
                    pressing = true;
                    pressX = e.tfinger.x * w, pressY = e.tfinger.y * h, pressScroll = scroll;
                    break;
                case SDL_FINGERMOTION:
                    if (pressing) scroll = pressScroll - (e.tfinger.y * h - pressY);
                    break;
                case SDL_FINGERUP:
                    if (pressing) release(e.tfinger.x * w, e.tfinger.y * h);
                    break;
                case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
                    if (e.button.which == SDL_TOUCH_MOUSEID) break;  // the finger events cover touches
                    int ww, wh;
                    SDL_GetWindowSize(platform.window, &ww, &wh);
                    double x = ww ? e.button.x * static_cast<double>(w) / ww : e.button.x;
                    double y = wh ? e.button.y * static_cast<double>(h) / wh : e.button.y;
                    if (e.type == SDL_MOUSEBUTTONDOWN) pressing = true, pressX = x, pressY = y, pressScroll = scroll;
                    else if (pressing) release(x, y);
                    break;
                }
                case SDL_MOUSEWHEEL: scroll -= e.wheel.y * rowH; break;
                default: break;
            }
            got = SDL_PollEvent(&e);
        }
        if (done || background) continue;
        double maxScroll = std::max(0.0, games.size() * rowH - listH);
        scroll = std::clamp(scroll, 0.0, maxScroll);

        r.bindTarget(-1, w, h);
        r.setViewport(0, 0, w, h);
        r.setProjection(0, 0, w, h);
        r.clear(rgb(24, 26, 32), 1);
        paint.text(10 * u, 8 * u, 2 * u, "gmRunner", rgb(255, 255, 255), 1);
        paint.text(10 * u, 24 * u, u, games.empty() ? "No games yet" : "Choose a game", rgb(150, 160, 180), 1);
        if (games.empty()) {
            double y = listTop + 4 * u;
            size_t cols = static_cast<size_t>(std::max(10.0, (w - 20 * u) / (6 * u)));
            for (auto& para : help) {
                for (auto& line : wrap(para, cols)) {
                    paint.text(10 * u, y, u, line, rgb(220, 225, 235), 1);
                    y += 10 * u;
                }
                y += 6 * u;
            }
        } else {
            r.setScissor(0, static_cast<int>(listTop), w, static_cast<int>(listH));
            for (size_t i = 0; i < games.size(); i++) {
                double y = listTop + i * rowH - scroll;
                if (y + rowH < listTop || y > listTop + listH) continue;
                bool on = i == sel;
                paint.rect(8 * u, y + u, w - 16 * u, rowH - 2 * u, on ? rgb(52, 90, 160) : rgb(40, 44, 54), 1);
                std::string name = baseName(games[i]);
                size_t fit = static_cast<size_t>(std::max(4.0, (w - 28 * u) / (6 * 1.5 * u)));
                if (name.size() > fit) name = name.substr(0, fit - 3) + "...";
                paint.text(14 * u, y + (rowH - 7 * 1.5 * u) / 2, 1.5 * u, name, rgb(255, 255, 255), 1);
            }
            r.flush();
            r.setScissor(0, 0, w, h);
        }
        r.flush();
        if (!shot.empty()) {
            std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
            r.readPixels(0, 0, w, h, px.data());
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            writePng(shot, px.data(), w, h);
            break;
        }
        SDL_GL_SwapWindow(platform.window);
    }
    for (auto* c : pads) SDL_GameControllerClose(c);  // the game loop opens them again
    return chosen;
}

}  // namespace gm::host
