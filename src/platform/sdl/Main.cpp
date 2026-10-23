// SDL2 host: loads a game, opens the window and audio, and runs the frame loop. The pieces it
// uses are in Host.h; what differs between desktop, Switch and Linux handhelds is in Target.h.
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <dirent.h>
#include <strings.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "data/DataWin.h"
#include "platform/sdl/Host.h"
#include "platform/sdl/Target.h"
#include "render/GL.h"
#include "render/Renderer.h"
#include "runtime/Audio.h"
#include "runtime/Graphics.h"
#include "runtime/Runner.h"
#include "util/Fs.h"

using namespace gm;
using namespace gm::host;

namespace {

void audioCallback(void* user, Uint8* stream, int len) {
    auto* audio = static_cast<Audio*>(user);
    audio->mix(reinterpret_cast<float*>(stream), len / static_cast<int>(sizeof(float) * 2));
}

std::string gameNameOf(const DataWin& dw) { return dw.gen.name.empty() ? "game" : std::string(dw.gen.name); }

// --smoke: headless (or hidden, for --smoke-shots), muted, seeded, with throwaway saves.
void prepareSmoke(RunOptions& opt) {
    opt.mute = opt.fast = true;
    if (opt.smokeShots.empty()) opt.headless = true;
    else opt.hidden = true, makeDirs(opt.smokeShots);  // screenshots need the renderer
    if (opt.seed < 0) opt.seed = 1;
    if (!opt.saveDir.empty()) return;  // never touch the player's real saves
#ifdef _WIN32
    const char* t = getenv("TEMP");
    opt.saveDir = std::string(t ? t : ".") + "\\gmrunner-smoke";
#else
    char tmpl[] = "/tmp/gmrunner-smoke-XXXXXX";
    opt.saveDir = mkdtemp(tmpl) ? tmpl : "/tmp/gmrunner-smoke";
#endif
}

// Phones stop the app's drawing when it goes to the background; the watch sees it at once
// (iOS must not draw from then on), the frame loop then waits.
SDL_atomic_t gBackground;
int SDLCALL watchApp(void*, SDL_Event* e) {
    if (e->type == SDL_APP_WILLENTERBACKGROUND || e->type == SDL_APP_DIDENTERBACKGROUND) SDL_AtomicSet(&gBackground, 1);
    if (e->type == SDL_APP_DIDENTERFOREGROUND) SDL_AtomicSet(&gBackground, 0);
    return 1;
}

// Game folders (with a data file) directly inside `root`, sorted.
std::vector<std::string> gamesIn(const std::string& root) {
    std::vector<std::string> found;
    if (DIR* d = opendir(root.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string path = root + "/" + e->d_name;
            if (e->d_name[0] != '.' && isDir(path) && !findDataFile(path).empty()) found.push_back(path);
        }
        closedir(d);
    }
    std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    return found;
}

// Lets the player choose a game on screen. Opens a window of its own (any orientation) and
// closes it again, so the game's window can be made to suit the game. "" if none was chosen.
std::string pickGame(const RunOptions& opt) {
    const TargetInfo& t = target();
    auto scan = [&]() { return opt.pickerRoot.empty() ? targetGameList() : gamesIn(opt.pickerRoot); };
    std::string chosenNatively;
    if (opt.pickerRoot.empty() && targetNativePick(chosenNatively)) return chosenNatively;
    std::vector<std::string> games = scan();
    if (games.size() == 1 && opt.pickerRoot.empty()) return games[0];  // nothing to choose
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | (t.sdlControllers ? SDL_INIT_GAMECONTROLLER : 0)) != 0) {
        targetShowError(std::string("SDL_Init failed: ") + SDL_GetError());
        return {};
    }
    SdlPlatform platform;
    bool gles = t.gles || opt.gles;
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, gles ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gles ? 0 : 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | (t.fixedScreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE);
    if (opt.hidden) flags |= SDL_WINDOW_HIDDEN;
    platform.window = SDL_CreateWindow("gmRunner", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 600, flags);
    if (platform.window) targetWindowCreated(platform.window);
    SDL_GLContext ctx = platform.window ? SDL_GL_CreateContext(platform.window) : nullptr;
    std::string chosen;
    if (ctx && !gl::load([](const char* n) { return SDL_GL_GetProcAddress(n); })) {
        SDL_GL_SetSwapInterval(1);
        // A hidden run with --screenshot saves the picker's first frame instead (testing).
        std::string shot = opt.hidden && !opt.screenshots.empty() ? opt.screenshotDir + "/picker.png" : "";
        std::vector<std::string> help = targetPickerHelp();
        if (!opt.pickerRoot.empty())
            help = {"Copy game folders (each with its data.win) into", opt.pickerRoot, "Then click here to look again."};
        chosen = runPicker(platform, gles, scan, help, shot);
    } else {
        targetShowError(std::string("Cannot open the game picker: ") + SDL_GetError());
    }
    if (ctx) SDL_GL_DeleteContext(ctx);
    if (platform.window) SDL_DestroyWindow(platform.window);
    return chosen;
}

// The window and its OpenGL (ES) context. Returns an error message, or "".
std::string openWindow(SdlPlatform& platform, const DataWin& dw, const RunOptions& opt, bool& gles, SDL_GLContext& ctx) {
    const TargetInfo& t = target();
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        platform.dispW = dm.w;
        platform.dispH = dm.h;
    }
    gles = t.gles || opt.gles;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, gles ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gles ? 0 : 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int ww, wh;
    Uint32 flags = SDL_WINDOW_OPENGL;
    if (t.orientFromGame) {  // phones: turn the screen to suit the game (or its rotate setting)
        int gw = dw.gen.windowWidth ? dw.gen.windowWidth : 640, gh = dw.gen.windowHeight ? dw.gen.windowHeight : 480;
        bool landscape = (gw >= gh) != (opt.rotate == 90 || opt.rotate == 270);
        SDL_SetHint(SDL_HINT_ORIENTATIONS, landscape ? "LandscapeLeft LandscapeRight" : "Portrait");
    }
    if (t.fixedScreen) {
        if (t.screenW > 0) platform.dispW = t.screenW, platform.dispH = t.screenH;
        ww = platform.dispW;
        wh = platform.dispH;
        flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI;  // phones: the panel's real pixels
        platform.fs_ = platform.fixedWindow = true;
    } else {
        int gw = dw.gen.windowWidth ? dw.gen.windowWidth : 640, gh = dw.gen.windowHeight ? dw.gen.windowHeight : 480;
        int scale = opt.scale > 0 ? opt.scale
                                  : std::max(1, std::min((platform.dispW * 8 / 10) / gw, (platform.dispH * 8 / 10) / gh));
        bool rot = opt.rotate == 90 || opt.rotate == 270;
        ww = (rot ? gh : gw) * scale;
        wh = (rot ? gw : gh) * scale;
        flags |= SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (opt.hidden) flags |= SDL_WINDOW_HIDDEN;
    }
    std::string title = dw.gen.displayName.empty() ? gameNameOf(dw) : std::string(dw.gen.displayName);
    platform.window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, flags);
    if (!platform.window && (flags & SDL_WINDOW_FULLSCREEN))
        platform.window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                                           flags & ~SDL_WINDOW_FULLSCREEN);
    if (!platform.window) return std::string("SDL_CreateWindow failed: ") + SDL_GetError();
    targetWindowCreated(platform.window);
    ctx = SDL_GL_CreateContext(platform.window);
    if (!ctx) return std::string("OpenGL context failed: ") + SDL_GetError();
    if (const char* missing = gl::load([](const char* n) { return SDL_GL_GetProcAddress(n); }))
        return std::string("OpenGL function missing: ") + missing;
    SDL_GL_SetSwapInterval(opt.fast ? 0 : 1);
    SDL_StopTextInput();
    if (t.fixedScreen) {
        int dw2, dh2;
        platform.drawableSize(dw2, dh2);
        fprintf(stderr, "[video] %dx%d fullscreen, %s\n", dw2, dh2, gles ? "OpenGL ES 2" : "OpenGL 2.1");
    }
    return {};
}

constexpr int kClosed = -1;  // the picker was closed without a game

// One game, from choosing it to its end. Returns the exit code, or kClosed.
int runOnce(int argc, char** argv) {
    const auto mainStart = std::chrono::steady_clock::now();
    const TargetInfo& tgt = target();
    RunOptions opt;
    std::vector<std::string> positional;
    if (!parseArgs(argc, argv, opt, positional)) return 0;
    if (!opt.pickerRoot.empty() || (tgt.picker && positional.empty())) {
        opt.gameDir = pickGame(opt);
        if (opt.gameDir.empty()) {  // closed without choosing
            SDL_Quit();
            targetFinish(0, false);
            return kClosed;
        }
    } else {
        opt.gameDir = targetFindGame(argc, argv, positional);
    }
    if (opt.gameDir.empty()) {
        targetShowError(targetNoGameMessage());
        targetFinish(0, true);
        return 1;
    }
    loadConfig(opt.gameDir + "/gmrunner.ini", opt);
    std::string dataPath = findDataFile(opt.gameDir);
    if (dataPath.empty()) {
        std::string where = opt.gameDir;
#ifndef _WIN32
        // Show the folder actually searched: relative paths depend on the current directory.
        char cwd[4096];
        if (!where.empty() && where[0] != '/' && where.find(':') == std::string::npos && getcwd(cwd, sizeof cwd))
            where = std::string(cwd) + "/" + where;
#endif
        targetShowError("No data.win found in " + where +
                        (isDir(opt.gameDir) ? "" : "\n(that folder does not exist - check the path)"));
        targetFinish(0, true);
        return 1;
    }
    // The game's other files (sounds, audio groups, included files) sit next to its data file.
    opt.gameDir = parentDir(dataPath);

    std::unique_ptr<DataWin> dw;
    auto loadStart = std::chrono::steady_clock::now();
    try {
        dw = DataWin::load(dataPath);
        fprintf(stderr, "[load] %s read and parsed in %.2fs\n", dataPath.c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - loadStart).count());
    } catch (const std::exception& e) {
        targetShowError(std::string("Cannot load ") + dataPath + ": " + e.what());
        targetFinish(0, true);
        return 1;
    }
    if (!opt.check) {
        std::string why;
        if (!dw->supported() && dw->unsupportedReason().find("YYC") == std::string::npos)
            why = "This game can't run yet: it was made with " + dw->unsupportedReason() + ".";
        else if (dw->code.empty() || !dw->supported())
            why = "This game can't run: it was compiled with YYC, so its code is native to the platform it\n"
                  "was built for and there is no GameMaker bytecode in data.win to run.";
        if (!why.empty()) {
            targetShowError(why);
            targetFinish(0, true);
            return 1;
        }
    }
    if (opt.check) {
        SdlPlatform none;
        Runner rt(*dw, none, opt.gameDir, "");
        bool ok = rt.printCompatibilityReport();
        ButtonMap m = makeButtonMap(*dw, opt);
        applyButtonChoices(m, opt.buttonMap);
        printf("\n%s\n", describeButtonMap(m).c_str());
        return ok ? 0 : 1;
    }
    if (opt.smoke) {
        if (!tgt.devTools) {
            targetShowError("--smoke is not available on this target");
            return 1;
        }
        prepareSmoke(opt);
    }

    Uint32 flags = SDL_INIT_TIMER | SDL_INIT_EVENTS;
    if (tgt.sdlControllers) flags |= SDL_INIT_GAMECONTROLLER;
    if (!opt.headless) flags |= SDL_INIT_VIDEO;
    if (!opt.mute && !opt.headless && opt.recordAudio.empty()) flags |= SDL_INIT_AUDIO;
    // Keep Ctrl+C / kill working even if game code is stuck in a loop (SDL would turn the signal
    // into a quit event that a stuck loop never reads).
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(flags) != 0) {
        targetShowError(std::string("SDL_Init failed: ") + SDL_GetError());
        targetFinish(0, true);
        return 1;
    }

    SdlPlatform platform;
    platform.rotate = opt.rotate;
    bool gles = false;
    SDL_GLContext glctx = nullptr;
    std::string gameName = gameNameOf(*dw);
    if (!opt.headless) {
        std::string err = openWindow(platform, *dw, opt, gles, glctx);
        if (!err.empty()) {
            SDL_Quit();
            targetShowError(err);
            targetFinish(0, true);
            return 1;
        }
    }

    HostInput input;
    // One game at a time; game_change replaces it (and its audio device) in the same window.
    std::unique_ptr<Runner> rt;
    SDL_AudioDeviceID audioDev = 0;
    auto startGame = [&](const std::vector<std::string>& params) {
        std::string saveDir = opt.saveDir.empty() ? targetSaveDir(gameName) : opt.saveDir;
        makeDirs(saveDir);
        rt = std::make_unique<Runner>(*dw, platform, opt.gameDir, saveDir);
        rt->parameters = params;
        rt->glES = gles;
        rt->debugTrace = opt.trace;
        rt->fixedClock = opt.fast;  // no frame limiter: game time instead of wall time, for repeatable runs
        if (opt.seed >= 0) {
            rt->fixedSeed = opt.seed;
            rt->rng.seed(static_cast<int32_t>(opt.seed));
        }
        if (opt.smoke) rt->vm.loopLimit = 20000000;  // also guards start-up code (global scripts)
        rt->audioOutput = !opt.recordAudio.empty();  // --record-audio mixes in the main loop
        rt->start(!opt.headless);
        rt->gfx->presentLinear = opt.linear;
        rt->gfx->presentIntegerScale = opt.integerScale;
        rt->gfx->presentRotation = opt.rotate;
        rt->gfx->screenOverlay = [&input](Renderer& r, int w, int h) {
            input.touchPad.draw(r, w, h);
            input.menu.draw(r, w, h);
        };
        if (!opt.mute && !opt.headless && opt.recordAudio.empty()) {
            SDL_AudioSpec want{}, have{};
            want.freq = Audio::kRate;
            want.format = AUDIO_F32SYS;
            want.channels = 2;
            want.samples = 1024;
            want.callback = audioCallback;
            want.userdata = rt->audio.get();
            audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
            if (audioDev) SDL_PauseAudioDevice(audioDev, 0);
            else fprintf(stderr, "[audio] %s\n", SDL_GetError());
            rt->audioOutput = audioDev != 0;
        }
    };
    std::vector<std::string> params = {argv[0]};
    for (size_t i = 1; i < positional.size(); i++) params.push_back(positional[i]);  // the game's own arguments
    startGame(params);
    if (opt.smoke) {
        int rc = runSmoke(*rt, *dw, opt);
        rt.reset();
        SDL_Quit();
        return rc;
    }

    input.setGame(*dw, opt);
    input.openExisting(*rt);
    DevTools dev(opt);

    const double frameStart = platform.now();
    double nextFrame = frameStart;
    double fpsTimer = frameStart;
    int fpsFrames = 0;
    long frame = 0;
    // game_change(dir, params): the game in `dir` (relative to this game's folder) replaces this
    // one; "-game <file>" in the parameters names its data file (a multi-chapter game's launcher).
    std::string exitError;  // shown once SDL is shut down (the Switch console can't share the screen)
    auto changeGame = [&]() -> bool {
        std::string dir = rt->gameChangeDir, paramStr = rt->gameChangeParams;
        if (audioDev) {
            SDL_CloseAudioDevice(audioDev);
            audioDev = 0;
        }
        rt.reset();
        std::string to = normalizePath(opt.gameDir + "/" + dir);
        std::vector<std::string> p = {argv[0]};
        std::stringstream ss(paramStr);
        for (std::string tok; ss >> tok;) p.push_back(tok);
        std::string path;
        for (size_t i = 1; i + 1 < p.size(); i++)
            if (p[i] == "-game") path = !p[i + 1].empty() && p[i + 1][0] == '/' ? p[i + 1] : to + "/" + p[i + 1];
        if (path.empty() || !isFile(path)) path = findDataFile(to);
        // A missing target (say a forwarder that only carries one chapter) says so instead of
        // quitting silently.
        if (path.empty()) {
            exitError = "game_change: no data.win in " + to;
            return false;
        }
        try {
            dw = DataWin::load(path);
        } catch (const std::exception& e) {
            exitError = "game_change: cannot load " + path + ": " + e.what();
            return false;
        }
        opt.gameDir = parentDir(path);
        gameName = gameNameOf(*dw);
        fprintf(stderr, "[runner] game_change -> %s\n", path.c_str());
        input.touchPad.painter.reset();  // its texture belonged to the old game's renderer
        startGame(p);
        input.setGame(*dw, opt);
        input.touchPad.visible = false;
        input.announcePads(*rt);
        targetGameChanged();
        if (!dw->gen.displayName.empty()) platform.setWindowCaption(std::string(dw->gen.displayName));
        return true;
    };

    fprintf(stderr, "[load] ready to run after %.2fs\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - mainStart).count());
    bool quit = false;
    while (!quit) {
        if (!targetFrame()) break;
        SDL_Event e;
        if (SDL_AtomicGet(&gBackground)) {  // in the background: no drawing, no sound, just wait
            if (audioDev) SDL_PauseAudioDevice(audioDev, 1);
            input.touchPad.releaseAll(*rt);
            while (SDL_AtomicGet(&gBackground) && !quit)
                if (SDL_WaitEventTimeout(&e, 200)) input.handleEvent(e, *rt, platform, opt, quit);
            if (audioDev) SDL_PauseAudioDevice(audioDev, 0);
            nextFrame = platform.now();  // don't race to catch up on the time away
            continue;
        }
        if (input.menu.open) {  // the in-game menu: the game waits, silent, under it
            if (audioDev) SDL_PauseAudioDevice(audioDev, 1);
            while (SDL_PollEvent(&e)) input.handleEvent(e, *rt, platform, opt, quit);
            targetPollInput(*rt, input.buttons, opt, quit);
            input.menu.poll();
            input.menu.tick(*rt, platform.now());
            if (!input.menu.open && audioDev) SDL_PauseAudioDevice(audioDev, 0);
            if (platform.window) {
                int w, h;
                platform.drawableSize(w, h);
                rt->present(w, h);
                SDL_GL_SwapWindow(platform.window);
            }
            SDL_Delay(16);
            nextFrame = platform.now();
            continue;
        }
        if (input.menu.exitRequested) break;
        while (SDL_PollEvent(&e)) input.handleEvent(e, *rt, platform, opt, quit);
        targetPollInput(*rt, input.buttons, opt, quit);
        if (input.menu.open || input.menu.exitRequested) continue;
        input.update(*rt, platform, opt);
        dev.beforeStep(*rt, input.buttons, *dw, opt, frame);

        double tStep0 = platform.now();
        if (!rt->step()) {
            if (!rt->gameChangeRequested || !changeGame()) break;
            continue;
        }
        double tStep = platform.now() - tStep0;
        frame++;
        if (opt.stats && frame % 60 == 0)
            fprintf(stderr, "[stats] frame %ld room %s: step %.2f ms, %d draw calls, %zu instances\n", frame,
                    std::string(dw->rooms[rt->room].name).c_str(), tStep * 1000.0, rt->gfx->renderer().drawCalls(),
                    rt->instances.size());
        dev.afterStep(*rt, platform, opt, frame);

        if (platform.window) {
            int w, h;
            platform.drawableSize(w, h);
            rt->present(w, h);
            SDL_GL_SwapWindow(platform.window);
        }

        double t = platform.now();
        fpsFrames++;
        if (t - fpsTimer >= 1.0) {
            rt->fps = std::min(rt->roomSpeed, fpsFrames / (t - fpsTimer));
            rt->fpsReal = fpsFrames / (t - fpsTimer);
            // --fast runs on game time: report the game speed so runs repeat exactly
            if (opt.fast) rt->fps = rt->fpsReal = rt->roomSpeed;
            if (opt.showFps) fprintf(stderr, "[fps] %.1f, %zu instances\n", rt->fpsReal, rt->instances.size());
            fpsTimer = t;
            fpsFrames = 0;
        }
        if (!opt.fast && !opt.headless) {
            double period = 1.0 / (rt->roomSpeed > 0 ? rt->roomSpeed : 60);
            nextFrame += period;
            double wait = nextFrame - platform.now();
            if (wait > 0.002) SDL_Delay(static_cast<Uint32>((wait - 0.001) * 1000));
            while (platform.now() < nextFrame) {
            }
            if (platform.now() - nextFrame > 0.25) nextFrame = platform.now();  // fell far behind: resync
        }
        if (opt.frames >= 0 && frame >= opt.frames) break;
    }

    dev.finish(opt);
    if (audioDev) SDL_CloseAudioDevice(audioDev);
    rt.reset();
    input.closeAll();
    if (glctx) SDL_GL_DeleteContext(glctx);
    if (platform.window) SDL_DestroyWindow(platform.window);
    if (!tgt.returnToPicker) SDL_Quit();  // (iOS: the app goes on with the launcher)
    if (!exitError.empty()) targetShowError(exitError);
    targetFinish(frame, !exitError.empty());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    targetStart(argc, argv);
    SDL_AddEventWatch(watchApp, nullptr);
    for (;;) {
        int rc = runOnce(argc, argv);
        // iOS apps don't quit: when a game ends, its launcher comes back.
        if (!target().returnToPicker || rc == kClosed) return rc == kClosed ? 0 : rc;
        argc = 1;  // the next game comes from the launcher
    }
}
