// imslug C API — C11 compilation + ABI gate.
//
// This translation unit is compiled as C11 with strict warnings
// (-Wall -Wextra -Werror on GCC/Clang, /W4 /WX on MSVC) and includes only
// imslug_c.h: it proves the header is C11-clean, that the C struct layouts are
// exactly what binding authors compile against, and that the C symbols link.
// The behavioral mirror checks live in tests/test_c_api.cpp (C++).
//
// Layout changes here are ABI breaks: update the version (imslug.h /
// imslug_c.h) and the bindings when you have to touch an assertion.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "imslug_c.h"

// ---------------------------------------------------------------------------
// Struct layout (fixed-size PODs, `int` booleans — see imslug_c.h)
// ---------------------------------------------------------------------------
_Static_assert(sizeof(imslug_config) == 6 * sizeof(uint32_t), "imslug_config size changed (ABI break)");
_Static_assert(offsetof(imslug_config, EnableSlugForRegisteredFonts) == 0, "imslug_config layout changed");
_Static_assert(offsetof(imslug_config, DebugDrawBounds) == 4, "imslug_config layout changed");
_Static_assert(offsetof(imslug_config, EnableFineClipReconstruction) == 8, "imslug_config layout changed");
_Static_assert(offsetof(imslug_config, MaxInstancesPerFrame) == 12, "imslug_config layout changed");
_Static_assert(offsetof(imslug_config, MaxGlyphSlabs) == 16, "imslug_config layout changed");
_Static_assert(offsetof(imslug_config, EnableColorFonts) == 20, "imslug_config layout changed");
_Static_assert(sizeof(((imslug_config*)0)->EnableColorFonts) == sizeof(int), "booleans are int in the C ABI");

_Static_assert(sizeof(imslug_statistics) == 6 * sizeof(uint32_t), "imslug_statistics size changed (ABI break)");
_Static_assert(offsetof(imslug_statistics, GlyphSlabs) == 0, "imslug_statistics layout changed");
_Static_assert(offsetof(imslug_statistics, LiveBatches) == 4, "imslug_statistics layout changed");
_Static_assert(offsetof(imslug_statistics, Instances) == 8, "imslug_statistics layout changed");
_Static_assert(offsetof(imslug_statistics, DroppedRuns) == 12, "imslug_statistics layout changed");
_Static_assert(offsetof(imslug_statistics, BoundsMismatches) == 16, "imslug_statistics layout changed");
_Static_assert(offsetof(imslug_statistics, ColorLayers) == 20, "imslug_statistics layout changed");

// The default ImWchar is 16-bit (define IMGUI_USE_WCHAR32 for non-BMP text).
_Static_assert(sizeof(imslug_wchar) == 2, "imslug_wchar must match ImWchar (16-bit without IMGUI_USE_WCHAR32)");

// Version macros must be internally consistent (the runtime comparison with
// the library's own values happens in imslug_version_num()).
_Static_assert(IMSLUG_VERSION_NUM ==
                   ((IMSLUG_VERSION_MAJOR * 10000) + (IMSLUG_VERSION_MINOR * 100) + IMSLUG_VERSION_PATCH),
               "IMSLUG_VERSION_NUM is inconsistent with the version components");

int main(void)
{
    // Callable without an ImGui context / module: pure queries and no-ops.
    const char* version = imslug_version_string();
    if (version == NULL || version[0] == '\0')
    {
        fprintf(stderr, "imslug_c_abi: FAIL: imslug_version_string() returned nothing\n");
        return 1;
    }
    if (imslug_version_num() != IMSLUG_VERSION_NUM)
    {
        fprintf(stderr, "imslug_c_abi: FAIL: imslug_version_num() != IMSLUG_VERSION_NUM\n");
        return 1;
    }

    const imslug_config config = imslug_config_default();
    if (!config.EnableSlugForRegisteredFonts || config.DebugDrawBounds || !config.EnableFineClipReconstruction ||
        config.MaxInstancesPerFrame != 0 || config.MaxGlyphSlabs != 0 || !config.EnableColorFonts)
    {
        fprintf(stderr, "imslug_c_abi: FAIL: imslug_config_default() is not the documented default\n");
        return 1;
    }

    imslug_get_statistics(NULL);  // documented no-op
    imslug_new_frame();           // no module: early-out
    imslug_shutdown();            // idempotent, no module
    if (imslug_is_registered(NULL) != 0)
    {
        fprintf(stderr, "imslug_c_abi: FAIL: imslug_is_registered(NULL) != 0\n");
        return 1;
    }

#ifdef IMSLUG_C_VULKAN
    // Vulkan declarations are visible (this TU was compiled with
    // IMSLUG_C_VULKAN); check the default init info and the uninitialized
    // overlay query (both are safe without a device).
    const imslug_vulkan_init_info vulkan_info = imslug_vulkan_init_info_default();
    if (vulkan_info.MSAASamples != VK_SAMPLE_COUNT_1_BIT || vulkan_info.MaxInstancesPerFrame != 0 ||
        vulkan_info.UseDynamicRendering != 0 || vulkan_info.PhysicalDevice != VK_NULL_HANDLE)
    {
        fprintf(stderr, "imslug_c_abi: FAIL: imslug_vulkan_init_info_default() is not zeroed + 1x MSAA\n");
        return 1;
    }
    if (imslug_impl_vulkan_get_debug_overlay() != IMSLUG_VULKAN_DEBUG_OVERLAY_NONE)
    {
        fprintf(stderr, "imslug_c_abi: FAIL: debug overlay is not NONE before init\n");
        return 1;
    }
#endif

    printf("imslug_c_abi: OK (C11, version %s)\n", version);
    return 0;
}
