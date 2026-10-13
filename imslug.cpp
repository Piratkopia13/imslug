// imslug — implementation.
//
// This file holds the version API (M0). The core glyph pipeline
// (stb_truetype outline extraction, band construction, pool packing, CPU
// reference rasterizer) lives in imslug_glyphgen/packer/refraster.cpp (M1);
// see .agent-docs/PLAN.md §4. Keep this file free of ImGui and Vulkan
// dependencies.

#include "imslug.h"

namespace ImSlug
{
    const char* GetVersion()
    {
        return IMSLUG_VERSION;
    }
}
