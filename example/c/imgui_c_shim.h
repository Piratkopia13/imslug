// imslug — example/c/imgui_c_shim.h
//
// Minimal C bridge to the Dear ImGui C++ API for the C example. A C app
// that wants Dear ImGui itself normally links cimgui (generated C bindings);
// this example does not vendor cimgui, so this shim exposes exactly the entry
// points example/c/main.c needs. It is example-local — not part of imslug's
// API (imslug_c.h is).
//
// The shim is compiled as C++ (imgui_c_shim.cpp); main.c stays pure C11 and
// only sees this C header.

#pragma once

#include <stdint.h>

#ifdef IMSLUG_C_EXAMPLE_VULKAN
#include <vulkan/vulkan.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    // ---------------------------------------------------------------------------
    // Context
    // ---------------------------------------------------------------------------
    // Creates/destroys the ImGui context (ImGui::CreateContext / DestroyContext).
    // Returns 1 on success.
    int ex_imgui_create_context(void);
    void ex_imgui_destroy_context(void);

    // ---------------------------------------------------------------------------
    // CPU smoke mode (no GPU)
    // ---------------------------------------------------------------------------
    // Sets io.DisplaySize + io.DeltaTime and disables imgui.ini.
    void ex_imgui_set_display_size(float width, float height);

    // Marks the renderer as texture-capable (ImGuiBackendFlags_RendererHasTextures
    // — what the stock Vulkan backend sets at init) and installs no-op platform
    // reset/sampler callbacks, so a frame can run headless without a GPU. The
    // imslug pre-pass still refuses to run without a backend module registered;
    // this hook only lets ImGui::NewFrame/Render run.
    void ex_imgui_install_fake_renderer_hooks(void);

    // ---------------------------------------------------------------------------
    // Frame
    // ---------------------------------------------------------------------------
    // ImGui_ImplVulkan_NewFrame() (when the Vulkan shim is initialized) followed
    // by ImGui::NewFrame().
    void ex_imgui_new_frame(float delta_time);

    // A minimal window: one line with `text`, one with frame/version info.
    void ex_imgui_ui_text(const char* text);

    // Sets io.FontDefault (the font every widget uses). Pass the ImFont*
    // returned by imslug_add_font_from_* to render the text through Slug.
    void ex_imgui_set_default_font(void* font);

    // ImGui::Render().
    void ex_imgui_render(void);

    // ImGui::GetDrawData() as a void* (main.c casts it to the ImDrawData forward
    // declaration from imslug_c.h).
    void* ex_imgui_get_draw_data(void);

// ---------------------------------------------------------------------------
// Vulkan (stock Dear ImGui Vulkan backend) — compiled when Vulkan is available
// ---------------------------------------------------------------------------
#ifdef IMSLUG_C_EXAMPLE_VULKAN
    // The subset of ImGui_ImplVulkan_InitInfo the example fills; the shim builds
    // the full C++ struct (DescriptorPoolSize > 0 lets the backend create its own
    // descriptor pool).
    typedef struct ex_imgui_vulkan_init_info
    {
        VkInstance Instance;
        VkPhysicalDevice PhysicalDevice;
        VkDevice Device;
        uint32_t QueueFamily;
        VkQueue Queue;
        uint32_t MinImageCount;
        uint32_t ImageCount;
        VkRenderPass RenderPass;
        uint32_t Subpass;
        VkSampleCountFlagBits MSAASamples;
        const VkAllocationCallbacks* Allocator;
    } ex_imgui_vulkan_init_info;

    // ImGui_ImplVulkan_Init with the fields above; returns 1 on success.
    int ex_imgui_vulkan_init(const ex_imgui_vulkan_init_info* info);

    // ImGui_ImplVulkan_Shutdown.
    void ex_imgui_vulkan_shutdown(void);
#endif  // IMSLUG_C_EXAMPLE_VULKAN

#ifdef __cplusplus
}
#endif
