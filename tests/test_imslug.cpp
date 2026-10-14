// imslug smoke test: the core library links, and the version string
// reported by the library matches the public header.
//
// The feature suites live alongside this file (outline extraction, bands,
// packing, CPU reference rasterizer, ...).

#include "imslug.h"

#include <cstdio>
#include <cstring>

int main()
{
    const char* version = ImSlug::GetVersion();
    if (version == nullptr || version[0] == '\0')
    {
        std::printf("imslug_smoke: FAIL: ImSlug::GetVersion() returned no string\n");
        return 1;
    }
    if (std::strcmp(version, IMSLUG_VERSION) != 0)
    {
        std::printf("imslug_smoke: FAIL: library version '%s' != header version '%s'\n", version, IMSLUG_VERSION);
        return 1;
    }
    if (IMSLUG_VERSION_NUM != ((IMSLUG_VERSION_MAJOR * 10000) + (IMSLUG_VERSION_MINOR * 100) + IMSLUG_VERSION_PATCH))
    {
        std::printf("imslug_smoke: FAIL: IMSLUG_VERSION_NUM is inconsistent with the version components\n");
        return 1;
    }

    std::printf("imslug_smoke: OK (version %s)\n", version);
    return 0;
}
