// The runner's stb_image, compiled privately in Graphics.cpp (STB_IMAGE_STATIC) so the runner can
// share a program with libraries that bring their own copy (Borealis on the Switch).
#pragma once

#include <cstdint>

namespace gm::stb {

uint8_t* loadImage(const uint8_t* data, int size, int* w, int* h, int* comp, int reqComp);
const char* failureReason();
void freeImage(void* pixels);
char* zlibDecode(const char* data, int len, int* outLen);     // zlib stream; free() the result
char* zlibDecodeRaw(const char* data, int len, int* outLen);  // raw deflate; free() the result

}  // namespace gm::stb
