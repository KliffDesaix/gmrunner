#include "DataWin.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <unordered_set>
#if (defined(__unix__) || defined(__APPLE__)) && !defined(__SWITCH__)
#define GMR_MMAP 1
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gm {

std::unique_ptr<DataWin> DataWin::load(const std::string& path) {
#ifdef GMR_MMAP
    int fd = open(path.c_str(), O_RDONLY);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0) {
        void* m = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (m != MAP_FAILED) {
            auto dw = std::unique_ptr<DataWin>(new DataWin());
            dw->mapped_ = m;
            dw->mappedSize_ = static_cast<size_t>(st.st_size);
            dw->r_ = Reader(static_cast<const uint8_t*>(m), dw->mappedSize_);
            dw->parse();
            return dw;
        }
    } else if (fd >= 0) {
        close(fd);
    }
#endif
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw FormatError("cannot open " + path);
    std::vector<uint8_t> buf(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return fromBuffer(std::move(buf));
}

std::unique_ptr<DataWin> DataWin::fromBuffer(std::vector<uint8_t> buffer) {
    auto dw = std::unique_ptr<DataWin>(new DataWin());
    dw->buf_ = std::move(buffer);
    dw->r_ = Reader(dw->buf_.data(), dw->buf_.size());
    dw->parse();
    return dw;
}

DataWin::~DataWin() {
#ifdef GMR_MMAP
    if (mapped_) munmap(mapped_, mappedSize_);
#endif
}

std::string DataWin::engineVersion() const {
    if (gen.gms2 && gen.bytecodeVersion <= 16) return "GameMaker Studio 2.0-2.1";
    if (gen.bytecodeVersion <= 16) {
        // GMS 1.x writes 1.0.0.<build> into GEN8; the bytecode version narrows the release down.
        const char* ver = gen.bytecodeVersion == 16 ? "1.4" : gen.bytecodeVersion == 15 ? "1.3-1.4" : "1.x";
        char buf[64];
        snprintf(buf, sizeof buf, "GameMaker Studio %s (build %u)", ver, gen.build);
        return buf;
    }
    // GMS2 writes 2.0.0.0 into GEN8; newer chunks tell the versions apart.
    if (hasChunk("UILR")) return "GameMaker 2024 or newer";
    if (hasChunk("PSEM")) return "GameMaker 2023.2 or newer";
    if (hasChunk("FEAT")) return "GameMaker 2022.8 or newer";
    if (hasChunk("SEQN")) return "GameMaker Studio 2.3 or newer";
    if (hasChunk("TGIN")) return "GameMaker Studio 2.2";
    return "GameMaker Studio 2";
}

int32_t DataWin::tpagIndexAt(uint32_t offset) const {
    auto it = tpagByOffset_.find(offset);
    return it == tpagByOffset_.end() ? -1 : it->second;
}

int32_t DataWin::codeIndex(std::string_view name) const {
    auto it = codeByName_.find(name);
    return it == codeByName_.end() ? -1 : it->second;
}

template <typename F> static void withChunk(const char* name, F&& f) {
    try {
        f();
    } catch (const FormatError& e) {
        throw FormatError(std::string(name) + ": " + e.what());
    }
}

void DataWin::parse() {
    if (r_.size() < 8 || std::memcmp(r_.data(), "FORM", 4) != 0) throw FormatError("not a FORM file");
    uint32_t end = 8 + r_.u32(4);
    for (uint32_t p = 8; p + 8 <= end;) {
        std::string name(reinterpret_cast<const char*>(r_.data() + p), 4);
        uint32_t len = r_.u32(p + 4);
        r_.check(p + 8, len);
        chunks_[name] = {p + 8, len};
        chunkOrder_.push_back({name, {p + 8, len}});
        p += 8 + len;
    }

    auto at = [&](const char* n) { return chunks_.at(n); };
    if (!hasChunk("GEN8")) throw FormatError("missing GEN8");
    parseGEN8(at("GEN8").offset, at("GEN8").size);
    if (hasChunk("STRG")) withChunk("STRG", [&] { parseSTRG(at("STRG").offset); });
    // YYC builds compile the game to native code: CODE is empty (or missing), whatever the
    // format version says, so no bytecode version would help.
    if (!hasChunk("CODE") || at("CODE").size == 0) {
        unsupported_ = engineVersion() + " compiled with YYC (native code): there is no bytecode to run";
        return;
    }
    if (gen.bytecodeVersion > 17) {
        unsupported_ = engineVersion() + " (bytecode " + std::to_string(gen.bytecodeVersion) +
                       "); only bytecode 15-17 is supported";
        return;
    }
    gen.gms2 = gen.major >= 2;
    gen.gms23 = gen.gms2 && (hasChunk("SEQN") || hasChunk("FEAT"));
    if (gen.bytecodeVersion < 15) {
        unsupported_ = "bytecode " + std::to_string(gen.bytecodeVersion) +
                       " (GameMaker Studio 1.x before 1.4); only bytecode 15-16 is supported";
        return;
    }

    if (hasChunk("OPTN")) withChunk("OPTN", [&] { parseOPTN(at("OPTN").offset); });
    if (hasChunk("TXTR")) withChunk("TXTR", [&] { parseTXTR(at("TXTR").offset, at("TXTR").size); });
    if (hasChunk("TGIN")) withChunk("TGIN", [&] { parseTGIN(at("TGIN").offset); });
    if (hasChunk("TPAG")) withChunk("TPAG", [&] { parseTPAG(at("TPAG").offset); });
    if (hasChunk("SPRT")) withChunk("SPRT", [&] { parseSPRT(at("SPRT").offset); });
    if (hasChunk("BGND")) withChunk("BGND", [&] { parseBGND(at("BGND").offset); });
    if (hasChunk("FONT")) withChunk("FONT", [&] { parseFONT(at("FONT").offset); });
    if (hasChunk("AUDO")) withChunk("AUDO", [&] { parseAUDO(at("AUDO").offset); });
    if (hasChunk("SOND")) withChunk("SOND", [&] { parseSOND(at("SOND").offset); });
    if (hasChunk("AGRP")) withChunk("AGRP", [&] { parseAGRP(at("AGRP").offset); });
    if (hasChunk("PATH")) withChunk("PATH", [&] { parsePATH(at("PATH").offset); });
    if (hasChunk("SCPT")) withChunk("SCPT", [&] { parseSCPT(at("SCPT").offset); });
    if (hasChunk("GLOB")) withChunk("GLOB", [&] { parseGLOB(at("GLOB").offset); });
    if (hasChunk("OBJT")) withChunk("OBJT", [&] { parseOBJT(at("OBJT").offset); });
    if (hasChunk("PSEM")) withChunk("PSEM", [&] { parsePSEM(at("PSEM").offset, at("PSEM").size); });
    if (hasChunk("PSYS")) withChunk("PSYS", [&] { parsePSYS(at("PSYS").offset); });
    if (hasChunk("ROOM")) withChunk("ROOM", [&] { parseROOM(at("ROOM").offset); });
    if (hasChunk("TMLN")) withChunk("TMLN", [&] { parseTMLN(at("TMLN").offset); });
    if (hasChunk("SHDR")) withChunk("SHDR", [&] { parseSHDR(at("SHDR").offset, at("SHDR").size); });
    if (hasChunk("CODE")) withChunk("CODE", [&] { parseCODE(at("CODE").offset, at("CODE").size); });
    if (hasChunk("VARI")) withChunk("VARI", [&] { parseVARI(at("VARI").offset, at("VARI").size); });
    if (hasChunk("FUNC")) withChunk("FUNC", [&] { parseFUNC(at("FUNC").offset, at("FUNC").size); });
    if (hasChunk("TAGS")) withChunk("TAGS", [&] { parseTAGS(at("TAGS").offset); });
    if (hasChunk("ACRV")) withChunk("ACRV", [&] { parseACRV(at("ACRV").offset, at("ACRV").size); });
    if (hasChunk("SEQN")) withChunk("SEQN", [&] { parseSEQN(at("SEQN").offset, at("SEQN").size); });
    if (hasChunk("EXTN")) withChunk("EXTN", [&] { parseEXTN(at("EXTN").offset); });
    if (hasChunk("FEDS")) withChunk("FEDS", [&] { parseFEDS(at("FEDS").offset); });
    resolveReferences();
}

void DataWin::parseGEN8(uint32_t o, uint32_t len) {
    gen.disableDebugger = r_.u8(o) != 0;
    gen.bytecodeVersion = r_.u8(o + 1);
    gen.fileName = r_.str(r_.u32(o + 4));
    gen.config = r_.str(r_.u32(o + 8));
    gen.lastObj = r_.u32(o + 12);
    gen.lastTile = r_.u32(o + 16);
    gen.gameId = r_.u32(o + 20);
    gen.name = r_.str(r_.u32(o + 40));
    gen.major = r_.u32(o + 44);
    gen.minor = r_.u32(o + 48);
    gen.release = r_.u32(o + 52);
    gen.build = r_.u32(o + 56);
    gen.windowWidth = r_.u32(o + 60);
    gen.windowHeight = r_.u32(o + 64);
    gen.infoFlags = r_.u32(o + 68);
    gen.displayName = r_.str(r_.u32(o + 100));
    gen.steamAppId = r_.i32(o + 120);
    uint32_t p = o + (gen.bytecodeVersion >= 14 ? 128 : 124);
    if (p + 4 <= o + len) {
        uint32_t n = r_.u32(p);
        for (uint32_t i = 0; i < n; i++) gen.roomOrder.push_back(r_.i32(p + 4 + i * 4));
        // GMS2: five 64-bit random/UID values, then the game speed (FPS).
        uint32_t q = p + 4 + n * 4 + 40;
        if (gen.major >= 2 && q + 4 <= o + len) gen.gms2Fps = r_.f32(q);
    }
}

uint32_t DataWin::entrySize(uint32_t listOffset) const {
    uint32_t n = r_.u32(listOffset);
    if (n < 2) return 0;
    return r_.u32(listOffset + 8) - r_.u32(listOffset + 4);
}

void DataWin::parseOPTN(uint32_t o) {
    if (r_.u32(o) == 0x80000000u) {  // GMS 1.4+ layout: marker, version, 64-bit flags, ...
        options.flags = r_.u64(o + 8);
        options.scale = r_.i32(o + 16);
        options.windowColor = r_.u32(o + 20);
    } else {
        options.flags = 0;
        static const uint64_t order[] = {OPT_FULLSCREEN, OPT_INTERPOLATE, 0x8, OPT_SHOW_CURSOR};
        for (int i = 0; i < 4; i++)
            if (r_.b32(o + i * 4)) options.flags |= order[i];
    }
}

void DataWin::parseSTRG(uint32_t o) {
    pointerList(o, [&](uint32_t, uint32_t p) { strings.push_back(r_.str(p + 4)); });
}

static size_t pngLength(const Reader& r, uint32_t o) {
    uint32_t p = o + 8;
    for (;;) {
        r.check(p, 8);
        const uint8_t* b = r.data() + p;
        uint32_t clen = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
        bool iend = std::memcmp(b + 4, "IEND", 4) == 0;
        p += 12 + clen;
        if (iend) return p - o;
    }
}

void DataWin::parseTXTR(uint32_t o, uint32_t len) {
    // Entry layout grows with the version: 8 bytes (1.4: scaled, data), 12 (GMS2: + mipmaps),
    // 16 (2022.3: + block size), 28 (2022.9: + width, height, index in group).
    uint32_t n = r_.u32(o);
    uint32_t size = entrySize(o);
    auto fieldFor = [](uint32_t sz) -> uint32_t { return sz == 8 ? 4 : sz == 12 ? 8 : sz == 16 ? 12 : 24; };
    if (size == 0 && n == 1) {
        // One page: no neighbour to measure against, so take the layout whose data pointer
        // lands on a texture (PNG, QOI or bzip2 QOI).
        uint32_t p = r_.u32(o + 4);
        for (uint32_t sz : {28u, 16u, 12u, 8u}) {
            uint32_t d = r_.u32(p + fieldFor(sz));
            if (d + 4 <= r_.size() && (std::memcmp(r_.data() + d, "\x89PNG", 4) == 0 ||
                                       std::memcmp(r_.data() + d, "fioq", 4) == 0 ||
                                       std::memcmp(r_.data() + d, "2zoq", 4) == 0)) {
                size = sz;
                break;
            }
        }
    }
    if (size == 0) size = gen.bytecodeVersion >= 17 ? 12 : 8;
    uint32_t dataField = fieldFor(size);
    std::vector<uint32_t> starts;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = r_.u32(o + 4 + i * 4);
        Texture t;
        t.scaled = r_.u32(p);
        uint32_t dataPtr = r_.u32(p + dataField);
        if (size >= 16) t.size = r_.u32(p + 8);
        if (size >= 28) {
            t.width = r_.u32(p + 12);
            t.height = r_.u32(p + 16);
            t.groupIndex = r_.u32(p + 20);
        }
        if (dataPtr) {
            r_.check(dataPtr, 12);
            const uint8_t* d = r_.data() + dataPtr;
            static const uint8_t pngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
            t.data = d;
            if (std::memcmp(d, pngSig, 8) == 0) {
                t.format = TextureFormat::Png;
                t.size = pngLength(r_, dataPtr);
            } else if (std::memcmp(d, "fioq", 4) == 0) {
                t.format = TextureFormat::Qoi;
            } else if (std::memcmp(d, "2zoq", 4) == 0) {
                t.format = TextureFormat::Bz2Qoi;
            } else {
                throw FormatError("texture " + std::to_string(i) + " has an unknown format");
            }
            if (t.format != TextureFormat::Png) {
                if (!t.width) t.width = r_.u16(dataPtr + 4);
                if (!t.height) t.height = r_.u16(dataPtr + 6);
            }
        }
        starts.push_back(dataPtr);
        textures.push_back(t);
    }
    // Sizes for formats without a stored length: up to the next texture's data (or chunk end).
    for (size_t i = 0; i < textures.size(); i++) {
        if (textures[i].size || !textures[i].data) continue;
        uint32_t start = starts[i], next = o + len;
        for (uint32_t s2 : starts)
            if (s2 > start && s2 < next) next = s2;
        textures[i].size = next - start;
    }
}

void DataWin::parseTPAG(uint32_t o) {
    pointerList(o, [&](uint32_t i, uint32_t p) {
        TexturePageItem t;
        t.srcX = r_.u16(p);
        t.srcY = r_.u16(p + 2);
        t.srcW = r_.u16(p + 4);
        t.srcH = r_.u16(p + 6);
        t.tgtX = r_.u16(p + 8);
        t.tgtY = r_.u16(p + 10);
        t.tgtW = r_.u16(p + 12);
        t.tgtH = r_.u16(p + 14);
        t.boundW = r_.u16(p + 16);
        t.boundH = r_.u16(p + 18);
        t.texture = r_.i16(p + 20);
        tpag.push_back(t);
        tpagByOffset_[p] = static_cast<int32_t>(i);
    });
}

void DataWin::parseSPRT(uint32_t o) {
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!p) {  // an empty slot (GameMaker 2024 can strip unused assets): keep the index
            sprites.emplace_back();
            return;
        }
        Sprite s;
        s.name = r_.str(r_.u32(p));
        s.width = r_.u32(p + 4);
        s.height = r_.u32(p + 8);
        s.marginLeft = r_.i32(p + 12);
        s.marginRight = r_.i32(p + 16);
        s.marginBottom = r_.i32(p + 20);
        s.marginTop = r_.i32(p + 24);
        s.transparent = r_.b32(p + 28);
        s.smooth = r_.b32(p + 32);
        s.preload = r_.b32(p + 36);
        s.bboxMode = r_.u32(p + 40);
        s.sepMasks = r_.b32(p + 44);
        s.originX = r_.i32(p + 48);
        s.originY = r_.i32(p + 52);
        uint32_t q = p + 56;
        if (r_.i32(q) == -1) {
            // GMS2 header: -1, version, type, playback speed, speed type, then the sequence
            // offset (version 2+, GMS 2.3) and the nine-slice offset (version 3+, 2.3.2).
            uint32_t version = r_.u32(q + 4);
            uint32_t type = r_.u32(q + 8);
            s.playbackSpeed = r_.f32(q + 12);
            s.playbackSpeedType = r_.u32(q + 16);
            q += 20;
            if (version >= 2) q += 4;  // sequence
            if (version >= 3) {
                if (uint32_t ns = r_.u32(q)) {  // left, top, right, bottom, enabled, 5 tile modes
                    NineSlice& n = s.nineSlice;
                    n.left = r_.i32(ns);
                    n.top = r_.i32(ns + 4);
                    n.right = r_.i32(ns + 8);
                    n.bottom = r_.i32(ns + 12);
                    n.enabled = r_.b32(ns + 16);
                    for (int k = 0; k < 5; k++) n.modes[k] = r_.i32(ns + 20 + k * 4);
                }
                q += 4;
            }
            if (type == 2) {  // skeleton: the texture pages as frames, then the skeleton data
                uint32_t frames = r_.u32(q);
                for (uint32_t i = 0; i < frames; i++) s.frames.push_back(tpagIndexAt(r_.u32(q + 4 + i * 4)));
                q += 4 + frames * 4;
                if (!parseSkeleton(q, s)) s.unsupportedType = true;
                sprites.push_back(s);
                return;
            }
            if (type != 0) {
                s.unsupportedType = true;
                sprites.push_back(s);
                return;
            }
        }
        uint32_t frames = r_.u32(q);
        for (uint32_t i = 0; i < frames; i++) s.frames.push_back(tpagIndexAt(r_.u32(q + 4 + i * 4)));
        q += 4 + frames * 4;
        uint32_t masks = r_.u32(q);
        q += 4;
        uint32_t maskSize = ((s.width + 7) / 8) * s.height;
        for (uint32_t i = 0; i < masks; i++) {
            r_.check(q, maskSize);
            s.masks.push_back(r_.data() + q);
            q += maskSize;
        }
        sprites.push_back(s);
    });
}

// A skeleton sprite's data (version 3, GameMaker 2023+): version, a flag, the sizes of the
// skeleton and the atlas and the page count, then the two, as stored.
bool DataWin::parseSkeleton(uint32_t q, Sprite& s) {
    SkeletonSprite sk;
    sk.version = r_.u32(q);
    if (sk.version != 3) return false;
    sk.skeletonSize = r_.u32(q + 8);
    sk.atlasSize = r_.u32(q + 12);
    q += 20;
    r_.check(q, sk.skeletonSize + sk.atlasSize);
    sk.skeleton = r_.data() + q;
    sk.atlas = r_.data() + q + sk.skeletonSize;
    s.skeleton = static_cast<int32_t>(skeletons.size());
    skeletons.push_back(sk);
    return true;
}

void DataWin::parseBGND(uint32_t o) {
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!p) {  // an empty slot (GameMaker 2024 can strip unused assets): keep the index
            backgrounds.emplace_back();
            return;
        }
        Background b;
        b.name = r_.str(r_.u32(p));
        b.transparent = r_.b32(p + 4);
        b.smooth = r_.b32(p + 8);
        b.preload = r_.b32(p + 12);
        b.tpag = tpagIndexAt(r_.u32(p + 16));
        if (gen.gms2) {  // tileset
            b.tileWidth = r_.u32(p + 24);
            b.tileHeight = r_.u32(p + 28);
            b.borderX = r_.u32(p + 32);
            b.borderY = r_.u32(p + 36);
            b.columns = r_.u32(p + 40);
            b.framesPerTile = r_.u32(p + 44);
            b.tileCount = r_.u32(p + 48);
            b.frameTime = static_cast<int64_t>(r_.u64(p + 56));
            uint32_t ids = b.tileCount * std::max<uint32_t>(1, b.framesPerTile);
            for (uint32_t i = 0; i < ids; i++) b.tileIds.push_back(r_.u32(p + 64 + i * 4));
        }
        backgrounds.push_back(b);
    });
}

// ACRV: version, then a pointer list of curves: name, graph type, channel count, and inline
// channels (name, type, iterations, point count, points). Points are 24 bytes (x, value and
// two bezier handles) since 2.3.1, 8 before; the size is detected from where curves end.
void DataWin::parseACRV(uint32_t o, uint32_t len) {
    uint32_t n = r_.u32(o + 4);
    std::vector<uint32_t> starts;
    for (uint32_t i = 0; i < n; i++) starts.push_back(r_.u32(o + 8 + i * 4));
    auto parseCurve = [&](uint32_t p, uint32_t pointSize, uint32_t& end) {
        AnimCurve c;
        c.name = r_.str(r_.u32(p));
        c.graphType = r_.i32(p + 4);
        uint32_t channels = r_.u32(p + 8), q = p + 12;
        for (uint32_t k = 0; k < channels && k < 1024; k++) {
            AnimCurveChannel ch;
            ch.name = r_.str(r_.u32(q));
            ch.type = r_.i32(q + 4);
            ch.iterations = r_.u32(q + 8);
            uint32_t pts = r_.u32(q + 12);
            q += 16;
            for (uint32_t j = 0; j < pts && j < 100000 && q + pointSize <= o + len; j++, q += pointSize) {
                AnimCurvePoint pt;
                pt.x = r_.f32(q);
                pt.value = r_.f32(q + 4);
                if (pointSize == 24) {
                    pt.tx0 = r_.f32(q + 8);
                    pt.ty0 = r_.f32(q + 12);
                    pt.tx1 = r_.f32(q + 16);
                    pt.ty1 = r_.f32(q + 20);
                }
                ch.points.push_back(pt);
            }
            c.channels.push_back(std::move(ch));
        }
        end = q;
        return c;
    };
    uint32_t pointSize = 24;
    if (n > 1) {  // the first curve must end where the second starts
        uint32_t end = 0;
        parseCurve(starts[0], 24, end);
        if (end != starts[1]) pointSize = 8;
    }
    for (uint32_t p : starts) {
        uint32_t end = 0;
        animCurves.push_back(parseCurve(p, pointSize, end));
    }
}

// SEQN: version (1), then a pointer list of sequences: name, playback, speed, speed type,
// length, origin, volume, (newer compilers: canvas width and height), broadcast-message keyframes, tracks, function ids and moment
// keyframes. Keyframe stores are inline: key, length, stretch, disabled and the channels
// (channel number + data). Tracks are model name, name, built-in parameter, traits, creation
// flag, the counts of tags, owned resources and subtracks, the tags (1 invisible, 2 disabled),
// the subtracks, then keyframes whose data depends on the model.
// Real/colour data may carry an embedded animation curve (graph type + channels, as in ACRV).
void DataWin::parseSEQN(uint32_t o, uint32_t len) {
    uint32_t end = o + len;
    if (len < 8 || r_.u32(o) != 1) return;
    auto need = [&](uint32_t p, uint32_t n) {
        if (p + n > end) throw std::runtime_error("SEQN: data past the chunk");
    };
    // Reads a keyframe store; `data` reads one channel's data at p and returns the next offset.
    auto keyframes = [&](uint32_t& p, std::vector<SeqKeyframe>& out, auto&& data) {
        need(p, 4);
        uint32_t n = r_.u32(p);
        p += 4;
        for (uint32_t k = 0; k < n && k < 100000; k++) {
            need(p, 20);
            SeqKeyframe kf;
            kf.key = r_.f32(p);
            kf.length = r_.f32(p + 4);
            kf.stretch = r_.u32(p + 8) != 0;
            kf.disabled = r_.u32(p + 12) != 0;
            uint32_t nch = r_.u32(p + 16);
            p += 20;
            for (uint32_t c = 0; c < nch && c < 64; c++) {
                need(p, 4);
                SeqChannel ch;
                ch.channel = r_.i32(p);
                p = data(p + 4, ch);
                kf.channels.push_back(std::move(ch));
            }
            out.push_back(std::move(kf));
        }
    };
    auto realData = [&](uint32_t p, SeqChannel& ch) {
        need(p, 12);
        ch.real = r_.f32(p);
        ch.colour = r_.u32(p);
        bool embedded = r_.u32(p + 4) != 0;
        ch.curve = r_.i32(p + 8);
        p += 12;
        if (embedded) {
            need(p, 8);
            AnimCurve c;
            c.graphType = r_.i32(p);
            uint32_t nch = r_.u32(p + 4);
            p += 8;
            for (uint32_t i = 0; i < nch && i < 64; i++) {
                need(p, 16);
                AnimCurveChannel cc;
                cc.name = r_.str(r_.u32(p));
                cc.type = r_.i32(p + 4);
                cc.iterations = r_.u32(p + 8);
                uint32_t pts = r_.u32(p + 12);
                p += 16;
                need(p, pts * 24);
                for (uint32_t j = 0; j < pts; j++, p += 24)
                    cc.points.push_back({r_.f32(p), r_.f32(p + 4), r_.f32(p + 8), r_.f32(p + 12), r_.f32(p + 16),
                                         r_.f32(p + 20)});
                c.channels.push_back(std::move(cc));
            }
            ch.embeddedCurve = static_cast<int32_t>(seqCurves.size());
            ch.curve = -1;
            seqCurves.push_back(std::move(c));
        }
        return p;
    };
    auto intData = [&](uint32_t p, SeqChannel& ch) {
        need(p, 4);
        ch.index = r_.i32(p);
        return p + 4;
    };
    std::function<void(uint32_t&, SeqTrack&)> track = [&](uint32_t& p, SeqTrack& t) {
        need(p, 32);
        t.model = r_.str(r_.u32(p));
        t.name = r_.str(r_.u32(p + 4));
        t.builtinName = r_.i32(p + 8);
        t.traits = r_.i32(p + 12);
        t.creation = r_.u32(p + 16) != 0;
        // The counts of tags, owned resources and subtracks come first, then the tags.
        uint32_t ntags = r_.u32(p + 20);
        uint32_t owned = r_.u32(p + 24);
        uint32_t nsub = r_.u32(p + 28);
        p += 32;
        need(p, ntags * 4);
        for (uint32_t i = 0; i < ntags && i < 64; i++) t.tags.push_back(r_.i32(p + i * 4));
        p += ntags * 4;
        if (owned) throw std::runtime_error("SEQN: track with owned resources");
        for (uint32_t i = 0; i < nsub && i < 4096; i++) {
            t.tracks.emplace_back();
            track(p, t.tracks.back());
        }
        const std::string_view m = t.model;
        if (m == "GMRealTrack" || m == "GMColourTrack") {
            need(p, 4);
            t.interpolation = r_.i32(p);
            p += 4;
            keyframes(p, t.keys, realData);
        } else if (m == "GMGraphicTrack" || m == "GMInstanceTrack" || m == "GMSequenceTrack" ||
                   m == "GMSpriteFramesTrack" || m == "GMBoolTrack" || m == "GMParticleTrack") {
            keyframes(p, t.keys, intData);
        } else if (m == "GMStringTrack") {
            keyframes(p, t.keys, [&](uint32_t q, SeqChannel& ch) {
                need(q, 4);
                ch.text = r_.str(r_.u32(q));
                return q + 4;
            });
        } else if (m == "GMAudioTrack") {
            keyframes(p, t.keys, [&](uint32_t q, SeqChannel& ch) {
                need(q, 12);
                ch.index = r_.i32(q);
                ch.mode = r_.i32(q + 8);
                return q + 12;
            });
        } else if (m != "GMGroupTrack" && m != "GMClipMaskTrack" && m != "GMClipMask_Mask" &&
                   m != "GMClipMask_Subject") {
            throw std::runtime_error("SEQN: unsupported track type " + std::string(m));
        }
    };
    // One sequence at p; returns where its data ends. `extra` is the size of the fields newer
    // compilers put after the volume (two floats, the sequence's canvas size).
    auto sequence = [&](uint32_t p, uint32_t extra, Sequence& s) {
        need(p, 32 + extra);
        s.name = r_.str(r_.u32(p));
        s.playback = r_.u32(p + 4);
        s.speed = r_.f32(p + 8);
        s.speedType = r_.u32(p + 12);
        s.length = r_.f32(p + 16);
        s.originX = r_.i32(p + 20);
        s.originY = r_.i32(p + 24);
        s.volume = r_.f32(p + 28);
        p += 32 + extra;
        keyframes(p, s.messages, [&](uint32_t q, SeqChannel& ch) {
            need(q, 4);
            uint32_t k = r_.u32(q);
            need(q, 4 + k * 4);
            for (uint32_t j = 0; j < k; j++) ch.messages.push_back(r_.str(r_.u32(q + 4 + j * 4)));
            return q + 4 + k * 4;
        });
        need(p, 4);
        uint32_t nt = r_.u32(p);
        p += 4;
        for (uint32_t t = 0; t < nt && t < 4096; t++) {
            s.tracks.emplace_back();
            track(p, s.tracks.back());
        }
        need(p, 4);
        uint32_t nf = r_.u32(p);
        p += 4;
        need(p, nf * 8);
        for (uint32_t f = 0; f < nf; f++) s.functions.push_back({r_.i32(p + f * 8), r_.str(r_.u32(p + f * 8 + 4))});
        p += nf * 8;
        keyframes(p, s.moments, [&](uint32_t q, SeqChannel& ch) {
            need(q, 8);
            ch.index = r_.i32(q);
            ch.text = r_.str(r_.u32(q + 4));
            return q + 8;
        });
        return p;
    };
    uint32_t n = r_.u32(o + 4);
    auto start = [&](uint32_t i) { return i < n ? r_.u32(o + 8 + i * 4) : end; };
    // The layout is the one the first sequence reads cleanly with, ending where the next begins.
    uint32_t extra = 0;
    for (uint32_t candidate : {0u, 8u}) {
        try {
            Sequence probe;
            uint32_t e = n && start(0) ? sequence(start(0), candidate, probe) : 0;
            if (e == start(1) || (n == 1 && e <= end)) {
                extra = candidate;
                break;
            }
        } catch (const std::exception&) {
        }
    }
    for (uint32_t i = 0; i < n && i < 100000; i++) {
        uint32_t p = start(i);
        Sequence s;
        try {
            if (!p) throw std::runtime_error("");
            sequence(p, extra, s);
        } catch (const std::exception& e) {
            // Each sequence has its own pointer, so one we can't read doesn't affect the rest.
            if (*e.what()) std::fprintf(stderr, "[data] sequence %u (%.*s): %s\n", i, static_cast<int>(s.name.size()), s.name.data(), e.what());
            s.tracks.clear();
            s.messages.clear();
            s.moments.clear();
        }
        sequences.push_back(std::move(s));
    }
}

// TGIN: version, then a pointer list of texture groups. Since 2022.9 a group has its name, a
// directory, a file extension and a load type before its lists (texture pages first). Pages of
// dynamic groups aren't in the data file but in <directory>/<group>_<index in group><extension>.
void DataWin::parseTGIN(uint32_t o) {
    uint32_t end = o + chunks_.at("TGIN").size;
    pointerList(o + 4, [&](uint32_t, uint32_t p) {
        uint32_t dirPtr = r_.u32(p + 4), extPtr = r_.u32(p + 8);
        // Older groups go straight to their list pointers, which point inside TGIN.
        if ((dirPtr >= o && dirPtr < end) || (extPtr >= o && extPtr < end) || r_.u32(p + 12) >= 16) return;
        std::string name(r_.str(r_.u32(p))), dir(r_.str(dirPtr)), ext(r_.str(extPtr));
        uint32_t pages = r_.u32(p + 16), n = r_.u32(pages);
        for (uint32_t i = 0; i < n && i < 100000; i++) {
            uint32_t id = r_.u32(pages + 4 + i * 4);
            if (id >= textures.size() || textures[id].data) continue;
            textures[id].externalFile = (dir.empty() ? "" : dir + "/") + name + "_" +
                                        std::to_string(textures[id].groupIndex) + ext;
        }
    });
}

// PSEM (2023.2+): version, then a pointer list of emitters, each with the particle type it
// emits. Newer versions added fields, so the layout follows the entries' size: 204 bytes (2023.2),
// 232 (enabled flag, delay and interval) and 236 (relative emission).
void DataWin::parsePSEM(uint32_t o, uint32_t len) {
    uint32_t n = r_.u32(o + 4);
    if (!n) return;
    uint32_t first = r_.u32(o + 8), size = n > 1 ? r_.u32(o + 12) - first : o + len - first;
    if (size != 204 && size != 232 && size != 236) {
        std::fprintf(stderr, "[data] particle emitters of %u bytes aren't supported\n", size);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = r_.u32(o + 8 + i * 4);
        PsEmitter e;
        uint32_t q = p;
        auto u = [&] { uint32_t v = r_.u32(q); q += 4; return v; };
        auto i32 = [&] { return static_cast<int32_t>(u()); };
        auto f = [&] { float v = r_.f32(q); q += 4; return v; };
        e.name = r_.str(u());
        if (size >= 232) e.enabled = u() != 0;
        e.mode = i32();
        e.count = f();
        if (size >= 236) e.relative = u() != 0;
        if (size >= 232) {
            e.delayMin = f(), e.delayMax = f(), e.delayUnit = i32();
            e.intervalMin = f(), e.intervalMax = f(), e.intervalUnit = i32();
        }
        e.distribution = i32(), e.shape = i32();
        e.regionX = f(), e.regionY = f(), e.regionW = f(), e.regionH = f(), e.rotation = f();
        e.sprite = i32(), e.texture = i32();
        e.frameIndex = f();
        e.animate = u() != 0, e.stretch = u() != 0, e.randomFrame = u() != 0;
        e.startColor = u(), e.midColor = u(), e.endColor = u();
        e.additive = u() != 0;
        e.lifeMin = f(), e.lifeMax = f(), e.scaleX = f(), e.scaleY = f();
        e.sizeMinX = f(), e.sizeMaxX = f(), e.sizeMinY = f(), e.sizeMaxY = f();
        e.sizeIncX = f(), e.sizeIncY = f(), e.sizeWiggleX = f(), e.sizeWiggleY = f();
        e.speedMin = f(), e.speedMax = f(), e.speedInc = f(), e.speedWiggle = f();
        e.gravity = f(), e.gravityDir = f();
        e.dirMin = f(), e.dirMax = f(), e.dirInc = f(), e.dirWiggle = f();
        e.angMin = f(), e.angMax = f(), e.angInc = f(), e.angWiggle = f();
        e.angRelative = u() != 0;
        e.spawnOnDeath = i32(), e.spawnOnDeathCount = i32(), e.spawnOnUpdate = i32(), e.spawnOnUpdateCount = i32();
        psEmitters.push_back(e);
    }
}

// PSYS (2023.2+): version, then a pointer list of systems: name, origin, draw order, global
// space flag and the indices of their emitters in PSEM.
void DataWin::parsePSYS(uint32_t o) {
    uint32_t n = r_.u32(o + 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = r_.u32(o + 8 + i * 4);
        PsSystem ps;
        if (p) {
            ps.name = r_.str(r_.u32(p));
            ps.originX = r_.i32(p + 4);
            ps.originY = r_.i32(p + 8);
            ps.drawOrder = r_.i32(p + 12);
            ps.globalSpace = r_.u32(p + 16) != 0;
            uint32_t ne = r_.u32(p + 20);
            for (uint32_t k = 0; k < ne && k < 4096; k++) ps.emitters.push_back(r_.i32(p + 24 + k * 4));
        }
        psSystems.push_back(std::move(ps));
    }
}

// TAGS: version, the list of tag names, then per asset its id ((type << 24) | index) and tags.
void DataWin::parseTAGS(uint32_t o) {
    uint32_t names = r_.u32(o + 4);
    uint32_t q = o + 8 + names * 4;
    pointerList(q, [&](uint32_t, uint32_t p) {
        std::vector<std::string_view> tags;
        uint32_t n = r_.u32(p + 4);
        for (uint32_t k = 0; k < n; k++) tags.push_back(r_.str(r_.u32(p + 8 + k * 4)));
        assetTags.push_back({r_.i32(p), std::move(tags)});
    });
}

void DataWin::parseFONT(uint32_t o) {
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!p) {  // an empty slot (GameMaker 2024 can strip unused assets): keep the index
            fonts.emplace_back();
            return;
        }
        Font f;
        f.name = r_.str(r_.u32(p));
        f.displayName = r_.str(r_.u32(p + 4));
        uint32_t em = r_.u32(p + 8);
        if (em & 0x80000000u) {
            float v;
            std::memcpy(&v, &em, 4);
            f.emSize = -v;
        } else {
            f.emSize = static_cast<float>(em);
        }
        f.bold = r_.b32(p + 12);
        f.italic = r_.b32(p + 16);
        f.rangeStart = r_.u16(p + 20);
        f.charset = r_.u8(p + 22);
        f.antiAlias = r_.u8(p + 23);
        f.rangeEnd = r_.u32(p + 24);
        f.tpag = tpagIndexAt(r_.u32(p + 28));
        f.scaleX = r_.f32(p + 32);
        f.scaleY = r_.f32(p + 36);
        // GMS2 inserts ascender offset (2.0.6), ascender (2022.2), SDF spread (2023.2) and line
        // height (2023.6) before the glyph list; locate the list by its shape instead.
        uint32_t g = p + 40;
        for (uint32_t cand = p + 40; cand <= p + 60; cand += 4) {
            uint32_t n = r_.u32(cand);
            if (n > 0 && n < 100000 && r_.u32(cand + 4) == cand + 4 + n * 4) {
                g = cand;
                break;
            }
            if (n == 0 && (cand + 4 == p + 60 || cand == p + 40)) g = cand;
        }
        if (g >= p + 44) f.ascenderOffset = r_.i32(p + 40);
        if (g >= p + 48) f.ascender = r_.i32(p + 44);
        if (g >= p + 52) f.sdfSpread = r_.i32(p + 48);
        if (g >= p + 56) f.lineHeight = r_.i32(p + 52);
        pointerList(g, [&](uint32_t, uint32_t gp) {
            f.glyphs.push_back({r_.u16(gp), r_.u16(gp + 2), r_.u16(gp + 4), r_.u16(gp + 6), r_.u16(gp + 8),
                                r_.i16(gp + 10), r_.i16(gp + 12)});
        });
        fonts.push_back(std::move(f));
    });
}

void DataWin::parseSOND(uint32_t o) {
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!p) {  // an empty slot (GameMaker 2024 can strip unused assets): keep the index
            sounds.emplace_back();
            return;
        }
        Sound s;
        s.name = r_.str(r_.u32(p));
        s.flags = r_.u32(p + 4);
        s.type = r_.str(r_.u32(p + 8));
        s.file = r_.str(r_.u32(p + 12));
        s.effects = r_.u32(p + 16);
        s.volume = r_.f32(p + 20);
        s.pitch = r_.f32(p + 24);
        s.group = r_.i32(p + 28);
        s.audioFile = r_.i32(p + 32);
        sounds.push_back(s);
    });
}

void DataWin::parseAUDO(uint32_t o) {
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!p) {  // an empty slot (GameMaker 2024 can strip unused assets): keep the index
            audio.emplace_back();
            return;
        }
        uint32_t n = r_.u32(p);
        r_.check(p + 4, n);
        audio.push_back({r_.data() + p + 4, n});
    });
}

void DataWin::parseAGRP(uint32_t o) {
    pointerList(o, [&](uint32_t, uint32_t p) { audioGroups.push_back(r_.str(r_.u32(p))); });
}

void DataWin::parsePATH(uint32_t o) {
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!p) {  // an empty slot (GameMaker 2024 can strip unused assets): keep the index
            paths.emplace_back();
            return;
        }
        Path path;
        path.name = r_.str(r_.u32(p));
        path.smooth = r_.b32(p + 4);
        path.closed = r_.b32(p + 8);
        path.precision = r_.u32(p + 12);
        uint32_t n = r_.u32(p + 16);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t q = p + 20 + i * 12;
            path.points.push_back({r_.f32(q), r_.f32(q + 4), r_.f32(q + 8)});
        }
        paths.push_back(std::move(path));
    });
}

void DataWin::parseSCPT(uint32_t o) {
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!p) {  // an empty slot (GameMaker 2024 can strip unused assets): keep the index
            scripts.emplace_back();
            return;
        }
        int32_t c = r_.i32(p + 4);
        bool ctor = c < -1;  // GMS 2.3 marks constructors with the top bit
        scripts.push_back({r_.str(r_.u32(p)), ctor ? (c & 0x7fffffff) : c, ctor});
    });
}

void DataWin::parseGLOB(uint32_t o) {
    uint32_t n = r_.u32(o);
    for (uint32_t i = 0; i < n; i++) globalInit.push_back(r_.i32(o + 4 + i * 4));
}

// Event actions are shared by objects and timelines; only the code reference matters at runtime.
static std::vector<int32_t> readActions(const Reader& r, uint32_t listPtr) {
    std::vector<int32_t> out;
    uint32_t n = r.u32(listPtr);
    for (uint32_t i = 0; i < n; i++) out.push_back(r.i32(r.u32(listPtr + 4 + i * 4) + 32));
    return out;
}

void DataWin::parseOBJT(uint32_t o) {
    // GMS 2022.5 added a "managed" flag after `visible`, shifting everything after it.
    // Detect it from where the event table lands for the first object.
    uint32_t shift = 0;
    if (gen.gms2 && r_.u32(o) > 0) {
        uint32_t p = r_.u32(o + 4);
        auto eventsAt = [&](uint32_t sh) {
            uint32_t vc = r_.u32(p + 64 + sh);
            if (vc > 100000) return false;
            uint32_t ev = p + 80 + sh + vc * 8;
            uint32_t types = r_.u32(ev);
            return types >= 12 && types <= EV_COUNT && r_.u32(ev + 4) > ev && r_.u32(ev + 4) < r_.size();
        };
        if (!eventsAt(0) && eventsAt(4)) shift = 4;
    }
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!p) {  // an empty slot (GameMaker 2024 can strip unused assets): keep the index
            objects.emplace_back();
            return;
        }
        GameObject obj;
        obj.name = r_.str(r_.u32(p));
        obj.sprite = r_.i32(p + 4);
        obj.visible = r_.b32(p + 8);
        if (shift) obj.managed = r_.b32(p + 12);
        uint32_t q = p + shift;
        obj.solid = r_.b32(q + 12);
        obj.depth = r_.i32(q + 16);
        obj.persistent = r_.b32(q + 20);
        obj.parent = r_.i32(q + 24);
        obj.mask = r_.i32(q + 28);
        obj.physics = r_.b32(q + 32);
        uint32_t vertexCount = r_.u32(q + 64);
        uint32_t ev = q + 80 + vertexCount * 8;
        uint32_t types = r_.u32(ev);
        if (types > EV_COUNT) throw FormatError("object " + std::string(obj.name) + ": bad event list");
        for (uint32_t t = 0; t < types; t++) {
            uint32_t list = r_.u32(ev + 4 + t * 4);
            pointerList(list, [&](uint32_t, uint32_t ep) {
                obj.events[t].push_back({r_.u32(ep), readActions(r_, ep + 4)});
            });
        }
        objects.push_back(std::move(obj));
    });
}

void DataWin::parseROOM(uint32_t o) {
    bool preCreate = gen.bytecodeVersion >= 16;
    auto roomChunk = chunks_.find("ROOM");
    uint32_t roomLo = roomChunk->second.offset, roomHi = roomLo + roomChunk->second.size;
    auto inRoomChunk = [&](uint32_t v) { return v >= roomLo && v < roomHi; };
    // GameMaker 2024 added a list after the tiles list in the room header, moving the physics
    // fields and the layer list down by 4 bytes. Older rooms have the physics "world" flag there.
    auto layersAt = [&](uint32_t p) { return inRoomChunk(r_.u32(p + 56)) ? p + 92 : p + 88; };
    // 2022.1+ layers carry effect fields (enabled, type, properties) before their data. Decide
    // from the data: an instance layer without them starts with a count and instance ids.
    bool layerEffects = hasChunk("FEAT");
    if (gen.gms2) {
        bool decided = false;
        pointerList(o, [&](uint32_t, uint32_t p) {
            if (decided || !p) return;
            std::unordered_set<uint32_t> ids;
            pointerList(r_.u32(p + 48), [&](uint32_t, uint32_t q) { ids.insert(r_.u32(q + 12)); });
            if (ids.empty()) return;
            pointerList(r_.u32(layersAt(p)), [&](uint32_t, uint32_t q) {
                if (decided || r_.u32(q + 8) != static_cast<uint32_t>(LayerType::Instances)) return;
                uint32_t n = r_.u32(q + 36);
                if (n > 0 && n <= ids.size() && ids.count(r_.u32(q + 40))) {
                    layerEffects = false;
                    decided = true;
                    return;
                }
                uint32_t props = r_.u32(q + 44);
                uint32_t d = q + 48 + props * 12;
                if (props < 1000 && r_.u32(d) > 0 && r_.u32(d) <= ids.size() && ids.count(r_.u32(d + 4))) {
                    layerEffects = true;
                    decided = true;
                }
            });
        });
    }
    uint32_t gameInstanceSize = 0;
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!gameInstanceSize && p) gameInstanceSize = entrySize(r_.u32(p + 48));
    });
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!p) {  // an empty slot (GameMaker 2024 can strip unused assets): keep the index
            rooms.emplace_back();
            return;
        }
        Room rm;
        rm.name = r_.str(r_.u32(p));
        rm.caption = r_.str(r_.u32(p + 4));
        rm.width = r_.u32(p + 8);
        rm.height = r_.u32(p + 12);
        rm.speed = r_.u32(p + 16);
        rm.persistent = r_.b32(p + 20);
        rm.bgColor = r_.u32(p + 24);
        rm.drawBgColor = r_.b32(p + 28);
        rm.creationCode = r_.i32(p + 32);
        rm.flags = r_.u32(p + 36);
        pointerList(r_.u32(p + 40), [&](uint32_t, uint32_t q) {
            rm.backgrounds.push_back({r_.b32(q), r_.b32(q + 4), r_.i32(q + 8), r_.i32(q + 12), r_.i32(q + 16),
                                      r_.b32(q + 20), r_.b32(q + 24), r_.i32(q + 28), r_.i32(q + 32),
                                      r_.b32(q + 36)});
        });
        pointerList(r_.u32(p + 44), [&](uint32_t, uint32_t q) {
            rm.views.push_back({r_.b32(q), r_.i32(q + 4), r_.i32(q + 8), r_.i32(q + 12), r_.i32(q + 16),
                                r_.i32(q + 20), r_.i32(q + 24), r_.i32(q + 28), r_.i32(q + 32), r_.u32(q + 36),
                                r_.u32(q + 40), r_.i32(q + 44), r_.i32(q + 48), r_.i32(q + 52)});
        });
        // GMS 2.2.2 added image speed + image index to room instances (48 bytes instead of 40).
        // Told by the spacing of the entries; a room with one instance has none, so it goes by
        // the game's other rooms (a 2.0-2.1 game tested has one-instance rooms).
        uint32_t instList = r_.u32(p + 48);
        uint32_t isz = entrySize(instList);
        bool imageFields = isz ? isz >= 48 : gameInstanceSize ? gameInstanceSize >= 48 : gen.gms2;
        pointerList(instList, [&](uint32_t, uint32_t q) {
            RoomInstance ri{};
            ri.x = r_.i32(q);
            ri.y = r_.i32(q + 4);
            ri.object = r_.i32(q + 8);
            ri.id = r_.u32(q + 12);
            ri.creationCode = r_.i32(q + 16);
            ri.scaleX = r_.f32(q + 20);
            ri.scaleY = r_.f32(q + 24);
            uint32_t k = q + 28;
            ri.imageSpeed = 1;
            ri.imageIndex = 0;
            if (imageFields) {
                ri.imageSpeed = r_.f32(k);
                ri.imageIndex = r_.i32(k + 4);
                k += 8;
            }
            ri.color = r_.u32(k);
            ri.rotation = r_.f32(k + 4);
            ri.preCreateCode = preCreate ? r_.i32(k + 8) : -1;
            rm.instances.push_back(ri);
        });
        pointerList(r_.u32(p + 52), [&](uint32_t, uint32_t q) {
            rm.tiles.push_back({r_.i32(q), r_.i32(q + 4), r_.i32(q + 8), r_.u32(q + 12), r_.u32(q + 16),
                                r_.u32(q + 20), r_.u32(q + 24), r_.i32(q + 28), r_.u32(q + 32), r_.f32(q + 36),
                                r_.f32(q + 40), r_.u32(q + 44)});
        });
        if (gen.gms2) {
            pointerList(r_.u32(layersAt(p)), [&](uint32_t, uint32_t q) {
                RoomLayer L;
                L.name = r_.str(r_.u32(q));
                L.id = r_.u32(q + 4);
                L.type = static_cast<LayerType>(r_.u32(q + 8));
                L.depth = r_.i32(q + 12);
                L.xOffset = r_.f32(q + 16);
                L.yOffset = r_.f32(q + 20);
                L.hSpeed = r_.f32(q + 24);
                L.vSpeed = r_.f32(q + 28);
                L.visible = r_.b32(q + 32);
                uint32_t d = q + 36;
                if (layerEffects) {  // enabled, type, property list (kind, name, value)
                    L.fxEnabled = r_.b32(q + 36);
                    if (uint32_t t = r_.u32(q + 40)) L.fxType = r_.str(t);
                    uint32_t props = r_.u32(q + 44);
                    for (uint32_t i = 0; i < props && i < 256; i++) {
                        uint32_t e = q + 48 + i * 12;
                        L.fxParams.push_back({r_.str(r_.u32(e + 4)), r_.str(r_.u32(e + 8))});
                    }
                    d += 8 + 4 + props * 12;
                }
                switch (L.type) {
                    case LayerType::Instances: {
                        uint32_t n = r_.u32(d);
                        for (uint32_t i = 0; i < n; i++) L.instanceIds.push_back(r_.u32(d + 4 + i * 4));
                        break;
                    }
                    case LayerType::Background:
                        L.bgVisible = r_.b32(d);
                        L.bgForeground = r_.b32(d + 4);
                        L.bgSprite = r_.i32(d + 8);
                        L.bgHTiled = r_.b32(d + 12);
                        L.bgVTiled = r_.b32(d + 16);
                        L.bgStretch = r_.b32(d + 20);
                        L.bgColor = r_.u32(d + 24);
                        L.bgFirstFrame = r_.f32(d + 28);
                        L.bgAnimSpeed = r_.f32(d + 32);
                        L.bgAnimSpeedType = r_.u32(d + 36);
                        break;
                    case LayerType::Tiles:
                        parseTileLayer(L, d);
                        break;
                    case LayerType::Assets: {
                        pointerList(r_.u32(d), [&](uint32_t, uint32_t t) {
                            L.legacyTiles.push_back({r_.i32(t), r_.i32(t + 4), r_.i32(t + 8), r_.u32(t + 12),
                                                     r_.u32(t + 16), r_.u32(t + 20), r_.u32(t + 24), r_.i32(t + 28),
                                                     r_.u32(t + 32), r_.f32(t + 36), r_.f32(t + 40), r_.u32(t + 44)});
                        });
                        auto spriteAssets = [&](uint32_t list, std::vector<RoomSpriteAsset>& out) {
                            pointerList(list, [&](uint32_t, uint32_t t) {
                                out.push_back({r_.str(r_.u32(t)), r_.i32(t + 4), r_.i32(t + 8), r_.i32(t + 12),
                                               r_.f32(t + 16), r_.f32(t + 20), r_.u32(t + 24), r_.f32(t + 28),
                                               r_.u32(t + 32), r_.f32(t + 36), r_.f32(t + 40)});
                            });
                        };
                        spriteAssets(r_.u32(d + 4), L.sprites);
                        // 2.3+: sequences (same fields as sprites) follow, then (2023.2+) particle systems.
                        if (gen.gms23) spriteAssets(r_.u32(d + 8), L.sequences);
                        if (!psSystems.empty())
                            pointerList(r_.u32(d + 12), [&](uint32_t, uint32_t t) {
                                L.particleSystems.push_back({r_.str(r_.u32(t)), r_.i32(t + 4), r_.i32(t + 8),
                                                             r_.i32(t + 12), r_.i32(t + 16), r_.f32(t + 20),
                                                             r_.f32(t + 24), r_.u32(t + 28), r_.f32(t + 32)});
                            });
                        break;
                    }
                    default:
                        break;
                }
                rm.layers.push_back(std::move(L));
            });
        }
        rooms.push_back(std::move(rm));
    });
}

void DataWin::parseTileLayer(RoomLayer& L, uint32_t q) {
    L.tileset = r_.i32(q);
    L.tilesX = r_.u32(q + 4);
    L.tilesY = r_.u32(q + 8);
    uint32_t n = L.tilesX * L.tilesY;
    L.tiles.reserve(n);
    for (uint32_t i = 0; i < n; i++) L.tiles.push_back(r_.u32(q + 12 + i * 4));
}

// Extensions: only the names and return types of their native (non-GML) functions are kept.
// Each file is name, cleanup, init, kind and its function list; each function is name, id, kind,
// return type, external name, argument count and argument types.
void DataWin::parseEXTN(uint32_t o) {
    uint32_t end = o + chunks_.at("EXTN").size;
    try {
        pointerList(o, [&](uint32_t, uint32_t e) {
            // After folder and name: class name, or version then class name (2.3+). The file list
            // follows, or a pointer to it (2022.6+). Counts are small; strings lie outside EXTN.
            uint32_t filesAt = e + 12;
            uint32_t v = r_.u32(filesAt);
            if (v >= 0x10000 && (v < o || v >= end)) v = r_.u32(filesAt = e + 16);
            if (v >= o && v < end) filesAt = v;
            pointerList(filesAt, [&](uint32_t, uint32_t f) {
                if (r_.u32(f + 12) == 2) return;  // a GML file: its functions are ordinary scripts
                pointerList(f + 16, [&](uint32_t, uint32_t fn) {
                    extensionFunctions.push_back({r_.str(r_.u32(fn)), r_.u32(fn + 12)});
                });
            });
        });
    } catch (const FormatError&) {
        extensionFunctions.clear();  // an unexpected layout only costs the extension stubs
    }
}

void DataWin::parseFEDS(uint32_t o) {  // version, then a list of {name, JSON}
    pointerList(o + 4, [&](uint32_t, uint32_t p) { effectDefs.push_back({r_.str(r_.u32(p)), r_.str(r_.u32(p + 4))}); });
}

void DataWin::parseTMLN(uint32_t o) {
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!p) {  // an empty slot (GameMaker 2024 can strip unused assets): keep the index
            timelines.emplace_back();
            return;
        }
        Timeline tl;
        tl.name = r_.str(r_.u32(p));
        uint32_t n = r_.u32(p + 4);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t step = r_.u32(p + 8 + i * 8);
            uint32_t actions = r_.u32(p + 12 + i * 8);
            tl.moments.push_back({step, readActions(r_, actions), {}});
        }
        timelines.push_back(std::move(tl));
    });
}

void DataWin::parseSHDR(uint32_t o, uint32_t) {
    pointerList(o, [&](uint32_t, uint32_t p) {
        if (!p) {  // an empty slot (GameMaker 2024 can strip unused assets): keep the index
            shaders.emplace_back();
            return;
        }
        Shader s;
        s.name = r_.str(r_.u32(p));
        s.type = r_.u32(p + 4) & 0x7fffffffu;
        s.glslesVertex = r_.str(r_.u32(p + 8));
        s.glslesFragment = r_.str(r_.u32(p + 12));
        s.glslVertex = r_.str(r_.u32(p + 16));
        s.glslFragment = r_.str(r_.u32(p + 20));
        uint32_t attrs = r_.u32(p + 40);
        for (uint32_t i = 0; i < attrs; i++) s.attributes.push_back(r_.str(r_.u32(p + 44 + i * 4)));
        shaders.push_back(std::move(s));
    });
}

void DataWin::parseCODE(uint32_t o, uint32_t len) {
    if (len == 0) return;  // YYC builds ship without bytecode
    pointerList(o, [&](uint32_t i, uint32_t p) {
        CodeEntry c;
        c.name = r_.str(r_.u32(p));
        c.length = r_.u32(p + 4);
        c.locals = r_.u16(p + 8);
        c.args = r_.u16(p + 10) & 0x7fff;
        c.bytecode = static_cast<uint32_t>(static_cast<int64_t>(p + 12) + r_.i32(p + 12));
        c.offset = r_.u32(p + 16);
        r_.check(c.bytecode, c.length);
        codeByName_[c.name] = static_cast<int32_t>(i);
        code.push_back(c);
    });
}

void DataWin::parseVARI(uint32_t o, uint32_t len) {
    if (len == 0) return;
    varCount1 = r_.u32(o);
    varCount2 = r_.u32(o + 4);
    maxLocalVars = r_.u32(o + 8);
    for (uint32_t p = o + 12; p + 20 <= o + len; p += 20) {
        if (r_.u32(p) == 0) break;  // GMS2 pads the chunk
        variables.push_back({r_.str(r_.u32(p)), r_.i32(p + 4), r_.i32(p + 8), r_.u32(p + 12), r_.u32(p + 16)});
    }
}

void DataWin::parseFUNC(uint32_t o, uint32_t len) {
    if (len == 0) return;
    uint32_t n = r_.u32(o);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = o + 4 + i * 12;
        functions.push_back({r_.str(r_.u32(p)), r_.u32(p + 4), r_.u32(p + 8)});
    }
    uint32_t q = o + 4 + n * 12;
    if (q + 4 > o + len) return;  // newer compilers can leave out the code locals list
    uint32_t nl = r_.u32(q);
    q += 4;
    for (uint32_t i = 0; i < nl; i++) {
        CodeLocals cl;
        uint32_t count = r_.u32(q);
        cl.codeName = r_.str(r_.u32(q + 4));
        q += 8;
        for (uint32_t k = 0; k < count; k++, q += 8) cl.locals.push_back({r_.u32(q), r_.str(r_.u32(q + 4))});
        codeLocals.push_back(std::move(cl));
    }
}

// Each Push/Pop/Call operand that references a variable or function stores, in its low
// 27 bits, the distance to the next instruction referencing the same entry. Walking these
// chains from VARI/FUNC's first address recovers which entry each instruction means.
//
// Newer GameMaker versions record the function chain at the operand word instead of the
// instruction (detected below), so the walk is done relative to that word.
void DataWin::resolveReferences() {
    auto walk = [&](auto& table, auto& refs, bool atOperand) {
        uint32_t lo = 0, hi = static_cast<uint32_t>(r_.size());
        auto c = chunks_.find("CODE");
        if (c != chunks_.end()) {
            lo = c->second.offset;
            hi = c->second.offset + c->second.size;
        }
        for (size_t i = 0; i < table.size(); i++) {
            uint32_t addr = table[i].firstAddress;
            for (uint32_t k = 0; k < table[i].occurrences; k++) {
                if (addr < lo || addr + 8 > hi) break;  // broken chain: leave the rest unresolved
                uint32_t instr = atOperand ? addr - 4 : addr;
                refs[instr] = static_cast<int32_t>(i);
                addr += r_.u32(instr + 4) & 0x07ffffffu;
            }
        }
    };
    auto pointsAtOperand = [&](const std::vector<Function>& fns) {
        for (auto& f : fns) {
            if (!f.occurrences || f.firstAddress < 4 || f.firstAddress + 4 > r_.size()) continue;
            uint8_t op = static_cast<uint8_t>(r_.u32(f.firstAddress) >> 24);
            // Not a call/push/pushref (break.i) instruction word: the chain points at operands.
            return op != 0xD9 && op != 0xC0 && op != 0x84 && op != 0xFF;
        }
        return false;
    };
    walk(variables, varRefs, false);
    walk(functions, funcRefs, pointsAtOperand(functions));
}

}  // namespace gm
