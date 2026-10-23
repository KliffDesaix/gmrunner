// Minimal PNG writer (uncompressed deflate blocks) for screenshots and tests.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gm {

inline bool writePng(const std::string& path, const uint8_t* rgba, int w, int h) {
    auto crc = [](const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
        for (size_t i = 0; i < n; i++) {
            c ^= d[i];
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
        return c;
    };
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (w * 4 + 1));
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + static_cast<size_t>(y) * w * 4, rgba + static_cast<size_t>(y + 1) * w * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t p = 0; p < raw.size() || p == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - p);
        bool last = p + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 0xFF);
        z.push_back((n >> 8) & 0xFF);
        z.push_back(~n & 0xFF);
        z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + p, raw.begin() + p + n);
        p += n;
        if (last) break;
    }
    uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((adler >> s) & 0xFF);

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    auto be32 = [&](uint32_t v) {
        uint8_t o[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        fwrite(o, 1, 4, f);
    };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(static_cast<uint32_t>(data.size()));
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        fwrite(td.data(), 1, td.size(), f);
        be32(crc(td.data(), td.size()) ^ 0xFFFFFFFFu);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                 uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h),
                                 8, 6, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
    return true;
}

}  // namespace gm
