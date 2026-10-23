// Minimal bzip2 decompressor (GameMaker 2022+ compresses texture pages with it).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gm {

// Decompresses a complete bzip2 stream ("BZh..."). Returns false on corrupt input. Streams with
// randomised blocks need libbz2 (GMR_LIBBZ2); without it they fail.
bool bzip2Decompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t sizeHint = 0);

}  // namespace gm
