// Bounds-checked little-endian reader over the in-memory data.win image.
// All offsets are absolute file offsets, which is how data.win cross-references
// everything (string pointers, pointer lists, bytecode addresses).
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gm {

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    Reader() = default;
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

    void check(uint32_t off, size_t n) const {
        if (static_cast<size_t>(off) + n > size_)
            throw FormatError("read out of bounds at 0x" + hex(off));
    }

    template <typename T> T read(uint32_t off) const {
        check(off, sizeof(T));
        T v;
        std::memcpy(&v, data_ + off, sizeof(T));  // host is little-endian (x86/arm64/Switch)
        return v;
    }

    uint8_t u8(uint32_t off) const { return read<uint8_t>(off); }
    uint16_t u16(uint32_t off) const { return read<uint16_t>(off); }
    int16_t i16(uint32_t off) const { return read<int16_t>(off); }
    uint32_t u32(uint32_t off) const { return read<uint32_t>(off); }
    int32_t i32(uint32_t off) const { return read<int32_t>(off); }
    uint64_t u64(uint32_t off) const { return read<uint64_t>(off); }
    float f32(uint32_t off) const { return read<float>(off); }
    double f64(uint32_t off) const { return read<double>(off); }
    bool b32(uint32_t off) const { return u32(off) != 0; }

    // String references point at the characters; the u32 length sits just before them.
    std::string_view str(uint32_t ptr) const {
        if (ptr == 0) return {};
        uint32_t len = u32(ptr - 4);
        check(ptr, len);
        return {reinterpret_cast<const char*>(data_ + ptr), len};
    }

    static std::string hex(uint32_t v) {
        char buf[16];
        snprintf(buf, sizeof buf, "%x", v);
        return buf;
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace gm
