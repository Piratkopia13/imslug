// imslug — implementation.
//
// imslug_c_opengl3.cpp — OpenGL 3.2+ / OpenGL ES 3.0+ C API: the C entry
// points declared by imslug_c.h when IMSLUG_C_OPENGL3 is defined. Thin mirror
// of the public backend entry points (backends/imslug_impl_opengl3.h).
//
// Lives in backends/ (invariant 7: the core stays backend-independent).

#ifndef IMSLUG_C_OPENGL3
#define IMSLUG_C_OPENGL3 1
#endif

#include "imslug_c.h"
#include "imslug.h"
#include "imslug_impl_opengl3.h"

// ---------------------------------------------------------------------------
// Standalone core
// ---------------------------------------------------------------------------
imslug_opengl3_init_info imslug_opengl3_init_info_default(void)
{
    const ImSlug::ImSlugOpenGL3InitInfo cpp;
    imslug_opengl3_init_info info;
    info.GlslVersionString = cpp.GlslVersionString;
    info.MaxInstancesPerFrame = cpp.MaxInstancesPerFrame;
    info.NumFramesInFlight = cpp.NumFramesInFlight;
    info.GetProcAddress = (imslug_opengl3_get_proc_address_fn)cpp.GetProcAddress;
    info.GetProcAddressUserData = cpp.GetProcAddressUserData;
    return info;
}

int imslug_opengl3_init(const imslug_opengl3_init_info* info)
{
    if (info == NULL)
        return 0;
    ImSlug::ImSlugOpenGL3InitInfo cpp;
    cpp.GlslVersionString = info->GlslVersionString;
    cpp.MaxInstancesPerFrame = info->MaxInstancesPerFrame;
    cpp.NumFramesInFlight = info->NumFramesInFlight;
    cpp.GetProcAddress = (ImSlug::ImSlugOpenGL3GetProcAddressFn)info->GetProcAddress;
    cpp.GetProcAddressUserData = info->GetProcAddressUserData;
    return ImSlug::ImSlugOpenGL3Init(&cpp) ? 1 : 0;
}

void imslug_opengl3_shutdown(void)
{
    ImSlug::ImSlugOpenGL3Shutdown();
}

int imslug_opengl3_upload_pools(imslug_pools* pools)
{
    return ImSlug::ImSlugOpenGL3UploadPools((ImSlug::ImSlugPools*)pools) ? 1 : 0;
}

uint32_t imslug_opengl3_get_frames_in_flight(void)
{
    return ImSlug::ImSlugOpenGL3GetFramesInFlight();
}

uint32_t imslug_opengl3_get_instance_capacity(void)
{
    return ImSlug::ImSlugOpenGL3GetInstanceCapacity();
}

imslug_glyph_instance* imslug_opengl3_get_instance_data(uint32_t frame_index)
{
    return (imslug_glyph_instance*)ImSlug::ImSlugOpenGL3GetInstanceData(frame_index);
}

int imslug_opengl3_reserve_instances(uint32_t frame_index, uint32_t required_instances)
{
    return ImSlug::ImSlugOpenGL3ReserveInstances(frame_index, required_instances) ? 1 : 0;
}

void imslug_opengl3_upload_instances(uint32_t frame_index, uint32_t instance_count)
{
    ImSlug::ImSlugOpenGL3UploadInstances(frame_index, instance_count);
}

void imslug_opengl3_draw(const imslug_opengl3_draw_info* draw)
{
    if (draw == NULL)
        return;
    ImSlug::ImSlugOpenGL3DrawInfo cpp;
    for (int i = 0; i < 16; ++i)
        cpp.MVP[i] = draw->MVP[i];
    cpp.ViewportWidth = draw->ViewportWidth;
    cpp.ViewportHeight = draw->ViewportHeight;
    cpp.ScissorX = draw->ScissorX;
    cpp.ScissorY = draw->ScissorY;
    cpp.ScissorW = draw->ScissorW;
    cpp.ScissorH = draw->ScissorH;
    cpp.FrameIndex = draw->FrameIndex;
    cpp.FirstInstance = draw->FirstInstance;
    cpp.InstanceCount = draw->InstanceCount;
    cpp.DebugFlags = draw->DebugFlags;
    ImSlug::ImSlugOpenGL3Draw(&cpp);
}

int imslug_opengl3_create_device_objects(void)
{
    return ImSlug::ImSlugOpenGL3CreateDeviceObjects() ? 1 : 0;
}

void imslug_opengl3_destroy_device_objects(void)
{
    ImSlug::ImSlugOpenGL3DestroyDeviceObjects();
}

void imslug_opengl3_build_ortho_mvp(float display_pos_x, float display_pos_y, float display_width, float display_height,
                                    float out_mvp[16], int upper_left_clip_origin)
{
    ImSlug::ImSlugOpenGL3BuildOrthoMVP(display_pos_x, display_pos_y, display_width, display_height, out_mvp,
                                       upper_left_clip_origin != 0);
}

// ---------------------------------------------------------------------------
// Dear ImGui adapter
// ---------------------------------------------------------------------------
#if !defined(IMSLUG_NO_IMGUI)

int imslug_impl_opengl3_init(const char* glsl_version)
{
    return ImSlug::ImSlug_ImplOpenGL3_Init(glsl_version) ? 1 : 0;
}

int imslug_impl_opengl3_init_from_info(const imslug_opengl3_init_info* info)
{
    if (info == NULL)
        return 0;
    ImSlug::ImSlugOpenGL3InitInfo cpp;
    cpp.GlslVersionString = info->GlslVersionString;
    cpp.MaxInstancesPerFrame = info->MaxInstancesPerFrame;
    cpp.NumFramesInFlight = info->NumFramesInFlight;
    cpp.GetProcAddress = (ImSlug::ImSlugOpenGL3GetProcAddressFn)info->GetProcAddress;
    cpp.GetProcAddressUserData = info->GetProcAddressUserData;
    return ImSlug::ImSlug_ImplOpenGL3_InitFromOpenGL3Info(&cpp) ? 1 : 0;
}

void imslug_impl_opengl3_shutdown(void)
{
    ImSlug::ImSlug_ImplOpenGL3_Shutdown();
}

int imslug_impl_opengl3_upload_pools(void)
{
    return ImSlug::ImSlug_ImplOpenGL3_UploadPools() ? 1 : 0;
}

void imslug_impl_opengl3_render_draw_data(ImDrawData* draw_data)
{
    ImSlug::ImSlug_ImplOpenGL3_RenderDrawData(draw_data);
}

void imslug_impl_opengl3_set_debug_overlay(uint32_t overlay_flags)
{
    ImSlug::ImSlug_ImplOpenGL3_SetDebugOverlay(overlay_flags);
}

uint32_t imslug_impl_opengl3_get_debug_overlay(void)
{
    return ImSlug::ImSlug_ImplOpenGL3_GetDebugOverlay();
}

#endif  // !defined(IMSLUG_NO_IMGUI)
