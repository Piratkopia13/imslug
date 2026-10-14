// The public header must stay C++11-compatible and STL-free (AGENTS.md
// invariant 4). This translation unit is deliberately compiled with
// the C++11 standard; it fails to build if that contract is broken.

#include "imslug.h"

#include <cstring>

static_assert(IMSLUG_VERSION_MAJOR >= 0, "version macros must be usable in constant expressions");

int main()
{
    return std::strcmp(ImSlug::GetVersion(), IMSLUG_VERSION) == 0 ? 0 : 1;
}
