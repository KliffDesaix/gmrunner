// What each controller and on-screen button does in the game: the per-game map (defaults from
// the game's code, the player's choices from gmrunner.ini) and the router that applies it.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

#include "data/DataWin.h"
#include "platform/sdl/Host.h"
#include "runtime/Runner.h"
#include "vm/Bytecode.h"

namespace gm::host {

namespace {

const char* const kButtonNames[GP_BUTTON_COUNT] = {"a",      "b",     "x",  "y",  "l",  "r",    "l2",   "r2",
                                                   "select", "start", "l3", "r3", "up", "down", "left", "right"};
const char* const kPadNames[GP_BUTTON_COUNT] = {
    "gp_face1",  "gp_face2", "gp_face3",  "gp_face4",  "gp_shoulderl", "gp_shoulderr", "gp_shoulderlb", "gp_shoulderrb",
    "gp_select", "gp_start", "gp_stickl", "gp_stickr", "gp_padu",      "gp_padd",      "gp_padl",       "gp_padr"};

struct KeyName {
    int vk;
    const char* name;
};
const KeyName kKeyNames[] = {
    {VK_BACKSPACE, "vk_backspace"}, {VK_TAB, "vk_tab"},     {VK_ENTER, "vk_enter"},       {VK_SHIFT, "vk_shift"},
    {VK_CONTROL, "vk_control"},     {VK_ALT, "vk_alt"},     {VK_PAUSE, "vk_pause"},       {VK_ESCAPE, "vk_escape"},
    {VK_SPACE, "vk_space"},         {VK_PAGEUP, "vk_pageup"}, {VK_PAGEDOWN, "vk_pagedown"}, {VK_END, "vk_end"},
    {VK_HOME, "vk_home"},           {VK_LEFT, "vk_left"},   {VK_UP, "vk_up"},             {VK_RIGHT, "vk_right"},
    {VK_DOWN, "vk_down"},           {VK_INSERT, "vk_insert"}, {VK_DELETE, "vk_delete"},   {106, "vk_multiply"},
    {107, "vk_add"},                {109, "vk_subtract"},   {110, "vk_decimal"},          {111, "vk_divide"},
    {VK_LSHIFT, "vk_lshift"},       {VK_RSHIFT, "vk_rshift"}, {VK_LCONTROL, "vk_lcontrol"}, {VK_RCONTROL, "vk_rcontrol"},
    {VK_LALT, "vk_lalt"},           {VK_RALT, "vk_ralt"},
};
const KeyName kKeyAliases[] = {{VK_CONTROL, "vk_ctrl"}, {VK_ENTER, "vk_return"}, {VK_ESCAPE, "vk_esc"}};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The game's code, read for what it checks: keyboard_check* arguments that are constants (vk_*,
// ord("Z")), single-letter ord() literals (key bindings kept in variables), keyboard events.
struct GameScan {
    std::set<int> keys;
    bool complete = true;  // every key check had a constant argument
    bool keyboard = false, gamepad = false, anyKey = false;
};

int sameKey(int vk) {  // the side-specific keys count as the plain ones
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) return VK_SHIFT;
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) return VK_CONTROL;
    if (vk == VK_LALT || vk == VK_RALT) return VK_ALT;
    return vk;
}

// The constant the instruction before `i` pushes (conversions skipped): a number, or ord() of a
// one-character string. -1 if it isn't one.
int constArg(const DataWin& dw, const std::vector<Instruction>& code, size_t i) {
    auto prev = [&](size_t& j) {
        while (j > 0 && code[j - 1].op == Op::Conv) j--;
        return j > 0 ? &code[--j] : nullptr;
    };
    size_t j = i;
    const Instruction* in = prev(j);
    if (!in) return -1;
    if (in->op == Op::PushI) return in->imm.i32;
    if (in->op == Op::Push) {
        switch (in->type1) {
            case DataType::Int16: case DataType::Int32: return in->imm.i32;
            case DataType::Int64: return static_cast<int>(in->imm.i64);
            case DataType::Double: return in->imm.d == static_cast<int>(in->imm.d) ? static_cast<int>(in->imm.d) : -1;
            default: return -1;
        }
    }
    if (in->op == Op::Call && in->operand == 1 && in->ref >= 0 && dw.functions[in->ref].name == "ord") {
        const Instruction* s = prev(j);
        if (s && s->op == Op::Push && s->type1 == DataType::String && s->ref >= 0 &&
            static_cast<size_t>(s->ref) < dw.strings.size() && dw.strings[s->ref].size() == 1)
            return std::toupper(static_cast<unsigned char>(dw.strings[s->ref][0]));
    }
    return -1;
}

GameScan scanGame(const DataWin& dw) {
    GameScan g;
    auto addKey = [&](int vk) {
        if (vk == VK_ANYKEY) g.anyKey = true;
        else if (vk > VK_ANYKEY && vk < Input::kKeys) g.keys.insert(sameKey(vk));
    };
    for (auto& obj : dw.objects)
        for (int ev : {EV_KEYBOARD, EV_KEYPRESS, EV_KEYRELEASE})
            for (auto& h : obj.events[ev]) {
                g.keyboard = true;
                addKey(static_cast<int>(h.subtype));
            }
    for (auto& v : dw.variables)
        if (v.name == "keyboard_key" || v.name == "keyboard_lastkey" || v.name == "keyboard_string") g.keyboard = true;
    std::set<uint32_t> seen;  // GMS 2.3 functions share their blob with the entry that holds them
    for (auto& entry : dw.code) {
        if (entry.length == 0 || !seen.insert(entry.bytecode).second) continue;
        std::vector<Instruction> code;
        try {
            code = decodeEntry(dw, entry);
        } catch (...) {
            continue;
        }
        for (size_t i = 0; i < code.size(); i++) {
            const Instruction& in = code[i];
            if (in.op != Op::Call || in.ref < 0) continue;
            std::string_view n = dw.functions[in.ref].name;
            if (n == "ord") {
                int k = constArg(dw, code, i + 1);  // ord("W") with nothing after it is a binding
                if (k >= '0' && k <= 'Z' && std::isalnum(k)) addKey(k);
            } else if (n.rfind("keyboard_", 0) == 0) {
                g.keyboard = true;
                if (n == "keyboard_check" || n == "keyboard_check_pressed" || n == "keyboard_check_released" ||
                    n == "keyboard_check_direct" || n == "keyboard_set_map") {
                    int k = constArg(dw, code, i);
                    if (k >= 0) addKey(k);
                    else g.complete = false;
                }
            } else if (n.rfind("gamepad_", 0) == 0) {
                g.gamepad = true;
            }
        }
    }
    return g;
}

bool contains(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

}  // namespace

const char* buttonName(int gp) { return gp >= 0 && gp < GP_BUTTON_COUNT ? kButtonNames[gp] : "?"; }
const char* padTargetName(int gp) { return gp >= 0 && gp < GP_BUTTON_COUNT ? kPadNames[gp] : "?"; }

int buttonFromName(const std::string& name) {
    std::string n = lower(name);
    for (int b = 0; b < GP_BUTTON_COUNT; b++)
        if (n == kButtonNames[b]) return b;
    return -1;
}

std::string keyName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk < VK_F1 + 12) return "vk_f" + std::to_string(vk - VK_F1 + 1);
    if (vk >= 96 && vk <= 105) return "vk_numpad" + std::to_string(vk - 96);
    for (auto& k : kKeyNames)
        if (k.vk == vk) return k.name;
    return "key" + std::to_string(vk);
}

static int keyFromName(const std::string& text) {
    std::string n = lower(text);
    if (n.size() == 1 && std::isalnum(static_cast<unsigned char>(n[0])))
        return std::toupper(static_cast<unsigned char>(n[0]));
    if (n.rfind("vk_", 0) != 0) n = "vk_" + n;
    for (auto& k : kKeyNames)
        if (n == k.name) return k.vk;
    for (auto& k : kKeyAliases)
        if (n == k.name) return k.vk;
    if (n.size() > 4 && n.rfind("vk_f", 0) == 0 && std::isdigit(static_cast<unsigned char>(n[4]))) {
        int f = atoi(n.c_str() + 4);
        if (f >= 1 && f <= 12) return VK_F1 + f - 1;
    }
    if (n.rfind("vk_numpad", 0) == 0 && n.size() == 10 && std::isdigit(static_cast<unsigned char>(n[9])))
        return 96 + (n[9] - '0');
    if (n.rfind("vk_key", 0) == 0) {  // keyName's fallback for codes without a name
        int k = atoi(n.c_str() + 6);
        if (k > 1 && k < Input::kKeys) return k;
    }
    return -1;
}

bool parseTargets(const std::string& text, std::vector<int>& pad, std::vector<int>& keys) {
    pad.clear();
    keys.clear();
    std::string word;
    bool ok = true;
    auto take = [&]() {
        if (word.empty()) return;
        std::string w = lower(word);
        int gp = -1;
        for (int b = 0; b < GP_BUTTON_COUNT; b++)
            if (w == kPadNames[b]) gp = b;
        if (w == "none") {
        } else if (gp >= 0) {
            if (!contains(pad, gp)) pad.push_back(gp);
        } else if (int vk = keyFromName(word); vk > 0) {
            if (!contains(keys, vk)) keys.push_back(vk);
        } else {
            ok = false;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ' || c == ',' || c == '+' || c == '\t') take();
        else word += c;
    }
    take();
    return ok;
}

std::string targetsText(const std::vector<int>& pad, const std::vector<int>& keys) {
    std::string s;
    for (int gp : pad) s += (s.empty() ? "" : " ") + std::string(padTargetName(gp));
    for (int vk : keys) s += (s.empty() ? "" : " ") + keyName(vk);
    return s.empty() ? "none" : s;
}

ButtonMap makeButtonMap(const DataWin& dw, const RunOptions& opt) {
    ButtonMap m;
    GameScan g = scanGame(dw);
    m.gameKeys.assign(g.keys.begin(), g.keys.end());
    m.gameKeysComplete = g.complete;
    auto in = [&](int vk) { return g.keys.count(vk) > 0; };

    // Every button presses its own gamepad button; the keyboard mirror (gamepad_to_keyboard)
    // adds the keys GameMaker games most often read.
    for (int b = 0; b < GP_BUTTON_COUNT; b++) m.pad[b] = {b};
    const int arrows[4] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT}, wasd[4] = {'W', 'S', 'A', 'D'};
    const int dirs[4] = {GP_PADU, GP_PADD, GP_PADL, GP_PADR};
    if (opt.padToKeys) {
        for (int k = 0; k < 4; k++) m.keys[dirs[k]] = {arrows[k]};
        m.keys[GP_FACE1] = {'Z'};
        m.keys[GP_FACE2] = {'X'};
        m.keys[GP_FACE4] = {'C'};
        m.keys[GP_START] = {VK_ENTER};
        m.keys[GP_SELECT] = {VK_ESCAPE};

        // Fitted to the keys the game checks: WASD for the d-pad, and keys the defaults miss on
        // the buttons in reach of a thumb. With some checks hidden in variables the defaults
        // stay and what was found is only added.
        bool hasArrows = in(VK_UP) || in(VK_DOWN) || in(VK_LEFT) || in(VK_RIGHT);
        bool hasWasd = (in('W') && in('S')) || (in('A') && in('D'));
        std::set<int> dirKeys(arrows, arrows + 4);
        if (hasWasd) {  // with the arrows when the game checks both (or might)
            for (int k = 0; k < 4; k++) {
                if (!hasArrows && g.complete) m.keys[dirs[k]].clear();
                m.keys[dirs[k]].push_back(wasd[k]);
            }
            dirKeys.insert(wasd, wasd + 4);
        }
        static const int kPreferred[] = {'Z', 'X', 'C', VK_SPACE, VK_SHIFT, VK_CONTROL, 'V', VK_ALT, 'A', 'S', 'D', 'Q', 'E', 'F', 'R'};
        std::vector<int> pool;
        for (int vk : kPreferred)
            if (in(vk) && !dirKeys.count(vk)) pool.push_back(vk);
        for (int vk : g.keys)
            if (!contains(pool, vk) && !dirKeys.count(vk) && vk != VK_ENTER && vk != VK_ESCAPE) pool.push_back(vk);
        const int order[6] = {GP_FACE1, GP_FACE2, GP_FACE4, GP_FACE3, GP_SHOULDERR, GP_SHOULDERL};
        std::set<int> taken;
        if (g.complete && !g.keys.empty()) {
            for (int b : order) {
                if (!m.keys[b].empty() && in(m.keys[b][0])) taken.insert(m.keys[b][0]);
                else m.keys[b].clear();
            }
        } else {
            for (int b : order)
                for (int vk : m.keys[b]) taken.insert(vk);
        }
        size_t next = 0;
        for (int b : order) {
            if (!m.keys[b].empty()) continue;
            while (next < pool.size() && taken.count(pool[next])) next++;
            if (next < pool.size()) taken.insert(pool[next]), m.keys[b] = {pool[next++]};
        }
        // Confirm and back, for games that use only Enter and Escape in their menus.
        if (m.keys[GP_FACE1].empty() && in(VK_ENTER)) m.keys[GP_FACE1] = {VK_ENTER};
        if (m.keys[GP_FACE2].empty() && in(VK_ESCAPE)) m.keys[GP_FACE2] = {VK_ESCAPE};
        if (g.complete && !in(VK_ENTER) && in('P') && !taken.count('P')) m.keys[GP_START] = {'P'};
    }

    bool mouse = gameUsesMouse(dw);
    m.readsKeyboard = g.keyboard;
    m.readsGamepad = g.gamepad;
    m.anyKey = g.anyKey;
    m.mouseOnly = mouse && !g.keyboard && !g.gamepad;
    m.readsUnknown = !mouse && !g.keyboard && !g.gamepad;  // reads input some way the scan missed
    decideOnScreen(m);
    return m;
}

void applyButtonChoices(ButtonMap& m, const std::vector<std::pair<std::string, std::string>>& choices) {
    for (auto& [name, text] : choices) {
        int b = buttonFromName(name);
        if (b < 0) {
            fprintf(stderr, "[input] map_%s: no such button\n", name.c_str());
            continue;
        }
        if (!parseTargets(text, m.pad[b], m.keys[b])) fprintf(stderr, "[input] map_%s: unknown names in \"%s\"\n", name.c_str(), text.c_str());
        m.set[b] = true;
    }

    decideOnScreen(m);
}

// The on-screen pad shows the buttons that do something in this game.
void decideOnScreen(ButtonMap& m) {
    for (int b = 0; b < GP_BUTTON_COUNT; b++) {
        bool any = !m.pad[b].empty() || !m.keys[b].empty();
        bool keyUsed = false;
        for (int vk : m.keys[b]) keyUsed = keyUsed || contains(m.gameKeys, sameKey(vk));
        m.onScreen[b] = any && (m.set[b] || m.readsUnknown || (!m.pad[b].empty() && m.readsGamepad) ||
                                (!m.keys[b].empty() && m.readsKeyboard && (!m.gameKeysComplete || m.anyKey || keyUsed)));
    }
}

std::string describeButtonMap(const ButtonMap& m) {
    std::string s = "buttons:";
    for (int b = 0; b < GP_BUTTON_COUNT; b++) {
        std::string t;
        for (int vk : m.keys[b]) t += (t.empty() ? "" : "+") + keyName(vk);
        bool own = m.pad[b].size() == 1 && m.pad[b][0] == b;
        if (!own)
            for (int gp : m.pad[b]) t += (t.empty() ? "" : "+") + std::string(padTargetName(gp));
        if (t.empty() && own) continue;
        s += " " + std::string(buttonName(b)) + "=" + (t.empty() ? "none" : t) + (m.set[b] ? "*" : "");
    }
    s += " | on screen:";
    int shown = 0;
    for (int b = 0; b < GP_BUTTON_COUNT; b++)
        if (m.onScreen[b]) s += " " + std::string(buttonName(b)), shown++;
    if (!shown) s += m.mouseOnly ? " none (mouse only)" : " none";
    s += " | keys in the game:";
    for (int vk : m.gameKeys) s += " " + keyName(vk);
    if (!m.gameKeysComplete) s += " (and some in variables)";
    return s;
}

void Buttons::press(Runner& rt, int source, int button, bool down) {
    if (source < 0 || source > kOnScreen || button < 0 || button >= GP_BUTTON_COUNT) return;
    if (held[source][button] == down) return;
    held[source][button] = down;
    if (intercept && intercept(rt, source, button, down)) return;
    route(rt, source, button, down);
}

void Buttons::route(Runner& rt, int source, int button, bool down) {
    int pad = source == kOnScreen ? 0 : source;
    for (int gp : map.pad[button]) {
        int& n = padCount[pad][gp];
        n = std::max(0, n + (down ? 1 : -1));
        rt.input.padButton(pad, gp, n > 0);
    }
    for (int vk : map.keys[button]) key(rt, vk, down);
}

void Buttons::key(Runner& rt, int vk, bool down) {
    if (vk <= 1 || vk >= Input::kKeys) return;
    int& n = keyCount[vk];
    int was = n;
    n = std::max(0, n + (down ? 1 : -1));
    if ((was > 0) == (n > 0)) return;
    rt.input.keyEvent(vk, n > 0);
    // A real keyboard reports the side-specific key too (vk_lshift with vk_shift).
    int side = vk == VK_SHIFT ? VK_LSHIFT : vk == VK_CONTROL ? VK_LCONTROL : vk == VK_ALT ? VK_LALT : 0;
    if (side) rt.input.keyEvent(side, n > 0);
}

void Buttons::releaseAll(Runner& rt) {
    for (int s = 0; s <= kOnScreen; s++)
        for (int b = 0; b < GP_BUTTON_COUNT; b++)
            if (held[s][b]) held[s][b] = false, route(rt, s, b, false);
}

void Buttons::reset() {
    for (auto& s : held)
        for (bool& b : s) b = false;
    for (auto& p : padCount)
        for (int& n : p) n = 0;
    for (int& n : keyCount) n = 0;
}

}  // namespace gm::host
