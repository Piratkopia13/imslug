// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// tests/golden/test_golden_opengl3.cpp — headless offscreen OpenGL 3.2+ golden
// tests: fixed strings at sizes 12/16/32/64 and DPI scales 1.0/1.25/2.0
// rendered through the real Slug OpenGL pipeline and compared per pixel against
// the CPU reference rasterizer (imslug_refraster.cpp — the arbiter).

#include "../test_util.h"

#include "imslug_impl_opengl3.h"
#include "imslug_internal.h"
#include "imslug_packed.h"

#define IMSLUG_C_OPENGL3 1
#include "imslug_c.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <GLFW/glfw3.h>

using namespace ImSlug;

#ifndef IMSLUG_PACKED_TEST_ASSET
#define IMSLUG_PACKED_TEST_ASSET "tests/assets/Roboto-ASCII.imslug"
#endif

namespace
{
    const int kExitSkip = 77;
    const float kMargin = 6.0f;

    const int kMaxChannelDiff = 3;
    const double kMaxMeanDiff = 0.25;
    const int kOutlierDiff = 8;
    const double kOutlierFraction = 0.0005;
    const int kMinOutlierBudget = 4;

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

    static bool LoadFboProcs()
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

    struct Case
    {
        char Name[64];
        const char* Text;
        float BakeSize;
        float Scale;
    };

    struct Target
    {
        GLuint Fbo = 0;
        GLuint ColorTex = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
    };

    struct State
    {
        GLFWwindow* Window = nullptr;
        ImSlugPools Pools = {};
        Target CurrentTarget = {};
    };

    static State g_State = {};

    static bool CreateTarget(uint32_t width, uint32_t height, Target* outTarget)
    {
        if (outTarget->Fbo != 0)
        {
            if (outTarget->Width == width && outTarget->Height == height)
                return true;
            p_glDeleteFramebuffers(1, &outTarget->Fbo);
            p_glDeleteTextures(1, &outTarget->ColorTex);
            outTarget->Fbo = 0;
            outTarget->ColorTex = 0;
        }

        outTarget->Width = width;
        outTarget->Height = height;

        p_glGenTextures(1, &outTarget->ColorTex);
        p_glBindTexture(GL_TEXTURE_2D_CONST, outTarget->ColorTex);
        p_glTexImage2D(GL_TEXTURE_2D_CONST, 0, GL_RGBA8_CONST, (GLsizei)width, (GLsizei)height, 0, GL_RGBA_CONST,
                       GL_UNSIGNED_BYTE_CONST, nullptr);

        p_glGenFramebuffers(1, &outTarget->Fbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER_CONST, outTarget->Fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER_CONST, GL_COLOR_ATTACHMENT0_CONST, GL_TEXTURE_2D_CONST,
                                 outTarget->ColorTex, 0);

        GLenum status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER_CONST);
        if (status != GL_FRAMEBUFFER_COMPLETE_CONST)
        {
            fprintf(stderr, "[golden_gl] incomplete FBO status: 0x%04X\n", status);
            return false;
        }
        return true;
    }

    struct CpuGlyph
    {
        const ImSlugGlyphRecord* Record;
        ImSlugGlyphInstance Instance;
    };

    static void CompositeCpu(const CpuGlyph* glyphs, int glyphCount, float pixelsPerEm, unsigned char* destination,
                             uint32_t width, uint32_t height)
    {
        for (int i = 0; i < glyphCount; ++i)
        {
            const ImSlugGlyphInstance& g = glyphs[i].Instance;
            const float regionMinX = g.ScreenMinX - 0.5f;
            const float regionMaxX = g.ScreenMaxX + 0.5f;
            const float regionMinY = g.ScreenMinY - 0.5f;
            const float regionMaxY = g.ScreenMaxY + 0.5f;

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
                    const float emX = g.EmMinX + (sx - g.ScreenMinX) * g.Jac00 + (sy - g.ScreenMaxY) * g.Jac10;
                    const float emY = g.EmMinY + (sx - g.ScreenMinX) * g.Jac01 + (sy - g.ScreenMaxY) * g.Jac11;
                    const float coverage =
                        ImSlugRasterSample(&g_State.Pools, glyphs[i].Record, emX, emY, pixelsPerEm, pixelsPerEm);
                    if (coverage < 0.001f)
                        continue;

                    const float srcA = g.ColorA * coverage;
                    const float srcR = g.ColorR * srcA;
                    const float srcG = g.ColorG * srcA;
                    const float srcB = g.ColorB * srcA;

                    unsigned char* texel = destination + ((size_t)py * width + (size_t)px) * 4;
                    const float dstR = (float)texel[0] / 255.0f;
                    const float dstG = (float)texel[1] / 255.0f;
                    const float dstB = (float)texel[2] / 255.0f;
                    const float dstA = (float)texel[3] / 255.0f;

                    const float outR = srcR + dstR * (1.0f - srcA);
                    const float outG = srcG + dstG * (1.0f - srcA);
                    const float outB = srcB + dstB * (1.0f - srcA);
                    const float outA = srcA + dstA * (1.0f - srcA);

                    texel[0] = (unsigned char)(std::clamp(outR, 0.0f, 1.0f) * 255.0f + 0.5f);
                    texel[1] = (unsigned char)(std::clamp(outG, 0.0f, 1.0f) * 255.0f + 0.5f);
                    texel[2] = (unsigned char)(std::clamp(outB, 0.0f, 1.0f) * 255.0f + 0.5f);
                    texel[3] = (unsigned char)(std::clamp(outA, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
    }

    static void WritePpm(const char* path, const unsigned char* pixels, uint32_t width, uint32_t height)
    {
        FILE* file = fopen(path, "wb");
        if (file == nullptr)
            return;
        fprintf(file, "P6\n%u %u\n255\n", width, height);
        unsigned char* row = (unsigned char*)malloc((size_t)width * 3);
        if (row != nullptr)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                const unsigned char* src = pixels + (size_t)y * width * 4;
                for (uint32_t x = 0; x < width; ++x)
                {
                    row[x * 3 + 0] = src[x * 4 + 0];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4 + 2];
                }
                fwrite(row, 3, width, file);
            }
            free(row);
        }
        fclose(file);
    }

    struct DiffStats
    {
        int MaxChannel;
        double MeanChannel;
        int MaxDeep;
        int OverThreshold;
        int OutlierPixels;
        int OutlierWorst;
    };

    static DiffStats Compare(const unsigned char* gpu, const unsigned char* cpu, uint32_t width, uint32_t height)
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
                const bool deep = b[3] >= 13 && b[3] <= 242;
                int pixelWorst = 0;
                for (int channel = 0; channel < 4; ++channel)
                {
                    const int diff = a[channel] > b[channel] ? a[channel] - b[channel] : b[channel] - a[channel];
                    if (diff > stats.MaxChannel)
                        stats.MaxChannel = diff;
                    if (diff > kMaxChannelDiff)
                        ++stats.OverThreshold;
                    if (deep && channel < 3 && diff > stats.MaxDeep)
                        stats.MaxDeep = diff;
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

    static bool CompareImages(const Case& c, const unsigned char* gpuFlipped, const unsigned char* cpu, uint32_t width,
                              uint32_t height)
    {
        const DiffStats stats = Compare(gpuFlipped, cpu, width, height);
        const int outlierBudget = (int)((double)(width * height) * kOutlierFraction) + kMinOutlierBudget;
        const bool pass = stats.MeanChannel <= kMaxMeanDiff && stats.OutlierPixels <= outlierBudget;

        printf("  %-22s %4ux%-4u max=%d mean=%.3f deep=%d over=%d outliers=%d/%d (worst %d)  %s\n", c.Name, width,
               height, stats.MaxChannel, stats.MeanChannel, stats.MaxDeep, stats.OverThreshold, stats.OutlierPixels,
               outlierBudget, stats.OutlierWorst, pass ? "ok" : "FAIL");

        if (pass)
            return true;

        char gpuPath[128], cpuPath[128], diffPath[128];
        snprintf(gpuPath, sizeof(gpuPath), "%s_gl_gpu.ppm", c.Name);
        snprintf(cpuPath, sizeof(cpuPath), "%s_gl_cpu.ppm", c.Name);
        snprintf(diffPath, sizeof(diffPath), "%s_gl_diff.ppm", c.Name);
        WritePpm(gpuPath, gpuFlipped, width, height);
        WritePpm(cpuPath, cpu, width, height);

        const size_t totalPixels = (size_t)width * height;
        unsigned char* diffImg = (unsigned char*)calloc(totalPixels * 4, 1);
        if (diffImg)
        {
            for (size_t i = 0; i < totalPixels; ++i)
            {
                int rDiff = std::abs((int)gpuFlipped[i * 4 + 0] - (int)cpu[i * 4 + 0]);
                int gDiff = std::abs((int)gpuFlipped[i * 4 + 1] - (int)cpu[i * 4 + 1]);
                int bDiff = std::abs((int)gpuFlipped[i * 4 + 2] - (int)cpu[i * 4 + 2]);
                int aDiff = std::abs((int)gpuFlipped[i * 4 + 3] - (int)cpu[i * 4 + 3]);
                diffImg[i * 4 + 0] = (unsigned char)std::min(255, rDiff * 16);
                diffImg[i * 4 + 1] = (unsigned char)std::min(255, gDiff * 16);
                diffImg[i * 4 + 2] = (unsigned char)std::min(255, bDiff * 16);
                diffImg[i * 4 + 3] = (unsigned char)std::min(255, aDiff * 16);
            }
            WritePpm(diffPath, diffImg, width, height);
            free(diffImg);
        }
        return false;
    }

    static bool RunCase(const Case& testCase, ImSlugFont* font, float bakeSize, float scale)
    {
        const float pixelsPerEm = bakeSize * scale;

        // Measure line layout
        float penX = kMargin;
        float penY = kMargin + bakeSize * scale * 0.8f;
        std::vector<CpuGlyph> glyphs;

        for (const char* p = testCase.Text; *p != '\0'; ++p)
        {
            uint32_t codepoint = (unsigned char)*p;
            const ImSlugGlyphRecord* rec = nullptr;
            if (ImSlugGetGlyphRecord(font, &g_State.Pools, codepoint, &rec) != ImSlugBuildStatus_Ok || !rec)
                continue;
            if (rec->CurveCount == 0)
            {
                penX += rec->Advance * pixelsPerEm;
                continue;
            }

            CpuGlyph g = {};
            g.Record = rec;
            ImSlugBuildGlyphInstance(rec, penX, penY, pixelsPerEm, 1.0f, 1.0f, 1.0f, 1.0f, &g.Instance);
            glyphs.push_back(g);

            penX += rec->Advance * pixelsPerEm;
        }

        uint32_t width = (uint32_t)std::ceil(penX + kMargin);
        uint32_t height = (uint32_t)std::ceil(penY + bakeSize * scale * 0.4f + kMargin);
        width = (width + 3) & ~3u;
        height = (height + 3) & ~3u;

        if (!CreateTarget(width, height, &g_State.CurrentTarget))
            return false;

        // 1. Render on GPU
        p_glBindFramebuffer(GL_FRAMEBUFFER_CONST, g_State.CurrentTarget.Fbo);
        if (p_glDisable)
            p_glDisable(0x0C11 /* GL_SCISSOR_TEST */);
        if (p_glViewport)
            p_glViewport(0, 0, (GLsizei)width, (GLsizei)height);
        p_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        p_glClear(GL_COLOR_BUFFER_BIT_CONST);

        if (!ImSlugOpenGL3UploadPools(&g_State.Pools))
            return false;

        ImSlugGlyphInstance* instDst = ImSlugOpenGL3GetInstanceData(0);
        for (size_t i = 0; i < glyphs.size(); ++i)
            instDst[i] = glyphs[i].Instance;
        ImSlugOpenGL3UploadInstances(0, (uint32_t)glyphs.size());

        float mvp[16];
        ImSlugOpenGL3BuildOrthoMVP(0.0f, 0.0f, (float)width, (float)height, mvp, false);

        ImSlugOpenGL3DrawInfo draw = {};
        for (int i = 0; i < 16; ++i)
            draw.MVP[i] = mvp[i];
        draw.ViewportWidth = (float)width;
        draw.ViewportHeight = (float)height;
        draw.ScissorX = 0;
        draw.ScissorY = 0;
        draw.ScissorW = width;
        draw.ScissorH = height;
        draw.FrameIndex = 0;
        draw.FirstInstance = 0;
        draw.InstanceCount = (uint32_t)glyphs.size();
        draw.DebugFlags = 0;
        ImSlugOpenGL3Draw(&draw);

        // Read back GPU pixels
        std::vector<unsigned char> gpuPixels((size_t)width * height * 4);
        p_glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA_CONST, GL_UNSIGNED_BYTE_CONST, gpuPixels.data());

        // Flip vertically for comparison (glReadPixels is bottom-up, CPU reference is top-down)
        std::vector<unsigned char> gpuFlipped((size_t)width * height * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            const unsigned char* src = gpuPixels.data() + (size_t)(height - 1 - y) * width * 4;
            unsigned char* dst = gpuFlipped.data() + (size_t)y * width * 4;
            memcpy(dst, src, (size_t)width * 4);
        }

        // 2. Render CPU reference
        std::vector<unsigned char> cpuPixels((size_t)width * height * 4);
        std::memset(cpuPixels.data(), 0, cpuPixels.size());
        CompositeCpu(glyphs.data(), (int)glyphs.size(), pixelsPerEm, cpuPixels.data(), width, height);

        return CompareImages(testCase, gpuFlipped.data(), cpuPixels.data(), width, height);
    }

    static bool TestStandaloneContract()
    {
        printf("[golden_gl] running backend contract tests...\n");

        // 1. Reserve instances growth
        uint32_t cap = ImSlugOpenGL3GetInstanceCapacity();
        TEST_CHECK(cap >= 1024);
        TEST_CHECK(ImSlugOpenGL3ReserveInstances(0, cap * 2));
        TEST_CHECK(ImSlugOpenGL3GetInstanceCapacity() >= cap * 2);

        // 2. Frame ring slots
        uint32_t frames = ImSlugOpenGL3GetFramesInFlight();
        TEST_CHECK(frames >= 2);
        for (uint32_t i = 0; i < frames; ++i)
        {
            ImSlugGlyphInstance* data = ImSlugOpenGL3GetInstanceData(i);
            TEST_CHECK(data != nullptr);
        }
        TEST_CHECK(ImSlugOpenGL3GetInstanceData(frames) == nullptr);

        // 3. Zero instances draw (safe no-op)
        ImSlugOpenGL3DrawInfo draw = {};
        draw.InstanceCount = 0;
        ImSlugOpenGL3Draw(&draw);

        // 4. C11 API verification
        imslug_opengl3_init_info cInfo = imslug_opengl3_init_info_default();
        TEST_CHECK(cInfo.NumFramesInFlight == 0);
        float outMvp[16] = {};
        imslug_opengl3_build_ortho_mvp(0.0f, 0.0f, 100.0f, 100.0f, outMvp, 0);
        TEST_CHECK(outMvp[0] == 2.0f / 100.0f);
        TEST_CHECK(outMvp[15] == 1.0f);

        printf("[golden_gl] backend contract tests passed.\n");
        return true;
    }
}  // namespace

int main(int argc, char** argv)
{
    const char* packedAssetPath = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--packed") == 0)
            packedAssetPath = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : IMSLUG_PACKED_TEST_ASSET;
        else if (std::strcmp(argv[i], "--help") == 0)
        {
            std::printf("usage: imslug_tests_golden_opengl3 [--packed [asset.imslug]]\n");
            return 0;
        }
    }

#if defined(IMSLUG_NO_STBTT)
    if (packedAssetPath == nullptr)
        packedAssetPath = IMSLUG_PACKED_TEST_ASSET;
#endif

    printf("[golden_gl] initializing headless OpenGL 3.2+ harness...\n");

    if (!glfwInit())
    {
        printf("[golden_gl] SKIP: GLFW initialization failed (no display/GPU)\n");
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

    g_State.Window = glfwCreateWindow(64, 64, "golden_gl", nullptr, nullptr);
    if (g_State.Window == nullptr)
    {
        printf("[golden_gl] SKIP: OpenGL 3.2 context creation failed\n");
        glfwTerminate();
        return kExitSkip;
    }

    glfwMakeContextCurrent(g_State.Window);

    if (!LoadFboProcs())
    {
        printf("[golden_gl] SKIP: FBO functions not available\n");
        glfwDestroyWindow(g_State.Window);
        glfwTerminate();
        return kExitSkip;
    }

    ImSlugOpenGL3InitInfo initInfo = {};
    initInfo.NumFramesInFlight = 2;
    initInfo.MaxInstancesPerFrame = 0;  // adaptive growth
    if (!ImSlugOpenGL3Init(&initInfo))
    {
        printf("[golden_gl] FAIL: ImSlugOpenGL3Init failed\n");
        glfwDestroyWindow(g_State.Window);
        glfwTerminate();
        return 1;
    }

    // Initialize pools
    ImSlugInitPools(&g_State.Pools);

    // Run contract tests
    if (!TestStandaloneContract())
    {
        ImSlugOpenGL3Shutdown();
        glfwDestroyWindow(g_State.Window);
        glfwTerminate();
        return 1;
    }

    // Load test fonts
    int fontSize = 0;
    unsigned char* fontData = nullptr;
    ImSlugFont* font = nullptr;
    if (packedAssetPath != nullptr)
    {
        fontData = imslug_test::LoadFile(packedAssetPath, &fontSize);
        if (fontData == nullptr)
        {
            printf("[golden_gl] FAIL: packed asset not found: %s\n", packedAssetPath);
            return 1;
        }
        font = ImSlugCreateFontFromPackedMemory(fontData, fontSize);
    }
#if !defined(IMSLUG_NO_STBTT)
    else
    {
        fontData = imslug_test::LoadFile(IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf"), &fontSize);
        if (fontData == nullptr)
        {
            printf("[golden_gl] FAIL: could not load Roboto-Regular.ttf\n");
            return 1;
        }
        font = ImSlugCreateFontFromMemory(fontData, fontSize, 0, true);
    }
#endif
    if (fontData != nullptr)
        free(fontData);

    if (font == nullptr)
    {
        printf("[golden_gl] FAIL: font load failed\n");
        return 1;
    }

    // Test matrix
    const float sizes[] = { 12.0f, 16.0f, 32.0f, 64.0f };
    const float scales[] = { 1.0f, 1.25f, 2.0f };
    const char* sampleText = "The quick brown fox jumps over the lazy dog! 0123456789";

    bool allPassed = true;
    for (float sz : sizes)
    {
        for (float sc : scales)
        {
            Case c = {};
            snprintf(c.Name, sizeof(c.Name), "Roboto_sz%.0f_sc%.2f", sz, sc);
            c.Text = sampleText;
            c.BakeSize = sz;
            c.Scale = sc;

            if (!RunCase(c, font, sz, sc))
            {
                allPassed = false;
                break;
            }
        }
        if (!allPassed)
            break;
    }

#if !defined(IMSLUG_NO_STBTT)
    // Test committed packed asset when running default TTF mode
    if (packedAssetPath == nullptr)
    {
        int packedSize = 0;
        unsigned char* packedData = imslug_test::LoadFile(IMSLUG_PACKED_TEST_ASSET, &packedSize);
        if (packedData != nullptr)
        {
            ImSlugFont* packedFont = ImSlugCreateFontFromPackedMemory(packedData, packedSize);
            if (packedFont != nullptr)
            {
                Case c = {};
                snprintf(c.Name, sizeof(c.Name), "Packed_ASCII_sz24_sc1.00");
                c.Text = "Packed ASCII text rendering verification!";
                c.BakeSize = 24.0f;
                c.Scale = 1.0f;
                if (!RunCase(c, packedFont, 24.0f, 1.0f))
                    allPassed = false;
                ImSlugDestroyFont(packedFont);
            }
            free(packedData);
        }
    }
#endif

    ImSlugDestroyFont(font);
    ImSlugDestroyPools(&g_State.Pools);

    if (g_State.CurrentTarget.Fbo != 0)
    {
        p_glDeleteFramebuffers(1, &g_State.CurrentTarget.Fbo);
        p_glDeleteTextures(1, &g_State.CurrentTarget.ColorTex);
    }

    ImSlugOpenGL3Shutdown();
    glfwDestroyWindow(g_State.Window);
    glfwTerminate();

    if (allPassed)
    {
        printf("[golden_gl] OK: all golden comparison and contract tests passed!\n");
        return 0;
    }
    return 1;
}
