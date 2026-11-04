// tests/golden/test_imgui_golden_opengl3.cpp — ImGui-integration golden test for OpenGL 3.2+.
//
// Renders one fixed ImGui scene twice in a headless offscreen OpenGL pass: once
// with the Slug font (our ImFontLoader + callbacks + Slug pipeline) and once
// with the same font through ImGui's own stb loader (atlas path, stock
// pipeline), then compares the two frames.

#include "imslug.h"
#include "imslug_impl_opengl3.h"
#include "imslug_internal.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include "../test_util.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

#include <GLFW/glfw3.h>

#ifndef IMSLUG_CJK_TEST_FONT
#define IMSLUG_CJK_TEST_FONT "tests/fonts/cjk/NotoSansSC-subset.ttf"
#endif

using namespace ImSlug;

namespace
{
    const int kExitSkip = 77;
    const uint32_t kBaseWidth = 1024;
    const uint32_t kBaseHeight = 640;
    const uint32_t kFrames = 3;

    typedef unsigned int GLenum;
    typedef unsigned int GLuint;
    typedef int GLint;
    typedef int GLsizei;

    static const GLenum GL_FRAMEBUFFER_CONST = 0x8D40;
    static const GLenum GL_COLOR_ATTACHMENT0_CONST = 0x8CE0;
    static const GLenum GL_TEXTURE_2D_CONST = 0x0DE1;
    static const GLenum GL_RGBA_CONST = 0x1908;
    static const GLenum GL_RGBA8_CONST = 0x8058;
    static const GLenum GL_UNSIGNED_BYTE_CONST = 0x1401;
    static const GLenum GL_COLOR_BUFFER_BIT_CONST = 0x00004000;
    static const GLenum GL_FRAMEBUFFER_COMPLETE_CONST = 0x8CD5;

    typedef void (*PFN_glGenFramebuffers)(GLsizei n, GLuint* framebuffers);
    typedef void (*PFN_glDeleteFramebuffers)(GLsizei n, const GLuint* framebuffers);
    typedef void (*PFN_glBindFramebuffer)(GLenum target, GLuint framebuffer);
    typedef void (*PFN_glFramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                                               GLint level);
    typedef GLenum (*PFN_glCheckFramebufferStatus)(GLenum target);
    typedef void (*PFN_glGenTextures)(GLsizei n, GLuint* textures);
    typedef void (*PFN_glDeleteTextures)(GLsizei n, const GLuint* textures);
    typedef void (*PFN_glBindTexture)(GLenum target, GLuint texture);
    typedef void (*PFN_glTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                                     GLint border, GLenum format, GLenum type, const void* pixels);
    typedef void (*PFN_glClearColor)(float r, float g, float b, float a);
    typedef void (*PFN_glClear)(unsigned int mask);
    typedef void (*PFN_glViewport)(int x, int y, int width, int height);
    typedef void (*PFN_glDisable)(unsigned int cap);
    typedef void (*PFN_glReadPixels)(int x, int y, int width, int height, unsigned int format, unsigned int type,
                                     void* pixels);

    static PFN_glGenFramebuffers p_glGenFramebuffers = nullptr;
    static PFN_glDeleteFramebuffers p_glDeleteFramebuffers = nullptr;
    static PFN_glBindFramebuffer p_glBindFramebuffer = nullptr;
    static PFN_glFramebufferTexture2D p_glFramebufferTexture2D = nullptr;
    static PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus = nullptr;
    static PFN_glGenTextures p_glGenTextures = nullptr;
    static PFN_glDeleteTextures p_glDeleteTextures = nullptr;
    static PFN_glBindTexture p_glBindTexture = nullptr;
    static PFN_glTexImage2D p_glTexImage2D = nullptr;
    static PFN_glClearColor p_glClearColor = nullptr;
    static PFN_glClear p_glClear = nullptr;
    static PFN_glViewport p_glViewport = nullptr;
    static PFN_glDisable p_glDisable = nullptr;
    static PFN_glReadPixels p_glReadPixels = nullptr;

    static bool LoadGL()
    {
        p_glGenFramebuffers = (PFN_glGenFramebuffers)glfwGetProcAddress("glGenFramebuffers");
        p_glDeleteFramebuffers = (PFN_glDeleteFramebuffers)glfwGetProcAddress("glDeleteFramebuffers");
        p_glBindFramebuffer = (PFN_glBindFramebuffer)glfwGetProcAddress("glBindFramebuffer");
        p_glFramebufferTexture2D = (PFN_glFramebufferTexture2D)glfwGetProcAddress("glFramebufferTexture2D");
        p_glCheckFramebufferStatus = (PFN_glCheckFramebufferStatus)glfwGetProcAddress("glCheckFramebufferStatus");
        p_glGenTextures = (PFN_glGenTextures)glfwGetProcAddress("glGenTextures");
        p_glDeleteTextures = (PFN_glDeleteTextures)glfwGetProcAddress("glDeleteTextures");
        p_glBindTexture = (PFN_glBindTexture)glfwGetProcAddress("glBindTexture");
        p_glTexImage2D = (PFN_glTexImage2D)glfwGetProcAddress("glTexImage2D");
        p_glClearColor = (PFN_glClearColor)glfwGetProcAddress("glClearColor");
        p_glClear = (PFN_glClear)glfwGetProcAddress("glClear");
        p_glViewport = (PFN_glViewport)glfwGetProcAddress("glViewport");
        p_glDisable = (PFN_glDisable)glfwGetProcAddress("glDisable");
        p_glReadPixels = (PFN_glReadPixels)glfwGetProcAddress("glReadPixels");

        return p_glGenFramebuffers && p_glBindFramebuffer && p_glFramebufferTexture2D && p_glCheckFramebufferStatus &&
               p_glReadPixels && p_glClear;
    }

    struct Target
    {
        GLuint Fbo = 0;
        GLuint ColorTex = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
    };

    static GLFWwindow* g_Window = nullptr;
    static Target g_Target = {};

    static bool EnsureTarget(uint32_t width, uint32_t height)
    {
        if (g_Target.Fbo != 0)
        {
            if (g_Target.Width == width && g_Target.Height == height)
                return true;
            p_glDeleteFramebuffers(1, &g_Target.Fbo);
            p_glDeleteTextures(1, &g_Target.ColorTex);
            g_Target.Fbo = 0;
            g_Target.ColorTex = 0;
        }

        g_Target.Width = width;
        g_Target.Height = height;

        p_glGenTextures(1, &g_Target.ColorTex);
        p_glBindTexture(GL_TEXTURE_2D_CONST, g_Target.ColorTex);
        p_glTexImage2D(GL_TEXTURE_2D_CONST, 0, GL_RGBA8_CONST, (GLsizei)width, (GLsizei)height, 0, GL_RGBA_CONST,
                       GL_UNSIGNED_BYTE_CONST, nullptr);

        p_glGenFramebuffers(1, &g_Target.Fbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER_CONST, g_Target.Fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER_CONST, GL_COLOR_ATTACHMENT0_CONST, GL_TEXTURE_2D_CONST,
                                 g_Target.ColorTex, 0);

        return p_glCheckFramebufferStatus(GL_FRAMEBUFFER_CONST) == GL_FRAMEBUFFER_COMPLETE_CONST;
    }

    static void BuildScene(float framebufferScale)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(kBaseWidth, kBaseHeight);
        io.DisplayFramebufferScale = ImVec2(framebufferScale, framebufferScale);
        io.DeltaTime = 1.0f / 60.0f;

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::Begin("GoldenScene", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);

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

        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(p, ImVec2(p.x + 160.0f, p.y + 26.0f), IM_COL32(70, 120, 200, 255), 4.0f);
        drawList->AddCircleFilled(ImVec2(p.x + 200.0f, p.y + 13.0f), 12.0f, IM_COL32(200, 140, 60, 255));
        drawList->AddText(ImVec2(p.x + 6.0f, p.y + 5.0f), IM_COL32(255, 255, 255, 255), "text over a shape");
        ImGui::Dummy(ImVec2(220.0f, 30.0f));

        ImGui::TextWrapped(
            "Wrapped text exercises the vertex reserve/retry path of RenderText and keeps the draw "
            "commands split across many glyphs: lorem ipsum dolor sit amet, consectetur adipiscing elit, "
            "sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.");

        ImGui::End();
        ImGui::Render();
    }

    static bool RenderVariant(bool useSlug, float framebufferScale, uint32_t overlayFlags,
                              std::vector<unsigned char>& outPixels, uint32_t& outW, uint32_t& outH)
    {
        uint32_t w = (uint32_t)(kBaseWidth * framebufferScale);
        uint32_t h = (uint32_t)(kBaseHeight * framebufferScale);
        outW = w;
        outH = h;
        if (!EnsureTarget(w, h))
            return false;

        IMGUI_CHECKVERSION();
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;

        const char* glsl_version = "#version 150";
        if (!ImGui_ImplOpenGL3_Init(glsl_version))
        {
            ImGui::DestroyContext();
            return false;
        }

        const char* fontPath = IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf");
        if (useSlug)
        {
            ImSlug::Config cfg;
            ImSlug::Init(cfg);
            if (!ImSlug::ImSlug_ImplOpenGL3_Init(glsl_version))
            {
                ImSlug::Shutdown();
                ImGui_ImplOpenGL3_Shutdown();
                ImGui::DestroyContext(ctx);
                return false;
            }
            ImSlug::AddFontFromFileTTF(fontPath, 17.0f);
            if (overlayFlags != 0)
                ImSlug::ImSlug_ImplOpenGL3_SetDebugOverlay(overlayFlags);
        }
        else
        {
            io.Fonts->AddFontFromFileTTF(fontPath, 17.0f);
        }

        p_glBindFramebuffer(GL_FRAMEBUFFER_CONST, g_Target.Fbo);

        for (uint32_t f = 0; f < kFrames; ++f)
        {
            ImGui_ImplOpenGL3_NewFrame();
            BuildScene(framebufferScale);

            if (useSlug)
                ImSlug::ImSlug_ImplOpenGL3_UploadPools();

            if (p_glDisable)
                p_glDisable(0x0C11 /* GL_SCISSOR_TEST */);
            if (p_glViewport)
                p_glViewport(0, 0, (GLsizei)w, (GLsizei)h);
            p_glClearColor(0.055f, 0.06f, 0.075f, 1.0f);
            p_glClear(GL_COLOR_BUFFER_BIT_CONST);

            ImDrawData* dd = ImGui::GetDrawData();
            if (useSlug)
                ImSlug::ImSlug_ImplOpenGL3_RenderDrawData(dd);
            else
                ImGui_ImplOpenGL3_RenderDrawData(dd);
        }

        // Read pixels
        std::vector<unsigned char> raw(w * h * 4);
        p_glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA_CONST, GL_UNSIGNED_BYTE_CONST, raw.data());

        // Flip vertically for top-down comparison
        outPixels.resize(w * h * 4);
        for (uint32_t y = 0; y < h; ++y)
        {
            const unsigned char* src = raw.data() + (size_t)(h - 1 - y) * w * 4;
            unsigned char* dst = outPixels.data() + (size_t)y * w * 4;
            memcpy(dst, src, (size_t)w * 4);
        }

        if (useSlug)
        {
            ImSlug::ImSlug_ImplOpenGL3_Shutdown();
            ImSlug::Shutdown();
        }
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext(ctx);
        return true;
    }

    static void BuildInkMask(const unsigned char* pixels, unsigned char* mask, uint32_t w, uint32_t h)
    {
        for (uint32_t i = 0; i < w * h; ++i)
        {
            const int sum = pixels[i * 4 + 0] + pixels[i * 4 + 1] + pixels[i * 4 + 2];
            mask[i] = (sum > 3 * 20) ? 1 : 0;
        }
    }

    static bool MasksAgree(const unsigned char* maskA, const unsigned char* maskB, uint32_t w, uint32_t h,
                           int* outMissA, int* outMissB)
    {
        *outMissA = 0;
        *outMissB = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t i = y * w + x;
                if (maskA[i] == 0 && maskB[i] == 0)
                    continue;
                bool hasA = false, hasB = false;
                for (int dy = -1; dy <= 1 && (!hasA || !hasB); ++dy)
                    for (int dx = -1; dx <= 1 && (!hasA || !hasB); ++dx)
                    {
                        const int nx = (int)x + dx;
                        const int ny = (int)y + dy;
                        if (nx < 0 || ny < 0 || nx >= (int)w || ny >= (int)h)
                            continue;
                        const uint32_t n = (uint32_t)ny * w + (uint32_t)nx;
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

    static int CountGlyphBoundsPixels(const unsigned char* pixels, uint32_t w, uint32_t h)
    {
        int count = 0;
        for (uint32_t i = 0; i < w * h; ++i)
        {
            const int r = pixels[i * 4 + 0];
            const int g = pixels[i * 4 + 1];
            const int b = pixels[i * 4 + 2];
            if (r > 230 && g > 80 && g < 160 && b < 60)
                ++count;
        }
        return count;
    }
}  // namespace

int main(int, char**)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[imgui_golden_gl] initializing headless OpenGL ImGui test...\n");

    if (!glfwInit())
    {
        printf("[imgui_golden_gl] SKIP: GLFW initialization failed\n");
        return kExitSkip;
    }

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif

    g_Window = glfwCreateWindow(64, 64, "imgui_golden_gl", nullptr, nullptr);
    if (!g_Window)
    {
        printf("[imgui_golden_gl] SKIP: OpenGL context creation failed\n");
        glfwTerminate();
        return kExitSkip;
    }

    glfwMakeContextCurrent(g_Window);

    if (!LoadGL())
    {
        printf("[imgui_golden_gl] SKIP: OpenGL functions not available\n");
        glfwDestroyWindow(g_Window);
        glfwTerminate();
        return kExitSkip;
    }

    const float scales[] = { 1.0f, 2.0f, 3.0f };
    for (float scale : scales)
    {
        printf("[imgui_golden_gl] testing scale %.1fx...\n", scale);
        std::vector<unsigned char> slugPixels, atlasPixels;
        uint32_t sw = 0, sh = 0, aw = 0, ah = 0;

        if (!RenderVariant(true, scale, 0, slugPixels, sw, sh))
        {
            printf("[imgui_golden_gl] FAIL: Slug render failed at scale %.1f\n", scale);
            return 1;
        }
        if (!RenderVariant(false, scale, 0, atlasPixels, aw, ah))
        {
            printf("[imgui_golden_gl] FAIL: Atlas render failed at scale %.1f\n", scale);
            return 1;
        }

        std::vector<unsigned char> maskSlug(sw * sh);
        std::vector<unsigned char> maskAtlas(aw * ah);
        BuildInkMask(slugPixels.data(), maskSlug.data(), sw, sh);
        BuildInkMask(atlasPixels.data(), maskAtlas.data(), aw, ah);

        int missSlug = 0, missAtlas = 0;
        MasksAgree(maskSlug.data(), maskAtlas.data(), sw, sh, &missSlug, &missAtlas);
        printf("  scale %.1fx (fb %ux%u): missSlug=%d, missAtlas=%d\n", scale, sw, sh, missSlug, missAtlas);

        // Allow up to a tiny fraction of pixels at antialiased edge boundaries
        const int allowedMisses = (int)(sw * sh * 0.001f) + 50;
        TEST_CHECK(missSlug <= allowedMisses);
        TEST_CHECK(missAtlas <= allowedMisses);
    }

    // Test debug overlay bounds
    {
        printf("[imgui_golden_gl] testing debug overlay bounds...\n");
        std::vector<unsigned char> boundsPixels;
        uint32_t bw = 0, bh = 0;
        if (RenderVariant(true, 1.0f, ImSlugOpenGL3DebugOverlay_GlyphBounds, boundsPixels, bw, bh))
        {
            int boundsCount = CountGlyphBoundsPixels(boundsPixels.data(), bw, bh);
            printf("  glyph bounds pixels found: %d\n", boundsCount);
            TEST_CHECK(boundsCount > 100);
        }
    }

    if (g_Target.Fbo != 0)
    {
        p_glDeleteFramebuffers(1, &g_Target.Fbo);
        p_glDeleteTextures(1, &g_Target.ColorTex);
    }

    glfwDestroyWindow(g_Window);
    glfwTerminate();

    printf("[imgui_golden_gl] OK: all ImGui golden tests passed!\n");
    return 0;
}
