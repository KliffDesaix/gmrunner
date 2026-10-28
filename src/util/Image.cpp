#include "Image.h"

#include <cstring>

#include "Bzip2.h"
#include "util/Fs.h"
#include "util/Stb.h"

namespace gm {

namespace {

// Sign-extends the low `bits` bits of v (the QOI diff ops store two's complement deltas).
inline int sx(int v, int bits) {
    int m = 1 << (bits - 1);
    v &= (1 << bits) - 1;
    return (v ^ m) - m;
}

// GameMaker's QOI dialect (from before the QOI 1.0 spec): "fioq", u16 width, u16 height,
// u32 data length, then a stream of these ops:
//   00xxxxxx index   010xxxxx run 1-32   011xxxxx xxxxxxxx run 33-8224
//   10rrggbb diff    110rrrrr ggggbbbb diff   1110rrrr rgggggbb bbbaaaaa diff
//   1111rgba then the flagged channel bytes
bool decodeQoi(const uint8_t* d, size_t n, std::vector<uint8_t>& rgba, int& w, int& h) {
    if (n < 12 || std::memcmp(d, "fioq", 4) != 0) return false;
    w = d[4] | (d[5] << 8);
    h = d[6] | (d[7] << 8);
    uint32_t len = d[8] | (d[9] << 8) | (d[10] << 16) | (static_cast<uint32_t>(d[11]) << 24);
    size_t end = 12 + static_cast<size_t>(len);
    if (end > n) end = n;
    size_t pixels = static_cast<size_t>(w) * h;
    rgba.assign(pixels * 4, 0);
    uint8_t index[64 * 4] = {0};
    int r = 0, g = 0, b = 0, a = 255;
    int run = 0;
    size_t pos = 12;
    for (size_t px = 0; px < pixels; px++) {
        if (run > 0) {
            run--;
        } else if (pos < end) {
            int b1 = d[pos++];
            if ((b1 & 0xc0) == 0x00) {
                int i = (b1 & 0x3f) * 4;
                r = index[i];
                g = index[i + 1];
                b = index[i + 2];
                a = index[i + 3];
            } else if ((b1 & 0xe0) == 0x40) {
                run = b1 & 0x1f;
            } else if ((b1 & 0xe0) == 0x60) {
                int b2 = pos < end ? d[pos++] : 0;
                run = (((b1 & 0x1f) << 8) | b2) + 32;
            } else if ((b1 & 0xc0) == 0x80) {
                r += sx(b1 >> 4, 2);
                g += sx(b1 >> 2, 2);
                b += sx(b1, 2);
            } else if ((b1 & 0xe0) == 0xc0) {
                int b2 = pos < end ? d[pos++] : 0;
                r += sx(b1, 5);
                g += sx(b2 >> 4, 4);
                b += sx(b2, 4);
            } else if ((b1 & 0xf0) == 0xe0) {
                int b2 = pos < end ? d[pos++] : 0, b3 = pos < end ? d[pos++] : 0;
                int merged = (b1 << 16) | (b2 << 8) | b3;
                r += sx(merged >> 15, 5);
                g += sx(merged >> 10, 5);
                b += sx(merged >> 5, 5);
                a += sx(merged, 5);
            } else {  // 1111rgba
                if (b1 & 8) r = pos < end ? d[pos++] : 0;
                if (b1 & 4) g = pos < end ? d[pos++] : 0;
                if (b1 & 2) b = pos < end ? d[pos++] : 0;
                if (b1 & 1) a = pos < end ? d[pos++] : 0;
            }
            r &= 0xff;
            g &= 0xff;
            b &= 0xff;
            a &= 0xff;
            int hidx = ((r ^ g ^ b ^ a) & 63) * 4;
            index[hidx] = static_cast<uint8_t>(r);
            index[hidx + 1] = static_cast<uint8_t>(g);
            index[hidx + 2] = static_cast<uint8_t>(b);
            index[hidx + 3] = static_cast<uint8_t>(a);
        }
        uint8_t* o = &rgba[px * 4];
        o[0] = static_cast<uint8_t>(r);
        o[1] = static_cast<uint8_t>(g);
        o[2] = static_cast<uint8_t>(b);
        o[3] = static_cast<uint8_t>(a);
    }
    return true;
}

}  // namespace

static bool headerSize(const uint8_t* d, size_t n, int& w, int& h) {
    if (n >= 24 && std::memcmp(d, "\x89PNG", 4) == 0) {  // IHDR: big-endian width, height
        w = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
        h = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
        return w > 0 && h > 0;
    }
    if (n >= 8 && (std::memcmp(d, "fioq", 4) == 0 || std::memcmp(d, "2zoq", 4) == 0)) {
        w = d[4] | (d[5] << 8);
        h = d[6] | (d[7] << 8);
        return w > 0 && h > 0;
    }
    return false;
}

bool textureSize(const Texture& t, int& w, int& h, const std::string& gameDir) {
    if (t.data) return headerSize(t.data, t.size, w, h);
    std::vector<uint8_t> head;
    return !t.externalFile.empty() && readFile(gameDir + "/" + t.externalFile, head, 0, 24) &&
           headerSize(head.data(), head.size(), w, h);
}

bool decodeTexture(const Texture& t, std::vector<uint8_t>& rgba, int& w, int& h, std::string& error,
                   const std::string& gameDir) {
    if (!t.data) {
        // A dynamic texture group's page: the same formats, in a file of its own.
        std::vector<uint8_t> bytes;
        if (t.externalFile.empty() || !readFile(gameDir + "/" + t.externalFile, bytes) || bytes.size() < 12) {
            error = t.externalFile.empty() ? "texture is stored outside data.win" : "missing " + t.externalFile;
            return false;
        }
        Texture f = t;
        f.data = bytes.data();
        f.size = bytes.size();
        f.format = std::memcmp(bytes.data(), "fioq", 4) == 0   ? TextureFormat::Qoi
                   : std::memcmp(bytes.data(), "2zoq", 4) == 0 ? TextureFormat::Bz2Qoi
                                                               : TextureFormat::Png;
        return decodeTexture(f, rgba, w, h, error, gameDir);
    }
    switch (t.format) {
        case TextureFormat::Png: {
            int n;
            uint8_t* px = stb::loadImage(t.data, static_cast<int>(t.size), &w, &h, &n, 4);
            if (!px) {
                error = stb::failureReason();
                return false;
            }
            rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
            stb::freeImage(px);
            return true;
        }
        case TextureFormat::Qoi:
            if (decodeQoi(t.data, t.size, rgba, w, h)) return true;
            error = "bad QOI data";
            return false;
        case TextureFormat::Bz2Qoi: {
            // "2zoq", u16 width, u16 height, [u32 uncompressed size (2022.5+)], bzip2 stream.
            size_t start = std::memcmp(t.data + 8, "BZh", 3) == 0 ? 8 : 12;
            size_t hint = start == 12 ? (t.data[8] | (t.data[9] << 8) | (t.data[10] << 16) |
                                         (static_cast<size_t>(t.data[11]) << 24)) : 0;
            std::vector<uint8_t> qoi;
            if (t.size <= start || !bzip2Decompress(t.data + start, t.size - start, qoi, hint)) {
                error = "bad bzip2 data";
                return false;
            }
            if (decodeQoi(qoi.data(), qoi.size(), rgba, w, h)) return true;
            error = "bad QOI data inside bzip2 stream";
            return false;
        }
        default:
            error = "unknown texture format";
            return false;
    }
}

}  // namespace gm
