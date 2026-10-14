// imslug — example/c/main.c
//
// C API example (M6): a pure-C11 consumer of imslug_c.h.
//
// Two modes:
//   * default (no GPU needed): creates the ImGui context, initializes imslug,
//     adds a font, runs one headless frame (NewFrame/Render through the
//     shim), checks the statistics and shuts down cleanly. This is what CI
//     runs on every platform (ctest `imslug_c_example`).
//   * --vulkan (when built with Vulkan): a full offscreen frame — headless
//     Vulkan device, stock ImGui Vulkan backend, imslug Vulkan backend,
//     imslug_impl_vulkan_render_draw_data into an offscreen image, readback +
//     ink check, optional --screenshot file.ppm, validation-layer report.
//     Exits 77 (ctest SKIP_RETURN_CODE) when no usable Vulkan device exists.
//
// The C example talks to Dear ImGui through example/c/imgui_c_shim.* (a tiny
// example-local C bridge); imslug itself is used exclusively through its C API.
//
// Usage:
//   imslug_example_c [--font <path>] [--size <px>] [--vulkan]
//                    [--screenshot <file.ppm>] [--no-validation]
//
// Environment:
//   IMSLUG_VALIDATION=0   disable the Vulkan validation layers (--vulkan)
//   IMSLUG_VK_DEVICE=<n>  force physical device index n (--vulkan)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#ifdef IMSLUG_C_EXAMPLE_VULKAN
#include <vulkan/vulkan.h>
#endif

#include "imslug_c.h"  // the C API (Vulkan entry points declared: vulkan.h above)

#include "imgui_c_shim.h"

#ifndef IMSLUG_C_EXAMPLE_FONT
#define IMSLUG_C_EXAMPLE_FONT "tests/fonts/Roboto-Regular.ttf"
#endif

#define EX_VK_SKIP 77  // ctest SKIP_RETURN_CODE

// ---------------------------------------------------------------------------
// CPU smoke (no GPU)
// ---------------------------------------------------------------------------
static int RunCpuSmoke(const char* font_path, float font_size)
{
    printf("[c-example] mode: CPU smoke (no GPU)\n");

    ex_imgui_set_display_size(1024.0f, 640.0f);
    ex_imgui_install_fake_renderer_hooks();

    const imslug_config config = imslug_config_default();
    if (!imslug_init(config))
    {
        fprintf(stderr, "[c-example] FAIL: imslug_init\n");
        return 1;
    }

    ImFont* font = imslug_add_font_from_file_ttf(font_path, font_size, NULL, NULL);
    if (font == NULL)
    {
        fprintf(stderr, "[c-example] FAIL: imslug_add_font_from_file_ttf('%s')\n", font_path);
        imslug_shutdown();
        return 1;
    }
    if (!imslug_is_registered(font))
    {
        fprintf(stderr, "[c-example] FAIL: font is not Slug-registered\n");
        imslug_shutdown();
        return 1;
    }

    // One headless frame: the glyphs bake when the text is rendered, so the
    // glyph-slab statistic proves the C API reached the whole pipeline.
    ex_imgui_new_frame(1.0f / 60.0f);
    ex_imgui_ui_text("imslug C API smoke (CPU, no GPU)");
    ex_imgui_render();

    imslug_statistics stats;
    imslug_get_statistics(&stats);
    printf("[c-example] font registered, %u glyph slab(s) after one frame\n", stats.GlyphSlabs);

    if (stats.GlyphSlabs == 0)
    {
        fprintf(stderr, "[c-example] FAIL: no glyph slabs (font pipeline did not run)\n");
        imslug_shutdown();
        return 1;
    }

    imslug_shutdown();
    printf("[c-example] OK (version %s, num %u)\n", imslug_version_string(), imslug_version_num());
    return 0;
}

#ifdef IMSLUG_C_EXAMPLE_VULKAN
// ---------------------------------------------------------------------------
// Headless Vulkan (offscreen frame)
// ---------------------------------------------------------------------------
typedef struct ex_vk_state
{
    VkInstance Instance;
    VkDebugUtilsMessengerEXT DebugMessenger;
    VkPhysicalDevice PhysicalDevice;
    VkDevice Device;
    VkQueue Queue;
    uint32_t QueueFamily;
    VkRenderPass RenderPass;
    VkImage Image;
    VkDeviceMemory ImageMemory;
    VkImageView ImageView;
    VkFramebuffer Framebuffer;
    VkBuffer Readback;
    VkDeviceMemory ReadbackMemory;
    VkCommandPool CommandPool;
    VkCommandBuffer CommandBuffer;
    VkFence Fence;
    uint32_t Width;
    uint32_t Height;
    int ValidationRequested;  // from --no-validation / the environment
    int ValidationEnabled;
    int ValidationMessages;
    int ValidationErrors;
} ex_vk_state;

static VKAPI_ATTR VkBool32 VKAPI_CALL ExVkDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT types,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                        void* user_data)
{
    ex_vk_state* state = (ex_vk_state*)user_data;
    (void)types;
    state->ValidationMessages++;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        state->ValidationErrors++;
    fprintf(stderr, "[validation] %s\n", data != NULL && data->pMessage != NULL ? data->pMessage : "(no message)");
    return VK_FALSE;
}

static uint32_t ExVkFindMemoryType(const ex_vk_state* state, uint32_t type_bits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(state->PhysicalDevice, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) != 0 && (memory.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    return UINT32_MAX;
}

static int ExVkFormatSupported(VkPhysicalDevice device)
{
    VkFormatProperties curve;
    VkFormatProperties band;
    VkFormatProperties target;
    vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_R16G16B16A16_SFLOAT, &curve);
    vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_R16G16_UINT, &band);
    vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_R8G8B8A8_UNORM, &target);
    const VkFormatFeatureFlags targetRequired =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    return (curve.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 &&
           (band.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 &&
           (target.optimalTilingFeatures & targetRequired) == targetRequired;
}

static int ExVkCreateInstance(ex_vk_state* state)
{
    uint32_t extension_count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL);
    VkExtensionProperties* extensions =
        (VkExtensionProperties*)malloc(sizeof(VkExtensionProperties) * (extension_count ? extension_count : 1));
    int has_debug_utils = 0;
    if (extensions != NULL && extension_count > 0)
    {
        vkEnumerateInstanceExtensionProperties(NULL, &extension_count, extensions);
        for (uint32_t i = 0; i < extension_count; ++i)
            if (strcmp(extensions[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                has_debug_utils = 1;
    }
    free(extensions);

    const char* env = getenv("IMSLUG_VALIDATION");
    state->ValidationEnabled = has_debug_utils && state->ValidationRequested && (env == NULL || strcmp(env, "0") != 0);

    const char* layers[1] = { "VK_LAYER_KHRONOS_validation" };
    const char* extensions_enabled[1] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    if (state->ValidationEnabled)
    {
        uint32_t layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, NULL);
        VkLayerProperties* available =
            (VkLayerProperties*)malloc(sizeof(VkLayerProperties) * (layer_count ? layer_count : 1));
        int found = 0;
        if (available != NULL && layer_count > 0)
        {
            vkEnumerateInstanceLayerProperties(&layer_count, available);
            for (uint32_t i = 0; i < layer_count; ++i)
                if (strcmp(available[i].layerName, layers[0]) == 0)
                    found = 1;
        }
        free(available);
        if (!found)
        {
            printf("[c-example] VK_LAYER_KHRONOS_validation not available - running without validation\n");
            state->ValidationEnabled = 0;
        }
    }

    VkApplicationInfo application_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    application_info.pApplicationName = "imslug C example";
    application_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    // The imslug shaders are SPIR-V 1.5 (vulkan1.2 target env); 1.2 is also
    // what the golden harness and the C++ example use.
    application_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo create_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    create_info.pApplicationInfo = &application_info;
    if (state->ValidationEnabled)
    {
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        create_info.enabledExtensionCount = 1;
        create_info.ppEnabledExtensionNames = extensions_enabled;
    }

    if (vkCreateInstance(&create_info, NULL, &state->Instance) != VK_SUCCESS)
    {
        state->Instance = VK_NULL_HANDLE;
        return 0;
    }

    if (state->ValidationEnabled)
    {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        messenger_info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = ExVkDebugCallback;
        messenger_info.pUserData = state;
        PFN_vkCreateDebugUtilsMessengerEXT create_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            state->Instance, "vkCreateDebugUtilsMessengerEXT");
        if (create_messenger != NULL)
            create_messenger(state->Instance, &messenger_info, NULL, &state->DebugMessenger);
    }
    return 1;
}

static int ExVkCreateDevice(ex_vk_state* state)
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(state->Instance, &device_count, NULL);
    if (device_count == 0)
        return 0;
    VkPhysicalDevice* devices = (VkPhysicalDevice*)malloc(sizeof(VkPhysicalDevice) * device_count);
    if (devices == NULL)
        return 0;
    vkEnumeratePhysicalDevices(state->Instance, &device_count, devices);

    const char* env = getenv("IMSLUG_VK_DEVICE");
    const int forced = env != NULL && *env != '\0' ? atoi(env) : -1;

    int best = -1;
    int best_score = -1;
    for (uint32_t i = 0; i < device_count; ++i)
    {
        if (forced >= 0 && (int)i != forced)
            continue;
        if (!ExVkFormatSupported(devices[i]))
            continue;

        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, NULL);
        VkQueueFamilyProperties* queues =
            (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * (queue_count ? queue_count : 1));
        uint32_t graphics_family = UINT32_MAX;
        if (queues != NULL && queue_count > 0)
        {
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, queues);
            for (uint32_t q = 0; q < queue_count; ++q)
                if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
                {
                    graphics_family = q;
                    break;
                }
        }
        free(queues);
        if (graphics_family == UINT32_MAX)
            continue;

        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > best_score)
        {
            best_score = score;
            best = (int)i;
        }
    }

    if (best < 0)
    {
        free(devices);
        return 0;
    }

    state->PhysicalDevice = devices[best];
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(state->PhysicalDevice, &properties);
    printf("[c-example] device %d: %s (Vulkan %u.%u.%u)\n", best, properties.deviceName,
           VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
           VK_VERSION_PATCH(properties.apiVersion));
    free(devices);

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(state->PhysicalDevice, &queue_count, NULL);
    VkQueueFamilyProperties* queues =
        (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * (queue_count ? queue_count : 1));
    if (queues == NULL)
        return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(state->PhysicalDevice, &queue_count, queues);
    state->QueueFamily = UINT32_MAX;
    for (uint32_t q = 0; q < queue_count; ++q)
        if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            state->QueueFamily = q;
            break;
        }
    free(queues);
    if (state->QueueFamily == UINT32_MAX)
        return 0;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue_info.queueFamilyIndex = state->QueueFamily;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo create_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;
    if (vkCreateDevice(state->PhysicalDevice, &create_info, NULL, &state->Device) != VK_SUCCESS)
    {
        state->Device = VK_NULL_HANDLE;
        return 0;
    }
    vkGetDeviceQueue(state->Device, state->QueueFamily, 0, &state->Queue);
    return 1;
}

static int ExVkCreateRenderObjects(ex_vk_state* state)
{
    // Render pass: one RGBA8 color attachment, cleared, left in TRANSFER_SRC
    // so the frame can be copied out for the readback / screenshot.
    VkAttachmentDescription color_attachment;
    memset(&color_attachment, 0, sizeof(color_attachment));
    color_attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference color_reference;
    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass;
    memset(&subpass, 0, sizeof(subpass));
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    VkSubpassDependency dependency;
    memset(&dependency, 0, sizeof(dependency));
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo render_pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &color_attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;
    if (vkCreateRenderPass(state->Device, &render_pass_info, NULL, &state->RenderPass) != VK_SUCCESS)
        return 0;

    // Target image.
    VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = state->Width;
    image_info.extent.height = state->Height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(state->Device, &image_info, NULL, &state->Image) != VK_SUCCESS)
        return 0;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(state->Device, state->Image, &requirements);
    VkMemoryAllocateInfo allocate_info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex =
        ExVkFindMemoryType(state, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate_info.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(state->Device, &allocate_info, NULL, &state->ImageMemory) != VK_SUCCESS)
        return 0;
    vkBindImageMemory(state->Device, state->Image, state->ImageMemory, 0);

    VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view_info.image = state->Image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(state->Device, &view_info, NULL, &state->ImageView) != VK_SUCCESS)
        return 0;

    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    framebuffer_info.renderPass = state->RenderPass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &state->ImageView;
    framebuffer_info.width = state->Width;
    framebuffer_info.height = state->Height;
    framebuffer_info.layers = 1;
    if (vkCreateFramebuffer(state->Device, &framebuffer_info, NULL, &state->Framebuffer) != VK_SUCCESS)
        return 0;

    // Host-visible readback buffer.
    VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer_info.size = (VkDeviceSize)state->Width * state->Height * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(state->Device, &buffer_info, NULL, &state->Readback) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(state->Device, state->Readback, &requirements);
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = ExVkFindMemoryType(
        state, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocate_info.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(state->Device, &allocate_info, NULL, &state->ReadbackMemory) != VK_SUCCESS)
        return 0;
    vkBindBufferMemory(state->Device, state->Readback, state->ReadbackMemory, 0);

    VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = state->QueueFamily;
    if (vkCreateCommandPool(state->Device, &pool_info, NULL, &state->CommandPool) != VK_SUCCESS)
        return 0;

    VkCommandBufferAllocateInfo allocate_buffer_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate_buffer_info.commandPool = state->CommandPool;
    allocate_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_buffer_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(state->Device, &allocate_buffer_info, &state->CommandBuffer) != VK_SUCCESS)
        return 0;

    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(state->Device, &fence_info, NULL, &state->Fence) != VK_SUCCESS)
        return 0;
    return 1;
}

static void ExVkDestroy(ex_vk_state* state)
{
    if (state->Device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(state->Device);
        if (state->Fence != VK_NULL_HANDLE)
            vkDestroyFence(state->Device, state->Fence, NULL);
        if (state->CommandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(state->Device, state->CommandPool, NULL);
        if (state->Readback != VK_NULL_HANDLE)
            vkDestroyBuffer(state->Device, state->Readback, NULL);
        if (state->ReadbackMemory != VK_NULL_HANDLE)
            vkFreeMemory(state->Device, state->ReadbackMemory, NULL);
        if (state->Framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(state->Device, state->Framebuffer, NULL);
        if (state->ImageView != VK_NULL_HANDLE)
            vkDestroyImageView(state->Device, state->ImageView, NULL);
        if (state->Image != VK_NULL_HANDLE)
            vkDestroyImage(state->Device, state->Image, NULL);
        if (state->ImageMemory != VK_NULL_HANDLE)
            vkFreeMemory(state->Device, state->ImageMemory, NULL);
        if (state->RenderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(state->Device, state->RenderPass, NULL);
        vkDestroyDevice(state->Device, NULL);
    }
    if (state->Instance != VK_NULL_HANDLE)
    {
        if (state->DebugMessenger != VK_NULL_HANDLE)
        {
            PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
                (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(state->Instance,
                                                                           "vkDestroyDebugUtilsMessengerEXT");
            if (destroy_messenger != NULL)
                destroy_messenger(state->Instance, state->DebugMessenger, NULL);
        }
        vkDestroyInstance(state->Instance, NULL);
    }
    memset(state, 0, sizeof(*state));
}

static int ExVkRenderFrame(ex_vk_state* state, ImDrawData* draw_data, int readback)
{
    vkResetCommandBuffer(state->CommandBuffer, 0);
    VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(state->CommandBuffer, &begin_info) != VK_SUCCESS)
        return 0;

    // Glyph-pool uploads are recorded outside the render pass (M3 contract).
    imslug_impl_vulkan_upload_pools(state->CommandBuffer);

    VkClearValue clear;
    clear.color.float32[0] = 0.0f;
    clear.color.float32[1] = 0.0f;
    clear.color.float32[2] = 0.0f;
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    pass_info.renderPass = state->RenderPass;
    pass_info.framebuffer = state->Framebuffer;
    pass_info.renderArea.extent.width = state->Width;
    pass_info.renderArea.extent.height = state->Height;
    pass_info.clearValueCount = 1;
    pass_info.pClearValues = &clear;
    vkCmdBeginRenderPass(state->CommandBuffer, &pass_info, VK_SUBPASS_CONTENTS_INLINE);

    // One frame through the C API: pre-pass + stock backend rendering.
    imslug_impl_vulkan_render_draw_data(draw_data, state->CommandBuffer, VK_NULL_HANDLE);

    vkCmdEndRenderPass(state->CommandBuffer);

    if (readback)
    {
        VkBufferImageCopy copy = { 0 };
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = state->Width;
        copy.imageExtent.height = state->Height;
        copy.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(state->CommandBuffer, state->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               state->Readback, 1, &copy);
    }

    if (vkEndCommandBuffer(state->CommandBuffer) != VK_SUCCESS)
        return 0;

    VkSubmitInfo submit_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &state->CommandBuffer;
    if (vkQueueSubmit(state->Queue, 1, &submit_info, state->Fence) != VK_SUCCESS)
        return 0;
    if (vkWaitForFences(state->Device, 1, &state->Fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return 0;
    vkResetFences(state->Device, 1, &state->Fence);
    return 1;
}

static int ExVkWritePpm(const char* path, const uint8_t* pixels, uint32_t width, uint32_t height)
{
    FILE* file = fopen(path, "wb");
    if (file == NULL)
        return 0;
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (uint32_t i = 0; i < width * height; ++i)
    {
        const uint8_t rgb[3] = { pixels[i * 4 + 0], pixels[i * 4 + 1], pixels[i * 4 + 2] };
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
    return 1;
}

static int RunVulkan(const char* font_path, float font_size, const char* screenshot_path, uint32_t width,
                     uint32_t height, int validation_requested)
{
    printf("[c-example] mode: headless Vulkan frame (%ux%u)\n", width, height);

    ex_vk_state vk;
    memset(&vk, 0, sizeof(vk));
    vk.Width = width;
    vk.Height = height;
    vk.ValidationRequested = validation_requested;

    if (!ExVkCreateInstance(&vk))
    {
        fprintf(stderr, "[c-example] FAIL: vkCreateInstance\n");
        ExVkDestroy(&vk);
        return 1;
    }
    if (!ExVkCreateDevice(&vk))
    {
        printf("[c-example] no usable Vulkan device - skipped (exit %d)\n", EX_VK_SKIP);
        ExVkDestroy(&vk);
        return EX_VK_SKIP;
    }
    if (!ExVkCreateRenderObjects(&vk))
    {
        fprintf(stderr, "[c-example] FAIL: creating render objects\n");
        ExVkDestroy(&vk);
        return 1;
    }

    // Stock Dear ImGui Vulkan backend first, then the imslug modules (the
    // documented order; the stock backend sets RendererHasTextures).
    ex_imgui_set_display_size((float)width, (float)height);
    ex_imgui_vulkan_init_info imgui_info;
    memset(&imgui_info, 0, sizeof(imgui_info));
    imgui_info.Instance = vk.Instance;
    imgui_info.PhysicalDevice = vk.PhysicalDevice;
    imgui_info.Device = vk.Device;
    imgui_info.QueueFamily = vk.QueueFamily;
    imgui_info.Queue = vk.Queue;
    imgui_info.MinImageCount = 2;
    imgui_info.ImageCount = 2;
    imgui_info.RenderPass = vk.RenderPass;
    imgui_info.Subpass = 0;
    imgui_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    imgui_info.Allocator = NULL;
    if (!ex_imgui_vulkan_init(&imgui_info))
    {
        fprintf(stderr, "[c-example] FAIL: ex_imgui_vulkan_init\n");
        ExVkDestroy(&vk);
        return 1;
    }

    const imslug_config config = imslug_config_default();
    if (!imslug_init(config))
    {
        fprintf(stderr, "[c-example] FAIL: imslug_init\n");
        ex_imgui_vulkan_shutdown();
        ExVkDestroy(&vk);
        return 1;
    }

    imslug_vulkan_init_info vulkan_info = imslug_vulkan_init_info_default();
    vulkan_info.PhysicalDevice = vk.PhysicalDevice;
    vulkan_info.Device = vk.Device;
    vulkan_info.Queue = vk.Queue;
    vulkan_info.QueueFamily = vk.QueueFamily;
    vulkan_info.ImageCount = 2;
    vulkan_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    vulkan_info.RenderPass = vk.RenderPass;
    vulkan_info.Subpass = 0;
    if (!imslug_impl_vulkan_init(vulkan_info))
    {
        fprintf(stderr, "[c-example] FAIL: imslug_impl_vulkan_init\n");
        imslug_shutdown();
        ex_imgui_vulkan_shutdown();
        ExVkDestroy(&vk);
        return 1;
    }

    ImFont* font = imslug_add_font_from_file_ttf(font_path, font_size, NULL, NULL);
    if (font == NULL || !imslug_is_registered(font))
    {
        fprintf(stderr, "[c-example] FAIL: adding Slug font '%s'\n", font_path);
        imslug_impl_vulkan_shutdown();
        imslug_shutdown();
        ex_imgui_vulkan_shutdown();
        ExVkDestroy(&vk);
        return 1;
    }
    ex_imgui_set_default_font(font);  // render the UI text through Slug

    // Three frames: the first establishes the auto-sized window layout and
    // lets the font atlas build/bake (mirrors the golden harness); only the
    // last frame is read back.
    const int kFrames = 3;
    for (int frame = 0; frame < kFrames; ++frame)
    {
        ex_imgui_new_frame(1.0f / 60.0f);
        ex_imgui_ui_text("imslug C API: Slug-rendered text (hello, world!)");
        ex_imgui_render();
        ImDrawData* draw_data = (ImDrawData*)ex_imgui_get_draw_data();
        if (draw_data == NULL || !ExVkRenderFrame(&vk, draw_data, frame + 1 == kFrames))
        {
            fprintf(stderr, "[c-example] FAIL: rendering frame %d\n", frame);
            imslug_impl_vulkan_shutdown();
            imslug_shutdown();
            ex_imgui_vulkan_shutdown();
            ExVkDestroy(&vk);
            return 1;
        }
    }

    imslug_statistics stats;
    imslug_get_statistics(&stats);

    // Read back and count ink (the clear is black; text and chrome are not).
    void* mapped = NULL;
    if (vkMapMemory(vk.Device, vk.ReadbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
    {
        fprintf(stderr, "[c-example] FAIL: vkMapMemory\n");
        imslug_impl_vulkan_shutdown();
        imslug_shutdown();
        ex_imgui_vulkan_shutdown();
        ExVkDestroy(&vk);
        return 1;
    }
    const uint8_t* pixels = (const uint8_t*)mapped;
    uint32_t ink_pixels = 0;
    for (uint32_t i = 0; i < width * height; ++i)
        if (pixels[i * 4 + 0] > 64 || pixels[i * 4 + 1] > 64 || pixels[i * 4 + 2] > 64)
            ink_pixels++;

    if (screenshot_path != NULL)
    {
        if (!ExVkWritePpm(screenshot_path, pixels, width, height))
            fprintf(stderr, "[c-example] warning: could not write '%s'\n", screenshot_path);
        else
            printf("[c-example] screenshot: %s\n", screenshot_path);
    }
    vkUnmapMemory(vk.Device, vk.ReadbackMemory);

    printf("[c-example] frame: %u Slug instance(s), %u batch(es), %u ink pixel(s), validation %d message(s) "
           "(%d error(s))\n",
           stats.Instances, stats.LiveBatches, ink_pixels, vk.ValidationMessages, vk.ValidationErrors);
    const int validation_errors = vk.ValidationErrors;

    imslug_impl_vulkan_shutdown();
    imslug_shutdown();
    ex_imgui_vulkan_shutdown();
    ExVkDestroy(&vk);

    if (stats.Instances == 0 || ink_pixels == 0)
    {
        fprintf(stderr, "[c-example] FAIL: no Slug instances / no ink in the offscreen frame\n");
        return 1;
    }
    if (validation_errors != 0)
    {
        fprintf(stderr, "[c-example] FAIL: %d Vulkan validation error(s)\n", validation_errors);
        return 1;
    }
    printf("[c-example] OK (version %s, num %u)\n", imslug_version_string(), imslug_version_num());
    return 0;
}
#endif  // IMSLUG_C_EXAMPLE_VULKAN

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);  // keep stdout/stderr ordered in CI logs
#if defined(_MSC_VER) && defined(_DEBUG)
    // Debug builds: MSVC's assert() pops a modal dialog that blocks headless
    // runs; report to stderr instead (same as tests/test_util.h).
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

    const char* font_path = IMSLUG_C_EXAMPLE_FONT;
    float font_size = 18.0f;
    const char* screenshot_path = NULL;
    int use_vulkan = 0;
    int validation_requested = 1;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--font") == 0 && i + 1 < argc)
            font_path = argv[++i];
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
            font_size = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--vulkan") == 0)
            use_vulkan = 1;
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshot_path = argv[++i];
        else if (strcmp(argv[i], "--no-validation") == 0)
            validation_requested = 0;
        else if (strcmp(argv[i], "--help") == 0)
        {
            printf("usage: imslug_example_c [--font <path>] [--size <px>] [--vulkan] [--screenshot <file.ppm>] "
                   "[--no-validation]\n");
            return 0;
        }
        else
        {
            fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
            return 2;
        }
    }

    printf("[c-example] imslug %s (C API)\n", imslug_version_string());

    if (!ex_imgui_create_context())
    {
        fprintf(stderr, "[c-example] FAIL: could not create the ImGui context\n");
        return 1;
    }

    int result;
#ifdef IMSLUG_C_EXAMPLE_VULKAN
    if (use_vulkan)
        result = RunVulkan(font_path, font_size, screenshot_path, 1024, 640, validation_requested);
    else
        result = RunCpuSmoke(font_path, font_size);
#else
    (void)validation_requested;
    if (use_vulkan)
    {
        fprintf(stderr, "[c-example] --vulkan: this build has no Vulkan support (configure with a Vulkan SDK)\n");
        result = 2;
    }
    else
        result = RunCpuSmoke(font_path, font_size);
#endif

    ex_imgui_destroy_context();
    return result;
}
