// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_impl_opengl3.cpp — standalone OpenGL 3.2+ / OpenGL ES 3.0+ rendering
// core for the Slug pipeline and Dear ImGui adapter.
//
// Lives in backends/ (invariant 7: the core stays backend-independent).

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "imslug_impl_opengl3.h"
#include "imslug.h"
#include "imslug_internal.h"
#include "shaders/imslug_shaders_opengl3.h"

#if !defined(IMSLUG_NO_IMGUI)
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#else
#include <dlfcn.h>
#endif

namespace ImSlug
{
    namespace
    {
        // --------------------------------------------------------------------
        // Private OpenGL types & constants (no system GL header required)
        // --------------------------------------------------------------------
        typedef unsigned int GLenum;
        typedef unsigned char GLboolean;
        typedef unsigned int GLbitfield;
        typedef void GLvoid;
        typedef signed char GLbyte;
        typedef short GLshort;
        typedef int GLint;
        typedef unsigned char GLubyte;
        typedef unsigned short GLushort;
        typedef unsigned int GLuint;
        typedef int GLsizei;
        typedef float GLfloat;
        typedef float GLclampf;
        typedef double GLdouble;
        typedef double GLclampd;
        typedef char GLchar;
        typedef ptrdiff_t GLintptr;
        typedef ptrdiff_t GLsizeiptr;

        static const GLboolean GL_FALSE_VAL = 0;
        static const GLboolean GL_TRUE_VAL = 1;

        static const GLenum GL_TRIANGLE_STRIP = 0x0005;
        static const GLenum GL_CULL_FACE = 0x0B44;
        static const GLenum GL_DEPTH_TEST = 0x0B71;
        static const GLenum GL_VIEWPORT = 0x0BA2;
        static const GLenum GL_BLEND = 0x0BE2;
        static const GLenum GL_SCISSOR_BOX = 0x0C10;
        static const GLenum GL_SCISSOR_TEST = 0x0C11;
        static const GLenum GL_UNPACK_ROW_LENGTH = 0x0CF2;
        static const GLenum GL_UNPACK_SKIP_ROWS = 0x0CF3;
        static const GLenum GL_UNPACK_SKIP_PIXELS = 0x0CF4;
        static const GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
        static const GLenum GL_TEXTURE_2D = 0x0DE1;
        static const GLenum GL_UNSIGNED_BYTE = 0x1401;
        static const GLenum GL_UNSIGNED_SHORT = 0x1403;
        static const GLenum GL_UNSIGNED_INT = 0x1405;
        static const GLenum GL_FLOAT = 0x1406;
        static const GLenum GL_HALF_FLOAT = 0x140B;
        static const GLenum GL_VERSION = 0x1F02;
        static const GLenum GL_NEAREST = 0x2600;
        static const GLenum GL_LINEAR = 0x2601;
        static const GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
        static const GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
        static const GLenum GL_TEXTURE_WRAP_S = 0x2802;
        static const GLenum GL_TEXTURE_WRAP_T = 0x2803;
        static const GLenum GL_FUNC_ADD = 0x8006;
        static const GLenum GL_SRC_ALPHA = 0x0302;
        static const GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
        static const GLenum GL_ONE = 1;
        static const GLenum GL_TEXTURE_BINDING_2D = 0x8069;
        static const GLenum GL_CLAMP_TO_EDGE = 0x812F;
        static const GLenum GL_MAJOR_VERSION = 0x821B;
        static const GLenum GL_MINOR_VERSION = 0x821C;
        static const GLenum GL_RG_INTEGER = 0x8228;
        static const GLenum GL_RG16UI = 0x823A;
        static const GLenum GL_TEXTURE0 = 0x84C0;
        static const GLenum GL_TEXTURE1 = 0x84C1;
        static const GLenum GL_TEXTURE2 = 0x84C2;
        static const GLenum GL_ACTIVE_TEXTURE = 0x84E0;
        static const GLenum GL_ARRAY_BUFFER = 0x8892;
        static const GLenum GL_ARRAY_BUFFER_BINDING = 0x8894;
        static const GLenum GL_RGBA16F = 0x881A;
        static const GLenum GL_STREAM_DRAW = 0x88E0;
        static const GLenum GL_STATIC_DRAW = 0x88E4;
        static const GLenum GL_DYNAMIC_DRAW = 0x88E8;
        static const GLenum GL_PIXEL_UNPACK_BUFFER = 0x88EC;
        static const GLenum GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF;
        static const GLenum GL_FRAGMENT_SHADER = 0x8B30;
        static const GLenum GL_VERTEX_SHADER = 0x8B31;
        static const GLenum GL_COMPILE_STATUS = 0x8B81;
        static const GLenum GL_LINK_STATUS = 0x8B82;
        static const GLenum GL_INFO_LOG_LENGTH = 0x8B84;
        static const GLenum GL_CURRENT_PROGRAM = 0x8B8D;
        static const GLenum GL_RGBA32UI = 0x8D70;
        static const GLenum GL_RGBA_INTEGER = 0x8D99;
        static const GLenum GL_RGBA = 0x1908;
        static const GLenum GL_VERTEX_ARRAY_BINDING = 0x85B5;
        static const GLenum GL_CLIP_ORIGIN = 0x935C;
        static const GLenum GL_UPPER_LEFT = 0x8CA2;

        // Function pointer prototypes
        typedef GLenum (*PFN_glGetError)(void);
        typedef const GLubyte* (*PFN_glGetString)(GLenum name);
        typedef void (*PFN_glGetIntegerv)(GLenum pname, GLint* data);
        typedef void (*PFN_glEnable)(GLenum cap);
        typedef void (*PFN_glDisable)(GLenum cap);
        typedef GLboolean (*PFN_glIsEnabled)(GLenum cap);
        typedef void (*PFN_glViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
        typedef void (*PFN_glScissor)(GLint x, GLint y, GLsizei width, GLsizei height);
        typedef void (*PFN_glBlendEquation)(GLenum mode);
        typedef void (*PFN_glBlendEquationSeparate)(GLenum modeRGB, GLenum modeAlpha);
        typedef void (*PFN_glBlendFuncSeparate)(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha,
                                                GLenum dfactorAlpha);
        typedef void (*PFN_glPixelStorei)(GLenum pname, GLint param);

        typedef void (*PFN_glGenTextures)(GLsizei n, GLuint* textures);
        typedef void (*PFN_glDeleteTextures)(GLsizei n, const GLuint* textures);
        typedef void (*PFN_glBindTexture)(GLenum target, GLuint texture);
        typedef void (*PFN_glActiveTexture)(GLenum texture);
        typedef void (*PFN_glTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                         GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
        typedef void (*PFN_glTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                            GLsizei height, GLenum format, GLenum type, const void* pixels);
        typedef void (*PFN_glTexParameteri)(GLenum target, GLenum pname, GLint param);
        typedef void (*PFN_glBindSampler)(GLuint unit, GLuint sampler);

        typedef void (*PFN_glGenBuffers)(GLsizei n, GLuint* buffers);
        typedef void (*PFN_glDeleteBuffers)(GLsizei n, const GLuint* buffers);
        typedef void (*PFN_glBindBuffer)(GLenum target, GLuint buffer);
        typedef void (*PFN_glBufferData)(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
        typedef void (*PFN_glBufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);

        typedef void (*PFN_glGenVertexArrays)(GLsizei n, GLuint* arrays);
        typedef void (*PFN_glDeleteVertexArrays)(GLsizei n, const GLuint* arrays);
        typedef void (*PFN_glBindVertexArray)(GLuint array);
        typedef void (*PFN_glEnableVertexAttribArray)(GLuint index);
        typedef void (*PFN_glDisableVertexAttribArray)(GLuint index);
        typedef void (*PFN_glVertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                                  GLsizei stride, const void* pointer);
        typedef void (*PFN_glVertexAttribIPointer)(GLuint index, GLint size, GLenum type, GLsizei stride,
                                                   const void* pointer);
        typedef void (*PFN_glVertexAttribDivisor)(GLuint index, GLuint divisor);

        typedef GLuint (*PFN_glCreateShader)(GLenum type);
        typedef void (*PFN_glDeleteShader)(GLuint shader);
        typedef void (*PFN_glShaderSource)(GLuint shader, GLsizei count, const GLchar* const* string,
                                           const GLint* length);
        typedef void (*PFN_glCompileShader)(GLuint shader);
        typedef void (*PFN_glGetShaderiv)(GLuint shader, GLenum pname, GLint* params);
        typedef void (*PFN_glGetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);

        typedef GLuint (*PFN_glCreateProgram)(void);
        typedef void (*PFN_glDeleteProgram)(GLuint program);
        typedef void (*PFN_glAttachShader)(GLuint program, GLuint shader);
        typedef void (*PFN_glDetachShader)(GLuint program, GLuint shader);
        typedef void (*PFN_glBindAttribLocation)(GLuint program, GLuint index, const GLchar* name);
        typedef void (*PFN_glBindFragDataLocation)(GLuint program, GLuint colorNumber, const GLchar* name);
        typedef void (*PFN_glLinkProgram)(GLuint program);
        typedef void (*PFN_glGetProgramiv)(GLuint program, GLenum pname, GLint* params);
        typedef void (*PFN_glGetProgramInfoLog)(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
        typedef void (*PFN_glUseProgram)(GLuint program);
        typedef GLint (*PFN_glGetUniformLocation)(GLuint program, const GLchar* name);
        typedef void (*PFN_glUniform1i)(GLint location, GLint v0);
        typedef void (*PFN_glUniform1ui)(GLint location, GLuint v0);
        typedef void (*PFN_glUniform2f)(GLint location, GLfloat v0, GLfloat v1);
        typedef void (*PFN_glUniform4f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);

        typedef void (*PFN_glDrawArraysInstanced)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);

        struct ImSlugGLProcs
        {
            PFN_glGetError glGetError;
            PFN_glGetString glGetString;
            PFN_glGetIntegerv glGetIntegerv;
            PFN_glEnable glEnable;
            PFN_glDisable glDisable;
            PFN_glIsEnabled glIsEnabled;
            PFN_glViewport glViewport;
            PFN_glScissor glScissor;
            PFN_glBlendEquation glBlendEquation;
            PFN_glBlendEquationSeparate glBlendEquationSeparate;
            PFN_glBlendFuncSeparate glBlendFuncSeparate;
            PFN_glPixelStorei glPixelStorei;

            PFN_glGenTextures glGenTextures;
            PFN_glDeleteTextures glDeleteTextures;
            PFN_glBindTexture glBindTexture;
            PFN_glActiveTexture glActiveTexture;
            PFN_glTexImage2D glTexImage2D;
            PFN_glTexSubImage2D glTexSubImage2D;
            PFN_glTexParameteri glTexParameteri;
            PFN_glBindSampler glBindSampler;

            PFN_glGenBuffers glGenBuffers;
            PFN_glDeleteBuffers glDeleteBuffers;
            PFN_glBindBuffer glBindBuffer;
            PFN_glBufferData glBufferData;
            PFN_glBufferSubData glBufferSubData;

            PFN_glGenVertexArrays glGenVertexArrays;
            PFN_glDeleteVertexArrays glDeleteVertexArrays;
            PFN_glBindVertexArray glBindVertexArray;
            PFN_glEnableVertexAttribArray glEnableVertexAttribArray;
            PFN_glDisableVertexAttribArray glDisableVertexAttribArray;
            PFN_glVertexAttribPointer glVertexAttribPointer;
            PFN_glVertexAttribIPointer glVertexAttribIPointer;
            PFN_glVertexAttribDivisor glVertexAttribDivisor;

            PFN_glCreateShader glCreateShader;
            PFN_glDeleteShader glDeleteShader;
            PFN_glShaderSource glShaderSource;
            PFN_glCompileShader glCompileShader;
            PFN_glGetShaderiv glGetShaderiv;
            PFN_glGetShaderInfoLog glGetShaderInfoLog;

            PFN_glCreateProgram glCreateProgram;
            PFN_glDeleteProgram glDeleteProgram;
            PFN_glAttachShader glAttachShader;
            PFN_glDetachShader glDetachShader;
            PFN_glBindAttribLocation glBindAttribLocation;
            PFN_glBindFragDataLocation glBindFragDataLocation;
            PFN_glLinkProgram glLinkProgram;
            PFN_glGetProgramiv glGetProgramiv;
            PFN_glGetProgramInfoLog glGetProgramInfoLog;
            PFN_glUseProgram glUseProgram;
            PFN_glGetUniformLocation glGetUniformLocation;
            PFN_glUniform1i glUniform1i;
            PFN_glUniform1ui glUniform1ui;
            PFN_glUniform2f glUniform2f;
            PFN_glUniform4f glUniform4f;

            PFN_glDrawArraysInstanced glDrawArraysInstanced;

            bool HasClipOrigin;
        };

        static ImSlugGLProcs g_GL = {};

        // --------------------------------------------------------------------
        // Dynamic symbol loader
        // --------------------------------------------------------------------
#if defined(_WIN32)
        static HMODULE s_LibGL = nullptr;
        static void* PlatformGetProcAddress(const char* name)
        {
            if (s_LibGL == nullptr)
                s_LibGL = LoadLibraryA("opengl32.dll");
            typedef PROC(WINAPI * PFN_wglGetProcAddress)(LPCSTR);
            static PFN_wglGetProcAddress p_wglGetProcAddress = nullptr;
            if (p_wglGetProcAddress == nullptr && s_LibGL != nullptr)
                p_wglGetProcAddress = (PFN_wglGetProcAddress)GetProcAddress(s_LibGL, "wglGetProcAddress");
            void* proc = nullptr;
            if (p_wglGetProcAddress != nullptr)
                proc = (void*)p_wglGetProcAddress(name);
            if (proc == nullptr || proc == (void*)1 || proc == (void*)2 || proc == (void*)3 || proc == (void*)-1)
            {
                if (s_LibGL != nullptr)
                    proc = (void*)GetProcAddress(s_LibGL, name);
                else
                    proc = nullptr;
            }
            return proc;
        }
#elif defined(__APPLE__)
        static void* PlatformGetProcAddress(const char* name)
        {
            static void* lib = nullptr;
            if (lib == nullptr)
                lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
            return lib != nullptr ? dlsym(lib, name) : nullptr;
        }
#elif defined(__EMSCRIPTEN__)
        static void* PlatformGetProcAddress(const char* name)
        {
            return (void*)emscripten_GetProcAddress(name);
        }
#else
        static void* PlatformGetProcAddress(const char* name)
        {
            typedef void* (*PFN_glXGetProcAddressARB)(const char*);
            typedef void* (*PFN_eglGetProcAddress)(const char*);
            static void* libGL = nullptr;
            static PFN_glXGetProcAddressARB glXGetProc = nullptr;
            static PFN_eglGetProcAddress eglGetProc = nullptr;
            if (libGL == nullptr)
            {
                const char* names[] = { "libGLX.so.0", "libOpenGL.so.0", "libGL.so.1",
                                        "libGL.so",    "libEGL.so.1",    nullptr };
                for (int i = 0; names[i] != nullptr && libGL == nullptr; ++i)
                    libGL = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
                if (libGL != nullptr)
                {
                    glXGetProc = (PFN_glXGetProcAddressARB)dlsym(libGL, "glXGetProcAddressARB");
                    eglGetProc = (PFN_eglGetProcAddress)dlsym(libGL, "eglGetProcAddress");
                }
            }
            void* proc = nullptr;
            if (glXGetProc != nullptr)
                proc = glXGetProc(name);
            if (proc == nullptr && eglGetProc != nullptr)
                proc = eglGetProc(name);
            if (proc == nullptr && libGL != nullptr)
                proc = dlsym(libGL, name);
            return proc;
        }
#endif

        static void* ResolveProc(const char* name, ImSlugOpenGL3GetProcAddressFn customLoader, void* userData)
        {
            if (customLoader != nullptr)
            {
                ImSlugOpenGL3Proc proc = customLoader(name, userData);
                if (proc != nullptr)
                    return (void*)proc;
            }
            return PlatformGetProcAddress(name);
        }

        static bool LoadGLFunctions(const ImSlugOpenGL3InitInfo* info)
        {
            ImSlugOpenGL3GetProcAddressFn customLoader = info ? info->GetProcAddress : nullptr;
            void* userData = info ? info->GetProcAddressUserData : nullptr;

#define LOAD_PROC(type, name) g_GL.name = (type)ResolveProc(#name, customLoader, userData)

            LOAD_PROC(PFN_glGetError, glGetError);
            LOAD_PROC(PFN_glGetString, glGetString);
            LOAD_PROC(PFN_glGetIntegerv, glGetIntegerv);
            LOAD_PROC(PFN_glEnable, glEnable);
            LOAD_PROC(PFN_glDisable, glDisable);
            LOAD_PROC(PFN_glIsEnabled, glIsEnabled);
            LOAD_PROC(PFN_glViewport, glViewport);
            LOAD_PROC(PFN_glScissor, glScissor);
            LOAD_PROC(PFN_glBlendEquation, glBlendEquation);
            LOAD_PROC(PFN_glBlendEquationSeparate, glBlendEquationSeparate);
            LOAD_PROC(PFN_glBlendFuncSeparate, glBlendFuncSeparate);
            LOAD_PROC(PFN_glPixelStorei, glPixelStorei);

            LOAD_PROC(PFN_glGenTextures, glGenTextures);
            LOAD_PROC(PFN_glDeleteTextures, glDeleteTextures);
            LOAD_PROC(PFN_glBindTexture, glBindTexture);
            LOAD_PROC(PFN_glActiveTexture, glActiveTexture);
            LOAD_PROC(PFN_glTexImage2D, glTexImage2D);
            LOAD_PROC(PFN_glTexSubImage2D, glTexSubImage2D);
            LOAD_PROC(PFN_glTexParameteri, glTexParameteri);
            LOAD_PROC(PFN_glBindSampler, glBindSampler);

            LOAD_PROC(PFN_glGenBuffers, glGenBuffers);
            LOAD_PROC(PFN_glDeleteBuffers, glDeleteBuffers);
            LOAD_PROC(PFN_glBindBuffer, glBindBuffer);
            LOAD_PROC(PFN_glBufferData, glBufferData);
            LOAD_PROC(PFN_glBufferSubData, glBufferSubData);

            LOAD_PROC(PFN_glGenVertexArrays, glGenVertexArrays);
            LOAD_PROC(PFN_glDeleteVertexArrays, glDeleteVertexArrays);
            LOAD_PROC(PFN_glBindVertexArray, glBindVertexArray);
            LOAD_PROC(PFN_glEnableVertexAttribArray, glEnableVertexAttribArray);
            LOAD_PROC(PFN_glDisableVertexAttribArray, glDisableVertexAttribArray);
            LOAD_PROC(PFN_glVertexAttribPointer, glVertexAttribPointer);
            LOAD_PROC(PFN_glVertexAttribIPointer, glVertexAttribIPointer);
            LOAD_PROC(PFN_glVertexAttribDivisor, glVertexAttribDivisor);

            LOAD_PROC(PFN_glCreateShader, glCreateShader);
            LOAD_PROC(PFN_glDeleteShader, glDeleteShader);
            LOAD_PROC(PFN_glShaderSource, glShaderSource);
            LOAD_PROC(PFN_glCompileShader, glCompileShader);
            LOAD_PROC(PFN_glGetShaderiv, glGetShaderiv);
            LOAD_PROC(PFN_glGetShaderInfoLog, glGetShaderInfoLog);

            LOAD_PROC(PFN_glCreateProgram, glCreateProgram);
            LOAD_PROC(PFN_glDeleteProgram, glDeleteProgram);
            LOAD_PROC(PFN_glAttachShader, glAttachShader);
            LOAD_PROC(PFN_glDetachShader, glDetachShader);
            LOAD_PROC(PFN_glBindAttribLocation, glBindAttribLocation);
            LOAD_PROC(PFN_glBindFragDataLocation, glBindFragDataLocation);
            LOAD_PROC(PFN_glLinkProgram, glLinkProgram);
            LOAD_PROC(PFN_glGetProgramiv, glGetProgramiv);
            LOAD_PROC(PFN_glGetProgramInfoLog, glGetProgramInfoLog);
            LOAD_PROC(PFN_glUseProgram, glUseProgram);
            LOAD_PROC(PFN_glGetUniformLocation, glGetUniformLocation);
            LOAD_PROC(PFN_glUniform1i, glUniform1i);
            LOAD_PROC(PFN_glUniform1ui, glUniform1ui);
            LOAD_PROC(PFN_glUniform2f, glUniform2f);
            LOAD_PROC(PFN_glUniform4f, glUniform4f);

            LOAD_PROC(PFN_glDrawArraysInstanced, glDrawArraysInstanced);
#undef LOAD_PROC

            if (g_GL.glGetString == nullptr || g_GL.glGetIntegerv == nullptr)
            {
                fprintf(stderr, "[imslug] ERROR: failed to load essential OpenGL functions (no active GL context?)\n");
                return false;
            }

            if (g_GL.glGenVertexArrays == nullptr || g_GL.glVertexAttribDivisor == nullptr ||
                g_GL.glDrawArraysInstanced == nullptr || g_GL.glCreateProgram == nullptr)
            {
                fprintf(stderr, "[imslug] ERROR: active OpenGL context lacks required GL 3.2+ features (VAO, "
                                "Instancing, Divisor)\n");
                return false;
            }

            return true;
        }

        // --------------------------------------------------------------------
        // Backend State
        // --------------------------------------------------------------------
        static const uint32_t kMaxFramesInFlight = 8;
        static const uint32_t kDefaultMaxInstances = 65536;

        struct ImSlugOpenGL3State
        {
            bool Initialized = false;
            bool DeviceObjectsCreated = false;
            char GlslVersion[64] = {};
            int GlVersion = 0;  // e.g. 330, 460
            bool IsES3 = false;

            uint32_t NumFramesInFlight = 2;
            uint32_t MaxInstancesPerFrame = kDefaultMaxInstances;
            uint32_t InstanceCapacity = kDefaultMaxInstances;
            uint32_t InstanceWarningCapacity = 0;
            uint32_t InstanceCursor = 0;

            // Per-frame instance storage
            ImSlugGlyphInstance* InstanceCPU[kMaxFramesInFlight] = {};
            GLuint InstanceVBO[kMaxFramesInFlight] = {};

            // Pool textures
            GLuint CurveTex = 0;
            GLuint BandTex = 0;
            GLuint PaintTex = 0;
            int AllocatedCurveCapacityRows = 0;
            int AllocatedBandCapacityRows = 0;
            int AllocatedPaintCapacityRows = 0;

            // Shader program
            GLuint Program = 0;
            GLuint VertShader = 0;
            GLuint FragShader = 0;

            GLint LocMVP0 = -1;
            GLint LocMVP1 = -1;
            GLint LocMVP2 = -1;
            GLint LocMVP3 = -1;
            GLint LocViewport = -1;
            GLint LocDebugFlags = -1;
            GLint LocCurveTex = -1;
            GLint LocBandTex = -1;
            GLint LocPaintTex = -1;

            ImSlugOpenGL3InitInfo InitInfo = {};
        };

        static ImSlugOpenGL3State g_State = {};

        static GLuint CheckCompile(GLuint shader, const char* name)
        {
            GLint status = 0;
            g_GL.glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status == 0)
            {
                GLint logLen = 0;
                g_GL.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
                if (logLen > 0)
                {
                    char* log = (char*)malloc((size_t)logLen);
                    if (log != nullptr)
                    {
                        g_GL.glGetShaderInfoLog(shader, logLen, nullptr, log);
                        fprintf(stderr, "[imslug] ERROR: failed to compile %s shader:\n%s\n", name, log);
                        free(log);
                    }
                }
                return 0;
            }
            return shader;
        }

        static GLuint CheckLink(GLuint program)
        {
            GLint status = 0;
            g_GL.glGetProgramiv(program, GL_LINK_STATUS, &status);
            if (status == 0)
            {
                GLint logLen = 0;
                g_GL.glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
                if (logLen > 0)
                {
                    char* log = (char*)malloc((size_t)logLen);
                    if (log != nullptr)
                    {
                        g_GL.glGetProgramInfoLog(program, logLen, nullptr, log);
                        fprintf(stderr, "[imslug] ERROR: failed to link program:\n%s\n", log);
                        free(log);
                    }
                }
                return 0;
            }
            return program;
        }
    }  // namespace

    // ========================================================================
    // Standalone Core Implementation
    // ========================================================================

    bool ImSlugOpenGL3Init(const ImSlugOpenGL3InitInfo* info)
    {
        if (g_State.Initialized)
            ImSlugOpenGL3Shutdown();

        if (!LoadGLFunctions(info))
            return false;

        g_State = ImSlugOpenGL3State();
        if (info != nullptr)
            g_State.InitInfo = *info;

        // Context version & profile parsing
        const char* verStr = (const char*)g_GL.glGetString(GL_VERSION);
        if (verStr != nullptr && strncmp(verStr, "OpenGL ES ", 10) == 0)
            g_State.IsES3 = true;

        GLint major = 0, minor = 0;
        g_GL.glGetIntegerv(GL_MAJOR_VERSION, &major);
        g_GL.glGetIntegerv(GL_MINOR_VERSION, &minor);
        if (major == 0 && verStr != nullptr)
        {
            // Fallback string parse
            const char* p = verStr;
            while (*p && (*p < '0' || *p > '9'))
                p++;
            if (sscanf(p, "%d.%d", &major, &minor) != 2)
            {
                major = 3;
                minor = 2;
            }
        }
        g_State.GlVersion = major * 100 + minor * 10;

        // Clip origin support
        g_State.GlslVersion[0] = '\0';
        g_GL.HasClipOrigin = (!g_State.IsES3 && g_State.GlVersion >= 450);

        // Frame ring configuration
        g_State.NumFramesInFlight = (info && info->NumFramesInFlight > 0) ? info->NumFramesInFlight : 2;
        if (g_State.NumFramesInFlight > kMaxFramesInFlight)
            g_State.NumFramesInFlight = kMaxFramesInFlight;

        g_State.MaxInstancesPerFrame = (info && info->MaxInstancesPerFrame > 0) ? info->MaxInstancesPerFrame : 0;
        g_State.InstanceCapacity =
            (g_State.MaxInstancesPerFrame > 0) ? g_State.MaxInstancesPerFrame : kDefaultMaxInstances;
        g_State.InstanceWarningCapacity = 0;
        g_State.InstanceCursor = 0;

        // Allocate instance rings
        for (uint32_t i = 0; i < g_State.NumFramesInFlight; ++i)
        {
            g_State.InstanceCPU[i] =
                (ImSlugGlyphInstance*)malloc((size_t)g_State.InstanceCapacity * sizeof(ImSlugGlyphInstance));
            if (g_State.InstanceCPU[i] == nullptr)
            {
                ImSlugOpenGL3Shutdown();
                return false;
            }
            g_GL.glGenBuffers(1, &g_State.InstanceVBO[i]);
            g_GL.glBindBuffer(GL_ARRAY_BUFFER, g_State.InstanceVBO[i]);
            g_GL.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_State.InstanceCapacity * sizeof(ImSlugGlyphInstance),
                              nullptr, GL_STREAM_DRAW);
        }
        g_GL.glBindBuffer(GL_ARRAY_BUFFER, 0);

        if (!ImSlugOpenGL3CreateDeviceObjects())
        {
            ImSlugOpenGL3Shutdown();
            return false;
        }

        g_State.Initialized = true;
        return true;
    }

    void ImSlugOpenGL3Shutdown()
    {
        if (g_State.DeviceObjectsCreated)
            ImSlugOpenGL3DestroyDeviceObjects();

        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        {
            if (g_State.InstanceCPU[i] != nullptr)
            {
                free(g_State.InstanceCPU[i]);
                g_State.InstanceCPU[i] = nullptr;
            }
            if (g_State.InstanceVBO[i] != 0)
            {
                if (g_GL.glDeleteBuffers != nullptr)
                    g_GL.glDeleteBuffers(1, &g_State.InstanceVBO[i]);
                g_State.InstanceVBO[i] = 0;
            }
        }

        g_State = ImSlugOpenGL3State();
    }

    bool ImSlugOpenGL3CreateDeviceObjects()
    {
        if (g_State.DeviceObjectsCreated)
            return true;

        // Resolve GLSL preamble based on context version
        const char* preamble = nullptr;
        if (g_State.IsES3 ||
            (g_State.InitInfo.GlslVersionString && strstr(g_State.InitInfo.GlslVersionString, "300 es")))
        {
            preamble = "#version 300 es\n"
                       "precision highp float;\n"
                       "precision highp int;\n"
                       "precision highp sampler2D;\n"
                       "precision highp usampler2D;\n";
        }
        else
        {
            const char* userVer = g_State.InitInfo.GlslVersionString;
            if (userVer == nullptr || !*userVer || strcmp(userVer, "#version 130") == 0 ||
                strcmp(userVer, "#version 140") == 0 || strcmp(userVer, "#version 150") == 0)
            {
                if (g_State.GlVersion >= 330)
                    preamble = "#version 330 core\n";
                else if (g_State.GlVersion >= 320)
                    preamble = "#version 150\n";
                else
                    preamble = "#version 130\n";
            }
            else
            {
                static char customBuf[80];
                snprintf(customBuf, sizeof(customBuf), "%s\n", userVer);
                preamble = customBuf;
            }
        }

        g_State.VertShader = g_GL.glCreateShader(GL_VERTEX_SHADER);
        const GLchar* vertSources[2] = { preamble, imslug_slug_vert_glsl };
        g_GL.glShaderSource(g_State.VertShader, 2, vertSources, nullptr);
        g_GL.glCompileShader(g_State.VertShader);
        if (!CheckCompile(g_State.VertShader, "vertex"))
            return false;

        g_State.FragShader = g_GL.glCreateShader(GL_FRAGMENT_SHADER);
        const GLchar* fragSources[2] = { preamble, imslug_slug_frag_glsl };
        g_GL.glShaderSource(g_State.FragShader, 2, fragSources, nullptr);
        g_GL.glCompileShader(g_State.FragShader);
        if (!CheckCompile(g_State.FragShader, "fragment"))
            return false;

        g_State.Program = g_GL.glCreateProgram();
        g_GL.glAttachShader(g_State.Program, g_State.VertShader);
        g_GL.glAttachShader(g_State.Program, g_State.FragShader);

        // Bind attribute locations 0..7 before linking
        g_GL.glBindAttribLocation(g_State.Program, 0, "InEmBBox");
        g_GL.glBindAttribLocation(g_State.Program, 1, "InScreenRect");
        g_GL.glBindAttribLocation(g_State.Program, 2, "InJac");
        g_GL.glBindAttribLocation(g_State.Program, 3, "InBanding");
        g_GL.glBindAttribLocation(g_State.Program, 4, "InGlyphLoc");
        g_GL.glBindAttribLocation(g_State.Program, 5, "InBandMaxWord");
        g_GL.glBindAttribLocation(g_State.Program, 6, "InColor");
        g_GL.glBindAttribLocation(g_State.Program, 7, "InPaintIndex");

        if (g_GL.glBindFragDataLocation != nullptr && !g_State.IsES3)
            g_GL.glBindFragDataLocation(g_State.Program, 0, "outColor");

        g_GL.glLinkProgram(g_State.Program);
        if (!CheckLink(g_State.Program))
            return false;

        // Query uniform locations
        g_State.LocMVP0 = g_GL.glGetUniformLocation(g_State.Program, "uMVP0");
        g_State.LocMVP1 = g_GL.glGetUniformLocation(g_State.Program, "uMVP1");
        g_State.LocMVP2 = g_GL.glGetUniformLocation(g_State.Program, "uMVP2");
        g_State.LocMVP3 = g_GL.glGetUniformLocation(g_State.Program, "uMVP3");
        g_State.LocViewport = g_GL.glGetUniformLocation(g_State.Program, "uViewport");
        g_State.LocDebugFlags = g_GL.glGetUniformLocation(g_State.Program, "uDebugFlags");
        g_State.LocCurveTex = g_GL.glGetUniformLocation(g_State.Program, "curveTexture");
        g_State.LocBandTex = g_GL.glGetUniformLocation(g_State.Program, "bandTexture");
        g_State.LocPaintTex = g_GL.glGetUniformLocation(g_State.Program, "paintTexture");

        // Set static texture unit assignments
        GLint origProgram = 0;
        g_GL.glGetIntegerv(GL_CURRENT_PROGRAM, &origProgram);
        g_GL.glUseProgram(g_State.Program);
        if (g_State.LocCurveTex != -1)
            g_GL.glUniform1i(g_State.LocCurveTex, 0);
        if (g_State.LocBandTex != -1)
            g_GL.glUniform1i(g_State.LocBandTex, 1);
        if (g_State.LocPaintTex != -1)
            g_GL.glUniform1i(g_State.LocPaintTex, 2);
        g_GL.glUseProgram(origProgram);

        // Ensure minimal dummy paint texture exists
        if (g_State.PaintTex == 0)
        {
            g_GL.glGenTextures(1, &g_State.PaintTex);
            GLint origTex = 0;
            g_GL.glGetIntegerv(GL_TEXTURE_BINDING_2D, &origTex);
            g_GL.glBindTexture(GL_TEXTURE_2D, g_State.PaintTex);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            uint32_t dummy[22 * 4] = {};
            g_GL.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, 22, 1, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, dummy);
            g_GL.glBindTexture(GL_TEXTURE_2D, origTex);
            g_State.AllocatedPaintCapacityRows = 1;
        }

        g_State.DeviceObjectsCreated = true;
        return true;
    }

    void ImSlugOpenGL3DestroyDeviceObjects()
    {
        if (g_State.Program != 0)
        {
            if (g_State.VertShader != 0)
            {
                g_GL.glDetachShader(g_State.Program, g_State.VertShader);
                g_GL.glDeleteShader(g_State.VertShader);
                g_State.VertShader = 0;
            }
            if (g_State.FragShader != 0)
            {
                g_GL.glDetachShader(g_State.Program, g_State.FragShader);
                g_GL.glDeleteShader(g_State.FragShader);
                g_State.FragShader = 0;
            }
            g_GL.glDeleteProgram(g_State.Program);
            g_State.Program = 0;
        }

        if (g_State.CurveTex != 0)
        {
            g_GL.glDeleteTextures(1, &g_State.CurveTex);
            g_State.CurveTex = 0;
        }
        if (g_State.BandTex != 0)
        {
            g_GL.glDeleteTextures(1, &g_State.BandTex);
            g_State.BandTex = 0;
        }
        if (g_State.PaintTex != 0)
        {
            g_GL.glDeleteTextures(1, &g_State.PaintTex);
            g_State.PaintTex = 0;
        }

        g_State.AllocatedCurveCapacityRows = 0;
        g_State.AllocatedBandCapacityRows = 0;
        g_State.AllocatedPaintCapacityRows = 0;
        g_State.DeviceObjectsCreated = false;
    }

    bool ImSlugOpenGL3UploadPools(ImSlugPools* pools)
    {
        if (pools == nullptr)
            return false;

        // Reset per-frame reservation cursor
        g_State.InstanceCursor = 0;

        // Save unpack state & bindings
        GLint origActiveTex = 0;
        g_GL.glGetIntegerv(GL_ACTIVE_TEXTURE, &origActiveTex);
        GLint origTex0 = 0;
        g_GL.glActiveTexture(GL_TEXTURE0);
        g_GL.glGetIntegerv(GL_TEXTURE_BINDING_2D, &origTex0);
        GLint origUnpackPBO = 0;
        g_GL.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &origUnpackPBO);
        GLint origRowLength = 0, origSkipPixels = 0, origSkipRows = 0, origAlignment = 0;
        g_GL.glGetIntegerv(GL_UNPACK_ROW_LENGTH, &origRowLength);
        g_GL.glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &origSkipPixels);
        g_GL.glGetIntegerv(GL_UNPACK_SKIP_ROWS, &origSkipRows);
        g_GL.glGetIntegerv(GL_UNPACK_ALIGNMENT, &origAlignment);

        if (origUnpackPBO != 0)
            g_GL.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        g_GL.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        g_GL.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        g_GL.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        g_GL.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        // 1. Curve Pool (RGBA16F, 4096 texels wide)
        if (pools->Curves.Height > 0)
        {
            if (g_State.CurveTex == 0)
                g_GL.glGenTextures(1, &g_State.CurveTex);
            g_GL.glBindTexture(GL_TEXTURE_2D, g_State.CurveTex);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            const bool needRealloc = (g_State.AllocatedCurveCapacityRows != pools->Curves.Height);
            if (needRealloc)
            {
                g_GL.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 4096, pools->Curves.Height, 0, GL_RGBA, GL_HALF_FLOAT,
                                  pools->Curves.Texels);
                g_State.AllocatedCurveCapacityRows = pools->Curves.Height;
            }
            else if (pools->Curves.DirtyRowMin <= pools->Curves.DirtyRowMax)
            {
                int dirtyMin = pools->Curves.DirtyRowMin;
                int count = pools->Curves.DirtyRowMax - dirtyMin + 1;
                const void* data = (const uint8_t*)pools->Curves.Texels + (size_t)dirtyMin * 4096 * 8;
                g_GL.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dirtyMin, 4096, count, GL_RGBA, GL_HALF_FLOAT, data);
            }
            pools->Curves.DirtyRowMin = INT_MAX;
            pools->Curves.DirtyRowMax = -1;
        }

        // 2. Band Pool (RG16UI, 4096 texels wide)
        if (pools->Bands.Height > 0)
        {
            if (g_State.BandTex == 0)
                g_GL.glGenTextures(1, &g_State.BandTex);
            g_GL.glBindTexture(GL_TEXTURE_2D, g_State.BandTex);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            const bool needRealloc = (g_State.AllocatedBandCapacityRows != pools->Bands.Height);
            if (needRealloc)
            {
                g_GL.glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16UI, 4096, pools->Bands.Height, 0, GL_RG_INTEGER,
                                  GL_UNSIGNED_SHORT, pools->Bands.Texels);
                g_State.AllocatedBandCapacityRows = pools->Bands.Height;
            }
            else if (pools->Bands.DirtyRowMin <= pools->Bands.DirtyRowMax)
            {
                int dirtyMin = pools->Bands.DirtyRowMin;
                int count = pools->Bands.DirtyRowMax - dirtyMin + 1;
                const void* data = (const uint8_t*)pools->Bands.Texels + (size_t)dirtyMin * 4096 * 4;
                g_GL.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dirtyMin, 4096, count, GL_RG_INTEGER, GL_UNSIGNED_SHORT,
                                     data);
            }
            pools->Bands.DirtyRowMin = INT_MAX;
            pools->Bands.DirtyRowMax = -1;
        }

        // 3. Paint Pool (RGBA32UI, 22 texels wide, 1 row per record)
        if (pools->Paints.Capacity > 0)
        {
            if (g_State.PaintTex == 0)
                g_GL.glGenTextures(1, &g_State.PaintTex);
            g_GL.glBindTexture(GL_TEXTURE_2D, g_State.PaintTex);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g_GL.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            const int paintRows = pools->Paints.Capacity;
            const bool needRealloc = (g_State.AllocatedPaintCapacityRows != paintRows);
            if (needRealloc)
            {
                g_GL.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, 22, paintRows, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT,
                                  pools->Paints.Bytes);
                g_State.AllocatedPaintCapacityRows = paintRows;
            }
            else if (pools->Paints.DirtyRecordMin <= pools->Paints.DirtyRecordMax)
            {
                int dirtyMin = pools->Paints.DirtyRecordMin;
                int count = pools->Paints.DirtyRecordMax - dirtyMin + 1;
                const void* data = (const uint8_t*)pools->Paints.Bytes + (size_t)dirtyMin * 352;
                g_GL.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dirtyMin, 22, count, GL_RGBA_INTEGER, GL_UNSIGNED_INT, data);
            }
            pools->Paints.DirtyRecordMin = INT_MAX;
            pools->Paints.DirtyRecordMax = -1;
        }

        // Restore unpack state & bindings
        if (origUnpackPBO != 0)
            g_GL.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, origUnpackPBO);
        g_GL.glPixelStorei(GL_UNPACK_ROW_LENGTH, origRowLength);
        g_GL.glPixelStorei(GL_UNPACK_SKIP_PIXELS, origSkipPixels);
        g_GL.glPixelStorei(GL_UNPACK_SKIP_ROWS, origSkipRows);
        g_GL.glPixelStorei(GL_UNPACK_ALIGNMENT, origAlignment);

        g_GL.glBindTexture(GL_TEXTURE_2D, origTex0);
        g_GL.glActiveTexture(origActiveTex);
        return true;
    }

    uint32_t ImSlugOpenGL3GetFramesInFlight()
    {
        return g_State.NumFramesInFlight;
    }

    uint32_t ImSlugOpenGL3GetInstanceCapacity()
    {
        return g_State.InstanceCapacity;
    }

    ImSlugGlyphInstance* ImSlugOpenGL3GetInstanceData(uint32_t frameIndex)
    {
        if (frameIndex >= g_State.NumFramesInFlight)
            return nullptr;
        return g_State.InstanceCPU[frameIndex];
    }

    bool ImSlugOpenGL3ReserveInstances(uint32_t frameIndex, uint32_t requiredInstances)
    {
        if (frameIndex >= g_State.NumFramesInFlight)
            return false;
        if (requiredInstances <= g_State.InstanceCapacity)
            return true;

        if (g_State.MaxInstancesPerFrame != 0)
        {
            // Fixed hard cap
            if (g_State.InstanceWarningCapacity != g_State.InstanceCapacity)
            {
                fprintf(
                    stderr,
                    "[imslug] WARNING: frame demanded %u instances exceeding hard cap %u; overflowing text will drop\n",
                    requiredInstances, g_State.InstanceCapacity);
                g_State.InstanceWarningCapacity = g_State.InstanceCapacity;
            }
            return false;
        }

        // Adaptive growth (doubling, up to 8x default)
        uint32_t newCap = g_State.InstanceCapacity;
        while (newCap < requiredInstances)
            newCap *= 2;
        if (newCap > kDefaultMaxInstances * 8)
            newCap = kDefaultMaxInstances * 8;
        if (newCap < requiredInstances)
            return false;

        for (uint32_t i = 0; i < g_State.NumFramesInFlight; ++i)
        {
            ImSlugGlyphInstance* newBuf =
                (ImSlugGlyphInstance*)realloc(g_State.InstanceCPU[i], (size_t)newCap * sizeof(ImSlugGlyphInstance));
            if (newBuf == nullptr)
                return false;
            g_State.InstanceCPU[i] = newBuf;

            g_GL.glBindBuffer(GL_ARRAY_BUFFER, g_State.InstanceVBO[i]);
            g_GL.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)newCap * sizeof(ImSlugGlyphInstance), nullptr,
                              GL_STREAM_DRAW);
        }
        g_GL.glBindBuffer(GL_ARRAY_BUFFER, 0);
        g_State.InstanceCapacity = newCap;
        return true;
    }

    void ImSlugOpenGL3UploadInstances(uint32_t frameIndex, uint32_t instanceCount)
    {
        if (frameIndex >= g_State.NumFramesInFlight || instanceCount == 0)
            return;
        if (instanceCount > g_State.InstanceCapacity)
            instanceCount = g_State.InstanceCapacity;

        g_GL.glBindBuffer(GL_ARRAY_BUFFER, g_State.InstanceVBO[frameIndex]);
        g_GL.glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)instanceCount * sizeof(ImSlugGlyphInstance),
                             g_State.InstanceCPU[frameIndex]);
        g_GL.glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    void ImSlugOpenGL3Draw(const ImSlugOpenGL3DrawInfo* draw)
    {
        if (draw == nullptr || draw->InstanceCount == 0)
            return;
        if (draw->FrameIndex >= g_State.NumFramesInFlight)
            return;
        if (g_State.Program == 0)
            return;

        // Unbind any active sampler objects on units 0..2
        if (g_GL.glBindSampler != nullptr)
        {
            g_GL.glBindSampler(0, 0);
            g_GL.glBindSampler(1, 0);
            g_GL.glBindSampler(2, 0);
        }

        // Bind pool textures to units 0, 1, 2
        g_GL.glActiveTexture(GL_TEXTURE0);
        g_GL.glBindTexture(GL_TEXTURE_2D, g_State.CurveTex);
        g_GL.glActiveTexture(GL_TEXTURE1);
        g_GL.glBindTexture(GL_TEXTURE_2D, g_State.BandTex);
        g_GL.glActiveTexture(GL_TEXTURE2);
        g_GL.glBindTexture(GL_TEXTURE_2D, g_State.PaintTex);

        // Bind program & upload uniforms
        g_GL.glUseProgram(g_State.Program);
        g_GL.glUniform4f(g_State.LocMVP0, draw->MVP[0], draw->MVP[1], draw->MVP[2], draw->MVP[3]);
        g_GL.glUniform4f(g_State.LocMVP1, draw->MVP[4], draw->MVP[5], draw->MVP[6], draw->MVP[7]);
        g_GL.glUniform4f(g_State.LocMVP2, draw->MVP[8], draw->MVP[9], draw->MVP[10], draw->MVP[11]);
        g_GL.glUniform4f(g_State.LocMVP3, draw->MVP[12], draw->MVP[13], draw->MVP[14], draw->MVP[15]);
        g_GL.glUniform2f(g_State.LocViewport, draw->ViewportWidth, draw->ViewportHeight);
        g_GL.glUniform1ui(g_State.LocDebugFlags, draw->DebugFlags);

        // Blending & culling state
        g_GL.glEnable(GL_BLEND);
        g_GL.glBlendEquation(GL_FUNC_ADD);
        g_GL.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        g_GL.glDisable(GL_CULL_FACE);
        g_GL.glDisable(GL_DEPTH_TEST);

        // Viewport and scissor
        g_GL.glViewport(0, 0, (GLsizei)draw->ViewportWidth, (GLsizei)draw->ViewportHeight);
        g_GL.glEnable(GL_SCISSOR_TEST);
        GLint glScissorY = (GLint)draw->ViewportHeight - (GLint)(draw->ScissorY + (int32_t)draw->ScissorH);
        g_GL.glScissor(draw->ScissorX, glScissorY, (GLsizei)draw->ScissorW, (GLsizei)draw->ScissorH);

        // Configure VAO with base instance offset
        GLuint vao = 0;
        g_GL.glGenVertexArrays(1, &vao);
        g_GL.glBindVertexArray(vao);
        g_GL.glBindBuffer(GL_ARRAY_BUFFER, g_State.InstanceVBO[draw->FrameIndex]);

        const GLintptr baseOffset = (GLintptr)draw->FirstInstance * sizeof(ImSlugGlyphInstance);

        // loc 0: InEmBBox (vec4 float)
        g_GL.glEnableVertexAttribArray(0);
        g_GL.glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE_VAL, sizeof(ImSlugGlyphInstance),
                                   (const void*)(baseOffset + offsetof(ImSlugGlyphInstance, EmMinX)));
        g_GL.glVertexAttribDivisor(0, 1);

        // loc 1: InScreenRect (vec4 float)
        g_GL.glEnableVertexAttribArray(1);
        g_GL.glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE_VAL, sizeof(ImSlugGlyphInstance),
                                   (const void*)(baseOffset + offsetof(ImSlugGlyphInstance, ScreenMinX)));
        g_GL.glVertexAttribDivisor(1, 1);

        // loc 2: InJac (vec4 float)
        g_GL.glEnableVertexAttribArray(2);
        g_GL.glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE_VAL, sizeof(ImSlugGlyphInstance),
                                   (const void*)(baseOffset + offsetof(ImSlugGlyphInstance, Jac00)));
        g_GL.glVertexAttribDivisor(2, 1);

        // loc 3: InBanding (vec4 float)
        g_GL.glEnableVertexAttribArray(3);
        g_GL.glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE_VAL, sizeof(ImSlugGlyphInstance),
                                   (const void*)(baseOffset + offsetof(ImSlugGlyphInstance, BandScaleX)));
        g_GL.glVertexAttribDivisor(3, 1);

        // loc 4: InGlyphLoc (uvec2 uint)
        g_GL.glEnableVertexAttribArray(4);
        g_GL.glVertexAttribIPointer(4, 2, GL_UNSIGNED_INT, sizeof(ImSlugGlyphInstance),
                                    (const void*)(baseOffset + offsetof(ImSlugGlyphInstance, GlyphLocX)));
        g_GL.glVertexAttribDivisor(4, 1);

        // loc 5: InBandMaxWord (uint)
        g_GL.glEnableVertexAttribArray(5);
        g_GL.glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, sizeof(ImSlugGlyphInstance),
                                    (const void*)(baseOffset + offsetof(ImSlugGlyphInstance, BandMaxWord)));
        g_GL.glVertexAttribDivisor(5, 1);

        // loc 6: InColor (vec4 float)
        g_GL.glEnableVertexAttribArray(6);
        g_GL.glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE_VAL, sizeof(ImSlugGlyphInstance),
                                   (const void*)(baseOffset + offsetof(ImSlugGlyphInstance, ColorR)));
        g_GL.glVertexAttribDivisor(6, 1);

        // loc 7: InPaintIndex (uint)
        g_GL.glEnableVertexAttribArray(7);
        g_GL.glVertexAttribIPointer(7, 1, GL_UNSIGNED_INT, sizeof(ImSlugGlyphInstance),
                                    (const void*)(baseOffset + offsetof(ImSlugGlyphInstance, PaintIndex)));
        g_GL.glVertexAttribDivisor(7, 1);

        // Draw 4 vertices per quad as triangle strip
        g_GL.glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)draw->InstanceCount);

        // Cleanup VAO
        g_GL.glBindVertexArray(0);
        g_GL.glBindBuffer(GL_ARRAY_BUFFER, 0);
        g_GL.glDeleteVertexArrays(1, &vao);
        g_GL.glDisable(GL_SCISSOR_TEST);

        // Unbind texture units 2 & 1, leave unit 0 active for ImGui
        g_GL.glActiveTexture(GL_TEXTURE2);
        g_GL.glBindTexture(GL_TEXTURE_2D, 0);
        g_GL.glActiveTexture(GL_TEXTURE1);
        g_GL.glBindTexture(GL_TEXTURE_2D, 0);
        g_GL.glActiveTexture(GL_TEXTURE0);
    }

    void ImSlugOpenGL3BuildOrthoMVP(float displayPosX, float displayPosY, float displayWidth, float displayHeight,
                                    float outMVP[16], bool upperLeftClipOrigin)
    {
        float L = displayPosX;
        float R = displayPosX + displayWidth;
        float T = displayPosY;
        float B = displayPosY + displayHeight;
        if (upperLeftClipOrigin)
        {
            float tmp = T;
            T = B;
            B = tmp;
        }

        // Row 0
        outMVP[0] = 2.0f / (R - L);
        outMVP[1] = 0.0f;
        outMVP[2] = 0.0f;
        outMVP[3] = (R + L) / (L - R);

        // Row 1
        outMVP[4] = 0.0f;
        outMVP[5] = 2.0f / (T - B);
        outMVP[6] = 0.0f;
        outMVP[7] = (T + B) / (B - T);

        // Row 2
        outMVP[8] = 0.0f;
        outMVP[9] = 0.0f;
        outMVP[10] = -1.0f;
        outMVP[11] = 0.0f;

        // Row 3
        outMVP[12] = 0.0f;
        outMVP[13] = 0.0f;
        outMVP[14] = 0.0f;
        outMVP[15] = 1.0f;
    }

    // ========================================================================
    // Dear ImGui Adapter Implementation
    // ========================================================================
#if !defined(IMSLUG_NO_IMGUI)

    namespace
    {
        struct ImGuiOpenGL3State
        {
            bool Initialized = false;
            uint32_t DebugOverlay = 0;
            uint32_t FrameIndex = 0;
            uint32_t FrameCounter = 0;
            int UploadFrameCount = -1;
            uint32_t StagedInstanceCount = 0;

            float DisplayPosX = 0.0f, DisplayPosY = 0.0f;
            float DisplaySizeX = 0.0f, DisplaySizeY = 0.0f;
            float FramebufferScaleX = 1.0f, FramebufferScaleY = 1.0f;
            uint32_t FramebufferWidth = 0, FramebufferHeight = 0;
            bool UpperLeftClipOrigin = false;
        };

        static ImGuiOpenGL3State g_ImGuiOpenGL3 = {};

        static uint32_t ImGuiRenderIO_GetFrameIndex()
        {
            return g_ImGuiOpenGL3.FrameIndex;
        }

        static ImSlugGlyphInstance* ImGuiRenderIO_ReserveInstances(uint32_t count, uint32_t* outFirstInstance)
        {
            if (!g_ImGuiOpenGL3.Initialized || count == 0)
                return nullptr;
            if (!ImSlugOpenGL3ReserveInstances(g_ImGuiOpenGL3.FrameIndex, g_State.InstanceCursor + count))
                return nullptr;

            *outFirstInstance = g_State.InstanceCursor;
            ImSlugGlyphInstance* base = ImSlugOpenGL3GetInstanceData(g_ImGuiOpenGL3.FrameIndex);
            ImSlugGlyphInstance* result = base + g_State.InstanceCursor;
            g_State.InstanceCursor += count;
            g_ImGuiOpenGL3.StagedInstanceCount = g_State.InstanceCursor;
            return result;
        }

        static void ImGuiRenderIO_DrawBatch(const ImSlugBatch* batch)
        {
            if (batch == nullptr || batch->InstanceCount == 0)
                return;
            if (g_ImGuiOpenGL3.FramebufferWidth == 0 || g_ImGuiOpenGL3.FramebufferHeight == 0)
                return;

            float clipMinX = (batch->ClipMinX - g_ImGuiOpenGL3.DisplayPosX) * g_ImGuiOpenGL3.FramebufferScaleX;
            float clipMinY = (batch->ClipMinY - g_ImGuiOpenGL3.DisplayPosY) * g_ImGuiOpenGL3.FramebufferScaleY;
            float clipMaxX = (batch->ClipMaxX - g_ImGuiOpenGL3.DisplayPosX) * g_ImGuiOpenGL3.FramebufferScaleX;
            float clipMaxY = (batch->ClipMaxY - g_ImGuiOpenGL3.DisplayPosY) * g_ImGuiOpenGL3.FramebufferScaleY;
            if (clipMinX < 0.0f)
                clipMinX = 0.0f;
            if (clipMinY < 0.0f)
                clipMinY = 0.0f;
            if (clipMaxX > (float)g_ImGuiOpenGL3.FramebufferWidth)
                clipMaxX = (float)g_ImGuiOpenGL3.FramebufferWidth;
            if (clipMaxY > (float)g_ImGuiOpenGL3.FramebufferHeight)
                clipMaxY = (float)g_ImGuiOpenGL3.FramebufferHeight;
            if (clipMaxX <= clipMinX || clipMaxY <= clipMinY)
                return;

            float mvp[16];
            ImSlugOpenGL3BuildOrthoMVP(g_ImGuiOpenGL3.DisplayPosX, g_ImGuiOpenGL3.DisplayPosY,
                                       g_ImGuiOpenGL3.DisplaySizeX, g_ImGuiOpenGL3.DisplaySizeY, mvp,
                                       g_ImGuiOpenGL3.UpperLeftClipOrigin);

            ImSlugOpenGL3DrawInfo draw = {};
            for (int i = 0; i < 16; ++i)
                draw.MVP[i] = mvp[i];
            draw.ViewportWidth = (float)g_ImGuiOpenGL3.FramebufferWidth;
            draw.ViewportHeight = (float)g_ImGuiOpenGL3.FramebufferHeight;
            draw.ScissorX = (int32_t)clipMinX;
            draw.ScissorY = (int32_t)clipMinY;
            draw.ScissorW = (uint32_t)(clipMaxX - clipMinX);
            draw.ScissorH = (uint32_t)(clipMaxY - clipMinY);
            draw.FrameIndex = batch->FrameIndex;
            draw.FirstInstance = batch->FirstInstance;
            draw.InstanceCount = batch->InstanceCount;
            draw.DebugFlags = g_ImGuiOpenGL3.DebugOverlay;

            ImSlugOpenGL3Draw(&draw);
        }

        static const ImSlugRenderIO g_ImGuiRenderIO = { "opengl3", ImGuiRenderIO_GetFrameIndex,
                                                        ImGuiRenderIO_ReserveInstances, ImGuiRenderIO_DrawBatch };
    }  // namespace

    bool ImSlug_ImplOpenGL3_Init(const char* glsl_version)
    {
        ImSlugOpenGL3InitInfo info = {};
        info.GlslVersionString = glsl_version;
        return ImSlug_ImplOpenGL3_InitFromOpenGL3Info(&info);
    }

    bool ImSlug_ImplOpenGL3_InitFromOpenGL3Info(const ImSlugOpenGL3InitInfo* info)
    {
        if (g_ImGuiOpenGL3.Initialized)
            ImSlug_ImplOpenGL3_Shutdown();

        const Config* config = ImSlugGetConfig();
        if (config == nullptr)
        {
            IMSLUG_ASSERT(false && "imslug: call ImSlug::Init() before ImSlug_ImplOpenGL3_Init()");
            return false;
        }

        ImSlugOpenGL3InitInfo initInfo = info ? *info : ImSlugOpenGL3InitInfo();
        if (initInfo.MaxInstancesPerFrame == 0)
            initInfo.MaxInstancesPerFrame = config->MaxInstancesPerFrame;

        if (!ImSlugOpenGL3Init(&initInfo))
            return false;

        g_ImGuiOpenGL3 = ImGuiOpenGL3State();
        g_ImGuiOpenGL3.Initialized = true;
        g_ImGuiOpenGL3.UploadFrameCount = -1;
        g_ImGuiOpenGL3.DebugOverlay = config->DebugDrawBounds ? (uint32_t)ImSlugOpenGL3DebugOverlay_GlyphBounds
                                                              : (uint32_t)ImSlugOpenGL3DebugOverlay_None;

        ImSlugSetRenderIO(&g_ImGuiRenderIO);
        return true;
    }

    void ImSlug_ImplOpenGL3_Shutdown()
    {
        if (!g_ImGuiOpenGL3.Initialized)
            return;
        ImSlugSetRenderIO(nullptr);
        ImSlugOpenGL3Shutdown();
        g_ImGuiOpenGL3 = ImGuiOpenGL3State();
    }

    bool ImSlug_ImplOpenGL3_UploadPools()
    {
        if (!g_ImGuiOpenGL3.Initialized)
            return false;

        const int frameCount = ImGui::GetFrameCount();
        IMSLUG_ASSERT(g_ImGuiOpenGL3.UploadFrameCount != frameCount &&
                      "imslug: ImSlug_ImplOpenGL3_UploadPools called twice in one frame");
        g_ImGuiOpenGL3.UploadFrameCount = frameCount;

        const uint32_t framesInFlight = ImSlugOpenGL3GetFramesInFlight();
        g_ImGuiOpenGL3.FrameIndex = framesInFlight != 0 ? (g_ImGuiOpenGL3.FrameCounter % framesInFlight) : 0;
        g_ImGuiOpenGL3.FrameCounter++;
        g_ImGuiOpenGL3.StagedInstanceCount = 0;
        g_State.InstanceCursor = 0;

        ImSlugPools* pools = ImSlugGetPools();
        if (pools != nullptr)
            return ImSlugOpenGL3UploadPools(pools);
        return true;
    }

    void ImSlug_ImplOpenGL3_RenderDrawData(ImDrawData* drawData)
    {
        if (!g_ImGuiOpenGL3.Initialized || drawData == nullptr)
        {
            ImGui_ImplOpenGL3_RenderDrawData(drawData);
            return;
        }
        IMSLUG_ASSERT(g_ImGuiOpenGL3.UploadFrameCount >= 0 &&
                      "imslug: call ImSlug_ImplOpenGL3_UploadPools() before rendering (see imslug_impl_opengl3.h)");

        g_ImGuiOpenGL3.FramebufferWidth = (uint32_t)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
        g_ImGuiOpenGL3.FramebufferHeight = (uint32_t)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
        g_ImGuiOpenGL3.DisplayPosX = drawData->DisplayPos.x;
        g_ImGuiOpenGL3.DisplayPosY = drawData->DisplayPos.y;
        g_ImGuiOpenGL3.DisplaySizeX = drawData->DisplaySize.x;
        g_ImGuiOpenGL3.DisplaySizeY = drawData->DisplaySize.y;
        g_ImGuiOpenGL3.FramebufferScaleX = drawData->FramebufferScale.x;
        g_ImGuiOpenGL3.FramebufferScaleY = drawData->FramebufferScale.y;

        bool upperLeft = false;
        if (g_GL.HasClipOrigin && g_GL.glGetIntegerv != nullptr)
        {
            GLint origin = 0;
            g_GL.glGetIntegerv(GL_CLIP_ORIGIN, &origin);
            if (origin == (GLint)GL_UPPER_LEFT)
                upperLeft = true;
        }
        g_ImGuiOpenGL3.UpperLeftClipOrigin = upperLeft;

        ImSlug::PreprocessDrawData(drawData);

        if (g_ImGuiOpenGL3.StagedInstanceCount > 0)
            ImSlugOpenGL3UploadInstances(g_ImGuiOpenGL3.FrameIndex, g_ImGuiOpenGL3.StagedInstanceCount);

        ImGui_ImplOpenGL3_RenderDrawData(drawData);

        g_ImGuiOpenGL3.FramebufferWidth = g_ImGuiOpenGL3.FramebufferHeight = 0;
    }

    void ImSlug_ImplOpenGL3_SetDebugOverlay(uint32_t flags)
    {
        IMSLUG_ASSERT(g_ImGuiOpenGL3.Initialized);
        if (!g_ImGuiOpenGL3.Initialized)
            return;
        g_ImGuiOpenGL3.DebugOverlay = flags;
    }

    uint32_t ImSlug_ImplOpenGL3_GetDebugOverlay()
    {
        return g_ImGuiOpenGL3.Initialized ? g_ImGuiOpenGL3.DebugOverlay : 0;
    }

#endif  // !defined(IMSLUG_NO_IMGUI)

}  // namespace ImSlug
