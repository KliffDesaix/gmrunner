// gmdump: inspect a GameMaker data.win with the runner's own loader.
//
//   gmdump <data.win> info | verify | objects | rooms | sprites | scripts | builtins
//   gmdump <data.win> code <entry name>
//   gmdump <data.win> disasm-all <out dir>
//   gmdump <data.win> textures <out dir>
//   gmdump <data.win> sprite <name|index> <out dir>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include "../src/data/DataWin.h"
#include "../src/vm/Bytecode.h"
#include "../src/util/Image.h"
#include "../src/util/Png.h"

using namespace gm;
namespace fs = std::filesystem;

static const char* kEventNames[EV_COUNT] = {"Create", "Destroy", "Alarm", "Step", "Collision",
                                            "Keyboard", "Mouse", "Other", "Draw", "KeyPress",
                                            "KeyRelease", "Trigger", "CleanUp", "Gesture", "PreCreate"};

static void info(const DataWin& dw) {
    auto& g = dw.gen;
    printf("game        %.*s (%.*s)\n", int(g.displayName.size()), g.displayName.data(), int(g.name.size()),
           g.name.data());
    printf("ide         %s, bytecode %u%s\n", dw.engineVersion().c_str(), g.bytecodeVersion,
           dw.supported() ? "" : " (unsupported: only header and strings loaded)");
    printf("window      %ux%u\n", g.windowWidth, g.windowHeight);
    printf("options     flags 0x%llx\n", static_cast<unsigned long long>(dw.options.flags));
    printf("chunks     ");
    for (auto& [n, c] : dw.chunkList()) printf(" %s(%u)", n.c_str(), c.size);
    printf("\n");
    printf("strings %zu, textures %zu, tpag %zu, sprites %zu, backgrounds %zu, fonts %zu\n", dw.strings.size(),
           dw.textures.size(), dw.tpag.size(), dw.sprites.size(), dw.backgrounds.size(), dw.fonts.size());
    printf("sounds %zu (embedded %zu), paths %zu, scripts %zu, objects %zu, rooms %zu, timelines %zu, shaders %zu\n",
           dw.sounds.size(), dw.audio.size(), dw.paths.size(), dw.scripts.size(), dw.objects.size(),
           dw.rooms.size(), dw.timelines.size(), dw.shaders.size());
    printf("code %zu, variables %zu (max locals %u), functions %zu\n", dw.code.size(), dw.variables.size(),
           dw.maxLocalVars, dw.functions.size());
    printf("room order:");
    for (int32_t r : g.roomOrder)
        printf(" %.*s", int(dw.rooms[r].name.size()), dw.rooms[r].name.data());
    printf("\n");
}

// Decodes every code entry and checks that operands, references and branch targets are sane.
static int verify(const DataWin& dw) {
    size_t instructions = 0, errors = 0;
    auto fail = [&](const CodeEntry& c, const char* what, uint32_t addr) {
        if (errors++ < 20)
            fprintf(stderr, "%.*s @%x: %s\n", int(c.name.size()), c.name.data(), addr - c.bytecode, what);
    };
    for (auto& c : dw.code) {
        std::vector<Instruction> code;
        try {
            code = decodeEntry(dw, c);
        } catch (const FormatError& e) {
            fail(c, e.what(), c.bytecode);
            continue;
        }
        instructions += code.size();
        std::set<uint32_t> starts;
        for (auto& in : code) starts.insert(in.address);
        starts.insert(c.bytecode + c.length);
        for (auto& in : code) {
            bool needsVar = (in.op == Op::Pop && in.type1 != DataType::Int16) ||
                            ((in.op == Op::Push || in.op == Op::PushLoc || in.op == Op::PushGlb ||
                              in.op == Op::PushBltn) && in.type1 == DataType::Variable);
            if (needsVar && in.ref < 0) fail(c, "unresolved variable", in.address);
            if (in.op == Op::Call && in.ref < 0) fail(c, "unresolved function", in.address);
            if (in.op == Op::Push && in.type1 == DataType::String &&
                (in.ref < 0 || static_cast<size_t>(in.ref) >= dw.strings.size()))
                fail(c, "bad string index", in.address);
            if (in.isBranch() && !in.popenvExit && !starts.count(in.address + in.jump * 4))
                fail(c, "branch into the middle of an instruction", in.address);
            if (in.varKind == VarKind::Other) fail(c, "unknown variable access kind", in.address);
        }
    }
    printf("decoded %zu instructions in %zu code entries, %zu errors\n", instructions, dw.code.size(), errors);
    return errors ? 1 : 0;
}

static void objects(const DataWin& dw) {
    for (size_t i = 0; i < dw.objects.size(); i++) {
        auto& o = dw.objects[i];
        printf("%4zu %.*s", i, int(o.name.size()), o.name.data());
        if (o.parent >= 0) printf(" : %.*s", int(dw.objects[o.parent].name.size()), dw.objects[o.parent].name.data());
        printf("  [depth %d%s%s]\n", o.depth, o.persistent ? ", persistent" : "", o.visible ? "" : ", invisible");
        for (int t = 0; t < int(EV_COUNT); t++)
            for (auto& h : o.events[t]) {
                printf("       %s(%u)", kEventNames[t], h.subtype);
                for (int32_t c : h.code)
                    if (c >= 0 && static_cast<size_t>(c) < dw.code.size())
                        printf(" %.*s", int(dw.code[c].name.size()), dw.code[c].name.data());
                    else
                        printf(" <code %d>", c);
                printf("\n");
            }
    }
}

static void rooms(const DataWin& dw) {
    for (size_t i = 0; i < dw.rooms.size(); i++) {
        auto& r = dw.rooms[i];
        int views = 0;
        for (auto& v : r.views) views += v.enabled;
        printf("%3zu %-28.*s %ux%u @%ufps  instances %zu  tiles %zu  views %d\n", i, int(r.name.size()),
               r.name.data(), r.width, r.height, r.speed, r.instances.size(), r.tiles.size(), views);
    }
}

// One room in detail: its layers (type, depth, contents) and instances.
static int room(const DataWin& dw, const std::string& want) {
    for (size_t i = 0; i < dw.rooms.size(); i++) {
        auto& r = dw.rooms[i];
        if (r.name != want && std::to_string(i) != want) continue;
        printf("room %zu %.*s %ux%u, %zu layers, %zu instances\n", i, int(r.name.size()), r.name.data(), r.width,
               r.height, r.layers.size(), r.instances.size());
        printf("  flags 0x%x%s\n", r.flags, r.flags & 1 ? " (views enabled)" : "");
        for (size_t v = 0; v < r.views.size(); v++)
            if (r.views[v].enabled)
                printf("  view %zu: view %d,%d %dx%d port %d,%d %dx%d follow %d border %d,%d speed %d,%d\n", v, r.views[v].viewX, r.views[v].viewY,
                       r.views[v].viewW, r.views[v].viewH, r.views[v].portX, r.views[v].portY, r.views[v].portW,
                       r.views[v].portH, r.views[v].follow,
                       r.views[v].borderX, r.views[v].borderY, r.views[v].speedX, r.views[v].speedY);
        static const char* kTypes[] = {"path", "background", "instances", "assets", "tiles", "?", "effect"};
        for (auto& L : r.layers) {
            uint32_t t = static_cast<uint32_t>(L.type);
            printf("  layer %-28.*s id %-6u %-10s depth %6d%s", int(L.name.size()), L.name.data(), L.id,
                   t < 7 ? kTypes[t] : "?", L.depth, L.visible ? "" : " (hidden)");
            if (L.type == LayerType::Tiles) {
                size_t used = 0;
                for (uint32_t t : L.tiles) used += (t & 0x7FFFF) != 0;
                printf("  tileset %d %ux%u, %zu tiles used", L.tileset, L.tilesX, L.tilesY, used);
            }
            if (L.type == LayerType::Background)
                printf("  sprite %d colour %06X alpha %u%s", L.bgSprite, L.bgColor & 0xFFFFFF, L.bgColor >> 24,
                       L.bgVisible ? "" : " (bg hidden)");
            if (L.type == LayerType::Instances) printf("  %zu instances", L.instanceIds.size());
            if (L.type == LayerType::Assets) printf("  %zu sprites %zu tiles %zu sequences", L.sprites.size(), L.legacyTiles.size(), L.sequences.size());
            if (!L.fxType.empty())
                printf("  fx %.*s%s (%zu params)", int(L.fxType.size()), L.fxType.data(), L.fxEnabled ? "" : " off",
                       L.fxParams.size());
            printf("\n");
            auto spriteName = [&](int32_t s) {
                return s >= 0 && static_cast<size_t>(s) < dw.sprites.size() ? dw.sprites[s].name : std::string_view("?");
            };
            for (auto& t : L.legacyTiles) {
                std::string_view bg = t.bg >= 0 && static_cast<size_t>(t.bg) < dw.backgrounds.size()
                                          ? dw.backgrounds[t.bg].name : std::string_view("?");
                printf("    tile %.*s (%d) src %u,%u %ux%u at %d,%d scale %g,%g colour %08X\n", int(bg.size()), bg.data(),
                       t.bg, t.srcX, t.srcY, t.w, t.h, t.x, t.y, t.scaleX, t.scaleY, t.color);
            }
            for (auto& a : L.sprites) {
                std::string_view sn = spriteName(a.sprite);
                printf("    sprite %.*s (%d) at %d,%d scale %g,%g\n", int(sn.size()), sn.data(), a.sprite, a.x, a.y,
                       a.scaleX, a.scaleY);
            }
        }
        for (auto& in : r.instances) {
            std::string_view obj = in.object >= 0 && static_cast<size_t>(in.object) < dw.objects.size()
                                       ? dw.objects[in.object].name : std::string_view("?");
            printf("  instance %-6u %-32.*s at %d,%d%s%s\n", in.id, int(obj.size()), obj.data(), in.x, in.y,
                   in.creationCode >= 0 ? " +code" : "", in.preCreateCode >= 0 ? " +precreate" : "");
        }
        return 0;
    }
    fprintf(stderr, "no room %s\n", want.c_str());
    return 2;
}

// Fonts: size in points, glyph cell height, and the advance of 'A' (for matching TrueType rendering).
static void fonts(const DataWin& dw) {
    for (size_t i = 0; i < dw.fonts.size(); i++) {
        const Font& f = dw.fonts[i];
        int maxH = 0, advA = -1;
        for (auto& g : f.glyphs) {
            maxH = std::max(maxH, static_cast<int>(g.h));
            if (g.ch == 'A') advA = g.shift;
        }
        printf("%3zu %-24s %-20s %5.1fpt%s%s cell %d px, 'A' advance %d, %zu glyphs\n", i, std::string(f.name).c_str(),
               std::string(f.displayName).c_str(), f.emSize, f.bold ? " bold" : "", f.italic ? " italic" : "", maxH,
               advA, f.glyphs.size());
    }
}

static void sprites(const DataWin& dw) {
    for (size_t i = 0; i < dw.sprites.size(); i++) {
        auto& s = dw.sprites[i];
        printf("%4zu %-32.*s %ux%u origin(%d,%d) frames %zu masks %zu%s", i, int(s.name.size()), s.name.data(),
               s.width, s.height, s.originX, s.originY, s.frames.size(), s.masks.size(),
               s.unsupportedType ? " UNSUPPORTED" : "");
        const NineSlice& n = s.nineSlice;
        if (n.enabled)
            printf(" nine-slice %d,%d,%d,%d modes %d%d%d%d%d", n.left, n.top, n.right, n.bottom, n.modes[0], n.modes[1],
                   n.modes[2], n.modes[3], n.modes[4]);
        printf("\n");
    }
}

static void builtins(const DataWin& dw) {
    std::set<std::string_view> scripts;
    for (auto& s : dw.scripts) scripts.insert(s.name);
    for (auto& f : dw.functions)
        if (!scripts.count(f.name)) printf("%.*s\n", int(f.name.size()), f.name.data());
}

static void writeFile(const fs::path& p, const void* data, size_t n) {
    std::ofstream f(p, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
}

// Sequences: settings, then the track tree with each track's keyframes (key, length, data).
static void printTrack(const DataWin& dw, const SeqTrack& t, int depth) {
    printf("%*s%.*s \"%.*s\" builtin=%d traits=%d%s keys=%zu\n", depth * 2, "", int(t.model.size()), t.model.data(),
           int(t.name.size()), t.name.data(), t.builtinName, t.traits, t.creation ? " creation" : "", t.keys.size());
    for (size_t i = 0; i < t.keys.size() && i < 8; i++) {
        const SeqKeyframe& k = t.keys[i];
        printf("%*s  @%g len=%g%s%s", depth * 2, "", k.key, k.length, k.stretch ? " stretch" : "", k.disabled ? " disabled" : "");
        for (const SeqChannel& c : k.channels) {
            printf(" [%d:", c.channel);
            if (t.model == "GMRealTrack") printf("%g", c.real);
            else if (t.model == "GMColourTrack") printf("%08x", c.colour);
            else if (!c.text.empty()) printf("%.*s", int(c.text.size()), c.text.data());
            else printf("%d", c.index);
            if (c.curve >= 0) printf(" curve=%d", c.curve);
            if (c.embeddedCurve >= 0) printf(" embedded=%zu", dw.seqCurves[c.embeddedCurve].channels.size());
            printf("]");
        }
        printf("\n");
    }
    if (t.keys.size() > 8) printf("%*s  ...\n", depth * 2, "");
    for (const SeqTrack& s : t.tracks) printTrack(dw, s, depth + 1);
}
static void sequences(const DataWin& dw) {
    for (size_t i = 0; i < dw.sequences.size(); i++) {
        const Sequence& s = dw.sequences[i];
        printf("%zu %.*s playback=%u speed=%g/%s length=%g origin=%d,%d volume=%g messages=%zu moments=%zu\n", i,
               int(s.name.size()), s.name.data(), s.playback, s.speed, s.speedType ? "frame" : "s", s.length, s.originX,
               s.originY, s.volume, s.messages.size(), s.moments.size());
        for (const SeqTrack& t : s.tracks) printTrack(dw, t, 1);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: gmdump <data.win> info|verify|objects|rooms|sprites|fonts|sequences|scripts|builtins\n"
                "       gmdump <data.win> code|locals <name>\n"
                "       gmdump <data.win> room <name|index>\n"
                "       gmdump <data.win> disasm-all|textures <out dir>\n"
                "       gmdump <data.win> sprite <name|index> <out dir>\n");
        return 2;
    }
    std::unique_ptr<DataWin> dw;
    try {
        dw = DataWin::load(argv[1]);
    } catch (const std::exception& e) {
        fprintf(stderr, "load failed: %s\n", e.what());
        return 1;
    }
    std::string cmd = argv[2];
    try {
        if (cmd == "info") info(*dw);
        else if (cmd == "verify") return verify(*dw);
        else if (cmd == "objects") objects(*dw);
        else if (cmd == "rooms") rooms(*dw);
        else if (cmd == "sprites") sprites(*dw);
        else if (cmd == "fonts") fonts(*dw);
        else if (cmd == "sequences") sequences(*dw);
        else if (cmd == "room" && argc > 3) return room(*dw, argv[3]);
        else if (cmd == "builtins") builtins(*dw);
        else if (cmd == "scripts") {
            for (auto& s : dw->scripts) printf("%.*s\n", int(s.name.size()), s.name.data());
        } else if (cmd == "locals" && argc > 3) {
            // The FUNC chunk's local-variable list for a code entry (index, name).
            for (auto& cl : dw->codeLocals) {
                if (cl.codeName != argv[3]) continue;
                for (auto& [idx, name] : cl.locals) printf("%u %.*s\n", idx, int(name.size()), name.data());
            }
        } else if (cmd == "code" && argc > 3) {
            int32_t i = dw->codeIndex(argv[3]);
            if (i < 0) i = dw->codeIndex(std::string("gml_Script_") + argv[3]);
            if (i < 0) i = dw->codeIndex(std::string("gml_Object_") + argv[3]);
            if (i < 0) {
                fprintf(stderr, "no code entry named %s\n", argv[3]);
                return 1;
            }
            auto& c = dw->code[i];
            printf("; %.*s  locals=%u args=%u length=%u entry=%05x\n", int(c.name.size()), c.name.data(), c.locals,
                   c.args, c.length, c.offset);
            fputs(disassemble(*dw, c).c_str(), stdout);
        } else if (cmd == "disasm-all" && argc > 3) {
            fs::create_directories(argv[3]);
            for (auto& c : dw->code) {
                std::string text = disassemble(*dw, c);
                writeFile(fs::path(argv[3]) / (std::string(c.name) + ".asm"), text.data(), text.size());
            }
            printf("wrote %zu files\n", dw->code.size());
        } else if (cmd == "textures" && argc > 3) {
            fs::create_directories(argv[3]);
            for (size_t i = 0; i < dw->textures.size(); i++) {
                std::vector<uint8_t> px;
                int w, h;
                std::string err;
                if (!decodeTexture(dw->textures[i], px, w, h, err)) {
                    fprintf(stderr, "texture %zu: %s\n", i, err.c_str());
                    continue;
                }
                writePng((fs::path(argv[3]) / ("texture_" + std::to_string(i) + ".png")).string(), px.data(), w, h);
            }
            printf("wrote %zu textures\n", dw->textures.size());
        } else if (cmd == "sprite" && argc > 4) {
            // Each frame of one sprite (by name or index) as frame_N.png, at its full bounding size.
            std::string want = argv[3];
            int idx = -1;
            for (size_t i = 0; i < dw->sprites.size(); i++)
                if (dw->sprites[i].name == want || std::to_string(i) == want) idx = static_cast<int>(i);
            if (idx < 0) {
                fprintf(stderr, "no sprite %s\n", want.c_str());
                return 2;
            }
            fs::create_directories(argv[4]);
            const Sprite& sp = dw->sprites[idx];
            std::map<int, std::vector<uint8_t>> pages;
            std::map<int, std::pair<int, int>> sizes;
            for (size_t f = 0; f < sp.frames.size(); f++) {
                const TexturePageItem& t = dw->tpag[sp.frames[f]];
                if (!pages.count(t.texture)) {
                    int w, h;
                    std::string err;
                    if (!decodeTexture(dw->textures[t.texture], pages[t.texture], w, h, err)) {
                        fprintf(stderr, "texture %d: %s\n", t.texture, err.c_str());
                        return 1;
                    }
                    sizes[t.texture] = {w, h};
                }
                const auto& px = pages[t.texture];
                int pw = sizes[t.texture].first;
                std::vector<uint8_t> out(static_cast<size_t>(t.boundW) * t.boundH * 4, 0);
                for (int y = 0; y < t.tgtH && y < t.srcH; y++)
                    for (int x = 0; x < t.tgtW && x < t.srcW; x++)
                        std::memcpy(&out[(static_cast<size_t>(t.tgtY + y) * t.boundW + t.tgtX + x) * 4],
                                    &px[(static_cast<size_t>(t.srcY + y) * pw + t.srcX + x) * 4], 4);
                writePng((fs::path(argv[4]) / ("frame_" + std::to_string(f) + ".png")).string(), out.data(), t.boundW,
                         t.boundH);
                printf("frame %zu: page %d src %d,%d %dx%d tgt %d,%d %dx%d bound %dx%d\n", f, t.texture, t.srcX, t.srcY,
                       t.srcW, t.srcH, t.tgtX, t.tgtY, t.tgtW, t.tgtH, t.boundW, t.boundH);
            }
        } else {
            fprintf(stderr, "unknown command %s\n", cmd.c_str());
            return 2;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
