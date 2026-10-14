// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// example/glfw_vulkan/main.cpp — GLFW + Vulkan example.
//
// An ordinary Dear ImGui application: the stock GLFW+Vulkan backends render
// everything, imslug renders the text of the registered fonts through Slug.
// The scene switcher exercises the full feature set: demo, widgets/plots/
// tables, clipping torture, mixed fonts, runtime FontScaleDpi, the 5k-glyph
// and CJK stress scenes, color emoji, a pre-baked `.imslug` packed asset and a
// heavy demo scene with a performance panel. M swaps the body text between
// Slug and ImGui's dynamic font system (an A/B comparison of the same font).
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
//   P        select the heavy demo / perf scene (scene 10)
//   M        text renderer: Slug outlines <-> ImGui dynamic fonts (atlas)
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
#include "imgui_internal.h"  // ImTextCharToUtf8 (CJK stress scene)
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include "imslug.h"
#include "imslug_impl_vulkan.h"
#include "imslug_internal.h"

// The default fonts, embedded at build time (generated artifacts; regenerate
// with scripts/gen_asset.*): the packed Roboto-ASCII asset for Slug and the
// raw Roboto TTF for ImGui's own atlas loader (mixed-font scene / M toggle).
#include "imslug_example_asset.h"
#include "imslug_example_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace ImSlug;

namespace
{
    const uint32_t kMaxFramesInFlight = 3;
    const int kDefaultWindowWidth = 1280;
    const int kDefaultWindowHeight = 800;
    const int kDefaultSize = 17;
    const int kMinSize = 12;
    const int kMaxSize = 48;
    const int kSceneCount = 10;
    const int kPerfHistory = 240;  // frame-ms ring for the performance panel

    // Sniffs the first bytes of a file for the `.imslug` container magic; the
    // stock ImGui loader cannot parse a packed asset, so a packed --font falls
    // back to the embedded TTF for the stock atlas font.
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
        ImFont* SlugFont;    // rendered by Slug
        ImFont* StockFont;   // rendered through the atlas (mixed-font scene)
        ImFont* CjkFont;     // fetched CJK subset (stress scene; may stay null)
        ImFont* EmojiFont;   // fetched Noto Color Emoji (color scene; may stay null)
        ImFont* PackedFont;  // pre-baked .imslug asset (packed scene; may stay null)
        bool CjkAttempted;
        bool EmojiAttempted;
        bool PackedAttempted;
        bool PackedFromFile;  // packed scene: loaded the committed file, not the embedded copy
        float EmojiSize;      // emoji scene: emoji text size (12..96 px)
        float EmojiZoom;      // emoji scene: single-glyph zoom (32..220 px)
        bool ImGuiInitialized;
        bool ImSlugBackendInitialized;
        bool FontsDirty;  // re-add fonts at the current size next frame
        int BaseSize;
        int Scene;
        float DpiScale;
        bool ShowDemoStats;
        int SlugGlyphCount;
        int SlugRunCount;

        // Text renderer: false = Slug registered fonts, true = the stock
        // registration through ImGui's dynamic font system (--renderer, M).
        bool AtlasText;

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

        // Performance panel: wall-clock frame ms ring.
        float FrameMsHistory[kPerfHistory];
        int FrameMsCursor;
        int FrameMsCount;

        // Draw-data shape captured right after ImGui::Render() (the panel runs
        // while the live draw data is rebuilt, so it reads this copy).
        int LastCmdLists;
        int LastDrawCommands;
        int LastVtxCount;
        int LastIdxCount;

        // Heavy demo scene state (sliders; the text wall rebuilds on change).
        int PerfLines;
        int PerfChars;
        float PerfScale;

        // Debug overlay flags (applied after the backend module init; see
        // --overlay on the command line and the F1/F2/F3 toggles).
        uint32_t DebugOverlay;
        bool DebugOverlayGiven;  // --overlay was passed (else keep the config seed)

        const char* FontPath;

        // Screenshot mode.
        const char* ScreenshotPath;
        int ScreenshotFrame;
        int FramesRendered;
        bool ScreenshotRequested;
        bool ScreenshotWritten;
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
    // Scenes
    // ---------------------------------------------------------------------
    void SceneDemo()
    {
        if (g_App.ShowDemoStats)
            ImGui::ShowMetricsWindow(&g_App.ShowDemoStats);
        ImGui::ShowDemoWindow(nullptr);
    }

    void SceneWidgets()
    {
        ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("widgets, plots and tables");
        ImGui::Text("A normal ImGui window: labels, buttons, sliders, plots.");
        static float value = 0.5f;
        static bool flag = true;
        ImGui::SliderFloat("slider", &value, 0.0f, 1.0f);
        ImGui::Checkbox("checkbox", &flag);
        if (ImGui::Button("a button"))
            value = 0.25f;
        ImGui::SameLine();
        ImGui::Text("inline text after a button");
        static float history[120] = {};
        history[g_App.FramesRendered % 120] = value;
        ImGui::PlotLines("plot", history, 120, g_App.FramesRendered % 120, "value", 0.0f, 1.0f, ImVec2(0.0f, 80.0f));
        if (ImGui::BeginTable("table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            for (int row = 0; row < 12; ++row)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("row %d", row);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("Hamburgefonstiv %d", row * 7);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("U+%04X", 0x40 + row);
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    // Long text in differently sized children, tooltips and scrolling: every
    // CPU fine-clip path (four sides), window-edge clipping and ellipsized
    // labels in one scene.
    void SceneClipping()
    {
        ImGui::Begin("clipping torture");

        ImGui::Text("1) labels clipped by narrow cells (CPU fine clip on the right):");
        if (ImGui::BeginTable("cliptable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
        {
            for (int row = 0; row < 6; ++row)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("cell %d", row);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted("a very long label that must be elided ...");
            }
            ImGui::EndTable();
        }

        ImGui::Text("2) selectables with long labels (clip on all sides):");
        for (int row = 0; row < 4; ++row)
            ImGui::Selectable("a selectable with a ridiculously long caption that overflows", false, 0,
                              ImVec2(180.0f, 0.0f));

        ImGui::Text("3) child windows of shrinking size (window-edge + fine clip):");
        const float widths[4] = { 220.0f, 96.0f, 40.0f, 14.0f };
        for (int i = 0; i < 4; ++i)
        {
            ImGui::BeginChild(i == 0 ? "child-wide" : "child", ImVec2(widths[i], 34.0f), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Hamburgefonstiv in a small child window");
            ImGui::TextUnformatted("second line, scrolled out of view");
            ImGui::EndChild();
            ImGui::SameLine();
        }
        ImGui::NewLine();

        ImGui::Text("4) tooltips (hover the box):");
        ImGui::Button("hover me", ImVec2(120.0f, 0.0f));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("tooltip text rendered by Slug as well\nsecond tooltip line");

        ImGui::Text("5) text at the window edge (scrolled horizontally):");
        ImGui::BeginChild("h-scroll", ImVec2(0.0f, 60.0f), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted("left edge | a long horizontally scrolling line with lots of words | right edge");
        ImGui::TextUnformatted("Hamburgefonstiv 0123456789 repeated: abcdefghijklmnopqrstuvwxyz");
        ImGui::EndChild();

        ImGui::Text("6) a tiny child (a glyph squeezed on every side):");
        ImGui::BeginChild("tiny", ImVec2(11.0f, 7.0f), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted("Wg");
        ImGui::EndChild();

        ImGui::End();
    }

    void SceneMixedFonts()
    {
        ImGui::SetNextWindowSize(ImVec2(560.0f, 360.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("mixed fonts (Slug + atlas)");
        ImGui::TextWrapped("The line below alternates between the Slug font and a stock atlas font in one frame; both "
                           "paths must stay correct.");
        ImGui::Separator();
        for (int row = 0; row < 6; ++row)
        {
            if ((row % 2) == 0)
                ImGui::Text("Slug: Hamburgefonstiv 0123456789");
            else if (g_App.StockFont != nullptr)
            {
                ImGui::PushFont(g_App.StockFont, 0.0f);
                ImGui::Text("atlas: Hamburgefonstiv 0123456789");
                ImGui::PopFont();
            }
        }
        ImGui::Separator();
        ImGui::Text("Shapes, lines and images keep using the atlas:");
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(p, ImVec2(p.x + 180.0f, p.y + 24.0f), IM_COL32(70, 120, 200, 255), 4.0f);
        drawList->AddCircleFilled(ImVec2(p.x + 220.0f, p.y + 12.0f), 12.0f, IM_COL32(200, 140, 60, 255));
        drawList->AddText(ImVec2(p.x + 8.0f, p.y + 4.0f), IM_COL32(255, 255, 255, 255), "text over a shape");
        ImGui::Dummy(ImVec2(240.0f, 30.0f));
        ImGui::End();
    }

    void SceneDpi()
    {
        ImGui::SetNextWindowSize(ImVec2(560.0f, 400.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("runtime FontScaleDpi");
        ImGui::TextWrapped("Changing style.FontScaleDpi re-bakes the metrics at a new size while the Slug outlines and "
                           "bands are reused: the atlas must not grow (watch the texture size).");
        ImGui::SliderFloat("style.FontScaleDpi", &g_App.DpiScale, 0.5f, 3.0f, "%.2f");
        ImGui::GetStyle().FontScaleDpi = g_App.DpiScale;
        ImGui::SeparatorText("sample text");
        ImGui::Text("Hamburgefonstiv 0123456789");
        ImGui::TextWrapped("The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog.");
        ImGui::SeparatorText("atlas / backend state");
        const ImTextureData* tex = ImGui::GetIO().Fonts->TexData;
        Statistics stats;
        ImSlug::GetStatistics(&stats);
        ImGui::Text("atlas texture: %dx%d", tex != nullptr ? tex->Width : 0, tex != nullptr ? tex->Height : 0);
        ImGui::Text("pixels per glyph rect (live): %d", stats.GlyphSlabs);
        ImGui::Text("Slug instances last frame: %u", stats.Instances);
        ImGui::End();
    }

    // Stress scene: ~5k glyphs of long multi-line text plus a wrapped
    // paragraph, at the current base size (watch the stats: instances/runs).
    // The text is regenerated only when the layout inputs change.
    void SceneStress()
    {
        ImGui::SetNextWindowSize(ImVec2(1100.0f, 700.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("5k glyph stress");
        ImGui::TextWrapped("One window, thousands of glyphs: %d lines of long mixed text plus a wrapped paragraph. "
                           "The stats below come from ImSlug::GetStatistics(); dropped runs must stay 0.",
                           38);
        static char* text = nullptr;
        if (text == nullptr)
        {
            const char* pattern = "Hamburgefonstiv 0123456789 Grumpy wizards make toxic brew. ";
            const int patternLength = (int)strlen(pattern);
            const int lines = 38;
            const int columns = (int)(1300.0f / (float)g_App.BaseSize * 10.0f);  // ~fill the window width
            text = (char*)malloc((size_t)lines * (size_t)(columns + 2) + 2);
            if (text != nullptr)
            {
                int at = 0;
                for (int line = 0; line < lines; ++line)
                {
                    for (int column = 0; column < columns; ++column)
                        text[at++] = pattern[(line * columns + column) % patternLength];
                    if (line + 1 < lines)
                        text[at++] = '\n';
                }
                text[at] = '\0';
            }
        }
        if (text != nullptr)
            ImGui::TextUnformatted(text);
        ImGui::End();
    }

    // CJK scene: the fetched Noto Sans SC subset (scripts/fetch_cjk_font.*)
    // as a second Slug font. Skipped with a message when the font is absent.
    void SceneCjk()
    {
        ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("CJK (fetched subset)");
        if (g_App.CjkFont == nullptr && !g_App.CjkAttempted)
        {
            g_App.CjkAttempted = true;
            FILE* cjkFile = fopen(IMSLUG_CJK_TEST_FONT, "rb");
            if (cjkFile != nullptr)
            {
                fclose(cjkFile);
                g_App.CjkFont = ImSlug::AddFontFromFileTTF(IMSLUG_CJK_TEST_FONT, 22.0f);
            }
        }
        if (g_App.CjkFont == nullptr)
        {
            ImGui::TextWrapped("The CJK test font is not fetched. Run scripts/fetch_cjk_font.* (see "
                               "tests/fonts/cjk/PROVENANCE.txt), then restart or reselect this scene.");
        }
        else
        {
            ImGui::PushFont(g_App.CjkFont, 22.0f);
            ImGui::TextWrapped("3600 Hanzi (U+4E00..U+4EFF, 120 lines x 30) through the fetched subset font; the "
                               "worst glyph in the range has 146 quadratic curves.");
            static char* cjkText = nullptr;
            if (cjkText == nullptr)
            {
                const int lines = 60;
                const int columns = 34;
                cjkText = (char*)malloc((size_t)lines * (size_t)columns * 4 + 8);
                if (cjkText != nullptr)
                {
                    int at = 0;
                    int index = 0;
                    for (int line = 0; line < lines; ++line)
                    {
                        for (int column = 0; column < columns; ++column)
                        {
                            const unsigned int codepoint = 0x4E00u + (unsigned int)(index++ % 0x100);
                            at += ImTextCharToUtf8(cjkText + at, codepoint);
                        }
                        if (line + 1 < lines)
                            cjkText[at++] = '\n';
                    }
                    cjkText[at] = '\0';
                }
            }
            if (cjkText != nullptr)
                ImGui::TextUnformatted(cjkText);
            ImGui::PopFont();
        }
        ImGui::End();
    }

    // Emoji scene: the fetched Noto Color Emoji (COLR v1; scripts/fetch_emoji_font.*).
    // The example's ImGui build uses the default 16-bit ImWchar, so the scene
    // sticks to BMP emoji (U+2600..U+2BFF); non-BMP codepoints (U+1F600+)
    // require IMGUI_USE_WCHAR32 in the whole app build. The scene shows the
    // layers at several sizes plus one glyph zoomed up to 220 px so the v1
    // gradients (radial/linear paints) are visible at arbitrary zoom.
    void SceneEmoji()
    {
        ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("color emoji (COLR v1)");
        if (g_App.EmojiFont == nullptr && !g_App.EmojiAttempted)
        {
            g_App.EmojiAttempted = true;
            FILE* emojiFile = fopen(IMSLUG_EMOJI_TEST_FONT, "rb");
            if (emojiFile != nullptr)
            {
                fclose(emojiFile);
                g_App.EmojiFont = ImSlug::AddFontFromFileTTF(IMSLUG_EMOJI_TEST_FONT, 24.0f);
            }
        }
        if (g_App.EmojiFont == nullptr)
        {
            ImGui::TextWrapped("The color-emoji test font is not fetched. Run scripts/fetch_emoji_font.* (see "
                               "tests/fonts/emoji/PROVENANCE.txt), then restart or reselect this scene.");
        }
        else
        {
            ImGui::TextWrapped("Noto Color Emoji: one Slug instance per COLR layer, flat v0/v1 solids and v1 gradients "
                               "(radial/linear) evaluated per fragment. Tint only fades the layer colors.");
            ImGui::SliderFloat("emoji size", &g_App.EmojiSize, 12.0f, 96.0f, "%.0f px");
            static const char* kEmoji = "\xE2\x98\x80 \xE2\x9B\x85 \xE2\x9D\xA4 \xE2\xAD\x90 \xE2\x9A\xA1 \xE2\x9C\xA8 "
                                        "\xE2\x9B\x84 \xE2\x98\x94 \xE2\x9A\xBD \xE2\x9C\x82 \xE2\x99\xBB \xE2\x9A\x93";
            ImGui::PushFont(g_App.EmojiFont, g_App.EmojiSize);
            ImGui::TextUnformatted(kEmoji);
            ImGui::PopFont();

            ImGui::Separator();
            ImGui::Text("zoom (gradients stay exact):");
            const float zoom = g_App.EmojiZoom;
            ImGui::SliderFloat("zoom size", &g_App.EmojiZoom, 32.0f, 220.0f, "%.0f px");
            ImGui::PushFont(g_App.EmojiFont, zoom);
            ImGui::TextUnformatted("\xE2\x9D\xA4");
            ImGui::PopFont();

            ImGui::Separator();
            Statistics stats;
            ImSlug::GetStatistics(&stats);
            ImGui::Text("this frame: %u instances (%u color layers), %u runs", stats.Instances, stats.ColorLayers,
                        stats.LiveBatches);
        }
        ImGui::End();
    }

    // Packed-asset scene: text rendered from a pre-baked `.imslug` container
    // (glyph records + curve/band texels; no outline parsing). The asset is the
    // committed test asset (tests/assets/Roboto-ASCII.imslug, also embedded in
    // this binary); the scene loads it as a second Slug font, so the shared
    // pools and the auto-detecting loader are both exercised. With no checkout
    // next to the executable the embedded copy stands in. In a no-parser build
    // this is also the default font source.
    void ScenePacked()
    {
        ImGui::SetNextWindowSize(ImVec2(900.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("packed asset (.imslug)");
        if (g_App.PackedFont == nullptr && !g_App.PackedAttempted)
        {
            g_App.PackedAttempted = true;
            FILE* assetFile = fopen(IMSLUG_PACKED_TEST_ASSET, "rb");
            if (assetFile != nullptr)
            {
                fclose(assetFile);
                // The same entry point as a TTF: the loader auto-detects the
                // container by content.
                g_App.PackedFont = ImSlug::AddFontFromFileTTF(IMSLUG_PACKED_TEST_ASSET, 20.0f);
                g_App.PackedFromFile = g_App.PackedFont != nullptr;
            }
            else
            {
                // Standalone run (no repo checkout next to the executable):
                // fall back to the embedded copy of the same committed asset.
                ImFontConfig packedConfig;
                packedConfig.FontDataOwnedByAtlas = false;
                g_App.PackedFont = ImSlug::AddFontFromMemoryTTF(imslug_example_asset_data,
                                                                (int)imslug_example_asset_size, 20.0f, &packedConfig);
            }
        }
        if (g_App.PackedFont == nullptr)
        {
            ImGui::TextWrapped("The packed test asset is missing (%s). Generate it with scripts/gen_asset.* "
                               "(see tests/assets/PROVENANCE.txt), then restart or reselect this scene.",
                               IMSLUG_PACKED_TEST_ASSET);
        }
        else
        {
            ImGui::TextWrapped("This text is rendered from %s - the committed Roboto-ASCII.imslug packed asset "
                               "(.imslug container v1): glyph records, curve texels and band texels copied into "
                               "the shared pools without any outline parsing. The loader auto-detects the format "
                               "by content, so no app code changes between TTF and packed fonts.",
                               g_App.PackedFromFile ? "tests/assets/Roboto-ASCII.imslug"
                                                    : "the asset copy embedded in this binary");
            ImGui::Separator();
            ImGui::PushFont(g_App.PackedFont, 20.0f);
            ImGui::TextUnformatted("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
            ImGui::TextUnformatted("abcdefghijklmnopqrstuvwxyz");
            ImGui::TextUnformatted("0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~");
            ImGui::TextUnformatted("The quick brown fox jumps over the lazy dog.");
            ImGui::PopFont();
            ImGui::Separator();
            Statistics stats;
            ImSlug::GetStatistics(&stats);
            ImGui::Text("this frame: %u instances, %u runs, %u live slabs", stats.Instances, stats.LiveBatches,
                        stats.GlyphSlabs);
        }
        ImGui::End();
    }

    // Heavy scene (key P): a text-heavy workload plus the performance panel.
    // M swaps the body text between Slug outlines and ImGui's dynamic font
    // system; the panel shows wall-clock frame time (plot), whole-frame GPU
    // time from timestamp queries, draw/Slug/atlas counters and the vsync
    // toggle. Isolated GPU text timings: scripts/bench.* (imslug_tests_bench
    // --gpu); see docs/DESIGN.md section 4.
    void ScenePerf()
    {
        ImGuiIO& io = ImGui::GetIO();

        ImGui::SetNextWindowPos(ImVec2(268.0f, 8.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("performance");
        ImGui::Text("text renderer: %s (M toggles)", g_App.AtlasText ? "ImGui dynamic fonts" : "Slug outlines");
        const float frameMs = io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f;
        float frameAvg = 0.0f;
        for (int i = 0; i < g_App.FrameMsCount; ++i)
            frameAvg += g_App.FrameMsHistory[i];
        if (g_App.FrameMsCount > 0)
            frameAvg /= (float)g_App.FrameMsCount;
        ImGui::Text("frame: %.1f FPS, %.2f ms (window avg %.2f ms)", io.Framerate, frameMs, frameAvg);
        ImGui::TextUnformatted("frame ms (wall clock):");
        ImGui::PlotLines("##frame_ms", g_App.FrameMsHistory, g_App.FrameMsCount,
                         g_App.FrameMsCount < kPerfHistory ? 0 : g_App.FrameMsCursor, nullptr, 0.0f,
                         frameAvg * 2.0f > 1.0f ? frameAvg * 2.0f : 1.0f, ImVec2(410.0f, 64.0f));
        if (g_App.TimestampSupported)
        {
            float gpuSum = 0.0f;
            int gpuCount = 0;
            for (uint32_t i = 0; i < g_App.FramesInFlight; ++i)
            {
                if (g_App.GpuFrameMsValid[i])
                {
                    gpuSum += g_App.GpuFrameMs[i];
                    ++gpuCount;
                }
            }
            if (gpuCount > 0)
                ImGui::Text("GPU frame (timestamps): %.2f ms over %d slots", gpuSum / (float)gpuCount, gpuCount);
            else
                ImGui::TextUnformatted("GPU frame (timestamps): warming up");
        }
        else
            ImGui::TextUnformatted("GPU frame: n/a (queue without timestamps)");
        bool vsync = g_App.Vsync;
        if (ImGui::Checkbox("vsync", &vsync))
            ToggleVsync();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", PresentModeName(g_App.PresentMode));

        ImGui::SeparatorText("draw stats (last frame)");
        ImGui::Text("windows %d, cmd lists %d, draw cmds %d", io.MetricsRenderWindows, g_App.LastCmdLists,
                    g_App.LastDrawCommands);
        ImGui::Text("verts %d, indices %d", g_App.LastVtxCount, g_App.LastIdxCount);
        Statistics stats;
        ImSlug::GetStatistics(&stats);
        ImGui::Text("Slug: %u instances (%u color layers), %u runs, %u slabs", stats.Instances, stats.ColorLayers,
                    stats.LiveBatches, stats.GlyphSlabs);
        if (stats.DroppedRuns != 0)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "dropped runs: %u", stats.DroppedRuns);
        const ImTextureData* tex = io.Fonts->TexData;
        ImGui::Text("atlas: %dx%d, %.2f MB, %d fonts", tex != nullptr ? tex->Width : 0,
                    tex != nullptr ? tex->Height : 0,
                    tex != nullptr ? (double)tex->GetSizeInBytes() / (1024.0 * 1024.0) : 0.0, io.Fonts->Fonts.Size);
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(706.0f, 8.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(550.0f, 720.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("heavy demo (Slug vs atlas)");
        ImGui::TextWrapped("A text-heavy frame to compare the two render paths. Press M (or use the switcher "
                           "radios) to swap the body text between Slug outlines and ImGui's dynamic font system, "
                           "and watch the performance panel; untick vsync for uncapped numbers.");
        ImGui::SliderInt("lines", &g_App.PerfLines, 8, 80);
        ImGui::SliderInt("chars/line", &g_App.PerfChars, 64, 512);
        ImGui::SliderFloat("large text", &g_App.PerfScale, 1.0f, 6.0f, "%.1fx");
        ImGui::Text("%d lines x %d chars = %.0f glyphs laid out per frame", g_App.PerfLines, g_App.PerfChars,
                    (double)g_App.PerfLines * (double)g_App.PerfChars);

        if (g_App.SlugFont != nullptr && g_App.StockFont != nullptr)
        {
            ImGui::SeparatorText("same text, both paths (2x size)");
            ImGui::PushFont(g_App.SlugFont, (float)g_App.BaseSize * 2.0f);
            ImGui::TextUnformatted("Slug: Hamburgefonstiv 0123456789");
            ImGui::PopFont();
            ImGui::PushFont(g_App.StockFont, (float)g_App.BaseSize * 2.0f);
            ImGui::TextUnformatted("atlas: Hamburgefonstiv 0123456789");
            ImGui::PopFont();
        }

        ImGui::SeparatorText("text load");
        ImFont* largeFont = g_App.AtlasText ? g_App.StockFont : g_App.SlugFont;
        if (largeFont != nullptr)
        {
            ImGui::PushFont(largeFont, (float)g_App.BaseSize * g_App.PerfScale);
            ImGui::TextWrapped("The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor "
                               "jugs. Waltz, bad nymph, for quick jigs vex. Sphinx of black quartz, judge my vow.");
            ImGui::PopFont();
        }

        // Text wall, regenerated only when the inputs (or the base size) change.
        static char* wall = nullptr;
        static int wallLines = -1;
        static int wallChars = -1;
        static int wallSize = -1;
        if (wallLines != g_App.PerfLines || wallChars != g_App.PerfChars || wallSize != g_App.BaseSize)
        {
            free(wall);
            wall = nullptr;
            const int lines = g_App.PerfLines;
            const int chars = g_App.PerfChars;
            char* next = (char*)malloc((size_t)lines * ((size_t)chars + 1) + 1);
            if (next != nullptr)
            {
                const char* pattern = "Hamburgefonstiv 0123456789 Grumpy wizards make toxic brew. ";
                const int patternLength = (int)strlen(pattern);
                size_t at = 0;
                for (int line = 0; line < lines; ++line)
                {
                    for (int column = 0; column < chars; ++column)
                        next[at++] = pattern[(line * chars + column) % patternLength];
                    next[at++] = '\n';
                }
                next[at] = '\0';
                wall = next;
                wallLines = lines;
                wallChars = chars;
                wallSize = g_App.BaseSize;
            }
        }
        if (wall != nullptr)
            ImGui::TextUnformatted(wall);
        ImGui::End();
    }

    // Text renderer toggle (M / the switcher radios / --renderer). Both font
    // registrations coexist: SlugFont renders from the outlines through imslug,
    // StockFont through ImGui's dynamic font system. Only the default font
    // swaps; scenes that push a dedicated font (CJK/emoji/packed) keep it.
    void SetTextRenderer(bool atlas)
    {
        if (g_App.AtlasText == atlas)
            return;
        if (atlas && g_App.StockFont == nullptr)
        {
            fprintf(stderr, "[imslug] text renderer: atlas unavailable (the stock font registration failed)\n");
            return;
        }
        g_App.AtlasText = atlas;
        ImFont* font = atlas ? g_App.StockFont : g_App.SlugFont;
        if (font != nullptr && ImGui::GetCurrentContext() != nullptr)
            ImGui::GetIO().FontDefault = font;
        printf("[imslug] text renderer: %s\n", atlas ? "ImGui dynamic fonts (atlas)" : "Slug outlines");
    }

    // Debug overlay toggle (runtime; see ImSlug_ImplVulkan_SetDebugOverlay).
    // Returns the new state so a caller can react (keys and checkboxes share it).
    bool ToggleDebugOverlay(uint32_t flag)
    {
        uint32_t overlay = ImSlug_ImplVulkan_GetDebugOverlay();
        overlay ^= flag;
        ImSlug_ImplVulkan_SetDebugOverlay(overlay);
        printf("[imslug] debug overlay: 0x%X%s%s%s\n", overlay,
               (overlay & ImSlugVulkanDebugOverlay_GlyphBounds) ? " bounds" : "",
               (overlay & ImSlugVulkanDebugOverlay_BandGrid) ? " band-grid" : "",
               (overlay & ImSlugVulkanDebugOverlay_RefHeat) ? " ref-heat" : "");
        return (overlay & flag) != 0;
    }

    void DrawSceneSwitcher()
    {
        const char* sceneNames[kSceneCount] = {
            "demo window",          "widgets / plots / tables", "clipping torture",
            "mixed fonts",          "runtime FontScaleDpi",     "5k glyph stress",
            "CJK (fetched subset)", "color emoji (COLR v1)",    "packed asset (.imslug)",
            "heavy demo / perf (P)"
        };
        ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(240.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("imslug example " IMSLUG_VERSION, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextUnformatted("Scene (keys 1-9, P):");
        for (int i = 0; i < kSceneCount; ++i)
        {
            const bool shortcut =
                i < 9 ? ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + i)) : ImGui::IsKeyPressed(ImGuiKey_P);
            if (ImGui::RadioButton(sceneNames[i], g_App.Scene == i) || shortcut)
                g_App.Scene = i;
        }
        ImGui::Separator();
        ImGui::TextUnformatted("text renderer (M):");
        if (ImGui::RadioButton("Slug", !g_App.AtlasText))
            SetTextRenderer(false);
        ImGui::SameLine();
        if (ImGui::RadioButton("ImGui atlas", g_App.AtlasText))
            SetTextRenderer(true);
        ImGui::Text("font: %s, base size %d px", g_App.AtlasText ? "atlas" : "Slug", g_App.BaseSize);
        if (ImGui::Button("+") || ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
            g_App.FontsDirty = true, g_App.BaseSize = g_App.BaseSize < kMaxSize ? g_App.BaseSize + 1 : kMaxSize;
        ImGui::SameLine();
        if (ImGui::Button("-") || ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
            g_App.FontsDirty = true, g_App.BaseSize = g_App.BaseSize > kMinSize ? g_App.BaseSize - 1 : kMinSize;
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
        ImGui::Separator();
        Statistics stats;
        ImSlug::GetStatistics(&stats);
        ImGui::Text("Slug: %u instances, %u runs\n(%u slabs, %u dropped)", stats.Instances, stats.LiveBatches,
                    stats.GlyphSlabs, stats.DroppedRuns);
        ImGui::Text("validation: %s", g_App.ValidationEnabled ? "ON" : "off");
        ImGui::Separator();
        ImGui::Text("debug overlay (F1/F2/F3):");
        {
            const uint32_t overlay = ImSlug_ImplVulkan_GetDebugOverlay();
            bool bounds = (overlay & ImSlugVulkanDebugOverlay_GlyphBounds) != 0;
            bool bands = (overlay & ImSlugVulkanDebugOverlay_BandGrid) != 0;
            bool refs = (overlay & ImSlugVulkanDebugOverlay_RefHeat) != 0;
            if (ImGui::Checkbox("glyph bounds", &bounds))
                ToggleDebugOverlay(ImSlugVulkanDebugOverlay_GlyphBounds);
            if (ImGui::Checkbox("band grid", &bands))
                ToggleDebugOverlay(ImSlugVulkanDebugOverlay_BandGrid);
            if (ImGui::Checkbox("ref heat", &refs))
                ToggleDebugOverlay(ImSlugVulkanDebugOverlay_RefHeat);
        }
        ImGui::End();
    }

    void BuildImGuiFrame()
    {
        ImGui::NewFrame();
        // Performance panel history: wall-clock frame ms (io.DeltaTime is the
        // time NewFrame measured for the previous frame).
        g_App.FrameMsHistory[g_App.FrameMsCursor] = ImGui::GetIO().DeltaTime * 1000.0f;
        g_App.FrameMsCursor = (g_App.FrameMsCursor + 1) % kPerfHistory;
        if (g_App.FrameMsCount < kPerfHistory)
            ++g_App.FrameMsCount;
        DrawSceneSwitcher();
        switch (g_App.Scene)
        {
        case 0:
            SceneDemo();
            break;
        case 1:
            SceneWidgets();
            break;
        case 2:
            SceneClipping();
            break;
        case 3:
            SceneMixedFonts();
            break;
        case 4:
            SceneDpi();
            break;
        case 5:
            SceneStress();
            break;
        case 6:
            SceneCjk();
            break;
        case 7:
            SceneEmoji();
            break;
        case 8:
            ScenePacked();
            break;
        default:  // 9: heavy demo / perf
            ScenePerf();
            break;
        }
    }

    // Re-adds the Slug fonts at the current base size. The previous fonts are
    // removed first: without that every size step would append another ImFont +
    // bake set to the atlas (glyph slabs pile up until the cap) and the atlas
    // grows. Removal discards the bakes, which releases the slabs through
    // FontBakedDestroy; the outlines and bands in the pools stay valid and are
    // reused across sizes (no atlas growth for Slug glyphs).
    void RebuildFonts()
    {
        ImGuiIO& io = ImGui::GetIO();
        if (g_App.SlugFont != nullptr)
        {
            io.Fonts->RemoveFont(g_App.SlugFont);
            g_App.SlugFont = nullptr;
        }
        if (g_App.StockFont != nullptr)
        {
            io.Fonts->RemoveFont(g_App.StockFont);
            g_App.StockFont = nullptr;
        }
        const char* fontPath = g_App.FontPath;  // --font / IMSLUG_EXAMPLE_FONT; null = embedded defaults
        if (fontPath != nullptr)
        {
            g_App.SlugFont = ImSlug::AddFontFromFileTTF(fontPath, (float)g_App.BaseSize);
        }
        else
        {
            // The embedded default: the committed packed Roboto-ASCII asset,
            // loaded straight from the binary (no outline parsing - the same
            // path a no-parser build ships). Static data: never transfer
            // ownership to the atlas.
            ImFontConfig slugConfig;
            slugConfig.FontDataOwnedByAtlas = false;
            g_App.SlugFont = ImSlug::AddFontFromMemoryTTF(imslug_example_asset_data, (int)imslug_example_asset_size,
                                                          (float)g_App.BaseSize, &slugConfig);
        }
        if (g_App.SlugFont == nullptr)
        {
            fprintf(stderr, "[imslug] cannot add Slug font: %s\n",
                    fontPath != nullptr ? fontPath : "(embedded packed asset)");
            g_App.ShouldClose = true;
            return;
        }
        // A stock font for the mixed-font scene and the M toggle: through
        // ImGui's own stb loader (Oversample 1 keeps the metrics identical).
        // ImGui cannot parse a packed `.imslug` asset, so the stock font uses
        // the file only when --font named a TTF; otherwise it comes from the
        // embedded Roboto TTF copy (in a no-parser build too - ImGui's own
        // parser is independent of imslug's).
        ImFontConfig stockConfig;
        stockConfig.OversampleH = 1;
        stockConfig.OversampleV = 1;
        if (fontPath != nullptr && !IsPackedAssetFile(fontPath))
        {
            // File source: ImGui loads (and, with the default ownership, frees)
            // the buffer - do not override FontDataOwnedByAtlas here or every
            // RebuildFonts() would leak the font file.
            g_App.StockFont = io.Fonts->AddFontFromFileTTF(fontPath, (float)g_App.BaseSize, &stockConfig);
        }
        else
        {
            // Embedded static data: never transfer ownership to the atlas.
            stockConfig.FontDataOwnedByAtlas = false;
            g_App.StockFont = io.Fonts->AddFontFromMemoryTTF(
                (void*)imslug_example_font_data, (int)imslug_example_font_size, (float)g_App.BaseSize, &stockConfig);
        }
        io.FontDefault = g_App.AtlasText ? g_App.StockFont : g_App.SlugFont;
        g_App.FontsDirty = false;
        printf("[imslug] fonts: Slug + atlas at %d px (font: %s)\n", g_App.BaseSize,
               fontPath != nullptr ? fontPath : "embedded Roboto (packed asset, TTF for the atlas)");
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
        else if (key == GLFW_KEY_F1)
            ToggleDebugOverlay(ImSlugVulkanDebugOverlay_GlyphBounds);
        else if (key == GLFW_KEY_F2)
            ToggleDebugOverlay(ImSlugVulkanDebugOverlay_BandGrid);
        else if (key == GLFW_KEY_F3)
            ToggleDebugOverlay(ImSlugVulkanDebugOverlay_RefHeat);
        else if (key == GLFW_KEY_M)
            SetTextRenderer(!g_App.AtlasText);
        else if (key == GLFW_KEY_0)
        {
            g_App.BaseSize = kDefaultSize;
            g_App.DpiScale = 1.0f;
            g_App.FontsDirty = true;
            printf("[imslug] fonts reset to %d px, DPI 1.00\n", g_App.BaseSize);
        }
    }

    // ---------------------------------------------------------------------
    // Frame loop
    // ---------------------------------------------------------------------
    bool RenderFrame()
    {
        App& app = g_App;
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
        BuildImGuiFrame();
        ImGui::Render();

        // Capture the draw-data shape for the performance panel (the live draw
        // data is cleared by the next NewFrame; the Slug pre-pass rewrites a
        // copy of this during RenderDrawData, so read it before that).
        {
            const ImDrawData* drawData = ImGui::GetDrawData();
            app.LastCmdLists = drawData->CmdListsCount;
            app.LastVtxCount = drawData->TotalVtxCount;
            app.LastIdxCount = drawData->TotalIdxCount;
            app.LastDrawCommands = 0;
            for (int i = 0; i < drawData->CmdListsCount; ++i)
                app.LastDrawCommands += drawData->CmdLists[i]->CmdBuffer.Size;
        }

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
        if (app.ScreenshotPath != nullptr && app.FramesRendered >= app.ScreenshotFrame)
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
            app.ShouldClose = true;
        }

        app.FramesRendered++;
        app.FrameIndex = (frame + 1) % app.FramesInFlight;
        return true;
    }

    // ---------------------------------------------------------------------
    // Main
    // ---------------------------------------------------------------------
    void ParseArgs(int argc, char** argv)
    {
        g_App.FontPath = nullptr;
        g_App.BaseSize = kDefaultSize;
        g_App.Scene = 0;
        g_App.EmojiSize = 24.0f;
        g_App.EmojiZoom = 120.0f;
        int sceneNumber = 0;  // 1-based scene number on the CLI (0 = not given)
        g_App.DpiScale = 1.0f;
        g_App.ShowDemoStats = false;
        g_App.ScreenshotPath = nullptr;
        g_App.ScreenshotFrame = 3;
        g_App.FramesRendered = 0;
        g_App.DebugOverlay = 0;
        g_App.DebugOverlayGiven = false;
        g_App.AtlasText = false;
        g_App.Vsync = true;
        g_App.LoggedPresentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;  // log the first pick
        g_App.ScreenshotWritten = false;
        g_App.PerfLines = 40;
        g_App.PerfChars = 200;
        g_App.PerfScale = 3.0f;
        for (int i = 1; i < argc; ++i)
        {
            if (strcmp(argv[i], "--font") == 0 && i + 1 < argc)
                g_App.FontPath = argv[++i];
            else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
                g_App.BaseSize = atoi(argv[++i]);
            else if (strcmp(argv[i], "--renderer") == 0 && i + 1 < argc)
            {
                const char* value = argv[++i];
                if (strcmp(value, "slug") == 0)
                    g_App.AtlasText = false;
                else if (strcmp(value, "atlas") == 0)
                    g_App.AtlasText = true;
                else
                {
                    fprintf(stderr, "[imslug] --renderer expects 'slug' or 'atlas' (got '%s')\n", value);
                    exit(1);
                }
            }
            else if (strcmp(argv[i], "--novsync") == 0)
                g_App.Vsync = false;
            else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc)
                sceneNumber = atoi(argv[++i]);
            else if (strcmp(argv[i], "--dpi") == 0 && i + 1 < argc)
                g_App.DpiScale = (float)atof(argv[++i]);
            else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            {
                g_App.ScreenshotPath = argv[++i];
                if (i + 1 < argc && argv[i + 1][0] != '-')
                    sceneNumber = atoi(argv[++i]);
                if (i + 1 < argc && argv[i + 1][0] != '-')
                    g_App.DpiScale = (float)atof(argv[++i]);
            }
            else if (strcmp(argv[i], "--overlay") == 0 && i + 1 < argc)
            {
                const char* spec = argv[++i];
                uint32_t flags = 0;
                if (strstr(spec, "bounds") != nullptr)
                    flags |= ImSlugVulkanDebugOverlay_GlyphBounds;
                if (strstr(spec, "grid") != nullptr)
                    flags |= ImSlugVulkanDebugOverlay_BandGrid;
                if (strstr(spec, "refs") != nullptr)
                    flags |= ImSlugVulkanDebugOverlay_RefHeat;
                if (flags == 0 && spec[0] >= '0' && spec[0] <= '9')
                    flags = (uint32_t)atoi(spec);
                if (strcmp(spec, "none") == 0)
                    flags = 0;
                g_App.DebugOverlay = flags;
                g_App.DebugOverlayGiven = true;
            }
            else if (strcmp(argv[i], "--help") == 0)
            {
                printf("usage: imslug_example_glfw_vulkan [--font <path>] [--size <px>] [--scene <1-%d>]\n"
                       "                                  [--renderer <slug|atlas>] [--novsync] [--dpi <scale>]\n"
                       "                                  [--overlay <none|bounds|grid|refs>]\n"
                       "                                  [--screenshot <file.ppm> [scene] [dpi_scale]]\n"
                       "font: --font <ttf|imslug> or IMSLUG_EXAMPLE_FONT; default: fonts embedded in this binary\n"
                       "keys: 1-9 + P scenes, M renderer (Slug/atlas), +/- base size, 0 reset; F1/F2/F3 overlays\n",
                       kSceneCount);
                exit(0);
            }
            else
            {
                fprintf(stderr, "[imslug] unknown argument: %s\n", argv[i]);
                exit(1);
            }
        }
        // No --font: the IMSLUG_EXAMPLE_FONT environment variable overrides
        // the embedded defaults (precedence: CLI, environment, embedded).
        if (g_App.FontPath == nullptr)
        {
            const char* envFont = getenv("IMSLUG_EXAMPLE_FONT");
            if (envFont != nullptr && envFont[0] != '\0')
                g_App.FontPath = envFont;
        }
        if (g_App.BaseSize < kMinSize)
            g_App.BaseSize = kMinSize;
        if (g_App.BaseSize > kMaxSize)
            g_App.BaseSize = kMaxSize;
        if (sceneNumber != 0)
            g_App.Scene = sceneNumber - 1;  // scenes are 1-based on the CLI (the UI keys)
        if (g_App.Scene < 0 || g_App.Scene >= kSceneCount)
            g_App.Scene = 0;
        if (g_App.DpiScale < 0.5f)
            g_App.DpiScale = 0.5f;
        if (g_App.DpiScale > 3.0f)
            g_App.DpiScale = 3.0f;
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
            glfwCreateWindow(kDefaultWindowWidth, kDefaultWindowHeight, "imslug - Slug text", nullptr, nullptr);
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
            ImGui::GetStyle().FontScaleDpi = g_App.DpiScale;

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
                g_App.FontsDirty = true;  // add the fonts (needs the module)
                // --overlay applied after the module exists; without the flag
                // the Config seed (DebugDrawBounds) stays in effect.
                if (g_App.DebugOverlayGiven)
                    ImSlug_ImplVulkan_SetDebugOverlay(g_App.DebugOverlay);
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

        while (ok && glfwWindowShouldClose(g_App.Window) == GLFW_FALSE && !g_App.ShouldClose)
        {
            glfwPollEvents();
            if (!ServiceSwapchain(&ok))
            {
                if (!ok)
                    break;
                glfwWaitEventsTimeout(0.1);  // minimized: wait for a restore/size event
                continue;
            }
            if (g_App.FontsDirty)
                RebuildFonts();
            ok = RenderFrame();
        }
        if (ok)
        {
            // Give the screenshot its frame if it did not land yet (e.g. when
            // the swapchain was recreated).
            int guard = 0;
            while (ok && g_App.ScreenshotPath != nullptr && !g_App.ShouldClose && guard++ < 8)
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
    return Run();
}
