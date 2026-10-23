// 3D and custom geometry: vertex formats and buffers (vertex_*), matrices (matrix_*), the
// camera matrices, depth / culling state, lights and extra shader texture stages. Follows
// YoYo's runner (yyVBufferBuilder.js, Function_D3D.js, Matrix.js).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "render/GL.h"
#include "runtime/Builtins.h"
#include "runtime/Graphics.h"

namespace gm {

#define G (*rt.gfx)

namespace {

enum { VT_FLOAT1 = 1, VT_FLOAT2, VT_FLOAT3, VT_FLOAT4, VT_COLOUR, VT_UBYTE4 };
enum { VU_POSITION = 1, VU_COLOUR, VU_NORMAL, VU_TEXCOORD };

struct VFElement {
    int type, usage, offset;
    uint32_t bit;
};
struct VFormat {
    bool live = true;
    std::vector<VFElement> elements;
    int size = 0;
    uint32_t mask = 0;
};
struct VBuffer {
    bool live = true;
    int format = -1;
    std::vector<uint8_t> data;
    int count = 0;           // complete vertices
    size_t current = 0;      // byte offset of the vertex being written
    uint32_t currentMask = 0;
    bool frozen = false;
};

std::vector<VFormat> g_formats;
VFormat g_building;
bool g_inFormat = false;
std::vector<VBuffer> g_vbuffers;
Renderer::Lights g_lights;

int typeSize(int t) { return t == VT_FLOAT2 ? 8 : t == VT_FLOAT3 ? 12 : t == VT_FLOAT4 ? 16 : 4; }

VFormat* format(int id) {
    return id >= 0 && static_cast<size_t>(id) < g_formats.size() && g_formats[id].live ? &g_formats[id] : nullptr;
}
VBuffer* vbuf(int id) {
    return id >= 0 && static_cast<size_t>(id) < g_vbuffers.size() && g_vbuffers[id].live ? &g_vbuffers[id] : nullptr;
}

// The byte offset for the next element of this usage and type in the vertex being written
// (usage -1 = any); the vertex is complete once every element has been written.
long nextElement(VBuffer& b, int usage, int type) {
    VFormat* f = format(b.format);
    if (!f || b.frozen) return -1;
    for (const VFElement& e : f->elements) {
        if ((usage != -1 && e.usage != usage) || e.type != type || (e.bit & b.currentMask)) continue;
        if (b.data.size() < b.current + static_cast<size_t>(f->size)) b.data.resize(b.current + f->size);
        long off = static_cast<long>(b.current) + e.offset;
        b.currentMask |= e.bit;
        if (b.currentMask == f->mask) {
            b.currentMask = 0;
            b.count++;
            b.current += static_cast<size_t>(f->size);
        }
        return off;
    }
    return -1;
}
void putFloats(VBuffer& b, int usage, int type, std::initializer_list<double> v) {
    long off = nextElement(b, usage, type);
    if (off < 0) return;
    for (double d : v) {
        float f = static_cast<float>(d);
        std::memcpy(b.data.data() + off, &f, 4);
        off += 4;
    }
}
void putU32(VBuffer& b, int usage, int type, uint32_t v) {
    long off = nextElement(b, usage, type);
    if (off >= 0) std::memcpy(b.data.data() + off, &v, 4);  // little-endian: R, G, B, A bytes
}

std::vector<Renderer::Attr> attributes(const VFormat& f) {
    std::vector<Renderer::Attr> out;
    int counts[16] = {};
    for (const VFElement& e : f.elements) {
        Renderer::Attr a;
        a.usage = e.usage;
        a.index = e.usage >= 0 && e.usage < 16 ? counts[e.usage]++ : 0;
        a.offset = e.offset;
        if (e.type == VT_COLOUR || e.type == VT_UBYTE4) {
            a.components = 4;
            a.type = gl::UNSIGNED_BYTE;
            a.normalized = e.type == VT_COLOUR;
        } else {
            a.components = e.type;
            a.type = gl::FLOAT;
            a.normalized = false;
        }
        out.push_back(a);
    }
    return out;
}

uint32_t glMode(int prim) {
    switch (prim) {
        case 1: return gl::POINTS;
        case 2: return gl::LINES;
        case 3: return gl::LINE_STRIP;
        case 5: return gl::TRIANGLE_STRIP;
        case 6: return gl::TRIANGLE_FAN;
        default: return gl::TRIANGLES;
    }
}

// ---- matrices: 16 numbers in GameMaker's order (row vectors, translation in 12-14)
using Mat = std::array<double, 16>;

Mat identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat multiply(const Mat& a, const Mat& b) {  // apply a, then b
    Mat r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    return r;
}

bool invert(const Mat& m, Mat& out) {
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0) return false;
    for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
    return true;
}

bool matArg(const Value& v, Mat& m) {
    GMArray* a = v.arr();
    if (!a || a->rows.empty() || a->rows[0].size() < 16) return false;
    for (int i = 0; i < 16; i++) m[i] = a->rows[0][i].num();
    return true;
}

// Returns the matrix as a new array, or writes it into `dest` when one is given (GameMaker's
// optional result-array argument).
Value matResult(const Mat& m, Value* args, int argc, int destArg) {
    Value arr = argc > destArg && args[destArg].isArray() ? args[destArg] : Value::newArray();
    GMArray* a = arr.arr();
    if (a->rows.empty()) a->rows.resize(1);
    a->rows[0].resize(std::max<size_t>(a->rows[0].size(), 16));
    for (int i = 0; i < 16; i++) a->rows[0][i] = Value(m[i]);
    if (argc > destArg && args[destArg].isArray()) return Value();
    return arr;
}

Mat fromFloats(const float* f) {
    Mat m;
    for (int i = 0; i < 16; i++) m[i] = f[i];
    return m;
}
std::array<float, 16> toFloats(const Mat& m) {
    std::array<float, 16> f;
    for (int i = 0; i < 16; i++) f[i] = static_cast<float>(m[i]);
    return f;
}

std::vector<Mat> g_matStack{identity()};

}  // namespace

// ---- vertex formats
GMFN(vertex_format_begin) {
    g_building = VFormat();
    g_inFormat = true;
    return Value();
}
static void addElement(int type, int usage) {
    if (!g_inFormat) return;
    VFElement e;
    e.type = type;
    e.usage = usage;
    e.offset = g_building.size;
    e.bit = 1u << g_building.elements.size();
    g_building.size += typeSize(type);
    g_building.mask |= e.bit;
    g_building.elements.push_back(e);
}
GMFN(vertex_format_add_position) { addElement(VT_FLOAT2, VU_POSITION); return Value(); }
GMFN(vertex_format_add_position_3d) { addElement(VT_FLOAT3, VU_POSITION); return Value(); }
GMFN(vertex_format_add_colour) { addElement(VT_COLOUR, VU_COLOUR); return Value(); }
GMFN(vertex_format_add_normal) { addElement(VT_FLOAT3, VU_NORMAL); return Value(); }
GMFN(vertex_format_add_texcoord) { addElement(VT_FLOAT2, VU_TEXCOORD); return Value(); }
GMFN(vertex_format_add_custom) { addElement(I(0), I(1)); return Value(); }  // (type, usage)
GMFN(vertex_format_end) {
    if (!g_inFormat) return Value(-1.0);
    g_inFormat = false;
    // Identical formats share an id, as in YoYo's runner.
    for (size_t i = 0; i < g_formats.size(); i++) {
        const VFormat& f = g_formats[i];
        if (!f.live || f.elements.size() != g_building.elements.size()) continue;
        bool same = true;
        for (size_t k = 0; k < f.elements.size() && same; k++)
            same = f.elements[k].type == g_building.elements[k].type && f.elements[k].usage == g_building.elements[k].usage;
        if (same) return Value(static_cast<double>(i));
    }
    g_formats.push_back(g_building);
    return Value(static_cast<double>(g_formats.size() - 1));
}
GMFN(vertex_format_delete) {
    (void)rt;  // formats may be shared between identical declarations, so they stay valid
    return Value();
}
GMFN(vertex_format_get_info) {
    VFormat* f = format(I(0));
    if (!f) return Value();
    Value st = rt.vm.newStruct(nullptr);
    Instance* si = st.obj()->inst;
    rt.vm.setInstanceVar(si, rt.vm.varIdForName("stride"), Value(static_cast<double>(f->size)));
    rt.vm.setInstanceVar(si, rt.vm.varIdForName("num_elements"), Value(static_cast<double>(f->elements.size())));
    Value list = Value::newArray();
    GMArray* la = list.writableArray();
    la->rows.resize(1);
    for (const VFElement& e : f->elements) {
        Value es = rt.vm.newStruct(nullptr);
        Instance* ei = es.obj()->inst;
        rt.vm.setInstanceVar(ei, rt.vm.varIdForName("usage"), Value(static_cast<double>(e.usage)));
        rt.vm.setInstanceVar(ei, rt.vm.varIdForName("type"), Value(static_cast<double>(e.type)));
        rt.vm.setInstanceVar(ei, rt.vm.varIdForName("size"), Value(static_cast<double>(typeSize(e.type))));
        rt.vm.setInstanceVar(ei, rt.vm.varIdForName("offset"), Value(static_cast<double>(e.offset)));
        la->rows[0].push_back(es);
    }
    rt.vm.setInstanceVar(si, rt.vm.varIdForName("elements"), list);
    return st;
}

// ---- vertex buffers
static int newVBuffer() {
    for (size_t i = 0; i < g_vbuffers.size(); i++)
        if (!g_vbuffers[i].live) {
            g_vbuffers[i] = VBuffer();
            return static_cast<int>(i);
        }
    g_vbuffers.emplace_back();
    return static_cast<int>(g_vbuffers.size() - 1);
}
GMFN(vertex_create_buffer) { return Value(static_cast<double>(newVBuffer())); }
GMFN(vertex_create_buffer_ext) {
    int id = newVBuffer();
    g_vbuffers[id].data.reserve(static_cast<size_t>(std::max(0, I(0))));
    return Value(static_cast<double>(id));
}
GMFN(vertex_delete_buffer) {
    if (VBuffer* b = vbuf(I(0))) {
        *b = VBuffer();
        b->live = false;
    }
    return Value();
}
GMFN(vertex_begin) {
    if (VBuffer* b = vbuf(I(0))) {
        b->format = I(1);
        b->data.clear();
        b->count = 0;
        b->current = 0;
        b->currentMask = 0;
        b->frozen = false;
    }
    return Value();
}
GMFN(vertex_end) {
    if (VBuffer* b = vbuf(I(0))) b->data.resize(b->current);  // drop a partly written vertex
    return Value();
}
GMFN(vertex_freeze) {
    VBuffer* b = vbuf(I(0));
    if (!b || b->frozen) return Value(-1.0);
    b->frozen = true;
    return Value(0.0);
}
GMFN(vertex_position) { if (VBuffer* b = vbuf(I(0))) putFloats(*b, VU_POSITION, VT_FLOAT2, {D(1), D(2)}); return Value(); }
GMFN(vertex_position_3d) {
    if (VBuffer* b = vbuf(I(0))) putFloats(*b, VU_POSITION, VT_FLOAT3, {D(1), D(2), D(3)});
    return Value();
}
GMFN(vertex_normal) { if (VBuffer* b = vbuf(I(0))) putFloats(*b, VU_NORMAL, VT_FLOAT3, {D(1), D(2), D(3)}); return Value(); }
GMFN(vertex_texcoord) { if (VBuffer* b = vbuf(I(0))) putFloats(*b, VU_TEXCOORD, VT_FLOAT2, {D(1), D(2)}); return Value(); }
GMFN(vertex_colour) {
    if (VBuffer* b = vbuf(I(0))) {
        uint32_t a = static_cast<uint32_t>(std::max(0.0, std::min(1.0, D(2))) * 255.0) & 0xFF;
        putU32(*b, VU_COLOUR, VT_COLOUR, (a << 24) | (static_cast<uint32_t>(I(1)) & 0xFFFFFF));
    }
    return Value();
}
// vertex_argb takes (alpha << 24) | colour with a GameMaker (BGR) colour, so it stores the same
// bytes as vertex_colour; vertex_rgba takes (colour << 8) | alpha.
GMFN(vertex_argb) {
    if (VBuffer* b = vbuf(I(0))) putU32(*b, VU_COLOUR, VT_COLOUR, static_cast<uint32_t>(ARG(1).toInt64()));
    return Value();
}
GMFN(vertex_rgba) {
    if (VBuffer* b = vbuf(I(0))) {
        uint32_t v = static_cast<uint32_t>(ARG(1).toInt64());
        putU32(*b, VU_COLOUR, VT_COLOUR, (v >> 8) | ((v & 0xFF) << 24));
    }
    return Value();
}
GMFN(vertex_float1) { if (VBuffer* b = vbuf(I(0))) putFloats(*b, -1, VT_FLOAT1, {D(1)}); return Value(); }
GMFN(vertex_float2) { if (VBuffer* b = vbuf(I(0))) putFloats(*b, -1, VT_FLOAT2, {D(1), D(2)}); return Value(); }
GMFN(vertex_float3) { if (VBuffer* b = vbuf(I(0))) putFloats(*b, -1, VT_FLOAT3, {D(1), D(2), D(3)}); return Value(); }
GMFN(vertex_float4) { if (VBuffer* b = vbuf(I(0))) putFloats(*b, -1, VT_FLOAT4, {D(1), D(2), D(3), D(4)}); return Value(); }
GMFN(vertex_ubyte4) {
    if (VBuffer* b = vbuf(I(0)))
        putU32(*b, -1, VT_UBYTE4, (I(1) & 0xFF) | (I(2) & 0xFF) << 8 | (I(3) & 0xFF) << 16 | static_cast<uint32_t>(I(4) & 0xFF) << 24);
    return Value();
}
GMFN(vertex_get_number) { VBuffer* b = vbuf(I(0)); return Value(b ? static_cast<double>(b->count) : 0.0); }
GMFN(vertex_get_buffer_size) {
    VBuffer* b = vbuf(I(0));
    VFormat* f = b ? format(b->format) : nullptr;
    return Value(b && f ? static_cast<double>(b->count * f->size) : 0.0);
}
static void submit(Runner& rt, VBuffer& b, int prim, double texture, int offset, int number) {
    VFormat* f = format(b.format);
    if (!f || f->size <= 0) return;
    int count = number < 0 ? b.count : number;
    if (offset < 0) return;
    count = std::min(count, b.count - offset);
    if (count <= 0) return;
    G.drawVertexBuffer(glMode(prim), texture, b.data.data() + static_cast<size_t>(offset) * f->size, count, f->size,
                       attributes(*f));
}
GMFN(vertex_submit) {  // (buffer, primitive, texture)
    if (VBuffer* b = vbuf(I(0))) submit(rt, *b, I(1), texArg(ARG(2)), 0, -1);
    return Value();
}
GMFN(vertex_submit_ext) {  // (buffer, primitive, texture, offset, number)
    if (VBuffer* b = vbuf(I(0))) submit(rt, *b, I(1), texArg(ARG(2)), I(3), I(4));
    return Value();
}
static int vbufferFromBytes(const uint8_t* p, size_t n, int fmt) {
    int id = newVBuffer();
    VBuffer& b = g_vbuffers[id];
    b.format = fmt;
    VFormat* f = format(fmt);
    int size = f ? f->size : 0;
    size_t whole = size > 0 ? n / static_cast<size_t>(size) * size : 0;
    b.data.assign(p, p + whole);
    b.count = size > 0 ? static_cast<int>(whole / size) : 0;
    b.current = whole;
    return id;
}
GMFN(vertex_create_buffer_from_buffer) {  // (buffer, format)
    std::vector<uint8_t>* src = bufferBytes(I(0));
    if (!src) return Value(-1.0);
    return Value(static_cast<double>(vbufferFromBytes(src->data(), src->size(), I(1))));
}
GMFN(vertex_create_buffer_from_buffer_ext) {  // (buffer, format, src_offset, num_vertices)
    std::vector<uint8_t>* src = bufferBytes(I(0));
    VFormat* f = format(I(1));
    if (!src || !f) return Value(-1.0);
    size_t off = static_cast<size_t>(std::max(0, I(2)));
    if (off > src->size()) off = src->size();
    size_t n = std::min(src->size() - off, static_cast<size_t>(std::max(0, I(3))) * f->size);
    return Value(static_cast<double>(vbufferFromBytes(src->data() + off, n, I(1))));
}
GMFN(vertex_update_buffer_from_buffer) {  // (vbuff, dest_offset, buffer, [src_offset], [size])
    VBuffer* b = vbuf(I(0));
    std::vector<uint8_t>* src = bufferBytes(I(2));
    if (!b || !src || b->frozen) return Value();
    VFormat* f = format(b->format);
    size_t soff = argc > 3 ? static_cast<size_t>(std::max(0, I(3))) : 0;
    if (soff > src->size()) return Value();
    size_t n = argc > 4 && I(4) >= 0 ? std::min(src->size() - soff, static_cast<size_t>(I(4))) : src->size() - soff;
    size_t dst = static_cast<size_t>(std::max(0, I(1)));
    if (b->data.size() < dst + n) b->data.resize(dst + n);
    std::memcpy(b->data.data() + dst, src->data() + soff, n);
    if (f && f->size > 0) {
        b->count = std::max(b->count, static_cast<int>((dst + n) / f->size));
        b->current = static_cast<size_t>(b->count) * f->size;
    }
    return Value();
}
GMFN(vertex_update_buffer_from_vertex) {  // (dest vbuff, dest_vert, src vbuff, [src_vert], [count])
    VBuffer* d = vbuf(I(0));
    VBuffer* sb = vbuf(I(2));
    if (!d || !sb || d->frozen) return Value();
    VFormat* f = format(sb->format);
    if (!f || f->size <= 0) return Value();
    int sv = argc > 3 ? std::max(0, I(3)) : 0;
    int n = argc > 4 && I(4) >= 0 ? std::min(I(4), sb->count - sv) : sb->count - sv;
    if (n <= 0) return Value();
    size_t dst = static_cast<size_t>(std::max(0, I(1))) * f->size;
    if (d->data.size() < dst + static_cast<size_t>(n) * f->size) d->data.resize(dst + static_cast<size_t>(n) * f->size);
    std::memcpy(d->data.data() + dst, sb->data.data() + static_cast<size_t>(sv) * f->size, static_cast<size_t>(n) * f->size);
    d->format = sb->format;
    d->count = std::max(d->count, static_cast<int>(dst / f->size) + n);
    d->current = static_cast<size_t>(d->count) * f->size;
    return Value();
}
GMFN(buffer_create_from_vertex_buffer) {  // (vbuff, type, alignment)
    VBuffer* b = vbuf(I(0));
    if (!b) return Value(-1.0);
    VFormat* f = format(b->format);
    size_t n = f ? static_cast<size_t>(b->count) * f->size : 0;
    return Value(static_cast<double>(bufferCreateWith(std::vector<uint8_t>(b->data.begin(), b->data.begin() + n), I(1), I(2))));
}
GMFN(buffer_create_from_vertex_buffer_ext) {  // (vbuff, type, alignment, start_vertex, num_vertices)
    VBuffer* b = vbuf(I(0));
    VFormat* f = b ? format(b->format) : nullptr;
    if (!b || !f) return Value(-1.0);
    int start = std::max(0, std::min(I(3), b->count)), n = std::max(0, std::min(I(4), b->count - start));
    auto first = b->data.begin() + static_cast<long>(start) * f->size;
    return Value(static_cast<double>(bufferCreateWith(std::vector<uint8_t>(first, first + static_cast<long>(n) * f->size), I(1), I(2))));
}

// ---- matrices
GMFN(matrix_build) {  // (x, y, z, xrot, yrot, zrot, xscale, yscale, zscale, [dest])
    const double k = M_PI / 180.0;
    double sp = std::sin(-D(3) * k), cp = std::cos(-D(3) * k);
    double sh = std::sin(-D(4) * k), ch = std::cos(-D(4) * k);
    double sr = std::sin(-D(5) * k), cr = std::cos(-D(5) * k);
    double srsp = -sr * -sp, crsp = cr * -sp;
    Mat m{};
    m[0] = (cr * ch + srsp * -sh) * D(6);
    m[4] = (-sr * cp) * D(6);
    m[8] = (cr * sh + srsp * ch) * D(6);
    m[12] = D(0);
    m[1] = (sr * ch + crsp * -sh) * D(7);
    m[5] = (cr * cp) * D(7);
    m[9] = (sr * sh + crsp * ch) * D(7);
    m[13] = D(1);
    m[2] = (cp * -sh) * D(8);
    m[6] = sp * D(8);
    m[10] = (cp * ch) * D(8);
    m[14] = D(2);
    m[15] = 1;
    return matResult(m, args, argc, 9);
}
GMFN(matrix_multiply) {
    Mat a, b;
    if (!matArg(ARG(0), a) || !matArg(ARG(1), b)) return Value();
    return matResult(multiply(a, b), args, argc, 2);
}
GMFN(matrix_inverse) {
    Mat a, r;
    if (!matArg(ARG(0), a) || !invert(a, r)) return Value();
    return matResult(r, args, argc, 1);
}
GMFN(matrix_transform_vertex) {  // (matrix, x, y, z, [w], [dest])
    Mat m;
    if (!matArg(ARG(0), m)) return Value();
    double x = D(1), y = D(2), z = D(3);
    bool hasW = argc > 4;
    double w = hasW && !ARG(4).isUndefined() ? D(4) : 1;
    double r[4] = {m[0] * x + m[4] * y + m[8] * z + m[12] * w, m[1] * x + m[5] * y + m[9] * z + m[13] * w,
                   m[2] * x + m[6] * y + m[10] * z + m[14] * w, m[3] * x + m[7] * y + m[11] * z + m[15] * w};
    int n = hasW ? 4 : 3;
    Value arr = argc > 5 && ARG(5).isArray() ? ARG(5) : Value::newArray();
    GMArray* a = arr.arr();
    if (a->rows.empty()) a->rows.resize(1);
    a->rows[0].resize(std::max<size_t>(a->rows[0].size(), n));
    for (int i = 0; i < n; i++) a->rows[0][i] = Value(r[i]);
    return argc > 5 && ARG(5).isArray() ? Value() : arr;
}
GMFN(matrix_build_projection_perspective) {  // (width, height, znear, zfar)
    double w = D(0), h = D(1), zn = D(2), zf = D(3);
    if (w == 0 || h == 0 || zn == zf) return matResult(identity(), args, argc, 4);
    Mat m{};
    m[0] = 2 * zn / w;
    m[5] = 2 * zn / h;
    m[10] = zf / (zf - zn);
    m[11] = 1;
    m[14] = -zn * zf / (zf - zn);
    return matResult(m, args, argc, 4);
}
GMFN(matrix_build_projection_perspective_fov) {  // (fov_y, aspect, znear, zfar)
    double fov = D(0), aspect = D(1), zn = D(2), zf = D(3);
    if (fov == 0 || aspect == 0 || zn == zf) return matResult(identity(), args, argc, 4);
    double ys = 1.0 / std::tan(fov * M_PI / 180.0 * 0.5);
    Mat m{};
    m[0] = ys / aspect;
    m[5] = ys;
    m[10] = zf / (zf - zn);
    m[11] = 1;
    m[14] = -zn * zf / (zf - zn);
    return matResult(m, args, argc, 4);
}
// matrix_get / matrix_set: matrix_view 0, matrix_projection 1, matrix_world 2.
GMFN(matrix_get) {
    Renderer& r = G.renderer();
    const float* f = I(0) == 0 ? r.viewMatrix() : I(0) == 1 ? r.projMatrix() : r.worldMatrix();
    return matResult(fromFloats(f), args, argc, 1);
}
GMFN(matrix_set) {
    Mat m;
    if (!matArg(ARG(1), m)) return Value();
    auto f = toFloats(m);
    Renderer& r = G.renderer();
    if (I(0) == 2) {
        r.setWorld(f.data());
    } else {
        std::array<float, 16> view, proj;
        std::copy(r.viewMatrix(), r.viewMatrix() + 16, view.begin());
        std::copy(r.projMatrix(), r.projMatrix() + 16, proj.begin());
        (I(0) == 0 ? view : proj) = f;
        G.setViewProjection(view.data(), proj.data());
    }
    return Value();
}
GMFN(matrix_stack_push) {  // ([matrix]): the new top is matrix * previous top
    if (g_matStack.size() > 50) return Value();
    Mat m;
    if (argc > 0 && matArg(ARG(0), m)) g_matStack.push_back(multiply(m, g_matStack.back()));
    else g_matStack.push_back(g_matStack.back());
    return Value();
}
GMFN(matrix_stack_pop) {
    if (g_matStack.size() > 1) g_matStack.pop_back();
    else g_matStack = {identity()};
    return Value();
}
GMFN(matrix_stack_set) {
    Mat m;
    if (matArg(ARG(0), m)) g_matStack.back() = m;
    return Value();
}
GMFN(matrix_stack_clear) { g_matStack = {identity()}; return Value(); }
GMFN(matrix_stack_top) { return matResult(g_matStack.back(), args, argc, 0); }
GMFN(matrix_stack_is_empty) { return boolV(g_matStack.size() <= 1); }

// ---- camera matrices
static Camera* camAt(Runner& rt, int id) {
    return id >= 0 && static_cast<size_t>(id) < rt.cameras.size() && rt.cameras[id].live ? &rt.cameras[id] : nullptr;
}
GMFN(camera_get_view_mat) {
    Camera* c = camAt(rt, I(0));
    if (!c) return Value();
    float v[16], p[16];
    rt.cameraMatrices(*c, v, p);
    return matResult(fromFloats(v), args, argc, 1);
}
GMFN(camera_get_proj_mat) {
    Camera* c = camAt(rt, I(0));
    if (!c) return Value();
    float v[16], p[16];
    rt.cameraMatrices(*c, v, p);
    return matResult(fromFloats(p), args, argc, 1);
}
GMFN(camera_apply) {  // makes the camera's matrices current for the rest of this target
    Camera* c = camAt(rt, I(0));
    if (!c) return Value();
    float v[16], p[16];
    rt.cameraMatrices(*c, v, p);
    G.setViewProjection(v, p);
    rt.activeCamera = I(0);
    return Value();
}

// ---- depth, culling, lights, texture stages
GMFN(gpu_set_ztestenable) { G.setZTest(B(0)); return Value(); }
GMFN(gpu_set_zwriteenable) { G.setZWrite(B(0)); return Value(); }
GMFN(gpu_set_zfunc) { G.setZFunc(I(0)); return Value(); }
GMFN(gpu_set_cullmode) { G.setCullMode(I(0)); return Value(); }
GMFN(gpu_get_ztestenable) { return boolV(G.zTest()); }
GMFN(gpu_set_depth) {
    G.drawZ = D(0);
    G.drawZSet = true;
    return Value();
}
GMFN(gpu_get_depth) { return Value(G.drawZSet ? G.drawZ : G.drawDepth); }
GMFN(gpu_set_tex_mip_enable) { G.texMipEnable = I(0); return Value(); }
GMFN(gpu_get_tex_mip_enable) { return Value(static_cast<double>(G.texMipEnable)); }
GMFN(gpu_get_zwriteenable) { return boolV(G.zWrite()); }
GMFN(gpu_get_zfunc) { return Value(static_cast<double>(G.zFunc())); }
GMFN(gpu_get_cullmode) { return Value(static_cast<double>(G.cullMode())); }
GMFN(draw_clear_depth) { G.renderer().clearDepth(); return Value(); }

GMFN(draw_set_lighting) {
    g_lights.enabled = B(0);
    G.renderer().setLights(g_lights);
    return Value();
}
GMFN(draw_get_lighting) { return boolV(g_lights.enabled); }
GMFN(draw_light_define_ambient) {
    g_lights.ambient = static_cast<uint32_t>(ARG(0).toInt64());
    G.renderer().setLights(g_lights);
    return Value();
}
GMFN(draw_light_define_direction) {  // (index, dx, dy, dz, colour)
    int i = I(0);
    if (i < 0 || i >= 8) return Value();
    double x = D(1), y = D(2), z = D(3), len = std::sqrt(x * x + y * y + z * z);
    if (len > 0) x /= len, y /= len, z /= len;
    g_lights.point[i] = false;
    g_lights.dirOrPos[i][0] = static_cast<float>(x);
    g_lights.dirOrPos[i][1] = static_cast<float>(y);
    g_lights.dirOrPos[i][2] = static_cast<float>(z);
    g_lights.dirOrPos[i][3] = 0;
    g_lights.color[i] = C(4);
    G.renderer().setLights(g_lights);
    return Value();
}
GMFN(draw_light_define_point) {  // (index, x, y, z, range, colour)
    int i = I(0);
    if (i < 0 || i >= 8) return Value();
    g_lights.point[i] = true;
    for (int k = 0; k < 4; k++) g_lights.dirOrPos[i][k] = static_cast<float>(D(k + 1));
    g_lights.color[i] = C(5);
    G.renderer().setLights(g_lights);
    return Value();
}
GMFN(draw_light_enable) {
    int i = I(0);
    if (i < 0 || i >= 8) return Value();
    g_lights.on[i] = B(1);
    G.renderer().setLights(g_lights);
    return Value();
}
GMFN(draw_light_get_ambient) { return Value(static_cast<double>(g_lights.ambient)); }
GMFN(draw_light_get) {  // [enabled, type (0 dir / 1 point), x, y, z, range, colour]
    int i = I(0);
    if (i < 0 || i >= 8) return Value();
    Value arr = Value::newArray();
    GMArray* a = arr.writableArray();
    a->rows.resize(1);
    const float* d = g_lights.dirOrPos[i];
    a->rows[0] = {Value::boolean(g_lights.on[i]), Value(g_lights.point[i] ? 1.0 : 0.0), Value(double(d[0])),
                  Value(double(d[1])), Value(double(d[2])), Value(g_lights.point[i] ? double(d[3]) : 0.0),
                  Value(static_cast<double>(g_lights.color[i]))};
    return arr;
}
GMFN(texture_set_stage) {  // (sampler index, texture)
    uint32_t tex;
    int w, h;
    if (!G.textureInfo(texArg(ARG(1)), tex, w, h)) tex = G.renderer().whiteTexture();
    G.renderer().setTextureStage(I(0), tex);
    return Value();
}
GMFN(shader_get_sampler_index) { return Value(static_cast<double>(G.renderer().samplerStage(I(0), S(1)))); }

void registerVertexBuiltins(Runner& rt) {
    GMREG(vertex_format_begin); GMREG(vertex_format_add_position); GMREG(vertex_format_add_position_3d);
    GMREG(vertex_format_add_colour); GMALIAS(vertex_format_add_color, vertex_format_add_colour);
    GMREG(vertex_format_add_normal); GMREG(vertex_format_add_texcoord);
    GMALIAS(vertex_format_add_textcoord, vertex_format_add_texcoord); GMREG(vertex_format_add_custom);
    GMREG(vertex_format_end); GMREG(vertex_format_delete); GMREG(vertex_format_get_info);
    GMREG(vertex_create_buffer); GMREG(vertex_create_buffer_ext); GMREG(vertex_delete_buffer); GMREG(vertex_begin);
    GMREG(vertex_end); GMREG(vertex_freeze); GMREG(vertex_position); GMREG(vertex_position_3d); GMREG(vertex_normal);
    GMREG(vertex_texcoord); GMREG(vertex_colour); GMALIAS(vertex_color, vertex_colour); GMREG(vertex_argb);
    GMREG(vertex_rgba); GMREG(vertex_float1); GMREG(vertex_float2); GMREG(vertex_float3); GMREG(vertex_float4);
    GMREG(vertex_ubyte4); GMREG(vertex_get_number); GMREG(vertex_get_buffer_size); GMREG(vertex_submit);
    GMREG(vertex_submit_ext); GMREG(vertex_create_buffer_from_buffer); GMREG(vertex_create_buffer_from_buffer_ext);
    GMREG(vertex_update_buffer_from_buffer); GMREG(vertex_update_buffer_from_vertex);
    GMREG(buffer_create_from_vertex_buffer); GMREG(buffer_create_from_vertex_buffer_ext);
    GMREG(matrix_build); GMREG(matrix_multiply); GMREG(matrix_inverse); GMREG(matrix_transform_vertex);
    GMREG(matrix_build_projection_perspective); GMREG(matrix_build_projection_perspective_fov); GMREG(matrix_get);
    GMREG(matrix_set); GMREG(matrix_stack_push); GMREG(matrix_stack_pop); GMREG(matrix_stack_set);
    GMREG(matrix_stack_clear); GMREG(matrix_stack_top); GMREG(matrix_stack_is_empty);
    GMREG(camera_get_view_mat); GMREG(camera_get_proj_mat); GMREG(camera_apply);
    GMREG(gpu_set_depth); GMREG(gpu_get_depth); GMREG(gpu_set_tex_mip_enable); GMREG(gpu_get_tex_mip_enable);
    GMREG(gpu_set_ztestenable); GMREG(gpu_set_zwriteenable); GMREG(gpu_set_zfunc); GMREG(gpu_set_cullmode);
    GMREG(gpu_get_ztestenable); GMREG(gpu_get_zwriteenable); GMREG(gpu_get_zfunc); GMREG(gpu_get_cullmode);
    GMREG(draw_clear_depth);
    GMREG(draw_set_lighting); GMREG(draw_get_lighting); GMREG(draw_light_define_ambient);
    GMREG(draw_light_define_direction); GMREG(draw_light_define_point); GMREG(draw_light_enable);
    GMREG(draw_light_get_ambient); GMREG(draw_light_get); GMREG(texture_set_stage); GMREG(shader_get_sampler_index);
}

}  // namespace gm
