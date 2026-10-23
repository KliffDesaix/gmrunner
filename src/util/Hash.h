// MD5 and SHA-1 digests as lowercase hex strings (md5_string_utf8, sha1_file, buffer_md5, ...).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gm {

std::string md5Hex(const uint8_t* data, size_t n);
std::string sha1Hex(const uint8_t* data, size_t n);

}  // namespace gm
