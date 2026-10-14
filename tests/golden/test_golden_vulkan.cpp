// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// tests/golden/test_golden_vulkan.cpp — headless offscreen Vulkan golden
// tests: fixed strings at sizes 12/16/32/64 and DPI scales 1.0/1.25/2.0
// (plus a fractional 3.5x zoom case) rendered through the real Slug pipeline
// and compared per pixel against the CPU reference rasterizer
// (imslug_refraster.cpp — the arbiter). No surface/swapchain is needed, so the
// test runs on lavapipe in CI.
//
// Mapping contract (documented, both sides use it):
//   * one instance per glyph, placed with an axis-aligned transform
//       screen (pixels, Y down) = (penX + emX * P, penY - emY * P),  P = sizes * scale
//     exactly ImSlugBuildGlyphInstance; the vertex shader keeps that mapping
//     affine (perspective-correct dilation degenerates to a 0.5px per-axis quad
//     expansion in the ortho case), so the fragment's interpolated sample
//     coordinate IS the em coordinate of the pixel center;
//   * the CPU side recomputes the same em coordinate from the instance's
//     ScreenRect/EmBBox and calls ImSlugRasterSample with pixelsPerEm = P;
//   * compositing mirrors the pipeline blend state (color: src alpha /
//     one-minus-src-alpha, alpha: one / one-minus-src-alpha) with the
//     destination kept in 8-bit like the render target;
//   * the same `fill < 0.001` discard threshold is applied on both sides.
//
// Thresholds (measured; 8-bit channel units):
//   max per-channel diff <= kMaxChannelDiff, mean <= kMaxMeanDiff.
// On failure the test writes <case>_gpu.ppm / <case>_cpu.ppm / <case>_diff.ppm
// next to the working directory (the build dir's golden/ folder).
//
// Exit codes: 0 pass, 1 failure, 77 skip (no usable Vulkan device, per
// ctest SKIP_RETURN_CODE).
//
// Validation layers: enabled by default (IMSLUG_VALIDATION=0 disables); any
// validation ERROR fails the run.

#include "../test_util.h"

#include "imslug_impl_vulkan.h"
#include "imslug_internal.h"
#include "imslug_packed.h"

#include <math.h>
#include <vulkan/vulkan.h>

using namespace ImSlug;

#ifndef IMSLUG_PACKED_TEST_ASSET
#define IMSLUG_PACKED_TEST_ASSET "tests/assets/Roboto-ASCII.imslug"
#endif

namespace
{
    const int kExitSkip = 77;
    const float kMargin = 6.0f;

    // Thresholds: the GPU and the CPU reference read the same fp16 texels and
    // do the same fp32 math, so differences come only from rounding. Measured
    // worst case is 1 LSB; keep a small headroom.
    const int kMaxChannelDiff = 3;
    const double kMaxMeanDiff = 0.25;

    struct Case
    {
        char Name[64];
        const char* Text;
        float BakeSize;
        float Scale;
    };

    struct Target
    {
        VkImage Image;
        VkDeviceMemory Memory;
        VkImageView View;
        VkFramebuffer Framebuffer;
        VkBuffer Readback;
        VkDeviceMemory ReadbackMemory;
        void* ReadbackMapped;
        uint32_t Width;
        uint32_t Height;
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

        ImSlugFont* Font;
        ImSlugPools Pools;
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
    // Vulkan setup
    // ---------------------------------------------------------------------
    bool CreateInstance()
    {
        // Dynamic rendering is used as core Vulkan 1.3 (lavapipe and every
        // desktop driver support it); the instance's apiVersion caps what core
        // functionality the app may use, so it has to be raised too.
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        vkEnumerateInstanceVersion(&loaderVersion);
        if (g_State.UseDynamicRendering && loaderVersion < VK_API_VERSION_1_3)
        {
            std::printf("  [golden] loader does not support Vulkan 1.3 - dynamic rendering unavailable\n");
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
        VkApplicationInfo applicationInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        applicationInfo.pApplicationName = "imslug golden tests";
        applicationInfo.apiVersion = g_State.UseDynamicRendering ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;

        VkInstanceCreateInfo createInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        createInfo.pApplicationInfo = &applicationInfo;

        const char* layers[1] = { "VK_LAYER_KHRONOS_validation" };
        if (g_State.ValidationEnabled && hasDebugUtils)
        {
            uint32_t layerCount = 0;
            vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            VkLayerProperties* available =
                (VkLayerProperties*)std::malloc(sizeof(VkLayerProperties) * (layerCount ? layerCount : 1));
            bool found = false;
            if (available != nullptr && layerCount > 0)
            {
                vkEnumerateInstanceLayerProperties(&layerCount, available);
                for (uint32_t i = 0; i < layerCount; ++i)
                    found = found || std::strcmp(available[i].layerName, layers[0]) == 0;
            }
            std::free(available);
            if (found)
            {
                createInfo.enabledLayerCount = 1;
                createInfo.ppEnabledLayerNames = layers;
                createInfo.enabledExtensionCount = 1;
                createInfo.ppEnabledExtensionNames = enabled;
            }
            else
            {
                std::printf("  [golden] VK_LAYER_KHRONOS_validation not available - running without validation\n");
                g_State.ValidationEnabled = false;
            }
        }
        else
        {
            g_State.ValidationEnabled = false;
        }

        if (vkCreateInstance(&createInfo, nullptr, &g_State.Instance) != VK_SUCCESS)
        {
            g_State.Instance = VK_NULL_HANDLE;
            return false;
        }

        if (g_State.ValidationEnabled)
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
            if (createMessenger != nullptr)
                createMessenger(g_State.Instance, &messengerInfo, nullptr, &g_State.DebugMessenger);
        }
        return true;
    }

    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memoryProperties;
        vkGetPhysicalDeviceMemoryProperties(g_State.PhysicalDevice, &memoryProperties);
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        {
            if ((typeBits & (1u << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
            {
                return i;
            }
        }
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

        int forced = -1;
        const char* env = std::getenv("IMSLUG_VK_DEVICE");
        if (env != nullptr && *env != '\0')
            forced = std::atoi(env);

        int best = -1;
        int bestScore = -1;
        for (uint32_t i = 0; i < deviceCount; ++i)
        {
            if (forced >= 0 && (int)i != forced)
                continue;
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(devices[i], &properties);

            VkFormatProperties curveProperties;
            VkFormatProperties bandProperties;
            VkFormatProperties targetProperties;
            vkGetPhysicalDeviceFormatProperties(devices[i], VK_FORMAT_R16G16B16A16_SFLOAT, &curveProperties);
            vkGetPhysicalDeviceFormatProperties(devices[i], VK_FORMAT_R16G16_UINT, &bandProperties);
            vkGetPhysicalDeviceFormatProperties(devices[i], VK_FORMAT_R8G8B8A8_UNORM, &targetProperties);
            const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if ((curveProperties.optimalTilingFeatures & required) != required ||
                (bandProperties.optimalTilingFeatures & required) != required ||
                (targetProperties.optimalTilingFeatures &
                 (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) !=
                    (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
            {
                continue;
            }
            if (g_State.UseDynamicRendering && properties.apiVersion < VK_API_VERSION_1_3)
            {
                // Core 1.3 dynamic rendering only (lavapipe + desktop GPUs
                // support it); the extension path is not used.
                continue;
            }
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : 2;
            if (score > bestScore)
            {
                best = (int)i;
                bestScore = score;
            }
        }
        if (best < 0)
        {
            std::free(devices);
            return false;
        }
        g_State.PhysicalDevice = devices[best];
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(g_State.PhysicalDevice, &properties);
        std::printf("  [golden] device: %s (Vulkan %u.%u.%u, dynamic rendering %s)\n", properties.deviceName,
                    VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
                    VK_VERSION_PATCH(properties.apiVersion), g_State.UseDynamicRendering ? "on" : "off");
        std::free(devices);

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
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
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

        // Core 1.3 dynamic rendering requires the feature bit at device
        // creation (the pipeline is created with VkPipelineRenderingCreateInfo
        // and the draws use vkCmdBeginRendering).
        VkPhysicalDeviceVulkan13Features features13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        if (g_State.UseDynamicRendering)
        {
            VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            query.pNext = &features13;
            vkGetPhysicalDeviceFeatures2(g_State.PhysicalDevice, &query);
            if (features13.dynamicRendering != VK_TRUE)
                return false;  // caller turns this into a skip
        }

        VkDeviceCreateInfo createInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        if (g_State.UseDynamicRendering)
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

        ImSlugVulkanInitInfo initInfo;
        initInfo.PhysicalDevice = g_State.PhysicalDevice;
        initInfo.Device = g_State.Device;
        initInfo.Queue = g_State.Queue;
        initInfo.QueueFamily = g_State.QueueFamily;
        initInfo.ImageCount = 2;
        initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.RenderPass = g_State.RenderPass;
        initInfo.Subpass = 0;
        initInfo.UseDynamicRendering = g_State.UseDynamicRendering;
        VkFormat dynamicColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        if (g_State.UseDynamicRendering)
        {
            VkPipelineRenderingCreateInfo renderingInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachmentFormats = &dynamicColorFormat;
            initInfo.PipelineRenderingCreateInfo = renderingInfo;
        }
        if (!ImSlugVulkanInit(&initInfo))
        {
            std::printf("  [golden] ImSlugVulkanInit failed\n");
            return false;
        }
        return true;
    }

    bool CreateTarget(uint32_t width, uint32_t height, Target* target)
    {
        std::memset(target, 0, sizeof(*target));
        target->Width = width;
        target->Height = height;

        VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(g_State.Device, &imageInfo, nullptr, &target->Image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(g_State.Device, target->Image, &requirements);
        VkMemoryAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocateInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(g_State.Device, &allocateInfo, nullptr, &target->Memory) != VK_SUCCESS)
        {
            return false;
        }
        vkBindImageMemory(g_State.Device, target->Image, target->Memory, 0);

        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = target->Image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(g_State.Device, &viewInfo, nullptr, &target->View) != VK_SUCCESS)
            return false;

        if (!g_State.UseDynamicRendering)
        {
            VkFramebufferCreateInfo framebufferInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            framebufferInfo.renderPass = g_State.RenderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &target->View;
            framebufferInfo.width = width;
            framebufferInfo.height = height;
            framebufferInfo.layers = 1;
            if (vkCreateFramebuffer(g_State.Device, &framebufferInfo, nullptr, &target->Framebuffer) != VK_SUCCESS)
                return false;
        }

        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = (VkDeviceSize)width * height * 4;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(g_State.Device, &bufferInfo, nullptr, &target->Readback) != VK_SUCCESS)
            return false;
        vkGetBufferMemoryRequirements(g_State.Device, target->Readback, &requirements);
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (allocateInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(g_State.Device, &allocateInfo, nullptr, &target->ReadbackMemory) != VK_SUCCESS)
        {
            return false;
        }
        vkBindBufferMemory(g_State.Device, target->Readback, target->ReadbackMemory, 0);
        if (vkMapMemory(g_State.Device, target->ReadbackMemory, 0, VK_WHOLE_SIZE, 0, &target->ReadbackMapped) !=
            VK_SUCCESS)
            return false;
        return true;
    }

    void DestroyTarget(Target* target)
    {
        if (target->ReadbackMapped != nullptr)
            vkUnmapMemory(g_State.Device, target->ReadbackMemory);
        if (target->Readback != VK_NULL_HANDLE)
            vkDestroyBuffer(g_State.Device, target->Readback, nullptr);
        if (target->ReadbackMemory != VK_NULL_HANDLE)
            vkFreeMemory(g_State.Device, target->ReadbackMemory, nullptr);
        if (target->Framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(g_State.Device, target->Framebuffer, nullptr);
        if (target->View != VK_NULL_HANDLE)
            vkDestroyImageView(g_State.Device, target->View, nullptr);
        if (target->Image != VK_NULL_HANDLE)
            vkDestroyImage(g_State.Device, target->Image, nullptr);
        if (target->Memory != VK_NULL_HANDLE)
            vkFreeMemory(g_State.Device, target->Memory, nullptr);
        std::memset(target, 0, sizeof(*target));
    }

    void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                         VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // ---------------------------------------------------------------------
    // One case: render, read back
    // ---------------------------------------------------------------------
    // Records the clear + the one Slug draw of `instanceCount` slot-relative
    // instances (frame `frameIndex`, `firstInstance`) into `target`; the
    // command buffer must be recording and the pools must already be uploaded.
    void RecordDraw(VkCommandBuffer cmd, Target* target, uint32_t frameIndex, uint32_t firstInstance,
                    uint32_t instanceCount)
    {
        const VkClearValue clearValue = { { { 0.0f, 0.0f, 0.0f, 0.0f } } };
        if (g_State.UseDynamicRendering)
        {
            TransitionImage(cmd, target->Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            colorAttachment.imageView = target->View;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue = clearValue;
            VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
            renderingInfo.renderArea.extent.width = target->Width;
            renderingInfo.renderArea.extent.height = target->Height;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            vkCmdBeginRendering(cmd, &renderingInfo);
        }
        else
        {
            VkRenderPassBeginInfo renderPassInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            renderPassInfo.renderPass = g_State.RenderPass;
            renderPassInfo.framebuffer = target->Framebuffer;
            renderPassInfo.renderArea.extent.width = target->Width;
            renderPassInfo.renderArea.extent.height = target->Height;
            renderPassInfo.clearValueCount = 1;
            renderPassInfo.pClearValues = &clearValue;
            vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        }

        VkViewport viewport = {};
        viewport.width = (float)target->Width;
        viewport.height = (float)target->Height;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        float mvp[16];
        ImSlugVulkanBuildOrthoMVP((float)target->Width, (float)target->Height, mvp);

        ImSlugVulkanDrawInfo drawInfo = {};
        drawInfo.FrameIndex = frameIndex;
        drawInfo.RenderPass = g_State.RenderPass;
        drawInfo.Subpass = 0;
        drawInfo.FirstInstance = firstInstance;
        drawInfo.InstanceCount = instanceCount;
        drawInfo.MVP = mvp;
        drawInfo.ViewportWidth = (float)target->Width;
        drawInfo.ViewportHeight = (float)target->Height;
        drawInfo.Scissor.extent.width = target->Width;
        drawInfo.Scissor.extent.height = target->Height;
        ImSlugVulkanDraw(cmd, &drawInfo);

        if (g_State.UseDynamicRendering)
        {
            vkCmdEndRendering(cmd);
            TransitionImage(cmd, target->Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT);
        }
        else
        {
            vkCmdEndRenderPass(cmd);
        }
    }

    void RecordCopyToReadback(VkCommandBuffer cmd, const Target* target)
    {
        VkBufferImageCopy copy = {};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = target->Width;
        copy.imageExtent.height = target->Height;
        copy.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, target->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->Readback, 1, &copy);
    }

    // Submits `cmd` and waits on `fence` (resetting it afterwards).
    bool SubmitAndWait(VkCommandBuffer cmd, VkFence fence)
    {
        VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        if (vkQueueSubmit(g_State.Queue, 1, &submitInfo, fence) != VK_SUCCESS)
            return false;
        if (vkWaitForFences(g_State.Device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return false;
        vkResetFences(g_State.Device, 1, &fence);
        return true;
    }

    bool RenderCase(const Case& testCase, Target* target, uint32_t instanceCount)
    {
        if (!CreateTarget(target->Width, target->Height, target))
            return false;

        VkCommandBuffer cmd = g_State.CommandBuffer;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        if (!ImSlugVulkanUploadPools(cmd, &g_State.Pools, 0))
        {
            std::printf("  [golden] %s: pool upload failed\n", testCase.Name);
            return false;
        }

        RecordDraw(cmd, target, 0, 0, instanceCount);
        RecordCopyToReadback(cmd, target);
        vkEndCommandBuffer(cmd);
        return SubmitAndWait(cmd, g_State.Fence);
    }

    // ---------------------------------------------------------------------
    // CPU reference composite
    // ---------------------------------------------------------------------
    struct CpuGlyph
    {
        const ImSlugGlyphRecord* Record;
        ImSlugGlyphInstance Instance;
    };

    void CompositeCpu(const CpuGlyph* glyphs, int glyphCount, float pixelsPerEm, unsigned char* destination,
                      uint32_t width, uint32_t height)
    {
        for (int i = 0; i < glyphCount; ++i)
        {
            const ImSlugGlyphInstance& g = glyphs[i].Instance;
            const float regionMinX = g.ScreenMinX - 0.5f;
            const float regionMaxX = g.ScreenMaxX + 0.5f;
            const float regionMinY = g.ScreenMinY - 0.5f;
            const float regionMaxY = g.ScreenMaxY + 0.5f;

            int pxStart = (int)std::ceil(regionMinX - 0.5f);
            int pxEnd = (int)std::floor(regionMaxX - 0.5f);
            int pyStart = (int)std::ceil(regionMinY - 0.5f);
            int pyEnd = (int)std::floor(regionMaxY - 0.5f);
            if (pxStart < 0)
                pxStart = 0;
            if (pyStart < 0)
                pyStart = 0;
            if (pxEnd > (int)width - 1)
                pxEnd = (int)width - 1;
            if (pyEnd > (int)height - 1)
                pyEnd = (int)height - 1;

            for (int py = pyStart; py <= pyEnd; ++py)
            {
                const float sy = (float)py + 0.5f;
                for (int px = pxStart; px <= pxEnd; ++px)
                {
                    const float sx = (float)px + 0.5f;
                    // The instance's inverse Jacobian, exactly like the vertex
                    // shader's dilated sample coordinates: em += J * (screen -
                    // anchor). Deriving the em coordinate from the rect extents
                    // instead would differ in the last fp32 bits and can flip
                    // the discontinuity ties (see the thresholds below).
                    const float emX = g.EmMinX + (sx - g.ScreenMinX) * g.Jac00 + (sy - g.ScreenMaxY) * g.Jac10;
                    const float emY = g.EmMinY + (sx - g.ScreenMinX) * g.Jac01 + (sy - g.ScreenMaxY) * g.Jac11;
                    const float coverage =
                        ImSlugRasterSample(&g_State.Pools, glyphs[i].Record, emX, emY, pixelsPerEm, pixelsPerEm);
                    if (coverage < 0.001f)
                        continue;  // matches the shader's discard

                    // Straight-alpha source (color) with alpha = ColorA * coverage,
                    // matching the shader's unpremultiplied output and the
                    // src-alpha / one-minus-src-alpha pipeline blend.
                    const float srcA = g.ColorA * coverage;
                    const float srcR = g.ColorR * srcA;
                    const float srcG = g.ColorG * srcA;
                    const float srcB = g.ColorB * srcA;

                    unsigned char* texel = destination + ((size_t)py * width + (size_t)px) * 4;
                    const float dstR = (float)texel[0] / 255.0f;
                    const float dstG = (float)texel[1] / 255.0f;
                    const float dstB = (float)texel[2] / 255.0f;
                    const float dstA = (float)texel[3] / 255.0f;
                    const float outR = srcR + dstR * (1.0f - srcA);
                    const float outG = srcG + dstG * (1.0f - srcA);
                    const float outB = srcB + dstB * (1.0f - srcA);
                    const float outA = srcA + dstA * (1.0f - srcA);
                    texel[0] = (unsigned char)(outR <= 0.0f ? 0.0f : (outR >= 1.0f ? 255.0f : outR * 255.0f + 0.5f));
                    texel[1] = (unsigned char)(outG <= 0.0f ? 0.0f : (outG >= 1.0f ? 255.0f : outG * 255.0f + 0.5f));
                    texel[2] = (unsigned char)(outB <= 0.0f ? 0.0f : (outB >= 1.0f ? 255.0f : outB * 255.0f + 0.5f));
                    texel[3] = (unsigned char)(outA <= 0.0f ? 0.0f : (outA >= 1.0f ? 255.0f : outA * 255.0f + 0.5f));
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // PPM output
    // ---------------------------------------------------------------------
    bool WritePpm(const char* path, const unsigned char* pixels, uint32_t width, uint32_t height)
    {
        FILE* file = std::fopen(path, "wb");
        if (file == nullptr)
            return false;
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        for (uint32_t y = 0; y < height; ++y)
        {
            const unsigned char* row = pixels + (size_t)y * width * 4;
            for (uint32_t x = 0; x < width; ++x)
            {
                unsigned char rgb[3] = { row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2] };
                std::fwrite(rgb, 1, 3, file);
            }
        }
        std::fclose(file);
        return true;
    }

    // ---------------------------------------------------------------------
    // Result reporting
    // ---------------------------------------------------------------------
    // Thresholds (measured):
    //   * mean channel diff <= kMaxMeanDiff LSB over the whole case;
    //   * every pixel within kMaxChannelDiff LSB, except at most
    //     kOutlierFraction of the pixels per case (min kMinOutlierBudget), which
    //     may differ arbitrarily.
    // The outliers are the reference algorithm's inherent discontinuity ties: a
    // sample landing (within fp32 rounding) exactly on a control-point
    // coordinate, where CalcRootCode's sign-bit eligibility table legitimately
    // resolves to either branch, so two implementations disagree by up to ~0.5
    // coverage at that single pixel (the same property forces the
    // CPU-vs-stbtt gate to use statistical tripwires). Everything else must
    // match within a few LSB.
    struct DiffStats
    {
        int MaxChannel;
        double MeanChannel;
        int MaxDeep;        // worst diff on anti-aliased pixels (CPU alpha in [0.05, 0.95])
        int OverThreshold;  // channels above kMaxChannelDiff
        int OutlierPixels;  // pixels with any channel above kOutlierDiff
        int OutlierWorst;   // worst channel diff among outlier pixels
    };

    const int kOutlierDiff = 8;              // LSB: beyond this a differing pixel counts as an outlier
    const double kOutlierFraction = 0.0005;  // at most 0.05% of the pixels may be outliers
    const int kMinOutlierBudget = 4;

    DiffStats Compare(const unsigned char* gpu, const unsigned char* cpu, uint32_t width, uint32_t height)
    {
        DiffStats stats = {};
        long long total = 0;
        long long samples = 0;
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const unsigned char* a = gpu + ((size_t)y * width + x) * 4;
                const unsigned char* b = cpu + ((size_t)y * width + x) * 4;
                const bool deep = b[3] >= 13 && b[3] <= 242;  // 0.05 .. 0.95 of 255
                int pixelWorst = 0;
                for (int channel = 0; channel < 4; ++channel)
                {
                    const int diff = a[channel] > b[channel] ? a[channel] - b[channel] : b[channel] - a[channel];
                    if (diff > stats.MaxChannel)
                        stats.MaxChannel = diff;
                    if (diff > kMaxChannelDiff)
                        ++stats.OverThreshold;
                    if (deep && channel < 3 && diff > stats.MaxDeep)
                        stats.MaxDeep = diff;
                    if (diff > pixelWorst)
                        pixelWorst = diff;
                    total += diff;
                    ++samples;
                }
                if (pixelWorst > kOutlierDiff)
                {
                    ++stats.OutlierPixels;
                    if (pixelWorst > stats.OutlierWorst)
                        stats.OutlierWorst = pixelWorst;
                }
            }
        }
        stats.MeanChannel = samples > 0 ? (double)total / (double)samples : 0.0;
        return stats;
    }

    // ---------------------------------------------------------------------
    // Backend contract regressions (instance ring + upload paths)
    // ---------------------------------------------------------------------
    // These run against the same headless pipeline as the golden matrix: they
    // need a real device, but they are not golden-image comparisons - they pin
    // the instance-ring and upload contracts with full-frame readbacks.
    struct RegressionGlyphs
    {
        CpuGlyph Glyphs[64];
        int Count;
        float ExtentX, ExtentY;
    };

    // Lays out an ASCII string on the baseline y = 0 and shifts it so its
    // bbox starts at (kMargin, kMargin) with the same sub-pixel jitter the
    // golden cases use. Writes the instances into `outInstances` (the caller
    // picks the ring slot / slot-relative offset) and fills the CPU pairs.
    bool LayoutRegressionText(const char* text, float pixelsPerEm, ImSlugGlyphInstance* outInstances,
                              RegressionGlyphs* out)
    {
        out->Count = 0;
        float penX = 0.0f;
        for (const char* p = text; *p != '\0' && out->Count < 64; ++p)
        {
            const ImSlugGlyphRecord* record = nullptr;
            if (ImSlugGetGlyphRecord(g_State.Font, &g_State.Pools, (unsigned char)*p, &record) != ImSlugBuildStatus_Ok)
            {
                continue;
            }
            if (record->CurveCount == 0)
            {
                penX += record->Advance * pixelsPerEm;
                continue;
            }
            ImSlugBuildGlyphInstance(record, penX, 0.0f, pixelsPerEm, 1.0f, 1.0f, 1.0f, 1.0f,
                                     &outInstances[out->Count]);
            out->Glyphs[out->Count].Record = record;
            penX += record->Advance * pixelsPerEm;
            ++out->Count;
        }
        if (out->Count == 0)
            return false;

        float minX = outInstances[0].ScreenMinX;
        float maxX = outInstances[0].ScreenMaxX;
        float minY = outInstances[0].ScreenMinY;
        float maxY = outInstances[0].ScreenMaxY;
        for (int i = 1; i < out->Count; ++i)
        {
            minX = outInstances[i].ScreenMinX < minX ? outInstances[i].ScreenMinX : minX;
            maxX = outInstances[i].ScreenMaxX > maxX ? outInstances[i].ScreenMaxX : maxX;
            minY = outInstances[i].ScreenMinY < minY ? outInstances[i].ScreenMinY : minY;
            maxY = outInstances[i].ScreenMaxY > maxY ? outInstances[i].ScreenMaxY : maxY;
        }
        const float offsetX = kMargin - minX + 0.0137f;
        const float offsetY = kMargin - minY + 0.0113f;
        for (int i = 0; i < out->Count; ++i)
        {
            outInstances[i].ScreenMinX += offsetX;
            outInstances[i].ScreenMaxX += offsetX;
            outInstances[i].ScreenMinY += offsetY;
            outInstances[i].ScreenMaxY += offsetY;
            out->Glyphs[i].Instance = outInstances[i];
        }
        out->ExtentX = maxX - minX;
        out->ExtentY = maxY - minY;
        return true;
    }

    // Full-frame readback vs the CPU reference for `glyphs` (same thresholds
    // and outlier budget as the golden matrix).
    bool CompareRegressionImage(const char* name, const Target& target, const RegressionGlyphs& glyphs,
                                float pixelsPerEm)
    {
        const uint32_t width = target.Width;
        const uint32_t height = target.Height;
        unsigned char* gpuPixels = (unsigned char*)std::malloc((size_t)width * height * 4);
        unsigned char* cpuPixels = (unsigned char*)std::malloc((size_t)width * height * 4);
        if (gpuPixels == nullptr || cpuPixels == nullptr)
        {
            std::printf("  %s: FAIL (out of memory)\n", name);
            std::free(gpuPixels);
            std::free(cpuPixels);
            return false;
        }
        std::memcpy(gpuPixels, target.ReadbackMapped, (size_t)width * height * 4);
        std::memset(cpuPixels, 0, (size_t)width * height * 4);
        CompositeCpu(glyphs.Glyphs, glyphs.Count, pixelsPerEm, cpuPixels, width, height);

        const DiffStats stats = Compare(gpuPixels, cpuPixels, width, height);
        const int outlierBudget = (int)((double)(width * height) * kOutlierFraction) + kMinOutlierBudget;
        const bool pass = stats.MeanChannel <= kMaxMeanDiff && stats.OutlierPixels <= outlierBudget;
        std::printf("  %-22s %4ux%-4u max=%d mean=%.3f deep=%d over=%d outliers=%d/%d (worst %d)  %s\n", name, width,
                    height, stats.MaxChannel, stats.MeanChannel, stats.MaxDeep, stats.OverThreshold,
                    stats.OutlierPixels, outlierBudget, stats.OutlierWorst, pass ? "ok" : "FAIL");
        std::free(gpuPixels);
        std::free(cpuPixels);
        return pass;
    }

    // The draw must apply the frame's instance-ring base. Distinct content
    // is written into ring slot 1 at a non-zero slot-relative FirstInstance,
    // while slot 0 holds a decoy at the same relative offset; the draw runs
    // with FrameIndex = 1 and must produce slot 1's content. Before the fix
    // the draw read slot 0 (the decoy) and this comparison fails.
    bool RunRingSlotRegression()
    {
        const uint32_t frames = ImSlugVulkanGetFramesInFlight();
        const uint32_t capacity = ImSlugVulkanGetInstanceCapacity();
        if (frames < 2)
        {
            std::printf("  ring-slot (frame 1, first 2): skipped (frames in flight = %u)\n", frames);
            return true;
        }
        ImSlugGlyphInstance* slot0 = ImSlugVulkanGetInstanceData(0);
        ImSlugGlyphInstance* slot1 = ImSlugVulkanGetInstanceData(1);
        const uint32_t firstInstance = 2;
        if (slot0 == nullptr || slot1 == nullptr || capacity < firstInstance + 8)
        {
            std::printf("  ring-slot: FAIL (instance ring unavailable)\n");
            return false;
        }

        const float pixelsPerEm = 32.0f;
        RegressionGlyphs expected = {};
        RegressionGlyphs decoy = {};
        if (!LayoutRegressionText("MMMMMM", pixelsPerEm, slot1 + firstInstance, &expected) ||
            !LayoutRegressionText("WWWWWW", pixelsPerEm, slot0 + firstInstance, &decoy))
        {
            std::printf("  ring-slot: FAIL (glyph layout)\n");
            return false;
        }

        Target target;
        const uint32_t width = (uint32_t)std::ceil(expected.ExtentX + 2.0f * kMargin);
        const uint32_t height = (uint32_t)std::ceil(expected.ExtentY + 2.0f * kMargin);
        if (!CreateTarget(width, height, &target))
        {
            std::printf("  ring-slot: FAIL (target creation)\n");
            return false;
        }

        VkCommandBuffer cmd = g_State.CommandBuffer;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);
        bool ok = ImSlugVulkanUploadPools(cmd, &g_State.Pools, 1);
        if (ok)
            RecordDraw(cmd, &target, 1, firstInstance, (uint32_t)expected.Count);
        if (ok)
            RecordCopyToReadback(cmd, &target);
        vkEndCommandBuffer(cmd);
        ok = ok && SubmitAndWait(cmd, g_State.Fence);
        const bool pass = ok && CompareRegressionImage("ring-slot (frame 1, first 2)", target, expected, pixelsPerEm);
        DestroyTarget(&target);
        return pass;
    }

    // Consecutive frames (up to three, at least two), each recorded (and
    // staged) before any submission, each appending different dirty rows.
    // Every frame's readback must match the CPU reference for its own text.
    // With one shared staging buffer the later recordings overwrite the bytes
    // the earlier frames' copies read, so frame 0/1 would contain another
    // frame's rows.
    bool RunStagingRegression()
    {
        const uint32_t frames = ImSlugVulkanGetFramesInFlight();
        const uint32_t capacity = ImSlugVulkanGetInstanceCapacity();
        if (frames < 2)
        {
            std::printf("  upload-staging frames: skipped (frames in flight = %u)\n", frames);
            return true;
        }
        const int frameCount = frames < 3 ? (int)frames : 3;

        const char* texts[3] = { "ABDK", "CENV", "GHMT" };
        const float pixelsPerEm = 32.0f;

        VkCommandBuffer cmds[3] = {};
        VkFence fences[3] = {};
        Target targets[3] = {};
        RegressionGlyphs glyphs[3] = {};
        VkCommandBufferAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocateInfo.commandPool = g_State.CommandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = (uint32_t)frameCount;
        bool ok = vkAllocateCommandBuffers(g_State.Device, &allocateInfo, cmds) == VK_SUCCESS;
        if (!ok)
            std::printf("  upload-staging frames: FAIL (command buffer allocation)\n");

        for (int k = 0; k < frameCount && ok; ++k)
        {
            VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            ok = vkCreateFence(g_State.Device, &fenceInfo, nullptr, &fences[k]) == VK_SUCCESS;
        }

        // Record every frame first: no submission runs between the records, so
        // the staging writes happen back to back - exactly the window the
        // per-frame staging buffers protect.
        for (int k = 0; k < frameCount && ok; ++k)
        {
            ImSlugGlyphInstance* slot = ImSlugVulkanGetInstanceData((uint32_t)k);
            char name[64];
            std::snprintf(name, sizeof(name), "upload-staging frame %d", k);
            if (slot == nullptr || capacity < (uint32_t)k + 8 ||
                !LayoutRegressionText(texts[k], pixelsPerEm, slot + (uint32_t)k, &glyphs[k]))
            {
                std::printf("  %s: FAIL (glyph layout)\n", name);
                ok = false;
                break;
            }
            targets[k].Width = (uint32_t)std::ceil(glyphs[k].ExtentX + 2.0f * kMargin);
            targets[k].Height = (uint32_t)std::ceil(glyphs[k].ExtentY + 2.0f * kMargin);
            if (!CreateTarget(targets[k].Width, targets[k].Height, &targets[k]))
            {
                std::printf("  %s: FAIL (target creation)\n", name);
                ok = false;
                break;
            }

            vkResetCommandBuffer(cmds[k], 0);
            VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmds[k], &beginInfo);
            if (!ImSlugVulkanUploadPools(cmds[k], &g_State.Pools, (uint32_t)k))
            {
                std::printf("  %s: FAIL (pool upload)\n", name);
                ok = false;
                break;
            }
            RecordDraw(cmds[k], &targets[k], (uint32_t)k, (uint32_t)k, (uint32_t)glyphs[k].Count);
            RecordCopyToReadback(cmds[k], &targets[k]);
            vkEndCommandBuffer(cmds[k]);
        }

        for (int k = 0; k < frameCount && ok; ++k)
        {
            VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &cmds[k];
            ok = vkQueueSubmit(g_State.Queue, 1, &submitInfo, fences[k]) == VK_SUCCESS;
        }
        for (int k = 0; k < frameCount && ok; ++k)
        {
            ok = vkWaitForFences(g_State.Device, 1, &fences[k], VK_TRUE, UINT64_MAX) == VK_SUCCESS &&
                 vkResetFences(g_State.Device, 1, &fences[k]) == VK_SUCCESS;
        }

        bool allPass = ok;
        for (int k = 0; k < frameCount && ok; ++k)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "upload-staging frame %d", k);
            allPass = CompareRegressionImage(name, targets[k], glyphs[k], pixelsPerEm) && allPass;
        }

        for (int k = 0; k < frameCount; ++k)
        {
            if (fences[k] != VK_NULL_HANDLE)
                vkDestroyFence(g_State.Device, fences[k], nullptr);
            DestroyTarget(&targets[k]);
        }
        if (cmds[0] != VK_NULL_HANDLE)
            vkFreeCommandBuffers(g_State.Device, g_State.CommandPool, (uint32_t)frameCount, cmds);
        return allPass;
    }

    // Out-of-range frame indices / first instances are release-safe
    // no-ops (debug asserts cover the programming error). Both draws are
    // recorded after a real upload, so the "no texture yet" early-out cannot
    // mask them; a regression would bind an unwritten descriptor set or fetch
    // out of bounds (validation error / stray ink). Release-only: the debug
    // build intentionally asserts on these calls, so the harness skips the
    // case when NDEBUG is off (see main).
#if defined(NDEBUG)
    bool RunDrawClampRegression()
    {
        const uint32_t frames = ImSlugVulkanGetFramesInFlight();
        const uint32_t capacity = ImSlugVulkanGetInstanceCapacity();
        ImSlugGlyphInstance* slot0 = ImSlugVulkanGetInstanceData(0);
        RegressionGlyphs glyphs = {};
        if (slot0 == nullptr || !LayoutRegressionText("A", 32.0f, slot0, &glyphs))
        {
            std::printf("  draw-clamp: FAIL (glyph layout)\n");
            return false;
        }

        Target target;
        if (!CreateTarget(32, 32, &target))
        {
            std::printf("  draw-clamp: FAIL (target creation)\n");
            return false;
        }

        VkCommandBuffer cmd = g_State.CommandBuffer;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);
        bool ok = ImSlugVulkanUploadPools(cmd, &g_State.Pools, 0);
        if (ok)
            RecordDraw(cmd, &target, frames, 0, 1);  // FrameIndex out of range
        if (ok)
            RecordDraw(cmd, &target, 0, capacity, 1);  // FirstInstance out of range
        if (ok)
            RecordCopyToReadback(cmd, &target);
        vkEndCommandBuffer(cmd);
        ok = ok && SubmitAndWait(cmd, g_State.Fence);

        bool clear = ok;
        const unsigned char* pixels = (const unsigned char*)target.ReadbackMapped;
        for (uint32_t i = 0; clear && i < target.Width * target.Height * 4; ++i)
            clear = pixels[i] == 0;
        std::printf("  %-22s %4ux%-4u %s\n", "draw-clamp no-ops", target.Width, target.Height, clear ? "ok" : "FAIL");
        DestroyTarget(&target);
        return clear;
    }
#endif  // defined(NDEBUG)
}  // namespace

int main(int argc, char** argv)
{
    g_State.UseDynamicRendering = false;
    g_State.ValidationEnabled = EnvFlag("IMSLUG_VALIDATION", true);
    const char* packedAssetPath = nullptr;
#if defined(IMSLUG_NO_STBTT)
    // No TTF parser in this build: the packed asset is the only font source.
    packedAssetPath = IMSLUG_PACKED_TEST_ASSET;
#endif
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--dynamic") == 0)  // or --dynamic-rendering
            g_State.UseDynamicRendering = true;
        else if (std::strcmp(argv[i], "--dynamic-rendering") == 0)
            g_State.UseDynamicRendering = true;
        else if (std::strcmp(argv[i], "--no-validation") == 0)
            g_State.ValidationEnabled = false;
        else if (std::strcmp(argv[i], "--packed") == 0)
            packedAssetPath = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : IMSLUG_PACKED_TEST_ASSET;
        else
        {
            std::printf(
                "usage: imslug_tests_golden [--dynamic-rendering] [--no-validation] [--packed [asset.imslug]]\n");
            return 1;
        }
    }

    if (!CreateInstance())
    {
        std::printf("imslug_golden: SKIP (no Vulkan instance)\n");
        return kExitSkip;
    }
    if (!CreateDevice())
    {
        std::printf("imslug_golden: SKIP (no usable Vulkan device)\n");
        return kExitSkip;
    }

    int fontSize = 0;
    unsigned char* fontData = nullptr;
    if (packedAssetPath != nullptr)
    {
        // The same golden matrix from a pre-baked `.imslug` asset (no
        // outline parsing); in a no-parser build this is the only mode.
        fontData = imslug_test::LoadFile(packedAssetPath, &fontSize);
        if (fontData == nullptr)
        {
            std::printf("imslug_golden: FAIL (packed asset not found: %s)\n", packedAssetPath);
            return 1;
        }
        g_State.Font = ImSlugCreateFontFromPackedMemory(fontData, fontSize);
    }
#if !defined(IMSLUG_NO_STBTT)
    else
    {
        fontData = imslug_test::LoadFontFile("Roboto-Regular.ttf", &fontSize);
        if (fontData == nullptr)
        {
            std::printf("imslug_golden: FAIL (test font not found)\n");
            return 1;
        }
        g_State.Font = ImSlugCreateFontFromMemory(fontData, fontSize, 0, true);
    }
#endif
    std::free(fontData);
    if (g_State.Font == nullptr)
    {
        std::printf("imslug_golden: FAIL (font load)\n");
        return 1;
    }
    std::printf("imslug_golden: font source %s\n", packedAssetPath != nullptr ? "packed .imslug asset" : "TTF");
    ImSlugInitPools(&g_State.Pools);

    if (!CreateRenderObjects())
    {
        std::printf("imslug_golden: SKIP (render objects / backend init failed)\n");
        return kExitSkip;
    }

    // Backend contract regressions first (they warm the pools; the golden
    // matrix below builds its own glyphs on top). Failures count like case
    // failures and also surface through the validation-error gate.
    int backendFailures = 0;
    if (!RunRingSlotRegression())
        ++backendFailures;
    if (!RunStagingRegression())
        ++backendFailures;
#if defined(NDEBUG)
    if (!RunDrawClampRegression())
        ++backendFailures;
#else
    std::printf("  draw-clamp no-ops: skipped (debug asserts active)\n");
#endif

    static const char* kTexts[2] = { "Hamburgefonstiv 0123456789", "@#%&gpqy{}()[]|" };
    static const float kBakeSizes[4] = { 12.0f, 16.0f, 32.0f, 64.0f };
    static const float kScales[3] = { 1.0f, 1.25f, 2.0f };
    Case cases[64];
    int caseCount = 0;
    for (int textIndex = 0; textIndex < 2; ++textIndex)
    {
        for (int sizeIndex = 0; sizeIndex < 4; ++sizeIndex)
        {
            for (int scaleIndex = 0; scaleIndex < 3; ++scaleIndex)
            {
                Case& testCase = cases[caseCount++];
                std::snprintf(testCase.Name, sizeof(testCase.Name), "t%d_%dpx_%dmilli", textIndex,
                              (int)kBakeSizes[sizeIndex], (int)(kScales[scaleIndex] * 1000.0f));
                testCase.Text = kTexts[textIndex];
                testCase.BakeSize = kBakeSizes[sizeIndex];
                testCase.Scale = kScales[scaleIndex];
            }
        }
    }
    // Fractional zoom-in evidence (acceptance criterion: sharp at any zoom).
    cases[caseCount].Name[0] = '\0';
    std::snprintf(cases[caseCount].Name, sizeof(cases[caseCount].Name), "zoom_32px_3500milli");
    cases[caseCount].Text = kTexts[0];
    cases[caseCount].BakeSize = 32.0f;
    cases[caseCount].Scale = 3.5f;
    ++caseCount;

    int failures = backendFailures;
    int worstMax = 0;
    double worstMean = 0.0;
    int worstDeep = 0;
    char worstName[64] = "";
    for (int caseIndex = 0; caseIndex < caseCount; ++caseIndex)
    {
        const Case& testCase = cases[caseIndex];
        const float pixelsPerEm = testCase.BakeSize * testCase.Scale;

        // Two passes: build at the origin to learn the extent, then place the
        // text inside a framebuffer that fits it plus the dilation margin.
        const uint32_t capacity = ImSlugVulkanGetInstanceCapacity();
        ImSlugGlyphInstance* instances = ImSlugVulkanGetInstanceData(0);
        CpuGlyph cpuGlyphs[512];
        int glyphCount = 0;
        float penX = 0.0f;
        for (const char* p = testCase.Text; *p != '\0' && glyphCount < 512; ++p)
        {
            const ImSlugGlyphRecord* record = nullptr;
            if (ImSlugGetGlyphRecord(g_State.Font, &g_State.Pools, (unsigned char)*p, &record) != ImSlugBuildStatus_Ok)
                continue;
            if (record->CurveCount == 0)
            {
                penX += record->Advance * pixelsPerEm;
                continue;
            }
            ImSlugBuildGlyphInstance(record, penX, 0.0f, pixelsPerEm, 1.0f, 1.0f, 1.0f, 1.0f, &instances[glyphCount]);
            cpuGlyphs[glyphCount].Record = record;
            cpuGlyphs[glyphCount].Instance = instances[glyphCount];
            penX += record->Advance * pixelsPerEm;
            ++glyphCount;
        }
        if (glyphCount == 0 || (uint32_t)glyphCount > capacity)
        {
            std::printf("imslug_golden: FAIL (%s has no glyphs / too many)\n", testCase.Name);
            return 1;
        }

        float minX = instances[0].ScreenMinX;
        float maxX = instances[0].ScreenMaxX;
        float minY = instances[0].ScreenMinY;
        float maxY = instances[0].ScreenMaxY;
        for (int i = 1; i < glyphCount; ++i)
        {
            if (instances[i].ScreenMinX < minX)
                minX = instances[i].ScreenMinX;
            if (instances[i].ScreenMaxX > maxX)
                maxX = instances[i].ScreenMaxX;
            if (instances[i].ScreenMinY < minY)
                minY = instances[i].ScreenMinY;
            if (instances[i].ScreenMaxY > maxY)
                maxY = instances[i].ScreenMaxY;
        }
        // Sub-pixel jitter: keep pixel centers off exact control-point
        // coordinates. At an exact tie the reference algorithm's sign-bit
        // root eligibility (CalcRootCode) is discontinuous - the same sample
        // can legitimately take either branch - so ties would compare two
        // different limits of the same formula. A generic offset exercises
        // the well-defined, non-tie behavior instead.
        const float offsetX = kMargin - minX + 0.0137f;
        const float offsetY = kMargin - minY + 0.0113f;
        for (int i = 0; i < glyphCount; ++i)
        {
            instances[i].ScreenMinX += offsetX;
            instances[i].ScreenMaxX += offsetX;
            instances[i].ScreenMinY += offsetY;
            instances[i].ScreenMaxY += offsetY;
        }
        for (int i = 0; i < glyphCount; ++i)
            cpuGlyphs[i].Instance = instances[i];

        const uint32_t width = (uint32_t)std::ceil(maxX - minX + 2.0f * kMargin);
        const uint32_t height = (uint32_t)std::ceil(maxY - minY + 2.0f * kMargin);

        Target target;
        std::memset(&target, 0, sizeof(target));
        target.Width = width;
        target.Height = height;
        if (!RenderCase(testCase, &target, (uint32_t)glyphCount))
        {
            std::printf("imslug_golden: FAIL (%s render)\n", testCase.Name);
            ++failures;
            continue;
        }

        unsigned char* gpuPixels = (unsigned char*)std::malloc((size_t)width * height * 4);
        unsigned char* cpuPixels = (unsigned char*)std::malloc((size_t)width * height * 4);
        if (gpuPixels == nullptr || cpuPixels == nullptr)
        {
            std::printf("imslug_golden: FAIL (out of memory)\n");
            return 1;
        }
        std::memcpy(gpuPixels, target.ReadbackMapped, (size_t)width * height * 4);
        std::memset(cpuPixels, 0, (size_t)width * height * 4);
        CompositeCpu(cpuGlyphs, glyphCount, pixelsPerEm, cpuPixels, width, height);

        const DiffStats stats = Compare(gpuPixels, cpuPixels, width, height);
        const int outlierBudget = (int)((double)(width * height) * kOutlierFraction) + kMinOutlierBudget;
        const bool pass = stats.MeanChannel <= kMaxMeanDiff && stats.OutlierPixels <= outlierBudget;
        std::printf("  %-22s %4ux%-4u max=%d mean=%.3f deep=%d over=%d outliers=%d/%d (worst %d)  %s\n", testCase.Name,
                    width, height, stats.MaxChannel, stats.MeanChannel, stats.MaxDeep, stats.OverThreshold,
                    stats.OutlierPixels, outlierBudget, stats.OutlierWorst, pass ? "ok" : "FAIL");
        if (!pass)
        {
            // Compact evidence: list the offending pixels (coordinates only).
            std::printf("  [golden] %s exceeding pixels:", testCase.Name);
            int printed = 0;
            for (uint32_t y = 0; y < height && printed < 16; ++y)
            {
                for (uint32_t x = 0; x < width && printed < 16; ++x)
                {
                    const unsigned char* a = gpuPixels + ((size_t)y * width + x) * 4;
                    const unsigned char* b = cpuPixels + ((size_t)y * width + x) * 4;
                    int d = 0;
                    for (int c = 0; c < 4; ++c)
                    {
                        const int channelDiff = std::abs((int)a[c] - (int)b[c]);
                        if (channelDiff > d)
                            d = channelDiff;
                    }
                    if (d > kOutlierDiff)
                    {
                        std::printf(" (%u,%u,gpu=%d,cpu=%d)", x, y, a[1], b[1]);
                        ++printed;
                    }
                }
            }
            std::printf("\n");
        }

        char path[256];
        std::snprintf(path, sizeof(path), "%s_gpu.ppm", testCase.Name);
        WritePpm(path, gpuPixels, width, height);
        std::snprintf(path, sizeof(path), "%s_cpu.ppm", testCase.Name);
        WritePpm(path, cpuPixels, width, height);
        if (!pass)
        {
            ++failures;
            unsigned char* diff = (unsigned char*)std::malloc((size_t)width * height * 4);
            for (size_t i = 0; i < (size_t)width * height * 4; ++i)
            {
                const int d = gpuPixels[i] > cpuPixels[i] ? gpuPixels[i] - cpuPixels[i] : cpuPixels[i] - gpuPixels[i];
                diff[i] = (unsigned char)(d > 255 ? 255 : d * 8);
            }
            std::snprintf(path, sizeof(path), "%s_diff.ppm", testCase.Name);
            WritePpm(path, diff, width, height);
            std::free(diff);
        }
        if (stats.MaxChannel > worstMax)
        {
            worstMax = stats.MaxChannel;
            std::snprintf(worstName, sizeof(worstName), "%s", testCase.Name);
        }
        if (stats.MeanChannel > worstMean)
            worstMean = stats.MeanChannel;
        if (stats.MaxDeep > worstDeep)
            worstDeep = stats.MaxDeep;

        std::free(gpuPixels);
        std::free(cpuPixels);
        DestroyTarget(&target);
    }

    std::printf("imslug_golden: %d case(s), worst max=%d (%s), worst mean=%.3f, worst deep=%d\n", caseCount, worstMax,
                worstName, worstMean, worstDeep);
    if (g_State.ValidationErrors > 0)
    {
        std::printf("imslug_golden: FAIL (%d validation error(s) out of %d message(s))\n", g_State.ValidationErrors,
                    g_State.ValidationMessages);
        ++failures;
    }
    else
    {
        std::printf("imslug_golden: validation clean (%d message(s), 0 errors)\n", g_State.ValidationMessages);
    }

    ImSlugVulkanShutdown();
    ImSlugDestroyFont(g_State.Font);
    ImSlugDestroyPools(&g_State.Pools);
    if (g_State.CommandPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(g_State.Device, g_State.CommandPool, nullptr);
    if (g_State.Fence != VK_NULL_HANDLE)
        vkDestroyFence(g_State.Device, g_State.Fence, nullptr);
    if (g_State.RenderPass != VK_NULL_HANDLE)
        vkDestroyRenderPass(g_State.Device, g_State.RenderPass, nullptr);
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

    if (failures == 0)
    {
        std::printf("imslug_golden: OK\n");
        return 0;
    }
    std::printf("imslug_golden: FAILED (%d)\n", failures);
    return 1;
}
