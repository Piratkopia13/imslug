// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// example/glfw_vulkan/main.cpp — standalone GLFW + Vulkan example (M2).
//
// Demonstrates the whole M2 pipeline without ImGui: loads a TTF through the M1
// core, uploads the curve/band pools, renders text through
// backends/imslug_impl_vulkan.* and lets you change the bake size and the
// fractional zoom (0.5x .. 8x) to inspect sharpness. Vulkan validation layers
// are enabled by default (set IMSLUG_VALIDATION=0 to disable); every message is
// printed and the error count is reported on exit.
//
// Usage:
//   imslug_example_glfw_vulkan [--font <path>] [--size <px>] [--zoom <scale>]
//                              [--screenshot <file.ppm> <px_size> <scale>]
// Keys:
//   + / -    bake size +-4 px (12 .. 96)        , / .   zoom -+ 0.25x (0.5 .. 8)
//   0        reset size/zoom to the defaults    Esc     quit
//
// Third-party: GLFW (zlib license) is fetched by CMake; the example itself is
// imslug code. Fonts come from tests/fonts/ (Roboto, Apache-2.0).

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "imslug_impl_vulkan.h"
#include "imslug_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace ImSlug;

namespace
{
    const uint32_t kMaxFramesInFlight = 3;
    const int kDefaultWindowWidth = 1280;
    const int kDefaultWindowHeight = 800;
    const int kDefaultSize = 32;
    const float kDefaultZoom = 1.0f;
    const float kMinZoom = 0.5f;
    const float kMaxZoom = 8.0f;
    const int kMargin = 24;

    struct App
    {
        GLFWwindow* Window;
        bool ValidationEnabled;
        int ValidationErrors;
        int ValidationMessages;

        VkInstance Instance;
        VkDebugUtilsMessengerEXT DebugMessenger;
        VkPhysicalDevice PhysicalDevice;
        VkDevice Device;
        VkQueue Queue;
        uint32_t QueueFamily;
        VkSurfaceKHR Surface;

        VkSwapchainKHR Swapchain;
        VkFormat SwapchainFormat;
        VkExtent2D SwapchainExtent;
        uint32_t SwapchainImageCount;
        VkImage* SwapchainImages;
        VkImageView* SwapchainViews;
        VkFramebuffer* Framebuffers;
        VkRenderPass RenderPass;

        VkCommandPool CommandPool;
        VkCommandBuffer* CommandBuffers;
        VkSemaphore ImageAvailable[kMaxFramesInFlight];
        VkSemaphore RenderFinished[kMaxFramesInFlight];
        VkFence InFlight[kMaxFramesInFlight];
        uint32_t FrameIndex;
        bool SwapchainDirty;

        ImSlugFont* Font;
        ImSlugPools Pools;

        int BakeSize;
        float Zoom;
        const char* FontPath;

        // Screenshot mode.
        const char* ScreenshotPath;
        int ScreenshotFrame;
        int FramesRendered;
        bool ScreenshotRequested;
        bool ShouldClose;
        VkBuffer ScreenshotBuffer;
        VkDeviceMemory ScreenshotMemory;
        void* ScreenshotMapped;
        VkDeviceSize ScreenshotBufferSize;
    };

    App g_App = {};

    // ---------------------------------------------------------------------
    // Tiny helpers
    // ---------------------------------------------------------------------
    bool Check(VkResult result, const char* what)
    {
        if (result != VK_SUCCESS)
        {
            fprintf(stderr, "[imslug] FATAL: %s failed (VkResult %d)\n", what, (int)result);
            return false;
        }
        return true;
    }

    unsigned char* ReadWholeFile(const char* path, int* outSize)
    {
        FILE* file = fopen(path, "rb");
        if (file == nullptr)
            return nullptr;
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        if (size <= 0)
        {
            fclose(file);
            return nullptr;
        }
        unsigned char* data = (unsigned char*)malloc((size_t)size);
        if (data == nullptr)
        {
            fclose(file);
            return nullptr;
        }
        const size_t read = fread(data, 1, (size_t)size, file);
        fclose(file);
        if (read != (size_t)size)
        {
            free(data);
            return nullptr;
        }
        *outSize = (int)size;
        return data;
    }

    const char* SeverityName(VkDebugUtilsMessageSeverityFlagBitsEXT severity)
    {
        switch (severity)
        {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            return "error";
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            return "warning";
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            return "info";
        default:
            return "verbose";
        }
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT types,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
    {
        (void)types;
        (void)userData;
        ++g_App.ValidationMessages;
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            ++g_App.ValidationErrors;
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            fprintf(stderr, "[vulkan %s] %s\n", SeverityName(severity), data->pMessage);
        else if (getenv("IMSLUG_VALIDATION_VERBOSE") != nullptr)
            fprintf(stdout, "[vulkan %s] %s\n", SeverityName(severity), data->pMessage);
        return VK_FALSE;
    }

    bool EnvFlag(const char* name, bool defaultValue)
    {
        const char* value = getenv(name);
        if (value == nullptr || *value == '\0')
            return defaultValue;
        return !(value[0] == '0' || value[0] == 'f' || value[0] == 'F' || value[0] == 'n' || value[0] == 'N');
    }

    // ---------------------------------------------------------------------
    // Instance / device
    // ---------------------------------------------------------------------
    bool CreateInstance()
    {
        g_App.ValidationEnabled = EnvFlag("IMSLUG_VALIDATION", true);

        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        if (glfwExtensions == nullptr || glfwExtensionCount == 0)
        {
            fprintf(stderr, "[imslug] GLFW did not provide instance extensions (no Vulkan support?)\n");
            return false;
        }
        const char* extensions[8] = {};
        uint32_t extensionCount = 0;
        for (uint32_t i = 0; i < glfwExtensionCount && extensionCount < 8; ++i)
            extensions[extensionCount++] = glfwExtensions[i];
        if (g_App.ValidationEnabled)
            extensions[extensionCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

        VkApplicationInfo applicationInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        applicationInfo.pApplicationName = "imslug example (GLFW + Vulkan)";
        applicationInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        applicationInfo.pEngineName = "imslug";
        applicationInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        applicationInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo createInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        createInfo.pApplicationInfo = &applicationInfo;
        createInfo.enabledExtensionCount = extensionCount;
        createInfo.ppEnabledExtensionNames = extensions;

        const char* layers[1] = { "VK_LAYER_KHRONOS_validation" };
        if (g_App.ValidationEnabled)
        {
            // Enable the layer only when it is actually available.
            uint32_t layerCount = 0;
            vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            VkLayerProperties* available =
                (VkLayerProperties*)malloc(sizeof(VkLayerProperties) * (layerCount ? layerCount : 1));
            bool found = false;
            if (available != nullptr && layerCount > 0)
            {
                vkEnumerateInstanceLayerProperties(&layerCount, available);
                for (uint32_t i = 0; i < layerCount; ++i)
                    found = found || strcmp(available[i].layerName, layers[0]) == 0;
            }
            free(available);
            if (found)
            {
                createInfo.enabledLayerCount = 1;
                createInfo.ppEnabledLayerNames = layers;
            }
            else
            {
                printf("[imslug] VK_LAYER_KHRONOS_validation not available - running without validation\n");
                g_App.ValidationEnabled = false;
            }
        }

        if (!Check(vkCreateInstance(&createInfo, nullptr, &g_App.Instance), "vkCreateInstance"))
            return false;

        if (g_App.ValidationEnabled)
        {
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT
            };
            messengerInfo.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            if (getenv("IMSLUG_VALIDATION_VERBOSE") != nullptr)
                messengerInfo.messageSeverity |=
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
            messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messengerInfo.pfnUserCallback = DebugCallback;
            PFN_vkCreateDebugUtilsMessengerEXT createMessenger =
                (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_App.Instance,
                                                                          "vkCreateDebugUtilsMessengerEXT");
            if (createMessenger != nullptr)
                createMessenger(g_App.Instance, &messengerInfo, nullptr, &g_App.DebugMessenger);
        }
        return true;
    }

    bool FindQueueFamily(VkPhysicalDevice device, uint32_t* outFamily)
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        if (count == 0)
            return false;
        VkQueueFamilyProperties* families = (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * count);
        if (families == nullptr)
            return false;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);
        bool found = false;
        for (uint32_t i = 0; i < count && !found; ++i)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, g_App.Surface, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present == VK_TRUE)
            {
                *outFamily = i;
                found = true;
            }
        }
        free(families);
        return found;
    }

    bool PickPhysicalDevice()
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(g_App.Instance, &deviceCount, nullptr);
        if (deviceCount == 0)
        {
            fprintf(stderr, "[imslug] no Vulkan physical devices\n");
            return false;
        }
        VkPhysicalDevice* devices = (VkPhysicalDevice*)malloc(sizeof(VkPhysicalDevice) * deviceCount);
        if (devices == nullptr)
            return false;
        vkEnumeratePhysicalDevices(g_App.Instance, &deviceCount, devices);

        // IMSLUG_VK_DEVICE=<index> forces one; otherwise prefer a discrete GPU
        // with a graphics+present queue family (then integrated, then any).
        int forced = -1;
        const char* env = getenv("IMSLUG_VK_DEVICE");
        if (env != nullptr && *env != '\0')
            forced = atoi(env);

        int best = -1;
        int bestScore = -1;
        for (uint32_t i = 0; i < deviceCount; ++i)
        {
            uint32_t family = 0;
            if (!FindQueueFamily(devices[i], &family))
                continue;
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(devices[i], &properties);
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : 2;
            if (forced >= 0)
            {
                if ((int)i == forced)
                {
                    best = (int)i;
                    bestScore = score;
                    break;
                }
                continue;
            }
            if (score > bestScore)
            {
                best = (int)i;
                bestScore = score;
            }
        }
        if (best < 0)
        {
            fprintf(stderr, "[imslug] no usable physical device (graphics + present)\n");
            free(devices);
            return false;
        }

        g_App.PhysicalDevice = devices[best];
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(g_App.PhysicalDevice, &properties);
        printf("[imslug] device: %s (Vulkan %u.%u.%u)\n", properties.deviceName,
               VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
               VK_VERSION_PATCH(properties.apiVersion));
        free(devices);

        if (!FindQueueFamily(g_App.PhysicalDevice, &g_App.QueueFamily))
            return false;

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = g_App.QueueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        const char* deviceExtensions[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo createInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        createInfo.enabledExtensionCount = 1;
        createInfo.ppEnabledExtensionNames = deviceExtensions;
        if (!Check(vkCreateDevice(g_App.PhysicalDevice, &createInfo, nullptr, &g_App.Device), "vkCreateDevice"))
            return false;
        vkGetDeviceQueue(g_App.Device, g_App.QueueFamily, 0, &g_App.Queue);
        return true;
    }

    // ---------------------------------------------------------------------
    // Swapchain / render pass / sync
    // ---------------------------------------------------------------------
    bool CreateSwapchain()
    {
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_App.PhysicalDevice, g_App.Surface, &capabilities);

        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == UINT32_MAX)
        {
            int width = 0, height = 0;
            glfwGetFramebufferSize(g_App.Window, &width, &height);
            extent.width = (uint32_t)width;
            extent.height = (uint32_t)height;
            if (extent.width < capabilities.minImageExtent.width)
                extent.width = capabilities.minImageExtent.width;
            if (extent.width > capabilities.maxImageExtent.width)
                extent.width = capabilities.maxImageExtent.width;
            if (extent.height < capabilities.minImageExtent.height)
                extent.height = capabilities.minImageExtent.height;
            if (extent.height > capabilities.maxImageExtent.height)
                extent.height = capabilities.maxImageExtent.height;
        }
        if (extent.width == 0 || extent.height == 0)
            return false;

        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
            imageCount = capabilities.maxImageCount;

        VkSwapchainCreateInfoKHR createInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        createInfo.surface = g_App.Surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = g_App.SwapchainFormat;
        createInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        createInfo.clipped = VK_TRUE;
        if (!Check(vkCreateSwapchainKHR(g_App.Device, &createInfo, nullptr, &g_App.Swapchain), "vkCreateSwapchainKHR"))
            return false;

        vkGetSwapchainImagesKHR(g_App.Device, g_App.Swapchain, &g_App.SwapchainImageCount, nullptr);
        g_App.SwapchainImages = (VkImage*)malloc(sizeof(VkImage) * g_App.SwapchainImageCount);
        vkGetSwapchainImagesKHR(g_App.Device, g_App.Swapchain, &g_App.SwapchainImageCount, g_App.SwapchainImages);
        g_App.SwapchainExtent = extent;

        g_App.SwapchainViews = (VkImageView*)malloc(sizeof(VkImageView) * g_App.SwapchainImageCount);
        for (uint32_t i = 0; i < g_App.SwapchainImageCount; ++i)
        {
            VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            viewInfo.image = g_App.SwapchainImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = g_App.SwapchainFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            if (!Check(vkCreateImageView(g_App.Device, &viewInfo, nullptr, &g_App.SwapchainViews[i]),
                       "vkCreateImageView"))
                return false;
        }

        VkAttachmentDescription colorAttachment = {};
        colorAttachment.format = g_App.SwapchainFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo renderPassInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        if (!Check(vkCreateRenderPass(g_App.Device, &renderPassInfo, nullptr, &g_App.RenderPass), "vkCreateRenderPass"))
            return false;

        g_App.Framebuffers = (VkFramebuffer*)malloc(sizeof(VkFramebuffer) * g_App.SwapchainImageCount);
        for (uint32_t i = 0; i < g_App.SwapchainImageCount; ++i)
        {
            VkFramebufferCreateInfo framebufferInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            framebufferInfo.renderPass = g_App.RenderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &g_App.SwapchainViews[i];
            framebufferInfo.width = extent.width;
            framebufferInfo.height = extent.height;
            framebufferInfo.layers = 1;
            if (!Check(vkCreateFramebuffer(g_App.Device, &framebufferInfo, nullptr, &g_App.Framebuffers[i]),
                       "vkCreateFramebuffer"))
                return false;
        }
        return true;
    }

    void DestroySwapchain()
    {
        if (g_App.Framebuffers != nullptr)
        {
            for (uint32_t i = 0; i < g_App.SwapchainImageCount; ++i)
                vkDestroyFramebuffer(g_App.Device, g_App.Framebuffers[i], nullptr);
            free(g_App.Framebuffers);
            g_App.Framebuffers = nullptr;
        }
        if (g_App.RenderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(g_App.Device, g_App.RenderPass, nullptr);
        g_App.RenderPass = VK_NULL_HANDLE;
        if (g_App.SwapchainViews != nullptr)
        {
            for (uint32_t i = 0; i < g_App.SwapchainImageCount; ++i)
                vkDestroyImageView(g_App.Device, g_App.SwapchainViews[i], nullptr);
            free(g_App.SwapchainViews);
            g_App.SwapchainViews = nullptr;
        }
        if (g_App.SwapchainImages != nullptr)
        {
            free(g_App.SwapchainImages);
            g_App.SwapchainImages = nullptr;
        }
        if (g_App.Swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(g_App.Device, g_App.Swapchain, nullptr);
        g_App.Swapchain = VK_NULL_HANDLE;
    }

    bool ChooseSwapchainFormat()
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(g_App.PhysicalDevice, g_App.Surface, &count, nullptr);
        if (count == 0)
            return false;
        VkSurfaceFormatKHR* formats = (VkSurfaceFormatKHR*)malloc(sizeof(VkSurfaceFormatKHR) * count);
        if (formats == nullptr)
            return false;
        vkGetPhysicalDeviceSurfaceFormatsKHR(g_App.PhysicalDevice, g_App.Surface, &count, formats);
        // Prefer a plain 8-bit UNORM format so the screenshot bytes are not
        // sRGB-encoded; fall back to whatever the surface offers.
        g_App.SwapchainFormat = formats[0].format;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)
            {
                g_App.SwapchainFormat = formats[i].format;
                break;
            }
        }
        free(formats);
        return true;
    }

    bool CreateSyncObjects()
    {
        VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        {
            if (!Check(vkCreateSemaphore(g_App.Device, &semaphoreInfo, nullptr, &g_App.ImageAvailable[i]),
                       "vkCreateSemaphore"))
                return false;
            if (!Check(vkCreateSemaphore(g_App.Device, &semaphoreInfo, nullptr, &g_App.RenderFinished[i]),
                       "vkCreateSemaphore"))
                return false;
            if (!Check(vkCreateFence(g_App.Device, &fenceInfo, nullptr, &g_App.InFlight[i]), "vkCreateFence"))
                return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------
    // Text
    // ---------------------------------------------------------------------
    struct TextBatch
    {
        uint32_t FirstInstance;
        uint32_t InstanceCount;
    };

    // Lays out an ASCII string left to right and appends one instance per
    // non-empty glyph. Returns the instance count written.
    uint32_t BuildText(const char* text, float penX, float baselineY, float pixelsPerEm, float r, float g, float b,
                       float a, ImSlugGlyphInstance* out, uint32_t capacity)
    {
        uint32_t count = 0;
        for (const char* p = text; *p != '\0' && count < capacity; ++p)
        {
            const ImSlugGlyphRecord* record = nullptr;
            if (ImSlugGetGlyphRecord(g_App.Font, &g_App.Pools, (unsigned char)*p, &record) != ImSlugBuildStatus_Ok)
                continue;
            if (record->CurveCount > 0)
                ImSlugBuildGlyphInstance(record, penX, baselineY, pixelsPerEm, r, g, b, a, &out[count++]);
            penX += record->Advance * pixelsPerEm;
        }
        return count;
    }

    float AscentFor(float bakeSize, float scale)
    {
        // Ascent/descent in pixels at `bakeSize`, scaled to the render scale.
        ImSlugFontMetrics metrics;
        ImSlugGetFontMetrics(g_App.Font, bakeSize, &metrics);
        return metrics.Ascent * scale;
    }

    // ---------------------------------------------------------------------
    // Frame building
    // ---------------------------------------------------------------------
    struct FrameBatches
    {
        uint32_t InstanceCount;
        TextBatch Main;
        TextBatch Ladder[3];
        uint32_t LadderCount;
        TextBatch Hud;
    };

    void BuildFrame(ImSlugGlyphInstance* instances, uint32_t capacity, FrameBatches* batches)
    {
        memset(batches, 0, sizeof(*batches));
        const float height = (float)g_App.SwapchainExtent.height;
        const float mainPpem = (float)g_App.BakeSize * g_App.Zoom;

        // Main line: the string the keyboard controls act on.
        const char* mainText = "Slug on Vulkan - sharp at any size";
        const float mainAscent = AscentFor((float)g_App.BakeSize, g_App.Zoom);
        float baseline = (float)kMargin + mainAscent;
        batches->Main.FirstInstance = batches->InstanceCount;
        batches->Main.InstanceCount = BuildText(mainText, (float)kMargin, baseline, mainPpem, 0.95f, 0.96f, 1.0f, 1.0f,
                                                instances + batches->InstanceCount, capacity - batches->InstanceCount);
        batches->InstanceCount += batches->Main.InstanceCount;

        // Ladder: the same text at fixed fractional scales (sharpness evidence).
        const float ladderScales[3] = { 0.5f, 1.0f, 2.0f };
        baseline += mainAscent + (float)kMargin * 0.5f;
        for (int i = 0; i < 3; ++i)
        {
            const float ppem = (float)g_App.BakeSize * ladderScales[i];
            baseline += AscentFor((float)g_App.BakeSize, ladderScales[i]);
            if (baseline > height - 40.0f)
                break;
            batches->Ladder[batches->LadderCount].FirstInstance = batches->InstanceCount;
            batches->Ladder[batches->LadderCount].InstanceCount =
                BuildText("Hamburgefonstiv 0123", (float)kMargin, baseline, ppem, 0.75f, 0.82f, 0.95f, 1.0f,
                          instances + batches->InstanceCount, capacity - batches->InstanceCount);
            batches->InstanceCount += batches->Ladder[batches->LadderCount].InstanceCount;
            ++batches->LadderCount;
            baseline += 12.0f;
        }

        // HUD line at the bottom.
        char hud[256];
        snprintf(hud, sizeof(hud),
                 "bake %d px | zoom %.2fx | pixelsPerEm %.1f | validation %s | +/- size, </> zoom, 0 reset",
                 g_App.BakeSize, g_App.Zoom, mainPpem, g_App.ValidationEnabled ? "ON" : "off");
        batches->Hud.FirstInstance = batches->InstanceCount;
        batches->Hud.InstanceCount =
            BuildText(hud, (float)kMargin, height - (float)kMargin, 14.0f, 0.65f, 0.7f, 0.78f, 1.0f,
                      instances + batches->InstanceCount, capacity - batches->InstanceCount);
        batches->InstanceCount += batches->Hud.InstanceCount;
    }

    // ---------------------------------------------------------------------
    // Screenshot (PPM readback)
    // ---------------------------------------------------------------------
    bool WritePpm(const char* path, const unsigned char* pixels, uint32_t width, uint32_t height, VkFormat format)
    {
        FILE* file = fopen(path, "wb");
        if (file == nullptr)
        {
            fprintf(stderr, "[imslug] cannot write %s\n", path);
            return false;
        }
        fprintf(file, "P6\n%u %u\n255\n", width, height);
        const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
        unsigned char* row = (unsigned char*)malloc((size_t)width * 3);
        for (uint32_t y = 0; y < height; ++y)
        {
            const unsigned char* source = pixels + (size_t)y * width * 4;
            for (uint32_t x = 0; x < width; ++x)
            {
                row[x * 3 + 0] = bgra ? source[x * 4 + 2] : source[x * 4 + 0];
                row[x * 3 + 1] = source[x * 4 + 1];
                row[x * 3 + 2] = bgra ? source[x * 4 + 0] : source[x * 4 + 2];
            }
            fwrite(row, 3, width, file);
        }
        free(row);
        fclose(file);
        printf("[imslug] screenshot written: %s (%ux%u)\n", path, width, height);
        return true;
    }

    bool EnsureScreenshotBuffer(VkDeviceSize size)
    {
        if (g_App.ScreenshotBuffer != VK_NULL_HANDLE && g_App.ScreenshotBufferSize >= size)
            return true;
        if (g_App.ScreenshotBuffer != VK_NULL_HANDLE)
        {
            vkUnmapMemory(g_App.Device, g_App.ScreenshotMemory);
            vkDestroyBuffer(g_App.Device, g_App.ScreenshotBuffer, nullptr);
            vkFreeMemory(g_App.Device, g_App.ScreenshotMemory, nullptr);
            g_App.ScreenshotBuffer = VK_NULL_HANDLE;
            g_App.ScreenshotMemory = VK_NULL_HANDLE;
        }
        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!Check(vkCreateBuffer(g_App.Device, &bufferInfo, nullptr, &g_App.ScreenshotBuffer), "vkCreateBuffer"))
            return false;
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(g_App.Device, g_App.ScreenshotBuffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties;
        vkGetPhysicalDeviceMemoryProperties(g_App.PhysicalDevice, &memoryProperties);
        uint32_t memoryType = 0;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        {
            if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            {
                memoryType = i;
                break;
            }
        }
        VkMemoryAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = memoryType;
        if (!Check(vkAllocateMemory(g_App.Device, &allocateInfo, nullptr, &g_App.ScreenshotMemory), "vkAllocateMemory"))
            return false;
        vkBindBufferMemory(g_App.Device, g_App.ScreenshotBuffer, g_App.ScreenshotMemory, 0);
        vkMapMemory(g_App.Device, g_App.ScreenshotMemory, 0, VK_WHOLE_SIZE, 0, &g_App.ScreenshotMapped);
        g_App.ScreenshotBufferSize = size;
        return true;
    }

    void RecordScreenshotCopy(VkCommandBuffer cmd, uint32_t imageIndex)
    {
        const VkImage image = g_App.SwapchainImages[imageIndex];
        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = g_App.SwapchainExtent.width;
        region.imageExtent.height = g_App.SwapchainExtent.height;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_App.ScreenshotBuffer, 1, &region);

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
    }

    // ---------------------------------------------------------------------
    // Input
    // ---------------------------------------------------------------------
    void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
    {
        (void)window;
        (void)scancode;
        (void)mods;
        if (action != GLFW_PRESS && action != GLFW_REPEAT)
            return;
        if (key == GLFW_KEY_ESCAPE)
            g_App.ShouldClose = true;
        else if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD)
            g_App.BakeSize = g_App.BakeSize < 96 ? g_App.BakeSize + 4 : g_App.BakeSize;
        else if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT)
            g_App.BakeSize = g_App.BakeSize > 12 ? g_App.BakeSize - 4 : g_App.BakeSize;
        else if (key == GLFW_KEY_PERIOD || key == GLFW_KEY_KP_MULTIPLY)
            g_App.Zoom = g_App.Zoom < kMaxZoom ? g_App.Zoom + 0.25f : g_App.Zoom;
        else if (key == GLFW_KEY_COMMA || key == GLFW_KEY_KP_DIVIDE)
            g_App.Zoom = g_App.Zoom > kMinZoom ? g_App.Zoom - 0.25f : g_App.Zoom;
        else if (key == GLFW_KEY_0)
        {
            g_App.BakeSize = kDefaultSize;
            g_App.Zoom = kDefaultZoom;
        }
        else
            return;
        printf("[imslug] bake %d px, zoom %.2fx (pixelsPerEm %.1f)\n", g_App.BakeSize, g_App.Zoom,
               (float)g_App.BakeSize * g_App.Zoom);
    }

    // ---------------------------------------------------------------------
    // Frame loop
    // ---------------------------------------------------------------------
    bool RenderFrame()
    {
        App& app = g_App;
        const uint32_t frame = app.FrameIndex;

        if (!Check(vkWaitForFences(app.Device, 1, &app.InFlight[frame], VK_TRUE, UINT64_MAX), "vkWaitForFences"))
            return false;

        uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(app.Device, app.Swapchain, UINT64_MAX, app.ImageAvailable[frame],
                                                 VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
        {
            app.SwapchainDirty = true;
            return true;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
            return Check(acquire, "vkAcquireNextImageKHR");

        if (!Check(vkResetFences(app.Device, 1, &app.InFlight[frame]), "vkResetFences"))
            return false;
        if (!Check(vkResetCommandBuffer(app.CommandBuffers[frame], 0), "vkResetCommandBuffer"))
            return false;

        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(app.CommandBuffers[frame], &beginInfo);

        // Pool uploads: dirty rows only (the first call uploads everything).
        if (!ImSlugVulkanUploadPools(app.CommandBuffers[frame], &app.Pools))
        {
            fprintf(stderr, "[imslug] pool upload failed\n");
            return false;
        }

        // Build this frame's instances in the ring slot.
        ImSlugGlyphInstance* instances = ImSlugVulkanGetInstanceData(frame);
        FrameBatches batches;
        BuildFrame(instances, ImSlugVulkanGetInstanceCapacity(), &batches);
        if (batches.InstanceCount > ImSlugVulkanGetInstanceCapacity())
            batches.InstanceCount = ImSlugVulkanGetInstanceCapacity();

        VkClearValue clearValue = {};
        clearValue.color.float32[0] = 0.055f;
        clearValue.color.float32[1] = 0.06f;
        clearValue.color.float32[2] = 0.075f;
        clearValue.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo renderPassInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        renderPassInfo.renderPass = app.RenderPass;
        renderPassInfo.framebuffer = app.Framebuffers[imageIndex];
        renderPassInfo.renderArea.extent = app.SwapchainExtent;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearValue;
        vkCmdBeginRenderPass(app.CommandBuffers[frame], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport = {};
        viewport.width = (float)app.SwapchainExtent.width;
        viewport.height = (float)app.SwapchainExtent.height;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(app.CommandBuffers[frame], 0, 1, &viewport);

        float mvp[16];
        ImSlugVulkanBuildOrthoMVP((float)app.SwapchainExtent.width, (float)app.SwapchainExtent.height, mvp);

        VkRect2D fullScissor = {};
        fullScissor.extent = app.SwapchainExtent;

        ImSlugVulkanDrawInfo draw = {};
        draw.FrameIndex = frame;
        draw.RenderPass = app.RenderPass;
        draw.Subpass = 0;
        draw.MVP = mvp;
        draw.ViewportWidth = (float)app.SwapchainExtent.width;
        draw.ViewportHeight = (float)app.SwapchainExtent.height;
        draw.Scissor = fullScissor;

        const TextBatch* allBatches[5] = { &batches.Main, &batches.Hud, &batches.Ladder[0], &batches.Ladder[1],
                                           &batches.Ladder[2] };
        const int batchCount = 2 + (int)batches.LadderCount;
        for (int i = 0; i < batchCount; ++i)
        {
            draw.FirstInstance = allBatches[i]->FirstInstance;
            draw.InstanceCount = allBatches[i]->InstanceCount;
            ImSlugVulkanDraw(app.CommandBuffers[frame], &draw);
        }

        vkCmdEndRenderPass(app.CommandBuffers[frame]);

        // Screenshot: copy the rendered swapchain image (still acquired) to the
        // readback buffer inside the same submission.
        app.ScreenshotRequested = false;
        if (app.ScreenshotPath != nullptr && app.FramesRendered >= app.ScreenshotFrame)
        {
            if (!EnsureScreenshotBuffer((VkDeviceSize)app.SwapchainExtent.width * app.SwapchainExtent.height * 4))
                return false;
            RecordScreenshotCopy(app.CommandBuffers[frame], imageIndex);
            app.ScreenshotRequested = true;
        }

        if (!Check(vkEndCommandBuffer(app.CommandBuffers[frame]), "vkEndCommandBuffer"))
            return false;

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &app.ImageAvailable[frame];
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &app.CommandBuffers[frame];
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &app.RenderFinished[frame];
        if (!Check(vkQueueSubmit(app.Queue, 1, &submitInfo, app.InFlight[frame]), "vkQueueSubmit"))
            return false;

        VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &app.RenderFinished[frame];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &app.Swapchain;
        presentInfo.pImageIndices = &imageIndex;
        const VkResult present = vkQueuePresentKHR(app.Queue, &presentInfo);
        if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR)
            app.SwapchainDirty = true;
        else if (present != VK_SUCCESS)
            return Check(present, "vkQueuePresentKHR");

        if (app.ScreenshotRequested)
        {
            if (!Check(vkQueueWaitIdle(app.Queue), "vkQueueWaitIdle"))
                return false;
            WritePpm(app.ScreenshotPath, (const unsigned char*)app.ScreenshotMapped, app.SwapchainExtent.width,
                     app.SwapchainExtent.height, app.SwapchainFormat);
            app.ShouldClose = true;
        }

        app.FramesRendered++;
        app.FrameIndex = (frame + 1) % kMaxFramesInFlight;
        return true;
    }

    // ---------------------------------------------------------------------
    // Main
    // ---------------------------------------------------------------------
    void ParseArgs(int argc, char** argv)
    {
        g_App.FontPath = nullptr;
        g_App.BakeSize = kDefaultSize;
        g_App.Zoom = kDefaultZoom;
        g_App.ScreenshotPath = nullptr;
        g_App.ScreenshotFrame = 2;
        g_App.FramesRendered = 0;
        for (int i = 1; i < argc; ++i)
        {
            if (strcmp(argv[i], "--font") == 0 && i + 1 < argc)
                g_App.FontPath = argv[++i];
            else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
                g_App.BakeSize = atoi(argv[++i]);
            else if (strcmp(argv[i], "--zoom") == 0 && i + 1 < argc)
                g_App.Zoom = (float)atof(argv[++i]);
            else if (strcmp(argv[i], "--screenshot") == 0 && i + 3 < argc)
            {
                g_App.ScreenshotPath = argv[++i];
                g_App.BakeSize = atoi(argv[++i]);
                g_App.Zoom = (float)atof(argv[++i]);
            }
            else if (strcmp(argv[i], "--help") == 0)
            {
                printf("usage: imslug_example_glfw_vulkan [--font <path>] [--size <px>] [--zoom <scale>]\n"
                       "                                  [--screenshot <file.ppm> <px_size> <scale>]\n");
                exit(0);
            }
            else
            {
                fprintf(stderr, "[imslug] unknown argument: %s\n", argv[i]);
                exit(1);
            }
        }
        if (g_App.BakeSize < 12)
            g_App.BakeSize = 12;
        if (g_App.BakeSize > 96)
            g_App.BakeSize = 96;
        if (g_App.Zoom < kMinZoom)
            g_App.Zoom = kMinZoom;
        if (g_App.Zoom > kMaxZoom)
            g_App.Zoom = kMaxZoom;
    }

    int Run()
    {
        if (glfwInit() != GLFW_TRUE)
        {
            fprintf(stderr, "[imslug] glfwInit failed\n");
            return 1;
        }
        if (glfwVulkanSupported() != GLFW_TRUE)
        {
            fprintf(stderr, "[imslug] GLFW reports no Vulkan support\n");
            glfwTerminate();
            return 1;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        g_App.Window =
            glfwCreateWindow(kDefaultWindowWidth, kDefaultWindowHeight, "imslug - Slug text (M2)", nullptr, nullptr);
        if (g_App.Window == nullptr)
        {
            fprintf(stderr, "[imslug] glfwCreateWindow failed\n");
            glfwTerminate();
            return 1;
        }
        glfwSetKeyCallback(g_App.Window, KeyCallback);

        bool ok = CreateInstance();
        if (ok && glfwCreateWindowSurface(g_App.Instance, g_App.Window, nullptr, &g_App.Surface) != VK_SUCCESS)
        {
            fprintf(stderr, "[imslug] glfwCreateWindowSurface failed\n");
            ok = false;
        }
        if (ok)
            ok = PickPhysicalDevice();
        if (ok)
            ok = ChooseSwapchainFormat();
        if (ok)
            ok = CreateSwapchain();

        // Font: default to the committed test font next to the executable's
        // source tree; --font overrides.
        if (ok)
        {
            const char* fontPath = g_App.FontPath != nullptr ? g_App.FontPath : IMSLUG_EXAMPLE_FONT_PATH;
            int fontSize = 0;
            unsigned char* fontData = ReadWholeFile(fontPath, &fontSize);
            if (fontData == nullptr)
            {
                fprintf(stderr, "[imslug] cannot read font: %s\n", fontPath);
                ok = false;
            }
            else
            {
                printf("[imslug] font: %s (%d bytes)\n", fontPath, fontSize);
                g_App.Font = ImSlugCreateFontFromMemory(fontData, fontSize, 0, true);
                free(fontData);
                if (g_App.Font == nullptr)
                {
                    fprintf(stderr, "[imslug] ImSlugCreateFontFromMemory failed\n");
                    ok = false;
                }
            }
        }
        ImSlugInitPools(&g_App.Pools);

        if (ok)
        {
            VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = g_App.QueueFamily;
            ok =
                Check(vkCreateCommandPool(g_App.Device, &poolInfo, nullptr, &g_App.CommandPool), "vkCreateCommandPool");
        }
        if (ok)
        {
            g_App.CommandBuffers = (VkCommandBuffer*)malloc(sizeof(VkCommandBuffer) * kMaxFramesInFlight);
            VkCommandBufferAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocateInfo.commandPool = g_App.CommandPool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = kMaxFramesInFlight;
            ok = Check(vkAllocateCommandBuffers(g_App.Device, &allocateInfo, g_App.CommandBuffers),
                       "vkAllocateCommandBuffers");
        }
        if (ok)
            ok = CreateSyncObjects();

        // imslug backend: maps 1:1 from what M3 will get out of
        // ImGui_ImplVulkan_InitInfo.PipelineInfoMain.
        if (ok)
        {
            ImSlugVulkanInitInfo initInfo;
            initInfo.PhysicalDevice = g_App.PhysicalDevice;
            initInfo.Device = g_App.Device;
            initInfo.Queue = g_App.Queue;
            initInfo.QueueFamily = g_App.QueueFamily;
            initInfo.Allocator = nullptr;
            initInfo.ImageCount = g_App.SwapchainImageCount;
            initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            initInfo.PipelineCache = VK_NULL_HANDLE;
            initInfo.RenderPass = g_App.RenderPass;
            initInfo.Subpass = 0;
            initInfo.UseDynamicRendering = false;
            ok = ImSlugVulkanInit(&initInfo);
            if (!ok)
                fprintf(stderr, "[imslug] ImSlugVulkanInit failed\n");
        }

        // Warm the glyph cache for the whole frame once so the screenshot path
        // is deterministic; per-frame builds then only append new glyphs.
        if (ok)
        {
            ImSlugGlyphInstance scratch[256];
            FrameBatches batches;
            BuildFrame(scratch, 256, &batches);
            if (batches.InstanceCount > 256)
                fprintf(stderr, "[imslug] warning: frame needs %u instances, scratch holds 256\n",
                        batches.InstanceCount);
        }

        while (ok && glfwWindowShouldClose(g_App.Window) == GLFW_FALSE && !g_App.ShouldClose)
        {
            glfwPollEvents();
            if (g_App.SwapchainDirty)
            {
                vkDeviceWaitIdle(g_App.Device);
                DestroySwapchain();
                int width = 0, height = 0;
                glfwGetFramebufferSize(g_App.Window, &width, &height);
                if (width > 0 && height > 0)
                {
                    ok = CreateSwapchain();
                    g_App.SwapchainDirty = !ok;
                }
                if (!ok)
                    break;
            }
            ok = RenderFrame();
        }
        if (ok)
        {
            // Give the screenshot its frame if it did not land yet (e.g. when
            // the swapchain was recreated).
            int guard = 0;
            while (g_App.ScreenshotPath != nullptr && !g_App.ShouldClose && guard++ < 8)
                ok = RenderFrame();
        }
        if (g_App.Device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(g_App.Device);
        ImSlugVulkanShutdown();
        ImSlugDestroyFont(g_App.Font);
        ImSlugDestroyPools(&g_App.Pools);

        if (g_App.Device != VK_NULL_HANDLE)
        {
            for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
            {
                if (g_App.ImageAvailable[i] != VK_NULL_HANDLE)
                    vkDestroySemaphore(g_App.Device, g_App.ImageAvailable[i], nullptr);
                if (g_App.RenderFinished[i] != VK_NULL_HANDLE)
                    vkDestroySemaphore(g_App.Device, g_App.RenderFinished[i], nullptr);
                if (g_App.InFlight[i] != VK_NULL_HANDLE)
                    vkDestroyFence(g_App.Device, g_App.InFlight[i], nullptr);
            }
            if (g_App.ScreenshotBuffer != VK_NULL_HANDLE)
            {
                vkUnmapMemory(g_App.Device, g_App.ScreenshotMemory);
                vkDestroyBuffer(g_App.Device, g_App.ScreenshotBuffer, nullptr);
                vkFreeMemory(g_App.Device, g_App.ScreenshotMemory, nullptr);
            }
            if (g_App.CommandPool != VK_NULL_HANDLE)
                vkDestroyCommandPool(g_App.Device, g_App.CommandPool, nullptr);
            DestroySwapchain();
            vkDestroyDevice(g_App.Device, nullptr);
        }
        if (g_App.DebugMessenger != VK_NULL_HANDLE)
        {
            PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger =
                (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_App.Instance,
                                                                           "vkDestroyDebugUtilsMessengerEXT");
            if (destroyMessenger != nullptr)
                destroyMessenger(g_App.Instance, g_App.DebugMessenger, nullptr);
        }
        if (g_App.Surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(g_App.Instance, g_App.Surface, nullptr);
        if (g_App.Instance != VK_NULL_HANDLE)
            vkDestroyInstance(g_App.Instance, nullptr);
        glfwDestroyWindow(g_App.Window);
        glfwTerminate();

        printf("[imslug] validation messages: %d (errors: %d)\n", g_App.ValidationMessages, g_App.ValidationErrors);
        return ok ? 0 : 1;
    }
}  // namespace

int main(int argc, char** argv)
{
    ParseArgs(argc, argv);
    return Run();
}
