// The fonts built into the runner (third_party/liberation-fonts, SIL Open Font License): the
// default font for text drawn without one, and the stand-ins for system fonts named by
// font_add. Liberation Sans, Serif and Mono share their metrics with Arial, Times New Roman and
// Courier New, so text measures as it does with those.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gm {

enum class BuiltinFont { Sans, SansBold, Serif, Mono };

struct FontBytes {
    const uint8_t* data = nullptr;
    size_t size = 0;  // 0 when the build has no fonts built in (MSVC)
};

FontBytes builtinFont(BuiltinFont which);

}  // namespace gm
