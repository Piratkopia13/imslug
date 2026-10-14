# imslug

**Slug-based GPU text rendering for Dear ImGui.** imslug replaces ImGui's
bitmap-atlas text rendering with the
[Slug algorithm](https://jcgt.org/published/0006/02/02/): glyphs are rendered
directly from their Bézier outlines in the fragment shader — at any size or
DPI, with analytic antialiasing, and without per-size atlas builds.

Vulkan-first and drop-in: no ImGui fork, stock `imgui_impl_vulkan` backend, and
a fallback path that keeps ordinary (non-registered) fonts working through
ImGui's texture pipeline.

> **Status: v0.1.0 — release prepared; the `v0.1.0` tag is pending the repo
> owner.** The feature set is complete: the Slug glyph pipeline and
> Vulkan renderer, the ImGui integration (mixed fonts, clipping, runtime DPI),
> color fonts (COLR v0/v1), the C API (`imslug_c.h`) and the Rust wrapper
> (`imslug-rs`), and the offline generator + `.imslug` container v1 — including
> a build without any TrueType parser. See [`CHANGELOG.md`](CHANGELOG.md) for
> the release history and [`docs/INTEGRATION.md`](docs/INTEGRATION.md) for the
> drop-in guide.

## Requirements

* CMake ≥ 3.21 and a C++17 compiler (MSVC 2022, GCC, or Clang).
* Dear ImGui ≥ 1.92.8 — fetched automatically at configure time (pinned;
  override with `-DIMSLUG_IMGUI_DIR=<checkout>`).
* Vulkan 1.2+ SDK — needed for the backend module, the golden tests and the
  example (the core still builds without Vulkan).
* GLFW — needed for the example app (fetched at a pinned tag; override with
  `-DIMSLUG_GLFW_DIR=<checkout>`).

## Quickstart

Deterministic entry points live in `scripts/` and are used identically by
humans, CI and the `.opencode/commands`:

| Task | Windows | Linux / macOS |
|---|---|---|
| Configure + build | `scripts\build.bat` | `scripts/build.sh` |
| Build + run tests (ctest) | `scripts\test.bat` | `scripts/test.sh` |
| Regenerate shader header | `scripts\gen_shaders.bat` | `scripts/gen_shaders.sh` |
| Verify committed shader header | `scripts\gen_shaders.bat --check` | `scripts/gen_shaders.sh --check` |
| Regenerate the packed test asset | `scripts\gen_asset.bat` | `scripts/gen_asset.sh` |
| Golden-image tests | `scripts\golden.bat` | `scripts/golden.sh` |
| Clean-project FetchContent check | `scripts\verify_fetchcontent.bat` | `scripts/verify_fetchcontent.sh` |
| Dear ImGui master smoke (drift check) | `scripts\smoke_imgui_master.bat` | `scripts/smoke_imgui_master.sh` |
| Benchmarks | `scripts\bench.bat` | `scripts/bench.sh` |
| Docs-hygiene check | `scripts\check_docs.bat` | `scripts/check_docs.sh` |

Agents should start with [`AGENTS.md`](AGENTS.md).

### Running the example app

The GLFW+Vulkan example is off by default. Enable it once through the build
script (Windows cmd can't pass `-D...=ON` through the batch file, so it goes
through `IMSLUG_CMAKE_DEFINES`; needs the Vulkan SDK, GLFW is fetched
automatically):

```bat
set IMSLUG_CMAKE_DEFINES=-DIMSLUG_BUILD_EXAMPLES=ON
scripts\build.bat
```

```powershell
$env:IMSLUG_CMAKE_DEFINES = "-DIMSLUG_BUILD_EXAMPLES=ON"
scripts\build.bat
```

```sh
scripts/build.sh -DIMSLUG_BUILD_EXAMPLES=ON
```

The option is cached in the build directory, so later builds only need
`scripts\build.bat` (`--target imslug_example_glfw_vulkan` builds just the
example). Run `build\imslug_example_glfw_vulkan.exe` (`build/Release/...`
with the Visual Studio generator). Keys 1–9 (plus `P` for the heavy demo)
switch scenes, `M` swaps Slug and ImGui's atlas renderer for the body text,
`+`/`-` change the base font size, F1/F2/F3 toggle the debug overlays, Esc
quits; `--help` lists the command line, including
`--screenshot <file.ppm> [scene] [dpi_scale]` for offscreen captures.

The example is standalone: the default fonts (the packed Roboto ASCII asset
for Slug and the Roboto TTF for ImGui's stock atlas loader, so the `M`
comparison works too) are embedded in the binary — no data files are needed
next to the executable. `--font <ttf|imslug>` or the `IMSLUG_EXAMPLE_FONT`
environment variable selects a different font at runtime (`--font` wins).
For a truly single-file executable that also runs without the Visual C++
redistributable, configure with `IMSLUG_STATIC_CRT=ON` (MSVC only; add
`-DIMSLUG_STATIC_CRT=ON` to `IMSLUG_CMAKE_DEFINES`) — the only remaining
dependency is `vulkan-1.dll` from the GPU driver. (It covers the targets this
project builds; a caller-provided `glfw`/imgui target must also use `/MT`.)

## Integration

```cpp
ImGui_ImplVulkan_Init(&init_info);       // stock backend, unchanged
ImSlug::Init();                          // imslug core (fonts go through it)
ImSlug_ImplVulkan_Init(&init_info);      // imslug Vulkan module
ImFont* font = ImSlug::AddFontFromFileTTF("fonts/Inter.ttf", 18.0f);
io.FontDefault = font;                   // or use it per window/font stack

// Per frame, before the render pass:
ImSlug_ImplVulkan_UploadPools(cmd);      // dirty glyph-pool rows only
// Inside the render pass, instead of ImGui_ImplVulkan_RenderDrawData:
ImSlug_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd, pipeline);
```

Registered fonts render through Slug at any size/DPI without atlas bitmaps;
every other font, shape and texture keeps going through ImGui's own path.
The full drop-in guide — CMake/FetchContent setup, init order, the offline
asset path, multi-viewport limits and troubleshooting — is
[`docs/INTEGRATION.md`](docs/INTEGRATION.md).
Color fonts (COLR v0/v1) render one Slug instance per layer with per-fragment
paint evaluation (flat colors, linear/radial/sweep gradients, ClipBoxes); the
fetched Noto Color Emoji is used by the emoji scene and the emoji goldens.

**Offline generation.** The same entry points also accept pre-baked
`.imslug` assets — auto-detected by content, no outline parsing:

```sh
imslug_gen font fonts/Inter.ttf --size-range 8:72 --chars 0x20-0x7E -o inter.imslug
```

```cpp
ImFont* font = ImSlug::AddFontFromFileTTF("inter.imslug", 18.0f);  // same API
```

The container v1 (magic `IMSLUG`, versioned, little-endian, deterministic) holds
font metrics, glyph records and the packed curve/band texels; `imslug_gen info`
validates and inspects an asset and `--emit-header` embeds one in C.
`imslug_gen embed <file>` wraps any raw file (a TTF, an image, …) in the same
deterministic byte-array header. A build with
`-DIMSLUG_TTF_PARSER=OFF` ships **without any TrueType parser** and loads packed
assets only (the example's scene 9 and the no-parser CI job demonstrate it).
Format, validation rules and limits: [`docs/PACKED_FORMAT.md`](docs/PACKED_FORMAT.md).

The GLFW+Vulkan example (`IMSLUG_BUILD_EXAMPLES=ON`,
`build/imslug_example_glfw_vulkan.exe`) has a scene switcher for the demo
window, widgets/plots/tables, a clipping torture scene, mixed Slug+atlas fonts,
a runtime `style.FontScaleDpi` slider, a 5k-glyph stress scene, the fetched
CJK subset, the color-emoji scene (with a zoom slider for the gradients), the
packed-asset scene and a heavy demo / perf scene (key `P`) with a performance
panel — wall-clock frame-time plot, whole-frame GPU timestamps, draw / Slug /
atlas counters and a vsync toggle (`--novsync` for uncapped runs). `M` (or
`--renderer atlas`) swaps the body text between Slug and ImGui's dynamic font
system for a direct comparison of the same font; debug overlay toggles are
F1/F2/F3 and `--screenshot <file.ppm> [scene] [dpi_scale]` captures offscreen
frames.

C and Rust consumers use the `extern "C"` mirror (`imslug_c.h`) instead of the
C++ API; `example/c/` is a pure-C11 example (CPU smoke, plus a headless Vulkan
frame with `--vulkan`) and `imslug-rs` is a sibling wrapper crate. Conventions,
ownership rules and the binding status live in
[`docs/BINDINGS.md`](docs/BINDINGS.md).

Design notes, the measured performance numbers and the debug-overlay /
small-size policy live in [`docs/DESIGN.md`](docs/DESIGN.md); the common
questions (OTF/CFF, sharpness, DPI, licensing, performance) are answered in
[`docs/FAQ.md`](docs/FAQ.md); the benchmark harness is `scripts\bench.bat` /
`scripts/bench.sh`.

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
