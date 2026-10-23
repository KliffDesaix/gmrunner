#include "util/Hash.h"

#include <cstring>
#include <vector>

namespace gm {
namespace {

uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

std::string hex(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += k[d[i] >> 4];
        s += k[d[i] & 15];
    }
    return s;
}

// Message padded to a multiple of 64 bytes with the bit length appended (little or big endian).
std::vector<uint8_t> pad(const uint8_t* data, size_t n, bool bigEndianLength) {
    std::vector<uint8_t> m(data, data + n);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    uint64_t bits = static_cast<uint64_t>(n) * 8;
    for (int i = 0; i < 8; i++) m.push_back(static_cast<uint8_t>(bits >> (bigEndianLength ? 56 - 8 * i : 8 * i)));
    return m;
}

}  // namespace

std::string md5Hex(const uint8_t* data, size_t n) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    auto m = pad(data, n, false);
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++)
            w[i] = m[off + i * 4] | m[off + i * 4 + 1] << 8 | m[off + i * 4 + 2] << 16 |
                   static_cast<uint32_t>(m[off + i * 4 + 3]) << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            uint32_t t = d;
            d = c;
            c = b;
            b = b + rotl(a + f + K[i] + w[g], R[i]);
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    uint8_t out[16];
    for (int i = 0; i < 16; i++) out[i] = static_cast<uint8_t>(h[i / 4] >> (8 * (i % 4)));
    return hex(out, 16);
}

std::string sha1Hex(const uint8_t* data, size_t n) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto m = pad(data, n, true);
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = static_cast<uint32_t>(m[off + i * 4]) << 24 | m[off + i * 4 + 1] << 16 | m[off + i * 4 + 2] << 8 |
                   m[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    uint8_t out[20];
    for (int i = 0; i < 20; i++) out[i] = static_cast<uint8_t>(h[i / 4] >> (24 - 8 * (i % 4)));
    return hex(out, 20);
}

}  // namespace gm
