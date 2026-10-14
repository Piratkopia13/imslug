// imslug C API — behavioral mirror checks.
//
// Compiles as C++17 against both headers at once: the C API (imslug_c.h) must
// mirror the canonical C++ API (imslug.h) value-for-value, and the header must
// be includable from C++ as well as C. The pure ABI gate (C11 + strict
// warnings) is tests/test_c_abi.c.

#include "imslug.h"  // canonical C++ API

#include "imslug_c.h"  // the C mirror

#include "imslug_internal.h"  // ImSlugGetConfig (test-only introspection)
#include "test_util.h"

#include <cstring>

// ---------------------------------------------------------------------------
// Struct layout (duplicated from tests/test_c_abi.c on purpose: both
// compilation modes must pin the same layout)
// ---------------------------------------------------------------------------
static_assert(sizeof(imslug_config) == 6 * sizeof(uint32_t), "imslug_config size changed (ABI break)");
static_assert(offsetof(imslug_config, EnableSlugForRegisteredFonts) == 0, "imslug_config layout changed");
static_assert(offsetof(imslug_config, DebugDrawBounds) == 4, "imslug_config layout changed");
static_assert(offsetof(imslug_config, EnableFineClipReconstruction) == 8, "imslug_config layout changed");
static_assert(offsetof(imslug_config, MaxInstancesPerFrame) == 12, "imslug_config layout changed");
static_assert(offsetof(imslug_config, MaxGlyphSlabs) == 16, "imslug_config layout changed");
static_assert(offsetof(imslug_config, EnableColorFonts) == 20, "imslug_config layout changed");
static_assert(sizeof(imslug_statistics) == 6 * sizeof(uint32_t), "imslug_statistics size changed (ABI break)");

namespace
{
    // The pre-pass refuses to run without a renderer that sets
    // RendererHasTextures + the platform reset callback; this suite mirrors
    // the C API only, so the hooks are just what Init/AddFont expect to see.
    void FakeResetRenderState(const ImDrawList*, const ImDrawCmd*) {}

    bool SameStatistics(const imslug_statistics& c, const ImSlug::Statistics& cpp)
    {
        return c.GlyphSlabs == cpp.GlyphSlabs && c.LiveBatches == cpp.LiveBatches && c.Instances == cpp.Instances &&
               c.DroppedRuns == cpp.DroppedRuns && c.BoundsMismatches == cpp.BoundsMismatches &&
               c.ColorLayers == cpp.ColorLayers;
    }
}  // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("imslug_c_api: C API mirror tests\n");

    // -----------------------------------------------------------------------
    // Version
    // -----------------------------------------------------------------------
    TEST_CHECK(imslug_version_string() != nullptr);
    TEST_CHECK(std::strcmp(imslug_version_string(), ImSlug::GetVersion()) == 0);
    TEST_CHECK(std::strcmp(imslug_version_string(), IMSLUG_VERSION) == 0);
    TEST_CHECK(imslug_version_num() == (uint32_t)IMSLUG_VERSION_NUM);

    // -----------------------------------------------------------------------
    // Config defaults mirror ImSlug::Config's defaults
    // -----------------------------------------------------------------------
    {
        const ImSlug::Config cpp;
        const imslug_config c = imslug_config_default();
        TEST_CHECK((c.EnableSlugForRegisteredFonts != 0) == cpp.EnableSlugForRegisteredFonts);
        TEST_CHECK((c.DebugDrawBounds != 0) == cpp.DebugDrawBounds);
        TEST_CHECK((c.EnableFineClipReconstruction != 0) == cpp.EnableFineClipReconstruction);
        TEST_CHECK(c.MaxInstancesPerFrame == cpp.MaxInstancesPerFrame);
        TEST_CHECK(c.MaxGlyphSlabs == cpp.MaxGlyphSlabs);
        TEST_CHECK((c.EnableColorFonts != 0) == cpp.EnableColorFonts);
    }

    // -----------------------------------------------------------------------
    // Safe calls without a context: no crash, documented values
    // -----------------------------------------------------------------------
    TEST_CHECK(imslug_is_registered(nullptr) == 0);
    imslug_get_statistics(nullptr);
    imslug_new_frame();
    imslug_shutdown();
    imslug_preprocess_draw_data(nullptr);
    imslug_draw_callback(nullptr, nullptr);

    // -----------------------------------------------------------------------
    // Context + core mirror
    // -----------------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.IniFilename = nullptr;
    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = FakeResetRenderState;

    {
        imslug_config config = imslug_config_default();
        config.EnableColorFonts = 0;  // exercise a non-default value
        config.MaxGlyphSlabs = 1234;  // int -> uint32 passthrough
        config.DebugDrawBounds = 0;
        TEST_CHECK(imslug_init(config) == 1);

        const ImSlug::Config* active = ImSlug::ImSlugGetConfig();
        TEST_CHECK(active != nullptr);
        if (active != nullptr)
        {
            TEST_CHECK(active->EnableColorFonts == false);
            TEST_CHECK(active->MaxGlyphSlabs == 1234u);
            TEST_CHECK(active->EnableSlugForRegisteredFonts == true);
            TEST_CHECK(active->EnableFineClipReconstruction == true);
        }

        // Font registration through the C API: the returned ImFont is a
        // normal ImFont and both registration queries agree. (The NULL
        // arguments are only safe with a live context, like the C++ API.)
        TEST_CHECK(imslug_add_font_from_file_ttf(nullptr, 18.0f, nullptr, nullptr) == nullptr);
        TEST_CHECK(imslug_add_font_from_memory_ttf(nullptr, 0, 18.0f, nullptr, nullptr) == nullptr);
        ImFont* font =
            imslug_add_font_from_file_ttf(IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf"), 18.0f, nullptr, nullptr);
        TEST_CHECK(font != nullptr);
        if (font != nullptr)
        {
            TEST_CHECK(imslug_is_registered(font) == 1);
            TEST_CHECK(ImSlug::IsRegistered(font));
        }

        // A stock (non-Slug) font must not be reported as registered.
        ImFont* stock = ImGui::GetIO().Fonts->AddFontDefault();
        TEST_CHECK(stock != nullptr);
        if (stock != nullptr)
            TEST_CHECK(imslug_is_registered(stock) == 0);

        // Memory path: ownership transfers to the atlas (do not free here).
        int fontDataSize = 0;
        unsigned char* fontData = imslug_test::LoadFontFile("Inter-Regular.ttf", &fontDataSize);
        TEST_CHECK(fontData != nullptr);
        if (fontData != nullptr)
        {
            ImFont* memoryFont = imslug_add_font_from_memory_ttf(fontData, fontDataSize, 16.0f, nullptr, nullptr);
            TEST_CHECK(memoryFont != nullptr);
            if (memoryFont != nullptr)
                TEST_CHECK(imslug_is_registered(memoryFont) == 1);
        }

        // Statistics mirror the C++ API field-for-field.
        imslug_statistics cStats;
        ImSlug::Statistics cppStats;
        imslug_get_statistics(&cStats);
        ImSlug::GetStatistics(&cppStats);
        TEST_CHECK(SameStatistics(cStats, cppStats));

        // Statistics with a live core still mirror after a manual NewFrame.
        imslug_new_frame();
        imslug_get_statistics(&cStats);
        ImSlug::GetStatistics(&cppStats);
        TEST_CHECK(SameStatistics(cStats, cppStats));
    }

    imslug_shutdown();
    ImGui::DestroyContext();

    // After shutdown every query is safe again.
    imslug_shutdown();
    imslug_get_statistics(nullptr);

    return imslug_test::Finish("imslug_c_api");
}
