// Buffers: binary data with GameMaker's buffer types (fixed, grow, wrap, fast), alignment,
// typed reads/writes, base64, CRC32, zlib compression and file I/O.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>

#include "runtime/Builtins.h"
#include "util/Fs.h"
#include "util/Hash.h"
#include "runtime/Stores.h"
#include "util/Stb.h"

namespace gm {
namespace {

enum BufferType { BUF_FIXED = 0, BUF_GROW = 1, BUF_WRAP = 2, BUF_FAST = 3 };
enum DataKind {
    T_U8 = 1, T_S8 = 2, T_U16 = 3, T_S16 = 4, T_U32 = 5, T_S32 = 6, T_F16 = 7, T_F32 = 8, T_F64 = 9,
    T_BOOL = 10, T_STRING = 11, T_U64 = 12, T_TEXT = 13
};

struct Buffer {
    std::vector<uint8_t> data;
    size_t pos = 0;
    int type = BUF_FIXED;
    int align = 1;
};
std::vector<std::unique_ptr<Buffer>> g_buffers;

Buffer* buf(int id) { return id >= 0 && static_cast<size_t>(id) < g_buffers.size() ? g_buffers[id].get() : nullptr; }
int addBuffer(std::unique_ptr<Buffer> b) {
    for (size_t i = 0; i < g_buffers.size(); i++)
        if (!g_buffers[i]) {
            g_buffers[i] = std::move(b);
            return static_cast<int>(i);
        }
    g_buffers.push_back(std::move(b));
    return static_cast<int>(g_buffers.size() - 1);
}

size_t sizeOf(int type) {
    switch (type) {
        case T_U8: case T_S8: case T_BOOL: return 1;
        case T_U16: case T_S16: case T_F16: return 2;
        case T_U32: case T_S32: case T_F32: return 4;
        case T_F64: case T_U64: return 8;
        default: return 0;
    }
}

size_t aligned(const Buffer& b, size_t pos) {
    size_t a = static_cast<size_t>(std::max(1, b.align));
    return (pos + a - 1) / a * a;
}

uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, mant = x & 0x7FFFFF;
    int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00 | (((x >> 23) & 0xFF) == 0xFF && mant ? 0x200 : 0));
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000;
        return static_cast<uint16_t>(sign | (mant >> (14 - exp)));
    }
    return static_cast<uint16_t>(sign | (exp << 10) | (mant >> 13));
}
float fromHalf(uint16_t h) {
    uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    uint32_t x;
    if (exp == 0) {
        if (!mant) x = sign;
        else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                exp--;
            }
            x = sign | (exp << 23) | ((mant & 0x3FF) << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7F800000 | (mant << 13);
    } else {
        x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

// Makes room for n bytes at pos. Grow buffers grow; wrap buffers wrap pos; fixed fail.
bool reserve(Buffer& b, size_t& pos, size_t n) {
    if (pos + n <= b.data.size()) return true;
    if (b.type == BUF_GROW) {
        b.data.resize(pos + n);
        return true;
    }
    if (b.type == BUF_WRAP && !b.data.empty() && n <= b.data.size()) {
        pos %= b.data.size();
        if (pos + n <= b.data.size()) return true;
        pos = 0;
        return true;
    }
    return false;
}

// Encodes one value of `type` at pos. Returns false when it doesn't fit.
bool writeTyped(Buffer& b, size_t& pos, int type, const Value& v) {
    pos = aligned(b, pos);
    if (type == T_STRING || type == T_TEXT) {
        std::string s = v.toString();
        size_t n = s.size() + (type == T_STRING ? 1 : 0);
        if (!reserve(b, pos, n)) return false;
        std::memcpy(b.data.data() + pos, s.data(), s.size());
        if (type == T_STRING) b.data[pos + s.size()] = 0;
        pos += n;
        return true;
    }
    size_t n = sizeOf(type);
    if (!n || !reserve(b, pos, n)) return false;
    uint8_t* p = b.data.data() + pos;
    double d = v.num();
    int64_t i = v.isIntegral() ? v.toInt64() : static_cast<int64_t>(std::isfinite(d) ? d : 0);
    switch (type) {
        case T_U8: case T_S8: { uint8_t x = static_cast<uint8_t>(i); std::memcpy(p, &x, 1); break; }
        case T_BOOL: { uint8_t x = v.truthy() ? 1 : 0; std::memcpy(p, &x, 1); break; }
        case T_U16: case T_S16: { uint16_t x = static_cast<uint16_t>(i); std::memcpy(p, &x, 2); break; }
        case T_U32: case T_S32: { uint32_t x = static_cast<uint32_t>(i); std::memcpy(p, &x, 4); break; }
        case T_F16: { uint16_t x = toHalf(static_cast<float>(d)); std::memcpy(p, &x, 2); break; }
        case T_F32: { float x = static_cast<float>(d); std::memcpy(p, &x, 4); break; }
        case T_F64: std::memcpy(p, &d, 8); break;
        case T_U64: { uint64_t x = static_cast<uint64_t>(i); std::memcpy(p, &x, 8); break; }
    }
    pos += n;
    return true;
}

Value readTyped(Buffer& b, size_t& pos, int type) {
    pos = aligned(b, pos);
    if (b.type == BUF_WRAP && !b.data.empty() && pos >= b.data.size()) pos %= b.data.size();
    if (type == T_STRING || type == T_TEXT) {
        size_t start = std::min(pos, b.data.size());
        if (type == T_STRING) {
            size_t end = start;
            while (end < b.data.size() && b.data[end]) end++;
            pos = std::min(end + 1, b.data.size());
            return Value(std::string(b.data.begin() + static_cast<long>(start), b.data.begin() + static_cast<long>(end)));
        }
        pos = b.data.size();
        return Value(std::string(b.data.begin() + static_cast<long>(start), b.data.end()));
    }
    size_t n = sizeOf(type);
    uint8_t raw[8] = {};
    if (n && pos + n <= b.data.size()) std::memcpy(raw, b.data.data() + pos, n);
    pos = std::min(pos + n, b.data.size());
    switch (type) {
        case T_U8: return Value(static_cast<double>(raw[0]));
        case T_S8: return Value(static_cast<double>(static_cast<int8_t>(raw[0])));
        case T_BOOL: return Value::boolean(raw[0] != 0);
        case T_U16: { uint16_t x; std::memcpy(&x, raw, 2); return Value(static_cast<double>(x)); }
        case T_S16: { int16_t x; std::memcpy(&x, raw, 2); return Value(static_cast<double>(x)); }
        case T_U32: { uint32_t x; std::memcpy(&x, raw, 4); return Value(static_cast<double>(x)); }
        case T_S32: { int32_t x; std::memcpy(&x, raw, 4); return Value(static_cast<double>(x)); }
        case T_F16: { uint16_t x; std::memcpy(&x, raw, 2); return Value(static_cast<double>(fromHalf(x))); }
        case T_F32: { float x; std::memcpy(&x, raw, 4); return Value(static_cast<double>(x)); }
        case T_F64: { double x; std::memcpy(&x, raw, 8); return Value(x); }
        case T_U64: { uint64_t x; std::memcpy(&x, raw, 8); return Value::i64(static_cast<int64_t>(x)); }
    }
    return Value(0.0);
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64Encode(const uint8_t* p, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = static_cast<uint32_t>(p[i]) << 16;
        if (i + 1 < n) v |= static_cast<uint32_t>(p[i + 1]) << 8;
        if (i + 2 < n) v |= p[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        out += i + 2 < n ? kB64[v & 63] : '=';
    }
    return out;
}
std::vector<uint8_t> base64Decode(const std::string& s) {
    std::vector<uint8_t> out;
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        const char* f = std::strchr(kB64, c);
        if (!f || !c) continue;  // skips '=', whitespace
        v = (v << 6) | static_cast<uint32_t>(f - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(v >> bits));
        }
    }
    return out;
}

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// zlib stream using stored (uncompressed) deflate blocks: valid for any zlib reader.
std::vector<uint8_t> zlibStore(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out = {0x78, 0x01};
    size_t i = 0;
    do {
        size_t len = std::min<size_t>(65535, n - i);
        out.push_back(i + len >= n ? 1 : 0);
        out.push_back(static_cast<uint8_t>(len));
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(~len));
        out.push_back(static_cast<uint8_t>(~len >> 8));
        out.insert(out.end(), p + i, p + i + len);
        i += len;
    } while (i < n);
    uint32_t a = 1, b = 0;
    for (size_t k = 0; k < n; k++) {
        a = (a + p[k]) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(adler >> s));
    return out;
}

// Clamps [offset, offset + size) to the buffer.
void clampRange(const Buffer& b, long& off, long& size) {
    long total = static_cast<long>(b.data.size());
    off = std::max(0L, std::min(off, total));
    if (size < 0 || off + size > total) size = total - off;
}

}  // namespace

// For vertex buffers (VertexLib): a buffer's bytes, and a new buffer holding given bytes.
std::vector<uint8_t>* bufferBytes(int id) {
    Buffer* b = buf(id);
    return b ? &b->data : nullptr;
}
int bufferCreateWith(std::vector<uint8_t> data, int type, int align) {
    auto b = std::make_unique<Buffer>();
    b->data = std::move(data);
    b->type = type;
    b->align = std::max(1, align);
    return addBuffer(std::move(b));
}

GMFN(buffer_create) {
    auto b = std::make_unique<Buffer>();
    b->data.resize(static_cast<size_t>(std::max(0, I(0))));
    b->type = argc > 1 ? I(1) : BUF_GROW;
    b->align = std::max(1, argc > 2 ? I(2) : 1);
    return Value(static_cast<double>(addBuffer(std::move(b))));
}
GMFN(buffer_delete) {
    if (buf(I(0))) g_buffers[I(0)].reset();
    return Value();
}
GMFN(buffer_exists) { return boolV(buf(I(0)) != nullptr); }
GMFN(buffer_get_size) { Buffer* b = buf(I(0)); return Value(b ? static_cast<double>(b->data.size()) : 0.0); }
GMFN(buffer_get_type) { Buffer* b = buf(I(0)); return Value(b ? static_cast<double>(b->type) : -1.0); }
GMFN(buffer_get_alignment) { Buffer* b = buf(I(0)); return Value(b ? static_cast<double>(b->align) : 0.0); }
GMFN(buffer_sizeof) { return Value(static_cast<double>(sizeOf(I(0)))); }
GMFN(buffer_tell) { Buffer* b = buf(I(0)); return Value(b ? static_cast<double>(b->pos) : 0.0); }
GMFN(buffer_seek) {
    if (Buffer* b = buf(I(0))) {
        long size = static_cast<long>(b->data.size());
        long base = I(1) == 0 ? 0 : (I(1) == 1 ? static_cast<long>(b->pos) : size);
        long p = base + static_cast<long>(D(2));
        if (b->type == BUF_WRAP && size > 0) p = ((p % size) + size) % size;
        b->pos = static_cast<size_t>(std::max(0L, std::min(p, size)));
    }
    return Value();
}
GMFN(buffer_resize) {
    if (Buffer* b = buf(I(0))) {
        b->data.resize(static_cast<size_t>(std::max(0, I(1))));
        b->pos = std::min(b->pos, b->data.size());
    }
    return Value();
}
GMFN(buffer_write) {
    Buffer* b = buf(I(0));
    if (!b) return Value(-1.0);
    return Value(writeTyped(*b, b->pos, I(1), ARG(2)) ? 0.0 : -1.0);
}
GMFN(buffer_read) {
    Buffer* b = buf(I(0));
    return b ? readTyped(*b, b->pos, I(1)) : Value();
}
GMFN(buffer_poke) {
    Buffer* b = buf(I(0));
    if (!b) return Value(-1.0);
    size_t p = static_cast<size_t>(std::max(0, I(1)));
    if (b->type == BUF_GROW && sizeOf(I(2)) && p + sizeOf(I(2)) > b->data.size()) return Value(-1.0);
    return Value(writeTyped(*b, p, I(2), ARG(3)) ? 0.0 : -1.0);
}
GMFN(buffer_peek) {
    Buffer* b = buf(I(0));
    size_t p = static_cast<size_t>(std::max(0, I(1)));
    return b ? readTyped(*b, p, I(2)) : Value();
}
GMFN(buffer_fill) {  // buffer_fill(buffer, offset, type, value, size)
    Buffer* b = buf(I(0));
    if (!b) return Value();
    size_t p = static_cast<size_t>(std::max(0, I(1)));
    size_t end = std::min(b->data.size(), p + static_cast<size_t>(std::max(0, I(4))));
    size_t n = std::max<size_t>(1, sizeOf(I(2)));
    while (p + n <= end && writeTyped(*b, p, I(2), ARG(3))) {}
    return Value();
}
GMFN(buffer_copy) {  // buffer_copy(src, src_offset, size, dest, dest_offset)
    Buffer *s = buf(I(0)), *d = buf(I(3));
    if (!s || !d) return Value();
    long off = I(1), size = I(2);
    clampRange(*s, off, size);
    size_t dst = static_cast<size_t>(std::max(0, I(4)));
    if (dst + static_cast<size_t>(size) > d->data.size()) {
        if (d->type == BUF_GROW) d->data.resize(dst + static_cast<size_t>(size));
        else size = static_cast<long>(d->data.size()) - static_cast<long>(std::min(dst, d->data.size()));
    }
    if (size > 0) std::memmove(d->data.data() + dst, s->data.data() + off, static_cast<size_t>(size));
    return Value();
}
GMFN(buffer_load) {
    auto b = std::make_unique<Buffer>();
    b->type = BUF_GROW;
    if (!readFile(rt.files->readPath(S(0)), b->data)) return Value(-1.0);
    return Value(static_cast<double>(addBuffer(std::move(b))));
}
GMFN(buffer_load_ext) {  // buffer_load_ext(buffer, filename, offset)
    Buffer* b = buf(I(0));
    std::vector<uint8_t> in;
    if (!b || !readFile(rt.files->readPath(S(1)), in)) return Value();
    size_t off = static_cast<size_t>(std::max(0, I(2)));
    if (off + in.size() > b->data.size()) {
        if (b->type == BUF_GROW) b->data.resize(off + in.size());
        else in.resize(b->data.size() > off ? b->data.size() - off : 0);
    }
    std::copy(in.begin(), in.end(), b->data.begin() + static_cast<long>(off));
    return Value();
}
GMFN(buffer_load_partial) {  // (buffer, filename, src_offset, src_len, dest_offset)
    Buffer* b = buf(I(0));
    std::vector<uint8_t> in;
    if (!b || !readFile(rt.files->readPath(S(1)), in)) return Value();
    size_t src = std::min(in.size(), static_cast<size_t>(std::max(0, I(2))));
    size_t len = std::min(in.size() - src, static_cast<size_t>(std::max(0, I(3))));
    size_t dst = static_cast<size_t>(std::max(0, I(4)));
    if (dst + len > b->data.size()) {
        if (b->type == BUF_GROW) b->data.resize(dst + len);
        else len = b->data.size() > dst ? b->data.size() - dst : 0;
    }
    std::copy(in.begin() + static_cast<long>(src), in.begin() + static_cast<long>(src + len),
              b->data.begin() + static_cast<long>(dst));
    return Value();
}
GMFN(buffer_save) {
    if (Buffer* b = buf(I(0))) {
        std::ofstream f(rt.files->writePath(S(1)), std::ios::binary);
        f.write(reinterpret_cast<const char*>(b->data.data()), static_cast<std::streamsize>(b->data.size()));
    }
    return Value();
}
GMFN(buffer_save_ext) {  // buffer_save_ext(buffer, filename, offset, size)
    if (Buffer* b = buf(I(0))) {
        long off = I(2), size = I(3);
        clampRange(*b, off, size);
        std::ofstream f(rt.files->writePath(S(1)), std::ios::binary);
        f.write(reinterpret_cast<const char*>(b->data.data() + off), size);
    }
    return Value();
}
// Asynchronous saves and loads finish at once and report through the Async - Save/Load event
// (72) with async_load's "id" and "status". Inside buffer_async_group_begin/_end, files go into
// a folder named after the group and the group reports one event.
static std::string g_asyncGroup;
static bool g_inAsyncGroup = false, g_asyncGroupOk = true;
static std::string asyncPath(const std::string& name) {
    return g_inAsyncGroup && !g_asyncGroup.empty() ? g_asyncGroup + "/" + name : name;
}
static Value asyncDone(Runner& rt, bool ok) {
    if (g_inAsyncGroup) {
        g_asyncGroupOk = g_asyncGroupOk && ok;
        return Value(-1.0);
    }
    int id = rt.nextAsyncId++;
    rt.queueAsync(72, {{"id", Value(static_cast<double>(id))}, {"status", Value::boolean(ok)}});
    return Value(static_cast<double>(id));
}
GMFN(buffer_save_async) {  // (buffer, filename, offset, size)
    Buffer* b = buf(I(0));
    bool ok = false;
    if (b) {
        long off = I(2), size = I(3);
        clampRange(*b, off, size);
        std::string path = rt.files->writePath(asyncPath(S(1)));
        std::string dir = path.substr(0, path.find_last_of('/'));
        if (dir != path) makeDirs(dir);
        std::ofstream f(path, std::ios::binary);
        ok = static_cast<bool>(f.write(reinterpret_cast<const char*>(b->data.data() + off), size));
    }
    return asyncDone(rt, ok);
}
GMFN(buffer_load_async) {  // (buffer, filename, offset, size): size -1 reads the whole file
    Buffer* b = buf(I(0));
    std::vector<uint8_t> in;
    bool ok = b && readFile(rt.files->readPath(asyncPath(S(1))), in);
    if (ok) {
        if (I(3) >= 0 && static_cast<size_t>(I(3)) < in.size()) in.resize(static_cast<size_t>(I(3)));
        size_t off = static_cast<size_t>(std::max(0, I(2)));
        if (off + in.size() > b->data.size()) {
            if (b->type == BUF_GROW) b->data.resize(off + in.size());
            else in.resize(b->data.size() > off ? b->data.size() - off : 0);
        }
        std::copy(in.begin(), in.end(), b->data.begin() + static_cast<long>(off));
    }
    return asyncDone(rt, ok);
}
GMFN(buffer_async_group_begin) {
    g_asyncGroup = S(0);
    g_inAsyncGroup = true;
    g_asyncGroupOk = true;
    return Value();
}
GMFN(buffer_async_group_end) {
    g_inAsyncGroup = false;
    int id = rt.nextAsyncId++;
    rt.queueAsync(72, {{"id", Value(static_cast<double>(id))}, {"status", Value::boolean(g_asyncGroupOk)}});
    return Value(static_cast<double>(id));
}
GMFN(buffer_async_group_option) { return Value(); }  // console save-dialog options
GMFN(buffer_get_address) {  // for native extensions: the buffer's memory
    Buffer* b = buf(I(0));
    return Value::ptr(b && !b->data.empty() ? b->data.data() : nullptr);
}

GMFN(base64_encode) {  // of the string's UTF-8 bytes
    std::string str = S(0);
    return Value(base64Encode(reinterpret_cast<const uint8_t*>(str.data()), str.size()));
}
GMFN(base64_decode) {
    auto bytes = base64Decode(S(0));
    return Value(std::string(bytes.begin(), bytes.end()));
}
GMFN(buffer_base64_encode) {
    Buffer* b = buf(I(0));
    if (!b) return Value("");
    long off = I(1), size = I(2);
    clampRange(*b, off, size);
    return Value(base64Encode(b->data.data() + off, static_cast<size_t>(size)));
}
GMFN(buffer_base64_decode) {
    auto b = std::make_unique<Buffer>();
    b->type = BUF_GROW;
    b->data = base64Decode(S(0));
    return Value(static_cast<double>(addBuffer(std::move(b))));
}
GMFN(buffer_base64_decode_ext) {  // (buffer, string, offset)
    Buffer* b = buf(I(0));
    if (!b) return Value();
    std::vector<uint8_t> in = base64Decode(S(1));
    size_t off = static_cast<size_t>(std::max(0, I(2)));
    if (off + in.size() > b->data.size()) {
        if (b->type == BUF_GROW) b->data.resize(off + in.size());
        else in.resize(b->data.size() > off ? b->data.size() - off : 0);
    }
    std::copy(in.begin(), in.end(), b->data.begin() + static_cast<long>(off));
    return Value();
}
GMFN(buffer_crc32) {
    Buffer* b = buf(I(0));
    if (!b) return Value(0.0);
    long off = I(1), size = I(2);
    clampRange(*b, off, size);
    return Value(static_cast<double>(crc32(b->data.data() + off, static_cast<size_t>(size))));
}
static Value bufferHash(Value* args, int argc, bool sha) {
    Buffer* b = buf(I(0));
    if (!b) return Value("");
    long off = I(1), size = I(2);
    clampRange(*b, off, size);
    const uint8_t* p = b->data.data() + off;
    return Value(sha ? sha1Hex(p, static_cast<size_t>(size)) : md5Hex(p, static_cast<size_t>(size)));
}
GMFN(buffer_md5) { return bufferHash(args, argc, false); }
GMFN(buffer_sha1) { return bufferHash(args, argc, true); }
GMFN(buffer_compress) {  // (buffer, offset, size) -> new buffer with a zlib stream
    Buffer* b = buf(I(0));
    if (!b) return Value(-1.0);
    long off = I(1), size = I(2);
    clampRange(*b, off, size);
    auto out = std::make_unique<Buffer>();
    out->type = BUF_GROW;
    out->data = zlibStore(b->data.data() + off, static_cast<size_t>(size));
    return Value(static_cast<double>(addBuffer(std::move(out))));
}
GMFN(buffer_decompress) {
    Buffer* b = buf(I(0));
    if (!b || b->data.empty()) return Value(-1.0);
    int len = 0;
    char* raw = stb::zlibDecode(reinterpret_cast<const char*>(b->data.data()), static_cast<int>(b->data.size()), &len);
    if (!raw) return Value(-1.0);
    auto out = std::make_unique<Buffer>();
    out->type = BUF_GROW;
    out->data.assign(raw, raw + len);
    free(raw);
    return Value(static_cast<double>(addBuffer(std::move(out))));
}

void registerBufferBuiltins(Runner& rt) {
    GMREG(buffer_create); GMREG(buffer_delete); GMREG(buffer_exists); GMREG(buffer_get_size);
    GMREG(buffer_get_type); GMREG(buffer_get_alignment); GMREG(buffer_sizeof); GMREG(buffer_tell);
    GMREG(buffer_seek); GMREG(buffer_resize); GMREG(buffer_write); GMREG(buffer_read); GMREG(buffer_poke);
    GMREG(buffer_peek); GMREG(buffer_fill); GMREG(buffer_copy); GMREG(buffer_load); GMREG(buffer_load_ext);
    GMREG(buffer_load_partial); GMREG(buffer_save); GMREG(buffer_save_ext); GMREG(base64_encode); GMREG(base64_decode); GMREG(buffer_base64_encode);
    GMREG(buffer_base64_decode); GMREG(buffer_base64_decode_ext); GMREG(buffer_crc32); GMREG(buffer_save_async); GMREG(buffer_load_async); GMREG(buffer_async_group_begin);
    GMREG(buffer_async_group_end); GMREG(buffer_async_group_option); GMREG(buffer_get_address); GMREG(buffer_md5); GMREG(buffer_sha1); GMREG(buffer_compress);
    GMREG(buffer_decompress);
}

}  // namespace gm
