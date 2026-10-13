// imslug — implementation.
//
// M0 stub: only the version API exists so far. The core glyph pipeline
// (stb_truetype outline extraction, band construction, pool packing, CPU
// reference rasterizer) lands in M1; see .agent-docs/milestones/M1-*.md and
// .agent-docs/PLAN.md §4. Keep this file free of ImGui and Vulkan dependencies.

#include "imslug.h"

namespace ImSlug
{
    const char* GetVersion()
    {
        return IMSLUG_VERSION;
    }
}
