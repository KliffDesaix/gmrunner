// Texture page decoding: PNG, GameMaker's QOI variant, and bzip2-compressed QOI.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "data/DataWin.h"

namespace gm {

// Decodes a texture page into RGBA8. Returns false (and sets `error`) on failure.
// Decodes a texture page to RGBA; pages of dynamic texture groups are read from gameDir.
bool decodeTexture(const Texture& t, std::vector<uint8_t>& rgba, int& w, int& h, std::string& error,
                   const std::string& gameDir = ".");
// A texture page's size from its header alone (PNG, QOI, bzip2'd QOI), without decoding it.
bool textureSize(const Texture& t, int& w, int& h, const std::string& gameDir = ".");

}  // namespace gm
