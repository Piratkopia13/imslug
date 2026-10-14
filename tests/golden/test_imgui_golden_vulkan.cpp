// tests/golden/test_imgui_golden_vulkan.cpp — ImGui-integration golden test.
//
// Renders one fixed ImGui scene twice in a headless offscreen Vulkan pass: once
// with the Slug font (our ImFontLoader + callbacks + Slug pipeline) and once
// with the same font through ImGui's own stb loader (atlas path, stock
// pipeline), then compares the two frames. This is the end-to-end check:
//
//   * everything except the font's glyphs goes through the stock backend in
//     both variants, so any difference is text rendering;
//   * layout must be identical: the ink masks of the two frames must be equal
//     after a 1 px dilation (catches shifted/missing/wrongly-scaled glyphs);
//   * anti-aliasing is perceptual: the mean |diff| and the tail of large
//     differences stay within documented budgets (Slug solves coverage
//     analytically, the atlas samples a rasterized bitmap);
//   * the default scene also runs at framebuffer scales 2x and 3x
//     (DisplayFramebufferScale > 1 with the logical display shrunk to match):
//     ImGui lays out in logical pixels while the renderer expands to the
//     framebuffer, so a projection built from framebuffer pixels instead of the
//     display size draws 1/scale too small - and, clipped by the batch scissor,
//     invisible on phones/retina displays.
//
// The default scene covers the acceptance surface: several sizes, tables with
// narrow cells (CPU fine clip on the right), shrinking children (window clip), a
// horizontally scrolled child, bold-ish long labels, shapes next to text and
// ellipsized text. Validation layers are enabled by default and any *error*
// fails the test; SKIP_RETURN_CODE 77 (no usable Vulkan device) lets it skip on
// machines without a driver (lavapipe in CI).
//
// Usage: imslug_tests_imgui_golden [--dynamic-rendering] [--no-validation]

#include "imslug.h"

#include "imslug_impl_vulkan.h"
#include "imslug_internal.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include "../test_util.h"

#include <math.h>
#include <string.h>
#include <vulkan/vulkan.h>

#ifndef IMSLUG_CJK_TEST_FONT
#define IMSLUG_CJK_TEST_FONT "tests/fonts/cjk/NotoSansSC-subset.ttf"
#endif

using namespace ImSlug;

namespace
{
    const int kExitSkip = 77;
    const uint32_t kWidth = 1024;
    const uint32_t kHeight = 640;
    const uint32_t kFrames = 3;  // let the atlas finish building/baking

    // Golden scenes. 0 = the default acceptance scene, 1 = 5k-glyph stress
    // (multi-line, filled frame), 2 = CJK subset (skipped when the fetched
    // font is absent; see tests/fonts/cjk/PROVENANCE.txt).
    const int kSceneCount = 3;

    // Comparison budgets (measured on the RTX 5080):
    //   * the ink masks must agree after a 1 px dilation (layout exactness);
    //   * mean per-channel difference over the whole frame stays small;
    //   * per-pixel differences above the AA band are budgeted *relative to
    //     the ink area* (a fully-filled 5k-glyph frame has 3.7x the ink of the
    //     default scene, so a frame-relative budget would be meaningless): at
    //     most
    //     15% of ink pixels may differ by more than 64 LSB (unhinted analytic
    //     coverage vs hinted atlas bitmaps shifts stem AA, not layout), and at
    //     most 2% may exceed 128 LSB (a structural/artifact tripwire).
    const double kMaxMeanDiff = 8.0;
    const double kMaxMeanInkDiff = 32.0;  // per-ink-pixel mean budget (measured 27.4 at 17 px)
    const int kEdgeDiffThreshold = 64;
    const int kStructuralDiffThreshold = 128;
    const double kMaxEdgeInkFraction = 0.15;
    const double kMaxStructuralInkFraction = 0.02;

    struct Target
    {
        VkImage Image;
        VkDeviceMemory Memory;
        VkImageView View;
        VkFramebuffer Framebuffer;
        VkBuffer Readback;
        VkDeviceMemory ReadbackMemory;
        void* ReadbackMapped;
    };

    struct State
    {
        bool UseDynamicRendering;
        bool ValidationEnabled;
        int ValidationErrors;
        int ValidationMessages;

        VkInstance Instance;
        VkDebugUtilsMessengerEXT DebugMessenger;
        VkPhysicalDevice PhysicalDevice;
        VkDevice Device;
        VkQueue Queue;
        uint32_t QueueFamily;

        VkRenderPass RenderPass;
        VkCommandPool CommandPool;
        VkCommandBuffer CommandBuffer;
        VkFence Fence;
        bool DynamicRenderingEnabled;

        VkDescriptorPool ImGuiDescriptorPool;
        Target RenderTarget;
    };

    State g_State = {};

    // ---------------------------------------------------------------------
    // Validation
    // ---------------------------------------------------------------------
    VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT types,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
    {
        (void)types;
        (void)userData;
        ++g_State.ValidationMessages;
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            ++g_State.ValidationErrors;
            std::printf("  [vulkan error] %s\n", data->pMessage);
        }
        else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        {
            std::printf("  [vulkan warning] %s\n", data->pMessage);
        }
        return VK_FALSE;
    }

    bool EnvFlag(const char* name, bool defaultValue)
    {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
            return defaultValue;
        return !(value[0] == '0' || value[0] == 'f' || value[0] == 'F' || value[0] == 'n' || value[0] == 'N');
    }

    // ---------------------------------------------------------------------
    // Vulkan setup (headless: no surface, no swapchain)
    // ---------------------------------------------------------------------
    bool CreateInstance()
    {
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        vkEnumerateInstanceVersion(&loaderVersion);
        if (g_State.UseDynamicRendering && loaderVersion < VK_API_VERSION_1_3)
        {
            std::printf("  [imslug-golden] loader does not support Vulkan 1.3 - dynamic rendering unavailable\n");
            return false;
        }

        uint32_t extensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
        VkExtensionProperties* extensions =
            (VkExtensionProperties*)std::malloc(sizeof(VkExtensionProperties) * (extensionCount ? extensionCount : 1));
        if (extensions == nullptr)
            return false;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions);
        bool hasDebugUtils = false;
        for (uint32_t i = 0; i < extensionCount; ++i)
            hasDebugUtils =
                hasDebugUtils || std::strcmp(extensions[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
        std::free(extensions);

        const char* enabled[1] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
        const char* layers[1] = { "VK_LAYER_KHRONOS_validation" };
        VkApplicationInfo applicationInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        applicationInfo.pApplicationName = "imslug ImGui golden tests";
        applicationInfo.apiVersion = g_State.UseDynamicRendering ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
        VkInstanceCreateInfo createInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        createInfo.pApplicationInfo = &applicationInfo;
        if (hasDebugUtils && g_State.ValidationEnabled)
        {
            createInfo.enabledExtensionCount = 1;
            createInfo.ppEnabledExtensionNames = enabled;
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = layers;
        }
        const VkResult result = vkCreateInstance(&createInfo, nullptr, &g_State.Instance);
        if (result == VK_ERROR_LAYER_NOT_PRESENT || result == VK_ERROR_EXTENSION_NOT_PRESENT)
        {
            g_State.ValidationEnabled = false;
            createInfo.enabledLayerCount = 0;
            createInfo.enabledExtensionCount = 0;
            if (vkCreateInstance(&createInfo, nullptr, &g_State.Instance) != VK_SUCCESS)
                return false;
        }
        else if (result != VK_SUCCESS)
        {
            return false;
        }

        if (hasDebugUtils && g_State.ValidationEnabled)
        {
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT
            };
            messengerInfo.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messengerInfo.pfnUserCallback = DebugCallback;
            PFN_vkCreateDebugUtilsMessengerEXT createMessenger =
                (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_State.Instance,
                                                                          "vkCreateDebugUtilsMessengerEXT");
            if (createMessenger == nullptr ||
                createMessenger(g_State.Instance, &messengerInfo, nullptr, &g_State.DebugMessenger) != VK_SUCCESS)
                g_State.ValidationEnabled = false;
        }
        else
        {
            g_State.ValidationEnabled = false;
        }
        return true;
    }

    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memoryProperties;
        vkGetPhysicalDeviceMemoryProperties(g_State.PhysicalDevice, &memoryProperties);
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        return UINT32_MAX;
    }

    bool CreateDevice()
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(g_State.Instance, &deviceCount, nullptr);
        if (deviceCount == 0)
            return false;
        VkPhysicalDevice* devices = (VkPhysicalDevice*)std::malloc(sizeof(VkPhysicalDevice) * deviceCount);
        if (devices == nullptr)
            return false;
        vkEnumeratePhysicalDevices(g_State.Instance, &deviceCount, devices);

        // The first device that can render the target and sample the pools.
        for (uint32_t i = 0; i < deviceCount; ++i)
        {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(devices[i], &properties);
            if (g_State.UseDynamicRendering && properties.apiVersion < VK_API_VERSION_1_3)
                continue;
            VkFormatProperties targetProperties;
            vkGetPhysicalDeviceFormatProperties(devices[i], VK_FORMAT_R8G8B8A8_UNORM, &targetProperties);
            const VkFormatFeatureFlags required =
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
            if ((targetProperties.optimalTilingFeatures & required) != required)
                continue;
            std::printf("  [imslug-golden] device: %s (Vulkan %u.%u.%u, dynamic rendering %s)\n", properties.deviceName,
                        VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
                        VK_VERSION_PATCH(properties.apiVersion), g_State.UseDynamicRendering ? "on" : "off");
            g_State.PhysicalDevice = devices[i];
            break;
        }
        std::free(devices);
        if (g_State.PhysicalDevice == VK_NULL_HANDLE)
            return false;

        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_State.PhysicalDevice, &queueCount, nullptr);
        VkQueueFamilyProperties* families =
            (VkQueueFamilyProperties*)std::malloc(sizeof(VkQueueFamilyProperties) * (queueCount ? queueCount : 1));
        if (families == nullptr)
            return false;
        vkGetPhysicalDeviceQueueFamilyProperties(g_State.PhysicalDevice, &queueCount, families);
        bool found = false;
        for (uint32_t i = 0; i < queueCount && !found; ++i)
        {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
            {
                g_State.QueueFamily = i;
                found = true;
            }
        }
        std::free(families);
        if (!found)
            return false;

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = g_State.QueueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkPhysicalDeviceVulkan13Features features13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        if (g_State.UseDynamicRendering)
        {
            VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            query.pNext = &features13;
            vkGetPhysicalDeviceFeatures2(g_State.PhysicalDevice, &query);
            if (features13.dynamicRendering != VK_TRUE)
                return false;  // caller turns this into a skip
            g_State.DynamicRenderingEnabled = true;
        }

        VkDeviceCreateInfo createInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        if (g_State.DynamicRenderingEnabled)
            createInfo.pNext = &features13;
        if (vkCreateDevice(g_State.PhysicalDevice, &createInfo, nullptr, &g_State.Device) != VK_SUCCESS)
        {
            g_State.Device = VK_NULL_HANDLE;
            return false;
        }
        vkGetDeviceQueue(g_State.Device, g_State.QueueFamily, 0, &g_State.Queue);
        return true;
    }

    bool CreateRenderObjects()
    {
        if (!g_State.UseDynamicRendering)
        {
            VkAttachmentDescription colorAttachment = {};
            colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
            colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass = {};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorReference;
            VkSubpassDependency dependency = {};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo renderPassInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            renderPassInfo.attachmentCount = 1;
            renderPassInfo.pAttachments = &colorAttachment;
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;
            renderPassInfo.dependencyCount = 1;
            renderPassInfo.pDependencies = &dependency;
            if (vkCreateRenderPass(g_State.Device, &renderPassInfo, nullptr, &g_State.RenderPass) != VK_SUCCESS)
                return false;
        }

        VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = g_State.QueueFamily;
        if (vkCreateCommandPool(g_State.Device, &poolInfo, nullptr, &g_State.CommandPool) != VK_SUCCESS)
            return false;

        VkCommandBufferAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocateInfo.commandPool = g_State.CommandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(g_State.Device, &allocateInfo, &g_State.CommandBuffer) != VK_SUCCESS)
            return false;

        VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(g_State.Device, &fenceInfo, nullptr, &g_State.Fence) != VK_SUCCESS)
            return false;

        VkDescriptorPoolSize poolSizes[1] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 } };
        VkDescriptorPoolCreateInfo descriptorPoolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        descriptorPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        descriptorPoolInfo.maxSets = 64;
        descriptorPoolInfo.poolSizeCount = 1;
        descriptorPoolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(g_State.Device, &descriptorPoolInfo, nullptr, &g_State.ImGuiDescriptorPool) !=
            VK_SUCCESS)
            return false;
        return true;
    }

    bool CreateTarget()
    {
        Target& target = g_State.RenderTarget;
        VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent.width = kWidth;
        imageInfo.extent.height = kHeight;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(g_State.Device, &imageInfo, nullptr, &target.Image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(g_State.Device, target.Image, &requirements);
        VkMemoryAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocateInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(g_State.Device, &allocateInfo, nullptr, &target.Memory) != VK_SUCCESS)
            return false;
        if (vkBindImageMemory(g_State.Device, target.Image, target.Memory, 0) != VK_SUCCESS)
            return false;

        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = target.Image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(g_State.Device, &viewInfo, nullptr, &target.View) != VK_SUCCESS)
            return false;

        if (!g_State.UseDynamicRendering)
        {
            VkFramebufferCreateInfo framebufferInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            framebufferInfo.renderPass = g_State.RenderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &target.View;
            framebufferInfo.width = kWidth;
            framebufferInfo.height = kHeight;
            framebufferInfo.layers = 1;
            if (vkCreateFramebuffer(g_State.Device, &framebufferInfo, nullptr, &target.Framebuffer) != VK_SUCCESS)
                return false;
        }

        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = (VkDeviceSize)kWidth * kHeight * 4;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(g_State.Device, &bufferInfo, nullptr, &target.Readback) != VK_SUCCESS)
            return false;
        vkGetBufferMemoryRequirements(g_State.Device, target.Readback, &requirements);
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (allocateInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(g_State.Device, &allocateInfo, nullptr, &target.ReadbackMemory) != VK_SUCCESS)
            return false;
        if (vkBindBufferMemory(g_State.Device, target.Readback, target.ReadbackMemory, 0) != VK_SUCCESS)
            return false;
        if (vkMapMemory(g_State.Device, target.ReadbackMemory, 0, VK_WHOLE_SIZE, 0, &target.ReadbackMapped) !=
            VK_SUCCESS)
            return false;
        return true;
    }

    void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                         VkPipelineStageFlags dstStage)
    {
        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    bool SubmitAndWait()
    {
        if (vkEndCommandBuffer(g_State.CommandBuffer) != VK_SUCCESS)
            return false;
        VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &g_State.CommandBuffer;
        if (vkQueueSubmit(g_State.Queue, 1, &submitInfo, g_State.Fence) != VK_SUCCESS)
            return false;
        if (vkWaitForFences(g_State.Device, 1, &g_State.Fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return false;
        vkResetFences(g_State.Device, 1, &g_State.Fence);
        return true;
    }

    bool WritePpm(const char* path, const unsigned char* pixels)
    {
        FILE* file = std::fopen(path, "wb");
        if (file == nullptr)
            return false;
        std::fprintf(file, "P6\n%u %u\n255\n", kWidth, kHeight);
        for (uint32_t i = 0; i < kWidth * kHeight; ++i)
        {
            const unsigned char rgb[3] = { pixels[i * 4 + 0], pixels[i * 4 + 1], pixels[i * 4 + 2] };
            std::fwrite(rgb, 1, 3, file);
        }
        std::fclose(file);
        return true;
    }

    // ---------------------------------------------------------------------
    // Scene (identical for both font variants)
    // ---------------------------------------------------------------------
    // Returns the font file for a scene (scene 2 needs the fetched CJK subset).
    const char* SceneFontPath(int scene)
    {
        return scene == 2 ? IMSLUG_CJK_TEST_FONT : IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf");
    }

    float SceneFontSize(int scene)
    {
        return scene == 2 ? 20.0f : 17.0f;
    }

    const char* SceneTitle(int scene)
    {
        return scene == 2 ? "cjk stress" : (scene == 1 ? "5k glyph stress" : "imslug ImGui golden scene");
    }

    // 5k visible glyphs at the default UI size: 38 lines x 132 chars at 17 px
    // fill the 1024x640 frame.
    void BuildStressSceneText(char* out, int capacity)
    {
        const char* pattern = "Hamburgefonstiv 0123456789 Grumpy wizards make toxic brew. ";
        int at = 0;
        const int patternLength = (int)strlen(pattern);
        for (int line = 0; line < 38; ++line)
        {
            for (int column = 0; column < 132; ++column)
            {
                out[at++] = pattern[(line * 132 + column) % patternLength];
                if (at >= capacity - 2)
                    break;
            }
            if (line + 1 < 38 && at < capacity - 2)
                out[at++] = '\n';
        }
        out[at] = '\0';
    }

    // Minimal UTF-8 encoder (the harness stays on public ImGui APIs).
    int EncodeUtf8(char* out, unsigned int codepoint)
    {
        if (codepoint < 0x80)
        {
            out[0] = (char)codepoint;
            return 1;
        }
        if (codepoint < 0x800)
        {
            out[0] = (char)(0xC0 | (codepoint >> 6));
            out[1] = (char)(0x80 | (codepoint & 0x3F));
            return 2;
        }
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }

    // 1200 Hanzi (30 lines x 40) at 20 px, all inside the subset range.
    void BuildCjkSceneText(char* out, int capacity)
    {
        int at = 0;
        int index = 0;
        for (int line = 0; line < 30; ++line)
        {
            for (int column = 0; column < 40; ++column)
            {
                unsigned int codepoint = 0x4E00u + (unsigned int)(index++ % 0x100);
                if (at + 4 >= capacity)
                    break;
                at += EncodeUtf8(out + at, codepoint);
            }
            if (line + 1 < 30 && at + 1 < capacity)
                out[at++] = '\n';
        }
        out[at] = '\0';
    }

    // The scene fills the ImGui display. The caller owns DisplaySize and
    // DisplayFramebufferScale (RenderVariant): the render target is always
    // kWidth x kHeight, so a framebuffer scale > 1 shrinks the *logical* display
    // by the same factor - the retina/phone case where ImGui lays out in logical
    // pixels and the renderer expands to the framebuffer.
    void BuildScene(int scene)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::Begin(SceneTitle(scene), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);

        if (scene == 1)
        {
            char* text = (char*)std::malloc(38 * 133 + 2);
            if (text != nullptr)
            {
                BuildStressSceneText(text, 38 * 133 + 2);
                ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), SceneFontSize(1), ImVec2(0.0f, 0.0f),
                                                    IM_COL32(255, 255, 255, 255), text);
                std::free(text);
            }
            ImGui::End();
            ImGui::Render();
            return;
        }
        if (scene == 2)
        {
            char text[30 * 40 * 4 + 64];
            BuildCjkSceneText(text, (int)sizeof(text));
            ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), SceneFontSize(2), ImVec2(0.0f, 0.0f),
                                                IM_COL32(255, 255, 255, 255), text);
            ImGui::End();
            ImGui::Render();
            return;
        }

        // Several sizes through the same (registered) font.
        ImGui::TextUnformatted("Hamburgefonstiv 0123456789 - base size");
        ImFont* font = ImGui::GetFont();
        ImGui::PushFont(font, 24.0f);
        ImGui::TextUnformatted("Larger text at 24 px: The quick brown fox jumps over the lazy dog");
        ImGui::PopFont();
        ImGui::PushFont(font, 12.0f);
        ImGui::TextUnformatted("Small text at 12 px: 0123456789 @#%&gpqy{}()[]|");
        ImGui::PopFont();
        ImGui::PushFont(font, 32.0f);
        ImGui::TextUnformatted("32 px: Handgloves");
        ImGui::PopFont();

        // Tables with narrow cells: the CPU fine clip cuts labels on the right.
        if (ImGui::BeginTable("table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
        {
            for (int row = 0; row < 5; ++row)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("row %d", row);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::TextUnformatted("a long label that gets elided ...");
                ImGui::TableNextColumn();
                ImGui::Text("U+%04X", 0x40 + row);
            }
            ImGui::EndTable();
        }

        // Child windows of shrinking width: window clipping + text at the edge.
        const float widths[3] = { 240.0f, 78.0f, 22.0f };
        for (int i = 0; i < 3; ++i)
        {
            ImGui::BeginChild(i == 0 ? "wide" : "child", ImVec2(widths[i], 40.0f), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Hamburgefonstiv in a child window");
            ImGui::TextUnformatted("second line here");
            ImGui::EndChild();
            ImGui::SameLine();
        }
        ImGui::NewLine();

        // Shapes next to text (atlas path in both variants).
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(p, ImVec2(p.x + 160.0f, p.y + 26.0f), IM_COL32(70, 120, 200, 255), 4.0f);
        drawList->AddCircleFilled(ImVec2(p.x + 200.0f, p.y + 13.0f), 12.0f, IM_COL32(200, 140, 60, 255));
        drawList->AddText(ImVec2(p.x + 6.0f, p.y + 5.0f), IM_COL32(255, 255, 255, 255), "text over a shape");
        ImGui::Dummy(ImVec2(220.0f, 30.0f));

        // Long wrapped text and a horizontally scrolled child (line 2 clipped).
        ImGui::TextWrapped(
            "Wrapped text exercises the vertex reserve/retry path of RenderText and keeps the draw "
            "commands split across many glyphs: lorem ipsum dolor sit amet, consectetur adipiscing elit, "
            "sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.");
        ImGui::BeginChild("hscroll", ImVec2(0.0f, 52.0f), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted("a very long horizontally scrollable line with plenty of words to clip on both sides "
                               "0123456789 abcdefghijklmnopqrstuvwxyz");
        ImGui::SetScrollX(120.0f);
        ImGui::EndChild();

        ImGui::End();
        ImGui::Render();
    }

    // ---------------------------------------------------------------------
    // Rendering one variant
    // ---------------------------------------------------------------------
    // `overlayFlags` is applied with ImSlug_ImplVulkan_SetDebugOverlay
    // after the module is up (Slug variants only); the default sentinel leaves
    // whatever Config::DebugDrawBounds seeded (`seedConfigBounds` passes that
    // config flag to ImSlug::Init). `outOverlayAfterInit` (when non-null)
    // receives GetDebugOverlay() right after Init, so the config-seeded
    // default can be verified.
    const uint32_t kOverlayUnchanged = 0xFFFFFFFFu;
    bool RenderVariant(bool slugVariant, unsigned char* outPixels, const char* name, int scene,
                       uint32_t overlayFlags = kOverlayUnchanged, bool seedConfigBounds = false,
                       uint32_t* outOverlayAfterInit = nullptr, float fbScale = 1.0f)
    {
        ImGui::CreateContext();
        // ImSlug::Init requires a live context (imslug.h); the module lives per
        // variant because each variant owns its contexts/atlas.
        Config imslugConfig;
        imslugConfig.DebugDrawBounds = seedConfigBounds;
        if (!ImSlug::Init(imslugConfig))
        {
            std::printf("  [imslug-golden] ImSlug::Init failed (%s)\n", name);
            return false;
        }
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // The logical display shrinks with the framebuffer scale; the target and
        // therefore the physical size of everything stay kWidth x kHeight.
        io.DisplaySize = ImVec2((float)kWidth / fbScale, (float)kHeight / fbScale);
        io.DisplayFramebufferScale = ImVec2(fbScale, fbScale);
        ImGui::StyleColorsDark();
        ImGui::GetStyle().FontScaleDpi = 1.0f;

        const char* fontPath = SceneFontPath(scene);
        const float baseSize = SceneFontSize(scene);
        if (slugVariant)
        {
            ImFont* font = ImSlug::AddFontFromFileTTF(fontPath, baseSize);
            if (font == nullptr)
                return false;
            io.FontDefault = font;
        }
        else
        {
            ImFontConfig config;
            config.OversampleH = 1;
            config.OversampleV = 1;
            ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath, baseSize, &config);
            if (font == nullptr)
                return false;
            io.FontDefault = font;
        }

        ImGui_ImplVulkan_InitInfo initInfo = {};
        initInfo.ApiVersion = g_State.DynamicRenderingEnabled ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
        initInfo.Instance = g_State.Instance;
        initInfo.PhysicalDevice = g_State.PhysicalDevice;
        initInfo.Device = g_State.Device;
        initInfo.QueueFamily = g_State.QueueFamily;
        initInfo.Queue = g_State.Queue;
        initInfo.DescriptorPool = g_State.ImGuiDescriptorPool;
        initInfo.MinImageCount = 2;
        initInfo.ImageCount = 2;
        initInfo.UseDynamicRendering = g_State.UseDynamicRendering;
        if (g_State.UseDynamicRendering)
        {
            // The formats array must outlive the InitInfo (the backend copies
            // it, but our adapter reads it after this block as well).
            static const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
            initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
                VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
            initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
        }
        else
        {
            initInfo.PipelineInfoMain.RenderPass = g_State.RenderPass;
            initInfo.PipelineInfoMain.Subpass = 0;
        }
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        if (!ImGui_ImplVulkan_Init(&initInfo))
        {
            std::printf("  [imslug-golden] ImGui_ImplVulkan_Init failed (%s)\n", name);
            return false;
        }
        if (!ImSlug_ImplVulkan_Init(&initInfo))
        {
            std::printf("  [imslug-golden] ImSlug_ImplVulkan_Init failed (%s)\n", name);
            return false;
        }
        if (outOverlayAfterInit != nullptr)
            *outOverlayAfterInit = ImSlug_ImplVulkan_GetDebugOverlay();
        if (slugVariant && overlayFlags != kOverlayUnchanged)
        {
            ImSlug_ImplVulkan_SetDebugOverlay(overlayFlags);
            // Round-trip: the toggle must report exactly what was set.
            if (ImSlug_ImplVulkan_GetDebugOverlay() != overlayFlags)
            {
                std::printf("  [imslug-golden] debug overlay round-trip failed (%s)\n", name);
                return false;
            }
        }

        bool ok = true;
        for (uint32_t frame = 0; frame < kFrames && ok; ++frame)
        {
            BuildScene(scene);
            ImDrawData* drawData = ImGui::GetDrawData();

            vkResetCommandBuffer(g_State.CommandBuffer, 0);
            VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(g_State.CommandBuffer, &beginInfo);

            ImSlug_ImplVulkan_UploadPools(g_State.CommandBuffer);

            const bool lastFrame = (frame + 1 == kFrames);
            // The render pass owns the layout (initialLayout UNDEFINED, final
            // TRANSFER_SRC); only the dynamic-rendering path needs barriers. The
            // render pass clears the attachment, so UNDEFINED as the old layout
            // is fine in both paths.
            if (g_State.UseDynamicRendering)
                TransitionImage(g_State.CommandBuffer, g_State.RenderTarget.Image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

            if (g_State.UseDynamicRendering)
            {
                VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
                colorAttachment.imageView = g_State.RenderTarget.View;
                colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colorAttachment.clearValue.color.float32[0] = 0.06f;
                colorAttachment.clearValue.color.float32[1] = 0.06f;
                colorAttachment.clearValue.color.float32[2] = 0.07f;
                colorAttachment.clearValue.color.float32[3] = 1.0f;
                VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
                renderingInfo.renderArea.extent.width = kWidth;
                renderingInfo.renderArea.extent.height = kHeight;
                renderingInfo.layerCount = 1;
                renderingInfo.colorAttachmentCount = 1;
                renderingInfo.pColorAttachments = &colorAttachment;
                vkCmdBeginRendering(g_State.CommandBuffer, &renderingInfo);
                ImSlug_ImplVulkan_RenderDrawData(drawData, g_State.CommandBuffer);
                vkCmdEndRendering(g_State.CommandBuffer);
            }
            else
            {
                VkClearValue clearValue = {};
                clearValue.color.float32[0] = 0.06f;
                clearValue.color.float32[1] = 0.06f;
                clearValue.color.float32[2] = 0.07f;
                clearValue.color.float32[3] = 1.0f;
                VkRenderPassBeginInfo renderPassInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                renderPassInfo.renderPass = g_State.RenderPass;
                renderPassInfo.framebuffer = g_State.RenderTarget.Framebuffer;
                renderPassInfo.renderArea.extent.width = kWidth;
                renderPassInfo.renderArea.extent.height = kHeight;
                renderPassInfo.clearValueCount = 1;
                renderPassInfo.pClearValues = &clearValue;
                vkCmdBeginRenderPass(g_State.CommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
                ImSlug_ImplVulkan_RenderDrawData(drawData, g_State.CommandBuffer);
                vkCmdEndRenderPass(g_State.CommandBuffer);
            }

            if (lastFrame)
            {
                if (g_State.UseDynamicRendering)
                    TransitionImage(g_State.CommandBuffer, g_State.RenderTarget.Image,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy region = {};
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent.width = kWidth;
                region.imageExtent.height = kHeight;
                region.imageExtent.depth = 1;
                vkCmdCopyImageToBuffer(g_State.CommandBuffer, g_State.RenderTarget.Image,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_State.RenderTarget.Readback, 1, &region);
            }
            ok = SubmitAndWait();
        }
        if (ok)
            memcpy(outPixels, g_State.RenderTarget.ReadbackMapped, (size_t)kWidth * kHeight * 4);

        ImSlug_ImplVulkan_Shutdown();
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext();
        ImSlug::Shutdown();  // after the atlas that owns the Slug fonts
        return ok;
    }

    // ---------------------------------------------------------------------
    // Comparison
    // ---------------------------------------------------------------------
    double ChannelDiff(const unsigned char* a, const unsigned char* b, uint32_t index, uint32_t channel)
    {
        return fabs((double)a[index * 4 + channel] - (double)b[index * 4 + channel]);
    }

    // Ink mask: a pixel that differs from the dark background.
    void BuildInkMask(const unsigned char* pixels, unsigned char* mask)
    {
        for (uint32_t i = 0; i < kWidth * kHeight; ++i)
        {
            const int sum = pixels[i * 4 + 0] + pixels[i * 4 + 1] + pixels[i * 4 + 2];
            mask[i] = sum > 3 * 20 ? 1 : 0;
        }
    }

    // Every ink pixel of A must have an ink pixel in B within 1 px (and vice
    // versa): tolerates AA differences, catches shifted/missing/extra glyphs.
    bool MasksAgree(const unsigned char* maskA, const unsigned char* maskB, int* outMissA, int* outMissB)
    {
        *outMissA = 0;
        *outMissB = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const uint32_t i = y * kWidth + x;
                if (maskA[i] == 0 && maskB[i] == 0)
                    continue;
                bool hasA = false, hasB = false;
                for (int dy = -1; dy <= 1 && (!hasA || !hasB); ++dy)
                    for (int dx = -1; dx <= 1 && (!hasA || !hasB); ++dx)
                    {
                        const int nx = (int)x + dx;
                        const int ny = (int)y + dy;
                        if (nx < 0 || ny < 0 || nx >= (int)kWidth || ny >= (int)kHeight)
                            continue;
                        const uint32_t n = (uint32_t)ny * kWidth + (uint32_t)nx;
                        if (maskB[n])
                            hasA = true;
                        if (maskA[n])
                            hasB = true;
                    }
                if (maskA[i] && !hasA)
                    (*outMissA)++;
                if (maskB[i] && !hasB)
                    (*outMissB)++;
            }
        return true;
    }

    // ---------------------------------------------------------------------
    // Debug overlay checks
    // ---------------------------------------------------------------------
    // The outline color of ImSlugVulkanDebugOverlay_GlyphBounds
    // (1.0, 0.45, 0.05) written with alpha 1, so it lands on the target almost
    // exactly (8-bit rounding only).
    int CountGlyphBoundsPixels(const unsigned char* pixels)
    {
        int count = 0;
        for (uint32_t i = 0; i < kWidth * kHeight; ++i)
        {
            const int r = pixels[i * 4 + 0];
            const int g = pixels[i * 4 + 1];
            const int b = pixels[i * 4 + 2];
            if (r > 230 && g > 80 && g < 160 && b < 60)
                ++count;
        }
        return count;
    }

    int CountNonBackgroundPixels(const unsigned char* pixels)
    {
        int count = 0;
        for (uint32_t i = 0; i < kWidth * kHeight; ++i)
        {
            if (pixels[i * 4 + 0] + pixels[i * 4 + 1] + pixels[i * 4 + 2] > 3 * 20)
                ++count;
        }
        return count;
    }

    int CountDifferentPixels(const unsigned char* a, const unsigned char* b)
    {
        int count = 0;
        for (uint32_t i = 0; i < kWidth * kHeight; ++i)
        {
            const int da = std::abs((int)a[i * 4 + 0] - (int)b[i * 4 + 0]) +
                           std::abs((int)a[i * 4 + 1] - (int)b[i * 4 + 1]) +
                           std::abs((int)a[i * 4 + 2] - (int)b[i * 4 + 2]);
            if (da > 8)
                ++count;
        }
        return count;
    }

    // Renders the default acceptance scene with the overlay off, with the
    // config-seeded bounds view, and with the two band views; every check is a
    // regression test of the overlay plumbing (Config -> adapter -> push
    // constants -> shaders).
    void RunDebugOverlayChecks(int* failures)
    {
        unsigned char* plain = (unsigned char*)std::malloc((size_t)kWidth * kHeight * 4);
        unsigned char* bounds = (unsigned char*)std::malloc((size_t)kWidth * kHeight * 4);
        unsigned char* grid = (unsigned char*)std::malloc((size_t)kWidth * kHeight * 4);
        unsigned char* heat = (unsigned char*)std::malloc((size_t)kWidth * kHeight * 4);
        if (plain == nullptr || bounds == nullptr || grid == nullptr || heat == nullptr)
        {
            std::printf("imslug_tests_imgui_golden: FAIL (out of memory, overlay checks)\n");
            ++(*failures);
        }
        else
        {
            std::printf("  debug overlay (scene 0):\n");
            uint32_t overlayAfterInit = 0xFFFFFFFFu;
            bool ok = RenderVariant(true, plain, "overlay-plain", 0, /*overlayFlags=*/kOverlayUnchanged,
                                    /*seedConfigBounds=*/false, &overlayAfterInit);
            TEST_CHECKF(overlayAfterInit == 0, "Config::DebugDrawBounds=false seeded overlay 0x%X", overlayAfterInit);
            if (ok)
                ok = RenderVariant(true, bounds, "overlay-bounds", 0, /*overlayFlags=*/kOverlayUnchanged,
                                   /*seedConfigBounds=*/true, &overlayAfterInit);
            TEST_CHECKF(overlayAfterInit == (uint32_t)ImSlugVulkanDebugOverlay_GlyphBounds,
                        "Config::DebugDrawBounds=true seeded overlay 0x%X (expected GlyphBounds)", overlayAfterInit);
            if (ok)
                ok =
                    RenderVariant(true, grid, "overlay-grid", 0,
                                  (uint32_t)(ImSlugVulkanDebugOverlay_GlyphBounds | ImSlugVulkanDebugOverlay_BandGrid));
            if (ok)
                ok = RenderVariant(true, heat, "overlay-refs", 0,
                                   (uint32_t)(ImSlugVulkanDebugOverlay_GlyphBounds | ImSlugVulkanDebugOverlay_RefHeat));
            if (!ok)
            {
                std::printf("imslug_tests_imgui_golden: FAIL (overlay variant render)\n");
                ++(*failures);
            }
            else
            {
                const int plainBoundsPixels = CountGlyphBoundsPixels(plain);
                const int boundsPixels = CountGlyphBoundsPixels(bounds);
                const int gridBoundsPixels = CountGlyphBoundsPixels(grid);
                const int gridDifferent = CountDifferentPixels(bounds, grid);
                const int heatDifferent = CountDifferentPixels(bounds, heat);
                const int plainInk = CountNonBackgroundPixels(plain);
                const int gridInk = CountNonBackgroundPixels(grid);
                std::printf("  bounds pixels: plain %d, seeded %d, grid %d; grid diffs %d, ref-heat diffs %d\n",
                            plainBoundsPixels, boundsPixels, gridBoundsPixels, gridDifferent, heatDifferent);
                std::printf("  non-background pixels: plain %d, grid %d\n", plainInk, gridInk);
                // The plain frame must not contain the outline color; the
                // seeded and explicit variants must (a dense text frame has
                // tens of thousands of outline pixels).
                TEST_CHECKF(plainBoundsPixels < 50, "overlay off still drew %d bounds pixels", plainBoundsPixels);
                TEST_CHECKF(boundsPixels > 3000, "config-seeded bounds outline only %d pixels", boundsPixels);
                TEST_CHECKF(gridBoundsPixels > 3000, "bounds outline lost with the band grid (%d pixels)",
                            gridBoundsPixels);
                // Both band views must change a large part of the text area and
                // keep the glyph ink (the grid paints the quad; ink never
                // shrinks below the plain frame's ink).
                TEST_CHECKF(gridDifferent > 10000, "band grid changed only %d pixels", gridDifferent);
                TEST_CHECKF(heatDifferent > 10000, "ref heat changed only %d pixels", heatDifferent);
                TEST_CHECKF(gridInk > plainInk, "band grid reduced non-background pixels (%d -> %d)", plainInk,
                            gridInk);

                // No text lost: every ink pixel of the plain frame must exist
                // in the bounds frame (the outline only adds pixels; the other
                // direction is expected to add outline ink outside the glyphs,
                // so it is reported, not asserted).
                unsigned char* maskPlain = (unsigned char*)std::malloc((size_t)kWidth * kHeight);
                unsigned char* maskBounds = (unsigned char*)std::malloc((size_t)kWidth * kHeight);
                if (maskPlain != nullptr && maskBounds != nullptr)
                {
                    BuildInkMask(plain, maskPlain);
                    BuildInkMask(bounds, maskBounds);
                    int missPlain = 0, missBounds = 0;
                    MasksAgree(maskPlain, maskBounds, &missPlain, &missBounds);
                    std::printf("  ink mask: plain->bounds misses %d, bounds->plain %d (outline ink)\n", missPlain,
                                missBounds);
                    TEST_CHECKF(missPlain == 0, "overlay bounds removed %d ink pixels of the plain frame", missPlain);
                }
                std::free(maskPlain);
                std::free(maskBounds);

                WritePpm("overlay_plain.ppm", plain);
                WritePpm("overlay_bounds.ppm", bounds);
                WritePpm("overlay_grid.ppm", grid);
                WritePpm("overlay_refs.ppm", heat);
                std::printf("  frames: overlay_{plain,bounds,grid,refs}.ppm\n");
            }
        }
        std::free(plain);
        std::free(bounds);
        std::free(grid);
        std::free(heat);
    }
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--dynamic-rendering") == 0)
            g_State.UseDynamicRendering = true;
        else if (std::strcmp(argv[i], "--no-validation") == 0)
            g_State.ValidationEnabled = false;
        else
        {
            std::printf("usage: imslug_tests_imgui_golden [--dynamic-rendering] [--no-validation]\n");
            return 2;
        }
    }
    if (!g_State.ValidationEnabled)
        g_State.ValidationEnabled = EnvFlag("IMSLUG_VALIDATION", true);

    std::printf("imslug_tests_imgui_golden: ImGui %s, Slug vs atlas text (%s)\n", IMGUI_VERSION,
                g_State.UseDynamicRendering ? "dynamic rendering" : "render pass");

    if (!CreateInstance())
    {
        std::printf("imslug_tests_imgui_golden: SKIP (no Vulkan instance)\n");
        return kExitSkip;
    }
    if (!CreateDevice())
    {
        std::printf("imslug_tests_imgui_golden: SKIP (no usable Vulkan device)\n");
        return kExitSkip;
    }

    if (!CreateRenderObjects() || !CreateTarget())
    {
        std::printf("imslug_tests_imgui_golden: SKIP (render objects / target creation failed)\n");
        return kExitSkip;
    }

    // Each variant owns its ImGui context; the imslug core (Init/Shutdown) is
    // per variant as well (see RenderVariant).

    unsigned char* slugPixels = (unsigned char*)std::malloc((size_t)kWidth * kHeight * 4);
    unsigned char* stockPixels = (unsigned char*)std::malloc((size_t)kWidth * kHeight * 4);
    if (slugPixels == nullptr || stockPixels == nullptr)
    {
        std::printf("imslug_tests_imgui_golden: FAIL (out of memory)\n");
        return 1;
    }

    int failures = 0;
    // Framebuffer scales: (1,1) is the plain layout case; 2 and 3 emulate the
    // hi-DPI targets (retina/phones) where ImGui lays out in logical pixels and
    // reports DisplayFramebufferScale > 1. Slug must place its instances in
    // display space (the batch MVP maps DisplayPos..DisplayPos+DisplaySize);
    // building the projection from the framebuffer size instead drew the text
    // 1/scale too small, outside its own scissor rect - invisible text. The
    // hi-DPI scales cover the placement path; stress and CJK stay at 1x to
    // bound the runtime.
    const float kFbScales[] = { 1.0f, 2.0f, 3.0f };
    const int kFbScaleCount = (int)(sizeof(kFbScales) / sizeof(kFbScales[0]));
    for (int scene = 0; scene < kSceneCount; ++scene)
    {
        if (scene == 2)
        {
            std::FILE* cjkFile = std::fopen(IMSLUG_CJK_TEST_FONT, "rb");
            if (cjkFile == nullptr)
            {
                std::printf("  scene %d (cjk): SKIP (%s not fetched; scripts/fetch_cjk_font.*)\n", scene,
                            IMSLUG_CJK_TEST_FONT);
                continue;
            }
            std::fclose(cjkFile);
        }
        const int scaleCount = (scene == 0) ? kFbScaleCount : 1;
        for (int scaleIndex = 0; scaleIndex < scaleCount; ++scaleIndex)
        {
            const float fbScale = kFbScales[scaleIndex];
            if (fbScale == 1.0f)
                std::printf("  scene %d (%s):\n", scene, SceneTitle(scene));
            else
                std::printf("  scene %d (%s), framebuffer scale %.0fx (logical display %.0fx%.0f):\n", scene,
                            SceneTitle(scene), fbScale, (float)kWidth / fbScale, (float)kHeight / fbScale);
            if (!RenderVariant(/*slugVariant=*/true, slugPixels, "slug", scene, kOverlayUnchanged,
                               /*seedConfigBounds=*/false, nullptr, fbScale))
            {
                std::printf("imslug_tests_imgui_golden: FAIL (Slug variant, scene %d, fb scale %.0f)\n", scene,
                            fbScale);
                ++failures;
                continue;
            }
            if (!RenderVariant(/*slugVariant=*/false, stockPixels, "atlas", scene, kOverlayUnchanged,
                               /*seedConfigBounds=*/false, nullptr, fbScale))
            {
                std::printf("imslug_tests_imgui_golden: FAIL (atlas variant, scene %d, fb scale %.0f)\n", scene,
                            fbScale);
                ++failures;
                continue;
            }

            {
                // Whole-frame statistics.
                double mean = 0.0;
                int worst = 0;
                int edgePixels = 0;
                int structuralPixels = 0;
                int inkPixels = 0;
                for (uint32_t i = 0; i < kWidth * kHeight; ++i)
                {
                    for (uint32_t channel = 0; channel < 3; ++channel)
                    {
                        const double diff = ChannelDiff(slugPixels, stockPixels, i, channel);
                        mean += diff;
                        if ((int)diff > worst)
                            worst = (int)diff;
                    }
                    const double diffSum = ChannelDiff(slugPixels, stockPixels, i, 0) +
                                           ChannelDiff(slugPixels, stockPixels, i, 1) +
                                           ChannelDiff(slugPixels, stockPixels, i, 2);
                    const double average = diffSum / 3.0;
                    // A pixel counts as ink when either variant inks it.
                    if (slugPixels[i * 4] + slugPixels[i * 4 + 1] + slugPixels[i * 4 + 2] > 60 ||
                        stockPixels[i * 4] + stockPixels[i * 4 + 1] + stockPixels[i * 4 + 2] > 60)
                    {
                        inkPixels++;
                        if (average > kEdgeDiffThreshold)
                            edgePixels++;
                        if (average > kStructuralDiffThreshold)
                            structuralPixels++;
                    }
                }
                mean /= (double)kWidth * kHeight * 3.0;
                const double inkFraction = (double)inkPixels / ((double)kWidth * kHeight);
                const double meanBudget =
                    kMaxMeanDiff > kMaxMeanInkDiff * inkFraction ? kMaxMeanDiff : kMaxMeanInkDiff * inkFraction;
                const double edgeInkFraction = inkPixels > 0 ? (double)edgePixels / (double)inkPixels : 0.0;
                const double structuralInkFraction = inkPixels > 0 ? (double)structuralPixels / (double)inkPixels : 0.0;

                unsigned char* maskA = (unsigned char*)std::malloc((size_t)kWidth * kHeight);
                unsigned char* maskB = (unsigned char*)std::malloc((size_t)kWidth * kHeight);
                int missA = 0, missB = 0;
                if (maskA != nullptr && maskB != nullptr)
                {
                    BuildInkMask(slugPixels, maskA);
                    BuildInkMask(stockPixels, maskB);
                    MasksAgree(maskA, maskB, &missA, &missB);
                }
                std::free(maskA);
                std::free(maskB);

                std::printf("  Slug vs atlas: mean %.3f LSB (budget %.2f), worst %d LSB, ink %d px (%.2f%% of frame)\n",
                            mean, meanBudget, worst, inkPixels, 100.0 * inkFraction);
                std::printf("  ink pixels > %d LSB: %d (%.2f%% of ink), > %d LSB: %d (%.2f%% of ink)\n",
                            kEdgeDiffThreshold, edgePixels, edgeInkFraction * 100.0, kStructuralDiffThreshold,
                            structuralPixels, structuralInkFraction * 100.0);
                std::printf("  layout: ink pixels without a match within 1 px: %d (slug->atlas) / %d (atlas->slug)\n",
                            missA, missB);

                TEST_CHECKF(mean <= meanBudget, "mean |diff| %.3f > %.3f LSB", mean, meanBudget);
                TEST_CHECKF(edgeInkFraction <= kMaxEdgeInkFraction, "%.3f%% of ink pixels differ by more than %d LSB",
                            edgeInkFraction * 100.0, kEdgeDiffThreshold);
                TEST_CHECKF(structuralInkFraction <= kMaxStructuralInkFraction,
                            "%.3f%% of ink pixels differ by more than %d LSB", structuralInkFraction * 100.0,
                            kStructuralDiffThreshold);
                TEST_CHECK(missA == 0 && missB == 0);
                TEST_CHECKF(inkPixels > 5000, "scene produced almost no ink (%d px)", inkPixels);

                const char* dir = ".";
                char path[512];
                const char* scaleSuffix = fbScale == 1.0f ? "" : (fbScale == 2.0f ? "_fb2x" : "_fb3x");
                std::snprintf(path, sizeof(path), "%s/imgui_golden_s%d%s_slug.ppm", dir, scene, scaleSuffix);
                WritePpm(path, slugPixels);
                std::snprintf(path, sizeof(path), "%s/imgui_golden_s%d%s_atlas.ppm", dir, scene, scaleSuffix);
                WritePpm(path, stockPixels);
                std::printf("  frames: imgui_golden_s%d%s_slug.ppm / imgui_golden_s%d%s_atlas.ppm\n", scene,
                            scaleSuffix, scene, scaleSuffix);
            }
        }
    }

    // The debug overlay views (also pins the Config seeding + toggle).
    RunDebugOverlayChecks(&failures);

    if (g_State.ValidationErrors > 0)
    {
        std::printf("imslug_tests_imgui_golden: FAIL (%d validation error(s) out of %d message(s))\n",
                    g_State.ValidationErrors, g_State.ValidationMessages);
        ++failures;
    }
    else
    {
        std::printf("imslug_tests_imgui_golden: validation clean (%d message(s), 0 errors)\n",
                    g_State.ValidationMessages);
    }

    std::free(slugPixels);
    std::free(stockPixels);
    vkDeviceWaitIdle(g_State.Device);
    if (g_State.RenderTarget.ReadbackMapped != nullptr)
        vkUnmapMemory(g_State.Device, g_State.RenderTarget.ReadbackMemory);
    if (g_State.ImGuiDescriptorPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(g_State.Device, g_State.ImGuiDescriptorPool, nullptr);
    vkDestroyFence(g_State.Device, g_State.Fence, nullptr);
    vkDestroyCommandPool(g_State.Device, g_State.CommandPool, nullptr);
    if (g_State.RenderPass != VK_NULL_HANDLE)
        vkDestroyRenderPass(g_State.Device, g_State.RenderPass, nullptr);
    vkDestroyBuffer(g_State.Device, g_State.RenderTarget.Readback, nullptr);
    vkFreeMemory(g_State.Device, g_State.RenderTarget.ReadbackMemory, nullptr);
    vkDestroyImageView(g_State.Device, g_State.RenderTarget.View, nullptr);
    vkDestroyImage(g_State.Device, g_State.RenderTarget.Image, nullptr);
    vkFreeMemory(g_State.Device, g_State.RenderTarget.Memory, nullptr);
    if (g_State.RenderTarget.Framebuffer != VK_NULL_HANDLE)
        vkDestroyFramebuffer(g_State.Device, g_State.RenderTarget.Framebuffer, nullptr);
    vkDestroyDevice(g_State.Device, nullptr);
    if (g_State.DebugMessenger != VK_NULL_HANDLE)
    {
        PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_State.Instance,
                                                                       "vkDestroyDebugUtilsMessengerEXT");
        if (destroyMessenger != nullptr)
            destroyMessenger(g_State.Instance, g_State.DebugMessenger, nullptr);
    }
    vkDestroyInstance(g_State.Instance, nullptr);

    if (failures != 0)
    {
        std::printf("imslug_tests_imgui_golden: FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("imslug_tests_imgui_golden: OK\n");
    return imslug_test::Finish("imslug_tests_imgui_golden");
}
