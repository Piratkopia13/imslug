// imslug — M4 benchmark harness.
//
// Two modes, both deterministic and reproducible on any machine:
//
//   --cpu   (default) CPU-side numbers, no Vulkan device needed: the pre-pass
//           breakdown (classify + rewrite + commit) at 1k/5k/20k glyphs, the
//           per-frame upload bytes from the pool dirty rows, the draw-call
//           count of a contiguous run, the text vertex/index waste (the input
//           for the M4 vertex-compaction decision) and the lazy glyph build
//           cost, plus the R8 small-size quality table (unhinted Slug vs the
//           atlas rasterizer at 8..32 px and the 4x-supersample ceiling).
//           These are the automated CPU-side checks CI can run.
//
//   --gpu   headless offscreen Vulkan: GPU time from timestamp queries for the
//           same scene rendered with the Slug font and with the atlas font
//           (Oversample 1, the metric-neutral configuration), so "Slug vs
//           stock" is an apples-to-apples comparison on the same device. Runs
//           the production order (UploadPools -> RenderDrawData, which runs the
//           pre-pass), reports the Slug instance count of the measured frame
//           and warns when the Slug row did not render any text. Validation
//           layers are on by default (--no-validation / IMSLUG_VALIDATION=0 to
//           skip); they do not change the GPU numbers on the test machine.
//           The manual cross-check procedure is in docs/DESIGN.md
//           ("Measuring performance").
//
//   --all   both, with a markdown report written to --report <path>.
//
// Usage via scripts/bench.* (builds the target, runs it, keeps the report in
// the build directory). Direct invocation:
//   imslug_tests_bench [--cpu|--gpu|--all] [--report <path>] [--frames N]
//                      [--no-validation]
//
// The scene matches the golden stress scene (38 lines x 132 chars at 17 px,
// ~4.8k visible glyphs) unless --lines/--columns override it.

#include "imslug.h"

#include "imslug_impl_vulkan.h"
#include "imslug_internal.h"

#include "test_util.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>  // assert() dialogs would block headless runs: report to stderr
#endif

#ifndef IMSLUG_CJK_TEST_FONT
#define IMSLUG_CJK_TEST_FONT "tests/fonts/cjk/NotoSansSC-subset.ttf"
#endif

using namespace ImSlug;

namespace
{
    // ---------------------------------------------------------------------
    // CLI / report
    // ---------------------------------------------------------------------
    struct Options
    {
        bool RunCpu = false;
        bool RunGpu = false;
        bool Validation = true;
        int CpuFrames = 60;
        int GpuWarmupFrames = 10;
        int GpuFrames = 120;
        int Lines = 38;
        int Columns = 132;
        float FontSize = 17.0f;
        const char* ReportPath = nullptr;
    };

    Options g_Options;

    std::string g_Report;

    void Report(const char* format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        std::printf("%s", buffer);
        g_Report += buffer;
    }

    double NowMs()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // Progress tracing for diagnosing headless GPU failures (crash bisecting):
    // set IMSLUG_BENCH_TRACE=1. Silent otherwise.
    void Trace(const char* format, ...)
    {
        static const bool enabled = std::getenv("IMSLUG_BENCH_TRACE") != nullptr;
        if (!enabled)
            return;
        va_list args;
        va_start(args, format);
        std::fprintf(stderr, "[bench-trace] ");
        std::vfprintf(stderr, format, args);
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
        va_end(args);
    }

    struct Stats
    {
        double Min = 1e30;
        double Max = 0.0;
        double Sum = 0.0;
        std::vector<double> Samples;
        int Count = 0;

        void Add(double value)
        {
            Min = value < Min ? value : Min;
            Max = value > Max ? value : Max;
            Sum += value;
            Samples.push_back(value);
            Count++;
        }
        double Mean() const { return Count > 0 ? Sum / Count : 0.0; }
        double Median()
        {
            if (Samples.empty())
                return 0.0;
            std::sort(Samples.begin(), Samples.end());
            return Samples[Samples.size() / 2];
        }
        double P99()
        {
            if (Samples.empty())
                return 0.0;
            std::sort(Samples.begin(), Samples.end());
            return Samples[(size_t)((Samples.size() - 1) * 0.99)];
        }
    };

    void BuildStressText(std::string* out, int lines, int columns)
    {
        const char* pattern = "Hamburgefonstiv 0123456789 Grumpy wizards make toxic brew. ";
        const int patternLength = (int)std::strlen(pattern);
        out->clear();
        out->reserve((size_t)lines * (size_t)(columns + 1));
        for (int line = 0; line < lines; ++line)
        {
            for (int column = 0; column < columns; ++column)
                out->push_back(pattern[(line * columns + column) % patternLength]);
            if (line + 1 < lines)
                out->push_back('\n');
        }
    }

    void BuildCjkText(std::string* out, int lines, int columns)
    {
        out->clear();
        int index = 0;
        for (int line = 0; line < lines; ++line)
        {
            for (int column = 0; column < columns; ++column)
            {
                const unsigned int codepoint = 0x4E00u + (unsigned int)(index++ % 0x100);
                char encoded[4];
                int length = 0;
                if (codepoint < 0x800)
                {
                    encoded[0] = (char)(0xC0 | (codepoint >> 6));
                    encoded[1] = (char)(0x80 | (codepoint & 0x3F));
                    length = 2;
                }
                else
                {
                    encoded[0] = (char)(0xE0 | (codepoint >> 12));
                    encoded[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                    encoded[2] = (char)(0x80 | (codepoint & 0x3F));
                    length = 3;
                }
                out->append(encoded, (size_t)length);
            }
            if (line + 1 < lines)
                out->push_back('\n');
        }
    }

    // ---------------------------------------------------------------------
    // CPU-side bench (no Vulkan; a fake render interface)
    // ---------------------------------------------------------------------
    const uint32_t kCpuRingCapacity = 65536 * 2;
    std::vector<ImSlugGlyphInstance> g_CpuRing((size_t)kCpuRingCapacity);
    uint32_t g_CpuRingUsed = 0;
    uint32_t g_CpuRingCapacity = kCpuRingCapacity;
    uint32_t g_CpuFrameIndex = 0;
    int g_CpuDrawCalls = 0;
    long long g_CpuUploadBytes = 0;

    uint32_t FakeGetFrameIndex()
    {
        return g_CpuFrameIndex;
    }
    ImSlugGlyphInstance* FakeReserve(uint32_t count, uint32_t* outFirst)
    {
        if (count > g_CpuRingCapacity - g_CpuRingUsed)
            return nullptr;
        *outFirst = g_CpuRingUsed;
        g_CpuRingUsed += count;
        return g_CpuRing.data() + (g_CpuRingUsed - count);
    }
    void FakeDraw(const ImSlugBatch*)
    {
        g_CpuDrawCalls++;
    }
    const ImSlugRenderIO g_CpuIO = { "bench-cpu", FakeGetFrameIndex, FakeReserve, FakeDraw };

    // Forces both pools to be dirty (as the first upload would) and measures the
    // bytes a dirty-row upload would move this frame.
    long long ConsumeExpectedUploadBytes()
    {
        ImSlugPools* pools = ImSlugGetPools();
        if (pools == nullptr)
            return 0;
        long long bytes = 0;
        if (pools->Curves.DirtyRowMin <= pools->Curves.DirtyRowMax)
            bytes += (long long)(pools->Curves.DirtyRowMax - pools->Curves.DirtyRowMin + 1) * kPoolWidth * 8;
        if (pools->Bands.DirtyRowMin <= pools->Bands.DirtyRowMax)
            bytes += (long long)(pools->Bands.DirtyRowMax - pools->Bands.DirtyRowMin + 1) * kPoolWidth * 4;
        ImSlugPoolClearDirtyRows(pools);
        return bytes;
    }

    struct DrawStats
    {
        int SlugCommands = 0;
        int ResetCommands = 0;
        int GeometryCommands = 0;
        long long VertexBytes = 0;
        long long IndexBytes = 0;
    };

    DrawStats CollectDrawStats(const ImDrawData* drawData)
    {
        DrawStats stats;
        const ImDrawCallback reset = ImGui::GetPlatformIO().DrawCallback_ResetRenderState;
        for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
        {
            const ImDrawList* list = drawData->CmdLists[listIndex];
            for (int i = 0; i < list->CmdBuffer.Size; ++i)
            {
                const ImDrawCmd& cmd = list->CmdBuffer[i];
                if (cmd.UserCallback == ImSlug::DrawCallback)
                    stats.SlugCommands++;
                else if (reset != nullptr && cmd.UserCallback == reset)
                    stats.ResetCommands++;
                else if (cmd.UserCallback == nullptr && cmd.ElemCount > 0)
                    stats.GeometryCommands++;
            }
            stats.VertexBytes += (long long)list->VtxBuffer.Size * (long long)sizeof(ImDrawVert);
            stats.IndexBytes += (long long)list->IdxBuffer.Size * (long long)sizeof(ImDrawIdx);
        }
        return stats;
    }

    // One measured configuration: only the text is drawn (no window chrome), so
    // the numbers isolate text rendering. The window is sized from the text so
    // ImGui's clip culling never removes glyphs from the measured scene.
    void CpuBenchScene(const char* name, ImFont* font, const std::string& text, float size, int frames,
                       bool measureFirstUse)
    {
        if (font == nullptr)
        {
            Report("  %s: font registration failed\n", name);
            return;
        }
        int lines = 1;
        int longestLine = 0;
        int currentLine = 0;
        for (char c : text)
        {
            if (c == '\n')
            {
                lines++;
                currentLine = 0;
            }
            else
            {
                currentLine++;
                longestLine = currentLine > longestLine ? currentLine : longestLine;
            }
        }

        // First-use glyph build cost (the lazy cache path).
        if (measureFirstUse)
        {
            const double start = NowMs();
            ImFontBaked* baked = font->GetFontBaked(size);
            int visible = 0;
            for (unsigned int codepoint = 0x20; codepoint <= 0x17F; ++codepoint)
            {
                const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)codepoint);
                if (glyph != nullptr && glyph->Visible)
                    visible++;
            }
            const double elapsed = NowMs() - start;
            Report("  lazy glyph build: %d glyphs in %.2f ms (%.2f us/glyph, target <= 50)\n", visible, elapsed,
                   visible > 0 ? elapsed * 1000.0 / (double)visible : 0.0);
        }

        long long firstUploadBytes = 0;
        Statistics stats = {};
        Stats frameTimes;
        DrawStats lastStats;
        long long totalUploadBytes = 0;
        bool firstUploadMeasured = false;
        for (int frame = 0; frame < frames; ++frame)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            // A window large enough for every line (no culling): the measured
            // scene must draw all the glyphs it claims.
            const float windowWidth = (float)longestLine * size * 0.7f + 40.0f;
            const float windowHeight = (float)lines * size * 1.25f + 40.0f;
            ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);
            ImGui::Begin("bench", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
            ImGui::GetWindowDrawList()->AddText(font, size, ImVec2(0.0f, 0.0f), IM_COL32(255, 255, 255, 255),
                                                text.c_str());
            ImGui::End();
            ImGui::Render();

            g_CpuRingUsed = 0;
            g_CpuDrawCalls = 0;
            g_CpuFrameIndex = (uint32_t)frame;
            ImDrawData* drawData = ImGui::GetDrawData();
            const double start = NowMs();
            ImSlug::PreprocessDrawData(drawData);
            const double elapsed = NowMs() - start;
            if (frame >= frames / 4)  // skip the first frames (atlas warm-up)
                frameTimes.Add(elapsed);
            ImSlugPools* pools = ImSlugGetPools();
            if (pools != nullptr && !firstUploadMeasured)
            {
                // The first upload moves every used row of both pools.
                firstUploadBytes = (long long)pools->Curves.UsedRows * kPoolWidth * 8 +
                                   (long long)pools->Bands.UsedRows * kPoolWidth * 4;
                firstUploadMeasured = true;
            }
            totalUploadBytes += ConsumeExpectedUploadBytes();
            ImSlug::GetStatistics(&stats);
            lastStats = CollectDrawStats(drawData);
        }

        const long long uploadPerFrame = frames > 0 ? totalUploadBytes / frames : 0;
        Report("  %-22s pre-pass %.3f ms mean / %.3f median / %.3f p99  |  %u instances, %d slug cmds, %d geom cmds\n",
               name, frameTimes.Mean(), frameTimes.Median(), frameTimes.P99(), stats.Instances, lastStats.SlugCommands,
               lastStats.GeometryCommands);
        Report("  %-22s vertex %.1f KB/frame, index %.1f KB/frame, initial pool upload %.1f KB, settled dirty-row "
               "upload %.1f KB/frame\n",
               "", (double)lastStats.VertexBytes / 1024.0, (double)lastStats.IndexBytes / 1024.0,
               (double)firstUploadBytes / 1024.0, (double)uploadPerFrame / 1024.0);
    }

    // A realistic UI frame for the "typical UI" comparison: labels, a table,
    // children, buttons/sliders (geometry), a wrapped paragraph and a plot -
    // the shape of a normal tool window, so both render paths do what they do
    // in production (text + shapes interleaved in many commands).
    // (Defined after the Vulkan section; shared by the CPU and GPU benches.)
    void BuildTypicalUiScene(ImFont* font, float size);

    // ---------------------------------------------------------------------
    // Small-size quality (R8): unhinted Slug vs the atlas rasterizer
    // ---------------------------------------------------------------------
    // PLAN R8: Slug is unhinted by design; the open question M4 must answer
    // with data is whether the optional 4x supersample below ~16 ppem (osgSlug
    // precedent) is needed. The atlas path (stbtt bitmaps) is the production
    // fallback, so it is the reference here: for sizes 8..32 px and four X
    // subpixel offsets this compares (a) what the shader computes today (one
    // Slug sample per pixel), (b) a 4x4 supersampled Slug sample (the sampling
    // cost of the option: 16x fragments), against the stbtt bitmap. Metrics:
    // mean |coverage - stbtt| over the glyph window, and |peak coverage delta|
    // (a stem-contrast/blur proxy: a washed-out small stem lowers the peak).
    struct SmallSizeAccumulator
    {
        double Sum1x = 0.0;   // sum |slug1x - stbtt|
        double Sum4x = 0.0;   // sum |slug4x - stbtt|
        double Peak1x = 0.0;  // sum |peak(slug1x) - peak(stbtt)|
        double Peak4x = 0.0;
        long long Samples1x = 0;
        long long Samples4x = 0;
        int Pixels = 0;
        int Runs = 0;

        double Mean1x() const { return Pixels > 0 ? Sum1x / (double)Pixels : 0.0; }
        double Mean4x() const { return Pixels > 0 ? Sum4x / (double)Pixels : 0.0; }
        double MeanPeak1x() const { return Runs > 0 ? Peak1x / (double)Runs : 0.0; }
        double MeanPeak4x() const { return Runs > 0 ? Peak4x / (double)Runs : 0.0; }
    };

    void RunSmallSizeQuality()
    {
        Report("\n## Small-size quality (R8): unhinted Slug vs the atlas rasterizer\n\n");

        int fontDataSize = 0;
        unsigned char* fontData = imslug_test::LoadFontFile("Roboto-Regular.ttf", &fontDataSize);
        ImSlugFont* font = fontData != nullptr ? ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true) : nullptr;
        stbtt_fontinfo info = {};
        if (fontData == nullptr || font == nullptr || !stbtt_InitFont(&info, fontData, 0))
        {
            Report("  skipped: test font missing or unparsable\n");
            std::free(fontData);
            if (font != nullptr)
                ImSlugDestroyFont(font);
            return;
        }
        const int unitsPerEm = (info.data[info.head + 18] << 8) | info.data[info.head + 19];
        ImSlugPools pools;
        ImSlugInitPools(&pools);

        const unsigned int codepoints[6] = { 'a', 'e', 'g', 'm', 'W', '0' };
        const float sizes[7] = { 8.0f, 10.0f, 12.0f, 14.0f, 16.0f, 24.0f, 32.0f };
        const float offsets[4] = { 0.0f, 0.25f, 0.5f, 0.75f };

        Report("| size px | mean delta 1x | mean delta 4x | peak delta 1x | peak delta 4x | px (runs) | samples 1x | "
               "samples 4x |\n");
        Report("|---|---|---|---|---|---|---|---|\n");
        for (float size : sizes)
        {
            SmallSizeAccumulator accumulator;
            for (unsigned int codepoint : codepoints)
            {
                const ImSlugGlyphRecord* record = nullptr;
                if (ImSlugGetGlyphRecord(font, &pools, codepoint, &record) != ImSlugBuildStatus_Ok)
                    continue;
                const float scale = stbtt_ScaleForPixelHeight(&info, 1.0f) * size;
                const float pixelsPerEm = scale * (float)unitsPerEm;
                const float inverse = 1.0f / pixelsPerEm;
                for (float offsetX : offsets)
                {
                    int width = 0;
                    int height = 0;
                    int xOffset = 0;
                    int yOffset = 0;
                    unsigned char* bitmap = stbtt_GetCodepointBitmapSubpixel(
                        &info, scale, scale, offsetX, 0.0f, (int)codepoint, &width, &height, &xOffset, &yOffset);
                    if (bitmap == nullptr || width <= 0 || height <= 0)
                        continue;
                    float peakStbtt = 0.0f;
                    float peak1x = 0.0f;
                    float peak4x = 0.0f;
                    for (int j = 0; j < height; ++j)
                    {
                        for (int i = 0; i < width; ++i)
                        {
                            const float emX = ((float)xOffset + (float)i + 0.5f) * inverse;
                            const float emY = -((float)yOffset + (float)j + 0.5f) * inverse;
                            const float reference = (float)bitmap[j * width + i] / 255.0f;
                            const float sample1x =
                                ImSlugRasterSample(&pools, record, emX, emY, pixelsPerEm, pixelsPerEm);
                            float sample4x = 0.0f;
                            for (int subY = 0; subY < 4; ++subY)
                            {
                                for (int subX = 0; subX < 4; ++subX)
                                {
                                    const float subEmX =
                                        ((float)xOffset + (float)i + ((float)subX + 0.5f) * 0.25f) * inverse;
                                    const float subEmY =
                                        -((float)yOffset + (float)j + ((float)subY + 0.5f) * 0.25f) * inverse;
                                    sample4x += ImSlugRasterSample(&pools, record, subEmX, subEmY, pixelsPerEm * 4.0f,
                                                                   pixelsPerEm * 4.0f);
                                }
                            }
                            sample4x *= 1.0f / 16.0f;
                            accumulator.Sum1x += std::fabs(sample1x - reference);
                            accumulator.Sum4x += std::fabs(sample4x - reference);
                            peakStbtt = reference > peakStbtt ? reference : peakStbtt;
                            peak1x = sample1x > peak1x ? sample1x : peak1x;
                            peak4x = sample4x > peak4x ? sample4x : peak4x;
                            accumulator.Samples1x++;
                            accumulator.Samples4x += 16;
                        }
                    }
                    accumulator.Peak1x += std::fabs((double)peak1x - (double)peakStbtt);
                    accumulator.Peak4x += std::fabs((double)peak4x - (double)peakStbtt);
                    accumulator.Pixels += width * height;
                    accumulator.Runs++;
                    stbtt_FreeBitmap(bitmap, nullptr);
                }
            }
            Report("| %.0f | %.4f | %.4f | %.4f | %.4f | %d (%d) | %lld | %lld |\n", (double)size, accumulator.Mean1x(),
                   accumulator.Mean4x(), accumulator.MeanPeak1x(), accumulator.MeanPeak4x(), accumulator.Pixels,
                   accumulator.Runs, accumulator.Samples1x, accumulator.Samples4x);
        }
        Report("\nThe atlas rasterizer is exact area coverage; Slug's single sample uses the reference estimator's\n"
               "one-pixel ramp, so the 1x deltas measure estimator error (M1 measured the same effect against the\n"
               "CPU rasterizer gate). The 4x column is the accuracy ceiling the supersample trade buys for 16x the\n"
               "fragment cost; the decision (ship/skip) is recorded in docs/DESIGN.md and PLAN section 11.\n");

        ImSlugDestroyPools(&pools);
        ImSlugDestroyFont(font);
        std::free(fontData);
    }

    void RunCpuBench()
    {
        Report("\n## CPU pre-pass (no GPU)\n\n");
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        // A large display so no window clip culling hides glyphs from the
        // measured scene (the pre-pass itself is display-independent).
        io.DisplaySize = ImVec2(4096.0f, 4096.0f);
        io.IniFilename = nullptr;
        if (!ImSlug::Init())
        {
            Report("  ImSlug::Init failed\n");
            ImGui::DestroyContext();
            return;
        }
        ImSlugSetRenderIO(&g_CpuIO);
        ImGui::GetPlatformIO().DrawCallback_ResetRenderState = [](const ImDrawList*, const ImDrawCmd*) {};

        std::string text;
        ImFont* font = ImSlug::AddFontFromFileTTF(IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf"), g_Options.FontSize);
        BuildStressText(&text, g_Options.Lines, g_Options.Columns);
        CpuBenchScene("5k glyphs (golden)", font, text, g_Options.FontSize, g_Options.CpuFrames,
                      /*measureFirstUse=*/true);

        // 1k and 20k scales: fewer/more lines of the same shape.
        BuildStressText(&text, g_Options.Lines / 5, g_Options.Columns);
        CpuBenchScene("1k glyphs", font, text, g_Options.FontSize, g_Options.CpuFrames, /*measureFirstUse=*/false);
        BuildStressText(&text, g_Options.Lines * 4, g_Options.Columns);
        CpuBenchScene("20k glyphs", font, text, g_Options.FontSize, g_Options.CpuFrames, /*measureFirstUse=*/false);

        // CJK, when the fetched subset is present (heavier per-glyph records).
        std::FILE* cjk = std::fopen(IMSLUG_CJK_TEST_FONT, "rb");
        if (cjk != nullptr)
        {
            std::fclose(cjk);
            ImFont* cjkFont = ImSlug::AddFontFromFileTTF(IMSLUG_CJK_TEST_FONT, 22.0f);
            BuildCjkText(&text, 120, 30);
            CpuBenchScene("3.6k Hanzi (CJK)", cjkFont, text, 22.0f, g_Options.CpuFrames, /*measureFirstUse=*/true);
        }
        else
        {
            Report("  CJK: skipped (%s not fetched; scripts/fetch_cjk_font.*)\n", IMSLUG_CJK_TEST_FONT);
        }

        // Typical UI frame: the pre-pass cost on a production-shaped command
        // stream (text interleaved with widgets, tables, children and shapes).
        if (font != nullptr)
        {
            Stats uiTimes;
            DrawStats uiStats;
            for (int frame = 0; frame < g_Options.CpuFrames; ++frame)
            {
                BuildTypicalUiScene(font, g_Options.FontSize);
                ImGui::Render();
                ImDrawData* drawData = ImGui::GetDrawData();
                g_CpuRingUsed = 0;
                g_CpuDrawCalls = 0;
                g_CpuFrameIndex = (uint32_t)frame;
                const double start = NowMs();
                ImSlug::PreprocessDrawData(drawData);
                if (frame >= g_Options.CpuFrames / 4)
                    uiTimes.Add(NowMs() - start);
                uiStats = CollectDrawStats(drawData);
            }
            Statistics uiStatistics;
            ImSlug::GetStatistics(&uiStatistics);
            Report("  %-22s pre-pass %.3f ms mean / %.3f median  |  %u instances, %d slug cmds, %d geom cmds\n",
                   "typical UI frame", uiTimes.Mean(), uiTimes.Median(), uiStatistics.Instances, uiStats.SlugCommands,
                   uiStats.GeometryCommands);
        }

        // Small-size quality (R8): unhinted Slug vs the atlas rasterizer at
        // 8..32 px, including the 4x-supersample accuracy ceiling.
        RunSmallSizeQuality();

        ImSlugSetRenderIO(nullptr);
        ImSlug::Shutdown();
        ImGui::DestroyContext();
    }

    // ---------------------------------------------------------------------
    // GPU bench (headless Vulkan, timestamp queries)
    // ---------------------------------------------------------------------
    struct GpuState
    {
        bool Valid;
        bool ValidationEnabled;
        int ValidationErrors;
        int ValidationMessages;
        std::string DeviceName;

        VkInstance Instance;
        VkDebugUtilsMessengerEXT DebugMessenger;
        VkPhysicalDevice PhysicalDevice;
        VkDevice Device;
        VkQueue Queue;
        uint32_t QueueFamily;
        VkRenderPass RenderPass;
        VkImage Target;
        VkDeviceMemory TargetMemory;
        VkImageView TargetView;
        VkFramebuffer Framebuffer;
        VkCommandPool CommandPool;
        VkCommandBuffer CommandBuffer;
        VkFence Fence;
        VkQueryPool Timestamps;
        VkDescriptorPool ImGuiDescriptorPool;
        float TimestampPeriod;
        bool TimestampsValid;
    };

    GpuState g_Gpu = {};

    VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
    {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            g_Gpu.ValidationErrors++;
            std::printf("  [vulkan error] %s\n", data->pMessage);
        }
        else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        {
            std::printf("  [vulkan warning] %s\n", data->pMessage);
        }
        g_Gpu.ValidationMessages++;
        return VK_FALSE;
    }

    bool CheckVk(VkResult result, const char* what)
    {
        if (result != VK_SUCCESS)
        {
            std::printf("  [bench] %s failed (VkResult %d)\n", what, (int)result);
            return false;
        }
        return true;
    }

    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memoryProperties;
        vkGetPhysicalDeviceMemoryProperties(g_Gpu.PhysicalDevice, &memoryProperties);
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        return UINT32_MAX;
    }

    bool CreateGpuObjects()
    {
        VkApplicationInfo applicationInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        applicationInfo.pApplicationName = "imslug bench";
        applicationInfo.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instanceInfo.pApplicationInfo = &applicationInfo;

        // Validation layers mirror the golden harness: the report claims
        // "validation clean" only when the layer actually ran (pass
        // --no-validation or IMSLUG_VALIDATION=0 to skip it).
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        messengerInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = DebugCallback;
        if (g_Options.Validation)
        {
            instanceInfo.enabledLayerCount = 1;
            static const char* const validationLayer = "VK_LAYER_KHRONOS_validation";
            instanceInfo.ppEnabledLayerNames = &validationLayer;
            instanceInfo.enabledExtensionCount = 1;
            static const char* const debugUtilsExtension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
            instanceInfo.ppEnabledExtensionNames = &debugUtilsExtension;
            instanceInfo.pNext = &messengerInfo;
        }

        VkResult instanceResult = vkCreateInstance(&instanceInfo, nullptr, &g_Gpu.Instance);
        if (instanceResult == VK_ERROR_LAYER_NOT_PRESENT || instanceResult == VK_ERROR_EXTENSION_NOT_PRESENT)
        {
            Report("  (validation layers unavailable; running without)\n");
            g_Options.Validation = false;
            instanceInfo.enabledLayerCount = 0;
            instanceInfo.enabledExtensionCount = 0;
            instanceInfo.pNext = nullptr;
            instanceResult = vkCreateInstance(&instanceInfo, nullptr, &g_Gpu.Instance);
        }
        if (!CheckVk(instanceResult, "vkCreateInstance"))
            return false;
        if (g_Options.Validation)
        {
            PFN_vkCreateDebugUtilsMessengerEXT createMessenger =
                (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_Gpu.Instance,
                                                                          "vkCreateDebugUtilsMessengerEXT");
            if (createMessenger == nullptr ||
                createMessenger(g_Gpu.Instance, &messengerInfo, nullptr, &g_Gpu.DebugMessenger) != VK_SUCCESS)
            {
                Report("  (debug messenger unavailable; running without validation)\n");
                g_Options.Validation = false;
            }
        }
        g_Gpu.ValidationEnabled = g_Options.Validation;

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(g_Gpu.Instance, &deviceCount, nullptr);
        if (deviceCount == 0)
            return false;
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(g_Gpu.Instance, &deviceCount, devices.data());

        // Pick the first device with a graphics queue that supports timestamps.
        VkPhysicalDeviceProperties properties = {};
        for (VkPhysicalDevice device : devices)
        {
            vkGetPhysicalDeviceProperties(device, &properties);
            if (properties.limits.timestampPeriod == 0.0f)
                continue;
            VkFormatProperties formatProperties;
            vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_R8G8B8A8_UNORM, &formatProperties);
            if ((formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0)
                continue;
            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
            for (uint32_t family = 0; family < queueFamilyCount; ++family)
                if ((queueFamilies[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
                {
                    g_Gpu.PhysicalDevice = device;
                    g_Gpu.QueueFamily = family;
                    break;
                }
            if (g_Gpu.PhysicalDevice != VK_NULL_HANDLE)
                break;
        }
        if (g_Gpu.PhysicalDevice == VK_NULL_HANDLE)
            return false;
        g_Gpu.DeviceName = properties.deviceName;
        g_Gpu.TimestampPeriod = properties.limits.timestampPeriod;

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = g_Gpu.QueueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures features = {};
        VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.pEnabledFeatures = &features;
        if (!CheckVk(vkCreateDevice(g_Gpu.PhysicalDevice, &deviceInfo, nullptr, &g_Gpu.Device), "vkCreateDevice"))
            return false;
        vkGetDeviceQueue(g_Gpu.Device, g_Gpu.QueueFamily, 0, &g_Gpu.Queue);

        // Color target.
        VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent.width = 1024;
        imageInfo.extent.height = 640;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!CheckVk(vkCreateImage(g_Gpu.Device, &imageInfo, nullptr, &g_Gpu.Target), "vkCreateImage"))
            return false;
        VkMemoryRequirements imageRequirements;
        vkGetImageMemoryRequirements(g_Gpu.Device, g_Gpu.Target, &imageRequirements);
        VkMemoryAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocateInfo.allocationSize = imageRequirements.size;
        allocateInfo.memoryTypeIndex =
            FindMemoryType(imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!CheckVk(vkAllocateMemory(g_Gpu.Device, &allocateInfo, nullptr, &g_Gpu.TargetMemory), "vkAllocateMemory"))
            return false;
        if (!CheckVk(vkBindImageMemory(g_Gpu.Device, g_Gpu.Target, g_Gpu.TargetMemory, 0), "vkBindImageMemory"))
            return false;
        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = g_Gpu.Target;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (!CheckVk(vkCreateImageView(g_Gpu.Device, &viewInfo, nullptr, &g_Gpu.TargetView), "vkCreateImageView"))
            return false;

        VkAttachmentDescription attachment = {};
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        VkRenderPassCreateInfo renderPassInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        if (!CheckVk(vkCreateRenderPass(g_Gpu.Device, &renderPassInfo, nullptr, &g_Gpu.RenderPass),
                     "vkCreateRenderPass"))
            return false;

        VkFramebufferCreateInfo framebufferInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = g_Gpu.RenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &g_Gpu.TargetView;
        framebufferInfo.width = 1024;
        framebufferInfo.height = 640;
        framebufferInfo.layers = 1;
        if (!CheckVk(vkCreateFramebuffer(g_Gpu.Device, &framebufferInfo, nullptr, &g_Gpu.Framebuffer),
                     "vkCreateFramebuffer"))
            return false;

        VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = g_Gpu.QueueFamily;
        if (!CheckVk(vkCreateCommandPool(g_Gpu.Device, &poolInfo, nullptr, &g_Gpu.CommandPool), "vkCreateCommandPool"))
            return false;
        VkCommandBufferAllocateInfo commandInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        commandInfo.commandPool = g_Gpu.CommandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        if (!CheckVk(vkAllocateCommandBuffers(g_Gpu.Device, &commandInfo, &g_Gpu.CommandBuffer),
                     "vkAllocateCommandBuffers"))
            return false;
        VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!CheckVk(vkCreateFence(g_Gpu.Device, &fenceInfo, nullptr, &g_Gpu.Fence), "vkCreateFence"))
            return false;

        VkQueryPoolCreateInfo queryInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = (uint32_t)(g_Options.GpuWarmupFrames + g_Options.GpuFrames) * 2 + 2;
        g_Gpu.TimestampsValid =
            CheckVk(vkCreateQueryPool(g_Gpu.Device, &queryInfo, nullptr, &g_Gpu.Timestamps), "vkCreateQueryPool");

        // The stock ImGui backend needs a descriptor pool (free-set capable).
        VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 };
        VkDescriptorPoolCreateInfo descriptorPoolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        descriptorPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        descriptorPoolInfo.maxSets = 64;
        descriptorPoolInfo.poolSizeCount = 1;
        descriptorPoolInfo.pPoolSizes = &poolSize;
        if (!CheckVk(vkCreateDescriptorPool(g_Gpu.Device, &descriptorPoolInfo, nullptr, &g_Gpu.ImGuiDescriptorPool),
                     "vkCreateDescriptorPool"))
            return false;
        return true;
    }

    void DestroyGpuObjects()
    {
        if (g_Gpu.Device == VK_NULL_HANDLE)
        {
            if (g_Gpu.DebugMessenger != VK_NULL_HANDLE)
            {
                PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger =
                    (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_Gpu.Instance,
                                                                               "vkDestroyDebugUtilsMessengerEXT");
                if (destroyMessenger != nullptr)
                    destroyMessenger(g_Gpu.Instance, g_Gpu.DebugMessenger, nullptr);
            }
            vkDestroyInstance(g_Gpu.Instance, nullptr);
            return;
        }
        vkDeviceWaitIdle(g_Gpu.Device);
        if (g_Gpu.ImGuiDescriptorPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(g_Gpu.Device, g_Gpu.ImGuiDescriptorPool, nullptr);
        if (g_Gpu.Timestamps != VK_NULL_HANDLE)
            vkDestroyQueryPool(g_Gpu.Device, g_Gpu.Timestamps, nullptr);
        vkDestroyFence(g_Gpu.Device, g_Gpu.Fence, nullptr);
        vkDestroyCommandPool(g_Gpu.Device, g_Gpu.CommandPool, nullptr);
        vkDestroyFramebuffer(g_Gpu.Device, g_Gpu.Framebuffer, nullptr);
        vkDestroyImageView(g_Gpu.Device, g_Gpu.TargetView, nullptr);
        vkDestroyImage(g_Gpu.Device, g_Gpu.Target, nullptr);
        vkFreeMemory(g_Gpu.Device, g_Gpu.TargetMemory, nullptr);
        vkDestroyRenderPass(g_Gpu.Device, g_Gpu.RenderPass, nullptr);
        vkDestroyDevice(g_Gpu.Device, nullptr);
        if (g_Gpu.DebugMessenger != VK_NULL_HANDLE)
        {
            PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger =
                (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_Gpu.Instance,
                                                                           "vkDestroyDebugUtilsMessengerEXT");
            if (destroyMessenger != nullptr)
                destroyMessenger(g_Gpu.Instance, g_Gpu.DebugMessenger, nullptr);
        }
        vkDestroyInstance(g_Gpu.Instance, nullptr);
    }

    // A realistic UI frame for the "typical UI" comparison: labels, a table,
    // children, buttons/sliders (geometry), a wrapped paragraph and a plot -
    // the shape of a normal tool window, so both render paths do what they do
    // in production (text + shapes interleaved in many commands).
    void BuildTypicalUiScene(ImFont* font, float size)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1024.0f, 640.0f), ImGuiCond_Always);
        ImGui::Begin("typical ui", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
        ImGui::PushFont(font, size);
        ImGui::TextUnformatted("imslug typical UI frame: labels, tables, children, widgets");
        const int steps =
            std::getenv("IMSLUG_BENCH_UI_STEPS") != nullptr ? atoi(std::getenv("IMSLUG_BENCH_UI_STEPS")) : 99;
        static float value = 0.5f;
        static bool flag = true;
        if (steps >= 2)
            ImGui::Separator();
        if (steps >= 3)
            ImGui::SliderFloat("slider", &value, 0.0f, 1.0f);
        if (steps >= 4)
            ImGui::Checkbox("checkbox", &flag);
        if (steps >= 5)
        {
            ImGui::SameLine();
            ImGui::Text("value %.3f", value);
        }
        if (steps >= 6 &&
            ImGui::BeginTable("items", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            for (int row = 0; row < 22; ++row)
            {
                ImGui::TableNextRow();
                for (int column = 0; column < 4; ++column)
                {
                    ImGui::TableSetColumnIndex(column);
                    ImGui::Text("cell %d.%d quick brown fox", row, column);
                }
            }
            ImGui::EndTable();
        }
        if (steps >= 7)
            ImGui::TextWrapped("A wrapped paragraph keeps RenderText busy across several lines and exercises the "
                               "vertex reserve/retry path: the quick brown fox jumps over the lazy dog, again and "
                               "again, while the UI keeps drawing widgets around it.");
        if (steps >= 8)
        {
            const float widths[3] = { 260.0f, 120.0f, 40.0f };
            for (int i = 0; i < 3; ++i)
            {
                char childName[32];
                std::snprintf(childName, sizeof(childName), "child %d", i);
                ImGui::BeginChild(childName, ImVec2(widths[i], 46.0f), ImGuiChildFlags_Borders);
                ImGui::TextUnformatted("child window text here");
                ImGui::TextUnformatted("second line 0123456789");
                ImGui::EndChild();
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }
        if (steps >= 9)
        {
            static float history[64] = {};
            history[ImGui::GetFrameCount() % 64] = value;
            ImGui::PlotLines("plot", history, 64, ImGui::GetFrameCount() % 64, "value", 0.0f, 1.0f,
                             ImVec2(400.0f, 90.0f));
        }
        ImGui::PopFont();
        ImGui::End();
    }

    struct GpuVariantResult
    {
        Stats Times;
        long long UploadBytes = 0;
        long long VertexBytes = 0;
        DrawStats Draws;
        uint32_t Instances = 0;  // Slug instances produced by the last pre-pass
    };

    // Renders one configuration and fills out->Times with the GPU frame times.
    bool GpuBenchVariant(bool slugFont, bool uiScene, const std::string& text, float size, GpuVariantResult* out)
    {
        Trace("variant(%s, ui=%d): create context", slugFont ? "slug" : "atlas", uiScene ? 1 : 0);
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1024.0f, 640.0f);
        ImGui::StyleColorsDark();
        Trace("variant: ImSlug::Init");
        if (!ImSlug::Init())
            return false;
        if (slugFont)
        {
            io.FontDefault = ImSlug::AddFontFromFileTTF(IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf"), size);
        }
        else
        {
            ImFontConfig config;
            config.OversampleH = 1;
            config.OversampleV = 1;
            io.FontDefault = io.Fonts->AddFontFromFileTTF(IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf"), size, &config);
        }
        if (io.FontDefault == nullptr)
            return false;
        Trace("variant: font registered");

        ImGui_ImplVulkan_InitInfo initInfo = {};
        initInfo.ApiVersion = VK_API_VERSION_1_2;
        initInfo.Instance = g_Gpu.Instance;
        initInfo.PhysicalDevice = g_Gpu.PhysicalDevice;
        initInfo.Device = g_Gpu.Device;
        initInfo.QueueFamily = g_Gpu.QueueFamily;
        initInfo.Queue = g_Gpu.Queue;
        initInfo.DescriptorPool = g_Gpu.ImGuiDescriptorPool;
        initInfo.MinImageCount = 2;
        initInfo.ImageCount = 2;
        initInfo.PipelineInfoMain.RenderPass = g_Gpu.RenderPass;
        initInfo.PipelineInfoMain.Subpass = 0;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        if (!ImGui_ImplVulkan_Init(&initInfo))
            return false;
        Trace("variant: stock backend init");
        if (!ImSlug_ImplVulkan_Init(&initInfo))
            return false;
        Trace("variant: imslug backend init");

        const int totalFrames = g_Options.GpuWarmupFrames + g_Options.GpuFrames;
        long long uploadBytes = 0;
        DrawStats drawStats;
        ImFont* font = io.FontDefault;
        for (int frame = 0; frame < totalFrames; ++frame)
        {
            Trace("variant: frame %d build", frame);
            if (uiScene)
            {
                BuildTypicalUiScene(font, size);
            }
            else
            {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(1024.0f, 640.0f), ImGuiCond_Always);
                ImGui::Begin("bench", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
                ImGui::GetWindowDrawList()->AddText(font, size, ImVec2(0.0f, 0.0f), IM_COL32(255, 255, 255, 255),
                                                    text.c_str());
                ImGui::End();
            }
            ImGui::Render();
            Trace("variant: frame %d render", frame);
            ImDrawData* drawData = ImGui::GetDrawData();
            // No manual pre-pass: production calls ImSlug_ImplVulkan_RenderDrawData,
            // which runs PreprocessDrawData itself (a second call is idempotent,
            // but the pre-pass must run *after* UploadPools advanced the ring).
            vkResetFences(g_Gpu.Device, 1, &g_Gpu.Fence);
            vkResetCommandBuffer(g_Gpu.CommandBuffer, 0);
            VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(g_Gpu.CommandBuffer, &beginInfo);

            ImSlugPools* pools = ImSlugGetPools();
            long long frameUpload = 0;
            if (pools != nullptr)
            {
                if (pools->Curves.DirtyRowMin <= pools->Curves.DirtyRowMax)
                    frameUpload +=
                        (long long)(pools->Curves.DirtyRowMax - pools->Curves.DirtyRowMin + 1) * kPoolWidth * 8;
                if (pools->Bands.DirtyRowMin <= pools->Bands.DirtyRowMax)
                    frameUpload +=
                        (long long)(pools->Bands.DirtyRowMax - pools->Bands.DirtyRowMin + 1) * kPoolWidth * 4;
            }
            if (slugFont)
                ImSlug_ImplVulkan_UploadPools(g_Gpu.CommandBuffer);
            Trace("variant: frame %d pools uploaded", frame);

            VkClearValue clearValue = {};
            clearValue.color.float32[0] = 0.06f;
            clearValue.color.float32[1] = 0.06f;
            clearValue.color.float32[2] = 0.07f;
            clearValue.color.float32[3] = 1.0f;
            VkRenderPassBeginInfo renderPassInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            renderPassInfo.renderPass = g_Gpu.RenderPass;
            renderPassInfo.framebuffer = g_Gpu.Framebuffer;
            renderPassInfo.renderArea.extent.width = 1024;
            renderPassInfo.renderArea.extent.height = 640;
            renderPassInfo.clearValueCount = 1;
            renderPassInfo.pClearValues = &clearValue;
            const uint32_t queryBase = (uint32_t)frame * 2;
            vkCmdResetQueryPool(g_Gpu.CommandBuffer, g_Gpu.Timestamps, queryBase, 2);
            vkCmdWriteTimestamp(g_Gpu.CommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_Gpu.Timestamps, queryBase);
            vkCmdBeginRenderPass(g_Gpu.CommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            if (slugFont)
                ImSlug_ImplVulkan_RenderDrawData(drawData, g_Gpu.CommandBuffer);
            else
                ImGui_ImplVulkan_RenderDrawData(drawData, g_Gpu.CommandBuffer);  // stock baseline, no imslug
            // After the call: the Slug pre-pass has rewritten the command
            // stream in place (callbacks + resets), which is what the stats
            // columns must report for both variants.
            if (frame == totalFrames - 1)
            {
                drawStats = CollectDrawStats(drawData);
                if (slugFont)
                {
                    Statistics statistics;
                    ImSlug::GetStatistics(&statistics);
                    out->Instances = statistics.Instances;
                }
            }
            Trace("variant: frame %d drawdata recorded", frame);
            vkCmdEndRenderPass(g_Gpu.CommandBuffer);
            vkCmdWriteTimestamp(g_Gpu.CommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_Gpu.Timestamps,
                                queryBase + 1);
            vkEndCommandBuffer(g_Gpu.CommandBuffer);

            VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &g_Gpu.CommandBuffer;
            vkQueueSubmit(g_Gpu.Queue, 1, &submitInfo, g_Gpu.Fence);
            vkWaitForFences(g_Gpu.Device, 1, &g_Gpu.Fence, VK_TRUE, UINT64_MAX);
            Trace("variant: frame %d submitted", frame);
            if (frame >= g_Options.GpuWarmupFrames)
                uploadBytes += frameUpload;
        }

        // Read the timestamps: the render-pass duration per frame.
        if (g_Gpu.TimestampsValid)
        {
            std::vector<uint64_t> timestamps((size_t)totalFrames * 2, 0);
            if (vkGetQueryPoolResults(g_Gpu.Device, g_Gpu.Timestamps, 0, (uint32_t)timestamps.size(),
                                      timestamps.size() * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                for (int frame = g_Options.GpuWarmupFrames; frame < totalFrames; ++frame)
                {
                    const double ticks = (double)(timestamps[(size_t)frame * 2 + 1] - timestamps[(size_t)frame * 2]);
                    out->Times.Add(ticks * (double)g_Gpu.TimestampPeriod * 1.0e-6);  // ns -> ms
                }
            }
        }
        out->Draws = drawStats;
        out->UploadBytes =
            totalFrames > g_Options.GpuWarmupFrames ? uploadBytes / (totalFrames - g_Options.GpuWarmupFrames) : 0;
        out->VertexBytes = drawStats.VertexBytes;

        ImSlug_ImplVulkan_Shutdown();
        ImSlug::Shutdown();
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext();
        Trace("variant: shutdown done");
        return true;
    }

    void GpuBenchSceneReport(const char* sceneName, bool uiScene, const std::string& text, float size)
    {
        Report("\n### %s\n\n", sceneName);
        Report("| variant | GPU ms mean | median | p99 | slug cmds | resets | geom cmds | instances | vertex KB/f | "
               "upload KB/f |\n");
        Report("|---|---|---|---|---|---|---|---|---|---|\n");
        GpuVariantResult slug, atlas;
        if (!GpuBenchVariant(true, uiScene, text, size, &slug))
        {
            Report("| slug | FAILED | | | | | | | | |\n");
            return;
        }
        if (!GpuBenchVariant(false, uiScene, text, size, &atlas))
        {
            Report("| atlas (stock) | FAILED | | | | | | | | |\n");
            return;
        }
        Report("| slug | %.3f | %.3f | %.3f | %d | %d | %d | %u | %.1f | %.1f |\n", slug.Times.Mean(),
               slug.Times.Median(), slug.Times.P99(), slug.Draws.SlugCommands, slug.Draws.ResetCommands,
               slug.Draws.GeometryCommands, slug.Instances, (double)slug.VertexBytes / 1024.0,
               (double)slug.UploadBytes / 1024.0);
        Report("| atlas (stock) | %.3f | %.3f | %.3f | %d | %d | %d | - | %.1f | %.1f |\n", atlas.Times.Mean(),
               atlas.Times.Median(), atlas.Times.P99(), atlas.Draws.SlugCommands, atlas.Draws.ResetCommands,
               atlas.Draws.GeometryCommands, (double)atlas.VertexBytes / 1024.0, (double)atlas.UploadBytes / 1024.0);
        const double ratio = atlas.Times.Median() > 0.0 ? slug.Times.Median() / atlas.Times.Median() : 0.0;
        const int slugCommands = slug.Draws.SlugCommands + slug.Draws.ResetCommands + slug.Draws.GeometryCommands;
        const int atlasCommands = atlas.Draws.GeometryCommands;
        Report("\nSlug/stock GPU time ratio (median): %.3f x; commands: slug %d (%d callbacks + %d resets + %d "
               "geometry) vs stock %d geometry\n",
               ratio, slugCommands, slug.Draws.SlugCommands, slug.Draws.ResetCommands, slug.Draws.GeometryCommands,
               atlasCommands);
        if (slug.Instances == 0 || slug.Draws.SlugCommands == 0)
            Report("WARNING: the Slug variant produced no instances/callbacks; this row does not measure Slug text "
                   "(check the pre-pass order: UploadPools -> RenderDrawData).\n");
    }

    void RunGpuBench()
    {
        Report("\n## GPU (headless offscreen, timestamp queries)\n\n");
        Report("Device: %s (Vulkan, timestamp period %.1f ns); %d frames after %d warm-up, 1024x640 offscreen, "
               "serialized submissions\n",
               g_Gpu.DeviceName.c_str(), (double)g_Gpu.TimestampPeriod, g_Options.GpuFrames, g_Options.GpuWarmupFrames);
        Report("The stock variant renders through ImGui_ImplVulkan_RenderDrawData only (no imslug pre-pass), so the "
               "comparison is production-imslug vs production-stock. `instances` is the Slug pre-pass output of the "
               "last measured frame: zero means the Slug row does not measure text rendering.\n");
        Report("The GPU fragment cost is the interesting number (text-dominated scene); the typical-UI scene is the "
               "production mix (text interleaved with widgets and shapes), where the text share of the frame is "
               "smaller.\n");

        std::string text;
        BuildStressText(&text, g_Options.Lines, g_Options.Columns);
        char label[256];
        std::snprintf(label, sizeof(label), "Text-dominated stress scene (%d lines x %d chars at %.0f px)",
                      g_Options.Lines, g_Options.Columns, (double)g_Options.FontSize);
        GpuBenchSceneReport(label, /*uiScene=*/false, text, g_Options.FontSize);
        GpuBenchSceneReport("Typical UI scene (labels, table, children, widgets, wrapped text, plot)",
                            /*uiScene=*/true, text, g_Options.FontSize);
    }

    void WriteReport()
    {
        if (g_Options.ReportPath == nullptr)
            return;
        std::FILE* file = std::fopen(g_Options.ReportPath, "w");
        if (file == nullptr)
            return;
        std::fprintf(file, "<!-- generated by scripts/bench.* (imslug_tests_bench) - machine-specific numbers -->\n");
        std::fprintf(file, "# imslug M4 benchmark run\n\n");
        std::fprintf(file, "Generated by the imslug bench harness. VM/CI numbers are not comparable to desktop "
                           "numbers; record the device name above when quoting the GPU rows.\n\n");
        std::fwrite(g_Report.data(), 1, g_Report.size(), file);
        std::fclose(file);
    }
}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_MSC_VER) && defined(_DEBUG)
    // Headless runs must not block on assert dialogs: report them to stderr.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--cpu") == 0)
            g_Options.RunCpu = true;
        else if (std::strcmp(argv[i], "--gpu") == 0)
            g_Options.RunGpu = true;
        else if (std::strcmp(argv[i], "--all") == 0)
            g_Options.RunCpu = g_Options.RunGpu = true;
        else if (std::strcmp(argv[i], "--no-validation") == 0)
            g_Options.Validation = false;
        else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc)
            g_Options.ReportPath = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            g_Options.CpuFrames = g_Options.GpuFrames = atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--lines") == 0 && i + 1 < argc)
            g_Options.Lines = atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--columns") == 0 && i + 1 < argc)
            g_Options.Columns = atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc)
            g_Options.FontSize = (float)atof(argv[++i]);
        else
        {
            std::printf("usage: imslug_tests_bench [--cpu|--gpu|--all] [--report <path>] [--frames N]\n"
                        "                          [--lines N] [--columns N] [--size px] [--no-validation]\n");
            return 2;
        }
    }
    if (!g_Options.RunCpu && !g_Options.RunGpu)
        g_Options.RunCpu = true;

    // IMSLUG_VALIDATION=0 disables the layer like the example and the golden
    // harness (the GPU mode enables it by default; see CreateGpuObjects).
    const char* validationEnv = std::getenv("IMSLUG_VALIDATION");
    if (validationEnv != nullptr && (validationEnv[0] == '0' || validationEnv[0] == 'n' || validationEnv[0] == 'N' ||
                                     validationEnv[0] == 'f' || validationEnv[0] == 'F'))
        g_Options.Validation = false;

    Report("imslug M4 benchmark harness\n===========================\n");
    if (g_Options.RunCpu)
        RunCpuBench();
    if (g_Options.RunGpu)
    {
        if (!CreateGpuObjects())
        {
            Report("\n## GPU\n\nSKIP: no usable Vulkan device with timestamps.\n");
        }
        else
        {
            RunGpuBench();
            if (!g_Options.Validation)
                Report("\nValidation: layer disabled (--no-validation / IMSLUG_VALIDATION=0).\n");
            else if (g_Gpu.ValidationErrors > 0)
                Report("\nValidation errors during the bench: %d (of %d messages) - numbers above are suspect.\n",
                       g_Gpu.ValidationErrors, g_Gpu.ValidationMessages);
            else
                Report("\nValidation: clean (%d messages, 0 errors).\n", g_Gpu.ValidationMessages);
            DestroyGpuObjects();
        }
    }
    WriteReport();
    std::printf("\n%s", g_Report.c_str());
    if (g_Options.ReportPath != nullptr)
        std::printf("\nreport written: %s\n", g_Options.ReportPath);
    return 0;
}
