// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// example/glfw_vulkan/main.cpp — GLFW + Vulkan desktop shell.
//
// An ordinary Dear ImGui application: the stock GLFW+Vulkan backends render
// everything, imslug renders the text of the registered fonts through Slug.
// The options/state, the scene switcher (demo, widgets/plots/tables, clipping
// torture, mixed fonts, runtime FontScaleDpi, the 5k-glyph and CJK stress
// scenes, color emoji, a pre-baked `.imslug` packed asset and the heavy
// demo/perf scene) and the per-frame ImGui build live in the shared
// platform-free example app (example/common/example_app.h/.cpp); this file is
// the desktop platform shell: the GLFW window, the Vulkan context, the
// screenshot CLI/readback, the vsync (present-mode) control, the GPU
// timestamp queries, the validation layers and the main loop. It implements
// the ExamplePlatform hooks the shared app calls into.
//
// Vulkan validation
// layers are enabled by default (set IMSLUG_VALIDATION=0 to disable); every
// message is printed and the error count is reported on exit.
//
// Usage:
//   imslug_example_glfw_vulkan [--font <path>] [--size <px>] [--scene <1-10>]
//                              [--renderer <slug|atlas>] [--novsync]
//                              [--dpi <scale>]
//                              [--overlay <none|bounds|grid|refs>]
//                              [--screenshot <file.ppm> [scene] [dpi_scale]]
// Keys:
//   1 .. 9   select scene (demo / widgets / clipping / mixed / dpi / 5k / cjk /
//            emoji / packed)
//   N        select the node editor scene (scene 10; pan/zoom text stress)
//   P        select the heavy demo / perf scene (scene 11)
//   M        text renderer: Slug outlines <-> ImGui dynamic fonts (atlas);
//            same toggle as the prominent top-center renderer bar (atlas mode
//            shows an explanation instead of the Slug-only samples)
//   + / -    base font size +-1 px (12 .. 48)
//   F1/F2/F3 debug overlay: glyph bounds / band grid / reference-walk heat
//   0        reset fonts/DPI to the defaults    Esc     quit
//
// Standalone: the default fonts are embedded in the binary (the packed
// Roboto-ASCII asset for Slug and the raw Roboto TTF for ImGui's stock atlas
// loader), so the executable runs with no data files next to it. Override the
// font with --font <ttf|imslug> or the IMSLUG_EXAMPLE_FONT environment
// variable (precedence: --font, then the environment, then the embedded
// defaults). The CJK/emoji scenes still use the fetched test fonts when
// present (they degrade with a message otherwise).
//
// The heavy scene's performance panel shows wall-clock frame time (plot; the
// vsync checkbox flips FIFO <-> MAILBOX/IMMEDIATE), whole-frame GPU time from
// timestamp queries when the queue supports them, and draw/Slug/atlas
// counters. Isolated GPU text timings: scripts/bench.* (--gpu).
//
// Third-party: Dear ImGui (MIT, pinned v1.92.9b) and GLFW (zlib license) are
// fetched by CMake; the example itself is imslug code. The embedded default
// fonts come from tests/fonts/Roboto-Regular.ttf (Apache-2.0) and its baked
// tests/assets/Roboto-ASCII.imslug asset; the CJK/emoji test fonts are fetched
// locally (IMSLUG_CJK_TEST_FONT / IMSLUG_EMOJI_TEST_FONT).

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include "example_app.h"
#include "imslug.h"
#include "imslug_impl_vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace ImSlug;

namespace
{
    const uint32_t kMaxFramesInFlight = 3;

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
        uint32_t FramesInFlight;  // backend ring size: min(swapchain images, 3)
        bool SwapchainDirty;

        // ImGui + imslug
        VkDescriptorPool ImGuiDescriptorPool;
        bool ImGuiInitialized;
        bool ImSlugBackendInitialized;

        // Shared-app options (filled by ParseArgs; the shared state is
        // installed by ExampleAppInit in main).
        ExampleAppOptions Options;

        // Present mode: true = vsync (FIFO, always supported), false = the
        // uncapped MAILBOX/IMMEDIATE fallback chain. Toggling recreates the
        // swapchain (see ToggleVsync / the performance panel).
        bool Vsync;
        VkPresentModeKHR PresentMode;
        VkPresentModeKHR LoggedPresentMode;  // only log when the mode changes

        // GPU timestamps: one start/end pair per frame slot; the pair is read
        // when the slot is reused (its fence has signaled by then). The counter
        // may be narrower than 64 bits (TimestampMask keeps a wrap harmless).
        bool TimestampSupported;
        float TimestampPeriodNs;
        uint64_t TimestampMask;
        VkQueryPool TimestampPool;
        bool TimestampRecorded[kMaxFramesInFlight];
        float GpuFrameMs[kMaxFramesInFlight];
        bool GpuFrameMsValid[kMaxFramesInFlight];

        // Screenshot mode.
        const char* ScreenshotPath;
        int ScreenshotFrame;
        bool ScreenshotRequested;
        bool ScreenshotWritten;
        VkBuffer ScreenshotBuffer;
        VkDeviceMemory ScreenshotMemory;
        void* ScreenshotMapped;
        VkDeviceSize ScreenshotBufferSize;
    };

    App g_App = {};
    ExamplePlatform g_Platform = {};

    // ---------------------------------------------------------------------
    // Tiny helpers
    // ---------------------------------------------------------------------
    // Sniffs the first bytes of a file for the `.imslug` container magic; the
    // stock ImGui loader cannot parse a packed asset, so a packed --font falls
    // back to the embedded TTF for the stock atlas font. Desktop-only: the
    // shared app receives the verdict as ExampleAppOptions::FontPathIsPacked.
    bool IsPackedAssetFile(const char* path)
    {
        FILE* file = fopen(path, "rb");
        if (file == nullptr)
            return false;
        char magic[8] = {};
        const size_t read = fread(magic, 1, sizeof(magic), file);
        fclose(file);
        return read == sizeof(magic) && memcmp(magic, "IMSLUG", 6) == 0;
    }

    bool Check(VkResult result, const char* what)
    {
        if (result != VK_SUCCESS)
        {
            fprintf(stderr, "[imslug] FATAL: %s failed (VkResult %d)\n", what, (int)result);
            return false;
        }
        return true;
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

        // Timestamp support for the performance panel's GPU numbers: the queue
        // must have valid bits, the device a sane timestampPeriod. The counter
        // width can be below 64; the mask keeps a wrap from inverting a pair.
        g_App.TimestampPeriodNs = properties.limits.timestampPeriod;
        {
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(g_App.PhysicalDevice, &familyCount, nullptr);
            VkQueueFamilyProperties* familyProperties =
                (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * (familyCount ? familyCount : 1));
            if (familyProperties != nullptr)
            {
                vkGetPhysicalDeviceQueueFamilyProperties(g_App.PhysicalDevice, &familyCount, familyProperties);
                const uint32_t validBits =
                    familyCount > g_App.QueueFamily ? familyProperties[g_App.QueueFamily].timestampValidBits : 0;
                g_App.TimestampSupported = validBits != 0;
                g_App.TimestampMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1);
                free(familyProperties);
            }
        }

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
    const char* PresentModeName(VkPresentModeKHR mode)
    {
        switch (mode)
        {
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return "MAILBOX (uncapped, no tearing)";
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return "IMMEDIATE (uncapped, tears)";
        default:
            return "FIFO (vsync)";
        }
    }

    // Vsync on -> FIFO (always supported). Vsync off -> MAILBOX when the
    // surface offers it (uncapped without tearing), else IMMEDIATE, else FIFO.
    VkPresentModeKHR ChoosePresentMode()
    {
        if (g_App.Vsync)
            return VK_PRESENT_MODE_FIFO_KHR;
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(g_App.PhysicalDevice, g_App.Surface, &count, nullptr);
        VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
        VkPresentModeKHR* modes = (VkPresentModeKHR*)malloc(sizeof(VkPresentModeKHR) * (count ? count : 1));
        if (modes != nullptr && count > 0)
        {
            vkGetPhysicalDeviceSurfacePresentModesKHR(g_App.PhysicalDevice, g_App.Surface, &count, modes);
            bool hasMailbox = false;
            bool hasImmediate = false;
            for (uint32_t i = 0; i < count; ++i)
            {
                hasMailbox = hasMailbox || modes[i] == VK_PRESENT_MODE_MAILBOX_KHR;
                hasImmediate = hasImmediate || modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
            if (hasMailbox)
                chosen = VK_PRESENT_MODE_MAILBOX_KHR;
            else if (hasImmediate)
                chosen = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        free(modes);
        return chosen;
    }

    void ToggleVsync()
    {
        g_App.Vsync = !g_App.Vsync;
        g_App.SwapchainDirty = true;  // recreated before the next frame
        printf("[imslug] vsync %s\n", g_App.Vsync ? "on (FIFO)" : "off (uncapped when supported)");
    }

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
        g_App.PresentMode = ChoosePresentMode();
        createInfo.presentMode = g_App.PresentMode;
        if (g_App.LoggedPresentMode != g_App.PresentMode)
        {
            printf("[imslug] present mode: %s\n", PresentModeName(g_App.PresentMode));
            g_App.LoggedPresentMode = g_App.PresentMode;
        }
        createInfo.clipped = VK_TRUE;
        if (!Check(vkCreateSwapchainKHR(g_App.Device, &createInfo, nullptr, &g_App.Swapchain), "vkCreateSwapchainKHR"))
            return false;

        vkGetSwapchainImagesKHR(g_App.Device, g_App.Swapchain, &g_App.SwapchainImageCount, nullptr);
        g_App.SwapchainImages = (VkImage*)malloc(sizeof(VkImage) * g_App.SwapchainImageCount);
        if (g_App.SwapchainImages == nullptr)
            return false;
        vkGetSwapchainImagesKHR(g_App.Device, g_App.Swapchain, &g_App.SwapchainImageCount, g_App.SwapchainImages);
        g_App.SwapchainExtent = extent;

        g_App.SwapchainViews = (VkImageView*)malloc(sizeof(VkImageView) * g_App.SwapchainImageCount);
        if (g_App.SwapchainViews == nullptr)
            return false;
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

        // The render pass outlives swapchain recreations (the ImGui/ImSlug
        // pipelines are created for it, so it must not be destroyed on resize).
        if (g_App.RenderPass == VK_NULL_HANDLE)
        {
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
            if (!Check(vkCreateRenderPass(g_App.Device, &renderPassInfo, nullptr, &g_App.RenderPass),
                       "vkCreateRenderPass"))
                return false;
        }

        g_App.Framebuffers = (VkFramebuffer*)malloc(sizeof(VkFramebuffer) * g_App.SwapchainImageCount);
        if (g_App.Framebuffers == nullptr)
            return false;
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
        // The render pass intentionally survives swapchain recreations (see
        // CreateSwapchain); it is destroyed once at shutdown.
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

    bool CreateTimestampPool()
    {
        if (!g_App.TimestampSupported)
        {
            printf("[imslug] GPU timestamps: unavailable (queue has no timestamp support)\n");
            return true;  // the performance panel reports n/a
        }
        VkQueryPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        poolInfo.queryCount = kMaxFramesInFlight * 2;
        return Check(vkCreateQueryPool(g_App.Device, &poolInfo, nullptr, &g_App.TimestampPool), "vkCreateQueryPool");
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
        if (row == nullptr)
        {
            fclose(file);
            return false;
        }
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
    // ExamplePlatform hooks (the shared app's desktop capabilities)
    // ---------------------------------------------------------------------
    // example_app.h's logical overlay flags mirror the module's mask; the
    // assertions keep a future backend module from silently diverging.
    static_assert(ExampleDebugOverlay_GlyphBounds == ImSlugVulkanDebugOverlay_GlyphBounds,
                  "overlay flag mapping drift (glyph bounds)");
    static_assert(ExampleDebugOverlay_BandGrid == ImSlugVulkanDebugOverlay_BandGrid,
                  "overlay flag mapping drift (band grid)");
    static_assert(ExampleDebugOverlay_RefHeat == ImSlugVulkanDebugOverlay_RefHeat,
                  "overlay flag mapping drift (ref heat)");

    uint32_t PlatformGetDebugOverlay(void* user)
    {
        (void)user;
        return ImSlug_ImplVulkan_GetDebugOverlay();
    }

    void PlatformSetDebugOverlay(void* user, uint32_t flags)
    {
        (void)user;
        ImSlug_ImplVulkan_SetDebugOverlay(flags);
    }

    const char* PlatformGetValidationState(void* user)
    {
        (void)user;
        return g_App.ValidationEnabled ? "ON" : "off";
    }

    bool PlatformGetGpuFrameMs(void* user, float* outMs, int* outSlots)
    {
        (void)user;
        if (!g_App.TimestampSupported)
            return false;
        float sum = 0.0f;
        int count = 0;
        for (uint32_t i = 0; i < g_App.FramesInFlight; ++i)
        {
            if (g_App.GpuFrameMsValid[i])
            {
                sum += g_App.GpuFrameMs[i];
                ++count;
            }
        }
        *outMs = count > 0 ? sum / (float)count : 0.0f;
        *outSlots = count;
        return true;
    }

    bool PlatformGetVsync(void* user)
    {
        (void)user;
        return g_App.Vsync;
    }

    void PlatformToggleVsync(void* user)
    {
        (void)user;
        ToggleVsync();
    }

    const char* PlatformGetPresentModeName(void* user)
    {
        (void)user;
        return PresentModeName(g_App.PresentMode);
    }

    ExamplePlatform MakePlatformHooks()
    {
        ExamplePlatform platform = {};
        platform.GetDebugOverlay = PlatformGetDebugOverlay;
        platform.SetDebugOverlay = PlatformSetDebugOverlay;
        platform.GetValidationState = PlatformGetValidationState;
        platform.GetGpuFrameMs = PlatformGetGpuFrameMs;
        platform.GetVsync = PlatformGetVsync;
        platform.ToggleVsync = PlatformToggleVsync;
        platform.GetPresentModeName = PlatformGetPresentModeName;
        platform.user = nullptr;
        return platform;
    }

    // ---------------------------------------------------------------------
    // Input
    // ---------------------------------------------------------------------
    // The shared app owns the actions; this maps GLFW keys onto them. The
    // scene (1-9/P), size (+/-) and renderer radio keys are polled by the
    // shared switcher itself.
    void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
    {
        (void)window;
        (void)scancode;
        (void)mods;
        if (action != GLFW_PRESS && action != GLFW_REPEAT)
            return;
        ExampleAppState& state = *ExampleAppGetState();
        if (key == GLFW_KEY_ESCAPE)
            state.ShouldClose = true;
        else if (key == GLFW_KEY_F1)
            ExampleAppToggleDebugOverlay(ExampleDebugOverlay_GlyphBounds);
        else if (key == GLFW_KEY_F2)
            ExampleAppToggleDebugOverlay(ExampleDebugOverlay_BandGrid);
        else if (key == GLFW_KEY_F3)
            ExampleAppToggleDebugOverlay(ExampleDebugOverlay_RefHeat);
        else if (key == GLFW_KEY_M)
            ExampleAppSetTextRenderer(!state.AtlasText);
        else if (key == GLFW_KEY_0)
            ExampleAppResetFonts();
    }

    // ---------------------------------------------------------------------
    // Frame loop
    // ---------------------------------------------------------------------
    bool RenderFrame()
    {
        App& app = g_App;
        ExampleAppState& state = *ExampleAppGetState();
        if (app.FramesInFlight == 0)
            return false;
        const uint32_t frame = app.FrameIndex % app.FramesInFlight;

        if (!Check(vkWaitForFences(app.Device, 1, &app.InFlight[frame], VK_TRUE, UINT64_MAX), "vkWaitForFences"))
            return false;

        // GPU timestamps: this slot's fence just signaled, so the pair recorded
        // for its previous use is complete and readable.
        if (app.TimestampPool != VK_NULL_HANDLE && app.TimestampRecorded[frame])
        {
            uint64_t values[2] = {};
            const VkResult result = vkGetQueryPoolResults(app.Device, app.TimestampPool, frame * 2, 2, sizeof(values),
                                                          values, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (result == VK_SUCCESS && values[1] >= values[0])
            {
                const uint64_t delta = (values[1] - values[0]) & app.TimestampMask;
                app.GpuFrameMs[frame] = (float)delta * app.TimestampPeriodNs / 1.0e6f;
                app.GpuFrameMsValid[frame] = true;
            }
            else
            {
                app.GpuFrameMsValid[frame] = false;  // never average a stale sample
            }
            app.TimestampRecorded[frame] = false;
        }

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

        // Whole-frame GPU bracket (dirty-row uploads + the render pass + the
        // screenshot copy); both writes are outside a render pass instance.
        if (app.TimestampPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(app.CommandBuffers[frame], app.TimestampPool, frame * 2, 2);
            vkCmdWriteTimestamp(app.CommandBuffers[frame], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, app.TimestampPool,
                                frame * 2);
            app.TimestampRecorded[frame] = true;
        }

        // ImGui frame: platform + renderer new-frame, build, then the pre-pass.
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ExampleAppBuildFrame();
        ImGui::Render();

        // Capture the draw-data shape for the performance panel (the live draw
        // data is cleared by the next NewFrame; the Slug pre-pass rewrites a
        // copy of this during RenderDrawData, so read it before that).
        ExampleAppCaptureDrawStats();

        // Glyph-pool uploads: dirty rows only; must stay outside the render
        // pass (transfer commands are illegal inside one).
        ImSlug_ImplVulkan_UploadPools(app.CommandBuffers[frame]);

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

        // Everything (Slug text through callbacks, shapes through the stock
        // path) goes through ImGui_ImplVulkan_RenderDrawData.
        ImSlug_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), app.CommandBuffers[frame]);

        vkCmdEndRenderPass(app.CommandBuffers[frame]);

        // Screenshot: copy the rendered swapchain image (still acquired) to the
        // readback buffer inside the same submission.
        app.ScreenshotRequested = false;
        if (app.ScreenshotPath != nullptr && state.FramesRendered >= app.ScreenshotFrame)
        {
            if (!EnsureScreenshotBuffer((VkDeviceSize)app.SwapchainExtent.width * app.SwapchainExtent.height * 4))
                return false;
            RecordScreenshotCopy(app.CommandBuffers[frame], imageIndex);
            app.ScreenshotRequested = true;
        }

        if (app.TimestampPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(app.CommandBuffers[frame], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, app.TimestampPool,
                                frame * 2 + 1);

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
            if (!WritePpm(app.ScreenshotPath, (const unsigned char*)app.ScreenshotMapped, app.SwapchainExtent.width,
                          app.SwapchainExtent.height, app.SwapchainFormat))
                return false;
            app.ScreenshotWritten = true;
            state.ShouldClose = true;
        }

        state.FramesRendered++;
        app.FrameIndex = (frame + 1) % app.FramesInFlight;
        return true;
    }

    // ---------------------------------------------------------------------
    // Main
    // ---------------------------------------------------------------------
    void ParseArgs(int argc, char** argv)
    {
        // Shared options (--font/--size/--renderer/--scene/--dpi/--overlay)
        // come from the common parser; --novsync/--screenshot/--help are the
        // desktop shell's own flags.
        ExampleAppOptionsDefaults(&g_App.Options);
        g_App.Vsync = true;
        g_App.LoggedPresentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;  // log the first pick
        g_App.ScreenshotPath = nullptr;
        g_App.ScreenshotFrame = 3;
        g_App.ScreenshotWritten = false;

        for (int i = 1; i < argc; ++i)
        {
            const int consumed = ExampleAppParseArg(argc, argv, i, &g_App.Options);
            if (consumed > 0)
            {
                i += consumed - 1;
                continue;
            }
            if (strcmp(argv[i], "--novsync") == 0)
                g_App.Vsync = false;
            else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            {
                g_App.ScreenshotPath = argv[++i];
                if (i + 1 < argc && argv[i + 1][0] != '-')
                    g_App.Options.SceneNumber = atoi(argv[++i]);
                if (i + 1 < argc && argv[i + 1][0] != '-')
                    g_App.Options.DpiScale = (float)atof(argv[++i]);
            }
            else if (strcmp(argv[i], "--help") == 0)
            {
                printf("usage: imslug_example_glfw_vulkan [--font <path>] [--size <px>] [--scene <1-%d>]\n"
                       "                                  [--renderer <slug|atlas>] [--novsync] [--dpi <scale>]\n"
                       "                                  [--overlay <none|bounds|grid|refs>]\n"
                       "                                  [--screenshot <file.ppm> [scene] [dpi_scale]]\n"
                       "font: --font <ttf|imslug> or IMSLUG_EXAMPLE_FONT; default: fonts embedded in this binary\n"
                       "keys: 1-9 + N/P scenes, M renderer (Slug/atlas), +/- base size, 0 reset; F1/F2/F3 overlays\n",
                       kExampleSceneCount);
                exit(0);
            }
            else
            {
                fprintf(stderr, "[imslug] unknown argument: %s\n", argv[i]);
                exit(1);
            }
        }
        // No --font: the IMSLUG_EXAMPLE_FONT environment variable overrides
        // the embedded defaults (precedence: CLI, environment, embedded). The
        // env var and the file sniff are desktop-only; the shared app sees
        // only the resolved path + packed verdict.
        if (g_App.Options.FontPath == nullptr)
        {
            const char* envFont = getenv("IMSLUG_EXAMPLE_FONT");
            if (envFont != nullptr && envFont[0] != '\0')
                g_App.Options.FontPath = envFont;
        }
        g_App.Options.FontPathIsPacked = g_App.Options.FontPath != nullptr && IsPackedAssetFile(g_App.Options.FontPath);
    }

    // Applies a pending swapchain recreation (resize / the vsync toggle).
    // Returns false when there is nothing to render into yet: the framebuffer
    // is 0x0 (minimized; the dirty flag stays set for a retry) or the
    // recreation failed (*ok is cleared). The caller must skip the frame.
    bool ServiceSwapchain(bool* ok)
    {
        if (!g_App.SwapchainDirty)
            return true;
        vkDeviceWaitIdle(g_App.Device);
        DestroySwapchain();
        int width = 0, height = 0;
        glfwGetFramebufferSize(g_App.Window, &width, &height);
        if (width > 0 && height > 0)
        {
            *ok = CreateSwapchain();
            g_App.SwapchainDirty = !*ok;
            if (*ok)
            {
                // Refresh the backend's image expectation and keep our frame
                // ring within the (possibly changed) image count.
                if (g_App.SwapchainImageCount < g_App.FramesInFlight)
                    g_App.FramesInFlight = g_App.SwapchainImageCount;
                ImGui_ImplVulkan_SetMinImageCount(g_App.SwapchainImageCount);
            }
        }
        return *ok && !g_App.SwapchainDirty;
    }

    int Run()
    {
        ExampleAppState& state = *ExampleAppGetState();
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
            glfwCreateWindow(kExampleWindowWidth, kExampleWindowHeight, kExampleWindowTitle, nullptr, nullptr);
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

        // imslug + ImGui: the documented order (imslug.h) is ImGui context ->
        // stock backend (publishes the platform_io hooks imslug needs, sets
        // RendererHasTextures) -> ImSlug::Init -> imslug backend module ->
        // AddFont*. The Init call therefore lives after ImGui_ImplVulkan_Init.
        if (ok)
        {
            VkDescriptorPoolSize poolSizes[1] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 } };
            VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            poolInfo.maxSets = 64;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = poolSizes;
            ok = Check(vkCreateDescriptorPool(g_App.Device, &poolInfo, nullptr, &g_App.ImGuiDescriptorPool),
                       "vkCreateDescriptorPool");
        }
        if (ok)
        {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;  // deterministic screenshots
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            ImGui::StyleColorsDark();
            ImGui::GetStyle().FontScaleDpi = state.DpiScale;

            if (!ImGui_ImplGlfw_InitForVulkan(g_App.Window, true))
            {
                fprintf(stderr, "[imslug] ImGui_ImplGlfw_InitForVulkan failed\n");
                ok = false;
            }

            ImGui_ImplVulkan_InitInfo initInfo = {};
            initInfo.ApiVersion = VK_API_VERSION_1_2;
            initInfo.Instance = g_App.Instance;
            initInfo.PhysicalDevice = g_App.PhysicalDevice;
            initInfo.Device = g_App.Device;
            initInfo.QueueFamily = g_App.QueueFamily;
            initInfo.Queue = g_App.Queue;
            initInfo.DescriptorPool = g_App.ImGuiDescriptorPool;
            initInfo.MinImageCount = 2;
            initInfo.ImageCount = g_App.SwapchainImageCount;
            initInfo.PipelineInfoMain.RenderPass = g_App.RenderPass;
            initInfo.PipelineInfoMain.Subpass = 0;
            initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            initInfo.UseDynamicRendering = false;
            initInfo.CheckVkResultFn = nullptr;
            if (ok && !ImGui_ImplVulkan_Init(&initInfo))
            {
                fprintf(stderr, "[imslug] ImGui_ImplVulkan_Init failed\n");
                ok = false;
            }
            if (ok)
                g_App.ImGuiInitialized = true;

            // ImSlug core after the stock backend (RendererHasTextures) and
            // before its own Vulkan module / any AddFont* call.
            if (ok && !ImSlug::Init())
            {
                fprintf(stderr, "[imslug] ImSlug::Init failed\n");
                ok = false;
            }

            if (ok && !ImSlug_ImplVulkan_Init(&initInfo))
            {
                fprintf(stderr, "[imslug] ImSlug_ImplVulkan_Init failed\n");
                ok = false;
            }
            if (ok)
            {
                g_App.ImSlugBackendInitialized = true;
                // Cycle over the backend's actual frames in flight
                // (min(images, 3)): a 2-image swapchain must not use slot 2.
                g_App.FramesInFlight = ImSlugVulkanGetFramesInFlight();
                if (g_App.FramesInFlight == 0 || g_App.FramesInFlight > kMaxFramesInFlight)
                    g_App.FramesInFlight = 1;
                state.FontsDirty = true;  // add the fonts (needs the module)
                // --overlay applied after the module exists; without the flag
                // the Config seed (DebugDrawBounds) stays in effect.
                if (g_App.Options.DebugOverlayGiven)
                    ImSlug_ImplVulkan_SetDebugOverlay(g_App.Options.DebugOverlay);
            }
        }

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
            if (g_App.CommandBuffers == nullptr)
            {
                fprintf(stderr, "[imslug] out of memory (command buffers)\n");
                ok = false;
            }
            else
            {
                VkCommandBufferAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                allocateInfo.commandPool = g_App.CommandPool;
                allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocateInfo.commandBufferCount = kMaxFramesInFlight;
                ok = Check(vkAllocateCommandBuffers(g_App.Device, &allocateInfo, g_App.CommandBuffers),
                           "vkAllocateCommandBuffers");
            }
        }
        if (ok)
            ok = CreateSyncObjects();
        if (ok)
            ok = CreateTimestampPool();

        while (ok && glfwWindowShouldClose(g_App.Window) == GLFW_FALSE && !state.ShouldClose)
        {
            glfwPollEvents();
            if (!ServiceSwapchain(&ok))
            {
                if (!ok)
                    break;
                glfwWaitEventsTimeout(0.1);  // minimized: wait for a restore/size event
                continue;
            }
            if (state.FontsDirty)
                ExampleAppRebuildFonts();
            ok = RenderFrame();
        }
        if (ok)
        {
            // Give the screenshot its frame if it did not land yet (e.g. when
            // the swapchain was recreated).
            int guard = 0;
            while (ok && g_App.ScreenshotPath != nullptr && !state.ShouldClose && guard++ < 8)
            {
                glfwPollEvents();
                if (!ServiceSwapchain(&ok))
                {
                    if (!ok)
                        break;
                    continue;
                }
                ok = RenderFrame();
            }
            if (ok && g_App.ScreenshotPath != nullptr && !g_App.ScreenshotWritten)
            {
                fprintf(stderr, "[imslug] screenshot frame did not land\n");
                ok = false;
            }
        }
        if (g_App.Device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(g_App.Device);
        if (g_App.ImSlugBackendInitialized)
            ImSlug_ImplVulkan_Shutdown();
        if (g_App.ImGuiInitialized)
        {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplGlfw_Shutdown();
        }
        ImSlug::Shutdown();  // core pools + glyph registry (after the atlas)
        if (ImGui::GetCurrentContext() != nullptr)
            ImGui::DestroyContext();
        if (g_App.ImGuiDescriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(g_App.Device, g_App.ImGuiDescriptorPool, nullptr);
            g_App.ImGuiDescriptorPool = VK_NULL_HANDLE;
        }

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
            free(g_App.CommandBuffers);
            g_App.CommandBuffers = nullptr;
            DestroySwapchain();
            if (g_App.TimestampPool != VK_NULL_HANDLE)
            {
                vkDestroyQueryPool(g_App.Device, g_App.TimestampPool, nullptr);
                g_App.TimestampPool = VK_NULL_HANDLE;
            }
            if (g_App.RenderPass != VK_NULL_HANDLE)
            {
                vkDestroyRenderPass(g_App.Device, g_App.RenderPass, nullptr);
                g_App.RenderPass = VK_NULL_HANDLE;
            }
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
    g_Platform = MakePlatformHooks();
    ExampleAppInit(&g_App.Options, &g_Platform);
    return Run();
}
