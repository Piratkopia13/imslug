// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_impl_webgpu.cpp — standalone WebGPU rendering core. See
// imslug_impl_webgpu.h for the contract; the Slug math itself lives in
// shaders/wgsl/slug.{vert,frag}.wgsl (the attributed WGSL port of the GLSL
// sources; the auditable mapping tables are shaders/reference/slug.hlsl for the
// GLSL and shaders/reference/wgsl-port.txt for the WGSL port).
//
// Module-level singleton (like the ImGui backends): one imslug pipeline per
// WebGPU device. All objects except the device objects passed in
// ImSlugWGPUInitInfo are owned here.
//
// Upload protocol:
//   * queue.writeTexture for dirty pool rows (one call per contiguous range;
//     after creation/growth the first upload covers [0, UsedRows));
//   * queue.writeBuffer for dirty paint records and for the frame's staged
//     instance records (one call per frame);
//   * one 80 B queue.writeBuffer per Slug batch into the uniform ring, bound at
//     draw time as a dynamic-offset uniform binding (the push-constant
//     replacement).
// All of these are CPU->GPU copies WebGPU performs itself: there is no staging
// buffer and no device wait, and growth releases the old resource while the
// implementation keeps in-flight frames alive.

#include "imslug_impl_webgpu.h"

#include "imslug_shaders_webgpu.h"

// ImGui adapter (the section at the bottom of this file). The rendering core
// above stays ImGui-agnostic; only the adapter block touches these.
#include "imslug.h"
#include "imslug_internal.h"

#include "imgui.h"
#include "imgui_impl_wgpu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ImSlug
{
    namespace
    {
        const uint32_t kDefaultMaxInstancesPerFrame = 16384;
        // Default-capacity overflow grows the instance ring (doubling, at least
        // to the observed demand) up to this many times the default. An
        // explicit MaxInstancesPerFrame is a hard cap and is never exceeded.
        const uint32_t kMaxInstanceGrowthFactor = 8;
        const uint32_t kMaxFramesInFlight = 3;

        // Per-draw uniform ring (the push-constant replacement): one 80 B
        // parameter record per Slug batch, one region per frame in flight. The
        // default is a batch budget no realistic frame reaches (text runs merge
        // into one batch per ImDrawCmd); an overflowing frame drops the
        // overflowing runs, grows the ring (doubling, up to
        // kMaxBatchGrowthFactor x the default) for the next frame and prints a
        // one-time stderr warning - the instance ring's policy.
        const uint32_t kDefaultMaxBatchesPerFrame = 2048;
        const uint32_t kMaxBatchGrowthFactor = 8;

        // Device-limit fallbacks (WebGPU spec defaults) for devices that do not
        // report limits. The pool design allows 65536 rows, so
        // maxTextureDimension2D is the limit that is actually reachable.
        const uint32_t kDefaultMaxTextureDimension2D = 8192;
        const uint64_t kDefaultMaxStorageBufferBindingSize = 128ull * 1024ull * 1024ull;
        const uint32_t kDefaultUniformBufferOffsetAlignment = 256;
        const uint64_t kMinPaintBufferBytes = 1024;

        // The push-constant replacement, byte-compatible with the WGSL `Pc`
        // uniform block (shaders/wgsl/slug.*.wgsl): four MVP rows, the
        // framebuffer dimensions (SlugDilate's `dim`) and the debug flags,
        // padded to the uniform address space's 16-byte struct alignment.
        struct UniformParams
        {
            float MVP[4][4];       // offset 0: the four rows of the screen-pixels -> clip MVP
            float ViewportWidth;   // offset 64
            float ViewportHeight;  // offset 68
            uint32_t DebugFlags;   // offset 72 (ImSlugWGPUDebugOverlay)
            uint32_t Pad;          // offset 76
        };
        static_assert(sizeof(UniformParams) == 80, "uniform params must match the WGSL Pc block");
        static_assert(offsetof(UniformParams, ViewportWidth) == 64, "GPU-visible uniform layout");
        static_assert(offsetof(UniformParams, ViewportHeight) == 68, "GPU-visible uniform layout");
        static_assert(offsetof(UniformParams, DebugFlags) == 72, "GPU-visible uniform layout");

        struct PoolTexture
        {
            WGPUTexture Texture;
            WGPUTextureView View;
            WGPUTextureFormat Format;
            uint32_t TexelBytes;
            int Width;
            int Height;
            bool HasData;  // at least one successful upload into this texture
        };

        struct State
        {
            bool Initialized;
            ImSlugWGPUInitInfo Info;
            uint32_t FramesInFlight;

            // Instance ring growth ceiling: the explicit MaxInstancesPerFrame (a
            // hard cap) or kMaxInstanceGrowthFactor x the default when the
            // capacity came from the default. Never below InstanceCapacity.
            uint32_t InstanceCapacity;
            uint32_t InstanceCapacityMax;
            uint32_t PendingInstanceCapacity;               // 0 = no growth scheduled
            uint32_t InstanceWarningCapacity;               // capacity the overflow warning last named
            uint32_t InstanceCursor;                        // per-frame reservation cursor
            uint32_t InstanceUploaded[kMaxFramesInFlight];  // records written to the slot's GPU buffer
            uint32_t CurrentFrameIndex;                     // frame index of the last UploadPools call

            // Device limits (queried once) and the uniforms ring geometry.
            uint32_t MaxTextureDimension2D;
            uint64_t MaxStorageBufferBindingSize;
            uint32_t UniformAlignment;  // minUniformBufferOffsetAlignment, never below 4
            uint32_t UniformStride;     // bytes between two parameter records
            uint32_t UniformBatchesPerFrame;
            uint32_t UniformBatchesPerFrameMax;
            uint32_t PendingUniformBatchesPerFrame;  // 0 = no growth scheduled
            uint32_t UniformWarningCapacity;         // capacity the overflow warning last named
            uint32_t UniformCursor[kMaxFramesInFlight];
            bool PoolHeightWarningPrinted;
            bool PaintSizeWarningPrinted;

            WGPUBindGroupLayout BindGroupLayout;
            WGPUPipelineLayout PipelineLayout;
            WGPURenderPipeline Pipeline;
            WGPUBindGroup BindGroup;
            bool BindGroupDirty;  // (re)create the bind group when the pools are ready

            PoolTexture Curves;
            PoolTexture Bands;

            // Paint records (storage buffer, binding 2). Always bound (a
            // minimal buffer exists before the first color glyph), so the bind
            // group is valid for plain text too.
            WGPUBuffer Paints;
            uint64_t PaintsSize;
            bool PaintsHasData;

            // One uniform ring for every frame in flight: each frame owns the
            // region [frameIndex * UniformBatchesPerFrame * UniformStride, ...)
            // and writes its own 80 B records, so no per-slot buffer is needed
            // (the frames-in-flight contract keeps the regions apart).
            WGPUBuffer Uniforms;
            uint64_t UniformsSize;

            // CPU instance ring: FramesInFlight slots of InstanceCapacity
            // records, exposed by ImSlugWGPUGetInstanceData. Each slot has its
            // own GPU buffer (VERTEX|COPY_DST) because a single buffer would
            // overwrite in-flight frames; the adapter writes the whole staged
            // range with one queue.writeBuffer per frame.
            ImSlugGlyphInstance* InstanceCpu;
            WGPUBuffer InstanceGpu[kMaxFramesInFlight];
        };

        State g_State = {};
        bool g_ErrorReported = false;  // process-wide: report the first device error once

        // ---------------------------------------------------------------------
        // Error reporting
        // ---------------------------------------------------------------------
        // WebGPU installs device-lost/uncaptured-error callbacks on the device
        // descriptor at device-creation time, and the device belongs to the
        // caller: a module cannot attach callbacks to an existing device. The
        // closest equivalent is a validation error scope around the module's
        // own creation work, whose pop callback prints the first failure once
        // through stderr, repo style. Never aborts, never returns an error.
        void ReportDeviceErrorOnce(const char* what, WGPUErrorType type, WGPUStringView message)
        {
            if (type == WGPUErrorType_NoError || g_ErrorReported)
                return;
            g_ErrorReported = true;
            if (message.data != nullptr && message.length != WGPU_STRLEN)
            {
                fprintf(stderr, "[imslug] WebGPU %s failed (validation error): %.*s\n", what, (int)message.length,
                        message.data);
            }
            else
            {
                fprintf(stderr, "[imslug] WebGPU %s failed (validation error): %s\n", what,
                        message.data != nullptr ? message.data : "(no message)");
            }
        }

        void PopErrorScopeCallback(WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message,
                                   void* userdata1, void* userdata2)
        {
            (void)status;
            (void)userdata2;
            ReportDeviceErrorOnce((const char*)userdata1, type, message);
        }

        void PushErrorReport()
        {
            wgpuDevicePushErrorScope(g_State.Info.Device, WGPUErrorFilter_Validation);
        }

        void PopErrorReport(const char* what)
        {
            WGPUPopErrorScopeCallbackInfo info = {};
            info.mode = WGPUCallbackMode_AllowSpontaneous;
            info.callback = PopErrorScopeCallback;
            info.userdata1 = (void*)what;
            wgpuDevicePopErrorScope(g_State.Info.Device, info);
        }

        // ---------------------------------------------------------------------
        // Small helpers
        // ---------------------------------------------------------------------
        uint32_t AlignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        bool IsDepthStencilFormat(WGPUTextureFormat format)
        {
            switch (format)
            {
            case WGPUTextureFormat_Depth16Unorm:
            case WGPUTextureFormat_Depth24Plus:
            case WGPUTextureFormat_Depth24PlusStencil8:
            case WGPUTextureFormat_Depth32Float:
            case WGPUTextureFormat_Depth32FloatStencil8:
            case WGPUTextureFormat_Stencil8:
                return true;
            default:
                return false;
            }
        }

        // Init-time format validation (mirrors the Vulkan core's "can this
        // device render the pool formats" check): a renderable color target and
        // an optional depth-stencil attachment are required, and the formats
        // must be of the right kind. Deeper capability checks (blendable color
        // formats, supported sample counts) are WebGPU validation's job and are
        // reported through the error scope rather than silently ignored.
        bool ValidateInitInfo(const ImSlugWGPUInitInfo& info)
        {
            if (info.Device == nullptr || info.Queue == nullptr || info.NumFramesInFlight == 0)
                return false;
            if (info.RenderTargetFormat == WGPUTextureFormat_Undefined || IsDepthStencilFormat(info.RenderTargetFormat))
                return false;
            if (info.DepthStencilFormat != WGPUTextureFormat_Undefined &&
                !IsDepthStencilFormat(info.DepthStencilFormat))
            {
                return false;
            }
            return true;
        }

        WGPUMultisampleState NormalizeMultisampleState(WGPUMultisampleState state)
        {
            if (state.count == 0)
            {
                state.count = 1;
                state.mask = 0xFFFFFFFFu;
                state.alphaToCoverageEnabled = WGPU_FALSE;
            }
            return state;
        }

        void DestroyBuffer(WGPUBuffer* buffer)
        {
            if (*buffer != nullptr)
            {
                wgpuBufferDestroy(*buffer);
                wgpuBufferRelease(*buffer);
                *buffer = nullptr;
            }
        }

        // ---------------------------------------------------------------------
        // Instance ring
        // ---------------------------------------------------------------------
        // The CPU ring is frames_in_flight x InstanceCapacity records; each slot
        // has a matching GPU VERTEX|COPY_DST buffer the adapter fills with one
        // queue.writeBuffer per frame. A frame that needs more drops its
        // overflowing runs; when the capacity came from the default (no explicit
        // MaxInstancesPerFrame), the ring grows instead: in place when the frame
        // has not written anything yet, otherwise at the next
        // ImSlugWGPUUploadPools (one frame of dropped text at most). The old
        // resources are destroyed immediately - WebGPU keeps a resource alive
        // while submitted work references it, so no device wait and no retire
        // list are needed.
        void DestroyInstanceRing(ImSlugGlyphInstance** cpu, WGPUBuffer gpu[kMaxFramesInFlight])
        {
            if (*cpu != nullptr)
            {
                free(*cpu);
                *cpu = nullptr;
            }
            for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
                DestroyBuffer(&gpu[i]);
        }

        bool CreateInstanceRingGpu(uint32_t capacity, WGPUBuffer gpu[kMaxFramesInFlight])
        {
            WGPUBufferDescriptor desc = {};
            desc.label = { "imslug instance ring", WGPU_STRLEN };
            desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
            desc.size = (uint64_t)capacity * sizeof(ImSlugGlyphInstance);
            for (uint32_t i = 0; i < g_State.FramesInFlight; ++i)
            {
                gpu[i] = wgpuDeviceCreateBuffer(g_State.Info.Device, &desc);
                if (gpu[i] == nullptr)
                {
                    for (uint32_t j = 0; j < i; ++j)
                        DestroyBuffer(&gpu[j]);
                    return false;
                }
            }
            return true;
        }

        // Doubling, at least to `demanded`, never above the ceiling.
        uint32_t InstanceGrowthTarget(uint32_t demanded)
        {
            uint32_t target = g_State.InstanceCapacity * 2;
            if (target < demanded)
                target = demanded;
            if (target > g_State.InstanceCapacityMax)
                target = g_State.InstanceCapacityMax;
            return target;
        }

        // Replaces the ring with a `newCapacity`-records-per-frame ring. Returns
        // false (keeping the current ring) when it cannot be created; the
        // callers then keep dropping runs.
        bool GrowInstanceRing(uint32_t newCapacity)
        {
            IMSLUG_ASSERT(g_State.Initialized);
            IMSLUG_ASSERT(newCapacity > g_State.InstanceCapacity);
            IMSLUG_ASSERT(newCapacity <= g_State.InstanceCapacityMax);
            if (newCapacity <= g_State.InstanceCapacity || newCapacity > g_State.InstanceCapacityMax)
                return false;

            const uint64_t cpuBytes = (uint64_t)g_State.FramesInFlight * newCapacity * sizeof(ImSlugGlyphInstance);
            ImSlugGlyphInstance* cpu = (ImSlugGlyphInstance*)malloc((size_t)cpuBytes);
            if (cpu == nullptr)
                return false;
            WGPUBuffer gpu[kMaxFramesInFlight] = {};
            if (!CreateInstanceRingGpu(newCapacity, gpu))
            {
                free(cpu);
                return false;
            }

            DestroyInstanceRing(&g_State.InstanceCpu, g_State.InstanceGpu);
            g_State.InstanceCpu = cpu;
            for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
                g_State.InstanceGpu[i] = gpu[i];
            for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
                g_State.InstanceUploaded[i] = 0;

            const uint32_t previous = g_State.InstanceCapacity;
            g_State.InstanceCapacity = newCapacity;
            fprintf(stderr, "[imslug] Slug instance ring grown from %u to %u instances/frame.\n", previous,
                    newCapacity);
            return true;
        }

        void WarnInstanceRingFull(uint32_t count, uint32_t growthTarget)
        {
            const uint32_t demand = g_State.InstanceCursor + count;
            if (g_State.InstanceWarningCapacity == g_State.InstanceCapacity)
                return;
            g_State.InstanceWarningCapacity = g_State.InstanceCapacity;
            if (growthTarget > 0)
            {
                fprintf(stderr,
                        "[imslug] Slug instance ring full: a text run of %u instances did not fit (frame "
                        "demand %u, capacity %u); the run is dropped. The ring grows to %u for the next "
                        "frame; raise MaxInstancesPerFrame (Config::MaxInstancesPerFrame / "
                        "ImSlugWGPUInitInfo::MaxInstancesPerFrame) to set a fixed capacity.\n",
                        count, demand, g_State.InstanceCapacity, growthTarget);
            }
            else
            {
                fprintf(stderr,
                        "[imslug] Slug instance ring full: a text run of %u instances did not fit (frame "
                        "demand %u, capacity %u); the run is dropped. Raise the configured "
                        "MaxInstancesPerFrame (Config::MaxInstancesPerFrame / "
                        "ImSlugWGPUInitInfo::MaxInstancesPerFrame) to fit the frame's glyph count.\n",
                        count, demand, g_State.InstanceCapacity);
            }
        }

        // ---------------------------------------------------------------------
        // Uniform ring (the push-constant replacement)
        // ---------------------------------------------------------------------
        // The record is padded to a multiple of minUniformBufferOffsetAlignment
        // because dynamic offsets must be aligned: stride = max(80, alignment)
        // rounded up to the alignment (with the WebGPU default of 256 this is
        // exactly 256).
        uint32_t UniformStrideFor(uint32_t alignment)
        {
            const uint32_t stride = alignment > sizeof(UniformParams) ? alignment : (uint32_t)sizeof(UniformParams);
            return AlignUp(stride, alignment);
        }

        bool CreateUniformRing(uint32_t batchesPerFrame)
        {
            const uint64_t bytes = (uint64_t)g_State.FramesInFlight * batchesPerFrame * g_State.UniformStride;
            WGPUBufferDescriptor desc = {};
            desc.label = { "imslug uniform ring", WGPU_STRLEN };
            desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            desc.size = bytes;
            WGPUBuffer buffer = wgpuDeviceCreateBuffer(g_State.Info.Device, &desc);
            if (buffer == nullptr)
                return false;

            DestroyBuffer(&g_State.Uniforms);
            g_State.Uniforms = buffer;
            g_State.UniformsSize = bytes;
            g_State.UniformBatchesPerFrame = batchesPerFrame;
            for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
                g_State.UniformCursor[i] = 0;
            g_State.BindGroupDirty = true;  // the bind group references the buffer
            return true;
        }

        uint32_t UniformGrowthTarget(uint32_t demanded)
        {
            uint32_t target = g_State.UniformBatchesPerFrame * 2;
            if (target < demanded)
                target = demanded;
            if (target > g_State.UniformBatchesPerFrameMax)
                target = g_State.UniformBatchesPerFrameMax;
            return target;
        }

        // A batch that does not fit the ring is dropped (it cannot be recorded:
        // the already-encoded batches of this frame bind the current buffer).
        // The ring grows for the next frame while the capacity came from the
        // default, mirroring the instance ring's overflow policy.
        void WarnUniformRingFull(uint32_t frameIndex)
        {
            const uint32_t demand = g_State.UniformCursor[frameIndex] + 1;
            uint32_t growthTarget = 0;
            if (g_State.UniformBatchesPerFrameMax > g_State.UniformBatchesPerFrame)
            {
                growthTarget = UniformGrowthTarget(demand);
                if (growthTarget > g_State.PendingUniformBatchesPerFrame)
                    g_State.PendingUniformBatchesPerFrame = growthTarget;
            }
            if (g_State.UniformWarningCapacity == g_State.UniformBatchesPerFrame)
                return;
            g_State.UniformWarningCapacity = g_State.UniformBatchesPerFrame;
            if (growthTarget > 0)
            {
                fprintf(stderr,
                        "[imslug] WebGPU uniform ring full: a Slug text run did not fit (frame batch demand "
                        "%u, capacity %u batches/frame); the run is dropped. The ring grows to %u for the "
                        "next frame (every Slug text run in its own draw command is one batch).\n",
                        demand, g_State.UniformBatchesPerFrame, growthTarget);
            }
            else
            {
                fprintf(stderr,
                        "[imslug] WebGPU uniform ring full: a Slug text run did not fit (frame batch demand "
                        "%u, capacity %u batches/frame, the maximum); the run is dropped. Merge text draws "
                        "(same font, size and clip) so one draw command carries more of the text.\n",
                        demand, g_State.UniformBatchesPerFrame);
            }
        }

        // ---------------------------------------------------------------------
        // Pool textures
        // ---------------------------------------------------------------------
        void DestroyPoolTexture(PoolTexture* texture)
        {
            if (texture->View != nullptr)
            {
                wgpuTextureViewRelease(texture->View);
                texture->View = nullptr;
            }
            if (texture->Texture != nullptr)
            {
                wgpuTextureDestroy(texture->Texture);
                wgpuTextureRelease(texture->Texture);
                texture->Texture = nullptr;
            }
            texture->Width = 0;
            texture->Height = 0;
            texture->HasData = false;
        }

        // Creates the texture at the requested size, or recreates it after pool
        // growth. `changed` reports a (re)creation so the bind group can be
        // rebuilt. The old texture is destroyed immediately: WebGPU keeps a
        // texture whose reads are in flight alive by itself (growth is rare).
        bool EnsurePoolTexture(PoolTexture* texture, WGPUTextureFormat format, uint32_t texelBytes, int width,
                               int height, bool* changed)
        {
            *changed = false;
            if (texture->Texture != nullptr && texture->Width == width && texture->Height == height)
                return true;
            if (width <= 0 || height <= 0)
                return true;  // nothing to create yet
            if ((uint32_t)height > g_State.MaxTextureDimension2D)
            {
                IMSLUG_ASSERT(false);  // the caller warns once and disables Slug for the frame
                return false;
            }

            DestroyPoolTexture(texture);
            // The bind group referenced the old view: mark it for a rebuild even
            // if a later step of this frame fails (the view handle is gone).
            g_State.BindGroupDirty = true;

            WGPUTextureDescriptor desc = {};
            desc.label = { "imslug pool", WGPU_STRLEN };
            desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            desc.dimension = WGPUTextureDimension_2D;
            desc.size = { (uint32_t)width, (uint32_t)height, 1 };
            desc.format = format;
            desc.mipLevelCount = 1;
            desc.sampleCount = 1;
            texture->Texture = wgpuDeviceCreateTexture(g_State.Info.Device, &desc);
            if (texture->Texture == nullptr)
            {
                // The old view is gone and no new one exists: never leave a
                // bind group behind whose texture view was destroyed (Draw's
                // non-null guard would bind it).
                if (g_State.BindGroup != nullptr)
                {
                    wgpuBindGroupRelease(g_State.BindGroup);
                    g_State.BindGroup = nullptr;
                }
                return false;
            }
            // Explicit view descriptor (mirrors imgui_impl_wgpu's
            // wgpuTextureCreateView call rather than relying on null-descriptor
            // defaults in the JS glue).
            WGPUTextureViewDescriptor viewDesc = {};
            viewDesc.format = format;
            viewDesc.dimension = WGPUTextureViewDimension_2D;
            viewDesc.baseMipLevel = 0;
            viewDesc.mipLevelCount = 1;
            viewDesc.baseArrayLayer = 0;
            viewDesc.arrayLayerCount = 1;
            viewDesc.aspect = WGPUTextureAspect_All;
            texture->View = wgpuTextureCreateView(texture->Texture, &viewDesc);
            if (texture->View == nullptr)
            {
                DestroyPoolTexture(texture);
                // The bind group referenced the old view, which the recreate
                // above destroyed: release it (BindGroupDirty is already set)
                // so Draw cannot bind a group with a dead view.
                if (g_State.BindGroup != nullptr)
                {
                    wgpuBindGroupRelease(g_State.BindGroup);
                    g_State.BindGroup = nullptr;
                }
                return false;
            }

            texture->Format = format;
            texture->TexelBytes = texelBytes;
            texture->Width = width;
            texture->Height = height;
            texture->HasData = false;
            *changed = true;
            return true;
        }

        // ---------------------------------------------------------------------
        // Bind group
        // ---------------------------------------------------------------------
        // Binding 0: curve pool (RGBA16F) - binding 1: band pool (RG16_UINT) -
        // binding 2: paint records (read-only storage) - binding 3: per-draw
        // parameters (uniform, dynamic offset). No samplers: the shaders use
        // textureLoad. The group exists only once both pool views exist (a bind
        // group entry needs a valid view), which is this module's
        // TexturesCreated equivalent.
        void WriteBindGroup()
        {
            IMSLUG_ASSERT(g_State.Curves.View != nullptr && g_State.Bands.View != nullptr);
            IMSLUG_ASSERT(g_State.Paints != nullptr && g_State.Uniforms != nullptr);

            WGPUBindGroupEntry entries[4] = {};
            entries[0].binding = 0;
            entries[0].textureView = g_State.Curves.View;
            entries[1].binding = 1;
            entries[1].textureView = g_State.Bands.View;
            entries[2].binding = 2;
            entries[2].buffer = g_State.Paints;
            entries[2].offset = 0;
            entries[2].size = g_State.PaintsSize;
            entries[3].binding = 3;
            entries[3].buffer = g_State.Uniforms;
            entries[3].offset = 0;
            entries[3].size = sizeof(UniformParams);

            WGPUBindGroupDescriptor desc = {};
            desc.layout = g_State.BindGroupLayout;
            desc.entryCount = 4;
            desc.entries = entries;

            if (g_State.BindGroup != nullptr)
            {
                wgpuBindGroupRelease(g_State.BindGroup);
                g_State.BindGroup = nullptr;
            }
            g_State.BindGroup = wgpuDeviceCreateBindGroup(g_State.Info.Device, &desc);
            g_State.BindGroupDirty = g_State.BindGroup == nullptr;
        }

        // ---------------------------------------------------------------------
        // Pipeline
        // ---------------------------------------------------------------------
        WGPUShaderModule CreateShaderModule(const char* wgsl, const char* label)
        {
            WGPUShaderSourceWGSL source = {};
            source.chain.sType = WGPUSType_ShaderSourceWGSL;
            source.code = { wgsl, WGPU_STRLEN };

            WGPUShaderModuleDescriptor desc = {};
            desc.label = { label, WGPU_STRLEN };
            desc.nextInChain = (WGPUChainedStruct*)&source;
            return wgpuDeviceCreateShaderModule(g_State.Info.Device, &desc);
        }

        bool CreatePipeline()
        {
            WGPUShaderModule vertModule = CreateShaderModule(imslug_slug_vert_wgsl, "imslug slug.vert");
            WGPUShaderModule fragModule = CreateShaderModule(imslug_slug_frag_wgsl, "imslug slug.frag");
            if (vertModule == nullptr || fragModule == nullptr)
            {
                if (vertModule != nullptr)
                    wgpuShaderModuleRelease(vertModule);
                if (fragModule != nullptr)
                    wgpuShaderModuleRelease(fragModule);
                return false;
            }

            // Instance-rate input only: the quad comes from @builtin(vertex_index).
            // The layout mirrors ImSlugGlyphInstance exactly (static_assert'ed in
            // imslug_internal.h) and the WGSL @location bindings in slug.vert.wgsl.
            WGPUVertexAttribute attributes[8] = {
                { nullptr, WGPUVertexFormat_Float32x4, offsetof(ImSlugGlyphInstance, EmMinX), 0 },
                { nullptr, WGPUVertexFormat_Float32x4, offsetof(ImSlugGlyphInstance, ScreenMinX), 1 },
                { nullptr, WGPUVertexFormat_Float32x4, offsetof(ImSlugGlyphInstance, Jac00), 2 },
                { nullptr, WGPUVertexFormat_Float32x4, offsetof(ImSlugGlyphInstance, BandScaleX), 3 },
                { nullptr, WGPUVertexFormat_Uint32x2, offsetof(ImSlugGlyphInstance, GlyphLocX), 4 },
                { nullptr, WGPUVertexFormat_Uint32, offsetof(ImSlugGlyphInstance, BandMaxWord), 5 },
                { nullptr, WGPUVertexFormat_Float32x4, offsetof(ImSlugGlyphInstance, ColorR), 6 },
                { nullptr, WGPUVertexFormat_Uint32, offsetof(ImSlugGlyphInstance, PaintIndex), 7 },
            };
            WGPUVertexBufferLayout vertexBuffer = {};
            vertexBuffer.stepMode = WGPUVertexStepMode_Instance;
            vertexBuffer.arrayStride = sizeof(ImSlugGlyphInstance);
            vertexBuffer.attributeCount = 8;
            vertexBuffer.attributes = attributes;

            WGPURenderPipelineDescriptor desc = {};
            desc.label = { "imslug slug pipeline", WGPU_STRLEN };
            desc.layout = g_State.PipelineLayout;
            desc.vertex.module = vertModule;
            desc.vertex.entryPoint = { "main", WGPU_STRLEN };
            desc.vertex.bufferCount = 1;
            desc.vertex.buffers = &vertexBuffer;
            desc.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
            desc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
            desc.primitive.frontFace = WGPUFrontFace_CCW;
            desc.primitive.cullMode = WGPUCullMode_None;
            desc.multisample = g_State.Info.MultisampleState;

            // Depth-stencil mirrors imgui_impl_wgpu's disabled state, so the
            // pipeline stays compatible with a pass that carries a depth
            // attachment of the same format (Slug draws share ImGui's pass).
            WGPUDepthStencilState depthStencil = {};
            depthStencil.format = g_State.Info.DepthStencilFormat;
            depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
            depthStencil.depthCompare = WGPUCompareFunction_Always;
            depthStencil.stencilFront.compare = WGPUCompareFunction_Always;
            depthStencil.stencilFront.failOp = WGPUStencilOperation_Keep;
            depthStencil.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
            depthStencil.stencilFront.passOp = WGPUStencilOperation_Keep;
            depthStencil.stencilBack = depthStencil.stencilFront;
            if (g_State.Info.DepthStencilFormat != WGPUTextureFormat_Undefined)
                desc.depthStencil = &depthStencil;

            // Blend mirrors imgui_impl_wgpu.cpp exactly, so Slug runs composite
            // like the ImGui draws around them:
            // color: src alpha / one-minus-src-alpha; alpha: one / one-minus-src-alpha.
            WGPUBlendState blend = {};
            blend.color.operation = WGPUBlendOperation_Add;
            blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
            blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            blend.alpha.operation = WGPUBlendOperation_Add;
            blend.alpha.srcFactor = WGPUBlendFactor_One;
            blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

            WGPUColorTargetState target = {};
            target.format = g_State.Info.RenderTargetFormat;
            target.blend = &blend;
            target.writeMask = WGPUColorWriteMask_All;

            WGPUFragmentState fragment = {};
            fragment.module = fragModule;
            fragment.entryPoint = { "main", WGPU_STRLEN };
            fragment.targetCount = 1;
            fragment.targets = &target;
            desc.fragment = &fragment;

            g_State.Pipeline = wgpuDeviceCreateRenderPipeline(g_State.Info.Device, &desc);

            // The pipeline holds its own references to the modules.
            wgpuShaderModuleRelease(vertModule);
            wgpuShaderModuleRelease(fragModule);
            return g_State.Pipeline != nullptr;
        }

        // ---------------------------------------------------------------------
        // Uploads
        // ---------------------------------------------------------------------
        struct UploadRange
        {
            bool Valid;
            int RowStart;
            int RowEnd;
        };

        UploadRange ComputeUploadRange(const PoolTexture& texture, bool firstUpload, int usedRows, int dirtyMin,
                                       int dirtyMax)
        {
            UploadRange range = {};
            if (texture.Texture == nullptr)
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

            if (range.RowEnd >= texture.Height)
                range.RowEnd = texture.Height - 1;
            range.Valid = range.RowEnd >= range.RowStart;
            return range;
        }

        // One queue.writeTexture for a contiguous row range of `texels` (u16
        // components, tightly packed rows). queue.writeTexture has no
        // bytesPerRow alignment requirement (the 256-byte rule belongs to
        // buffer<->texture copy commands, which this module does not use); the
        // pool widths are 4096 texels, so bytesPerRow is a multiple of 256 for
        // both formats anyway.
        void UploadPoolRows(PoolTexture* texture, const uint16_t* texels, const UploadRange& range)
        {
            const uint32_t rows = (uint32_t)(range.RowEnd - range.RowStart + 1);
            const uint32_t bytesPerRow = (uint32_t)texture->Width * texture->TexelBytes;

            WGPUTexelCopyTextureInfo destination = {};
            destination.texture = texture->Texture;
            destination.mipLevel = 0;
            destination.origin = { 0, (uint32_t)range.RowStart, 0 };
            destination.aspect = WGPUTextureAspect_All;

            WGPUTexelCopyBufferLayout layout = {};
            layout.offset = 0;
            layout.bytesPerRow = bytesPerRow;
            layout.rowsPerImage = rows;

            WGPUExtent3D size = { (uint32_t)texture->Width, rows, 1 };
            const uint8_t* data = (const uint8_t*)texels + (size_t)range.RowStart * bytesPerRow;
            wgpuQueueWriteTexture(g_State.Info.Queue, &destination, data, (size_t)rows * bytesPerRow, &layout, &size);
            texture->HasData = true;
        }

        // ---------------------------------------------------------------------
        // Destruction
        // ---------------------------------------------------------------------
        // Destroys every object the module owns. Used by Shutdown and by the
        // Init failure paths (nothing was submitted yet, so no wait is needed).
        // Null handles are tolerated; Info.Device must be set.
        void DestroyDeviceObjects()
        {
            if (g_State.BindGroup != nullptr)
                wgpuBindGroupRelease(g_State.BindGroup);
            if (g_State.Pipeline != nullptr)
                wgpuRenderPipelineRelease(g_State.Pipeline);
            if (g_State.PipelineLayout != nullptr)
                wgpuPipelineLayoutRelease(g_State.PipelineLayout);
            if (g_State.BindGroupLayout != nullptr)
                wgpuBindGroupLayoutRelease(g_State.BindGroupLayout);
            g_State.BindGroup = nullptr;
            g_State.Pipeline = nullptr;
            g_State.PipelineLayout = nullptr;
            g_State.BindGroupLayout = nullptr;

            DestroyPoolTexture(&g_State.Curves);
            DestroyPoolTexture(&g_State.Bands);
            DestroyBuffer(&g_State.Paints);
            DestroyBuffer(&g_State.Uniforms);
            DestroyInstanceRing(&g_State.InstanceCpu, g_State.InstanceGpu);
        }

        // Init failure path: frees anything already created and clears the
        // state, so a failed Init leaves nothing behind and Shutdown()/a later
        // Init() start clean.
        bool FailInit()
        {
            DestroyDeviceObjects();
            memset(&g_State, 0, sizeof(g_State));
            return false;
        }

        // Creates the ring+uniform+paint buffers in one place so Init can keep
        // the error scope balanced when a creation fails.
        bool CreateFixedBuffers(uint32_t instanceCapacity, uint32_t batchesPerFrame)
        {
            const uint64_t cpuBytes = (uint64_t)g_State.FramesInFlight * instanceCapacity * sizeof(ImSlugGlyphInstance);
            g_State.InstanceCpu = (ImSlugGlyphInstance*)malloc((size_t)cpuBytes);
            if (g_State.InstanceCpu == nullptr)
                return false;
            WGPUBuffer gpu[kMaxFramesInFlight] = {};
            if (!CreateInstanceRingGpu(instanceCapacity, gpu))
                return false;
            for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
                g_State.InstanceGpu[i] = gpu[i];

            if (!CreateUniformRing(batchesPerFrame))
                return false;

            // The paint storage buffer always exists (at least one record) so
            // binding 2 is valid for plain text as well.
            WGPUBufferDescriptor desc = {};
            desc.label = { "imslug paint records", WGPU_STRLEN };
            desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
            desc.size = sizeof(ImSlugPaintRecord);
            g_State.Paints = wgpuDeviceCreateBuffer(g_State.Info.Device, &desc);
            if (g_State.Paints == nullptr)
                return false;
            g_State.PaintsSize = sizeof(ImSlugPaintRecord);
            g_State.PaintsHasData = false;
            return true;
        }
    }  // namespace

    // ---------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------
    bool ImSlugWGPUInit(const ImSlugWGPUInitInfo* initInfo)
    {
        IMSLUG_ASSERT(initInfo != nullptr);
        IMSLUG_ASSERT(!g_State.Initialized);
        if (initInfo == nullptr || g_State.Initialized)
            return false;
        if (!ValidateInitInfo(*initInfo))
            return false;

        memset(&g_State, 0, sizeof(g_State));
        g_State.Info = *initInfo;
        g_State.Info.MultisampleState = NormalizeMultisampleState(initInfo->MultisampleState);
        g_State.FramesInFlight =
            initInfo->NumFramesInFlight < kMaxFramesInFlight ? initInfo->NumFramesInFlight : kMaxFramesInFlight;
        g_State.InstanceCapacity =
            initInfo->MaxInstancesPerFrame != 0 ? initInfo->MaxInstancesPerFrame : kDefaultMaxInstancesPerFrame;
        // An explicit MaxInstancesPerFrame is a hard cap: overflow keeps
        // dropping runs (one-time stderr warning). The default capacity grows
        // up to kMaxInstanceGrowthFactor times the default.
        g_State.InstanceCapacityMax = initInfo->MaxInstancesPerFrame != 0
                                          ? initInfo->MaxInstancesPerFrame
                                          : kDefaultMaxInstancesPerFrame * kMaxInstanceGrowthFactor;
        g_State.UniformBatchesPerFrame = kDefaultMaxBatchesPerFrame;
        g_State.UniformBatchesPerFrameMax = kDefaultMaxBatchesPerFrame * kMaxBatchGrowthFactor;
        g_State.BindGroupDirty = true;

        // Device limits (the WebGPU spec defaults are the documented fallbacks
        // for a device that does not report them; the implementation may not
        // have to, e.g. before the device is ready).
        WGPULimits limits = {};
        const bool haveLimits = wgpuDeviceGetLimits(initInfo->Device, &limits) == WGPUStatus_Success;
        g_State.MaxTextureDimension2D = haveLimits && limits.maxTextureDimension2D != 0 ? limits.maxTextureDimension2D
                                                                                        : kDefaultMaxTextureDimension2D;
        g_State.MaxStorageBufferBindingSize = haveLimits && limits.maxStorageBufferBindingSize != 0
                                                  ? limits.maxStorageBufferBindingSize
                                                  : kDefaultMaxStorageBufferBindingSize;
        g_State.UniformAlignment = haveLimits && limits.minUniformBufferOffsetAlignment != 0
                                       ? limits.minUniformBufferOffsetAlignment
                                       : kDefaultUniformBufferOffsetAlignment;
        if (g_State.UniformAlignment < 4)
            g_State.UniformAlignment = 4;
        g_State.UniformStride = UniformStrideFor(g_State.UniformAlignment);

        // The pools are 4096 texels wide and at most 65536 rows by design, and
        // the paint records must fit one storage buffer binding.
        if (g_State.MaxTextureDimension2D < (uint32_t)kPoolWidth)
            return FailInit();
        if (g_State.MaxStorageBufferBindingSize < sizeof(ImSlugPaintRecord))
            return FailInit();

        PushErrorReport();

        bool ok = true;

        // Bind group layout: curve + band textures, paint storage, dynamic
        // uniform (see WriteBindGroup for the binding contract).
        if (ok)
        {
            WGPUBindGroupLayoutEntry entries[4] = {};
            entries[0].binding = 0;
            entries[0].visibility = WGPUShaderStage_Fragment;
            entries[0].texture.sampleType = WGPUTextureSampleType_Float;
            entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
            entries[1].binding = 1;
            entries[1].visibility = WGPUShaderStage_Fragment;
            entries[1].texture.sampleType = WGPUTextureSampleType_Uint;
            entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
            entries[2].binding = 2;
            entries[2].visibility = WGPUShaderStage_Fragment;
            entries[2].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
            entries[3].binding = 3;
            entries[3].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            entries[3].buffer.type = WGPUBufferBindingType_Uniform;
            entries[3].buffer.hasDynamicOffset = WGPU_TRUE;

            WGPUBindGroupLayoutDescriptor desc = {};
            desc.label = { "imslug bind group layout", WGPU_STRLEN };
            desc.entryCount = 4;
            desc.entries = entries;
            g_State.BindGroupLayout = wgpuDeviceCreateBindGroupLayout(initInfo->Device, &desc);
            ok = g_State.BindGroupLayout != nullptr;
        }
        if (ok)
        {
            WGPUPipelineLayoutDescriptor desc = {};
            desc.label = { "imslug pipeline layout", WGPU_STRLEN };
            desc.bindGroupLayoutCount = 1;
            desc.bindGroupLayouts = &g_State.BindGroupLayout;
            g_State.PipelineLayout = wgpuDeviceCreatePipelineLayout(initInfo->Device, &desc);
            ok = g_State.PipelineLayout != nullptr;
        }

        // Rings and the always-present paint buffer.
        if (ok)
            ok = CreateFixedBuffers(g_State.InstanceCapacity, g_State.UniformBatchesPerFrame);

        // Pipeline (also validates that both WGSL modules compile and that the
        // layout matches the shaders' resource usage).
        if (ok)
            ok = CreatePipeline();

        PopErrorReport("ImSlugWGPUInit");
        if (!ok)
            return FailInit();

        g_State.Initialized = true;
        return true;
    }

    void ImSlugWGPUShutdown()
    {
        if (!g_State.Initialized)
            return;

        DestroyDeviceObjects();
        memset(&g_State, 0, sizeof(g_State));
    }

    bool ImSlugWGPUUploadPools(ImSlugPools* pools, uint32_t frameIndex)
    {
        IMSLUG_ASSERT(g_State.Initialized);
        IMSLUG_ASSERT(frameIndex < g_State.FramesInFlight);
        if (!g_State.Initialized || pools == nullptr || frameIndex >= g_State.FramesInFlight)
            return false;

        // The growth and (re)creation work of this frame runs inside the same
        // validation error scope as Init, so a late creation failure (size
        // limits, allocation) is reported once through stderr instead of only
        // to the app's device callbacks. Every exit path below pops the scope,
        // keeping the device's error-scope stack balanced.
        PushErrorReport();

        // Instance/uniform bookkeeping (both are no-ops in steady state):
        // apply the growth a previous frame requested, before anything of this
        // frame is written; then start the frame's cursors.
        if (g_State.PendingInstanceCapacity > g_State.InstanceCapacity)
        {
            if (GrowInstanceRing(g_State.PendingInstanceCapacity))
                g_State.PendingInstanceCapacity = 0;
        }
        if (g_State.PendingUniformBatchesPerFrame > g_State.UniformBatchesPerFrame)
        {
            if (CreateUniformRing(g_State.PendingUniformBatchesPerFrame))
                g_State.PendingUniformBatchesPerFrame = 0;
        }
        g_State.CurrentFrameIndex = frameIndex;
        g_State.InstanceCursor = 0;
        g_State.InstanceUploaded[frameIndex] = 0;
        g_State.UniformCursor[frameIndex] = 0;

        // A pool beyond maxTextureDimension2D cannot be uploaded: warn once and
        // leave the frame alone (nothing uploaded, dirty rows kept) - the same
        // guard-and-fallback shape as the Vulkan module's image-dimension check,
        // and never a half-uploaded pair.
        const int curveRows = pools->Curves.Height;
        const int bandRows = pools->Bands.Height;
        if ((curveRows > 0 && (uint32_t)curveRows > g_State.MaxTextureDimension2D) ||
            (bandRows > 0 && (uint32_t)bandRows > g_State.MaxTextureDimension2D))
        {
            IMSLUG_ASSERT(false);
            if (!g_State.PoolHeightWarningPrinted)
            {
                g_State.PoolHeightWarningPrinted = true;
                fprintf(stderr,
                        "[imslug] WebGPU glyph pool height limit reached: the pools need %d/%d rows but the device "
                        "allows %u (maxTextureDimension2D); Slug text is disabled. Reduce the number of baked glyphs "
                        "(Config::MaxGlyphSlabs) or the font bake size.\n",
                        curveRows, bandRows, g_State.MaxTextureDimension2D);
            }
            PopErrorReport("ImSlugWGPUUploadPools");
            return false;
        }

        bool curveChanged = false;
        bool bandChanged = false;
        if (curveRows > 0 &&
            !EnsurePoolTexture(&g_State.Curves, WGPUTextureFormat_RGBA16Float, 8, kPoolWidth, curveRows, &curveChanged))
        {
            PopErrorReport("ImSlugWGPUUploadPools");
            return false;
        }
        if (bandRows > 0 &&
            !EnsurePoolTexture(&g_State.Bands, WGPUTextureFormat_RG16Uint, 4, kPoolWidth, bandRows, &bandChanged))
        {
            PopErrorReport("ImSlugWGPUUploadPools");
            return false;
        }

        // Paint records (storage buffer). The buffer grows with the pool; the
        // first upload covers every record, later frames only the dirty ones.
        // maxStorageBufferBindingSize bounds the binding: clamp the buffer (and
        // with it the uploaded range) and warn once.
        const uint64_t paintBytes = (uint64_t)pools->Paints.Count * sizeof(ImSlugPaintRecord);
        bool paintChanged = false;
        if (paintBytes > g_State.PaintsSize)
        {
            uint64_t newSize = paintBytes < kMinPaintBufferBytes ? kMinPaintBufferBytes : paintBytes;
            if (newSize > g_State.MaxStorageBufferBindingSize)
            {
                // WebGPU requires buffer sizes and binding sizes to be multiples
                // of 4; keep the clamp legal. The upload range is clamped to the
                // same limit below, so a permanently over-limit pool still
                // uploads everything that fits.
                newSize = g_State.MaxStorageBufferBindingSize & ~(uint64_t)3;
                if (!g_State.PaintSizeWarningPrinted)
                {
                    g_State.PaintSizeWarningPrinted = true;
                    fprintf(stderr,
                            "[imslug] WebGPU paint pool exceeds maxStorageBufferBindingSize: %llu bytes needed, "
                            "%llu allowed; layers whose paint records fall beyond the device limit are not drawn. "
                            "Reduce the baked color-glyph set (fewer COLR layers; Config::MaxGlyphSlabs) or set "
                            "Config::EnableColorFonts = false.\n",
                            (unsigned long long)paintBytes, (unsigned long long)g_State.MaxStorageBufferBindingSize);
                }
            }
            if (newSize > g_State.PaintsSize)
            {
                WGPUBufferDescriptor desc = {};
                desc.label = { "imslug paint records", WGPU_STRLEN };
                desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
                desc.size = newSize;
                WGPUBuffer grown = wgpuDeviceCreateBuffer(g_State.Info.Device, &desc);
                if (grown == nullptr)
                {
                    PopErrorReport("ImSlugWGPUUploadPools");
                    return false;
                }
                DestroyBuffer(&g_State.Paints);
                g_State.Paints = grown;
                g_State.PaintsSize = newSize;
                g_State.PaintsHasData = false;
                paintChanged = true;
            }
        }
        // The bind group points at the pool views, the paint buffer and the
        // uniform ring: (re)create it when any of them changed, as soon as both
        // views exist (the TexturesCreated equivalent).
        const bool curveReady = g_State.Curves.View != nullptr;
        const bool bandReady = g_State.Bands.View != nullptr;
        if (curveChanged || bandChanged || paintChanged)
            g_State.BindGroupDirty = true;
        if (g_State.BindGroupDirty && curveReady && bandReady)
            WriteBindGroup();

        PopErrorReport("ImSlugWGPUUploadPools");

        // Per-pool ranges: a pool without a texture has nothing to consume and
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

        int paintFirst = 0;
        int paintLast = -1;
        const int paintMaxRecords = (int)(g_State.PaintsSize / sizeof(ImSlugPaintRecord));
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
        if (paintLast >= paintMaxRecords)
            paintLast = paintMaxRecords - 1;  // the storage-buffer clamp
        const bool paintValid = paintLast >= paintFirst && pools->Paints.Bytes != nullptr;
        const uint64_t paintUploadBytes =
            paintValid ? (uint64_t)(paintLast - paintFirst + 1) * sizeof(ImSlugPaintRecord) : 0;

        if (curveRange.Valid)
        {
            UploadPoolRows(&g_State.Curves, pools->Curves.Texels, curveRange);
            ImSlugCurvePoolClearDirtyRows(&pools->Curves);
        }
        if (bandRange.Valid)
        {
            UploadPoolRows(&g_State.Bands, pools->Bands.Texels, bandRange);
            ImSlugBandPoolClearDirtyRows(&pools->Bands);
        }
        if (paintValid)
        {
            wgpuQueueWriteBuffer(g_State.Info.Queue, g_State.Paints, (uint64_t)paintFirst * sizeof(ImSlugPaintRecord),
                                 pools->Paints.Bytes + (size_t)paintFirst * sizeof(ImSlugPaintRecord),
                                 (size_t)paintUploadBytes);
            g_State.PaintsHasData = true;
            ImSlugPaintPoolClearDirtyRecords(&pools->Paints);
        }
        return true;
    }

    uint32_t ImSlugWGPUGetFramesInFlight()
    {
        return g_State.FramesInFlight;
    }

    uint32_t ImSlugWGPUGetInstanceCapacity()
    {
        return g_State.InstanceCapacity;
    }

    ImSlugGlyphInstance* ImSlugWGPUGetInstanceData(uint32_t frameIndex)
    {
        IMSLUG_ASSERT(g_State.Initialized);
        IMSLUG_ASSERT(frameIndex < g_State.FramesInFlight);
        if (!g_State.Initialized || frameIndex >= g_State.FramesInFlight || g_State.InstanceCpu == nullptr)
            return nullptr;
        return g_State.InstanceCpu + (size_t)frameIndex * g_State.InstanceCapacity;
    }

    ImSlugGlyphInstance* ImSlugWGPUReserveInstances(uint32_t count, uint32_t* outFirstInstance)
    {
        IMSLUG_ASSERT(g_State.Initialized);
        IMSLUG_ASSERT(outFirstInstance != nullptr);
        if (!g_State.Initialized || outFirstInstance == nullptr || count == 0)
            return nullptr;
        IMSLUG_ASSERT(g_State.InstanceCursor <= g_State.InstanceCapacity);

        if (count > g_State.InstanceCapacity - g_State.InstanceCursor)
        {
            // Overflow. Growing in place is safe only while this frame has not
            // reserved anything yet: no batch of the frame points into the
            // current ring (draws are recorded later, and the instance upload
            // happens after the pre-pass), and in-flight frames keep the old
            // GPU buffers alive by themselves.
            if (g_State.InstanceCursor == 0 && g_State.InstanceCapacityMax > g_State.InstanceCapacity)
                GrowInstanceRing(InstanceGrowthTarget(g_State.InstanceCursor + count));

            if (count > g_State.InstanceCapacity - g_State.InstanceCursor)
            {
                // Still does not fit: the run is dropped. Warn once per
                // capacity (so a fixed cap warns once, growth once per step)
                // and schedule growth for the next frame when the capacity came
                // from the default; an explicit MaxInstancesPerFrame is a hard
                // cap and never grows.
                uint32_t growthTarget = 0;
                if (g_State.InstanceCapacityMax > g_State.InstanceCapacity)
                    growthTarget = InstanceGrowthTarget(g_State.InstanceCursor + count);
                if (growthTarget > g_State.PendingInstanceCapacity)
                    g_State.PendingInstanceCapacity = growthTarget;
                WarnInstanceRingFull(count, growthTarget);
                return nullptr;
            }
        }

        ImSlugGlyphInstance* base = ImSlugWGPUGetInstanceData(g_State.CurrentFrameIndex);
        if (base == nullptr)
            return nullptr;
        const uint32_t firstInstance = g_State.InstanceCursor;
        g_State.InstanceCursor += count;
        *outFirstInstance = firstInstance;
        return base + firstInstance;
    }

    bool ImSlugWGPUUploadInstances(uint32_t frameIndex, uint32_t instanceCount)
    {
        IMSLUG_ASSERT(g_State.Initialized);
        IMSLUG_ASSERT(frameIndex < g_State.FramesInFlight);
        if (!g_State.Initialized || frameIndex >= g_State.FramesInFlight)
            return false;
        IMSLUG_ASSERT(instanceCount <= g_State.InstanceCapacity);
        if (instanceCount > g_State.InstanceCapacity)
            instanceCount = g_State.InstanceCapacity;

        // A zero count must not leave a previous frame's range drawable; it
        // needs no buffer, so clear the slot and return.
        if (instanceCount == 0)
        {
            g_State.InstanceUploaded[frameIndex] = 0;
            return true;
        }
        // The null check comes before the slot is marked uploaded: a missing
        // buffer must never make records drawable.
        if (g_State.InstanceCpu == nullptr || g_State.InstanceGpu[frameIndex] == nullptr)
            return false;

        // Mark the upload before the write: a frame may only draw what it
        // actually uploaded.
        g_State.InstanceUploaded[frameIndex] = instanceCount;

        const ImSlugGlyphInstance* source = g_State.InstanceCpu + (size_t)frameIndex * g_State.InstanceCapacity;
        wgpuQueueWriteBuffer(g_State.Info.Queue, g_State.InstanceGpu[frameIndex], 0, source,
                             (size_t)instanceCount * sizeof(ImSlugGlyphInstance));
        return true;
    }

    void ImSlugWGPUDraw(WGPURenderPassEncoder pass, const ImSlugWGPUDrawInfo* drawInfo)
    {
        IMSLUG_ASSERT(g_State.Initialized);
        IMSLUG_ASSERT(drawInfo != nullptr);
        if (!g_State.Initialized || drawInfo == nullptr || pass == nullptr || drawInfo->InstanceCount == 0)
            return;

        // Defensive clamps (release-safe): never bind an unwritten bind group
        // and never fetch past the uploaded instance range.
        if (g_State.BindGroup == nullptr || g_State.Pipeline == nullptr || drawInfo->MVP == nullptr)
        {
            IMSLUG_ASSERT(g_State.BindGroup != nullptr);
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
        IMSLUG_ASSERT(firstInstance < g_State.InstanceUploaded[frameIndex]);
        if (firstInstance >= g_State.InstanceUploaded[frameIndex])
            return;
        if (instanceCount > g_State.InstanceUploaded[frameIndex] - firstInstance)
            instanceCount = g_State.InstanceUploaded[frameIndex] - firstInstance;

        // Scissor (the caller clamps against the framebuffer; this only guards
        // degenerate values - WebGPU rejects out-of-bounds rects).
        const uint32_t viewportWidth = drawInfo->ViewportWidth > 0.0f ? (uint32_t)drawInfo->ViewportWidth : 0;
        const uint32_t viewportHeight = drawInfo->ViewportHeight > 0.0f ? (uint32_t)drawInfo->ViewportHeight : 0;
        uint32_t scissorX = drawInfo->ScissorX;
        uint32_t scissorY = drawInfo->ScissorY;
        uint32_t scissorW = drawInfo->ScissorW;
        uint32_t scissorH = drawInfo->ScissorH;
        IMSLUG_ASSERT(viewportWidth > 0 && viewportHeight > 0);
        if (viewportWidth == 0 || viewportHeight == 0 || scissorW == 0 || scissorH == 0 || scissorX >= viewportWidth ||
            scissorY >= viewportHeight)
        {
            return;
        }
        if (scissorW > viewportWidth - scissorX)
            scissorW = viewportWidth - scissorX;
        if (scissorH > viewportHeight - scissorY)
            scissorH = viewportHeight - scissorY;

        // One parameter record per batch (the push-constant replacement).
        if (g_State.UniformCursor[frameIndex] >= g_State.UniformBatchesPerFrame)
        {
            IMSLUG_ASSERT(false && "imslug: WebGPU uniform ring exhausted; the text run is dropped");
            WarnUniformRingFull(frameIndex);
            return;
        }
        const uint32_t batchIndex = g_State.UniformCursor[frameIndex]++;
        const uint64_t uniformOffset = (uint64_t)frameIndex * g_State.UniformBatchesPerFrame * g_State.UniformStride +
                                       (uint64_t)batchIndex * g_State.UniformStride;
        IMSLUG_ASSERT(uniformOffset <= UINT32_MAX);
        if (uniformOffset > UINT32_MAX)
            return;

        UniformParams params = {};
        memcpy(params.MVP, drawInfo->MVP, sizeof(params.MVP));
        params.ViewportWidth = drawInfo->ViewportWidth;
        params.ViewportHeight = drawInfo->ViewportHeight;
        params.DebugFlags = drawInfo->DebugFlags;
        wgpuQueueWriteBuffer(g_State.Info.Queue, g_State.Uniforms, uniformOffset, &params, sizeof(params));

        const uint32_t dynamicOffset = (uint32_t)uniformOffset;
        wgpuRenderPassEncoderSetPipeline(pass, g_State.Pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, g_State.BindGroup, 1, &dynamicOffset);
        // The slot's buffer is bound at the batch's offset; the draw itself uses
        // firstInstance 0 (base-instance is feature-gated, offsets are always
        // core-valid).
        wgpuRenderPassEncoderSetVertexBuffer(
            pass, 0, g_State.InstanceGpu[frameIndex], (uint64_t)firstInstance * sizeof(ImSlugGlyphInstance),
            (uint64_t)(g_State.InstanceCapacity - firstInstance) * sizeof(ImSlugGlyphInstance));
        wgpuRenderPassEncoderSetScissorRect(pass, scissorX, scissorY, scissorW, scissorH);
        wgpuRenderPassEncoderDraw(pass, 4, instanceCount, 0, 0);
    }

    // =====================================================================
    // ImGui adapter
    // =====================================================================
    // Bridges the stock ImGui WebGPU backend and this module:
    //   * Init maps ImGui_ImplWGPU_InitInfo onto ImSlugWGPUInitInfo and
    //     registers the ImSlugRenderIO the core's pre-pass uses;
    //   * UploadPools uploads the dirty pool rows and advances the
    //     frames-in-flight slot;
    //   * RenderDrawData rewrites the draw data (Slug text -> callbacks),
    //     uploads the frame's staged instances with one queue.writeBuffer and
    //     hands everything to ImGui_ImplWGPU_RenderDrawData;
    //   * the batch callback records one instanced draw per text run into the
    //     backend's active pass (published through
    //     PlatformIO().Renderer_RenderState by the stock backend).
    // The callback runs before the stock scissor/bind-group setup, so it sets
    // its own pipeline, bind group and scissor; the core injects a
    // ResetRenderState command right after every Slug run, which re-establishes
    // ImGui's render state (ImGui_ImplWGPU_SetupRenderState).
    namespace
    {

        struct ImGuiWGPUState
        {
            bool Initialized;

            // The queue reference acquired with wgpuDeviceGetQueue at Init
            // (ImGui_ImplWGPU_InitInfo carries the device only) and released at
            // Shutdown.
            WGPUQueue Queue;

            // Debug overlay flags (ImSlugWGPUDebugOverlay), toggled with
            // ImSlug_ImplWGPU_SetDebugOverlay; seeded from
            // Config::DebugDrawBounds at Init.
            uint32_t DebugOverlay;

            // Per-frame slot; advanced by UploadPools (one per frame). The
            // instance allocation cursor lives in the core (one reserve path
            // for the adapter and the harnesses).
            uint32_t FrameIndex;    // ring slot for the current frame
            uint32_t FrameCounter;  // advanced by UploadPools (one per frame)
            int UploadFrameCount;   // ImGui::GetFrameCount() of the last upload (-1 = none)
            // High-water mark of this frame's successful reservations: the
            // pre-pass reserves contiguously from 0, so this is exactly the
            // staged range the frame uploads with one queue.writeBuffer.
            uint32_t StagedInstanceCount;

            // Per RenderDrawData call (callbacks run inside the stock backend's
            // render loop, which republishes the pass encoder).
            float DisplayPosX, DisplayPosY;
            float DisplaySizeX, DisplaySizeY;  // ImGui layout space (logical px)
            float FramebufferScaleX, FramebufferScaleY;
            uint32_t FramebufferWidth, FramebufferHeight;  // physical px (DisplaySize x FramebufferScale)
        };

        ImGuiWGPUState g_ImGuiWGPU;

        uint32_t ImGuiRenderIO_GetFrameIndex()
        {
            return g_ImGuiWGPU.FrameIndex;
        }

        ImSlugGlyphInstance* ImGuiRenderIO_ReserveInstances(uint32_t count, uint32_t* outFirstInstance)
        {
            if (!g_ImGuiWGPU.Initialized || count == 0)
                return nullptr;
            // The core owns the per-frame cursor, the growth policy and the
            // one-time overflow warning (see ImSlugWGPUReserveInstances).
            ImSlugGlyphInstance* instances = ImSlugWGPUReserveInstances(count, outFirstInstance);
            if (instances != nullptr)
            {
                const uint32_t end = *outFirstInstance + count;
                if (end > g_ImGuiWGPU.StagedInstanceCount)
                    g_ImGuiWGPU.StagedInstanceCount = end;
            }
            return instances;
        }

        void ImGuiRenderIO_DrawBatch(const ImSlugBatch* batch)
        {
            if (batch == nullptr || batch->InstanceCount == 0)
                return;
            const ImGui_ImplWGPU_RenderState* renderState =
                (const ImGui_ImplWGPU_RenderState*)ImGui::GetPlatformIO().Renderer_RenderState;
            if (renderState == nullptr || renderState->RenderPassEncoder == nullptr)
                return;  // not inside ImGui_ImplWGPU_RenderDrawData
            if (g_ImGuiWGPU.FramebufferWidth == 0 || g_ImGuiWGPU.FramebufferHeight == 0)
                return;
            IMSLUG_ASSERT(g_ImGuiWGPU.DisplaySizeX > 0.0f && g_ImGuiWGPU.DisplaySizeY > 0.0f);

            // Clip rects arrive in ImGui display coordinates; convert and clamp
            // exactly like the stock backend does (the pass encoder rejects
            // off-bounds scissor rects).
            float clipMinX = (batch->ClipMinX - g_ImGuiWGPU.DisplayPosX) * g_ImGuiWGPU.FramebufferScaleX;
            float clipMinY = (batch->ClipMinY - g_ImGuiWGPU.DisplayPosY) * g_ImGuiWGPU.FramebufferScaleY;
            float clipMaxX = (batch->ClipMaxX - g_ImGuiWGPU.DisplayPosX) * g_ImGuiWGPU.FramebufferScaleX;
            float clipMaxY = (batch->ClipMaxY - g_ImGuiWGPU.DisplayPosY) * g_ImGuiWGPU.FramebufferScaleY;
            if (clipMinX < 0.0f)
                clipMinX = 0.0f;
            if (clipMinY < 0.0f)
                clipMinY = 0.0f;
            if (clipMaxX > (float)g_ImGuiWGPU.FramebufferWidth)
                clipMaxX = (float)g_ImGuiWGPU.FramebufferWidth;
            if (clipMaxY > (float)g_ImGuiWGPU.FramebufferHeight)
                clipMaxY = (float)g_ImGuiWGPU.FramebufferHeight;
            if (clipMaxX <= clipMinX || clipMaxY <= clipMinY)
                return;

            // The instances (and the batch clip rects) are in ImGui display
            // coordinates, so the projection must map the *display* extent to
            // clip space - the same projection imgui_impl_wgpu builds from
            // DisplayPos/DisplaySize. FramebufferWidth/Height (physical pixels)
            // only size the viewport, clamp the scissor and feed the shader's
            // dilation (pc.viewport = render target size). Building this from the
            // framebuffer size instead drew at 1/FramebufferScale size on
            // hi-DPI targets (phones/retina), i.e. the text fell outside its own
            // scissor rect and vanished.
            float mvp[16];
            ImSlugWGPUBuildOrthoMVP(g_ImGuiWGPU.DisplaySizeX, g_ImGuiWGPU.DisplaySizeY, mvp);

            ImSlugWGPUDrawInfo drawInfo = {};
            drawInfo.FrameIndex = batch->FrameIndex;
            drawInfo.FirstInstance = batch->FirstInstance;
            drawInfo.InstanceCount = batch->InstanceCount;
            drawInfo.MVP = mvp;
            drawInfo.ViewportWidth = (float)g_ImGuiWGPU.FramebufferWidth;
            drawInfo.ViewportHeight = (float)g_ImGuiWGPU.FramebufferHeight;
            drawInfo.DebugFlags = g_ImGuiWGPU.DebugOverlay;
            drawInfo.ScissorX = (uint32_t)clipMinX;
            drawInfo.ScissorY = (uint32_t)clipMinY;
            drawInfo.ScissorW = (uint32_t)(clipMaxX - clipMinX);
            drawInfo.ScissorH = (uint32_t)(clipMaxY - clipMinY);
            ImSlugWGPUDraw(renderState->RenderPassEncoder, &drawInfo);
        }

        const ImSlugRenderIO g_ImGuiRenderIO = { "webgpu", ImGuiRenderIO_GetFrameIndex, ImGuiRenderIO_ReserveInstances,
                                                 ImGuiRenderIO_DrawBatch };
    }  // namespace

    bool ImSlug_ImplWGPU_Init(const ImGui_ImplWGPU_InitInfo* initInfo)
    {
        IMSLUG_ASSERT(initInfo != nullptr);
        if (initInfo == nullptr)
            return false;
        if (g_ImGuiWGPU.Initialized)
            ImSlug_ImplWGPU_Shutdown();

        const Config* config = ImSlugGetConfig();
        if (config == nullptr)
        {
            IMSLUG_ASSERT(false && "imslug: call ImSlug::Init() before ImSlug_ImplWGPU_Init()");
            return false;
        }

        // ImGui_ImplWGPU_InitInfo carries the device only; the queue is the
        // device's default queue (wgpuDeviceGetQueue returns a new reference the
        // module owns until Shutdown).
        WGPUQueue queue = initInfo->Device != nullptr ? wgpuDeviceGetQueue(initInfo->Device) : nullptr;
        if (queue == nullptr)
            return false;

        ImSlugWGPUInitInfo info = {};
        info.Device = initInfo->Device;
        info.Queue = queue;
        info.RenderTargetFormat = initInfo->RenderTargetFormat;
        info.DepthStencilFormat = initInfo->DepthStencilFormat;
        info.MultisampleState = initInfo->PipelineMultisampleState;
        info.MaxInstancesPerFrame = config->MaxInstancesPerFrame;
        info.NumFramesInFlight = initInfo->NumFramesInFlight > 0 ? (uint32_t)initInfo->NumFramesInFlight : 0;

        if (!ImSlugWGPUInit(&info))
        {
            wgpuQueueRelease(queue);
            return false;
        }

        g_ImGuiWGPU = ImGuiWGPUState();
        g_ImGuiWGPU.Initialized = true;
        g_ImGuiWGPU.Queue = queue;
        g_ImGuiWGPU.UploadFrameCount = -1;
        // Config::DebugDrawBounds seeds the glyph-bounds overlay (the runtime
        // toggle stays available for the band views).
        g_ImGuiWGPU.DebugOverlay = config->DebugDrawBounds ? (uint32_t)ImSlugWGPUDebugOverlay_GlyphBounds
                                                           : (uint32_t)ImSlugWGPUDebugOverlay_None;
        ImSlugSetRenderIO(&g_ImGuiRenderIO);
        return true;
    }

    void ImSlug_ImplWGPU_Shutdown()
    {
        if (!g_ImGuiWGPU.Initialized)
            return;
        ImSlugSetRenderIO(nullptr);
        ImSlugWGPUShutdown();
        if (g_ImGuiWGPU.Queue != nullptr)
            wgpuQueueRelease(g_ImGuiWGPU.Queue);
        g_ImGuiWGPU = ImGuiWGPUState();
    }

    void ImSlug_ImplWGPU_UploadPools()
    {
        if (!g_ImGuiWGPU.Initialized)
            return;
        // One upload per frame (a second call would skip an instance-ring slot).
        // ImGui's frame count is the only per-frame signal available outside the
        // frame cycle.
        const int frameCount = ImGui::GetFrameCount();
        IMSLUG_ASSERT(g_ImGuiWGPU.UploadFrameCount != frameCount &&
                      "imslug: ImSlug_ImplWGPU_UploadPools called twice in one frame");
        g_ImGuiWGPU.UploadFrameCount = frameCount;

        const uint32_t framesInFlight = ImSlugWGPUGetFramesInFlight();
        g_ImGuiWGPU.FrameIndex = framesInFlight != 0 ? (g_ImGuiWGPU.FrameCounter % framesInFlight) : 0;
        g_ImGuiWGPU.FrameCounter++;
        g_ImGuiWGPU.StagedInstanceCount = 0;

        ImSlugPools* pools = ImSlugGetPools();
        if (pools != nullptr)
            ImSlugWGPUUploadPools(pools, g_ImGuiWGPU.FrameIndex);
    }

    void ImSlug_ImplWGPU_RenderDrawData(ImDrawData* drawData, WGPURenderPassEncoder pass)
    {
        if (!g_ImGuiWGPU.Initialized || drawData == nullptr)
        {
            // No Slug module (or no draw data): render through the stock path.
            ImGui_ImplWGPU_RenderDrawData(drawData, pass);
            return;
        }
        IMSLUG_ASSERT(g_ImGuiWGPU.UploadFrameCount >= 0 &&
                      "imslug: call ImSlug_ImplWGPU_UploadPools() before rendering (see imslug_impl_webgpu.h)");

        g_ImGuiWGPU.FramebufferWidth = (uint32_t)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
        g_ImGuiWGPU.FramebufferHeight = (uint32_t)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
        g_ImGuiWGPU.DisplayPosX = drawData->DisplayPos.x;
        g_ImGuiWGPU.DisplayPosY = drawData->DisplayPos.y;
        g_ImGuiWGPU.DisplaySizeX = drawData->DisplaySize.x;
        g_ImGuiWGPU.DisplaySizeY = drawData->DisplaySize.y;
        g_ImGuiWGPU.FramebufferScaleX = drawData->FramebufferScale.x;
        g_ImGuiWGPU.FramebufferScaleY = drawData->FramebufferScale.y;

        ImSlug::PreprocessDrawData(drawData);

        // Instances staged by the pre-pass: one queue.writeBuffer for the whole
        // frame's contiguous range into the current slot's GPU buffer (plan
        // section 3.1: after the pre-pass, before the draw data is rendered).
        if (g_ImGuiWGPU.StagedInstanceCount > 0)
            ImSlugWGPUUploadInstances(g_ImGuiWGPU.FrameIndex, g_ImGuiWGPU.StagedInstanceCount);

        ImGui_ImplWGPU_RenderDrawData(drawData, pass);

        g_ImGuiWGPU.FramebufferWidth = g_ImGuiWGPU.FramebufferHeight = 0;
    }

    void ImSlug_ImplWGPU_SetDebugOverlay(uint32_t overlayFlags)
    {
        // Valid while the module is initialized; a call before Init is ignored
        // (debug builds assert - the caller asked for a view that cannot exist).
        IMSLUG_ASSERT(g_ImGuiWGPU.Initialized);
        if (!g_ImGuiWGPU.Initialized)
            return;
        IMSLUG_ASSERT((overlayFlags & ~(uint32_t)(ImSlugWGPUDebugOverlay_GlyphBounds | ImSlugWGPUDebugOverlay_BandGrid |
                                                  ImSlugWGPUDebugOverlay_RefHeat)) == 0);
        g_ImGuiWGPU.DebugOverlay = overlayFlags;
    }

    uint32_t ImSlug_ImplWGPU_GetDebugOverlay()
    {
        return g_ImGuiWGPU.Initialized ? g_ImGuiWGPU.DebugOverlay : (uint32_t)ImSlugWGPUDebugOverlay_None;
    }
}  // namespace ImSlug
