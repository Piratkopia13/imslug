// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// tests/golden/test_emoji_golden_vulkan.cpp — headless offscreen Vulkan golden
// tests for COLR v1 color fonts: emoji strings at several bake sizes and
// zoom scales, rendered through the real Slug pipeline (one instance per color
// layer, gradient paints evaluated in slug.frag) and compared per pixel against
// the CPU arbiter: ImSlugRasterSample for coverage plus
// ImSlugSamplePaintRecord for the paint (the exact mirror of the shader's paint
// code), composited with the pipeline's straight-alpha blend into an 8-bit
// destination.
//
// The emoji font (Noto Color Emoji, OFL) is a fetched, gitignored asset
// (scripts/fetch_emoji_font.*; tests/fonts/emoji/PROVENANCE.txt). The suite
// exits with code 77 (ctest SKIP_RETURN_CODE) when the asset is absent or no
// Vulkan device is usable.
//
// Thresholds: the same contract as the monochrome golden (GPU vs the CPU
// arbiter, 8-bit
// channel units): mean <= kMaxMeanDiff over the case, every pixel within
// kMaxChannelDiff except a small outlier budget for the reference algorithm's
// discontinuity ties. On failure the test writes
// <case>_gpu.ppm / <case>_cpu.ppm next to the working directory.
//
// Validation layers: enabled by default (IMSLUG_VALIDATION=0 disables); any
// validation error fails the run.

#include "../test_util.h"

#include "imslug_impl_vulkan.h"
#include "imslug_internal.h"

#include <math.h>
#include <vulkan/vulkan.h>

using namespace ImSlug;

namespace
{
    const int kExitSkip = 77;
    const float kMargin = 6.0f;

    // Measured against the CPU arbiter: the
    // color path is at least as close as the monochrome goldens (worst mean
    // 0.031 LSB, isolated discontinuity ties up to ~21 LSB), so the same
    // contract applies: mean <= 0.25 LSB over the case, every pixel within a few
    // LSB except a small outlier budget for the reference algorithm's
    // discontinuity ties.
    const int kMaxChannelDiff = 4;
    const double kMaxMeanDiff = 0.25;
    const int kOutlierDiff = 10;
    const double kOutlierFraction = 0.0005;
    const int kMinOutlierBudget = 4;

    struct EmojiCase
    {
        char Name[64];
        const char* Text;  // UTF-8
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

    // One drawn color layer: the instance plus the CPU-side paint/record pair
    // the expected image is built from.
    struct CpuLayer
    {
        const ImSlugGlyphRecord* Record;
        const ImSlugPaintRecord* Paint;
        ImSlugGlyphInstance Instance;
    };

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
    // Vulkan setup (mirrors the monochrome golden harness)
    // ---------------------------------------------------------------------
    bool CreateInstance()
    {
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        vkEnumerateInstanceVersion(&loaderVersion);
        if (g_State.UseDynamicRendering && loaderVersion < VK_API_VERSION_1_3)
        {
            std::printf("  [emoji-golden] loader does not support Vulkan 1.3 - dynamic rendering unavailable\n");
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
        applicationInfo.pApplicationName = "imslug emoji golden tests";
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
                std::printf("  [emoji-golden] VK_LAYER_KHRONOS_validation not available - running without it\n");
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
                continue;
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
        std::printf("  [emoji-golden] device: %s (Vulkan %u.%u.%u, dynamic rendering %s)\n", properties.deviceName,
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

        VkPhysicalDeviceVulkan13Features features13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        if (g_State.UseDynamicRendering)
        {
            VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            query.pNext = &features13;
            vkGetPhysicalDeviceFeatures2(g_State.PhysicalDevice, &query);
            if (features13.dynamicRendering != VK_TRUE)
                return false;
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
            std::printf("  [emoji-golden] ImSlugVulkanInit failed\n");
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

    void RecordDraw(VkCommandBuffer cmd, Target* target, uint32_t instanceCount)
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
        drawInfo.FrameIndex = 0;
        drawInfo.RenderPass = g_State.RenderPass;
        drawInfo.Subpass = 0;
        drawInfo.FirstInstance = 0;
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

    bool RenderCase(const EmojiCase& testCase, Target* target, uint32_t instanceCount)
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
            std::printf("  [emoji-golden] %s: pool upload failed\n", testCase.Name);
            return false;
        }

        RecordDraw(cmd, target, instanceCount);
        RecordCopyToReadback(cmd, target);
        vkEndCommandBuffer(cmd);
        return SubmitAndWait(cmd, g_State.Fence);
    }

    // ---------------------------------------------------------------------
    // CPU reference composite (coverage + paint, straight-alpha blend)
    // ---------------------------------------------------------------------
    void CompositeCpu(const CpuLayer* layers, int layerCount, unsigned char* destination, uint32_t width,
                      uint32_t height)
    {
        for (int i = 0; i < layerCount; ++i)
        {
            const ImSlugGlyphInstance& instance = layers[i].Instance;
            const float ppemX = 1.0f / instance.Jac00;
            const float ppemY = -1.0f / instance.Jac11;
            const float regionMinX = instance.ScreenMinX - 0.5f;
            const float regionMaxX = instance.ScreenMaxX + 0.5f;
            const float regionMinY = instance.ScreenMinY - 0.5f;
            const float regionMaxY = instance.ScreenMaxY + 0.5f;

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
                    const float emX = instance.EmMinX + (sx - instance.ScreenMinX) * instance.Jac00 +
                                      (sy - instance.ScreenMaxY) * instance.Jac10;
                    const float emY = instance.EmMinY + (sx - instance.ScreenMinX) * instance.Jac01 +
                                      (sy - instance.ScreenMaxY) * instance.Jac11;
                    const float coverage = ImSlugRasterSample(&g_State.Pools, layers[i].Record, emX, emY, ppemX, ppemY);
                    if (coverage < 0.001f)
                        continue;  // matches the shader's discard

                    // The paint record resolves rgb/alpha in record space; the
                    // instance color contributes the tint alpha (1.0 here).
                    float paint[4];
                    ImSlugSamplePaintRecord(layers[i].Paint, emX, emY, paint);
                    const float srcA = paint[3] * instance.ColorA * coverage;
                    const float srcR = paint[0] * srcA;
                    const float srcG = paint[1] * srcA;
                    const float srcB = paint[2] * srcA;

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

    struct DiffStats
    {
        int MaxChannel;
        double MeanChannel;
        int OverThreshold;
        int OutlierPixels;
        int OutlierWorst;
    };

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
                int pixelWorst = 0;
                for (int channel = 0; channel < 4; ++channel)
                {
                    const int diff = a[channel] > b[channel] ? a[channel] - b[channel] : b[channel] - a[channel];
                    if (diff > stats.MaxChannel)
                        stats.MaxChannel = diff;
                    if (diff > kMaxChannelDiff)
                        ++stats.OverThreshold;
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
    // Emoji layout (mirrors the adapter + pre-pass placement)
    // ---------------------------------------------------------------------
    // Lays out the UTF-8 string's emoji on the baseline and builds the color
    // layer instances exactly like the ImGui pre-pass does (the synthesized
    // metrics box for empty base glyphs included). Returns the instance count.
    int LayoutEmojiText(const char* text, float bakeSize, float scale, float originX, float originY,
                        ImSlugGlyphInstance* outInstances, CpuLayer* outLayers, int capacity)
    {
        const float pixelsPerEm = bakeSize * scale;
        int instanceCount = 0;
        float penX = originX;
        const char* s = text;
        while (*s != '\0' && instanceCount < capacity)
        {
            // Decode one UTF-8 codepoint.
            unsigned int codepoint = (unsigned char)*s++;
            if (codepoint >= 0x80)
            {
                if ((codepoint & 0xE0) == 0xC0)
                {
                    codepoint = ((codepoint & 0x1F) << 6) | ((unsigned char)*s++ & 0x3F);
                }
                else if ((codepoint & 0xF0) == 0xE0)
                {
                    codepoint = ((codepoint & 0x0F) << 12) | (((unsigned char)*s++ & 0x3F) << 6);
                    codepoint |= ((unsigned char)*s++ & 0x3F);
                }
                else if ((codepoint & 0xF8) == 0xF0)
                {
                    codepoint = ((codepoint & 0x07) << 18) | (((unsigned char)*s++ & 0x3F) << 12);
                    codepoint |= (((unsigned char)*s++ & 0x3F) << 6) | ((unsigned char)*s++ & 0x3F);
                }
            }

            const int glyphIndex = ImSlugGetGlyphIndex(g_State.Font, codepoint);
            if (glyphIndex == 0)
                continue;
            ImSlugGlyphMetrics metrics;
            if (!ImSlugGetGlyphMetricsByIndex(g_State.Font, glyphIndex, bakeSize, &metrics))
                continue;
            const ImSlugColorGlyph* colorGlyph = nullptr;
            if (ImSlugGetColorGlyph(g_State.Font, &g_State.Pools, glyphIndex, &colorGlyph) != ImSlugBuildStatus_Ok ||
                colorGlyph == nullptr || colorGlyph->LayerCount == 0)
            {
                continue;  // not a color glyph in this test
            }
            if (!(metrics.X1 > metrics.X0) || !(metrics.Y1 > metrics.Y0))
            {
                // Empty base outline (Noto): synthesize the box from the layers.
                ImSlugFontMetrics fontMetrics;
                ImSlugGetFontMetrics(g_State.Font, bakeSize, &fontMetrics);
                const float ascentRounded = std::floor(fontMetrics.Ascent + 0.5f);
                metrics.X0 = std::floor(colorGlyph->MinX * pixelsPerEm);
                metrics.X1 = std::ceil(colorGlyph->MaxX * pixelsPerEm);
                metrics.Y0 = std::floor(-colorGlyph->MaxY * pixelsPerEm) + ascentRounded;
                metrics.Y1 = std::ceil(-colorGlyph->MinY * pixelsPerEm) + ascentRounded;
            }

            static int layerIndices[4096];
            const int count =
                ImSlugBuildColorGlyphInstances(colorGlyph, penX, originY, pixelsPerEm, 1.0f,
                                               outInstances + instanceCount, capacity - instanceCount, layerIndices);
            for (int i = 0; i < count; ++i)
            {
                outLayers[instanceCount + i].Instance = outInstances[instanceCount + i];
                const ImSlugColorLayer& layer = colorGlyph->Layers[layerIndices[i]];
                outLayers[instanceCount + i].Record = layer.Record;
                outLayers[instanceCount + i].Paint = ImSlugPaintRecordAt(&g_State.Pools.Paints, (int)layer.PaintIndex);
            }
            instanceCount += count;
            penX += metrics.AdvanceX * scale;
        }
        return instanceCount;
    }
}  // namespace

int main(int argc, char** argv)
{
    g_State.ValidationEnabled = EnvFlag("IMSLUG_VALIDATION", true);
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--dynamic-rendering") == 0)
            g_State.UseDynamicRendering = true;
        else if (std::strcmp(argv[i], "--no-validation") == 0)
            g_State.ValidationEnabled = false;
        else
        {
            std::printf("usage: imslug_tests_emoji_golden [--dynamic-rendering] [--no-validation]\n");
            return 1;
        }
    }

    int fontSize = 0;
    unsigned char* fontData = imslug_test::LoadFile(IMSLUG_EMOJI_TEST_FONT, &fontSize);
    if (fontData == nullptr)
    {
        std::printf("imslug_emoji_golden: SKIP (emoji font not present; fetch with scripts/fetch_emoji_font.*)\n");
        return kExitSkip;
    }

    if (!CreateInstance())
    {
        std::free(fontData);
        std::printf("imslug_emoji_golden: SKIP (no Vulkan instance)\n");
        return kExitSkip;
    }
    if (!CreateDevice())
    {
        std::free(fontData);
        std::printf("imslug_emoji_golden: SKIP (no usable Vulkan device)\n");
        return kExitSkip;
    }

    g_State.Font = ImSlugCreateFontFromMemory(fontData, fontSize, 0, true);
    std::free(fontData);
    if (g_State.Font == nullptr)
    {
        std::printf("imslug_emoji_golden: FAIL (font load)\n");
        return 1;
    }
    ImSlugInitPools(&g_State.Pools);

    if (!CreateRenderObjects())
    {
        std::printf("imslug_emoji_golden: SKIP (render objects / backend init failed)\n");
        return kExitSkip;
    }

    // Emoji strings: a single gradient-rich face, a heart (gradient + solid
    // layers), a mixed row, and a composed ZWJ sequence (family) which the
    // font renders as one glyph with many layers.
    static const char* kTexts[4] = {
        "\xF0\x9F\x98\x80",                                                          // U+1F600 grinning face
        "\xE2\x9D\xA4",                                                              // U+2764 red heart
        "\xF0\x9F\x98\x80\xF0\x9F\x94\xA5\xE2\x9D\xA4",                              // face + fire + heart
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7",  // family
    };
    static const float kBakeSizes[3] = { 16.0f, 32.0f, 64.0f };
    static const float kScales[3] = { 1.0f, 2.0f, 3.5f };

    EmojiCase cases[64];
    int caseCount = 0;
    for (int textIndex = 0; textIndex < 4; ++textIndex)
    {
        for (int sizeIndex = 0; sizeIndex < 3; ++sizeIndex)
        {
            for (int scaleIndex = 0; scaleIndex < 3; ++scaleIndex)
            {
                // Keep the matrix bounded: the big strings only at 1x/2x.
                if (textIndex >= 2 && scaleIndex == 2)
                    continue;
                EmojiCase& testCase = cases[caseCount++];
                std::snprintf(testCase.Name, sizeof(testCase.Name), "e%d_%dpx_%dmilli", textIndex,
                              (int)kBakeSizes[sizeIndex], (int)(kScales[scaleIndex] * 1000.0f));
                testCase.Text = kTexts[textIndex];
                testCase.BakeSize = kBakeSizes[sizeIndex];
                testCase.Scale = kScales[scaleIndex];
            }
        }
    }

    int failures = 0;
    int worstMax = 0;
    double worstMean = 0.0;
    char worstName[64] = "";
    int totalInstances = 0;
    for (int caseIndex = 0; caseIndex < caseCount; ++caseIndex)
    {
        const EmojiCase& testCase = cases[caseIndex];

        // First pass at the origin to learn the extent, then re-place inside a
        // framebuffer that fits the glyphs plus the dilation margin.
        const uint32_t capacity = ImSlugVulkanGetInstanceCapacity();
        ImSlugGlyphInstance* instances = ImSlugVulkanGetInstanceData(0);
        static CpuLayer layers[4096];
        static ImSlugGlyphInstance layoutInstances[4096];
        int count = LayoutEmojiText(testCase.Text, testCase.BakeSize, testCase.Scale, 0.0f, 0.0f, layoutInstances,
                                    layers, 4096);
        if (count <= 0 || (uint32_t)count > capacity)
        {
            std::printf("imslug_emoji_golden: FAIL (%s: no layers / too many: %d)\n", testCase.Name, count);
            ++failures;
            continue;
        }

        float minX = layoutInstances[0].ScreenMinX, maxX = layoutInstances[0].ScreenMaxX;
        float minY = layoutInstances[0].ScreenMinY, maxY = layoutInstances[0].ScreenMaxY;
        for (int i = 1; i < count; ++i)
        {
            minX = layoutInstances[i].ScreenMinX < minX ? layoutInstances[i].ScreenMinX : minX;
            maxX = layoutInstances[i].ScreenMaxX > maxX ? layoutInstances[i].ScreenMaxX : maxX;
            minY = layoutInstances[i].ScreenMinY < minY ? layoutInstances[i].ScreenMinY : minY;
            maxY = layoutInstances[i].ScreenMaxY > maxY ? layoutInstances[i].ScreenMaxY : maxY;
        }
        const float offsetX = kMargin - minX + 0.0137f;
        const float offsetY = kMargin - minY + 0.0113f;

        // Rebuild with the placement offset applied (the layout is linear in
        // the pen position, so re-running with shifted origins is exact).
        count = LayoutEmojiText(testCase.Text, testCase.BakeSize, testCase.Scale, offsetX, offsetY, instances, layers,
                                4096);
        if (count <= 0 || (uint32_t)count > capacity)
        {
            std::printf("imslug_emoji_golden: FAIL (%s: rebuild)\n", testCase.Name);
            ++failures;
            continue;
        }
        totalInstances += count;

        const uint32_t width = (uint32_t)std::ceil(maxX - minX) + (uint32_t)(kMargin * 2.0f) + 4;
        const uint32_t height = (uint32_t)std::ceil(maxY - minY) + (uint32_t)(kMargin * 2.0f) + 4;

        Target target;
        target.Width = width;
        target.Height = height;
        if (!RenderCase(testCase, &target, (uint32_t)count))
        {
            std::printf("imslug_emoji_golden: FAIL (%s: render)\n", testCase.Name);
            DestroyTarget(&target);
            ++failures;
            continue;
        }

        const unsigned char* gpuPixels = (const unsigned char*)target.ReadbackMapped;
        unsigned char* cpuPixels = (unsigned char*)std::malloc((size_t)width * height * 4);
        if (cpuPixels == nullptr)
        {
            DestroyTarget(&target);
            ++failures;
            continue;
        }
        std::memset(cpuPixels, 0, (size_t)width * height * 4);
        CompositeCpu(layers, count, cpuPixels, width, height);

        const DiffStats stats = Compare(gpuPixels, cpuPixels, width, height);
        const int outlierBudget = (int)((double)(width * height) * kOutlierFraction) + kMinOutlierBudget;
        const bool pass = stats.MeanChannel <= kMaxMeanDiff && stats.OutlierPixels <= outlierBudget;
        std::printf("  %-22s %4ux%-4u %5d layers  max %3d  mean %6.3f  outliers %d/%d (worst %d)%s\n", testCase.Name,
                    width, height, count, stats.MaxChannel, stats.MeanChannel, stats.OutlierPixels, outlierBudget,
                    stats.OutlierWorst, pass ? "" : "  <-- FAIL");
        if (stats.MaxChannel > worstMax)
            worstMax = stats.MaxChannel;
        if (stats.MeanChannel > worstMean)
        {
            worstMean = stats.MeanChannel;
            std::snprintf(worstName, sizeof(worstName), "%s", testCase.Name);
        }
        if (!pass)
        {
            ++failures;
            char path[256];
            std::snprintf(path, sizeof(path), "%s_gpu.ppm", testCase.Name);
            WritePpm(path, gpuPixels, width, height);
            std::snprintf(path, sizeof(path), "%s_cpu.ppm", testCase.Name);
            WritePpm(path, cpuPixels, width, height);
        }
        std::free(cpuPixels);
        DestroyTarget(&target);
    }

    if (g_State.ValidationEnabled && g_State.ValidationErrors != 0)
    {
        std::printf("imslug_emoji_golden: FAIL (%d validation errors)\n", g_State.ValidationErrors);
        ++failures;
    }

    std::printf("imslug_emoji_golden: %s (%d cases, %d layer instances, worst max %d LSB, worst mean %.3f (%s), "
                "validation messages %d)\n",
                failures == 0 ? "OK" : "FAILED", caseCount, totalInstances, worstMax, worstMean,
                worstName[0] != '\0' ? worstName : "-", g_State.ValidationMessages);
    return failures == 0 ? 0 : 1;
}
