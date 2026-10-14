// imslug — implementation.
//
// imslug_c_vulkan.cpp — Vulkan C API: the C entry points declared by
// imslug_c.h when Vulkan headers are visible. Thin mirror of the ImGui-facing
// backend entry points (backends/imslug_impl_vulkan.h) through the additive
// ImSlug_ImplVulkan_InitFromVulkanInfo path, so the C API never touches an
// ImGui_ImplVulkan_InitInfo.
//
// Lives in backends/ (invariant 7: the core stays backend-independent).

#include <vulkan/vulkan.h>

#include "imslug_c.h"  // after vulkan.h: the Vulkan declarations are visible

#include "imslug.h"
#include "imslug_impl_vulkan.h"

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
imslug_vulkan_init_info imslug_vulkan_init_info_default(void)
{
    // Default-constructed ImSlugVulkanInitInfo is the source of the defaults
    // (zeroed handles, MSAASamples = VK_SAMPLE_COUNT_1_BIT).
    const ImSlug::ImSlugVulkanInitInfo cpp;
    imslug_vulkan_init_info info;
    info.PhysicalDevice = cpp.PhysicalDevice;
    info.Device = cpp.Device;
    info.Queue = cpp.Queue;
    info.QueueFamily = cpp.QueueFamily;
    info.Allocator = cpp.Allocator;
    info.ImageCount = cpp.ImageCount;
    info.MSAASamples = cpp.MSAASamples;
    info.PipelineCache = cpp.PipelineCache;
    info.RenderPass = cpp.RenderPass;
    info.Subpass = cpp.Subpass;
    info.UseDynamicRendering = cpp.UseDynamicRendering ? 1 : 0;
    info.PipelineRenderingCreateInfo = cpp.PipelineRenderingCreateInfo;
    info.MaxInstancesPerFrame = cpp.MaxInstancesPerFrame;
    return info;
}

int imslug_impl_vulkan_init(imslug_vulkan_init_info init_info)
{
    ImSlug::ImSlugVulkanInitInfo cpp;
    cpp.PhysicalDevice = init_info.PhysicalDevice;
    cpp.Device = init_info.Device;
    cpp.Queue = init_info.Queue;
    cpp.QueueFamily = init_info.QueueFamily;
    cpp.Allocator = init_info.Allocator;
    cpp.ImageCount = init_info.ImageCount;
    cpp.MSAASamples = init_info.MSAASamples;
    cpp.PipelineCache = init_info.PipelineCache;
    cpp.RenderPass = init_info.RenderPass;
    cpp.Subpass = init_info.Subpass;
    cpp.UseDynamicRendering = init_info.UseDynamicRendering != 0;
    cpp.PipelineRenderingCreateInfo = init_info.PipelineRenderingCreateInfo;
    cpp.MaxInstancesPerFrame = init_info.MaxInstancesPerFrame;
    return ImSlug::ImSlug_ImplVulkan_InitFromVulkanInfo(&cpp) ? 1 : 0;
}

void imslug_impl_vulkan_shutdown(void)
{
    ImSlug::ImSlug_ImplVulkan_Shutdown();
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------
void imslug_impl_vulkan_upload_pools(VkCommandBuffer cmd)
{
    ImSlug::ImSlug_ImplVulkan_UploadPools(cmd);
}

void imslug_impl_vulkan_render_draw_data(ImDrawData* draw_data, VkCommandBuffer cmd, VkPipeline main_pipeline)
{
    ImSlug::ImSlug_ImplVulkan_RenderDrawData(draw_data, cmd, main_pipeline);
}

// ---------------------------------------------------------------------------
// Debug overlay
// ---------------------------------------------------------------------------
void imslug_impl_vulkan_set_debug_overlay(uint32_t overlay_flags)
{
    ImSlug::ImSlug_ImplVulkan_SetDebugOverlay(overlay_flags);
}

uint32_t imslug_impl_vulkan_get_debug_overlay(void)
{
    return ImSlug::ImSlug_ImplVulkan_GetDebugOverlay();
}
