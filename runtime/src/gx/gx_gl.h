// Minimal OpenGL 3.3 core loader for the GPU renderer: just the entry points and constants it uses, loaded
// through a caller-supplied getproc (SDL_GL_GetProcAddress, eglGetProcAddress...). No system GL headers needed.
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define SSNE_GLAPI __stdcall
#else
#define SSNE_GLAPI
#endif

namespace ssne::gl {

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

enum : GLenum {
    ZERO = 0, ONE = 1,
    TRIANGLES = 0x0004,
    NEVER = 0x0200, LESS = 0x0201, EQUAL = 0x0202, LEQUAL = 0x0203, GREATER = 0x0204, NOTEQUAL = 0x0205,
    GEQUAL = 0x0206, ALWAYS = 0x0207,
    SRC_COLOR = 0x0300, ONE_MINUS_SRC_COLOR = 0x0301, SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303,
    DST_ALPHA = 0x0304, ONE_MINUS_DST_ALPHA = 0x0305, DST_COLOR = 0x0306, ONE_MINUS_DST_COLOR = 0x0307,
    SRC1_ALPHA = 0x8589, ONE_MINUS_SRC1_ALPHA = 0x88FB,
    FRONT = 0x0404, BACK = 0x0405, FRONT_AND_BACK = 0x0408,
    CULL_FACE = 0x0B44, DEPTH_TEST = 0x0B71, BLEND = 0x0BE2, SCISSOR_TEST = 0x0C11, COLOR_LOGIC_OP = 0x0BF2,
    DEPTH_CLAMP = 0x864F, CLIP_DISTANCE0 = 0x3000, CLIP_DISTANCE1 = 0x3001,
    CW = 0x0900, CCW = 0x0901,
    TEXTURE_2D = 0x0DE1, UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05, UNPACK_ROW_LENGTH = 0x0CF2,
    UNSIGNED_BYTE = 0x1401, UNSIGNED_INT = 0x1405, FLOAT = 0x1406, UNSIGNED_INT_24_8 = 0x84FA,
    RGBA = 0x1908, RGBA8 = 0x8058, DEPTH_COMPONENT = 0x1902, DEPTH_COMPONENT24 = 0x81A6,
    DEPTH_STENCIL = 0x84F9, DEPTH24_STENCIL8 = 0x88F0,
    VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02,
    NEAREST = 0x2600, LINEAR = 0x2601, TEXTURE_MAG_FILTER = 0x2800, TEXTURE_MIN_FILTER = 0x2801,
    TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803, REPEAT = 0x2901, CLAMP_TO_EDGE = 0x812F, MIRRORED_REPEAT = 0x8370,
    TEXTURE_MAX_LEVEL = 0x813D,
    NEAREST_MIPMAP_NEAREST = 0x2700, LINEAR_MIPMAP_NEAREST = 0x2701, NEAREST_MIPMAP_LINEAR = 0x2702,
    LINEAR_MIPMAP_LINEAR = 0x2703,
    TEXTURE0 = 0x84C0,
    COLOR_BUFFER_BIT = 0x4000, DEPTH_BUFFER_BIT = 0x0100,
    FRAMEBUFFER = 0x8D40, READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9, COLOR_ATTACHMENT0 = 0x8CE0,
    DEPTH_ATTACHMENT = 0x8D00, DEPTH_STENCIL_ATTACHMENT = 0x821A, FRAMEBUFFER_COMPLETE = 0x8CD5,
    ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893, STREAM_DRAW = 0x88E0, DYNAMIC_DRAW = 0x88E8,
    FRAGMENT_SHADER = 0x8B30, VERTEX_SHADER = 0x8B31, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82,
    INFO_LOG_LENGTH = 0x8B84,
    FUNC_ADD = 0x8006, FUNC_REVERSE_SUBTRACT = 0x800B,
    CLEAR = 0x1500, AND = 0x1501, AND_REVERSE = 0x1502, COPY = 0x1503, AND_INVERTED = 0x1504, NOOP = 0x1505,
    XOR = 0x1506, OR = 0x1507, NOR = 0x1508, EQUIV = 0x1509, INVERT = 0x150A, OR_REVERSE = 0x150B,
    COPY_INVERTED = 0x150C, OR_INVERTED = 0x150D, NAND = 0x150E, SET = 0x150F,
    COLOR_ATTACHMENT1 = 0x8CE1,
    TEXTURE_MAX_ANISOTROPY = 0x84FE, MAX_TEXTURE_MAX_ANISOTROPY = 0x84FF,
    TEXTURE_2D_MULTISAMPLE = 0x9100, MAX_SAMPLES = 0x8D57, MULTISAMPLE = 0x809D,
    TIME_ELAPSED = 0x88BF, TIMESTAMP = 0x8E28, QUERY_RESULT = 0x8866, QUERY_RESULT_AVAILABLE = 0x8867,
    PROGRAM_BINARY_RETRIEVABLE_HINT = 0x8257, PROGRAM_BINARY_LENGTH = 0x8741, NUM_PROGRAM_BINARY_FORMATS = 0x87FE,
    EXTENSIONS = 0x1F03, NUM_EXTENSIONS = 0x821D,
    COMPLETION_STATUS = 0x91B1,  // KHR / ARB_parallel_shader_compile
};

#define SSNE_GL_FUNCS(X)                                                                                        \
    X(const GLubyte*, GetString, (GLenum name))                                                                 \
    X(GLenum, GetError, ())                                                                                     \
    X(void, Enable, (GLenum cap))                                                                               \
    X(void, Disable, (GLenum cap))                                                                              \
    X(void, Viewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                                 \
    X(void, Scissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                                  \
    X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                           \
    X(void, ClearDepth, (GLdouble d))                                                                           \
    X(void, Clear, (GLbitfield mask))                                                                           \
    X(void, ColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                                    \
    X(void, DepthMask, (GLboolean flag))                                                                        \
    X(void, DepthFunc, (GLenum func))                                                                           \
    X(void, DepthRange, (GLdouble n, GLdouble f))                                                               \
    X(void, BlendFuncSeparate, (GLenum sr, GLenum dr, GLenum sa, GLenum da))                                    \
    X(void, BlendEquationSeparate, (GLenum rgb, GLenum alpha))                                                  \
    X(void, LogicOp, (GLenum op))                                                                               \
    X(void, CullFace, (GLenum mode))                                                                            \
    X(void, FrontFace, (GLenum mode))                                                                           \
    X(void, GenTextures, (GLsizei n, GLuint* t))                                                                \
    X(void, DeleteTextures, (GLsizei n, const GLuint* t))                                                       \
    X(void, BindTexture, (GLenum target, GLuint t))                                                             \
    X(void, TexImage2D, (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,           \
                         GLenum fmt, GLenum type, const void* data))                                            \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param))                                          \
    X(void, ActiveTexture, (GLenum unit))                                                                       \
    X(void, PixelStorei, (GLenum pname, GLint param))                                                           \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* data))          \
    X(void, GenFramebuffers, (GLsizei n, GLuint* f))                                                            \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint* f))                                                   \
    X(void, BindFramebuffer, (GLenum target, GLuint f))                                                         \
    X(void, FramebufferTexture2D, (GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level))       \
    X(GLenum, CheckFramebufferStatus, (GLenum target))                                                          \
    X(void, BlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1,     \
                              GLint dy1, GLbitfield mask, GLenum filter))                                       \
    X(GLuint, CreateShader, (GLenum type))                                                                      \
    X(void, ShaderSource, (GLuint s, GLsizei n, const GLchar* const* src, const GLint* len))                    \
    X(void, CompileShader, (GLuint s))                                                                          \
    X(void, GetShaderiv, (GLuint s, GLenum pname, GLint* v))                                                    \
    X(void, GetShaderInfoLog, (GLuint s, GLsizei max, GLsizei* len, GLchar* log))                               \
    X(GLuint, CreateProgram, ())                                                                                \
    X(void, AttachShader, (GLuint p, GLuint s))                                                                 \
    X(void, LinkProgram, (GLuint p))                                                                            \
    X(void, GetProgramiv, (GLuint p, GLenum pname, GLint* v))                                                   \
    X(void, GetProgramInfoLog, (GLuint p, GLsizei max, GLsizei* len, GLchar* log))                              \
    X(void, DeleteShader, (GLuint s))                                                                           \
    X(void, DeleteProgram, (GLuint p))                                                                          \
    X(void, UseProgram, (GLuint p))                                                                             \
    X(GLint, GetUniformLocation, (GLuint p, const GLchar* name))                                                \
    X(void, Uniform1i, (GLint loc, GLint v))                                                                    \
    X(void, Uniform1f, (GLint loc, GLfloat v))                                                                  \
    X(void, Uniform1iv, (GLint loc, GLsizei n, const GLint* v))                                                 \
    X(void, Uniform2fv, (GLint loc, GLsizei n, const GLfloat* v))                                               \
    X(void, Uniform4fv, (GLint loc, GLsizei n, const GLfloat* v))                                               \
    X(void, Uniform4iv, (GLint loc, GLsizei n, const GLint* v))                                                 \
    X(void, GenVertexArrays, (GLsizei n, GLuint* a))                                                            \
    X(void, BindVertexArray, (GLuint a))                                                                        \
    X(void, GenBuffers, (GLsizei n, GLuint* b))                                                                 \
    X(void, BindBuffer, (GLenum target, GLuint b))                                                              \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))                       \
    X(void, VertexAttribPointer, (GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride,           \
                                  const void* ptr))                                                             \
    X(void, EnableVertexAttribArray, (GLuint i))                                                                \
    X(void, DisableVertexAttribArray, (GLuint i))                                                               \
    X(void, DrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices))                       \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                              \
    X(void, GenSamplers, (GLsizei n, GLuint* s))                                                                \
    X(void, SamplerParameteri, (GLuint s, GLenum pname, GLint param))                                           \
    X(void, BindSampler, (GLuint unit, GLuint s))                                                               \
    X(void, Finish, ())                                                                                         \
    X(void, GetIntegerv, (GLenum pname, GLint* v))                                                              \
    X(void, DrawBuffers, (GLsizei n, const GLenum* bufs))                                                       \
    X(void, GenerateMipmap, (GLenum target))

// Optional entry points: timer queries (GPU time per frame, for automatic resolution; core in 3.3 but not required)
// ARB_get_program_binary (core in 4.1, the shader cache) and KHR / ARB_parallel_shader_compile (shaders compiled on
// the driver's threads). Null when the driver doesn't have them.
#define SSNE_GL_OPTIONAL_FUNCS(X)                                                                               \
    X(void, SamplerParameterf, (GLuint s, GLenum pname, GLfloat v))                                             \
    X(void, TexImage2DMultisample, (GLenum target, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h,           \
                                    GLboolean fixed))                                                           \
    X(void, GenQueries, (GLsizei n, GLuint* q))                                                                 \
    X(void, BeginQuery, (GLenum target, GLuint q))                                                              \
    X(void, EndQuery, (GLenum target))                                                                          \
    X(void, QueryCounter, (GLuint q, GLenum target))                                                            \
    X(void, GetQueryObjectiv, (GLuint q, GLenum pname, GLint* v))                                               \
    X(void, GetQueryObjectui64v, (GLuint q, GLenum pname, uint64_t* v))                                         \
    X(void, GetProgramBinary, (GLuint p, GLsizei max, GLsizei* len, GLenum* fmt, void* bin))                    \
    X(void, ProgramBinary, (GLuint p, GLenum fmt, const void* bin, GLsizei len))                                \
    X(void, ProgramParameteri, (GLuint p, GLenum pname, GLint v))                                               \
    X(const GLubyte*, GetStringi, (GLenum name, GLuint index))                                                  \
    X(void, MaxShaderCompilerThreadsKHR, (GLuint count))                                                        \
    X(void, MaxShaderCompilerThreadsARB, (GLuint count))

#define SSNE_GL_DECLARE(ret, name, args) \
    typedef ret(SSNE_GLAPI* PFN_##name) args;  \
    extern PFN_##name name;
SSNE_GL_FUNCS(SSNE_GL_DECLARE)
SSNE_GL_OPTIONAL_FUNCS(SSNE_GL_DECLARE)
#undef SSNE_GL_DECLARE

// Returns false (and the name of the first missing function) if any entry point is missing.
bool load(void* (*getproc)(const char*), const char** missing);

} // namespace ssne::gl
