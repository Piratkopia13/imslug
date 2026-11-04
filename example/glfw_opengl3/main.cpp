// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// example/glfw_opengl3/main.cpp — GLFW + OpenGL 3.2+ / ES 3.0 desktop shell.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "example_app.h"
#include "imslug.h"
#include "imslug_impl_opengl3.h"

using namespace ImSlug;

namespace
{
    // GL_TIME_ELAPSED query constant
    static const unsigned int GL_TIME_ELAPSED_CONST = 0x88BF;
    static const unsigned int GL_QUERY_RESULT_CONST = 0x8866;
    static const unsigned int GL_QUERY_RESULT_AVAILABLE_CONST = 0x8867;
    static const unsigned int GL_RGBA_CONST = 0x1908;
    static const unsigned int GL_UNSIGNED_BYTE_CONST = 0x1401;
    static const unsigned int GL_COLOR_BUFFER_BIT_CONST = 0x00004000;

    typedef void (*PFN_glGenQueries)(int n, unsigned int* ids);
    typedef void (*PFN_glDeleteQueries)(int n, const unsigned int* ids);
    typedef void (*PFN_glBeginQuery)(unsigned int target, unsigned int id);
    typedef void (*PFN_glEndQuery)(unsigned int target);
    typedef void (*PFN_glGetQueryObjectui64v)(unsigned int id, unsigned int pname, uint64_t* params);
    typedef void (*PFN_glGetQueryObjectiv)(unsigned int id, unsigned int pname, int* params);
    typedef void (*PFN_glClearColor)(float r, float g, float b, float a);
    typedef void (*PFN_glClear)(unsigned int mask);
    typedef void (*PFN_glViewport)(int x, int y, int width, int height);
    typedef void (*PFN_glReadPixels)(int x, int y, int width, int height, unsigned int format, unsigned int type,
                                     void* pixels);

    static PFN_glGenQueries p_glGenQueries = nullptr;
    static PFN_glDeleteQueries p_glDeleteQueries = nullptr;
    static PFN_glBeginQuery p_glBeginQuery = nullptr;
    static PFN_glEndQuery p_glEndQuery = nullptr;
    static PFN_glGetQueryObjectui64v p_glGetQueryObjectui64v = nullptr;
    static PFN_glGetQueryObjectiv p_glGetQueryObjectiv = nullptr;
    static PFN_glClearColor p_glClearColor = nullptr;
    static PFN_glClear p_glClear = nullptr;
    static PFN_glViewport p_glViewport = nullptr;
    static PFN_glReadPixels p_glReadPixels = nullptr;

    static void LoadShellGLProcs()
    {
        p_glGenQueries = (PFN_glGenQueries)glfwGetProcAddress("glGenQueries");
        p_glDeleteQueries = (PFN_glDeleteQueries)glfwGetProcAddress("glDeleteQueries");
        p_glBeginQuery = (PFN_glBeginQuery)glfwGetProcAddress("glBeginQuery");
        p_glEndQuery = (PFN_glEndQuery)glfwGetProcAddress("glEndQuery");
        p_glGetQueryObjectui64v = (PFN_glGetQueryObjectui64v)glfwGetProcAddress("glGetQueryObjectui64v");
        p_glGetQueryObjectiv = (PFN_glGetQueryObjectiv)glfwGetProcAddress("glGetQueryObjectiv");
        p_glClearColor = (PFN_glClearColor)glfwGetProcAddress("glClearColor");
        p_glClear = (PFN_glClear)glfwGetProcAddress("glClear");
        p_glViewport = (PFN_glViewport)glfwGetProcAddress("glViewport");
        p_glReadPixels = (PFN_glReadPixels)glfwGetProcAddress("glReadPixels");
    }

    struct ShellApp
    {
        GLFWwindow* Window = nullptr;
        ExampleAppOptions Options = {};
        bool Vsync = true;
        const char* ScreenshotPath = nullptr;
        int ScreenshotFrame = 2;
        int ScreenshotScene = 0;
        float ScreenshotDpi = 0.0f;

        // GPU timer query
        unsigned int GpuQuery = 0;
        bool GpuQueryInFlight = false;
        bool GpuTimestampsSupported = false;
        float LastGpuFrameMs = 0.0f;
    };

    static ShellApp g_App = {};

    static bool IsPackedAssetFile(const char* path)
    {
        FILE* file = fopen(path, "rb");
        if (file == nullptr)
            return false;
        char magic[8] = {};
        const size_t read = fread(magic, 1, sizeof(magic), file);
        fclose(file);
        return read == sizeof(magic) && memcmp(magic, "IMSLUG", 6) == 0;
    }

    static bool WritePpm(const char* path, const unsigned char* pixels, uint32_t width, uint32_t height)
    {
        FILE* file = fopen(path, "wb");
        if (file == nullptr)
        {
            fprintf(stderr, "[imslug] cannot write %s\n", path);
            return false;
        }
        fprintf(file, "P6\n%u %u\n255\n", width, height);
        unsigned char* row = (unsigned char*)malloc((size_t)width * 3);
        if (row == nullptr)
        {
            fclose(file);
            return false;
        }
        // OpenGL glReadPixels is bottom-up; flip vertically for top-down PPM
        for (uint32_t y = 0; y < height; ++y)
        {
            const unsigned char* src = pixels + (size_t)(height - 1 - y) * width * 4;
            for (uint32_t x = 0; x < width; ++x)
            {
                row[x * 3 + 0] = src[x * 4 + 0];
                row[x * 3 + 1] = src[x * 4 + 1];
                row[x * 3 + 2] = src[x * 4 + 2];
            }
            fwrite(row, 3, width, file);
        }
        free(row);
        fclose(file);
        printf("[imslug] screenshot written: %s (%ux%u)\n", path, width, height);
        return true;
    }

    // Platform hook implementations for ExampleApp
    static uint32_t PlatformGetDebugOverlay(void*)
    {
        return ImSlug_ImplOpenGL3_GetDebugOverlay();
    }

    static void PlatformSetDebugOverlay(void*, uint32_t flags)
    {
        ImSlug_ImplOpenGL3_SetDebugOverlay(flags);
    }

    static const char* PlatformGetValidationState(void*)
    {
        return "OpenGL 3.2+";
    }

    static bool PlatformGetGpuFrameMs(void*, float* outMs, int* outSlots)
    {
        if (!g_App.GpuTimestampsSupported)
            return false;
        *outMs = g_App.LastGpuFrameMs;
        *outSlots = (g_App.LastGpuFrameMs > 0.0f) ? 1 : 0;
        return true;
    }

    static bool PlatformGetVsync(void*)
    {
        return g_App.Vsync;
    }

    static void PlatformToggleVsync(void*)
    {
        g_App.Vsync = !g_App.Vsync;
        glfwSwapInterval(g_App.Vsync ? 1 : 0);
    }

    static const char* PlatformGetPresentModeName(void*)
    {
        return g_App.Vsync ? "FIFO (vsync)" : "Immediate (novsync)";
    }

    static ExamplePlatform MakePlatformHooks()
    {
        ExamplePlatform p = {};
        p.GetDebugOverlay = PlatformGetDebugOverlay;
        p.SetDebugOverlay = PlatformSetDebugOverlay;
        p.GetValidationState = PlatformGetValidationState;
        p.GetGpuFrameMs = PlatformGetGpuFrameMs;
        p.GetVsync = PlatformGetVsync;
        p.ToggleVsync = PlatformToggleVsync;
        p.GetPresentModeName = PlatformGetPresentModeName;
        p.user = nullptr;
        return p;
    }

    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
    {
        (void)scancode;
        (void)mods;
        if (action != GLFW_PRESS)
            return;

        if (key == GLFW_KEY_ESCAPE)
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        else if (key == GLFW_KEY_F1)
            ExampleAppToggleDebugOverlay(ExampleDebugOverlay_GlyphBounds);
        else if (key == GLFW_KEY_F2)
            ExampleAppToggleDebugOverlay(ExampleDebugOverlay_BandGrid);
        else if (key == GLFW_KEY_F3)
            ExampleAppToggleDebugOverlay(ExampleDebugOverlay_RefHeat);
        else if (key == GLFW_KEY_0)
            ExampleAppResetFonts();
        else if (key == GLFW_KEY_M)
            ExampleAppSetTextRenderer(!ExampleAppGetState()->AtlasText);
    }

    static void ParseCli(int argc, char** argv)
    {
        ExampleAppOptionsDefaults(&g_App.Options);
        for (int i = 1; i < argc; ++i)
        {
            int consumed = ExampleAppParseArg(argc, argv, i, &g_App.Options);
            if (consumed > 0)
            {
                i += consumed - 1;
                continue;
            }
            if (strcmp(argv[i], "--novsync") == 0)
            {
                g_App.Vsync = false;
            }
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
                printf("usage: imslug_example_glfw_opengl3 [--font <path>] [--size <px>] [--scene <1-%d>]\n"
                       "                                   [--renderer <slug|atlas>] [--novsync] [--dpi <scale>]\n"
                       "                                   [--overlay <none|bounds|grid|refs>]\n"
                       "                                   [--screenshot <file.ppm> [scene] [dpi_scale]]\n",
                       kExampleSceneCount);
                exit(0);
            }
            else
            {
                fprintf(stderr, "[imslug] unknown argument: %s\n", argv[i]);
                exit(1);
            }
        }

        if (g_App.Options.FontPath == nullptr)
        {
            const char* envFont = getenv("IMSLUG_EXAMPLE_FONT");
            if (envFont != nullptr && envFont[0] != '\0')
                g_App.Options.FontPath = envFont;
        }
        g_App.Options.FontPathIsPacked = g_App.Options.FontPath != nullptr && IsPackedAssetFile(g_App.Options.FontPath);
    }
}  // namespace

int main(int argc, char** argv)
{
    ParseCli(argc, argv);

    if (!glfwInit())
    {
        fprintf(stderr, "[imslug] failed to initialize GLFW\n");
        return 1;
    }

    // Request OpenGL 3.2 Core
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    const char* glsl_version = "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    const char* glsl_version = "#version 130";
#endif

    if (g_App.ScreenshotPath != nullptr)
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    g_App.Window = glfwCreateWindow(kExampleWindowWidth, kExampleWindowHeight, kExampleWindowTitle, nullptr, nullptr);
    if (g_App.Window == nullptr)
    {
        fprintf(stderr, "[imslug] failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(g_App.Window);
    glfwSwapInterval(g_App.Vsync ? 1 : 0);
    glfwSetKeyCallback(g_App.Window, KeyCallback);

    LoadShellGLProcs();

    // Query support for GPU timer queries
    if (p_glGenQueries != nullptr && p_glBeginQuery != nullptr && p_glEndQuery != nullptr &&
        p_glGetQueryObjectui64v != nullptr)
    {
        p_glGenQueries(1, &g_App.GpuQuery);
        g_App.GpuTimestampsSupported = (g_App.GpuQuery != 0);
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForOpenGL(g_App.Window, true))
    {
        fprintf(stderr, "[imslug] ImGui_ImplGlfw_InitForOpenGL failed\n");
        return 1;
    }

    if (!ImGui_ImplOpenGL3_Init(glsl_version))
    {
        fprintf(stderr, "[imslug] ImGui_ImplOpenGL3_Init failed\n");
        return 1;
    }

    // Initialize ImSlug core + OpenGL3 backend
    ImSlug::Config config;
    if (!ImSlug::Init(config))
    {
        fprintf(stderr, "[imslug] ImSlug::Init failed\n");
        return 1;
    }

    if (!ImSlug::ImSlug_ImplOpenGL3_Init(glsl_version))
    {
        fprintf(stderr, "[imslug] ImSlug_ImplOpenGL3_Init failed\n");
        return 1;
    }

    if (g_App.Options.DebugOverlayGiven)
        ImSlug::ImSlug_ImplOpenGL3_SetDebugOverlay(g_App.Options.DebugOverlay);

    // Initialize shared ExampleApp state
    const ExamplePlatform platform = MakePlatformHooks();
    ExampleAppInit(&g_App.Options, &platform);

    ExampleAppState* state = ExampleAppGetState();
    state->FontsDirty = true;
    ExampleAppRebuildFonts();

    // Main render loop
    while (!glfwWindowShouldClose(g_App.Window) && !state->ShouldClose)
    {
        glfwPollEvents();

        if (state->FontsDirty)
            ExampleAppRebuildFonts();

        int fbWidth = 0, fbHeight = 0;
        glfwGetFramebufferSize(g_App.Window, &fbWidth, &fbHeight);
        if (fbWidth <= 0 || fbHeight <= 0)
            continue;

        // GPU timer bracket
        if (g_App.GpuTimestampsSupported)
            p_glBeginQuery(GL_TIME_ELAPSED_CONST, g_App.GpuQuery);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ExampleAppBuildFrame();
        ImGui::Render();

        ExampleAppCaptureDrawStats();

        // Upload dirty pool rows before render pass
        ImSlug::ImSlug_ImplOpenGL3_UploadPools();

        if (p_glViewport != nullptr)
            p_glViewport(0, 0, fbWidth, fbHeight);
        if (p_glClearColor != nullptr)
            p_glClearColor(0.055f, 0.06f, 0.075f, 1.0f);
        if (p_glClear != nullptr)
            p_glClear(GL_COLOR_BUFFER_BIT_CONST);

        ImSlug::ImSlug_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (g_App.GpuTimestampsSupported)
        {
            p_glEndQuery(GL_TIME_ELAPSED_CONST);
            uint64_t timeElapsedNs = 0;
            p_glGetQueryObjectui64v(g_App.GpuQuery, GL_QUERY_RESULT_CONST, &timeElapsedNs);
            g_App.LastGpuFrameMs = (float)timeElapsedNs / 1.0e6f;
        }

        // Handle screenshot capture
        if (g_App.ScreenshotPath != nullptr && state->FramesRendered >= g_App.ScreenshotFrame)
        {
            size_t numBytes = (size_t)fbWidth * fbHeight * 4;
            unsigned char* pixels = (unsigned char*)malloc(numBytes);
            if (pixels != nullptr)
            {
                if (p_glReadPixels != nullptr)
                    p_glReadPixels(0, 0, fbWidth, fbHeight, GL_RGBA_CONST, GL_UNSIGNED_BYTE_CONST, pixels);
                WritePpm(g_App.ScreenshotPath, pixels, (uint32_t)fbWidth, (uint32_t)fbHeight);
                free(pixels);
            }
            break;
        }

        glfwSwapBuffers(g_App.Window);
        state->FramesRendered++;
    }

    // Cleanup
    if (g_App.GpuQuery != 0 && p_glDeleteQueries != nullptr)
    {
        p_glDeleteQueries(1, &g_App.GpuQuery);
        g_App.GpuQuery = 0;
    }

    ImSlug::ImSlug_ImplOpenGL3_Shutdown();
    ImSlug::Shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(g_App.Window);
    glfwTerminate();
    return 0;
}
