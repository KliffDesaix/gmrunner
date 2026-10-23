#include "GL.h"

#include <string>

namespace gl {

#define GM_GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
GM_GL_FUNCS(GM_GL_DEFINE)
#undef GM_GL_DEFINE

const char* load(void* (*getProc)(const char*)) {
    static std::string missing;
#define GM_GL_LOAD(ret, name, args)                                                     \
    name = reinterpret_cast<PFN_##name>(getProc("gl" #name));                           \
    if (!name) name = reinterpret_cast<PFN_##name>(getProc("gl" #name "EXT"));          \
    if (!name) {                                                                        \
        missing = "gl" #name;                                                           \
        return missing.c_str();                                                         \
    }
    GM_GL_FUNCS(GM_GL_LOAD)
#undef GM_GL_LOAD
    return nullptr;
}

}  // namespace gl
