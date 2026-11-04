// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// tests/golden/test_emoji_golden_opengl3.cpp — headless offscreen OpenGL 3.2+
// golden tests for COLR v1 color fonts: emoji strings at several bake sizes and
// zoom scales, rendered through the real Slug OpenGL pipeline (one instance per
// color layer, gradient paints evaluated in slug.frag via GL_RGBA32UI paint
// texture) and compared per pixel against the CPU arbiter: ImSlugRasterSample
// for coverage plus ImSlugSamplePaintRecord for the paint, composited with
// straight-alpha blend into an 8-bit destination.
//
// The emoji font (Noto Color Emoji, OFL) is a fetched, gitignored asset
// (scripts/fetch_emoji_font.*; tests/fonts/emoji/PROVENANCE.txt). The suite
// exits with code 77 (ctest SKIP_RETURN_CODE) when the asset is absent or no
// OpenGL 3.2+ context is usable.

#include "../test_util.h"

#include "imslug_impl_opengl3.h"
#include "imslug_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <GLFW/glfw3.h>

using namespace ImSlug;

namespace
{
    const int kExitSkip = 77;
    const float kMargin = 6.0f;

    const int kMaxChannelDiff = 4;
    const double kMaxMeanDiff = 0.25;
    const int kOutlierDiff = 10;
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
    static const GLenum GL_SCISSOR_TEST_CONST = 0x0C11;

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

    struct EmojiCase
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
        ImSlugFont* Font = nullptr;
        ImSlugPools Pools = {};
        Target CurrentTarget = {};
    };

    static State g_State = {};

    struct CpuLayer
    {
        const ImSlugGlyphRecord* Record;
        const ImSlugPaintRecord* Paint;
        ImSlugGlyphInstance Instance;
    };

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
            fprintf(stderr, "[emoji_golden_gl] incomplete FBO status: 0x%04X\n", status);
            return false;
        }
        return true;
    }

    static void DestroyTarget(Target* target)
    {
        if (target->Fbo != 0)
        {
            p_glDeleteFramebuffers(1, &target->Fbo);
            target->Fbo = 0;
        }
        if (target->ColorTex != 0)
        {
            p_glDeleteTextures(1, &target->ColorTex);
            target->ColorTex = 0;
        }
        target->Width = 0;
        target->Height = 0;
    }

    static void CompositeCpu(const CpuLayer* layers, int layerCount, unsigned char* destination, uint32_t width,
                             uint32_t height)
    {
        for (int i = 0; i < layerCount; ++i)
        {
            const ImSlugGlyphInstance& instance = layers[i].Instance;
            const float ppemX = 1.0f / instance.Jac00;
            const float ppemY = -1.0f / instance.Jac11;
            const float regionMinX = instance.ScreenMinX - 0.5f;
            const float regionMaxX = instance.ScreenMaxX + 0.5f;
            const float regionMinY = instance.ScreenMinY - 0.5f;
            const float regionMaxY = instance.ScreenMaxY + 0.5f;

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
                    const float emX = instance.EmMinX + (sx - instance.ScreenMinX) * instance.Jac00 +
                                      (sy - instance.ScreenMaxY) * instance.Jac10;
                    const float emY = instance.EmMinY + (sx - instance.ScreenMinX) * instance.Jac01 +
                                      (sy - instance.ScreenMaxY) * instance.Jac11;
                    const float coverage = ImSlugRasterSample(&g_State.Pools, layers[i].Record, emX, emY, ppemX, ppemY);
                    if (coverage < 0.001f)
                        continue;

                    float paint[4];
                    ImSlugSamplePaintRecord(layers[i].Paint, emX, emY, paint);
                    const float srcA = paint[3] * instance.ColorA * coverage;
                    const float srcR = paint[0] * srcA;
                    const float srcG = paint[1] * srcA;
                    const float srcB = paint[2] * srcA;

                    unsigned char* texel = destination + ((size_t)py * width + (size_t)px) * 4;
                    const float dstR = (float)texel[0] / 255.0f;
                    const float dstG = (float)texel[1] / 255.0f;
                    const float dstB = (float)texel[2] / 255.0f;
                    const float dstA = (float)texel[3] / 255.0f;
                    const float outR = srcR + dstR * (1.0f - srcA);
                    const float outG = srcG + dstG * (1.0f - srcA);
                    const float outB = srcB + dstB * (1.0f - srcA);
                    const float outA = srcA + dstA * (1.0f - srcA);

                    texel[0] = (unsigned char)(outR <= 0.0f ? 0.0f : (outR >= 1.0f ? 255.0f : outR * 255.0f + 0.5f));
                    texel[1] = (unsigned char)(outG <= 0.0f ? 0.0f : (outG >= 1.0f ? 255.0f : outG * 255.0f + 0.5f));
                    texel[2] = (unsigned char)(outB <= 0.0f ? 0.0f : (outB >= 1.0f ? 255.0f : outB * 255.0f + 0.5f));
                    texel[3] = (unsigned char)(outA <= 0.0f ? 0.0f : (outA >= 1.0f ? 255.0f : outA * 255.0f + 0.5f));
                }
            }
        }
    }

    static bool WritePpm(const char* path, const unsigned char* pixels, uint32_t width, uint32_t height)
    {
        FILE* file = fopen(path, "wb");
        if (file == nullptr)
            return false;
        fprintf(file, "P6\n%u %u\n255\n", width, height);
        for (uint32_t y = 0; y < height; ++y)
        {
            const unsigned char* row = pixels + (size_t)y * width * 4;
            for (uint32_t x = 0; x < width; ++x)
            {
                unsigned char rgb[3] = { row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2] };
                fwrite(rgb, 1, 3, file);
            }
        }
        fclose(file);
        return true;
    }

    struct DiffStats
    {
        int MaxChannel;
        double MeanChannel;
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
                int pixelWorst = 0;
                for (int channel = 0; channel < 4; ++channel)
                {
                    const int diff = a[channel] > b[channel] ? a[channel] - b[channel] : b[channel] - a[channel];
                    if (diff > stats.MaxChannel)
                        stats.MaxChannel = diff;
                    if (diff > kMaxChannelDiff)
                        ++stats.OverThreshold;
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

    static int LayoutEmojiText(const char* text, float bakeSize, float scale, float originX, float originY,
                               ImSlugGlyphInstance* outInstances, CpuLayer* outLayers, int capacity)
    {
        const float pixelsPerEm = bakeSize * scale;
        int instanceCount = 0;
        float penX = originX;
        const char* s = text;
        while (*s != '\0' && instanceCount < capacity)
        {
            unsigned int codepoint = (unsigned char)*s++;
            if (codepoint >= 0x80)
            {
                if ((codepoint & 0xE0) == 0xC0)
                {
                    codepoint = ((codepoint & 0x1F) << 6) | ((unsigned char)*s++ & 0x3F);
                }
                else if ((codepoint & 0xF0) == 0xE0)
                {
                    codepoint = ((codepoint & 0x0F) << 12) | (((unsigned char)*s++ & 0x3F) << 6);
                    codepoint |= ((unsigned char)*s++ & 0x3F);
                }
                else if ((codepoint & 0xF8) == 0xF0)
                {
                    codepoint = ((codepoint & 0x07) << 18) | (((unsigned char)*s++ & 0x3F) << 12);
                    codepoint |= (((unsigned char)*s++ & 0x3F) << 6) | ((unsigned char)*s++ & 0x3F);
                }
            }

            const int glyphIndex = ImSlugGetGlyphIndex(g_State.Font, codepoint);
            if (glyphIndex == 0)
                continue;
            ImSlugGlyphMetrics metrics;
            if (!ImSlugGetGlyphMetricsByIndex(g_State.Font, glyphIndex, bakeSize, &metrics))
                continue;
            const ImSlugColorGlyph* colorGlyph = nullptr;
            if (ImSlugGetColorGlyph(g_State.Font, &g_State.Pools, glyphIndex, &colorGlyph) != ImSlugBuildStatus_Ok ||
                colorGlyph == nullptr || colorGlyph->LayerCount == 0)
            {
                continue;
            }

            if (!(metrics.X1 > metrics.X0) || !(metrics.Y1 > metrics.Y0))
            {
                ImSlugFontMetrics fontMetrics;
                ImSlugGetFontMetrics(g_State.Font, bakeSize, &fontMetrics);
                const float ascentRounded = std::floor(fontMetrics.Ascent + 0.5f);
                metrics.X0 = std::floor(colorGlyph->MinX * pixelsPerEm);
                metrics.X1 = std::ceil(colorGlyph->MaxX * pixelsPerEm);
                metrics.Y0 = std::floor(-colorGlyph->MaxY * pixelsPerEm) + ascentRounded;
                metrics.Y1 = std::ceil(-colorGlyph->MinY * pixelsPerEm) + ascentRounded;
            }

            static int layerIndices[4096];
            const int count =
                ImSlugBuildColorGlyphInstances(colorGlyph, penX, originY, pixelsPerEm, 1.0f,
                                               outInstances + instanceCount, capacity - instanceCount, layerIndices);
            for (int i = 0; i < count; ++i)
            {
                outLayers[instanceCount + i].Instance = outInstances[instanceCount + i];
                const ImSlugColorLayer& layer = colorGlyph->Layers[layerIndices[i]];
                outLayers[instanceCount + i].Record = layer.Record;
                outLayers[instanceCount + i].Paint = ImSlugPaintRecordAt(&g_State.Pools.Paints, (int)layer.PaintIndex);
            }
            instanceCount += count;
            penX += metrics.AdvanceX * scale;
        }
        return instanceCount;
    }
}  // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    int fontSize = 0;
    unsigned char* fontData = imslug_test::LoadFile(IMSLUG_EMOJI_TEST_FONT, &fontSize);
    if (fontData == nullptr)
    {
        std::printf(
            "imslug_emoji_golden_opengl3: SKIP (emoji font not present; fetch with scripts/fetch_emoji_font.*)\n");
        return kExitSkip;
    }

    if (!glfwInit())
    {
        std::free(fontData);
        std::printf("imslug_emoji_golden_opengl3: SKIP (glfwInit failed)\n");
        return kExitSkip;
    }

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    g_State.Window = glfwCreateWindow(640, 480, "emoji golden opengl3", nullptr, nullptr);
    if (!g_State.Window)
    {
        std::free(fontData);
        glfwTerminate();
        std::printf("imslug_emoji_golden_opengl3: SKIP (OpenGL 3.2 Core context unavailable)\n");
        return kExitSkip;
    }

    glfwMakeContextCurrent(g_State.Window);

    if (!LoadFboProcs())
    {
        std::free(fontData);
        glfwDestroyWindow(g_State.Window);
        glfwTerminate();
        std::printf("imslug_emoji_golden_opengl3: SKIP (FBO procs missing)\n");
        return kExitSkip;
    }

    ImSlugOpenGL3InitInfo initInfo = {};
    if (!ImSlugOpenGL3Init(&initInfo))
    {
        std::free(fontData);
        glfwDestroyWindow(g_State.Window);
        glfwTerminate();
        std::printf("imslug_emoji_golden_opengl3: FAIL (ImSlugOpenGL3Init failed)\n");
        return 1;
    }

    g_State.Font = ImSlugCreateFontFromMemory(fontData, fontSize, 0, true);
    std::free(fontData);
    if (g_State.Font == nullptr)
    {
        std::printf("imslug_emoji_golden_opengl3: FAIL (font load)\n");
        ImSlugOpenGL3Shutdown();
        glfwDestroyWindow(g_State.Window);
        glfwTerminate();
        return 1;
    }
    ImSlugInitPools(&g_State.Pools);

    static const char* kTexts[5] = {
        "\xF0\x9F\x98\x80",                                                          // U+1F600 grinning face
        "\xE2\x9D\xA4",                                                              // U+2764 red heart
        "\xF0\x9F\x98\x80\xF0\x9F\x94\xA5\xE2\x9D\xA4",                              // face + fire + heart
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7",  // family
        "\xF0\x9F\x90\x8C",                                                          // U+1F40C snail (mascot)
    };
    static const float kBakeSizes[3] = { 16.0f, 32.0f, 64.0f };
    static const float kScales[3] = { 1.0f, 2.0f, 3.5f };

    EmojiCase cases[64];
    int caseCount = 0;
    for (int textIndex = 0; textIndex < 5; ++textIndex)
    {
        for (int sizeIndex = 0; sizeIndex < 3; ++sizeIndex)
        {
            for (int scaleIndex = 0; scaleIndex < 3; ++scaleIndex)
            {
                if (textIndex >= 2 && scaleIndex == 2)
                    continue;
                EmojiCase& testCase = cases[caseCount++];
                std::snprintf(testCase.Name, sizeof(testCase.Name), "e%d_%dpx_%dmilli", textIndex,
                              (int)kBakeSizes[sizeIndex], (int)(kScales[scaleIndex] * 1000.0f));
                testCase.Text = kTexts[textIndex];
                testCase.BakeSize = kBakeSizes[sizeIndex];
                testCase.Scale = kScales[scaleIndex];
            }
        }
    }

    int failures = 0;
    int worstMax = 0;
    double worstMean = 0.0;
    char worstName[64] = "";
    int totalInstances = 0;

    for (int caseIndex = 0; caseIndex < caseCount; ++caseIndex)
    {
        const EmojiCase& testCase = cases[caseIndex];

        const uint32_t capacity = ImSlugOpenGL3GetInstanceCapacity();
        ImSlugGlyphInstance* instances = ImSlugOpenGL3GetInstanceData(0);
        static CpuLayer layers[4096];
        static ImSlugGlyphInstance layoutInstances[4096];
        int count = LayoutEmojiText(testCase.Text, testCase.BakeSize, testCase.Scale, 0.0f, 0.0f, layoutInstances,
                                    layers, 4096);
        if (count <= 0 || (uint32_t)count > capacity)
        {
            std::printf("imslug_emoji_golden_opengl3: FAIL (%s: no layers / too many: %d)\n", testCase.Name, count);
            ++failures;
            continue;
        }

        float minX = layoutInstances[0].ScreenMinX, maxX = layoutInstances[0].ScreenMaxX;
        float minY = layoutInstances[0].ScreenMinY, maxY = layoutInstances[0].ScreenMaxY;
        for (int i = 1; i < count; ++i)
        {
            minX = layoutInstances[i].ScreenMinX < minX ? layoutInstances[i].ScreenMinX : minX;
            maxX = layoutInstances[i].ScreenMaxX > maxX ? layoutInstances[i].ScreenMaxX : maxX;
            minY = layoutInstances[i].ScreenMinY < minY ? layoutInstances[i].ScreenMinY : minY;
            maxY = layoutInstances[i].ScreenMaxY > maxY ? layoutInstances[i].ScreenMaxY : maxY;
        }
        const float offsetX = kMargin - minX + 0.0137f;
        const float offsetY = kMargin - minY + 0.0113f;

        count = LayoutEmojiText(testCase.Text, testCase.BakeSize, testCase.Scale, offsetX, offsetY, instances, layers,
                                4096);
        uint32_t width = (uint32_t)std::ceil(maxX - minX) + (uint32_t)(kMargin * 2.0f) + 4;
        uint32_t height = (uint32_t)std::ceil(maxY - minY) + (uint32_t)(kMargin * 2.0f) + 4;
        width = (width + 3) & ~3u;
        height = (height + 3) & ~3u;

        if (!CreateTarget(width, height, &g_State.CurrentTarget))
        {
            std::printf("imslug_emoji_golden_opengl3: FAIL (%s: create target)\n", testCase.Name);
            ++failures;
            continue;
        }

        p_glBindFramebuffer(GL_FRAMEBUFFER_CONST, g_State.CurrentTarget.Fbo);
        if (p_glViewport)
            p_glViewport(0, 0, (GLsizei)width, (GLsizei)height);
        if (p_glDisable)
            p_glDisable(GL_SCISSOR_TEST_CONST);
        p_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        p_glClear(GL_COLOR_BUFFER_BIT_CONST);

        if (!ImSlugOpenGL3UploadPools(&g_State.Pools))
        {
            std::printf("imslug_emoji_golden_opengl3: FAIL (%s: pool upload failed)\n", testCase.Name);
            ++failures;
            continue;
        }

        ImSlugOpenGL3UploadInstances(0, (uint32_t)count);

        float mvp[16];
        ImSlugOpenGL3BuildOrthoMVP(0.0f, 0.0f, (float)width, (float)height, mvp, false);

        ImSlugOpenGL3DrawInfo drawInfo = {};
        for (int i = 0; i < 16; ++i)
            drawInfo.MVP[i] = mvp[i];
        drawInfo.ViewportWidth = (float)width;
        drawInfo.ViewportHeight = (float)height;
        drawInfo.ScissorX = 0;
        drawInfo.ScissorY = 0;
        drawInfo.ScissorW = width;
        drawInfo.ScissorH = height;
        drawInfo.FrameIndex = 0;
        drawInfo.FirstInstance = 0;
        drawInfo.InstanceCount = (uint32_t)count;
        drawInfo.DebugFlags = 0;

        ImSlugOpenGL3Draw(&drawInfo);

        std::vector<unsigned char> gpuPixels((size_t)width * height * 4);
        p_glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA_CONST, GL_UNSIGNED_BYTE_CONST, gpuPixels.data());

        std::vector<unsigned char> gpuFlipped((size_t)width * height * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            const unsigned char* src = gpuPixels.data() + (size_t)(height - 1 - y) * width * 4;
            unsigned char* dst = gpuFlipped.data() + (size_t)y * width * 4;
            std::memcpy(dst, src, (size_t)width * 4);
        }

        std::vector<unsigned char> cpuPixels((size_t)width * height * 4, 0);
        CompositeCpu(layers, count, cpuPixels.data(), width, height);

        const DiffStats stats = Compare(gpuFlipped.data(), cpuPixels.data(), width, height);
        const int outlierBudget = (int)((double)(width * height) * kOutlierFraction) + kMinOutlierBudget;
        const bool pass = stats.MeanChannel <= kMaxMeanDiff && stats.OutlierPixels <= outlierBudget;
        std::printf("  %-22s %4ux%-4u %5d layers  max %3d  mean %6.3f  outliers %d/%d (worst %d)%s\n", testCase.Name,
                    width, height, count, stats.MaxChannel, stats.MeanChannel, stats.OutlierPixels, outlierBudget,
                    stats.OutlierWorst, pass ? "" : "  <-- FAIL");
        if (stats.MaxChannel > worstMax)
            worstMax = stats.MaxChannel;
        if (stats.MeanChannel > worstMean)
        {
            worstMean = stats.MeanChannel;
            std::snprintf(worstName, sizeof(worstName), "%s", testCase.Name);
        }
        if (!pass)
        {
            ++failures;
            char path[256];
            std::snprintf(path, sizeof(path), "%s_gl_gpu.ppm", testCase.Name);
            WritePpm(path, gpuFlipped.data(), width, height);
            std::snprintf(path, sizeof(path), "%s_gl_cpu.ppm", testCase.Name);
            WritePpm(path, cpuPixels.data(), width, height);
        }
    }

    DestroyTarget(&g_State.CurrentTarget);
    ImSlugDestroyFont(g_State.Font);
    ImSlugDestroyPools(&g_State.Pools);
    ImSlugOpenGL3Shutdown();
    glfwDestroyWindow(g_State.Window);
    glfwTerminate();

    std::printf(
        "imslug_emoji_golden_opengl3: %s (%d cases, %d layer instances, worst max %d LSB, worst mean %.3f (%s))\n",
        failures == 0 ? "OK" : "FAILED", caseCount, totalInstances, worstMax, worstMean,
        worstName[0] != '\0' ? worstName : "-");
    return failures == 0 ? 0 : 1;
}
