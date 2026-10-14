// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// example/web/main.cpp — browser platform shell (GLFW via Emscripten's
// contrib.glfw3 port + WebGPU via emdawnwebgpu) around the shared example app
// (example/common/example_app.*). Desktop-only capabilities are null hooks:
// no --screenshot/--novsync/--help CLI, no GPU timestamps, no present-mode
// toggle, no filesystem (the fonts are the embedded Roboto asset/atlas TTF
// and, when the build embedded it, the CJK subset; the emoji scene shows its
// "font not fetched" message).
//
// Built by scripts/build_web.* (CMake target imslug_example_web); the page
// shell is example/web/shell.html and communicates through the JS globals it
// sets: window.__imslugFrames / __imslugReady / __imslugError.
//
// Per-frame order (the contract shared with the desktop shell and
// backends/imslug_impl_webgpu.h):
//   poll → resize+configure surface → ImGui_ImplWGPU_NewFrame →
//   ImGui_ImplGlfw_NewFrame → ExampleAppBuildFrame (NewFrame + scenes) →
//   ImGui::Render → ExampleAppCaptureDrawStats → ImSlug_ImplWGPU_UploadPools →
//   begin pass → ImSlug_ImplWGPU_RenderDrawData → end/submit → JS hooks.
// Emscripten presents implicitly at the end of the rAF callback (no
// wgpuSurfacePresent call; see the pinned Dear ImGui GLFW+WebGPU example).

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_wgpu.h"

#include "example_app.h"
#include "imslug.h"
#include "imslug_impl_webgpu.h"

#include <GLFW/glfw3.h>

#include <emscripten.h>
#include <emscripten/html5.h>
#include "emscripten_mainloop_stub.h"

#include <stdio.h>

namespace
{
    GLFWwindow* g_Window = nullptr;
    WGPUInstance g_Instance = nullptr;
    WGPUAdapter g_Adapter = nullptr;
    WGPUDevice g_Device = nullptr;
    WGPUQueue g_Queue = nullptr;
    WGPUSurface g_Surface = nullptr;
    WGPUSurfaceConfiguration g_SurfaceConfig = {};
    uint32_t g_SurfaceWidth = 0;
    uint32_t g_SurfaceHeight = 0;
    int g_DeviceLost = 0;

    // ---------------------------------------------------------------------
    // Page reporting
    // ---------------------------------------------------------------------
    void ReportPageError(const char* what)
    {
        fprintf(stderr, "[imslug] %s\n", what);
        // clang-format off
        EM_ASM({ if (window.__imslugSetError) window.__imslugSetError(UTF8ToString($0)); }, what);
        // clang-format on
    }

    void DeviceLostCallback(WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message, void*, void*)
    {
        g_DeviceLost = 1;
        fprintf(stderr, "[imslug] WebGPU device lost (%s): %s\n", ImGui_ImplWGPU_GetDeviceLostReasonName(reason),
                message.data);
        // clang-format off
        EM_ASM({ if (window.__imslugDeviceLostHook) window.__imslugDeviceLostHook(); }, 0);
        // clang-format on
    }

    void UncapturedErrorCallback(WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void*, void*)
    {
        fprintf(stderr, "[imslug] WebGPU error (%s): %s\n", ImGui_ImplWGPU_GetErrorTypeName(type), message.data);
    }

    // ---------------------------------------------------------------------
    // ExamplePlatform hooks (only the ones the web can provide; a null hook
    // hides the corresponding row in the shared UI)
    // ---------------------------------------------------------------------
    static_assert((uint32_t)ExampleDebugOverlay_GlyphBounds == (uint32_t)ImSlug::ImSlugWGPUDebugOverlay_GlyphBounds,
                  "overlay flag mapping drift (glyph bounds)");
    static_assert((uint32_t)ExampleDebugOverlay_BandGrid == (uint32_t)ImSlug::ImSlugWGPUDebugOverlay_BandGrid,
                  "overlay flag mapping drift (band grid)");
    static_assert((uint32_t)ExampleDebugOverlay_RefHeat == (uint32_t)ImSlug::ImSlugWGPUDebugOverlay_RefHeat,
                  "overlay flag mapping drift (ref heat)");

    uint32_t PlatformGetDebugOverlay(void*)
    {
        return ImSlug::ImSlug_ImplWGPU_GetDebugOverlay();
    }

    void PlatformSetDebugOverlay(void*, uint32_t flags)
    {
        ImSlug::ImSlug_ImplWGPU_SetDebugOverlay(flags);
    }

    ExamplePlatform MakePlatformHooks()
    {
        ExamplePlatform platform = {};
        platform.GetDebugOverlay = PlatformGetDebugOverlay;
        platform.SetDebugOverlay = PlatformSetDebugOverlay;
        // GetValidationState / GetGpuFrameMs / GetVsync / ToggleVsync /
        // GetPresentModeName stay null: the browser has no validation layers,
        // no timestamp queries and no present-mode control.
        platform.user = nullptr;
        return platform;
    }

    // ---------------------------------------------------------------------
    // Input
    // ---------------------------------------------------------------------
    // The scene (1-9/P), size (+/-) and renderer radio keys are polled by the
    // shared switcher; this maps the remaining platform keys. Esc is not
    // mapped (a page cannot close itself).
    void KeyCallback(GLFWwindow*, int key, int, int action, int)
    {
        if (action != GLFW_PRESS && action != GLFW_REPEAT)
            return;
        ExampleAppState& state = *ExampleAppGetState();
        if (key == GLFW_KEY_F1)
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
    // WebGPU bootstrap (raw C API, the same flavour the WebGPU module uses;
    // see the pinned Dear ImGui example_glfw_wgpu Emscripten path)
    // ---------------------------------------------------------------------
    WGPUAdapter RequestAdapterBlocking(WGPUInstance instance)
    {
        WGPURequestAdapterOptions options = {};
        options.powerPreference = WGPUPowerPreference_HighPerformance;

        struct Result
        {
            WGPUAdapter Acquired;
            bool Done;
        } result = { nullptr, false };

        WGPURequestAdapterCallbackInfo callbackInfo = {};
        callbackInfo.mode = WGPUCallbackMode_WaitAnyOnly;
        callbackInfo.userdata1 = &result;
        callbackInfo.callback =
            [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void*)
        {
            Result* out = (Result*)userdata1;
            out->Done = true;
            if (status == WGPURequestAdapterStatus_Success)
                out->Acquired = adapter;
            else
                fprintf(stderr, "[imslug] WebGPU adapter request failed (%.*s)\n", (int)message.length, message.data);
        };

        WGPUFuture future = wgpuInstanceRequestAdapter(instance, &options, callbackInfo);
        WGPUFutureWaitInfo waitInfo = { future, false };
        const WGPUWaitStatus waitStatus = wgpuInstanceWaitAny(instance, 1, &waitInfo, ~0ull);
        if (waitStatus != WGPUWaitStatus_Success || !result.Done || result.Acquired == nullptr)
            return nullptr;
        return result.Acquired;
    }

    WGPUDevice RequestDeviceBlocking(WGPUInstance instance, WGPUAdapter adapter)
    {
        WGPUDeviceDescriptor deviceDesc = {};
        // The app owns the device, so the process-wide error/lost reporting is
        // installed here (the WebGPU module can only bracket its own work).
        deviceDesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
        deviceDesc.deviceLostCallbackInfo.callback = DeviceLostCallback;
        deviceDesc.uncapturedErrorCallbackInfo.callback = UncapturedErrorCallback;

        struct Result
        {
            WGPUDevice Acquired;
            bool Done;
        } result = { nullptr, false };

        WGPURequestDeviceCallbackInfo callbackInfo = {};
        callbackInfo.mode = WGPUCallbackMode_WaitAnyOnly;
        callbackInfo.userdata1 = &result;
        callbackInfo.callback =
            [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void*)
        {
            Result* out = (Result*)userdata1;
            out->Done = true;
            if (status == WGPURequestDeviceStatus_Success)
                out->Acquired = device;
            else
                fprintf(stderr, "[imslug] WebGPU device request failed (%.*s)\n", (int)message.length, message.data);
        };

        WGPUFuture future = wgpuAdapterRequestDevice(adapter, &deviceDesc, callbackInfo);
        WGPUFutureWaitInfo waitInfo = { future, false };
        const WGPUWaitStatus waitStatus = wgpuInstanceWaitAny(instance, 1, &waitInfo, ~0ull);
        if (waitStatus != WGPUWaitStatus_Success || !result.Done || result.Acquired == nullptr)
            return nullptr;
        return result.Acquired;
    }

    bool ConfigureSurface(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return false;
        g_SurfaceConfig.width = g_SurfaceWidth = width;
        g_SurfaceConfig.height = g_SurfaceHeight = height;
        wgpuSurfaceConfigure(g_Surface, &g_SurfaceConfig);
        return true;
    }

    bool InitWGPU()
    {
        WGPUInstanceDescriptor instanceDesc = {};
        WGPUInstanceFeatureName timedWaitAny = WGPUInstanceFeatureName_TimedWaitAny;
        instanceDesc.requiredFeatureCount = 1;
        instanceDesc.requiredFeatures = &timedWaitAny;
        g_Instance = wgpuCreateInstance(&instanceDesc);
        if (g_Instance == nullptr)
            return false;

        g_Adapter = RequestAdapterBlocking(g_Instance);
        if (g_Adapter == nullptr)
            return false;
        g_Device = RequestDeviceBlocking(g_Instance, g_Adapter);
        if (g_Device == nullptr)
            return false;

        // Canvas surface (the HTML selector from example/web/shell.html).
        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc = {};
        canvasDesc.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
        canvasDesc.selector = { "#canvas", WGPU_STRLEN };

        WGPUSurfaceDescriptor surfaceDesc = {};
        surfaceDesc.nextInChain = &canvasDesc.chain;
        g_Surface = wgpuInstanceCreateSurface(g_Instance, &surfaceDesc);
        if (g_Surface == nullptr)
            return false;

        WGPUSurfaceCapabilities capabilities = {};
        wgpuSurfaceGetCapabilities(g_Surface, g_Adapter, &capabilities);
        if (capabilities.formatCount == 0)
        {
            wgpuSurfaceCapabilitiesFreeMembers(capabilities);
            return false;
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(g_Window, &width, &height);

        g_SurfaceConfig = {};
        g_SurfaceConfig.device = g_Device;
        g_SurfaceConfig.format = capabilities.formats[0];
        g_SurfaceConfig.usage = WGPUTextureUsage_RenderAttachment;
        g_SurfaceConfig.alphaMode = WGPUCompositeAlphaMode_Auto;
        g_SurfaceConfig.presentMode = WGPUPresentMode_Fifo;
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);

        if (!ConfigureSurface((uint32_t)width, (uint32_t)height))
            return false;

        wgpuAdapterRelease(g_Adapter);
        g_Adapter = nullptr;
        g_Queue = wgpuDeviceGetQueue(g_Device);
        return g_Queue != nullptr;
    }

    void ShutdownWGPU()
    {
        if (g_Queue != nullptr)
        {
            wgpuQueueRelease(g_Queue);
            g_Queue = nullptr;
        }
        if (g_Surface != nullptr)
        {
            wgpuSurfaceUnconfigure(g_Surface);
            wgpuSurfaceRelease(g_Surface);
            g_Surface = nullptr;
        }
        if (g_Device != nullptr)
        {
            wgpuDeviceRelease(g_Device);
            g_Device = nullptr;
        }
        if (g_Adapter != nullptr)
        {
            wgpuAdapterRelease(g_Adapter);
            g_Adapter = nullptr;
        }
        if (g_Instance != nullptr)
        {
            wgpuInstanceRelease(g_Instance);
            g_Instance = nullptr;
        }
    }

    void GlfwErrorCallback(int error, const char* description)
    {
        fprintf(stderr, "[imslug] GLFW error %d: %s\n", error, description);
    }

    // ---------------------------------------------------------------------
    // Frame
    // ---------------------------------------------------------------------
    void RenderFrame()
    {
        ExampleAppState& state = *ExampleAppGetState();

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(g_Window, &width, &height);
        if (width > 0 && height > 0 && ((uint32_t)width != g_SurfaceWidth || (uint32_t)height != g_SurfaceHeight))
            ConfigureSurface((uint32_t)width, (uint32_t)height);

        WGPUSurfaceTexture surfaceTexture = {};
        wgpuSurfaceGetCurrentTexture(g_Surface, &surfaceTexture);
        if (ImGui_ImplWGPU_IsSurfaceStatusError(surfaceTexture.status))
        {
            ReportPageError("unrecoverable surface texture status");
            emscripten_cancel_main_loop();
            return;
        }
        if (ImGui_ImplWGPU_IsSurfaceStatusSubOptimal(surfaceTexture.status))
        {
            if (surfaceTexture.texture != nullptr)
                wgpuTextureRelease(surfaceTexture.texture);
            if (width > 0 && height > 0)
                ConfigureSurface((uint32_t)width, (uint32_t)height);
            return;
        }

        ImGui_ImplWGPU_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ExampleAppBuildFrame();
        ImGui::Render();

        // Capture the draw-data shape for the performance panel (the live draw
        // data is cleared by the next NewFrame; the Slug pre-pass rewrites a
        // copy of this during RenderDrawData, so read it before that).
        ExampleAppCaptureDrawStats();

        // Glyph-pool uploads (dirty rows only) + the frames-in-flight slot
        // advance. Queue writes may be issued while the pass is still being
        // encoded; keep the documented order (uploads, then the pass).
        ImSlug::ImSlug_ImplWGPU_UploadPools();

        WGPUTextureViewDescriptor viewDesc = {};
        viewDesc.format = g_SurfaceConfig.format;
        viewDesc.dimension = WGPUTextureViewDimension_2D;
        viewDesc.mipLevelCount = WGPU_MIP_LEVEL_COUNT_UNDEFINED;
        viewDesc.arrayLayerCount = WGPU_ARRAY_LAYER_COUNT_UNDEFINED;
        viewDesc.aspect = WGPUTextureAspect_All;
        WGPUTextureView view = wgpuTextureCreateView(surfaceTexture.texture, &viewDesc);

        WGPURenderPassColorAttachment colorAttachment = {};
        colorAttachment.view = view;
        colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        colorAttachment.loadOp = WGPULoadOp_Clear;
        colorAttachment.storeOp = WGPUStoreOp_Store;
        colorAttachment.clearValue = { 0.055, 0.06, 0.075, 1.0 };  // same clear as the desktop shell

        WGPURenderPassDescriptor passDesc = {};
        passDesc.colorAttachmentCount = 1;
        passDesc.colorAttachments = &colorAttachment;

        WGPUCommandEncoderDescriptor encoderDesc = {};
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(g_Device, &encoderDesc);
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);

        // Everything (Slug text through callbacks, shapes through the stock
        // path) goes through ImSlug_ImplWGPU_RenderDrawData.
        ImSlug::ImSlug_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass);

        wgpuRenderPassEncoderEnd(pass);

        WGPUCommandBufferDescriptor commandDesc = {};
        WGPUCommandBuffer command = wgpuCommandEncoderFinish(encoder, &commandDesc);
        wgpuQueueSubmit(g_Queue, 1, &command);

        wgpuCommandBufferRelease(command);
        wgpuRenderPassEncoderRelease(pass);
        wgpuCommandEncoderRelease(encoder);
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(surfaceTexture.texture);

        state.FramesRendered++;

        // Page hooks (the JS bodies live in example/web/shell.html so
        // clang-format never touches them; scripts/web_smoke reads the globals).
        // clang-format off
        EM_ASM({ if (window.__imslugFrameHook) window.__imslugFrameHook(); }, 0);
        // clang-format on
    }
}  // namespace

int main(int, char**)
{
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit())
    {
        ReportPageError("GLFW initialization failed");
        return 1;
    }

    // No graphics context: the canvas is driven by WebGPU.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    g_Window = glfwCreateWindow(kExampleWindowWidth, kExampleWindowHeight, kExampleWindowTitle, nullptr, nullptr);
    if (g_Window == nullptr)
    {
        ReportPageError("window creation failed");
        return 1;
    }
    glfwSetKeyCallback(g_Window, KeyCallback);

    // Shared app first (copies the options/hooks), then the GPU stack, then
    // the fonts (once the Slug module exists), mirroring the desktop shell.
    ExampleAppOptions options;
    ExampleAppOptionsDefaults(&options);
    options.FontPath = nullptr;  // no filesystem: the embedded fonts only
    options.FontPathIsPacked = false;
    const ExamplePlatform platform = MakePlatformHooks();
    ExampleAppInit(&options, &platform);

    if (!InitWGPU())
    {
        ReportPageError("WebGPU initialization failed");
        ShutdownWGPU();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // no filesystem (and no persisted layout)
    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForOther(g_Window, true))
    {
        ReportPageError("ImGui_ImplGlfw_InitForOther failed");
        return 1;
    }
    ImGui_ImplGlfw_InstallEmscriptenCallbacks(g_Window, "#canvas");

    ImGui_ImplWGPU_InitInfo initInfo;
    initInfo.Device = g_Device;
    initInfo.NumFramesInFlight = 3;
    initInfo.RenderTargetFormat = g_SurfaceConfig.format;
    initInfo.DepthStencilFormat = WGPUTextureFormat_Undefined;
    if (!ImGui_ImplWGPU_Init(&initInfo))
    {
        ReportPageError("ImGui_ImplWGPU_Init failed");
        return 1;
    }

    // ImSlug core after the stock backend (RendererHasTextures) and before
    // its own module / any AddFont* call.
    if (!ImSlug::Init())
    {
        ReportPageError("ImSlug::Init failed");
        return 1;
    }
    if (!ImSlug::ImSlug_ImplWGPU_Init(&initInfo))
    {
        ReportPageError("ImSlug_ImplWGPU_Init failed");
        return 1;
    }

    ExampleAppState& state = *ExampleAppGetState();
    state.FontsDirty = true;  // add the fonts (needs the module)

    ExampleAppState& loopState = state;
    EMSCRIPTEN_MAINLOOP_BEGIN
    {
        if (g_DeviceLost != 0)
        {
            emscripten_cancel_main_loop();
            return;
        }
        glfwPollEvents();
        if (loopState.ShouldClose)
        {
            emscripten_cancel_main_loop();
            return;
        }
        if (loopState.FontsDirty)
            ExampleAppRebuildFonts();
        RenderFrame();
    }
    EMSCRIPTEN_MAINLOOP_END;

    // Unreachable in the browser (simulate_infinite_loop); kept for clarity
    // and for a future native-WebGPU harness.
    ImSlug::ImSlug_ImplWGPU_Shutdown();
    ImSlug::Shutdown();
    ImGui_ImplWGPU_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    ShutdownWGPU();
    glfwDestroyWindow(g_Window);
    glfwTerminate();
    return 0;
}
