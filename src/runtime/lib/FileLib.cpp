// file_*, file_text_*, ini_* and directory_* with a per-game save area.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <sstream>

#include "runtime/Builtins.h"
#include "util/Hash.h"
#include "util/Utf8.h"
#include "runtime/Stores.h"
#include "util/Fs.h"

// stb_image's inflate (compiled into Graphics.cpp for PNG), used for zip entries.

#include "util/Stb.h"

namespace gm {

std::string FileStore::relative(const std::string& gmlPath) const {
    std::string p = gmlPath;
    for (char& c : p)
        if (c == '\\') c = '/';
    auto strip = [&](const std::string& prefix) {
        if (!prefix.empty() && p.compare(0, prefix.size(), prefix) == 0) {
            p = p.substr(prefix.size());
            return true;
        }
        return false;
    };
    if (!strip(rt.saveDir + "/")) strip(rt.gameDir + "/");
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    return p;
}

std::string FileStore::readPath(const std::string& gmlPath) const {
    std::string rel = relative(gmlPath);
    std::string saved = rt.saveDir + "/" + rel;
    if (isFile(saved) || isDir(saved)) return saved;
    return rt.gameDir + "/" + rel;
}

std::string FileStore::writePath(const std::string& gmlPath) const {
    std::string path = rt.saveDir + "/" + relative(gmlPath);
    makeDirs(parentDir(path));
    return path;
}

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// ---- plain files
static Value fileHash(Runner& rt, const std::string& gmlPath, bool sha) {
    std::string data;
    if (!readFile(rt.files->readPath(gmlPath), data)) return Value("");
    auto* p = reinterpret_cast<const uint8_t*>(data.data());
    return Value(sha ? sha1Hex(p, data.size()) : md5Hex(p, data.size()));
}
GMFN(md5_file) { return fileHash(rt, S(0), false); }
GMFN(sha1_file) { return fileHash(rt, S(0), true); }
static Value stringHash(const std::string& s, bool unicode, bool sha) {
    std::string bytes = s;
    if (unicode) {  // UTF-16LE, as GameMaker's *_string_unicode
        bytes.clear();
        for (char32_t c : utf8Decode(s)) {
            auto put = [&](uint32_t u) { bytes += static_cast<char>(u & 0xFF); bytes += static_cast<char>(u >> 8); };
            if (c >= 0x10000) {
                put(0xD800 + ((c - 0x10000) >> 10));
                put(0xDC00 + ((c - 0x10000) & 0x3FF));
            } else {
                put(static_cast<uint32_t>(c));
            }
        }
    }
    auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
    return Value(sha ? sha1Hex(p, bytes.size()) : md5Hex(p, bytes.size()));
}
GMFN(md5_string_utf8) { return stringHash(S(0), false, false); }
GMFN(md5_string_unicode) { return stringHash(S(0), true, false); }
GMFN(sha1_string_utf8) { return stringHash(S(0), false, true); }
GMFN(sha1_string_unicode) { return stringHash(S(0), true, true); }
GMFN(file_exists) { return boolV(isFile(rt.files->readPath(S(0)))); }
GMFN(file_delete) { return boolV(std::remove(rt.files->writePath(S(0)).c_str()) == 0); }
GMFN(directory_exists) {
    std::string rel = rt.files->relative(S(0));
    return boolV(isDir(rt.saveDir + "/" + rel) || isDir(rt.gameDir + "/" + rel));
}
GMFN(directory_destroy) {  // only ever inside the save folder
    std::string rel = rt.files->relative(S(0));
    if (!rel.empty() && rel != "." && rel.find("..") == std::string::npos) removeTree(rt.saveDir + "/" + rel);
    return Value();
}
// zip_unzip(file, dest): extracts a zip (stored or deflated entries) into dest inside the save
// folder and returns how many files it wrote, or -1 if it isn't a readable zip.
GMFN(zip_unzip) {
    std::vector<char> zip;
    if (!readFile(rt.files->readPath(S(0)), zip)) return Value(-1.0);
    std::string z(zip.begin(), zip.end());
    auto u16 = [&](size_t o) { return o + 2 <= z.size() ? uint32_t(uint8_t(z[o]) | uint8_t(z[o + 1]) << 8) : 0u; };
    auto u32 = [&](size_t o) { return o + 4 <= z.size() ? u16(o) | u16(o + 2) << 16 : 0u; };
    size_t eocd = std::string::npos;
    for (size_t o = z.size() >= 22 ? z.size() - 22 : 0; o + 1 > 0 && o + 65558 >= z.size(); o--) {
        if (u32(o) == 0x06054b50) {
            eocd = o;
            break;
        }
        if (o == 0) break;
    }
    if (eocd == std::string::npos) return Value(-1.0);
    std::string dest = rt.saveDir + "/" + rt.files->relative(S(1));
    makeDirs(dest);
    uint32_t entries = u16(eocd + 10), cd = u32(eocd + 16);
    int written = 0;
    for (uint32_t i = 0, o = cd; i < entries && u32(o) == 0x02014b50; i++) {
        uint32_t method = u16(o + 10), csize = u32(o + 20), usize = u32(o + 24);
        uint32_t nlen = u16(o + 28), elen = u16(o + 30), clen = u16(o + 32), local = u32(o + 42);
        std::string name = o + 46 + nlen <= z.size() ? z.substr(o + 46, nlen) : "";
        o += 46 + nlen + elen + clen;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos) continue;  // stay inside dest
        if (name.back() == '/') {
            makeDirs(dest + "/" + name);
            continue;
        }
        if (u32(local) != 0x04034b50) continue;
        size_t data = local + 30 + u16(local + 26) + u16(local + 28);
        if (data + csize > z.size()) continue;
        std::string out;
        if (method == 0) {
            out = z.substr(data, csize);
        } else if (method == 8) {
            int n = 0;
            char* p = stb::zlibDecodeRaw(z.data() + data, static_cast<int>(csize), &n);
            if (!p) continue;
            out.assign(p, static_cast<size_t>(n));
            free(p);
        } else {
            continue;
        }
        (void)usize;
        std::string path = dest + "/" + name;
        makeDirs(parentDir(path));
        std::ofstream f(path, std::ios::binary);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (f) written++;
    }
    return Value(static_cast<double>(written));
}

GMFN(directory_create) {
    makeDirs(rt.saveDir + "/" + rt.files->relative(S(0)));
    return Value();
}

// ---- text files
static TextFile* text(Runner& rt, int id) {
    auto& t = rt.files->texts;
    return id >= 0 && static_cast<size_t>(id) < t.size() ? t[id].get() : nullptr;
}
static int addText(Runner& rt, std::unique_ptr<TextFile> f) {
    auto& t = rt.files->texts;
    for (size_t i = 0; i < t.size(); i++)
        if (!t[i]) {
            t[i] = std::move(f);
            return static_cast<int>(i);
        }
    t.push_back(std::move(f));
    return static_cast<int>(t.size() - 1);
}
static Value openReadText(Runner& rt, const std::string& data) {
    auto f = std::make_unique<TextFile>();
    std::string line;
    for (size_t i = 0; i <= data.size(); i++) {
        if (i == data.size() || data[i] == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (i < data.size() || !line.empty()) f->lines.push_back(line);
            line.clear();
        } else {
            line += data[i];
        }
    }
    return Value(static_cast<double>(addText(rt, std::move(f))));
}
GMFN(file_text_open_read) {
    std::string data;
    if (!readFile(rt.files->readPath(S(0)), data)) return Value(-1.0);
    return openReadText(rt, data);
}
GMFN(file_text_open_from_string) { return openReadText(rt, S(0)); }

// ---- binary files: the whole file in memory, written back on close (mode 0 read, 1 write
// (starts empty), 2 read and write).
struct BinFile {
    std::string path;
    std::vector<uint8_t> data;
    size_t pos = 0;
    bool write = false, dirty = false;
};
static std::vector<std::unique_ptr<BinFile>> gBins;
static BinFile* bin(int id) { return id >= 0 && static_cast<size_t>(id) < gBins.size() ? gBins[id].get() : nullptr; }
GMFN(file_bin_open) {
    auto f = std::make_unique<BinFile>();
    int mode = I(1);
    f->write = mode != 0;
    std::string data;
    if (mode == 0) {
        if (!readFile(rt.files->readPath(S(0)), data)) return Value(-1.0);
    } else {
        f->path = rt.files->writePath(S(0));
        if (mode == 2 && !readFile(f->path, data)) readFile(rt.files->readPath(S(0)), data);
        f->dirty = true;  // a write-mode open creates the file even if nothing is written
    }
    f->data.assign(data.begin(), data.end());
    for (size_t i = 0; i < gBins.size(); i++)
        if (!gBins[i]) {
            gBins[i] = std::move(f);
            return Value(static_cast<double>(i));
        }
    gBins.push_back(std::move(f));
    return Value(static_cast<double>(gBins.size() - 1));
}
GMFN(file_bin_close) {
    BinFile* f = bin(I(0));
    if (!f) return Value();
    if (f->write && f->dirty) {
        makeDirs(parentDir(f->path));
        std::ofstream o(f->path, std::ios::binary);
        o.write(reinterpret_cast<const char*>(f->data.data()), static_cast<std::streamsize>(f->data.size()));
    }
    gBins[I(0)].reset();
    return Value();
}
GMFN(file_bin_rewrite) {
    if (BinFile* f = bin(I(0))) {
        f->data.clear();
        f->pos = 0;
        f->dirty = true;
    }
    return Value();
}
GMFN(file_bin_size) { BinFile* f = bin(I(0)); return Value(f ? static_cast<double>(f->data.size()) : -1.0); }
GMFN(file_bin_position) { BinFile* f = bin(I(0)); return Value(f ? static_cast<double>(f->pos) : -1.0); }
GMFN(file_bin_seek) {
    if (BinFile* f = bin(I(0))) f->pos = static_cast<size_t>(std::clamp<double>(D(1), 0, static_cast<double>(f->data.size())));
    return Value();
}
GMFN(file_bin_read_byte) {
    BinFile* f = bin(I(0));
    if (!f || f->pos >= f->data.size()) return Value(0.0);
    return Value(static_cast<double>(f->data[f->pos++]));
}
GMFN(file_bin_write_byte) {
    BinFile* f = bin(I(0));
    if (!f || !f->write) return Value();
    uint8_t b = static_cast<uint8_t>(I(1) & 0xFF);
    if (f->pos < f->data.size()) f->data[f->pos] = b;
    else f->data.push_back(b);
    f->pos++;
    f->dirty = true;
    return Value();
}
GMFN(file_rename) {  // (old, new) inside the save area
    std::string from = rt.files->writePath(S(0)), to = rt.files->writePath(S(1));
    if (!isFile(from) || isFile(to)) return boolV(false);
    makeDirs(parentDir(to));
    return boolV(std::rename(from.c_str(), to.c_str()) == 0);
}
GMFN(file_attributes) { return boolV(isFile(rt.files->readPath(S(0)))); }  // (name, attr): attributes aren't modelled
static Value openWrite(Runner& rt, const std::string& gmlPath, bool append) {
    auto f = std::make_unique<TextFile>();
    f->write = true;
    f->path = rt.files->writePath(gmlPath);
    if (append) readFile(f->path, f->out);
    return Value(static_cast<double>(addText(rt, std::move(f))));
}
GMFN(file_text_open_write) { return openWrite(rt, S(0), false); }
GMFN(file_text_open_append) { return openWrite(rt, S(0), true); }
GMFN(file_text_close) {
    TextFile* f = text(rt, I(0));
    if (!f) return Value();
    if (f->write) {
        std::ofstream o(f->path, std::ios::binary);
        o << f->out;
    }
    rt.files->texts[I(0)].reset();
    return Value();
}
GMFN(file_text_read_string) {
    TextFile* f = text(rt, I(0));
    if (!f || f->line >= f->lines.size()) return Value("");
    std::string s = f->lines[f->line].substr(std::min(f->col, f->lines[f->line].size()));
    f->col = f->lines[f->line].size();
    return Value(s);
}
GMFN(file_text_read_real) {
    TextFile* f = text(rt, I(0));
    if (!f || f->line >= f->lines.size()) return Value(0.0);
    const std::string& l = f->lines[f->line];
    const char* start = l.c_str() + std::min(f->col, l.size());
    char* end = nullptr;
    double d = std::strtod(start, &end);
    f->col += static_cast<size_t>(end - start);
    return Value(d);
}
GMFN(file_text_readln) {
    TextFile* f = text(rt, I(0));
    if (!f || f->line >= f->lines.size()) return Value("");
    std::string rest = f->lines[f->line].substr(std::min(f->col, f->lines[f->line].size()));
    f->line++;
    f->col = 0;
    return Value(rest);
}
GMFN(file_text_eof) {
    TextFile* f = text(rt, I(0));
    return boolV(!f || f->line >= f->lines.size());
}
GMFN(file_text_eoln) {
    TextFile* f = text(rt, I(0));
    return boolV(!f || f->line >= f->lines.size() || f->col >= f->lines[f->line].size());
}
GMFN(file_text_write_string) {
    if (TextFile* f = text(rt, I(0))) f->out += ARG(1).toString();
    return Value();
}
GMFN(file_text_write_real) {
    if (TextFile* f = text(rt, I(0))) {
        char buf[64];
        snprintf(buf, sizeof buf, " %.6f", D(1));
        f->out += buf;
    }
    return Value();
}
GMFN(file_text_writeln) {
    if (TextFile* f = text(rt, I(0))) f->out += "\r\n";
    return Value();
}

// ---- ini
static void iniParse(IniFile& ini, const std::string& data) {
    std::string section;
    std::istringstream in(data);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line[a] == ';' || line[a] == '#') continue;
        if (line[a] == '[') {
            size_t b = line.find(']', a);
            section = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            if (!ini.sections.count(section)) ini.sectionOrder.push_back(section);
            ini.sections[section];
            continue;
        }
        size_t eq = line.find('=', a);
        if (eq == std::string::npos) continue;
        std::string k = line.substr(a, eq - a), v = line.substr(eq + 1);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        size_t vs = v.find_first_not_of(" \t");
        v = vs == std::string::npos ? "" : v.substr(vs);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (!ini.sections.count(section)) ini.sectionOrder.push_back(section);
        ini.sections[section][k] = v;
    }
}
static std::string iniSerialize(const IniFile& ini) {
    std::string out;
    for (auto& name : ini.sectionOrder) {
        auto it = ini.sections.find(name);
        if (it == ini.sections.end()) continue;
        out += "[" + name + "]\r\n";
        for (auto& [k, v] : it->second) out += k + "=\"" + v + "\"\r\n";
    }
    return out;
}
static void iniFlush(Runner& rt) {
    auto& ini = rt.files->ini;
    if (ini && ini->dirty && !ini->path.empty()) {
        std::ofstream o(ini->path, std::ios::binary);
        o << iniSerialize(*ini);
    }
}
GMFN(ini_open) {
    iniFlush(rt);
    auto ini = std::make_unique<IniFile>();
    std::string data;
    if (readFile(rt.files->readPath(S(0)), data)) iniParse(*ini, data);
    ini->path = rt.files->writePath(S(0));
    rt.files->ini = std::move(ini);
    return Value();
}
GMFN(ini_open_from_string) {
    iniFlush(rt);
    auto ini = std::make_unique<IniFile>();
    iniParse(*ini, S(0));
    rt.files->ini = std::move(ini);
    return Value();
}
GMFN(ini_close) {
    std::string s = rt.files->ini ? iniSerialize(*rt.files->ini) : "";
    iniFlush(rt);
    rt.files->ini.reset();
    return Value(s);
}
static const std::string* iniFind(Runner& rt, const std::string& sec, const std::string& key) {
    auto& ini = rt.files->ini;
    if (!ini) return nullptr;
    auto s = ini->sections.find(sec);
    if (s == ini->sections.end()) return nullptr;
    auto k = s->second.find(key);
    return k == s->second.end() ? nullptr : &k->second;
}
GMFN(ini_read_string) {
    const std::string* v = iniFind(rt, S(0), S(1));
    return v ? Value(*v) : ARG(2);
}
GMFN(ini_read_real) {
    const std::string* v = iniFind(rt, S(0), S(1));
    if (!v) return Value(D(2));
    char* end = nullptr;
    double d = std::strtod(v->c_str(), &end);
    return end == v->c_str() ? Value(D(2)) : Value(d);
}
static void iniWrite(Runner& rt, const std::string& sec, const std::string& key, const std::string& val) {
    auto& ini = rt.files->ini;
    if (!ini) return;
    if (!ini->sections.count(sec)) ini->sectionOrder.push_back(sec);
    ini->sections[sec][key] = val;
    ini->dirty = true;
}
GMFN(ini_write_string) { iniWrite(rt, S(0), S(1), ARG(2).toString()); return Value(); }
GMFN(ini_write_real) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.6f", D(2));
    iniWrite(rt, S(0), S(1), buf);
    return Value();
}
GMFN(ini_key_exists) { return boolV(iniFind(rt, S(0), S(1)) != nullptr); }
GMFN(ini_section_exists) { return boolV(rt.files->ini && rt.files->ini->sections.count(S(0))); }
GMFN(ini_key_delete) {
    auto& ini = rt.files->ini;
    if (ini) {
        auto s = ini->sections.find(S(0));
        if (s != ini->sections.end() && s->second.erase(S(1))) ini->dirty = true;
    }
    return Value();
}
GMFN(ini_section_delete) {
    auto& ini = rt.files->ini;
    if (ini && ini->sections.erase(S(0))) ini->dirty = true;
    return Value();
}

void registerFileBuiltins(Runner& rt) {
    GMREG(md5_file); GMREG(sha1_file); GMREG(md5_string_utf8); GMREG(md5_string_unicode);
    GMREG(sha1_string_utf8); GMREG(sha1_string_unicode);
    GMREG(file_text_open_from_string); GMREG(file_bin_open); GMREG(file_bin_close); GMREG(file_bin_rewrite);
    GMREG(file_bin_size); GMREG(file_bin_position); GMREG(file_bin_seek); GMREG(file_bin_read_byte);
    GMREG(file_bin_write_byte); GMREG(file_rename); GMREG(file_attributes);
    GMREG(file_exists); GMREG(file_delete); GMREG(directory_exists); GMREG(directory_create); GMREG(directory_destroy);
    GMREG(zip_unzip);
    GMREG(file_text_open_read); GMREG(file_text_open_write); GMREG(file_text_open_append); GMREG(file_text_close);
    GMREG(file_text_read_string); GMREG(file_text_read_real); GMREG(file_text_readln); GMREG(file_text_eof);
    GMREG(file_text_eoln); GMREG(file_text_write_string); GMREG(file_text_write_real); GMREG(file_text_writeln);
    GMREG(ini_open); GMREG(ini_open_from_string); GMREG(ini_close); GMREG(ini_read_string); GMREG(ini_read_real);
    GMREG(ini_write_string); GMREG(ini_write_real); GMREG(ini_key_exists); GMREG(ini_section_exists);
    GMREG(ini_key_delete); GMREG(ini_section_delete);
}

}  // namespace gm
