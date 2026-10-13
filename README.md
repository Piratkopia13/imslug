# imslug

**Slug-based GPU text rendering for Dear ImGui.** imslug replaces ImGui's
bitmap-atlas text rendering with the
[Slug algorithm](https://jcgt.org/published/0006/02/02/): glyphs are rendered
directly from their Bézier outlines in the fragment shader — at any size or
DPI, with analytic antialiasing, and without per-size atlas builds.

Vulkan-first and drop-in: no ImGui fork, stock `imgui_impl_vulkan` backend, and
a fallback path that keeps ordinary (non-registered) fonts working through
ImGui's texture pipeline.

> **Status: pre-0.1 — not usable yet.** The repository is at milestone M0
> (build/test/doc scaffolding only). The core glyph pipeline lands in M1, the
> standalone Vulkan renderer in M2, and the ImGui integration in M3.

## Requirements

* CMake ≥ 3.21 and a C++17 compiler (MSVC 2022, GCC, or Clang).
* Dear ImGui ≥ 1.92.8 — fetched automatically at configure time (pinned;
  override with `-DIMSLUG_IMGUI_DIR=<checkout>`).
* Vulkan SDK — needed from M2 onward (not required for the M0 build).
* GLFW — needed for the example app from M2 onward.

## Quickstart

Deterministic entry points live in `scripts/` and are used identically by
humans, CI and the `.opencode/commands`:

| Task | Windows | Linux / macOS |
|---|---|---|
| Configure + build | `scripts\build.bat` | `scripts/build.sh` |
| Build + run tests (ctest) | `scripts\test.bat` | `scripts/test.sh` |
| Regenerate shader header | `scripts\gen_shaders.bat` | `scripts/gen_shaders.sh` |
| Verify committed shader header | `scripts\gen_shaders.bat --check` | `scripts/gen_shaders.sh --check` |
| Golden-image tests (M2+) | `scripts\golden.bat` | `scripts/golden.sh` |
| Benchmarks (M4+) | `scripts\bench.bat` | `scripts/bench.sh` |
| Docs-hygiene check | `scripts\check_docs.bat` | `scripts/check_docs.sh` |

Agents should start with [`AGENTS.md`](AGENTS.md).

## Credits

The Slug algorithm and its reference shader implementation are by **Eric
Lengyel**:

* Paper: [GPU-Centered Font Rendering Directly from Glyph Outlines](https://jcgt.org/published/0006/02/02/) (JCGT 2017)
* Reference shaders: [EricLengyel/Slug](https://github.com/EricLengyel/Slug)
  (dual MIT / Apache-2.0 — "Slug shader code Copyright 2017 by Eric Lengyel")
* Patent US 10,373,352 is dedicated to the public domain:
  [A Decade of Slug](https://terathon.com/blog/decade-slug.html)

Prior art that made this practical: [DearWidgets](https://github.com/soufianekhiat/DearWidgets) (CC0),
[Slughorn](https://github.com/AlphaPixel/slughorn) (MIT),
[Sluggish](https://github.com/mightycow/Sluggish) (Unlicense),
[slug-webgpu](https://github.com/diffusionstudio/slug-webgpu) (MIT),
[JSlug](https://github.com/manthrax/JSlug) (Unlicense).
See [`NOTICE`](NOTICE) for the full attribution and license texts.

## License

MIT — see [`LICENSE`](LICENSE).

## Disclaimer

imslug is an unofficial project, not affiliated with or endorsed by Terathon
Software. "Slug" refers to the rendering algorithm by Eric Lengyel; Slug® is a
trademark of Terathon Software LLC.
