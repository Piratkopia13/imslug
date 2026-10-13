// tests/golden/test_imgui_golden_vulkan.cpp — M3 integration golden test.
//
// Renders one fixed ImGui scene twice in a headless offscreen Vulkan pass: once
// with the Slug font (our ImFontLoader + callbacks + Slug pipeline) and once
// with the same font through ImGui's own stb loader (atlas path, stock
// pipeline), then compares the two frames. This is the end-to-end M3 check:
//
//   * everything except the font's glyphs goes through the stock backend in
//     both variants, so any difference is text rendering;
//   * layout must be identical: the ink masks of the two frames must be equal
//     after a 1 px dilation (catches shifted/missing/wrongly-scaled glyphs);
//   * anti-aliasing is perceptual: the mean |diff| and the tail of large
//     differences stay within documented budgets (Slug solves coverage
//     analytically, the atlas samples a rasterized bitmap).
//
// The scene covers the M3 acceptance surface: several sizes, tables with narrow
// cells (CPU fine clip on the right), shrinking children (window clip), a
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

using namespace ImSlug;

namespace
{
    const int kExitSkip = 77;
    const uint32_t kWidth = 1024;
    const uint32_t kHeight = 640;
    const uint32_t kFrames = 3;  // let the atlas finish building/baking

    // Comparison budgets (measured on the RTX 5080; see the M3 notes):
    //   * the ink masks must agree after a 1 px dilation (layout exactness);
    //   * mean per-channel difference over the whole frame stays small;
    //   * only AA edge pixels may exceed the per-pixel budget, and only a
    //     small fraction of the frame may do so.
    const double kMaxMeanDiff = 8.0;
    const int kEdgeDiffThreshold = 64;
    const double kMaxEdgeFraction = 0.02;

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
    void BuildScene(const char* title)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)kWidth, (float)kHeight);
        io.DeltaTime = 1.0f / 60.0f;

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2((float)kWidth, (float)kHeight), ImGuiCond_Always);
        ImGui::Begin(title, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);

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
    bool RenderVariant(bool slugVariant, unsigned char* outPixels, const char* name)
    {
        ImGui::CreateContext();
        // ImSlug::Init requires a live context (imslug.h); the module lives per
        // variant because each variant owns its contexts/atlas.
        if (!ImSlug::Init())
        {
            std::printf("  [imslug-golden] ImSlug::Init failed (%s)\n", name);
            return false;
        }
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2((float)kWidth, (float)kHeight);
        ImGui::StyleColorsDark();
        ImGui::GetStyle().FontScaleDpi = 1.0f;

        const char* fontPath = IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf");
        if (slugVariant)
        {
            ImFont* font = ImSlug::AddFontFromFileTTF(fontPath, 17.0f);
            if (font == nullptr)
                return false;
            io.FontDefault = font;
        }
        else
        {
            ImFontConfig config;
            config.OversampleH = 1;
            config.OversampleV = 1;
            ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath, 17.0f, &config);
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

        bool ok = true;
        for (uint32_t frame = 0; frame < kFrames && ok; ++frame)
        {
            BuildScene("imslug ImGui golden scene");
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
    if (!RenderVariant(/*slugVariant=*/true, slugPixels, "slug"))
    {
        std::printf("imslug_tests_imgui_golden: FAIL (Slug variant)\n");
        ++failures;
    }

    if (!RenderVariant(/*slugVariant=*/false, stockPixels, "atlas"))
    {
        std::printf("imslug_tests_imgui_golden: FAIL (atlas variant)\n");
        ++failures;
    }

    if (failures == 0)
    {
        // Whole-frame statistics.
        double mean = 0.0;
        int worst = 0;
        int edgePixels = 0;
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
            if (diffSum / 3.0 > kEdgeDiffThreshold)
                edgePixels++;
        }
        mean /= (double)kWidth * kHeight * 3.0;
        const double edgeFraction = (double)edgePixels / ((double)kWidth * kHeight);

        unsigned char* maskA = (unsigned char*)std::malloc((size_t)kWidth * kHeight);
        unsigned char* maskB = (unsigned char*)std::malloc((size_t)kWidth * kHeight);
        int missA = 0, missB = 0;
        if (maskA != nullptr && maskB != nullptr)
        {
            BuildInkMask(slugPixels, maskA);
            BuildInkMask(stockPixels, maskB);
            MasksAgree(maskA, maskB, &missA, &missB);
        }
        int inkPixels = 0;
        if (maskA != nullptr)
            for (uint32_t i = 0; i < kWidth * kHeight; ++i)
                inkPixels += maskA[i];
        std::free(maskA);
        std::free(maskB);

        std::printf("  Slug vs atlas: mean %.3f LSB, worst %d LSB, pixels > %d: %.3f%%, ink %d px\n", mean, worst,
                    kEdgeDiffThreshold, edgeFraction * 100.0, inkPixels);
        std::printf("  layout: ink pixels without a match within 1 px: %d (slug->atlas) / %d (atlas->slug)\n", missA,
                    missB);

        TEST_CHECKF(mean <= kMaxMeanDiff, "mean |diff| %.3f > %.3f LSB", mean, kMaxMeanDiff);
        TEST_CHECKF(edgeFraction <= kMaxEdgeFraction, "%.3f%% of pixels differ by more than %d LSB",
                    edgeFraction * 100.0, kEdgeDiffThreshold);
        TEST_CHECK(missA == 0 && missB == 0);
        TEST_CHECKF(inkPixels > 5000, "scene produced almost no ink (%d px)", inkPixels);

        const char* dir = ".";
        char path[512];
        std::snprintf(path, sizeof(path), "%s/imgui_golden_slug.ppm", dir);
        WritePpm(path, slugPixels);
        std::snprintf(path, sizeof(path), "%s/imgui_golden_atlas.ppm", dir);
        WritePpm(path, stockPixels);
        std::printf("  frames: imgui_golden_slug.ppm / imgui_golden_atlas.ppm\n");
    }

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
