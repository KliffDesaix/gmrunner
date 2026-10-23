// GameMaker's random number generator (WELL512 with GameMaker's seeding), so seeded sequences
// match the real runner and are identical on every platform the runner is built for.
// Follows YoYo's HTML5 runner (InitRandom / randReal in Function_Maths.js).
#pragma once

#include <cstdint>

namespace gm {

class GMRandom {
public:
    explicit GMRandom(int32_t s = 0) { seed(s); }

    void seed(int32_t s) {
        seed_ = s;
        uint32_t v = static_cast<uint32_t>(s);
        for (int i = 0; i < 16; i++) {
            v = static_cast<uint32_t>(static_cast<int32_t>(v * 214013u + 2531011u) >> 16) & 0x7fffffffu;
            state_[i] = static_cast<int32_t>(v);
        }
        index_ = 0;
    }
    int32_t currentSeed() const { return seed_; }

    // A value in [0, 1] (inclusive of 1, as in GameMaker).
    double next() {
        auto shl = [](int32_t x, int n) { return static_cast<int32_t>(static_cast<uint32_t>(x) << n); };
        int32_t a = state_[index_];
        int32_t c = state_[(index_ + 13) & 15];
        int32_t b = a ^ c ^ shl(a, 16) ^ shl(c, 15);
        c = state_[(index_ + 9) & 15];
        c ^= c >> 11;  // arithmetic shift, as on int32 in the reference
        a = state_[index_] = b ^ c;
        int32_t d = a ^ static_cast<int32_t>(static_cast<uint32_t>(shl(a, 5)) & 0xDA442D24u);
        index_ = (index_ + 15) & 15;
        a = state_[index_];
        state_[index_] = a ^ b ^ d ^ shl(a, 2) ^ shl(b, 18) ^ shl(c, 28);
        return static_cast<double>(state_[index_] & 0x7fffffff) / 2147483647.0;
    }

private:
    int32_t state_[16];
    int index_ = 0;
    int32_t seed_ = 0;
};

}  // namespace gm
