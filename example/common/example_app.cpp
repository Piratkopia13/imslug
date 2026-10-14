// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// example/common/example_app.cpp — shared example application (platform-free).
//
// The scene set, the switcher, the font handling and the per-frame ImGui
// build, exactly as the standalone example had them; the platform shell
// (example/glfw_vulkan/main.cpp, the future example/web/main.cpp) owns the
// window/GPU context and implements the ExamplePlatform hooks. See
// example_app.h for the seams.

#include "example_app.h"

#include "imgui.h"
#include "imgui_internal.h"  // ImTextCharToUtf8 (CJK stress scene)
#include "imslug.h"

// The default fonts, embedded at build time (generated artifacts; regenerate
// with scripts/gen_asset.*): the packed Roboto-ASCII asset for Slug and the
// raw Roboto TTF for ImGui's own atlas loader (mixed-font scene / M toggle).
#include "imslug_example_asset.h"
#include "imslug_example_font.h"

// The browser build has no filesystem: when the web shell was built with the
// fetched test fonts embedded (scripts/build_web.* runs imslug_gen embed and
// defines IMSLUG_EXAMPLE_CJK_EMBED / IMSLUG_EXAMPLE_EMOJI_EMBED, adding the
// headers' directory to the include path), the CJK and color-emoji scenes
// register those subsets instead of probing the fetched files. The emoji
// subset is COLR v1 only (built by scripts/subset_emoji_for_web.py).
#if defined(__EMSCRIPTEN__) && defined(IMSLUG_EXAMPLE_CJK_EMBED)
#include "imslug_example_cjk.h"
#endif
#if defined(__EMSCRIPTEN__) && defined(IMSLUG_EXAMPLE_EMOJI_EMBED)
#include "imslug_example_emoji.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace ImSlug;

namespace
{
    ExampleAppState g_App = {};
    ExamplePlatform g_Platform = {};

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
    // as a second Slug font. Desktop probes the fetched file; the web shell
    // has no filesystem and registers the subset embedded at build time
    // (IMSLUG_EXAMPLE_CJK_EMBED, scripts/build_web.*). Skipped with a message
    // when neither is present.
    void SceneCjk()
    {
        ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("CJK (fetched subset)");
        if (g_App.CjkFont == nullptr && !g_App.CjkAttempted)
        {
            g_App.CjkAttempted = true;
#if defined(__EMSCRIPTEN__) && defined(IMSLUG_EXAMPLE_CJK_EMBED)
            ImFontConfig cjkConfig;
            cjkConfig.FontDataOwnedByAtlas = false;
            g_App.CjkFont =
                ImSlug::AddFontFromMemoryTTF(imslug_example_cjk_data, (int)imslug_example_cjk_size, 22.0f, &cjkConfig);
#elif !defined(__EMSCRIPTEN__)
            FILE* cjkFile = fopen(IMSLUG_CJK_TEST_FONT, "rb");
            if (cjkFile != nullptr)
            {
                fclose(cjkFile);
                g_App.CjkFont = ImSlug::AddFontFromFileTTF(IMSLUG_CJK_TEST_FONT, 22.0f);
            }
#endif
        }
        if (g_App.CjkFont == nullptr)
        {
#if defined(__EMSCRIPTEN__)
            ImGui::TextWrapped("This web build does not embed the CJK subset. Fetch it (scripts/fetch_cjk_font.*) "
                               "and rebuild (scripts/build_web.* embeds it; see tests/fonts/cjk/PROVENANCE.txt).");
#else
            ImGui::TextWrapped("The CJK test font is not fetched. Run scripts/fetch_cjk_font.* (see "
                               "tests/fonts/cjk/PROVENANCE.txt), then restart or reselect this scene.");
#endif
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
#if defined(__EMSCRIPTEN__) && defined(IMSLUG_EXAMPLE_EMOJI_EMBED)
            // The browser build embeds a build-time COLR v1 subset of the
            // fetched font (scripts/build_web.*: subset_emoji_for_web.py +
            // imslug_gen embed; SVG/bitmap tables dropped).
            ImFontConfig emojiConfig;
            emojiConfig.FontDataOwnedByAtlas = false;
            g_App.EmojiFont = ImSlug::AddFontFromMemoryTTF(imslug_example_emoji_data, (int)imslug_example_emoji_size,
                                                           24.0f, &emojiConfig);
#elif !defined(__EMSCRIPTEN__)
            FILE* emojiFile = fopen(IMSLUG_EMOJI_TEST_FONT, "rb");
            if (emojiFile != nullptr)
            {
                fclose(emojiFile);
                g_App.EmojiFont = ImSlug::AddFontFromFileTTF(IMSLUG_EMOJI_TEST_FONT, 24.0f);
            }
#endif
        }
        if (g_App.EmojiFont == nullptr)
        {
#if defined(__EMSCRIPTEN__)
            ImGui::TextWrapped("This web build does not embed the color-emoji subset. Fetch the font "
                               "(scripts/fetch_emoji_font.*) and rebuild (scripts/build_web.* embeds a COLR v1 "
                               "subset; see tests/fonts/emoji/PROVENANCE.txt).");
#else
            ImGui::TextWrapped("The color-emoji test font is not fetched. Run scripts/fetch_emoji_font.* (see "
                               "tests/fonts/emoji/PROVENANCE.txt), then restart or reselect this scene.");
#endif
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
#ifndef __EMSCRIPTEN__
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
#endif
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
#ifndef __EMSCRIPTEN__
            ImGui::TextWrapped("The packed test asset is missing (%s). Generate it with scripts/gen_asset.* "
                               "(see tests/assets/PROVENANCE.txt), then restart or reselect this scene.",
                               IMSLUG_PACKED_TEST_ASSET);
#else
            ImGui::TextWrapped("The embedded packed test asset could not be loaded.");
#endif
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
                         g_App.FrameMsCount < kExamplePerfHistory ? 0 : g_App.FrameMsCursor, nullptr, 0.0f,
                         frameAvg * 2.0f > 1.0f ? frameAvg * 2.0f : 1.0f, ImVec2(410.0f, 64.0f));
        // The GPU-time and vsync rows are desktop-only: a shell without the
        // hooks (web) hides them instead of showing a stale/placeholder state.
        if (g_Platform.GetGpuFrameMs != nullptr)
        {
            float gpuMs = 0.0f;
            int gpuSlots = 0;
            if (g_Platform.GetGpuFrameMs(g_Platform.user, &gpuMs, &gpuSlots))
            {
                if (gpuSlots > 0)
                    ImGui::Text("GPU frame (timestamps): %.2f ms over %d slots", gpuMs, gpuSlots);
                else
                    ImGui::TextUnformatted("GPU frame (timestamps): warming up");
            }
            else
                ImGui::TextUnformatted("GPU frame: n/a (queue without timestamps)");
        }
        if (g_Platform.GetVsync != nullptr)
        {
            bool vsync = g_Platform.GetVsync(g_Platform.user);
            if (ImGui::Checkbox("vsync", &vsync))
                g_Platform.ToggleVsync(g_Platform.user);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", g_Platform.GetPresentModeName(g_Platform.user));
        }

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

    // Debug overlay toggle (runtime; mapped onto the shell's backend module
    // through the ExamplePlatform hooks). Returns the new state so a caller
    // can react (keys and checkboxes share it).
    bool ToggleDebugOverlay(uint32_t flag)
    {
        uint32_t overlay = g_Platform.GetDebugOverlay(g_Platform.user);
        overlay ^= flag;
        g_Platform.SetDebugOverlay(g_Platform.user, overlay);
        printf("[imslug] debug overlay: 0x%X%s%s%s\n", overlay,
               (overlay & ExampleDebugOverlay_GlyphBounds) ? " bounds" : "",
               (overlay & ExampleDebugOverlay_BandGrid) ? " band-grid" : "",
               (overlay & ExampleDebugOverlay_RefHeat) ? " ref-heat" : "");
        return (overlay & flag) != 0;
    }

    void DrawSceneSwitcher()
    {
        const char* sceneNames[kExampleSceneCount] = {
            "demo window",          "widgets / plots / tables", "clipping torture",
            "mixed fonts",          "runtime FontScaleDpi",     "5k glyph stress",
            "CJK (fetched subset)", "color emoji (COLR v1)",    "packed asset (.imslug)",
            "heavy demo / perf (P)"
        };
        ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(240.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("imslug example " IMSLUG_VERSION, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextUnformatted("Scene (keys 1-9, P):");
        for (int i = 0; i < kExampleSceneCount; ++i)
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
            g_App.FontsDirty = true,
            g_App.BaseSize = g_App.BaseSize < kExampleMaxSize ? g_App.BaseSize + 1 : kExampleMaxSize;
        ImGui::SameLine();
        if (ImGui::Button("-") || ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
            g_App.FontsDirty = true,
            g_App.BaseSize = g_App.BaseSize > kExampleMinSize ? g_App.BaseSize - 1 : kExampleMinSize;
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
        ImGui::Separator();
        Statistics stats;
        ImSlug::GetStatistics(&stats);
        ImGui::Text("Slug: %u instances, %u runs\n(%u slabs, %u dropped)", stats.Instances, stats.LiveBatches,
                    stats.GlyphSlabs, stats.DroppedRuns);
        if (g_Platform.GetValidationState != nullptr)
            ImGui::Text("validation: %s", g_Platform.GetValidationState(g_Platform.user));
        ImGui::Separator();
        ImGui::Text("debug overlay (F1/F2/F3):");
        {
            const uint32_t overlay = g_Platform.GetDebugOverlay(g_Platform.user);
            bool bounds = (overlay & ExampleDebugOverlay_GlyphBounds) != 0;
            bool bands = (overlay & ExampleDebugOverlay_BandGrid) != 0;
            bool refs = (overlay & ExampleDebugOverlay_RefHeat) != 0;
            if (ImGui::Checkbox("glyph bounds", &bounds))
                ToggleDebugOverlay(ExampleDebugOverlay_GlyphBounds);
            if (ImGui::Checkbox("band grid", &bands))
                ToggleDebugOverlay(ExampleDebugOverlay_BandGrid);
            if (ImGui::Checkbox("ref heat", &refs))
                ToggleDebugOverlay(ExampleDebugOverlay_RefHeat);
        }
        ImGui::End();
    }

    void BuildImGuiFrame()
    {
        ImGui::NewFrame();
        // Performance panel history: wall-clock frame ms (io.DeltaTime is the
        // time NewFrame measured for the previous frame).
        g_App.FrameMsHistory[g_App.FrameMsCursor] = ImGui::GetIO().DeltaTime * 1000.0f;
        g_App.FrameMsCursor = (g_App.FrameMsCursor + 1) % kExamplePerfHistory;
        if (g_App.FrameMsCount < kExamplePerfHistory)
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

    void CaptureDrawStats()
    {
        const ImDrawData* drawData = ImGui::GetDrawData();
        g_App.LastCmdLists = drawData->CmdListsCount;
        g_App.LastVtxCount = drawData->TotalVtxCount;
        g_App.LastIdxCount = drawData->TotalIdxCount;
        g_App.LastDrawCommands = 0;
        for (int i = 0; i < drawData->CmdListsCount; ++i)
            g_App.LastDrawCommands += drawData->CmdLists[i]->CmdBuffer.Size;
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
        // the file only when --font named a TTF (FontPathIsPacked is the
        // shell's one-time file sniff); otherwise it comes from the embedded
        // Roboto TTF copy (in a no-parser build too - ImGui's own parser is
        // independent of imslug's).
        ImFontConfig stockConfig;
        stockConfig.OversampleH = 1;
        stockConfig.OversampleV = 1;
        if (fontPath != nullptr && !g_App.FontPathIsPacked)
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

    // Installs the shared state from the shell's options.
    void LoadOptions(const ExampleAppOptions* options)
    {
        g_App.FontPath = options->FontPath;
        g_App.FontPathIsPacked = options->FontPathIsPacked;
        g_App.BaseSize = options->BaseSize;
        if (g_App.BaseSize < kExampleMinSize)
            g_App.BaseSize = kExampleMinSize;
        if (g_App.BaseSize > kExampleMaxSize)
            g_App.BaseSize = kExampleMaxSize;
        g_App.Scene = options->SceneNumber != 0 ? options->SceneNumber - 1 : 0;  // scenes are 1-based on the CLI
        if (g_App.Scene < 0 || g_App.Scene >= kExampleSceneCount)
            g_App.Scene = 0;
        g_App.DpiScale = options->DpiScale;
        if (g_App.DpiScale < 0.5f)
            g_App.DpiScale = 0.5f;
        if (g_App.DpiScale > 3.0f)
            g_App.DpiScale = 3.0f;
        g_App.AtlasText = options->AtlasText;
        g_App.EmojiSize = 24.0f;
        g_App.EmojiZoom = 120.0f;
        g_App.PerfLines = 40;
        g_App.PerfChars = 200;
        g_App.PerfScale = 3.0f;
    }
}  // namespace

ExampleAppState* ExampleAppGetState()
{
    return &g_App;
}

void ExampleAppOptionsDefaults(ExampleAppOptions* options)
{
    options->FontPath = nullptr;
    options->FontPathIsPacked = false;
    options->BaseSize = kExampleDefaultSize;
    options->SceneNumber = 0;
    options->DpiScale = 1.0f;
    options->AtlasText = false;
    options->DebugOverlay = 0;
    options->DebugOverlayGiven = false;
}

int ExampleAppParseArg(int argc, char** argv, int argIndex, ExampleAppOptions* options)
{
    const char* arg = argv[argIndex];
    if (strcmp(arg, "--font") == 0 && argIndex + 1 < argc)
    {
        // A filesystem path: only the desktop shell passes one (the web shell
        // always uses the embedded fonts).
        options->FontPath = argv[argIndex + 1];
        return 2;
    }
    if (strcmp(arg, "--size") == 0 && argIndex + 1 < argc)
    {
        options->BaseSize = atoi(argv[argIndex + 1]);
        return 2;
    }
    if (strcmp(arg, "--renderer") == 0 && argIndex + 1 < argc)
    {
        const char* value = argv[argIndex + 1];
        if (strcmp(value, "slug") == 0)
            options->AtlasText = false;
        else if (strcmp(value, "atlas") == 0)
            options->AtlasText = true;
        else
        {
            fprintf(stderr, "[imslug] --renderer expects 'slug' or 'atlas' (got '%s')\n", value);
            exit(1);
        }
        return 2;
    }
    if (strcmp(arg, "--scene") == 0 && argIndex + 1 < argc)
    {
        options->SceneNumber = atoi(argv[argIndex + 1]);
        return 2;
    }
    if (strcmp(arg, "--dpi") == 0 && argIndex + 1 < argc)
    {
        options->DpiScale = (float)atof(argv[argIndex + 1]);
        return 2;
    }
    if (strcmp(arg, "--overlay") == 0 && argIndex + 1 < argc)
    {
        const char* spec = argv[argIndex + 1];
        uint32_t flags = 0;
        if (strstr(spec, "bounds") != nullptr)
            flags |= ExampleDebugOverlay_GlyphBounds;
        if (strstr(spec, "grid") != nullptr)
            flags |= ExampleDebugOverlay_BandGrid;
        if (strstr(spec, "refs") != nullptr)
            flags |= ExampleDebugOverlay_RefHeat;
        if (flags == 0 && spec[0] >= '0' && spec[0] <= '9')
            flags = (uint32_t)atoi(spec);
        if (strcmp(spec, "none") == 0)
            flags = 0;
        options->DebugOverlay = flags;
        options->DebugOverlayGiven = true;
        return 2;
    }
    return 0;
}

void ExampleAppInit(const ExampleAppOptions* options, const ExamplePlatform* platform)
{
    g_App = ExampleAppState {};
    g_Platform = *platform;
    LoadOptions(options);
}

void ExampleAppRebuildFonts()
{
    RebuildFonts();
}

void ExampleAppBuildFrame()
{
    BuildImGuiFrame();
}

void ExampleAppCaptureDrawStats()
{
    CaptureDrawStats();
}

bool ExampleAppToggleDebugOverlay(uint32_t flag)
{
    return ToggleDebugOverlay(flag);
}

void ExampleAppSetTextRenderer(bool atlas)
{
    SetTextRenderer(atlas);
}

void ExampleAppResetFonts()
{
    g_App.BaseSize = kExampleDefaultSize;
    g_App.DpiScale = 1.0f;
    g_App.FontsDirty = true;
    printf("[imslug] fonts reset to %d px, DPI 1.00\n", g_App.BaseSize);
}
