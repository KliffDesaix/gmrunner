#include "Bzip2.h"

#include <cstring>

#ifdef GMR_LIBBZ2
#include <bzlib.h>
#endif

namespace gm {

namespace {

class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    // False once bits past the end of the data have been consumed (they read as zeros).
    bool ok() const { return pad_ * 8 <= have_; }
    // The next `count` bits (at most 32) without consuming them.
    uint32_t peek(int count) {
        while (have_ < count) {
            uint32_t byte = 0;
            if (pos_ < n_) byte = d_[pos_++];
            else pad_++;
            buf_ = (buf_ << 8) | byte;
            have_ += 8;
        }
        return static_cast<uint32_t>((buf_ >> (have_ - count)) & ((1ull << count) - 1));
    }
    void skip(int count) { have_ -= count; }  // after peek(>= count)
    uint32_t bits(int count) {
        uint32_t v = peek(count);
        skip(count);
        return v;
    }
    uint32_t bit() { return bits(1); }

private:
    const uint8_t* d_;
    size_t n_, pos_ = 0;
    uint64_t buf_ = 0;
    int have_ = 0, pad_ = 0;
};

constexpr int kMaxGroups = 6, kMaxAlpha = 258, kMaxCodeLen = 20, kGroupSize = 50;

struct Huffman {
    int32_t limit[kMaxCodeLen + 2], base[kMaxCodeLen + 2], perm[kMaxAlpha];
    int minLen, maxLen;
    // Codes up to kFastBits long decode with one lookup: (symbol << 5) | length, or -1.
    static constexpr int kFastBits = 10;
    int16_t fast[1 << kFastBits];

    void build(const uint8_t* lengths, int alphaSize) {
        minLen = 32;
        maxLen = 0;
        for (int i = 0; i < alphaSize; i++) {
            if (lengths[i] > maxLen) maxLen = lengths[i];
            if (lengths[i] < minLen) minLen = lengths[i];
        }
        int pp = 0;
        for (int len = minLen; len <= maxLen; len++)
            for (int s = 0; s < alphaSize; s++)
                if (lengths[s] == len) perm[pp++] = s;
        int32_t count[kMaxCodeLen + 2] = {0};
        for (int i = 0; i < alphaSize; i++) count[lengths[i]]++;
        std::memset(fast, 0xff, sizeof fast);
        for (int len = minLen, c = 0, p = 0; len <= maxLen; len++, c <<= 1)
            for (int k = 0; k < count[len]; k++, c++, p++) {
                if (len > kFastBits) continue;
                int first = c << (kFastBits - len), n = 1 << (kFastBits - len);
                for (int j = 0; j < n; j++) fast[first + j] = static_cast<int16_t>(perm[p] << 5 | len);
            }
        // Canonical code: first code value and index into perm for each length.
        int32_t code = 0, index = 0;
        for (int len = 0; len <= kMaxCodeLen + 1; len++) limit[len] = -1, base[len] = 0;
        for (int len = minLen; len <= maxLen; len++) {
            base[len] = index - code;        // perm index = code + base[len]
            code += count[len];
            index += count[len];
            limit[len] = code - 1;           // largest code of this length
            code <<= 1;
        }
    }

    int decode(BitReader& br) const {
        int16_t e = fast[br.peek(kFastBits)];
        if (e >= 0) {
            br.skip(e & 31);
            return e >> 5;
        }
        int len = minLen;
        int32_t v = static_cast<int32_t>(br.bits(len));
        while (len <= maxLen) {
            if (v <= limit[len]) return perm[v + base[len]];
            v = (v << 1) | static_cast<int32_t>(br.bit());
            len++;
        }
        return -1;
    }
};

// Randomised blocks (an obsolete bzip2 feature GameMaker's compressor still emits now and then)
// flip the lowest bit of the bytes this table picks out (bzip2's randtable.c, BZ2_rNums).
const uint16_t kRandNums[512] = {
    619, 720, 127, 481, 931, 816, 813, 233, 566, 247, 985, 724, 205, 454, 863, 491,
    741, 242, 949, 214, 733, 859, 335, 708, 621, 574, 73, 654, 730, 472, 419, 436,
    278, 496, 867, 210, 399, 680, 480, 51, 878, 465, 811, 169, 869, 675, 611, 697,
    867, 561, 862, 687, 507, 283, 482, 129, 807, 591, 733, 623, 150, 238, 59, 379,
    684, 877, 625, 169, 643, 105, 170, 607, 520, 932, 727, 476, 693, 425, 174, 647,
    73, 122, 335, 530, 442, 853, 695, 249, 445, 515, 909, 545, 703, 919, 874, 474,
    882, 500, 594, 612, 641, 801, 220, 162, 819, 984, 589, 513, 495, 799, 161, 604,
    958, 533, 221, 400, 386, 867, 600, 782, 382, 596, 414, 171, 516, 375, 682, 485,
    911, 276, 98, 553, 163, 354, 666, 933, 424, 341, 533, 870, 227, 730, 475, 186,
    263, 647, 537, 686, 600, 224, 469, 68, 770, 919, 190, 373, 294, 822, 808, 206,
    184, 943, 795, 384, 383, 461, 404, 758, 839, 887, 715, 67, 618, 276, 204, 918,
    873, 777, 604, 560, 951, 160, 578, 722, 79, 804, 96, 409, 713, 940, 652, 934,
    970, 447, 318, 353, 859, 672, 112, 785, 645, 863, 803, 350, 139, 93, 354, 99,
    820, 908, 609, 772, 154, 274, 580, 184, 79, 626, 630, 742, 653, 282, 762, 623,
    680, 81, 927, 626, 789, 125, 411, 521, 938, 300, 821, 78, 343, 175, 128, 250,
    170, 774, 972, 275, 999, 639, 495, 78, 352, 126, 857, 956, 358, 619, 580, 124,
    737, 594, 701, 612, 669, 112, 134, 694, 363, 992, 809, 743, 168, 974, 944, 375,
    748, 52, 600, 747, 642, 182, 862, 81, 344, 805, 988, 739, 511, 655, 814, 334,
    249, 515, 897, 955, 664, 981, 649, 113, 974, 459, 893, 228, 433, 837, 553, 268,
    926, 240, 102, 654, 459, 51, 686, 754, 806, 760, 493, 403, 415, 394, 687, 700,
    946, 670, 656, 610, 738, 392, 760, 799, 887, 653, 978, 321, 576, 617, 626, 502,
    894, 679, 243, 440, 680, 879, 194, 572, 640, 724, 926, 56, 204, 700, 707, 151,
    457, 449, 797, 195, 791, 558, 945, 679, 297, 59, 87, 824, 713, 663, 412, 693,
    342, 606, 134, 108, 571, 364, 631, 212, 174, 643, 304, 329, 343, 97, 430, 751,
    497, 314, 983, 374, 822, 928, 140, 206, 73, 263, 980, 736, 876, 478, 430, 305,
    170, 514, 364, 692, 829, 82, 855, 953, 676, 246, 369, 970, 294, 750, 807, 827,
    150, 790, 288, 923, 804, 378, 215, 828, 592, 281, 565, 555, 710, 82, 896, 831,
    547, 261, 524, 462, 293, 465, 502, 56, 661, 821, 976, 991, 658, 869, 905, 758,
    745, 193, 768, 550, 608, 933, 378, 286, 215, 979, 792, 961, 61, 688, 793, 644,
    986, 403, 106, 366, 905, 644, 372, 567, 466, 434, 645, 210, 389, 550, 919, 135,
    780, 773, 635, 389, 707, 100, 626, 958, 165, 504, 920, 176, 193, 713, 857, 265,
    203, 50, 668, 108, 645, 990, 626, 197, 510, 357, 358, 850, 858, 364, 936, 638,
};

bool decodeBuiltin(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t sizeHint) {
    if (size < 4 || data[0] != 'B' || data[1] != 'Z' || data[2] != 'h' || data[3] < '1' || data[3] > '9')
        return false;
    const uint32_t blockMax = (data[3] - '0') * 100000u;
    BitReader br(data + 4, size - 4);
    out.clear();
    if (sizeHint) out.reserve(sizeHint);
    std::vector<uint32_t> tt(blockMax);

    for (;;) {
        uint64_t magic = (static_cast<uint64_t>(br.bits(24)) << 24) | br.bits(24);
        if (magic == 0x177245385090ull) {  // end of stream
            br.bits(32);                    // combined CRC
            return br.ok();
        }
        if (magic != 0x314159265359ull) return false;
        br.bits(32);  // block CRC
        bool randomised = br.bit();
        uint32_t origPtr = br.bits(24);

        // Symbol map.
        uint8_t seqToUnseq[256];
        int nInUse = 0;
        uint32_t used16 = br.bits(16);
        for (int i = 0; i < 16; i++)
            if (used16 & (0x8000 >> i)) {
                uint32_t bitsI = br.bits(16);
                for (int j = 0; j < 16; j++)
                    if (bitsI & (0x8000 >> j)) seqToUnseq[nInUse++] = static_cast<uint8_t>(i * 16 + j);
            }
        if (nInUse == 0) return false;
        int alphaSize = nInUse + 2;

        int nGroups = static_cast<int>(br.bits(3));
        int nSelectors = static_cast<int>(br.bits(15));
        if (nGroups < 2 || nGroups > kMaxGroups || nSelectors < 1) return false;
        std::vector<uint8_t> selectors(nSelectors);
        uint8_t mtfGroups[kMaxGroups];
        for (int i = 0; i < nGroups; i++) mtfGroups[i] = static_cast<uint8_t>(i);
        for (int i = 0; i < nSelectors; i++) {
            int j = 0;
            while (br.bit()) {
                if (++j >= nGroups) return false;
            }
            uint8_t v = mtfGroups[j];
            for (; j > 0; j--) mtfGroups[j] = mtfGroups[j - 1];
            mtfGroups[0] = v;
            selectors[i] = v;
        }

        Huffman tables[kMaxGroups];
        for (int t = 0; t < nGroups; t++) {
            uint8_t lengths[kMaxAlpha];
            int curr = static_cast<int>(br.bits(5));
            for (int i = 0; i < alphaSize; i++) {
                for (;;) {
                    if (curr < 1 || curr > kMaxCodeLen) return false;
                    if (!br.bit()) break;
                    curr += br.bit() ? -1 : 1;
                }
                lengths[i] = static_cast<uint8_t>(curr);
            }
            tables[t].build(lengths, alphaSize);
        }

        // Huffman + RUNA/RUNB + move-to-front decoding into tt (low 8 bits = byte).
        uint8_t mtf[256];
        for (int i = 0; i < 256; i++) mtf[i] = static_cast<uint8_t>(i);
        uint32_t counts[256] = {0};
        uint32_t nblock = 0;
        int eob = nInUse + 1;
        int groupLeft = 0, selIndex = 0;
        const Huffman* table = nullptr;
        uint32_t runLen = 0, runBit = 1;
        bool inRun = false;
        auto flushRun = [&]() -> bool {
            if (!inRun) return true;
            uint8_t b = seqToUnseq[mtf[0]];
            if (nblock + runLen > blockMax) return false;
            counts[b] += runLen;
            for (uint32_t k = 0; k < runLen; k++) tt[nblock++] = b;
            runLen = 0;
            runBit = 1;
            inRun = false;
            return true;
        };
        for (;;) {
            if (groupLeft == 0) {
                if (selIndex >= nSelectors) return false;
                table = &tables[selectors[selIndex++]];
                groupLeft = kGroupSize;
            }
            groupLeft--;
            int sym = table->decode(br);
            if (sym < 0 || !br.ok()) return false;
            if (sym <= 1) {  // RUNA / RUNB: bijective base-2 run length of the front symbol
                runLen += (sym + 1) * runBit;
                runBit <<= 1;
                inRun = true;
                if (runLen > blockMax) return false;
                continue;
            }
            if (!flushRun()) return false;
            if (sym == eob) break;
            int idx = sym - 1;
            uint8_t v = mtf[idx];
            std::memmove(mtf + 1, mtf, idx);
            mtf[0] = v;
            uint8_t b = seqToUnseq[v];
            if (nblock >= blockMax) return false;
            counts[b]++;
            tt[nblock++] = b;
        }
        if (origPtr >= nblock) return false;

        // Inverse Burrows-Wheeler transform.
        uint32_t cftab[256];
        uint32_t sum = 0;
        for (int i = 0; i < 256; i++) {
            cftab[i] = sum;
            sum += counts[i];
        }
        for (uint32_t i = 0; i < nblock; i++) {
            uint8_t b = static_cast<uint8_t>(tt[i] & 0xff);
            tt[cftab[b]++] |= i << 8;
        }
        uint32_t pos = tt[origPtr] >> 8;
        // Undo the initial run-length encoding: 4 equal bytes are followed by a repeat count.
        // Written through a pointer with room for one more run (at most 255 bytes) at all times.
        size_t used = out.size();
        out.resize(used + nblock + 512);
        uint8_t* o = out.data() + used;
        uint8_t* end = out.data() + out.size() - 256;
        int last = -1, same = 0;
        int randToGo = 0, randPos = 0;
        for (uint32_t i = 0; i < nblock; i++) {
            pos = tt[pos];
            uint8_t b = static_cast<uint8_t>(pos & 0xff);
            pos >>= 8;
            if (randomised) {
                if (randToGo == 0) {
                    randToGo = kRandNums[randPos];
                    randPos = (randPos + 1) % 512;
                }
                if (--randToGo == 1) b ^= 1;
            }
            if (o >= end) {
                size_t at = static_cast<size_t>(o - out.data());
                out.resize(out.size() * 2);
                o = out.data() + at;
                end = out.data() + out.size() - 256;
            }
            if (same == 4) {
                std::memset(o, last, b);
                o += b;
                same = 0;
                last = -1;
                continue;
            }
            if (b == last) same++;
            else {
                last = b;
                same = 1;
            }
            *o++ = b;
        }
        out.resize(static_cast<size_t>(o - out.data()));
    }
}

#ifdef GMR_LIBBZ2
bool decodeLibbz2(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t sizeHint) {
    bz_stream s{};
    if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK) return false;
    out.resize(sizeHint ? sizeHint : size * 4);
    s.next_in = reinterpret_cast<char*>(const_cast<uint8_t*>(data));
    s.avail_in = static_cast<unsigned>(size);
    size_t done = 0;
    int r;
    do {
        if (done == out.size()) out.resize(out.size() * 2);
        s.next_out = reinterpret_cast<char*>(out.data() + done);
        s.avail_out = static_cast<unsigned>(out.size() - done);
        r = BZ2_bzDecompress(&s);
        done = out.size() - s.avail_out;
    } while (r == BZ_OK && (s.avail_in > 0 || s.avail_out == 0));
    BZ2_bzDecompressEnd(&s);
    out.resize(done);
    return r == BZ_STREAM_END;
}
#endif

}  // namespace

bool bzip2Decompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t sizeHint) {
    if (decodeBuiltin(data, size, out, sizeHint)) return true;
#ifdef GMR_LIBBZ2
    return decodeLibbz2(data, size, out, sizeHint);
#else
    return false;
#endif
}

}  // namespace gm
