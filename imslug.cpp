// imslug — implementation.
//
// This file holds the version API (M0) and the small backend-independent core
// helpers (M2: building a glyph instance from a record). The core glyph
// pipeline (stb_truetype outline extraction, band construction, pool packing,
// CPU reference rasterizer) lives in imslug_glyphgen/packer/refraster.cpp (M1);
// see .agent-docs/PLAN.md §4. Keep this file free of ImGui and Vulkan
// dependencies.

#include "imslug.h"

#include "imslug_internal.h"

namespace ImSlug
{
    const char* GetVersion()
    {
        return IMSLUG_VERSION;
    }

    void ImSlugBuildGlyphInstance(const ImSlugGlyphRecord* record, float penX, float penY, float pixelsPerEm, float r,
                                  float g, float b, float a, ImSlugGlyphInstance* outInstance)
    {
        IMSLUG_ASSERT(record != nullptr && outInstance != nullptr);
        IMSLUG_ASSERT(pixelsPerEm > 0.0f);

        const float inverse = 1.0f / pixelsPerEm;

        outInstance->EmMinX = record->MinX;
        outInstance->EmMinY = record->MinY;
        outInstance->EmMaxX = record->MaxX;
        outInstance->EmMaxY = record->MaxY;

        // Screen rect (Y down): the em bbox with the Y flip applied, so the
        // screen rect corner that matches em (MinX, MinY) is (MinX, ScreenMaxY).
        outInstance->ScreenMinX = penX + record->MinX * pixelsPerEm;
        outInstance->ScreenMinY = penY - record->MaxY * pixelsPerEm;
        outInstance->ScreenMaxX = penX + record->MaxX * pixelsPerEm;
        outInstance->ScreenMaxY = penY - record->MinY * pixelsPerEm;

        // Inverse Jacobian: screen delta -> em delta (em Y up, screen Y down).
        outInstance->Jac00 = inverse;
        outInstance->Jac01 = 0.0f;
        outInstance->Jac10 = 0.0f;
        outInstance->Jac11 = -inverse;

        outInstance->BandScaleX = record->BandScaleX;
        outInstance->BandScaleY = record->BandScaleY;
        outInstance->BandOffsetX = record->BandOffsetX;
        outInstance->BandOffsetY = record->BandOffsetY;

        outInstance->GlyphLocX = (uint32_t)record->BandLocX;
        outInstance->GlyphLocY = (uint32_t)record->BandLocY;
        outInstance->BandMaxWord = ImSlugPackBandMaxWord(record->BandMaxX, record->BandMaxY, record->Flags);

        outInstance->ColorR = r;
        outInstance->ColorG = g;
        outInstance->ColorB = b;
        outInstance->ColorA = a;
    }
}  // namespace ImSlug
