// imslug — example/c/imgui_c_shim.cpp
//
// Implementation of the minimal C bridge declared in imgui_c_shim.h. The only
// C++ in the C example lives here (Dear ImGui itself is C++); main.c stays
// C11 and talks to imslug through imslug_c.h.

#include "imgui_c_shim.h"

#include "imgui.h"

#ifdef IMSLUG_C_EXAMPLE_VULKAN
#include "imgui_impl_vulkan.h"
#endif

namespace
{
    bool g_ContextCreated = false;

#ifdef IMSLUG_C_EXAMPLE_VULKAN
    bool g_VulkanInitialized = false;
#endif

    // No-op stand-in for the stock backend's platform callbacks (the CPU
    // smoke never dispatches them; installing them mirrors what
    // ImGui_ImplVulkan_Init publishes and satisfies the pre-pass hooks).
    void FakePlatformCallback(const ImDrawList*, const ImDrawCmd*) {}
}  // namespace

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------
int ex_imgui_create_context(void)
{
    if (g_ContextCreated)
        return 0;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    g_ContextCreated = true;
    return 1;
}

void ex_imgui_destroy_context(void)
{
    if (!g_ContextCreated)
        return;
    ImGui::DestroyContext();
    g_ContextCreated = false;
}

// ---------------------------------------------------------------------------
// CPU smoke mode
// ---------------------------------------------------------------------------
void ex_imgui_set_display_size(float width, float height)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;  // never touch an ini file from an example run
}

void ex_imgui_install_fake_renderer_hooks(void)
{
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    platformIO.DrawCallback_ResetRenderState = FakePlatformCallback;
    platformIO.DrawCallback_SetSamplerLinear = FakePlatformCallback;
    platformIO.DrawCallback_SetSamplerNearest = FakePlatformCallback;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void ex_imgui_new_frame(float delta_time)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = delta_time > 0.0f ? delta_time : 1.0f / 60.0f;
#ifdef IMSLUG_C_EXAMPLE_VULKAN
    if (g_VulkanInitialized)
        ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();
}

void ex_imgui_set_default_font(void* font)
{
    ImGui::GetIO().FontDefault = (ImFont*)font;
}

void ex_imgui_ui_text(const char* text)
{
    ImGui::SetNextWindowPos(ImVec2(32.0f, 32.0f), ImGuiCond_Always);
    ImGui::Begin("imslug C API", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted(text != nullptr ? text : "");
    ImGui::Text("ImGui %s - frame %d", IMGUI_VERSION, ImGui::GetFrameCount());
    ImGui::End();
}

void ex_imgui_render(void)
{
    ImGui::Render();
}

void* ex_imgui_get_draw_data(void)
{
    return (void*)ImGui::GetDrawData();
}

// ---------------------------------------------------------------------------
// Vulkan (stock Dear ImGui Vulkan backend)
// ---------------------------------------------------------------------------
#ifdef IMSLUG_C_EXAMPLE_VULKAN
int ex_imgui_vulkan_init(const ex_imgui_vulkan_init_info* info)
{
    if (info == nullptr || g_VulkanInitialized)
        return 0;

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.ApiVersion = VK_API_VERSION_1_2;
    initInfo.Instance = info->Instance;
    initInfo.PhysicalDevice = info->PhysicalDevice;
    initInfo.Device = info->Device;
    initInfo.QueueFamily = info->QueueFamily;
    initInfo.Queue = info->Queue;
    // Let the stock backend create its own descriptor pool (sized for its
    // minimum sampled-image/sampler needs; see imgui_impl_vulkan.h).
    initInfo.DescriptorPoolSize = 8;
    initInfo.MinImageCount = info->MinImageCount;
    initInfo.ImageCount = info->ImageCount;
    initInfo.PipelineInfoMain.RenderPass = info->RenderPass;
    initInfo.PipelineInfoMain.Subpass = info->Subpass;
    initInfo.PipelineInfoMain.MSAASamples = info->MSAASamples;
    initInfo.UseDynamicRendering = false;
    initInfo.Allocator = info->Allocator;
    if (!ImGui_ImplVulkan_Init(&initInfo))
        return 0;
    g_VulkanInitialized = true;
    return 1;
}

void ex_imgui_vulkan_shutdown(void)
{
    if (!g_VulkanInitialized)
        return;
    ImGui_ImplVulkan_Shutdown();
    g_VulkanInitialized = false;
}
#endif  // IMSLUG_C_EXAMPLE_VULKAN
