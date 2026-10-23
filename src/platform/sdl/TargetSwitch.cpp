// Nintendo Switch homebrew target (devkitPro libnx + SDL2): a log file on the SD card, games from
// the NRO's romfs or sdmc:/switch/gmrunner/games, a text-console game picker and error screen,
// and controllers read through libnx (single Joy-Cons held sideways included).
#ifdef __SWITCH__

#include <switch.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>

#include "platform/sdl/Host.h"
#include "platform/sdl/Target.h"
#include "runtime/Runner.h"
#include "util/Fs.h"

#ifdef GMR_SWITCH_LAUNCHER
extern "C" int gmr_switch_launcher(const char* root, char* out, int outSize);  // switch/launcher
#endif

namespace gm::host {

const TargetInfo& target() {
    static const TargetInfo info = {
        "switch",  // osName
        true,      // fixedScreen
        1280, 720, // screen size (SDL scales it to the TV)
        true,      // gles
        false,     // sdlControllers: read through libnx below
        2,         // mouseStick: right stick
        false,     // devTools
        false,  // picker
        false,  // touchControls
        false,  // orientFromGame
        false,  // returnToPicker
    };
    return info;
}

namespace {

Result romfsRc = -1;
std::string gSelf;           // this NRO, to come back to the game list after a game
bool gLauncherUsed = false;  // the game came from the game list

// Controllers through libnx: players 1-4, player 1 also handheld mode.
struct SwitchPads {
    bool ready = false;
    PadState pad[Input::kPads];
    u64 prev[Input::kPads] = {};
    bool connected[Input::kPads] = {};
} swPads;

void initPads() {
    padConfigureInput(Input::kPads, HidNpadStyleSet_NpadStandard);
    // A single Joy-Con is held sideways (the system then turns its stick to match).
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
    padInitialize(&swPads.pad[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
    for (int i = 1; i < Input::kPads; i++) padInitialize(&swPads.pad[i], static_cast<HidNpadIdType>(HidNpadIdType_No1 + i));
    swPads.ready = true;
}

}  // namespace

void targetStart(int argc, char** argv) {
    romfsRc = romfsInit();
    if (argc > 0 && argv[0]) gSelf = argv[0];
    // Nothing reads stderr on the console: keep it in a log next to the runner (overwritten every
    // launch) so problems on hardware can be looked at afterwards.
    if (freopen("sdmc:/switch/gmrunner/gmrunner.log", "w", stderr)) setvbuf(stderr, nullptr, _IOLBF, 0);
    std::string args;
    for (int i = 0; i < argc; i++) args += std::string(i ? " " : "") + "[" + argv[i] + "]";
    AppletType at = appletGetAppletType();
    fprintf(stderr, "[switch] gmrunner started, argv: %s, applet type %d (%s)\n", args.c_str(), static_cast<int>(at),
            at == AppletType_Application || at == AppletType_SystemApplication ? "application, full memory"
                                                                                : "applet, limited memory");
    fprintf(stderr, "[switch] romfs: %s\n", R_SUCCEEDED(romfsRc) ? "mounted" : "none (plain gmrunner.nro)");
}

void targetFinish(long frame, bool withError) {
    fprintf(stderr, "[switch] exiting (frame %ld)%s\n", frame, withError ? ", with an error" : "");
    // A game started from the game list returns to it: the Homebrew loader starts this NRO again.
    if (gLauncherUsed && !gSelf.empty() && envHasNextLoad()) {
        envSetNextLoad(gSelf.c_str(), gSelf.c_str());
        fprintf(stderr, "[switch] back to the game list next\n");
    }
    if (R_SUCCEEDED(romfsRc)) romfsExit();
}

// Picks a game: argv path (hbmenu file association), romfs:/game.txt, or a folder in
// sdmc:/switch/gmrunner/games that contains a data.win (a picker when there are several).
std::string targetFindGame(int argc, char** argv, const std::vector<std::string>&) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        // Loaders and forwarders may pass the NRO's own path along; only a game counts here.
        if (a.size() >= 4 && strcasecmp(a.c_str() + a.size() - 4, ".nro") == 0) {
            fprintf(stderr, "[switch] argument %d is an NRO, ignored: %s\n", i, a.c_str());
            continue;
        }
        if (isFile(a)) {
            fprintf(stderr, "[switch] game from argument %d (file): %s\n", i, a.c_str());
            return parentDir(a);
        }
        if (isDir(a) && !findDataFile(a).empty()) {
            fprintf(stderr, "[switch] game from argument %d (folder): %s\n", i, a.c_str());
            return a;
        }
        fprintf(stderr, "[switch] argument %d is not a game, ignored: %s\n", i, a.c_str());
    }
    // Per-game NROs (tools/make-game-nro.sh, used for home-menu forwarders) name their game
    // folder in romfs:/game.txt: a folder under sdmc:/switch/gmrunner/games/ or a full path.
    if (FILE* f = fopen("romfs:/game.txt", "rb")) {
        char buf[512] = {};
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        std::string name(buf, n);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.pop_back();
        if (!name.empty()) {
            fprintf(stderr, "[switch] game named by romfs:/game.txt: %s\n", name.c_str());
            return name.find(':') != std::string::npos ? name : "sdmc:/switch/gmrunner/games/" + name;
        }
    }
    std::vector<std::string> found;
    if (DIR* d = opendir("sdmc:/switch/gmrunner/games")) {
        while (dirent* e = readdir(d)) {
            std::string path = std::string("sdmc:/switch/gmrunner/games/") + e->d_name;
            if (e->d_name[0] != '.' && isDir(path) && !findDataFile(path).empty()) found.push_back(path);
        }
        closedir(d);
    }
    std::sort(found.begin(), found.end());
    fprintf(stderr, "[switch] %zu games in sdmc:/switch/gmrunner/games\n", found.size());
#ifdef GMR_SWITCH_LAUNCHER
    // The game list in the console's look (switch/launcher, Borealis), even for one game: it is
    // where games are renamed and given icons, and where the player comes back to.
    {
        makeDirs("sdmc:/switch/gmrunner/games");
        char buf[1024] = {};
        fprintf(stderr, "[switch] opening the game list\n");
        int r = gmr_switch_launcher("sdmc:/switch/gmrunner/games", buf, sizeof buf);
        if (r == 1) {
            fprintf(stderr, "[switch] game list: chose %s\n", buf);
            gLauncherUsed = true;
            return buf;
        }
        if (r == 0) {  // + pressed: back to the Homebrew Menu
            fprintf(stderr, "[switch] game list closed\n");
            if (R_SUCCEEDED(romfsRc)) romfsExit();
            exit(0);
        }
        fprintf(stderr, "[switch] the game list could not start; using the text list\n");
    }
#endif
    if (found.size() <= 1) return found.empty() ? std::string() : found[0];

    // Several games: let the player pick one on a text console before SDL starts. The console
    // needs the screen; if it can't get it, start the first game rather than crash.
    fprintf(stderr, "[switch] opening the game picker\n");
    if (!consoleInit(nullptr)) {
        fprintf(stderr, "[switch] text console unavailable; starting %s\n", found[0].c_str());
        return found[0];
    }
    fprintf(stderr, "[switch] game picker open\n");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    size_t sel = 0;
    std::string chosen;
    bool redraw = true;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) { sel = (sel + 1) % found.size(); redraw = true; }
        if (down & (HidNpadButton_Up | HidNpadButton_StickLUp)) { sel = (sel + found.size() - 1) % found.size(); redraw = true; }
        if (down & HidNpadButton_A) { chosen = found[sel]; break; }
        if (down & HidNpadButton_Plus) {
            fprintf(stderr, "[switch] picker: + pressed, quitting\n");
            break;
        }
        if (redraw) {
            printf("\x1b[2J\x1b[1;1HgmRunner\n\nChoose a game (A = start, + = quit):\n\n");
            for (size_t i = 0; i < found.size(); i++)
                printf("%s %s\n", i == sel ? " >" : "  ", found[i].substr(found[i].find_last_of('/') + 1).c_str());
            redraw = false;
        }
        consoleUpdate(nullptr);
    }
    if (chosen.empty()) fprintf(stderr, "[switch] picker closed without a game (the system asked the app to exit?)\n");
    else fprintf(stderr, "[switch] picker: chose %s\n", chosen.c_str());
    consoleExit(nullptr);
    return chosen;
}

std::string targetNoGameMessage() {
    return "No game found.\nCopy a GameMaker game folder (with data.win) to\nsdmc:/switch/gmrunner/games/<name>/";
}

std::vector<std::string> targetGameList() { return {}; }
std::vector<std::string> targetPickerHelp() { return {}; }
bool targetNativePick(std::string&) { return false; }

std::string targetSaveDir(const std::string& gameName) { return "sdmc:/switch/gmrunner/saves/" + gameName; }

void targetShowError(const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
    if (!consoleInit(nullptr)) {
        fprintf(stderr, "[switch] text console unavailable for the error screen\n");
        return;
    }
    printf("gmrunner error:\n\n%s\n\nPress + to exit.\n", msg.c_str());
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}

bool targetFrame() {
    if (appletMainLoop()) return true;
    fprintf(stderr, "[switch] the system asked the app to exit\n");
    return false;
}

void targetPollInput(Runner& rt, Buttons& buttons, const RunOptions& opt, bool& quit) {
    if (!swPads.ready) initPads();
    static const std::pair<u64, int> kButtons[] = {
        {HidNpadButton_A, GP_FACE1}, {HidNpadButton_B, GP_FACE2}, {HidNpadButton_X, GP_FACE3},
        {HidNpadButton_Y, GP_FACE4}, {HidNpadButton_L, GP_SHOULDERL}, {HidNpadButton_R, GP_SHOULDERR},
        {HidNpadButton_ZL, GP_SHOULDERLB}, {HidNpadButton_ZR, GP_SHOULDERRB}, {HidNpadButton_Plus, GP_START},
        {HidNpadButton_Minus, GP_SELECT}, {HidNpadButton_StickL, GP_STICKL}, {HidNpadButton_StickR, GP_STICKR},
        {HidNpadButton_Left, GP_PADL}, {HidNpadButton_Up, GP_PADU}, {HidNpadButton_Right, GP_PADR},
        {HidNpadButton_Down, GP_PADD}};
    // Xbox layout: map by position (the bottom face button is gp_face1).
    static const std::pair<u64, int> kXbox[] = {{HidNpadButton_B, GP_FACE1}, {HidNpadButton_A, GP_FACE2},
                                                {HidNpadButton_Y, GP_FACE3}, {HidNpadButton_X, GP_FACE4}};
    for (int i = 0; i < Input::kPads; i++) {
        PadState& ps = swPads.pad[i];
        padUpdate(&ps);
        bool conn = padIsConnected(&ps);
        auto& p = rt.input.pads[i];
        if (conn != swPads.connected[i]) {
            swPads.connected[i] = conn;
            if (conn) {
                p.connected = true;
                u32 style = padGetStyleSet(&ps);
                p.name = style & HidNpadStyleTag_NpadHandheld ? "Nintendo Switch Handheld"
                         : style & HidNpadStyleTag_NpadJoyLeft ? "Joy-Con (L)"
                         : style & HidNpadStyleTag_NpadJoyRight ? "Joy-Con (R)"
                                                                : "Nintendo Switch Controller";
                fprintf(stderr, "[input] controller %d connected: %s\n", i, p.name.c_str());
            } else {
                for (int b = 0; b < GP_BUTTON_COUNT; b++) buttons.press(rt, i, b, false);
                p = Input::Pad();
                swPads.prev[i] = 0;
                fprintf(stderr, "[input] controller %d disconnected\n", i);
            }
        }
        if (!conn) continue;
        u64 now = padGetButtons(&ps);
        // A single Joy-Con held sideways reports its buttons by their printed names; turn them
        // into the positions they have sideways (right = A, bottom = B, top = X, left = Y; SL/SR
        // = L/R), as Switch games do, and use its stick as the left stick.
        u32 style = padGetStyleSet(&ps);
        bool singleL = (style & HidNpadStyleTag_NpadJoyLeft) && !(style & HidNpadStyleTag_NpadJoyDual);
        bool singleR = (style & HidNpadStyleTag_NpadJoyRight) && !(style & HidNpadStyleTag_NpadJoyDual);
        if (singleL || singleR) {
            static const std::pair<u64, u64> kLeft[] = {
                {HidNpadButton_Down, HidNpadButton_A}, {HidNpadButton_Left, HidNpadButton_B},
                {HidNpadButton_Right, HidNpadButton_X}, {HidNpadButton_Up, HidNpadButton_Y},
                {HidNpadButton_LeftSL, HidNpadButton_L}, {HidNpadButton_LeftSR, HidNpadButton_R},
                {HidNpadButton_Minus, HidNpadButton_Plus}, {HidNpadButton_StickL, HidNpadButton_StickL},
                {HidNpadButton_L, HidNpadButton_ZL}, {HidNpadButton_ZL, HidNpadButton_ZL}};
            static const std::pair<u64, u64> kRight[] = {
                {HidNpadButton_X, HidNpadButton_A}, {HidNpadButton_A, HidNpadButton_B},
                {HidNpadButton_Y, HidNpadButton_X}, {HidNpadButton_B, HidNpadButton_Y},
                {HidNpadButton_RightSL, HidNpadButton_L}, {HidNpadButton_RightSR, HidNpadButton_R},
                {HidNpadButton_Plus, HidNpadButton_Plus}, {HidNpadButton_StickR, HidNpadButton_StickL},
                {HidNpadButton_R, HidNpadButton_ZR}, {HidNpadButton_ZR, HidNpadButton_ZR}};
            u64 mapped = 0;
            for (auto [from, to] : singleL ? kLeft : kRight)
                if (now & from) mapped |= to;
            now = mapped;
        }
        if (now != swPads.prev[i]) {
            for (auto [bit, gp] : kButtons) {
                if (opt.xboxLayout && bit & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y)) continue;
                if ((now ^ swPads.prev[i]) & bit) buttons.press(rt, i, gp, now & bit);
            }
            if (opt.xboxLayout)
                for (auto [bit, gp] : kXbox)
                    if ((now ^ swPads.prev[i]) & bit) buttons.press(rt, i, gp, now & bit);
            swPads.prev[i] = now;
        }
        HidAnalogStickState l = padGetStickPos(&ps, 0), r = padGetStickPos(&ps, 1);
        if (singleR) l = r, r = {};  // the right Joy-Con's only stick moves
        else if (singleL) r = {};
        auto axis = [](s32 v) { return std::max(-1.f, std::min(1.f, v / 32767.f)); };
        p.axis[GP_AXISLH] = axis(l.x);
        p.axis[GP_AXISLV] = -axis(l.y);  // GameMaker's vertical axes point down
        p.axis[GP_AXISRH] = axis(r.x);
        p.axis[GP_AXISRV] = -axis(r.y);
    }
}

void targetWindowCreated(SDL_Window*) {}
void targetSafeArea(SDL_Window*, double inset[4]) { inset[0] = inset[1] = inset[2] = inset[3] = 0; }
bool targetShowMenu(const std::string&) { return false; }
int targetMenuResult() { return 0; }
void targetGameChanged() {
    for (int i = 0; i < Input::kPads; i++) swPads.connected[i] = false, swPads.prev[i] = 0;  // re-announced next poll
}

}  // namespace gm::host

#endif
