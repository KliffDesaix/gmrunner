// Developer tools: scripted input (--input, --press), debug commands (--goto, --create, --set,
// --count), screenshots and contact sheets, offline audio recording, and --smoke.
#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>

#include "data/DataWin.h"
#include "platform/sdl/Host.h"
#include "render/Renderer.h"
#include "runtime/Audio.h"
#include "runtime/Graphics.h"
#include "runtime/Runner.h"
#include "runtime/Sequences.h"
#include "util/Png.h"

namespace gm::host {

namespace {

int32_t objectNamed(const DataWin& dw, const std::string& name) {
    for (size_t o = 0; o < dw.objects.size(); o++)
        if (dw.objects[o].name == name) return static_cast<int32_t>(o);
    return -1;
}

// --create, --set and --count (reaching a state without playing to it).
void debugCommands(Runner& rt, const DataWin& dw, const RunOptions& opt, long frame) {
    for (auto& [f, spec] : opt.debugCreate) {
        if (f != frame) continue;
        std::stringstream ss(spec);
        std::string name, sx, sy;
        std::getline(ss, name, ':');
        std::getline(ss, sx, ':');
        std::getline(ss, sy, ':');
        int32_t o = objectNamed(dw, name);
        if (o >= 0) {
            rt.createInstance(atof(sx.c_str()), atof(sy.c_str()), o);
            continue;
        }
        // a sequence: played on the top layer
        int32_t seq = -1;
        for (size_t i = 0; i < dw.sequences.size() && seq < 0; i++)
            if (dw.sequences[i].name == name) seq = static_cast<int32_t>(i);
        Layer* top = nullptr;
        for (Layer& L : rt.layers)
            if (!top || L.depth < top->depth) top = &L;
        if (seq < 0 || !top || !rt.sequences) fprintf(stderr, "--create: no object or sequence %s\n", name.c_str());
        else rt.sequences->createElement(*top, seq, atof(sx.c_str()), atof(sy.c_str()));
    }
    for (auto& [f, spec] : opt.debugSet) {
        if (f != frame) continue;
        size_t dot = spec.find('.'), eq = spec.find('=');
        if (dot == std::string::npos || eq == std::string::npos || eq < dot) continue;
        std::string owner = spec.substr(0, dot), var = spec.substr(dot + 1, eq - dot - 1);
        Value v(atof(spec.c_str() + eq + 1));
        int32_t id = rt.vm.varIdForName(var);
        if (owner == "global") {
            rt.vm.globalVar(id) = v;
            continue;
        }
        int32_t o = objectNamed(dw, owner);
        if (o < 0) fprintf(stderr, "--set: no object %s\n", owner.c_str());
        else rt.forEachInstanceOf(o, [&](Instance* i) { rt.vm.memberSet(i, id, v); });
    }
    for (auto& [f, spec] : opt.debugCount) {
        if (f != frame) continue;
        std::string out = "[count] frame " + std::to_string(frame) + ":";
        if (spec == "all") {
            std::map<int32_t, int> n;
            for (Instance* i : rt.instances)
                if (!i->destroyed) n[i->object]++;
            for (auto& [o, c] : n) out += " " + std::string(dw.objects[o].name) + "=" + std::to_string(c);
        } else {
            std::stringstream ss(spec);
            std::string name;
            while (std::getline(ss, name, ',')) {
                int32_t o = objectNamed(dw, name);
                out += " " + name + "=" + std::to_string(o < 0 ? -1 : rt.instanceCountOf(o));
            }
        }
        printf("%s\n", out.c_str());
    }
}

void writeWav(const std::string& path, const std::vector<int16_t>& samples) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2), v32;
    uint16_t v16;
    fwrite("RIFF", 1, 4, f);
    v32 = 36 + dataBytes; fwrite(&v32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v32 = 16; fwrite(&v32, 4, 1, f);
    v16 = 1; fwrite(&v16, 2, 1, f);
    v16 = 2; fwrite(&v16, 2, 1, f);
    v32 = Audio::kRate; fwrite(&v32, 4, 1, f);
    v32 = Audio::kRate * 4; fwrite(&v32, 4, 1, f);
    v16 = 4; fwrite(&v16, 2, 1, f);
    v16 = 16; fwrite(&v16, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
    fwrite(samples.data(), 2, samples.size(), f);
    fclose(f);
    fprintf(stderr, "[audio] recorded %.1fs -> %s\n", samples.size() / 2.0 / Audio::kRate, path.c_str());
}

}  // namespace

DevTools::DevTools(RunOptions& opt) {
    std::sort(opt.script.begin(), opt.script.end(), [](auto& a, auto& b) { return std::get<0>(a) < std::get<0>(b); });
}

void DevTools::beforeStep(Runner& rt, Buttons& buttons, const DataWin& dw, const RunOptions& opt, long frame) {
    while (scriptPos < opt.script.size() && std::get<0>(opt.script[scriptPos]) <= frame) {
        int vk = std::get<1>(opt.script[scriptPos]);
        bool down = std::get<2>(opt.script[scriptPos]);
        if (vk >= kScriptPad) {
            rt.input.pads[0].connected = true;
            buttons.press(rt, 0, vk - kScriptPad, down);
        } else if (vk >= kScriptMouse) {
            rt.input.mouseButton(vk - kScriptMouse, down);
        } else {
            rt.input.keyEvent(vk, down);
        }
        scriptPos++;
    }
    while (stickScriptPos < opt.stickScript.size() && std::get<0>(opt.stickScript[stickScriptPos]) <= frame) {
        auto& [f, right, sx, sy] = opt.stickScript[stickScriptPos++];
        rt.input.pads[0].axis[right ? GP_AXISRH : GP_AXISLH] = static_cast<float>(sx);
        rt.input.pads[0].axis[right ? GP_AXISRV : GP_AXISLV] = static_cast<float>(sy);
    }
    while (mouseScriptPos < opt.mouseScript.size() && std::get<0>(opt.mouseScript[mouseScriptPos]) <= frame) {
        rt.input.mouseX = std::get<1>(opt.mouseScript[mouseScriptPos]);
        rt.input.mouseY = std::get<2>(opt.mouseScript[mouseScriptPos]);
        mouseScriptPos++;
    }
    for (auto& [f, name] : opt.gotoRooms)
        if (f == frame)
            for (size_t r = 0; r < dw.rooms.size(); r++)
                if (dw.rooms[r].name == name) rt.gotoRoom(static_cast<int32_t>(r));
    debugCommands(rt, dw, opt, frame);
}

void DevTools::afterStep(Runner& rt, SdlPlatform& platform, const RunOptions& opt, long frame) {
    if (!opt.recordAudio.empty()) {
        recordCarry += Audio::kRate / (rt.roomSpeed > 0 ? rt.roomSpeed : 60);
        int n = static_cast<int>(recordCarry);
        recordCarry -= n;
        std::vector<float> buf(static_cast<size_t>(n) * 2);
        rt.audio->mix(buf.data(), n);
        for (float v : buf) recorded.push_back(static_cast<int16_t>(std::max(-1.f, std::min(1.f, v)) * 32767));
    }
    for (auto& [f, name] : opt.screenshots) {
        if (f != frame) continue;
        std::vector<uint8_t> px;
        int w, h;
        bool ok;
        if (opt.screenCapture && platform.window) {
            platform.drawableSize(w, h);
            rt.present(w, h);
            px.resize(static_cast<size_t>(w) * h * 4);
            rt.gfx->renderer().readPixels(0, 0, w, h, px.data());
            ok = true;
        } else {
            ok = rt.gfx->readBackbuffer(px, w, h);
        }
        if (!ok) continue;
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;  // the window is opaque, whatever alpha was drawn
        if (opt.sheet.empty()) {
            std::string path = opt.screenshotDir + "/" + name;
            writePng(path, px.data(), w, h);
            fprintf(stderr, "[shot] frame %ld -> %s (%dx%d)\n", frame, path.c_str(), w, h);
        } else {
            shots.push_back({std::move(px), w, h});
        }
    }
}

void DevTools::finish(const RunOptions& opt) {
    if (!opt.sheet.empty() && !shots.empty()) {
        int cols = std::min<int>(4, static_cast<int>(shots.size()));
        int rows = (static_cast<int>(shots.size()) + cols - 1) / cols;
        int cw = 0, ch = 0;
        for (auto& sh : shots) {
            cw = std::max(cw, sh.w);
            ch = std::max(ch, sh.h);
        }
        int gap = 4, W = cols * cw + (cols - 1) * gap, H = rows * ch + (rows - 1) * gap;
        std::vector<uint8_t> img(static_cast<size_t>(W) * H * 4, 0);
        for (size_t i = 0; i < img.size(); i += 4) img[i + 1] = 80, img[i + 3] = 255;  // dark green gaps
        for (size_t n = 0; n < shots.size(); n++) {
            int ox = static_cast<int>(n % cols) * (cw + gap), oy = static_cast<int>(n / cols) * (ch + gap);
            for (int y = 0; y < shots[n].h; y++)
                for (int x = 0; x < shots[n].w; x++) {
                    const uint8_t* src = &shots[n].px[(static_cast<size_t>(y) * shots[n].w + x) * 4];
                    uint8_t* dst = &img[(static_cast<size_t>(oy + y) * W + ox + x) * 4];
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                }
        }
        writePng(opt.sheet, img.data(), W, H);
        fprintf(stderr, "[sheet] %zu frames -> %s\n", shots.size(), opt.sheet.c_str());
    }
    if (!opt.recordAudio.empty()) writeWav(opt.recordAudio, recorded);
}

// --smoke: boots the game, then visits every room for a while with simple key presses and
// prints the errors, unimplemented engine functions and warnings it ran into, grouped.
// Runs headless with a throwaway save folder and a fixed seed.
int runSmoke(Runner& rt, const DataWin& dw, const RunOptions& opt) {
    using Kind = Runner::DiagKind;
    rt.collectDiagnostics = true;
    static const int kKeys[] = {VK_ENTER, 'Z', VK_SPACE, 'X', VK_DOWN, VK_UP, VK_RIGHT, VK_LEFT, 'C'};
    long frame = 0, gameEnds = 0, crashes = 0;
    int keyIndex = 0, heldKey = -1;
    auto runFrames = [&](long n) {
        for (long i = 0; i < n; i++, frame++) {
            if (frame % 12 == 0) {
                heldKey = kKeys[keyIndex++ % (sizeof kKeys / sizeof kKeys[0])];
                rt.input.keyEvent(heldKey, true);
            } else if (frame % 12 == 3 && heldKey >= 0) {
                rt.input.keyEvent(heldKey, false);
                heldKey = -1;
            }
            try {
                if (!rt.step()) {  // game_end: note it and keep going
                    gameEnds++;
                    rt.gameEnded = false;
                }
            } catch (const std::exception& e) {
                crashes++;
                rt.report(Kind::Error, std::string("runner crash: ") + e.what());
            }
        }
    };
    double t0 = SDL_GetTicks() / 1000.0;
    rt.diagPhase = "boot";
    runFrames(opt.smokeBoot);
    rt.diagPhase = "rooms";
    size_t roomsWithErrors = 0;
    // --smoke-shots: each room's last frame, plus 4x4 sheets of half-size thumbnails and an index.
    std::vector<uint8_t> sheet;
    int sheetW = 0, sheetH = 0, sheetCells = 0, sheetCount = 0, cellW = 320, cellH = 240;
    std::string index;
    auto flushSheet = [&] {
        if (!sheetCells) return;
        char name[32];
        snprintf(name, sizeof name, "/sheet_%02d.png", sheetCount++);
        writePng(opt.smokeShots + name, sheet.data(), sheetW, sheetH);
        sheetCells = 0;
    };
    auto capture = [&](size_t r) {
        std::vector<uint8_t> px;
        int w, h;
        if (opt.smokeShots.empty() || !rt.gfx || !rt.gfx->readBackbuffer(px, w, h) || w <= 0 || h <= 0) return;
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        // Name the room actually showing; the tour's key presses can walk out of the one entered.
        std::string room(dw.rooms[r].name);
        if (rt.room >= 0 && static_cast<size_t>(rt.room) < dw.rooms.size() && static_cast<size_t>(rt.room) != r)
            room += "-now-" + std::string(dw.rooms[rt.room].name);
        char num[24];
        snprintf(num, sizeof num, "/%03zu_", r);
        writePng(opt.smokeShots + num + room + ".png", px.data(), w, h);
        if (sheetCells == 0) {
            sheetW = 4 * cellW + 3 * 4;
            sheetH = 4 * cellH + 3 * 4;
            sheet.assign(static_cast<size_t>(sheetW) * sheetH * 4, 0);
            for (size_t i = 0; i < sheet.size(); i += 4) sheet[i + 1] = 80, sheet[i + 3] = 255;
        }
        int ox = (sheetCells % 4) * (cellW + 4), oy = (sheetCells / 4) * (cellH + 4);
        for (int y = 0; y < cellH; y++)  // nearest-neighbour fit into the cell
            for (int x = 0; x < cellW; x++) {
                const uint8_t* src = &px[(static_cast<size_t>(y * h / cellH) * w + x * w / cellW) * 4];
                std::memcpy(&sheet[(static_cast<size_t>(oy + y) * sheetW + ox + x) * 4], src, 4);
            }
        index += "sheet_" + std::to_string(sheetCount) + " #" + std::to_string(sheetCells) + ": " + room + "\n";
        if (++sheetCells == 16) flushSheet();
    };
    for (size_t r = 0; r < dw.rooms.size(); r++) {
        long before = 0;
        for (auto& [m, e] : rt.diagnostics)
            if (e.kind == Kind::Error || e.kind == Kind::Compile) before += e.count;
        rt.gotoRoom(static_cast<int32_t>(r));
        runFrames(opt.smokeFrames);
        capture(r);
        long after = 0;
        for (auto& [m, e] : rt.diagnostics)
            if (e.kind == Kind::Error || e.kind == Kind::Compile) after += e.count;
        if (after > before) roomsWithErrors++;
    }
    flushSheet();
    if (!opt.smokeShots.empty())
        if (FILE* f = fopen((opt.smokeShots + "/index.txt").c_str(), "w")) {
            fputs(index.c_str(), f);
            fclose(f);
        }
    double secs = SDL_GetTicks() / 1000.0 - t0;

    auto list = [&](Kind kind, const char* title, bool showRooms) {
        std::vector<std::pair<std::string, const Runner::DiagEntry*>> v;
        for (auto& [m, e] : rt.diagnostics)
            if (e.kind == kind || (kind == Kind::Error && e.kind == Kind::Compile)) v.push_back({m, &e});
        std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second->count > b.second->count; });
        printf("\n%s (%zu)\n", title, v.size());
        for (auto& [m, e] : v) {
            std::string rooms;
            for (size_t i = 0; i < e->rooms.size() && i < 4; i++) rooms += (i ? ", " : "") + e->rooms[i];
            if (e->rooms.size() > 4) rooms += ", +" + std::to_string(e->rooms.size() - 4) + " more";
            printf("  %5ldx  %s\n", e->count, m.c_str());
            if (showRooms) printf("          first during %s; rooms: %s\n", e->phase.c_str(), rooms.c_str());
        }
    };
    size_t errors = 0, unimplemented = 0;
    for (auto& [m, e] : rt.diagnostics) {
        if (e.kind == Kind::Error || e.kind == Kind::Compile) errors++;
        if (e.kind == Kind::Unimplemented) unimplemented++;
    }
    printf("\n==== smoke test: %s\n", std::string(dw.gen.displayName.empty() ? dw.gen.name : dw.gen.displayName).c_str());
    printf("GameMaker:  %s\n", dw.engineVersion().c_str());
    printf("Run:        %ld boot steps, then %zu rooms x %ld steps (%ld steps, %.1f s)\n", opt.smokeBoot,
           dw.rooms.size(), opt.smokeFrames, frame, secs);
    printf("Result:     %zu distinct errors, %zu rooms with errors, %ld runner crashes, %zu unimplemented "
           "functions, game_end called %ld times\n",
           errors, roomsWithErrors, crashes, unimplemented, gameEnds);
    list(Kind::Error, "Errors (event aborted)", true);
    list(Kind::Unimplemented, "Unimplemented engine functions (called, did nothing)", true);
    list(Kind::Warning, "Warnings", false);
    printf("\nNote: rooms are entered directly, skipping the game's own setup for them, so some errors\n"
           "seen only during \"rooms\" can be artefacts of that. Errors during \"boot\" happen in normal play.\n");
    return errors ? 2 : 0;
}

}  // namespace gm::host
