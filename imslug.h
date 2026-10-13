// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// Public C++ header. Constraints (AGENTS.md invariant 4, PLAN §6):
//   * must stay C++11-compatible — no C++14/17 constructs here;
//   * must not use the C++ standard library (no <string>, no <vector>, ...);
//   * implementation is C++17 and uses no exceptions.
//
// The C API mirror arrives in M6; the glyph pipeline landed in M1 and the
// ImGui integration arrives in M3 (see .agent-docs/milestones/).

#pragma once

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
#define IMSLUG_VERSION_MAJOR 0
#define IMSLUG_VERSION_MINOR 1
#define IMSLUG_VERSION_PATCH 0

// Numeric version for compile-time comparisons, e.g. 0.1.0 -> 100.
#define IMSLUG_VERSION_NUM ((IMSLUG_VERSION_MAJOR * 10000) + (IMSLUG_VERSION_MINOR * 100) + (IMSLUG_VERSION_PATCH))

// Version string of the library ("major.minor.patch", plus a suffix while pre-0.1).
#define IMSLUG_VERSION "0.1.0-dev"

// Export/visibility decoration hook. Empty for the static library; a future
// shared-library or C API build may redefine it (M6/M8).
#ifndef IMSLUG_API
#define IMSLUG_API
#endif

namespace ImSlug
{
    // Returns the library version as a static string, identical to IMSLUG_VERSION.
    IMSLUG_API const char* GetVersion();
}
