// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_impl_vulkan.cpp — standalone Vulkan rendering core (M2). See
// imslug_impl_vulkan.h for the contract; the Slug math itself lives in
// shaders/vulkan/slug.vert/.frag (ported from EricLengyel/Slug with
// attribution; mapping table in shaders/reference/slug.hlsl).
//
// Module-level singleton (like the ImGui backends): one imslug pipeline per
// Vulkan device. All Vulkan objects except the device objects passed in
// ImSlugVulkanInitInfo are owned here.
//
// Upload protocol (PLAN §5.3): one staging buffer per frame in flight +
// vkCmdCopyBufferToImage + barrier to SHADER_READ_ONLY_OPTIMAL, dirty rows
// only, full re-upload after growth (rare; a device wait is used because the
// old image may still be in flight). The descriptor sets point at the pool
// images and are rewritten only when the images are (re)created.

#include "imslug_impl_vulkan.h"

#include "imslug_shaders_vulkan.h"

// M3 ImGui adapter (the section at the bottom of this file). The M2 rendering
// core above stays ImGui-agnostic; only the adapter block touches these.
#include "imslug.h"
#include "imslug_internal.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <string.h>

namespace ImSlug
{
    namespace
    {
        const uint32_t kDefaultMaxInstancesPerFrame = 16384;
        const uint32_t kMaxFramesInFlight = 3;
        const uint32_t kMaxPipelineEntries = 16;
        const uint32_t kMaxDynamicColorFormats = 4;

        // Must match the PushConstants block in shaders/vulkan/slug.vert.
        struct PushConstants
        {
            float MVP[4][4];
            float ViewportWidth;
            float ViewportHeight;
            uint32_t DebugFlags;  // M4 debug overlay (ImSlugVulkanDebugOverlay)
        };
        static_assert(sizeof(PushConstants) == 76, "push constant block must match slug.vert");

        struct Buffer
        {
            VkBuffer Handle;
            VkDeviceMemory Memory;
            void* Mapped;
            VkDeviceSize Size;
        };

        struct PoolTexture
        {
            VkImage Image;
            VkDeviceMemory Memory;
            VkImageView View;
            VkFormat Format;
            VkDeviceSize TexelBytes;
            int Width;
            int Height;
            bool HasData;  // at least one successful upload into this image
        };

        struct PipelineEntry
        {
            VkRenderPass RenderPass;
            uint32_t Subpass;
            VkPipeline Pipeline;
        };

        struct State
        {
            bool Initialized;
            ImSlugVulkanInitInfo Info;
            uint32_t FramesInFlight;
            uint32_t InstanceCapacity;
            uint32_t MaxImageDimension;

            VkDescriptorSetLayout DescriptorSetLayout;
            VkPipelineLayout PipelineLayout;
            VkDescriptorPool DescriptorPool;
            VkDescriptorSet DescriptorSets[kMaxFramesInFlight];

            VkSampler CurveSampler;
            VkSampler BandSampler;
            PoolTexture Curves;
            PoolTexture Bands;
            bool TexturesCreated;

            // M5: paint records (storage buffer, binding 2). Always bound (a
            // minimal 1-record buffer exists before the first color glyph), so
            // the descriptor set is valid for plain text too.
            Buffer Paints;
            bool PaintsHasData;

            // One staging buffer per frame in flight (B2): the CPU memcpys for
            // frame N use slot N only, so a later frame's recording can never
            // overwrite bytes an earlier frame's recorded copy still has to
            // read. Recording frame N again requires its previous submission to
            // have completed (frames-in-flight contract), so slot growth needs
            // no device wait.
            Buffer Staging[kMaxFramesInFlight];
            Buffer Instances;

            // Dynamic rendering: copy of the init info's create-info (with the
            // color format pointer re-pointed at our own array).
            VkPipelineRenderingCreateInfo RenderingCreateInfo;
            VkFormat RenderingColorFormats[kMaxDynamicColorFormats];

            PipelineEntry Pipelines[kMaxPipelineEntries];
            uint32_t PipelineCount;
        };

        State g_State = {};

        // ---------------------------------------------------------------------
        // Small helpers
        // ---------------------------------------------------------------------
        void CheckVk(VkResult result, const char* what)
        {
            IMSLUG_ASSERT(result == VK_SUCCESS);
            (void)result;
            (void)what;
        }

        uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties)
        {
            VkPhysicalDeviceMemoryProperties memoryProperties;
            vkGetPhysicalDeviceMemoryProperties(g_State.Info.PhysicalDevice, &memoryProperties);
            for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            {
                if ((typeBits & (1u << i)) != 0 &&
                    (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
                {
                    return i;
                }
            }
            IMSLUG_ASSERT(false);
            return 0;
        }

        void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                          Buffer* outBuffer)
        {
            VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bufferInfo.size = size;
            bufferInfo.usage = usage;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            CheckVk(vkCreateBuffer(g_State.Info.Device, &bufferInfo, g_State.Info.Allocator, &outBuffer->Handle),
                    "vkCreateBuffer");

            VkMemoryRequirements requirements;
            vkGetBufferMemoryRequirements(g_State.Info.Device, outBuffer->Handle, &requirements);

            VkMemoryAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocateInfo.allocationSize = requirements.size;
            allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, properties);
            CheckVk(vkAllocateMemory(g_State.Info.Device, &allocateInfo, g_State.Info.Allocator, &outBuffer->Memory),
                    "vkAllocateMemory(buffer)");
            CheckVk(vkBindBufferMemory(g_State.Info.Device, outBuffer->Handle, outBuffer->Memory, 0),
                    "vkBindBufferMemory");

            outBuffer->Size = size;
            outBuffer->Mapped = nullptr;
            if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
                CheckVk(vkMapMemory(g_State.Info.Device, outBuffer->Memory, 0, VK_WHOLE_SIZE, 0, &outBuffer->Mapped),
                        "vkMapMemory");
        }

        void DestroyBuffer(Buffer* buffer)
        {
            if (buffer->Mapped != nullptr)
            {
                vkUnmapMemory(g_State.Info.Device, buffer->Memory);
                buffer->Mapped = nullptr;
            }
            if (buffer->Handle != VK_NULL_HANDLE)
                vkDestroyBuffer(g_State.Info.Device, buffer->Handle, g_State.Info.Allocator);
            if (buffer->Memory != VK_NULL_HANDLE)
                vkFreeMemory(g_State.Info.Device, buffer->Memory, g_State.Info.Allocator);
            buffer->Handle = VK_NULL_HANDLE;
            buffer->Memory = VK_NULL_HANDLE;
            buffer->Size = 0;
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
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        // ---------------------------------------------------------------------
        // Pool textures
        // ---------------------------------------------------------------------
        void DestroyPoolTexture(PoolTexture* texture)
        {
            if (texture->View != VK_NULL_HANDLE)
                vkDestroyImageView(g_State.Info.Device, texture->View, g_State.Info.Allocator);
            if (texture->Image != VK_NULL_HANDLE)
                vkDestroyImage(g_State.Info.Device, texture->Image, g_State.Info.Allocator);
            if (texture->Memory != VK_NULL_HANDLE)
                vkFreeMemory(g_State.Info.Device, texture->Memory, g_State.Info.Allocator);
            texture->View = VK_NULL_HANDLE;
            texture->Image = VK_NULL_HANDLE;
            texture->Memory = VK_NULL_HANDLE;
            texture->Width = 0;
            texture->Height = 0;
            texture->HasData = false;
        }

        // Creates the image at the requested size, or recreates it after pool
        // growth. `changed` reports a (re)creation so the descriptor sets can
        // be rewritten. On recreation the old image may still be read by
        // in-flight frames, so the device is idled first (growth is rare).
        bool EnsurePoolTexture(PoolTexture* texture, VkFormat format, VkDeviceSize texelBytes, int width, int height,
                               bool* changed)
        {
            *changed = false;
            if (texture->Image != VK_NULL_HANDLE && texture->Width == width && texture->Height == height)
                return true;
            if (width <= 0 || height <= 0)
                return true;  // nothing to create yet
            if ((uint32_t)height > g_State.MaxImageDimension)
            {
                IMSLUG_ASSERT(false);  // pool height beyond device limits (M4: fallback)
                return false;
            }

            if (texture->Image != VK_NULL_HANDLE)
            {
                CheckVk(vkDeviceWaitIdle(g_State.Info.Device), "vkDeviceWaitIdle");
                DestroyPoolTexture(texture);
            }

            VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent.width = (uint32_t)width;
            imageInfo.extent.height = (uint32_t)height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            CheckVk(vkCreateImage(g_State.Info.Device, &imageInfo, g_State.Info.Allocator, &texture->Image),
                    "vkCreateImage");

            VkMemoryRequirements requirements;
            vkGetImageMemoryRequirements(g_State.Info.Device, texture->Image, &requirements);
            VkMemoryAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocateInfo.allocationSize = requirements.size;
            allocateInfo.memoryTypeIndex =
                FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            CheckVk(vkAllocateMemory(g_State.Info.Device, &allocateInfo, g_State.Info.Allocator, &texture->Memory),
                    "vkAllocateMemory(image)");
            CheckVk(vkBindImageMemory(g_State.Info.Device, texture->Image, texture->Memory, 0), "vkBindImageMemory");

            VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            viewInfo.image = texture->Image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            CheckVk(vkCreateImageView(g_State.Info.Device, &viewInfo, g_State.Info.Allocator, &texture->View),
                    "vkCreateImageView");

            texture->Format = format;
            texture->TexelBytes = texelBytes;
            texture->Width = width;
            texture->Height = height;
            texture->HasData = false;
            *changed = true;
            return true;
        }

        void WriteDescriptorSets()
        {
            VkDescriptorImageInfo curveInfo = {};
            curveInfo.sampler = g_State.CurveSampler;
            curveInfo.imageView = g_State.Curves.View;
            curveInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo bandInfo = {};
            bandInfo.sampler = g_State.BandSampler;
            bandInfo.imageView = g_State.Bands.View;
            bandInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // M5: paint records (std430 array in slug.frag). The buffer always
            // exists, even before the first color glyph.
            VkDescriptorBufferInfo paintInfo = {};
            paintInfo.buffer = g_State.Paints.Handle;
            paintInfo.offset = 0;
            paintInfo.range = VK_WHOLE_SIZE;

            VkWriteDescriptorSet writes[3] = {};
            for (int i = 0; i < 3; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstBinding = (uint32_t)i;
                writes[i].descriptorCount = 1;
            }
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[0].pImageInfo = &curveInfo;
            writes[1].pImageInfo = &bandInfo;
            writes[2].pBufferInfo = &paintInfo;

            for (uint32_t frame = 0; frame < g_State.FramesInFlight; ++frame)
            {
                writes[0].dstSet = g_State.DescriptorSets[frame];
                writes[1].dstSet = g_State.DescriptorSets[frame];
                writes[2].dstSet = g_State.DescriptorSets[frame];
                vkUpdateDescriptorSets(g_State.Info.Device, 3, writes, 0, nullptr);
            }
        }

        // ---------------------------------------------------------------------
        // Pipelines
        // ---------------------------------------------------------------------
        VkShaderModule CreateShaderModule(const uint32_t* words, uint32_t wordCount)
        {
            VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            moduleInfo.codeSize = (size_t)wordCount * sizeof(uint32_t);
            moduleInfo.pCode = words;
            VkShaderModule module = VK_NULL_HANDLE;
            CheckVk(vkCreateShaderModule(g_State.Info.Device, &moduleInfo, g_State.Info.Allocator, &module),
                    "vkCreateShaderModule");
            return module;
        }

        VkPipeline CreatePipeline(VkRenderPass renderPass, uint32_t subpass)
        {
            VkShaderModule vertModule = CreateShaderModule(imslug_slug_vert_spv, imslug_slug_vert_spv_len);
            VkShaderModule fragModule = CreateShaderModule(imslug_slug_frag_spv, imslug_slug_frag_spv_len);

            VkPipelineShaderStageCreateInfo stages[2] = {};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vertModule;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragModule;
            stages[1].pName = "main";

            // Instance-rate input only: the quad comes from gl_VertexIndex.
            VkVertexInputBindingDescription binding = {};
            binding.binding = 0;
            binding.stride = sizeof(ImSlugGlyphInstance);
            binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

            VkVertexInputAttributeDescription attributes[8] = {};
            attributes[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ImSlugGlyphInstance, EmMinX) };
            attributes[1] = { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ImSlugGlyphInstance, ScreenMinX) };
            attributes[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ImSlugGlyphInstance, Jac00) };
            attributes[3] = { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ImSlugGlyphInstance, BandScaleX) };
            attributes[4] = { 4, 0, VK_FORMAT_R32G32_UINT, offsetof(ImSlugGlyphInstance, GlyphLocX) };
            attributes[5] = { 5, 0, VK_FORMAT_R32_UINT, offsetof(ImSlugGlyphInstance, BandMaxWord) };
            attributes[6] = { 6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ImSlugGlyphInstance, ColorR) };
            attributes[7] = { 7, 0, VK_FORMAT_R32_UINT, offsetof(ImSlugGlyphInstance, PaintIndex) };

            VkPipelineVertexInputStateCreateInfo vertexInput = {
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
            };
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = 8;
            vertexInput.pVertexAttributeDescriptions = attributes;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
            };
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            inputAssembly.primitiveRestartEnable = VK_FALSE;

            VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterization = {
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
            };
            rasterization.polygonMode = VK_POLYGON_MODE_FILL;
            rasterization.cullMode = VK_CULL_MODE_NONE;
            rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterization.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisample = {
                VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
            };
            multisample.rasterizationSamples =
                g_State.Info.MSAASamples != 0 ? g_State.Info.MSAASamples : VK_SAMPLE_COUNT_1_BIT;

            // Stock imgui_impl_vulkan has no depth attachment; keep it disabled.
            VkPipelineDepthStencilStateCreateInfo depthStencil = {
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
            };

            // Blend state mirrors imgui_impl_vulkan.cpp exactly (M3 compatibility):
            // color: src alpha / one-minus-src alpha; alpha: one / one-minus-src alpha.
            VkPipelineColorBlendAttachmentState blendAttachment = {};
            blendAttachment.blendEnable = VK_TRUE;
            blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
            blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            VkPipelineColorBlendStateCreateInfo colorBlend = {
                VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
            };
            colorBlend.attachmentCount = 1;
            colorBlend.pAttachments = &blendAttachment;

            const VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamicState.dynamicStateCount = 2;
            dynamicState.pDynamicStates = dynamicStates;

            VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            pipelineInfo.stageCount = 2;
            pipelineInfo.pStages = stages;
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterization;
            pipelineInfo.pMultisampleState = &multisample;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &colorBlend;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = g_State.PipelineLayout;

            if (g_State.Info.UseDynamicRendering)
            {
                pipelineInfo.pNext = &g_State.RenderingCreateInfo;
            }
            else
            {
                pipelineInfo.renderPass = renderPass;
                pipelineInfo.subpass = subpass;
            }

            VkPipeline pipeline = VK_NULL_HANDLE;
            CheckVk(vkCreateGraphicsPipelines(g_State.Info.Device, g_State.Info.PipelineCache, 1, &pipelineInfo,
                                              g_State.Info.Allocator, &pipeline),
                    "vkCreateGraphicsPipelines");

            vkDestroyShaderModule(g_State.Info.Device, vertModule, g_State.Info.Allocator);
            vkDestroyShaderModule(g_State.Info.Device, fragModule, g_State.Info.Allocator);
            return pipeline;
        }

        VkPipeline GetOrCreatePipeline(VkRenderPass renderPass, uint32_t subpass)
        {
            for (uint32_t i = 0; i < g_State.PipelineCount; ++i)
            {
                if (g_State.Pipelines[i].RenderPass == renderPass && g_State.Pipelines[i].Subpass == subpass)
                    return g_State.Pipelines[i].Pipeline;
            }
            IMSLUG_ASSERT(g_State.PipelineCount < kMaxPipelineEntries);
            if (g_State.PipelineCount >= kMaxPipelineEntries)
                return VK_NULL_HANDLE;

            PipelineEntry& entry = g_State.Pipelines[g_State.PipelineCount++];
            entry.RenderPass = renderPass;
            entry.Subpass = subpass;
            entry.Pipeline = CreatePipeline(renderPass, subpass);
            return entry.Pipeline;
        }

        // ---------------------------------------------------------------------
        // Uploads
        // ---------------------------------------------------------------------
        struct UploadRange
        {
            bool Valid;
            int RowStart;
            int RowEnd;
            VkDeviceSize Offset;
            VkDeviceSize Bytes;
        };

        UploadRange ComputeUploadRange(const PoolTexture& texture, bool firstUpload, int usedRows, int dirtyMin,
                                       int dirtyMax)
        {
            UploadRange range = {};
            if (texture.Image == VK_NULL_HANDLE)
                return range;

            if (firstUpload)
            {
                if (usedRows <= 0)
                    return range;  // no glyphs built yet
                range.RowStart = 0;
                range.RowEnd = usedRows - 1;
            }
            else
            {
                if (dirtyMin > dirtyMax)
                    return range;  // nothing new
                range.RowStart = dirtyMin < 0 ? 0 : dirtyMin;
                range.RowEnd = dirtyMax >= texture.Height ? texture.Height - 1 : dirtyMax;
                if (range.RowStart > range.RowEnd)
                    return range;
            }

            range.Bytes =
                (VkDeviceSize)(range.RowEnd - range.RowStart + 1) * (VkDeviceSize)texture.Width * texture.TexelBytes;
            range.Valid = true;
            return range;
        }

        bool EnsureStaging(uint32_t frameIndex, VkDeviceSize bytes)
        {
            Buffer& staging = g_State.Staging[frameIndex];
            if (staging.Handle != VK_NULL_HANDLE && staging.Size >= bytes)
                return true;
            if (staging.Handle != VK_NULL_HANDLE)
            {
                // Per-frame staging: the previous submission that used this
                // slot has completed (frames-in-flight contract), so the old
                // buffer is idle and no device wait is needed.
                DestroyBuffer(&staging);
            }
            CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging);
            return staging.Mapped != nullptr;
        }

        // Stages `range` rows of `texels` (u16 components) into the frame's
        // staging buffer and records the copy + the layout transitions around it.
        void RecordUpload(VkCommandBuffer cmd, PoolTexture* texture, const uint16_t* texels, const UploadRange& range,
                          VkDeviceSize stagingOffset, const Buffer& staging)
        {
            const size_t componentsPerTexel = (size_t)(texture->TexelBytes / 2);
            memcpy((unsigned char*)staging.Mapped + stagingOffset,
                   texels + (size_t)range.RowStart * (size_t)texture->Width * componentsPerTexel, (size_t)range.Bytes);

            // The image is either freshly created (UNDEFINED) or holds data
            // from a previous frame (SHADER_READ_ONLY_OPTIMAL). A single queue
            // is assumed (the barrier orders against the frame's earlier reads).
            const VkImageLayout oldLayout =
                texture->HasData ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            const VkPipelineStageFlags srcStage =
                texture->HasData ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            const VkAccessFlags srcAccess = texture->HasData ? VK_ACCESS_SHADER_READ_BIT : 0;
            TransitionImage(cmd, texture->Image, oldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcStage, srcAccess,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

            VkBufferImageCopy region = {};
            region.bufferOffset = stagingOffset;
            region.bufferRowLength = 0;  // tightly packed = texture width
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageOffset.x = 0;
            region.imageOffset.y = range.RowStart;
            region.imageOffset.z = 0;
            region.imageExtent.width = (uint32_t)texture->Width;
            region.imageExtent.height = (uint32_t)(range.RowEnd - range.RowStart + 1);
            region.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(cmd, staging.Handle, texture->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);

            TransitionImage(cmd, texture->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_SHADER_READ_BIT);
            texture->HasData = true;
        }

        // Destroys every Vulkan object the module owns. Used by Shutdown (after
        // a device wait) and by the Init failure paths (nothing was submitted
        // yet, so no wait is needed). Null handles are tolerated; Info.Device /
        // Info.Allocator must be set.
        void DestroyDeviceObjects()
        {
            for (uint32_t i = 0; i < g_State.PipelineCount; ++i)
                vkDestroyPipeline(g_State.Info.Device, g_State.Pipelines[i].Pipeline, g_State.Info.Allocator);
            if (g_State.DescriptorPool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(g_State.Info.Device, g_State.DescriptorPool, g_State.Info.Allocator);
            if (g_State.DescriptorSetLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(g_State.Info.Device, g_State.DescriptorSetLayout, g_State.Info.Allocator);
            if (g_State.PipelineLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(g_State.Info.Device, g_State.PipelineLayout, g_State.Info.Allocator);
            if (g_State.CurveSampler != VK_NULL_HANDLE)
                vkDestroySampler(g_State.Info.Device, g_State.CurveSampler, g_State.Info.Allocator);
            if (g_State.BandSampler != VK_NULL_HANDLE)
                vkDestroySampler(g_State.Info.Device, g_State.BandSampler, g_State.Info.Allocator);
            DestroyPoolTexture(&g_State.Curves);
            DestroyPoolTexture(&g_State.Bands);
            for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
                DestroyBuffer(&g_State.Staging[i]);
            DestroyBuffer(&g_State.Instances);
            DestroyBuffer(&g_State.Paints);
        }

        // Init failure path (R2): frees anything already created and clears the
        // state, so a failed Init leaves nothing behind and Shutdown()/a later
        // Init() start clean.
        bool FailInit()
        {
            DestroyDeviceObjects();
            memset(&g_State, 0, sizeof(g_State));
            return false;
        }
    }  // namespace

    // ---------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------
    bool ImSlugVulkanInit(const ImSlugVulkanInitInfo* initInfo)
    {
        IMSLUG_ASSERT(initInfo != nullptr);
        IMSLUG_ASSERT(!g_State.Initialized);
        if (initInfo == nullptr || g_State.Initialized)
            return false;
        if (initInfo->PhysicalDevice == VK_NULL_HANDLE || initInfo->Device == VK_NULL_HANDLE ||
            initInfo->Queue == VK_NULL_HANDLE || initInfo->ImageCount == 0)
        {
            return false;
        }

        memset(&g_State, 0, sizeof(g_State));
        g_State.Info = *initInfo;
        g_State.FramesInFlight = initInfo->ImageCount < kMaxFramesInFlight ? initInfo->ImageCount : kMaxFramesInFlight;
        g_State.InstanceCapacity =
            initInfo->MaxInstancesPerFrame != 0 ? initInfo->MaxInstancesPerFrame : kDefaultMaxInstancesPerFrame;

        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(initInfo->PhysicalDevice, &deviceProperties);
        g_State.MaxImageDimension = deviceProperties.limits.maxImageDimension2D;
        if (g_State.MaxImageDimension < (uint32_t)kPoolWidth)
            return FailInit();

        // The pool formats are core Vulkan; verify the device can sample them.
        VkFormatProperties curveProperties;
        vkGetPhysicalDeviceFormatProperties(initInfo->PhysicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &curveProperties);
        if ((curveProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)
            return FailInit();
        VkFormatProperties bandProperties;
        vkGetPhysicalDeviceFormatProperties(initInfo->PhysicalDevice, VK_FORMAT_R16G16_UINT, &bandProperties);
        if ((bandProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)
            return FailInit();

        // Descriptor set layout: curve (sampler2D) + band (usampler2D) + paint
        // records (M5 storage buffer read by slug.frag).
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (int i = 0; i < 2; ++i)
        {
            bindings[i].binding = (uint32_t)i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo setLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setLayoutInfo.bindingCount = 3;
        setLayoutInfo.pBindings = bindings;
        CheckVk(vkCreateDescriptorSetLayout(initInfo->Device, &setLayoutInfo, initInfo->Allocator,
                                            &g_State.DescriptorSetLayout),
                "vkCreateDescriptorSetLayout");

        VkPushConstantRange pushRange = {};
        // Both stages declare the block in slug.vert/frag (the fragment stage
        // reads DebugFlags), so the layout range must cover both.
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = (uint32_t)sizeof(PushConstants);
        VkPipelineLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &g_State.DescriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        CheckVk(vkCreatePipelineLayout(initInfo->Device, &layoutInfo, initInfo->Allocator, &g_State.PipelineLayout),
                "vkCreatePipelineLayout");

        // Own pool, one set per frame in flight (static after upload).
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 2 * g_State.FramesInFlight;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[1].descriptorCount = g_State.FramesInFlight;
        VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = g_State.FramesInFlight;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        CheckVk(vkCreateDescriptorPool(initInfo->Device, &poolInfo, initInfo->Allocator, &g_State.DescriptorPool),
                "vkCreateDescriptorPool");

        VkDescriptorSetLayout setLayouts[kMaxFramesInFlight] = {};
        for (uint32_t i = 0; i < g_State.FramesInFlight; ++i)
            setLayouts[i] = g_State.DescriptorSetLayout;
        VkDescriptorSetAllocateInfo allocateSets = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocateSets.descriptorPool = g_State.DescriptorPool;
        allocateSets.descriptorSetCount = g_State.FramesInFlight;
        allocateSets.pSetLayouts = setLayouts;
        CheckVk(vkAllocateDescriptorSets(initInfo->Device, &allocateSets, g_State.DescriptorSets),
                "vkAllocateDescriptorSets");

        VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        CheckVk(vkCreateSampler(initInfo->Device, &samplerInfo, initInfo->Allocator, &g_State.CurveSampler),
                "vkCreateSampler");
        CheckVk(vkCreateSampler(initInfo->Device, &samplerInfo, initInfo->Allocator, &g_State.BandSampler),
                "vkCreateSampler");

        // Dynamic rendering: copy the create-info and re-point the format array.
        if (initInfo->UseDynamicRendering)
        {
            g_State.RenderingCreateInfo = initInfo->PipelineRenderingCreateInfo;
            g_State.RenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            g_State.RenderingCreateInfo.pNext = nullptr;
            const uint32_t colorCount = g_State.RenderingCreateInfo.colorAttachmentCount;
            IMSLUG_ASSERT(colorCount <= kMaxDynamicColorFormats);
            if (colorCount > kMaxDynamicColorFormats)
                return FailInit();
            for (uint32_t i = 0; i < colorCount; ++i)
                g_State.RenderingColorFormats[i] = initInfo->PipelineRenderingCreateInfo.pColorAttachmentFormats[i];
            g_State.RenderingCreateInfo.pColorAttachmentFormats =
                colorCount > 0 ? g_State.RenderingColorFormats : nullptr;
        }

        const VkDeviceSize instanceBytes =
            (VkDeviceSize)g_State.FramesInFlight * g_State.InstanceCapacity * sizeof(ImSlugGlyphInstance);
        CreateBuffer(instanceBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g_State.Instances);
        if (g_State.Instances.Mapped == nullptr)
            return FailInit();

        // M5: the paint record storage buffer always exists (at least one
        // record) so binding 2 is valid for plain text as well.
        CreateBuffer(sizeof(ImSlugPaintRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_State.Paints);
        if (g_State.Paints.Handle == VK_NULL_HANDLE)
            return FailInit();
        g_State.PaintsHasData = false;

        g_State.Initialized = true;
        return true;
    }

    void ImSlugVulkanShutdown()
    {
        if (!g_State.Initialized)
            return;

        CheckVk(vkDeviceWaitIdle(g_State.Info.Device), "vkDeviceWaitIdle");
        DestroyDeviceObjects();
        memset(&g_State, 0, sizeof(g_State));
    }

    bool ImSlugVulkanUploadPools(VkCommandBuffer cmd, ImSlugPools* pools, uint32_t frameIndex)
    {
        IMSLUG_ASSERT(g_State.Initialized);
        IMSLUG_ASSERT(cmd != VK_NULL_HANDLE);
        IMSLUG_ASSERT(frameIndex < g_State.FramesInFlight);
        if (!g_State.Initialized || pools == nullptr || cmd == VK_NULL_HANDLE || frameIndex >= g_State.FramesInFlight)
            return false;

        bool curveChanged = false;
        bool bandChanged = false;
        const int curveRows = pools->Curves.Height;
        const int bandRows = pools->Bands.Height;
        if (curveRows > 0 &&
            !EnsurePoolTexture(&g_State.Curves, VK_FORMAT_R16G16B16A16_SFLOAT, 8, kPoolWidth, curveRows, &curveChanged))
        {
            return false;
        }
        if (bandRows > 0 &&
            !EnsurePoolTexture(&g_State.Bands, VK_FORMAT_R16G16_UINT, 4, kPoolWidth, bandRows, &bandChanged))
        {
            return false;
        }

        // Descriptor sets are written once both views exist (and rewritten on
        // (re)creation); a single missing pool cannot be sampled yet.
        const bool curveReady = g_State.Curves.View != VK_NULL_HANDLE;
        const bool bandReady = g_State.Bands.View != VK_NULL_HANDLE;
        if (curveReady && bandReady && (!g_State.TexturesCreated || curveChanged || bandChanged))
        {
            WriteDescriptorSets();
            g_State.TexturesCreated = true;
        }

        // Per-pool ranges (R4): a pool without a view has nothing to consume and
        // keeps its dirty rows; the other pool uploads and clears normally.
        UploadRange curveRange = {};
        if (curveReady)
        {
            curveRange = ComputeUploadRange(g_State.Curves, !g_State.Curves.HasData, pools->Curves.UsedRows,
                                            pools->Curves.DirtyRowMin, pools->Curves.DirtyRowMax);
        }
        UploadRange bandRange = {};
        if (bandReady)
        {
            bandRange = ComputeUploadRange(g_State.Bands, !g_State.Bands.HasData, pools->Bands.UsedRows,
                                           pools->Bands.DirtyRowMin, pools->Bands.DirtyRowMax);
        }

        // M5: paint records. The buffer grows with the pool (device wait: rare);
        // the first upload covers every record, later frames only the dirty ones.
        const VkDeviceSize paintBytes = (VkDeviceSize)pools->Paints.Count * sizeof(ImSlugPaintRecord);
        bool paintChanged = false;
        if (paintBytes > g_State.Paints.Size)
        {
            if (g_State.Paints.Handle != VK_NULL_HANDLE)
            {
                CheckVk(vkDeviceWaitIdle(g_State.Info.Device), "vkDeviceWaitIdle");
                DestroyBuffer(&g_State.Paints);
            }
            const VkDeviceSize newSize = paintBytes < 1024 ? 1024 : paintBytes;
            CreateBuffer(newSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_State.Paints);
            if (g_State.Paints.Handle == VK_NULL_HANDLE)
                return false;
            g_State.PaintsHasData = false;
            paintChanged = true;
        }

        int paintFirst = 0;
        int paintLast = -1;
        if (!g_State.PaintsHasData)
        {
            if (pools->Paints.Count > 0)
                paintLast = pools->Paints.Count - 1;
        }
        else if (pools->Paints.DirtyRecordMin <= pools->Paints.DirtyRecordMax)
        {
            paintFirst = pools->Paints.DirtyRecordMin < 0 ? 0 : pools->Paints.DirtyRecordMin;
            paintLast = pools->Paints.DirtyRecordMax >= pools->Paints.Count ? pools->Paints.Count - 1
                                                                            : pools->Paints.DirtyRecordMax;
        }
        const bool paintValid = paintLast >= paintFirst && pools->Paints.Bytes != nullptr;
        const VkDeviceSize paintUploadBytes =
            paintValid ? (VkDeviceSize)(paintLast - paintFirst + 1) * sizeof(ImSlugPaintRecord) : 0;

        if (paintChanged && g_State.TexturesCreated)
            WriteDescriptorSets();

        const VkDeviceSize totalBytes = curveRange.Bytes + bandRange.Bytes + paintUploadBytes;
        if (totalBytes > 0 && !EnsureStaging(frameIndex, totalBytes))
            return false;

        VkDeviceSize offset = 0;
        if (curveRange.Valid)
        {
            curveRange.Offset = offset;
            offset += curveRange.Bytes;
        }
        if (bandRange.Valid)
        {
            bandRange.Offset = offset;
            offset += bandRange.Bytes;
        }
        const VkDeviceSize paintStagingOffset = offset;

        if (curveRange.Valid)
        {
            RecordUpload(cmd, &g_State.Curves, pools->Curves.Texels, curveRange, curveRange.Offset,
                         g_State.Staging[frameIndex]);
            ImSlugCurvePoolClearDirtyRows(&pools->Curves);
        }
        if (bandRange.Valid)
        {
            RecordUpload(cmd, &g_State.Bands, pools->Bands.Texels, bandRange, bandRange.Offset,
                         g_State.Staging[frameIndex]);
            ImSlugBandPoolClearDirtyRows(&pools->Bands);
        }
        if (paintValid)
        {
            memcpy((unsigned char*)g_State.Staging[frameIndex].Mapped + paintStagingOffset,
                   pools->Paints.Bytes + (size_t)paintFirst * sizeof(ImSlugPaintRecord), (size_t)paintUploadBytes);

            // The buffer is either freshly created (no readers) or holds data a
            // previous frame read (SHADER_READ_ONLY via FRAGMENT_SHADER).
            const VkPipelineStageFlags srcStage =
                g_State.PaintsHasData ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            const VkAccessFlags srcAccess = g_State.PaintsHasData ? VK_ACCESS_SHADER_READ_BIT : 0;

            VkBufferMemoryBarrier barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = g_State.Paints.Handle;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);

            VkBufferCopy region = {};
            region.srcOffset = paintStagingOffset;
            region.dstOffset = (VkDeviceSize)paintFirst * sizeof(ImSlugPaintRecord);
            region.size = paintUploadBytes;
            vkCmdCopyBuffer(cmd, g_State.Staging[frameIndex].Handle, g_State.Paints.Handle, 1, &region);

            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 1, &barrier, 0, nullptr);

            g_State.PaintsHasData = true;
            ImSlugPaintPoolClearDirtyRecords(&pools->Paints);
        }
        return true;
    }

    uint32_t ImSlugVulkanGetFramesInFlight()
    {
        return g_State.FramesInFlight;
    }

    uint32_t ImSlugVulkanGetInstanceCapacity()
    {
        return g_State.InstanceCapacity;
    }

    ImSlugGlyphInstance* ImSlugVulkanGetInstanceData(uint32_t frameIndex)
    {
        IMSLUG_ASSERT(g_State.Initialized);
        IMSLUG_ASSERT(frameIndex < g_State.FramesInFlight);
        if (!g_State.Initialized || frameIndex >= g_State.FramesInFlight || g_State.Instances.Mapped == nullptr)
            return nullptr;
        return (ImSlugGlyphInstance*)g_State.Instances.Mapped + (size_t)frameIndex * g_State.InstanceCapacity;
    }

    void ImSlugVulkanDraw(VkCommandBuffer cmd, const ImSlugVulkanDrawInfo* drawInfo)
    {
        IMSLUG_ASSERT(g_State.Initialized);
        IMSLUG_ASSERT(drawInfo != nullptr);
        if (!g_State.Initialized || drawInfo == nullptr || cmd == VK_NULL_HANDLE || drawInfo->InstanceCount == 0)
            return;

        // Defensive clamps (R3, release-safe): never bind an unwritten
        // descriptor set and never fetch past the instance ring.
        if (!g_State.TexturesCreated || drawInfo->MVP == nullptr)
        {
            IMSLUG_ASSERT(g_State.TexturesCreated);
            IMSLUG_ASSERT(drawInfo->MVP != nullptr);
            return;
        }
        const uint32_t frameIndex = drawInfo->FrameIndex;
        if (frameIndex >= g_State.FramesInFlight)
        {
            IMSLUG_ASSERT(false);
            return;
        }
        uint32_t firstInstance = drawInfo->FirstInstance;
        uint32_t instanceCount = drawInfo->InstanceCount;
        if (firstInstance >= g_State.InstanceCapacity)
        {
            IMSLUG_ASSERT(false);
            return;
        }
        IMSLUG_ASSERT(instanceCount <= g_State.InstanceCapacity - firstInstance);
        if (instanceCount > g_State.InstanceCapacity - firstInstance)
            instanceCount = g_State.InstanceCapacity - firstInstance;

        const VkPipeline pipeline = GetOrCreatePipeline(drawInfo->RenderPass, drawInfo->Subpass);
        if (pipeline == VK_NULL_HANDLE)
            return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_State.PipelineLayout, 0, 1,
                                &g_State.DescriptorSets[frameIndex], 0, nullptr);

        // The instance ring is bound whole; batches index it with a
        // slot-relative firstInstance, and the draw adds this frame's ring base
        // (FirstInstance alone would read slot 0 for every frame).
        const VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g_State.Instances.Handle, &vertexOffset);

        PushConstants push = {};
        memcpy(push.MVP, drawInfo->MVP, sizeof(push.MVP));
        push.ViewportWidth = drawInfo->ViewportWidth;
        push.ViewportHeight = drawInfo->ViewportHeight;
        push.DebugFlags = drawInfo->DebugFlags;
        // Both stages own the block (the fragment shader reads DebugFlags), so
        // the push must name every stage of the layout's range (VUID-...-01796).
        vkCmdPushConstants(cmd, g_State.PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);

        const uint64_t ringFirstInstance = (uint64_t)frameIndex * g_State.InstanceCapacity + (uint64_t)firstInstance;
        IMSLUG_ASSERT(ringFirstInstance <= UINT32_MAX);
        if (ringFirstInstance > UINT32_MAX)
            return;

        vkCmdSetScissor(cmd, 0, 1, &drawInfo->Scissor);
        vkCmdDraw(cmd, 4, instanceCount, 0, (uint32_t)ringFirstInstance);
    }

    // =====================================================================
    // M3 ImGui adapter
    // =====================================================================
    // Bridges the stock ImGui Vulkan backend and this module:
    //   * Init maps ImGui_ImplVulkan_InitInfo onto ImSlugVulkanInitInfo and
    //     registers the ImSlugRenderIO the core's pre-pass uses;
    //   * UploadPools records the dirty-row copies (outside the render pass)
    //     and advances the frames-in-flight slot;
    //   * RenderDrawData rewrites the draw data (Slug text -> callbacks) and
    //     hands everything to ImGui_ImplVulkan_RenderDrawData;
    //   * the batch callback records one instanced draw per text run into the
    //     backend's active command buffer (published through
    //     PlatformIO().Renderer_RenderState by the stock backend).
    // The callback runs before the stock scissor/texture setup, so it sets its
    // own dynamic scissor; the core injects a ResetRenderState command right
    // after every Slug run (R5).
    namespace
    {

        struct ImGuiVulkanState
        {
            bool Initialized;
            bool DynamicRendering;
            VkRenderPass RenderPass;  // main viewport (multi-viewport: compatible passes)
            uint32_t Subpass;

            // M4 debug overlay flags (ImSlugVulkanDebugOverlay), toggled with
            // ImSlug_ImplVulkan_SetDebugOverlay; seeded from
            // Config::DebugDrawBounds at Init.
            uint32_t DebugOverlay;

            // Per-frame slot and the instance allocation cursor inside it.
            uint32_t FrameIndex;      // ring slot for the current frame
            uint32_t FrameCounter;    // advanced by UploadPools (one per frame)
            int UploadFrameCount;     // ImGui::GetFrameCount() of the last upload (-1 = none)
            uint32_t InstanceCursor;  // per-frame instance allocation cursor

            // Per RenderDrawData call (callbacks run inside the stock backend's
            // render loop, which republishes CommandBuffer + framebuffer size).
            float DisplayPosX, DisplayPosY;
            float FramebufferScaleX, FramebufferScaleY;
            uint32_t FramebufferWidth, FramebufferHeight;
        };

        ImGuiVulkanState g_ImGuiVulkan;

        uint32_t ImGuiRenderIO_GetFrameIndex()
        {
            return g_ImGuiVulkan.FrameIndex;
        }

        ImSlugGlyphInstance* ImGuiRenderIO_ReserveInstances(uint32_t count, uint32_t* outFirstInstance)
        {
            if (!g_ImGuiVulkan.Initialized || count == 0)
                return nullptr;
            const uint32_t capacity = ImSlugVulkanGetInstanceCapacity();
            if (count > capacity - g_ImGuiVulkan.InstanceCursor)
                return nullptr;
            ImSlugGlyphInstance* base = ImSlugVulkanGetInstanceData(g_ImGuiVulkan.FrameIndex);
            if (base == nullptr)
                return nullptr;
            const uint32_t first = g_ImGuiVulkan.InstanceCursor;
            g_ImGuiVulkan.InstanceCursor += count;
            *outFirstInstance = first;
            return base + first;
        }

        void ImGuiRenderIO_DrawBatch(const ImSlugBatch* batch)
        {
            if (batch == nullptr || batch->InstanceCount == 0)
                return;
            const ImGui_ImplVulkan_RenderState* renderState =
                (const ImGui_ImplVulkan_RenderState*)ImGui::GetPlatformIO().Renderer_RenderState;
            if (renderState == nullptr || renderState->CommandBuffer == VK_NULL_HANDLE)
                return;  // not inside ImGui_ImplVulkan_RenderDrawData
            if (g_ImGuiVulkan.FramebufferWidth == 0 || g_ImGuiVulkan.FramebufferHeight == 0)
                return;

            // Clip rects arrive in ImGui display coordinates; convert and clamp
            // exactly like the stock backend does (vkCmdSetScissor rejects
            // off-bounds values).
            float clipMinX = (batch->ClipMinX - g_ImGuiVulkan.DisplayPosX) * g_ImGuiVulkan.FramebufferScaleX;
            float clipMinY = (batch->ClipMinY - g_ImGuiVulkan.DisplayPosY) * g_ImGuiVulkan.FramebufferScaleY;
            float clipMaxX = (batch->ClipMaxX - g_ImGuiVulkan.DisplayPosX) * g_ImGuiVulkan.FramebufferScaleX;
            float clipMaxY = (batch->ClipMaxY - g_ImGuiVulkan.DisplayPosY) * g_ImGuiVulkan.FramebufferScaleY;
            if (clipMinX < 0.0f)
                clipMinX = 0.0f;
            if (clipMinY < 0.0f)
                clipMinY = 0.0f;
            if (clipMaxX > (float)g_ImGuiVulkan.FramebufferWidth)
                clipMaxX = (float)g_ImGuiVulkan.FramebufferWidth;
            if (clipMaxY > (float)g_ImGuiVulkan.FramebufferHeight)
                clipMaxY = (float)g_ImGuiVulkan.FramebufferHeight;
            if (clipMaxX <= clipMinX || clipMaxY <= clipMinY)
                return;

            float mvp[16];
            ImSlugVulkanBuildOrthoMVP((float)g_ImGuiVulkan.FramebufferWidth, (float)g_ImGuiVulkan.FramebufferHeight,
                                      mvp);

            ImSlugVulkanDrawInfo drawInfo = {};
            drawInfo.FrameIndex = batch->FrameIndex;
            drawInfo.RenderPass = g_ImGuiVulkan.DynamicRendering ? VK_NULL_HANDLE : g_ImGuiVulkan.RenderPass;
            drawInfo.Subpass = g_ImGuiVulkan.Subpass;
            drawInfo.FirstInstance = batch->FirstInstance;
            drawInfo.InstanceCount = batch->InstanceCount;
            drawInfo.MVP = mvp;
            drawInfo.ViewportWidth = (float)g_ImGuiVulkan.FramebufferWidth;
            drawInfo.ViewportHeight = (float)g_ImGuiVulkan.FramebufferHeight;
            drawInfo.DebugFlags = g_ImGuiVulkan.DebugOverlay;
            drawInfo.Scissor.offset.x = (int32_t)clipMinX;
            drawInfo.Scissor.offset.y = (int32_t)clipMinY;
            drawInfo.Scissor.extent.width = (uint32_t)(clipMaxX - clipMinX);
            drawInfo.Scissor.extent.height = (uint32_t)(clipMaxY - clipMinY);
            ImSlugVulkanDraw(renderState->CommandBuffer, &drawInfo);
        }

        const ImSlugRenderIO g_ImGuiRenderIO = { "vulkan", ImGuiRenderIO_GetFrameIndex, ImGuiRenderIO_ReserveInstances,
                                                 ImGuiRenderIO_DrawBatch };
    }  // namespace

    namespace
    {
        // Shared by ImSlug_ImplVulkan_Init and
        // ImSlug_ImplVulkan_InitFromVulkanInfo: initializes the M2 core from
        // the Vulkan objects and registers the render interface. Instance-ring
        // and MSAA precedence is documented in imslug_impl_vulkan.h.
        bool ImSlugImplVulkanInitFromInfo(const ImSlugVulkanInitInfo* vulkanInfo)
        {
            IMSLUG_ASSERT(vulkanInfo != nullptr);
            if (vulkanInfo == nullptr)
                return false;
            if (g_ImGuiVulkan.Initialized)
                ImSlug_ImplVulkan_Shutdown();

            const Config* config = ImSlugGetConfig();
            if (config == nullptr)
            {
                IMSLUG_ASSERT(false && "imslug: call ImSlug::Init() before ImSlug_ImplVulkan_Init()");
                return false;
            }

            ImSlugVulkanInitInfo info = *vulkanInfo;
            if (info.MSAASamples == 0)
                info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            if (info.MaxInstancesPerFrame == 0)
                info.MaxInstancesPerFrame = config->MaxInstancesPerFrame;

            if (!ImSlugVulkanInit(&info))
                return false;

            g_ImGuiVulkan = ImGuiVulkanState();
            g_ImGuiVulkan.Initialized = true;
            g_ImGuiVulkan.UploadFrameCount = -1;
            g_ImGuiVulkan.DynamicRendering = info.UseDynamicRendering;
            g_ImGuiVulkan.RenderPass = info.RenderPass;
            g_ImGuiVulkan.Subpass = info.Subpass;
            // Config::DebugDrawBounds seeds the glyph-bounds overlay (the runtime
            // toggle stays available for the band views).
            g_ImGuiVulkan.DebugOverlay = config->DebugDrawBounds ? (uint32_t)ImSlugVulkanDebugOverlay_GlyphBounds
                                                                 : (uint32_t)ImSlugVulkanDebugOverlay_None;
            ImSlugSetRenderIO(&g_ImGuiRenderIO);
            return true;
        }
    }  // namespace

    bool ImSlug_ImplVulkan_Init(const ImGui_ImplVulkan_InitInfo* initInfo)
    {
        IMSLUG_ASSERT(initInfo != nullptr);
        if (initInfo == nullptr)
            return false;

        ImSlugVulkanInitInfo vulkanInfo = {};
        vulkanInfo.PhysicalDevice = initInfo->PhysicalDevice;
        vulkanInfo.Device = initInfo->Device;
        vulkanInfo.Queue = initInfo->Queue;
        vulkanInfo.QueueFamily = initInfo->QueueFamily;
        vulkanInfo.Allocator = initInfo->Allocator;
        vulkanInfo.ImageCount = initInfo->ImageCount;
        vulkanInfo.PipelineCache = initInfo->PipelineCache;
        vulkanInfo.MSAASamples = initInfo->PipelineInfoMain.MSAASamples != 0 ? initInfo->PipelineInfoMain.MSAASamples
                                                                             : VK_SAMPLE_COUNT_1_BIT;
        vulkanInfo.RenderPass = initInfo->PipelineInfoMain.RenderPass;
        vulkanInfo.Subpass = initInfo->PipelineInfoMain.Subpass;
        vulkanInfo.UseDynamicRendering = initInfo->UseDynamicRendering;
#ifdef IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
        if (initInfo->UseDynamicRendering && initInfo->PipelineInfoMain.PipelineRenderingCreateInfo.sType ==
                                                 VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR)
            vulkanInfo.PipelineRenderingCreateInfo = initInfo->PipelineInfoMain.PipelineRenderingCreateInfo;
#endif
        return ImSlugImplVulkanInitFromInfo(&vulkanInfo);
    }

    bool ImSlug_ImplVulkan_InitFromVulkanInfo(const ImSlugVulkanInitInfo* initInfo)
    {
        return ImSlugImplVulkanInitFromInfo(initInfo);
    }

    void ImSlug_ImplVulkan_Shutdown()
    {
        if (!g_ImGuiVulkan.Initialized)
            return;
        ImSlugSetRenderIO(nullptr);
        ImSlugVulkanShutdown();
        g_ImGuiVulkan = ImGuiVulkanState();
    }

    void ImSlug_ImplVulkan_UploadPools(VkCommandBuffer cmd)
    {
        if (!g_ImGuiVulkan.Initialized || cmd == VK_NULL_HANDLE)
            return;
        // One upload per frame (a second call would skip an instance-ring slot
        // and overwrite staging of a frame still in flight). ImGui's frame
        // count is the only per-frame signal available outside the frame cycle.
        const int frameCount = ImGui::GetFrameCount();
        IMSLUG_ASSERT(g_ImGuiVulkan.UploadFrameCount != frameCount &&
                      "imslug: ImSlug_ImplVulkan_UploadPools called twice in one frame");
        g_ImGuiVulkan.UploadFrameCount = frameCount;

        const uint32_t framesInFlight = ImSlugVulkanGetFramesInFlight();
        g_ImGuiVulkan.FrameIndex = framesInFlight != 0 ? (g_ImGuiVulkan.FrameCounter % framesInFlight) : 0;
        g_ImGuiVulkan.FrameCounter++;
        g_ImGuiVulkan.InstanceCursor = 0;

        ImSlugPools* pools = ImSlugGetPools();
        if (pools != nullptr)
            ImSlugVulkanUploadPools(cmd, pools, g_ImGuiVulkan.FrameIndex);
    }

    void ImSlug_ImplVulkan_RenderDrawData(ImDrawData* drawData, VkCommandBuffer cmd, VkPipeline mainPipeline)
    {
        if (!g_ImGuiVulkan.Initialized || drawData == nullptr)
        {
            // No Slug module (or no draw data): render through the stock path.
            ImGui_ImplVulkan_RenderDrawData(drawData, cmd, mainPipeline);
            return;
        }
        IMSLUG_ASSERT(g_ImGuiVulkan.UploadFrameCount >= 0 &&
                      "imslug: call ImSlug_ImplVulkan_UploadPools() before rendering (see imslug_impl_vulkan.h)");

        g_ImGuiVulkan.FramebufferWidth = (uint32_t)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
        g_ImGuiVulkan.FramebufferHeight = (uint32_t)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
        g_ImGuiVulkan.DisplayPosX = drawData->DisplayPos.x;
        g_ImGuiVulkan.DisplayPosY = drawData->DisplayPos.y;
        g_ImGuiVulkan.FramebufferScaleX = drawData->FramebufferScale.x;
        g_ImGuiVulkan.FramebufferScaleY = drawData->FramebufferScale.y;

        ImSlug::PreprocessDrawData(drawData);
        ImGui_ImplVulkan_RenderDrawData(drawData, cmd, mainPipeline);

        g_ImGuiVulkan.FramebufferWidth = g_ImGuiVulkan.FramebufferHeight = 0;
    }

    void ImSlug_ImplVulkan_SetDebugOverlay(uint32_t overlayFlags)
    {
        // Valid while the module is initialized; a call before Init is ignored
        // (debug builds assert - the caller asked for a view that cannot exist).
        IMSLUG_ASSERT(g_ImGuiVulkan.Initialized);
        if (!g_ImGuiVulkan.Initialized)
            return;
        IMSLUG_ASSERT(
            (overlayFlags & ~(uint32_t)(ImSlugVulkanDebugOverlay_GlyphBounds | ImSlugVulkanDebugOverlay_BandGrid |
                                        ImSlugVulkanDebugOverlay_RefHeat)) == 0);
        g_ImGuiVulkan.DebugOverlay = overlayFlags;
    }

    uint32_t ImSlug_ImplVulkan_GetDebugOverlay()
    {
        return g_ImGuiVulkan.Initialized ? g_ImGuiVulkan.DebugOverlay : (uint32_t)ImSlugVulkanDebugOverlay_None;
    }
}  // namespace ImSlug
