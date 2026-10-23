// The font files go into the binary with the assembler's .incbin, so every target (Switch NRO,
// PortMaster binary, Android library, iOS app) carries them without packaging them separately.
// GMR_FONT_DIR is set by the build to third_party/liberation-fonts.
#include "util/BuiltinFonts.h"

#if defined(GMR_FONT_DIR) && !defined(_MSC_VER)

#if defined(__APPLE__)
#define GMR_DATA_SECTION ".const_data"
#define GMR_SYMBOL(name) "_" #name
#else
#define GMR_DATA_SECTION ".section .rodata"
#define GMR_SYMBOL(name) #name
#endif

#define GMR_INCBIN(name, file)                                                                     \
    extern "C" const uint8_t name[];                                                               \
    extern "C" const uint8_t name##_end[];                                                         \
    __asm__(GMR_DATA_SECTION "\n.balign 16\n.globl " GMR_SYMBOL(name) "\n" GMR_SYMBOL(name) ":\n" \
            ".incbin \"" GMR_FONT_DIR "/" file "\"\n.globl " GMR_SYMBOL(name##_end) "\n"           \
            GMR_SYMBOL(name##_end) ":\n.byte 0\n.text\n")

GMR_INCBIN(gmr_font_sans, "LiberationSans-Regular.ttf");
GMR_INCBIN(gmr_font_sans_bold, "LiberationSans-Bold.ttf");
GMR_INCBIN(gmr_font_serif, "LiberationSerif-Regular.ttf");
GMR_INCBIN(gmr_font_mono, "LiberationMono-Regular.ttf");

namespace gm {

FontBytes builtinFont(BuiltinFont which) {
    auto bytes = [](const uint8_t* begin, const uint8_t* end) {
        return FontBytes{begin, static_cast<size_t>(end - begin)};
    };
    switch (which) {
        case BuiltinFont::Sans: return bytes(gmr_font_sans, gmr_font_sans_end);
        case BuiltinFont::SansBold: return bytes(gmr_font_sans_bold, gmr_font_sans_bold_end);
        case BuiltinFont::Serif: return bytes(gmr_font_serif, gmr_font_serif_end);
        case BuiltinFont::Mono: return bytes(gmr_font_mono, gmr_font_mono_end);
    }
    return {};
}

}  // namespace gm

#else

namespace gm {
FontBytes builtinFont(BuiltinFont) { return {}; }
}  // namespace gm

#endif
