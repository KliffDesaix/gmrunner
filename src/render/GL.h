// Minimal OpenGL loader: the GL 2.1 / GLES 2.0 subset the renderer uses, loaded through a
// caller-supplied GetProcAddress (SDL_GL_GetProcAddress on desktop and Switch).
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define GM_GLAPI __stdcall
#else
#define GM_GLAPI
#endif

namespace gl {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLboolean = unsigned char;
using GLchar = char;
using GLbitfield = unsigned int;
using GLsizeiptr = ptrdiff_t;
using GLintptr = ptrdiff_t;

constexpr GLenum NO_ERROR_ = 0, FALSE_ = 0, TRUE_ = 1;
constexpr GLenum TRIANGLES = 0x0004, LINES = 0x0001, POINTS = 0x0000;
constexpr GLenum BLEND = 0x0BE2, SCISSOR_TEST = 0x0C11, DEPTH_TEST = 0x0B71, CULL_FACE = 0x0B44;
constexpr GLenum ZERO = 0, ONE = 1, SRC_COLOR = 0x0300, ONE_MINUS_SRC_COLOR = 0x0301, SRC_ALPHA = 0x0302,
                 ONE_MINUS_SRC_ALPHA = 0x0303, DST_ALPHA = 0x0304, ONE_MINUS_DST_ALPHA = 0x0305,
                 DST_COLOR = 0x0306, ONE_MINUS_DST_COLOR = 0x0307, SRC_ALPHA_SATURATE = 0x0308;
constexpr GLenum FUNC_ADD = 0x8006, FUNC_SUBTRACT = 0x800A, FUNC_REVERSE_SUBTRACT = 0x800B, MAX_ = 0x8008;
constexpr GLenum COLOR_BUFFER_BIT = 0x4000;
constexpr GLenum TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0, RGBA = 0x1908, UNSIGNED_BYTE = 0x1401, FLOAT = 0x1406;
constexpr GLenum TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800, TEXTURE_WRAP_S = 0x2802,
                 TEXTURE_WRAP_T = 0x2803, NEAREST = 0x2600, LINEAR = 0x2601, CLAMP_TO_EDGE = 0x812F, REPEAT = 0x2901;
constexpr GLenum UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05;
constexpr GLenum ARRAY_BUFFER = 0x8892, STREAM_DRAW = 0x88E0, DYNAMIC_DRAW = 0x88E8;
constexpr GLenum VERTEX_SHADER = 0x8B31, FRAGMENT_SHADER = 0x8B30, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82,
                 INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0, FRAMEBUFFER_COMPLETE = 0x8CD5,
                 FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum RENDERBUFFER = 0x8D41, DEPTH_COMPONENT16 = 0x81A5, DEPTH_ATTACHMENT = 0x8D00,
                 DEPTH_BUFFER_BIT = 0x0100;
constexpr GLenum DEPTH24_STENCIL8 = 0x88F0, STENCIL_ATTACHMENT = 0x8D20, STENCIL_BUFFER_BIT = 0x0400,
                 STENCIL_TEST = 0x0B90, KEEP = 0x1E00, REPLACE = 0x1E01, INCR = 0x1E02, DECR = 0x1E03,
                 INVERT = 0x150A, INCR_WRAP = 0x8507, DECR_WRAP = 0x8508;
constexpr GLenum NEVER = 0x0200, LESS = 0x0201, EQUAL = 0x0202, LEQUAL = 0x0203, GREATER = 0x0204, NOTEQUAL = 0x0205,
                 GEQUAL = 0x0206, ALWAYS = 0x0207;
constexpr GLenum FRONT = 0x0404, BACK = 0x0405, CW = 0x0900, CCW = 0x0901;
constexpr GLenum LINE_STRIP = 0x0003, TRIANGLE_STRIP = 0x0005, TRIANGLE_FAN = 0x0006;
constexpr GLenum BYTE_ = 0x1400, SHORT_ = 0x1402, UNSIGNED_SHORT = 0x1403, INT_ = 0x1404;
constexpr GLenum VERSION = 0x1F02, RENDERER = 0x1F01, EXTENSIONS = 0x1F03, SHADING_LANGUAGE_VERSION = 0x8B8C;

#define GM_GL_FUNCS(X)                                                                                      \
    X(GLenum, GetError, (void))                                                                             \
    X(const unsigned char*, GetString, (GLenum))                                                            \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                                     \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                                      \
    X(void, Enable, (GLenum))                                                                               \
    X(void, Disable, (GLenum))                                                                              \
    X(void, BlendFunc, (GLenum, GLenum))                                                                    \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                            \
    X(void, BlendEquation, (GLenum))                                                                        \
    X(void, BlendEquationSeparate, (GLenum, GLenum))                                                        \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                               \
    X(void, Clear, (GLbitfield))                                                                            \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                                        \
    X(void, GenTextures, (GLsizei, GLuint*))                                                                \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                                       \
    X(void, BindTexture, (GLenum, GLuint))                                                                  \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))       \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                                         \
    X(void, PixelStorei, (GLenum, GLint))                                                                   \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                            \
    X(void, ActiveTexture, (GLenum))                                                                        \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                                 \
    X(void, BindBuffer, (GLenum, GLuint))                                                                   \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                          \
    X(GLuint, CreateShader, (GLenum))                                                                       \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                            \
    X(void, CompileShader, (GLuint))                                                                        \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                          \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                         \
    X(void, DeleteShader, (GLuint))                                                                         \
    X(GLuint, CreateProgram, (void))                                                                        \
    X(void, AttachShader, (GLuint, GLuint))                                                                 \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar*))                                            \
    X(void, LinkProgram, (GLuint))                                                                          \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                         \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                        \
    X(void, UseProgram, (GLuint))                                                                           \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                                   \
    X(void, GetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*))                \
    X(void, Uniform1i, (GLint, GLint))                                                                      \
    X(void, Uniform1iv, (GLint, GLsizei, const GLint*))                                                     \
    X(void, Uniform2iv, (GLint, GLsizei, const GLint*))                                                     \
    X(void, Uniform3iv, (GLint, GLsizei, const GLint*))                                                     \
    X(void, Uniform4iv, (GLint, GLsizei, const GLint*))                                                     \
    X(void, Uniform1f, (GLint, GLfloat))                                                                    \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                         \
    X(void, Uniform1fv, (GLint, GLsizei, const GLfloat*))                                                   \
    X(void, Uniform2fv, (GLint, GLsizei, const GLfloat*))                                                   \
    X(void, Uniform3fv, (GLint, GLsizei, const GLfloat*))                                                   \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat*))                                                   \
    X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))                                  \
    X(void, EnableVertexAttribArray, (GLuint))                                                              \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                  \
    X(void, DisableVertexAttribArray, (GLuint))                                                             \
    X(void, VertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat))                                   \
    X(GLint, GetAttribLocation, (GLuint, const GLchar*))                                                    \
    X(void, DepthFunc, (GLenum))                                                                            \
    X(void, DepthMask, (GLboolean))                                                                         \
    X(void, StencilFunc, (GLenum, GLint, GLuint))                                                           \
    X(void, StencilOp, (GLenum, GLenum, GLenum))                                                            \
    X(void, StencilMask, (GLuint))                                                                          \
    X(void, ClearStencil, (GLint))                                                                          \
    X(void, CullFace, (GLenum))                                                                             \
    X(void, FrontFace, (GLenum))                                                                            \
    X(void, GenRenderbuffers, (GLsizei, GLuint*))                                                           \
    X(void, DeleteRenderbuffers, (GLsizei, const GLuint*))                                                  \
    X(void, BindRenderbuffer, (GLenum, GLuint))                                                             \
    X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                                        \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                                      \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                                                           \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                            \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                                   \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                              \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                  \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                             \
    X(void, GetIntegerv, (GLenum, GLint*))

#define GM_GL_DECLARE(ret, name, args) \
    using PFN_##name = ret(GM_GLAPI*) args;  \
    extern PFN_##name name;
GM_GL_FUNCS(GM_GL_DECLARE)
#undef GM_GL_DECLARE

// Returns the name of the first missing function, or nullptr on success.
const char* load(void* (*getProc)(const char*));

}  // namespace gl
