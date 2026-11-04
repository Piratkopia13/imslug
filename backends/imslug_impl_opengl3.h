// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_impl_opengl3.h — standalone OpenGL 3.2+ / OpenGL ES 3.0+ rendering
// core for the Slug pipeline. ImGui-free by design: it executes draw commands
// against the active OpenGL context. The ImSlug_Impl* adapter at the bottom of
// this header bridges to the stock ImGui OpenGL3 backend.
//
// What the module owns:
//   * the three pool textures (curve RGBA16F + band RG16UI + paint RGBA32UI);
//   * one CPU instance ring plus one GPU ARRAY_BUFFER per frame in flight;
//   * the Slug program object and compiled shaders.

#pragma once

#include "imslug_internal.h"

// Forward declarations only: the OpenGL core stays ImGui-free, so this header
// does not include imgui.h / imgui_impl_opengl3.h. Include those before this
// header to use the ImSlug_ImplOpenGL3_* adapter.
struct ImDrawData;

#ifndef IMSLUG_API
#define IMSLUG_API  // imslug.h defines the real one; this header stays standalone
#endif

namespace ImSlug
{
    // ========================================================================
    // Standalone OpenGL 3.2+ / OpenGL ES 3.0+ core (ImGui-free)
    // ========================================================================

    // Optional custom OpenGL function loader callback (matches GLFWglproc / SDL_GL_GetProcAddress).
    // If nullptr, imslug uses its own built-in platform loader (wglGetProcAddress / dlsym).
    typedef void (*ImSlugOpenGL3Proc)(void);
    typedef ImSlugOpenGL3Proc (*ImSlugOpenGL3GetProcAddressFn)(const char* procName, void* userData);

    struct ImSlugOpenGL3InitInfo
    {
        // Optional GLSL version string (e.g. "#version 150", "#version 330 core", "#version 300 es").
        // nullptr = auto-detect from active GL context.
        const char* GlslVersionString = nullptr;
        uint32_t MaxInstancesPerFrame = 0;                       // 0 = default (65536).
        uint32_t NumFramesInFlight = 0;                          // 0 = default (2); clamped to [1, 8].
        ImSlugOpenGL3GetProcAddressFn GetProcAddress = nullptr;  // Optional custom loader callback.
        void* GetProcAddressUserData = nullptr;
    };

    struct ImSlugOpenGL3DrawInfo
    {
        float MVP[16];           // Row-major 4x4 orthographic projection.
        float ViewportWidth;     // Framebuffer width in pixels.
        float ViewportHeight;    // Framebuffer height in pixels.
        int32_t ScissorX;        // Top-left X in framebuffer pixels.
        int32_t ScissorY;        // Top-left Y in framebuffer pixels (converted to lower-left GL scissor).
        uint32_t ScissorW;       // Width in framebuffer pixels.
        uint32_t ScissorH;       // Height in framebuffer pixels.
        uint32_t FrameIndex;     // Ring slot in [0, ImSlugOpenGL3GetFramesInFlight()).
        uint32_t FirstInstance;  // Offset in instances within the slot's instance buffer.
        uint32_t InstanceCount;  // Number of instances to draw.
        uint32_t DebugFlags;     // Bitmask of ImSlugOpenGL3DebugOverlay flags.
    };

    enum ImSlugOpenGL3DebugOverlay : uint32_t
    {
        ImSlugOpenGL3DebugOverlay_None = 0,
        ImSlugOpenGL3DebugOverlay_GlyphBounds = 1u << 0,
        ImSlugOpenGL3DebugOverlay_BandGrid = 1u << 1,
        ImSlugOpenGL3DebugOverlay_RefHeat = 1u << 2,
    };

    IMSLUG_API bool ImSlugOpenGL3Init(const ImSlugOpenGL3InitInfo* info);
    IMSLUG_API void ImSlugOpenGL3Shutdown();
    IMSLUG_API bool ImSlugOpenGL3UploadPools(ImSlugPools* pools);

    IMSLUG_API uint32_t ImSlugOpenGL3GetFramesInFlight();
    IMSLUG_API uint32_t ImSlugOpenGL3GetInstanceCapacity();
    IMSLUG_API ImSlugGlyphInstance* ImSlugOpenGL3GetInstanceData(uint32_t frameIndex);
    IMSLUG_API bool ImSlugOpenGL3ReserveInstances(uint32_t frameIndex, uint32_t requiredInstances);
    IMSLUG_API void ImSlugOpenGL3UploadInstances(uint32_t frameIndex, uint32_t instanceCount);
    IMSLUG_API void ImSlugOpenGL3Draw(const ImSlugOpenGL3DrawInfo* draw);

    // Device-object lifecycle hooks (matching ImGui_ImplOpenGL3_CreateDeviceObjects / DestroyDeviceObjects
    // in case the host recreates GL objects on context loss).
    IMSLUG_API bool ImSlugOpenGL3CreateDeviceObjects();
    IMSLUG_API void ImSlugOpenGL3DestroyDeviceObjects();

    // Build a row-major 4x4 orthographic projection mapping [displayPos, displayPos + displaySize]
    // to OpenGL clip space: X in [-1, +1] (left to right), Y in [+1, -1] (top to bottom when
    // upperLeftClipOrigin is false, matching standard OpenGL Y-up clip space; or [-1, +1] when
    // upperLeftClipOrigin is true for GL_ARB_clip_control + GL_UPPER_LEFT), Z = 0, W = 1.
    IMSLUG_API void ImSlugOpenGL3BuildOrthoMVP(float displayPosX, float displayPosY, float displayWidth,
                                               float displayHeight, float outMVP[16], bool upperLeftClipOrigin = false);

    // ========================================================================
    // Dear ImGui adapter (omitted when IMSLUG_NO_IMGUI is defined)
    // ========================================================================
#if !defined(IMSLUG_NO_IMGUI)

    // One-line initialization matching ImGui_ImplOpenGL3_Init(glsl_version).
    IMSLUG_API bool ImSlug_ImplOpenGL3_Init(const char* glsl_version = nullptr);
    // Full initialization with custom MaxInstances / custom GetProcAddress.
    IMSLUG_API bool ImSlug_ImplOpenGL3_InitFromOpenGL3Info(const ImSlugOpenGL3InitInfo* info);
    IMSLUG_API void ImSlug_ImplOpenGL3_Shutdown();
    IMSLUG_API bool ImSlug_ImplOpenGL3_UploadPools();
    IMSLUG_API void ImSlug_ImplOpenGL3_RenderDrawData(ImDrawData* drawData);

    IMSLUG_API void ImSlug_ImplOpenGL3_SetDebugOverlay(uint32_t flags);
    IMSLUG_API uint32_t ImSlug_ImplOpenGL3_GetDebugOverlay();

#endif  // !defined(IMSLUG_NO_IMGUI)

}  // namespace ImSlug
