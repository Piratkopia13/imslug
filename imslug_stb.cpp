// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_stb.cpp — the single translation unit that instantiates the vendored
// stb_truetype implementation (third_party/stb/stb_truetype.h, unmodified;
// see PROVENANCE.txt there). It is built as its own target with third-party
// warning settings so /W4 never applies to stb (AGENTS.md invariant 2).
//
// Do not add anything else to this file.

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
