// imslug — implementation.
//
// imslug_c.cpp — C API: translation between the C surface (imslug_c.h)
// and the canonical C++ API (imslug.h). Every function is a thin mirror: the
// only logic here is the int<->bool translation of the config struct and the
// statistics copy, so the C API cannot drift behaviorally from the C++ API.
//
// Vulkan entry points live in backends/imslug_c_vulkan.cpp (invariant 7: the
// core stays backend-independent).

#include "imslug.h"  // canonical C++ API (defines the version macros, imgui.h)

#include "imslug_c.h"
#include "imslug_packed.h"  // packed-asset validation entry points

// The C mirror is ABI-pinned by tests/test_c_api.cpp; these asserts keep the
// implementation honest even in TUs that do not build the tests.
static_assert(sizeof(imslug_config) == 6 * sizeof(uint32_t), "C ABI: imslug_config is a fixed-size POD");
static_assert(sizeof(imslug_statistics) == 6 * sizeof(uint32_t), "C ABI: imslug_statistics is a fixed-size POD");

namespace
{
    bool CppBool(const int value)
    {
        return value != 0;
    }

    int CBool(const bool value)
    {
        return value ? 1 : 0;
    }

    ImSlug::Config ToCppConfig(const imslug_config& config)
    {
        ImSlug::Config cpp;
        cpp.EnableSlugForRegisteredFonts = CppBool(config.EnableSlugForRegisteredFonts);
        cpp.DebugDrawBounds = CppBool(config.DebugDrawBounds);
        cpp.EnableFineClipReconstruction = CppBool(config.EnableFineClipReconstruction);
        cpp.MaxInstancesPerFrame = config.MaxInstancesPerFrame;
        cpp.MaxGlyphSlabs = config.MaxGlyphSlabs;
        cpp.EnableColorFonts = CppBool(config.EnableColorFonts);
        return cpp;
    }
}  // namespace

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
const char* imslug_version_string(void)
{
    return ImSlug::GetVersion();
}

uint32_t imslug_version_num(void)
{
    return (uint32_t)IMSLUG_VERSION_NUM;
}

// ---------------------------------------------------------------------------
// Configuration + lifetime
// ---------------------------------------------------------------------------
imslug_config imslug_config_default(void)
{
    // Default-constructed ImSlug::Config is the single source of the defaults.
    const ImSlug::Config cpp;
    imslug_config config;
    config.EnableSlugForRegisteredFonts = CBool(cpp.EnableSlugForRegisteredFonts);
    config.DebugDrawBounds = CBool(cpp.DebugDrawBounds);
    config.EnableFineClipReconstruction = CBool(cpp.EnableFineClipReconstruction);
    config.MaxInstancesPerFrame = cpp.MaxInstancesPerFrame;
    config.MaxGlyphSlabs = cpp.MaxGlyphSlabs;
    config.EnableColorFonts = CBool(cpp.EnableColorFonts);
    return config;
}

int imslug_init(imslug_config config)
{
    return CBool(ImSlug::Init(ToCppConfig(config)));
}

void imslug_shutdown(void)
{
    ImSlug::Shutdown();
}

void imslug_new_frame(void)
{
    ImSlug::NewFrame();
}

// ---------------------------------------------------------------------------
// Font registration
// ---------------------------------------------------------------------------
ImFont* imslug_add_font_from_file_ttf(const char* path, float size_pixels, const ImFontConfig* font_config,
                                      const imslug_wchar* glyph_ranges)
{
    return ImSlug::AddFontFromFileTTF(path, size_pixels, font_config, (const ImWchar*)glyph_ranges);
}

ImFont* imslug_add_font_from_memory_ttf(const void* data, int data_size, float size_pixels,
                                        const ImFontConfig* font_config, const imslug_wchar* glyph_ranges)
{
    return ImSlug::AddFontFromMemoryTTF(data, data_size, size_pixels, font_config, (const ImWchar*)glyph_ranges);
}

int imslug_is_registered(const ImFont* font)
{
    return CBool(ImSlug::IsRegistered(font));
}

// ---------------------------------------------------------------------------
// Packed assets
// ---------------------------------------------------------------------------
int imslug_is_packed_font_data(const void* data, int data_size)
{
    return CBool(ImSlug::IsPackedFontData(data, data_size));
}

int imslug_validate_packed_font_data(const void* data, int data_size, char* error_message, int error_message_capacity)
{
    return CBool(ImSlug::ImSlugValidatePackedMemory(data, data_size, nullptr, error_message, error_message_capacity));
}

// ---------------------------------------------------------------------------
// Draw pipeline
// ---------------------------------------------------------------------------
void imslug_preprocess_draw_data(ImDrawData* draw_data)
{
    ImSlug::PreprocessDrawData(draw_data);
}

void imslug_draw_callback(const ImDrawList* parent_list, const ImDrawCmd* cmd)
{
    ImSlug::DrawCallback(parent_list, cmd);
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
void imslug_get_statistics(imslug_statistics* out)
{
    if (out == nullptr)
        return;
    ImSlug::Statistics stats;
    ImSlug::GetStatistics(&stats);
    out->GlyphSlabs = stats.GlyphSlabs;
    out->LiveBatches = stats.LiveBatches;
    out->Instances = stats.Instances;
    out->DroppedRuns = stats.DroppedRuns;
    out->BoundsMismatches = stats.BoundsMismatches;
    out->ColorLayers = stats.ColorLayers;
}
