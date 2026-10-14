# imslug — FAQ

Short answers to the questions that come up when adopting imslug. Design
details live in [`DESIGN.md`](DESIGN.md); integration steps in
[`INTEGRATION.md`](INTEGRATION.md).

## General

**What is imslug, in one paragraph?**
A drop-in extension for Dear ImGui that renders registered fonts with the
[Slug algorithm](https://jcgt.org/published/0006/02/02/) directly from their
outlines in a Vulkan fragment shader — no glyph atlas, no per-size
rasterization, analytic antialiasing at any size or DPI. Fonts you do not
register keep using ImGui's normal texture pipeline. It is not a fork: it plugs
into ImGui ≥ 1.92.8 through the documented font-loader and draw-data interfaces.

**Which Dear ImGui versions work?**
1.92.8 or newer — `imslug.h` fails at compile time on older versions.
`v1.92.9b` is the pinned and tested release; a daily CI smoke job tracks Dear
ImGui `master` so API drift is noticed early (1.93 is not supported yet).

**Which platforms / backends?**
Vulkan (1.2+; the committed SPIR-V is 1.5) is the reference backend, on any OS
Vulkan runs on (developed and CI-tested on Windows/MSVC and Linux/GCC). A
**WebGPU backend** (WGSL shaders, `backends/imslug_impl_webgpu.*`) runs the
same pipeline in the browser through Emscripten. The core, pre-pass and data
formats are backend-independent, so a further backend is a new module, not a
rewrite; a desktop GL3 backend has no plan of record (WebGPU covers the
browser).

**Does it need a specific GPU?**
No. The device must support the two pool formats
(`R16G16B16A16_SFLOAT`, `R16G16_UINT`) and Vulkan 1.2; if not, imslug reports
failure at init and text stays on the stock path. The CI golden tests run on
lavapipe (software Vulkan), and local development targets a discrete GPU.

**What does the browser demo need?**
WebGPU: Chrome/Edge 113+ (Windows/macOS/ChromeOS), 121+ (Android) or recent
Linux; Firefox 141+ (Windows) / 145+ (macOS); Safari 26+ (macOS/iOS). The page
checks `navigator.gpu` and shows a notice otherwise. The web build has no
filesystem: fonts are the embedded Roboto packed asset / atlas TTF, plus
build-time subsets of the fetched CJK and color-emoji fonts when present (the
emoji subset is COLR v1 only, ~12 KB, so the full 25 MB font is never shipped).
See `docs/INTEGRATION.md` §13 and `scripts/build_web.*`.

**How mature is it?**
v0.1.0 is the first release: feature-complete (glyph pipeline, Vulkan renderer,
ImGui integration, color fonts, C API, offline assets), with unit, fuzz,
golden-image and ImGui-level test suites and a two-platform CI matrix. Treat
0.x semantics as the contract: additions are source- and ABI-compatible, and
breaking changes bump the minor version.

## Fonts and rendering

**OTF/CFF fonts — do they work?**
Not in v0.1. The tested and supported path is TrueType (`glyf`) outlines.
`stb_truetype` does parse some CFF fonts and imslug's cubic→quadratic converter
exists (`ImSlugConvertCubicToQuadratics`, unit-tested), but the CFF path is
outside the test matrix and unverified end to end (metrics parity, band
construction, color tables), so treat it as unsupported. If the parser rejects
your font, `ImSlug::AddFontFromFileTTF` returns `NULL` (Dear ImGui rolls the
source back); add the font with `io.Fonts->AddFontFromFileTTF` instead and it
renders through the stock atlas path. Full CFF support (FreeType or a dedicated
CFF pass) is planned after v0.1 — deliberately **no new dependency** is being
added for it now (see `DESIGN.md` §8).

**Why is small text less sharp than an OS-rendered font?**
Slug is unhinted by design — and so is Dear ImGui's own stb loader: neither
snaps stems to the pixel grid. At ≥ 16 px the coverage matches the atlas path
closely; below ~12 px stems lose up to 0.14 peak coverage, and 4× supersampling
was measured and deliberately not shipped (5–10 % mean improvement for 16× the
fragment cost). Practical advice: use ≥ 13–16 px for body text and keep ImGui's
default pixel-snapped pen positions. The full table and the reasoning are in
`DESIGN.md` §7.

**How does DPI / `style.FontScaleDpi` work with imslug?**
No special handling is needed. Glyph data is size-independent (em space), so
runtime size/DPI changes re-bake nothing: the fragment shader resolves coverage
per pixel. This is the main structural advantage over atlas text, which must
re-rasterize per (font, size, density). The example's scene 5 exercises a
runtime `FontScaleDpi` slider.

**Do Slug fonts allocate glyph bitmaps?**
No. Registered fonts are rect-less: nothing is rasterized or packed into the
atlas, and their UVs are imslug's glyph handles. The atlas is free for the
fonts that stay on the stock path. One full Latin font costs roughly
0.25–0.4 MB of pool data once (curves + bands; the CJK subset ~0.8 MB),
uploaded as new glyphs are built.

**Can I mix Slug fonts and atlas fonts in one UI?**
Yes, in the same frame and even in the same draw list. Every font that is not
registered through `ImSlug::AddFont*` renders through ImGui's normal path;
mixed-font scenes are covered by the tests.

**What about color emoji?**
COLR v0/v1 color glyphs render with one Slug instance per layer and
per-fragment gradients (Noto Color Emoji and Segoe UI Emoji verified). The
documented approximations for a few rare composite modes are in `DESIGN.md`
§9. Most emoji are non-BMP codepoints: the whole build needs
`IMGUI_USE_WCHAR32` for those (imslug's CMake defines it by default:
`IMSLUG_WCHAR32=ON`; the example's snail mascot relies on it). Packed
`.imslug` v1 assets are monochrome only.

**Why do I see no text at very large glyph counts?**
The per-frame instance ring (default 16384 glyphs/layers) only drops runs that
do not fit — and never silently: the first drop prints a one-time stderr warning
with the demand and the capacity, and with the default capacity the ring grows
for the next frame (doubling, capped at 8x the default), so the text returns.
A **fixed** `Config::MaxInstancesPerFrame` is a hard cap; leave it 0 (adaptive)
or raise it. Ongoing drops show in `Statistics::DroppedRuns`.

## Performance

**Is it slower than the atlas path?**
For text-heavy frames, yes on the GPU by design: the fragment shader fetches
band headers and walks ~6 references per pixel instead of one bilinear atlas
fetch. Measured on an RTX 5080 at 1024×640: 2.3–2.8× the atlas time for
text-dominated scenes and **+0.011 ms** in a typical UI frame (under 0.2 % of a
60 Hz frame). CPU-side the pre-pass is ~0.3 ms for 5k glyphs. Full numbers,
method and the honest caveats are in `DESIGN.md` §4; the release criterion is
frame-time parity with a bounded, text-share-proportional delta, not per-pixel
parity.

**What do I get for that cost?**
Resolution-independent text (one dataset for all sizes/DPIs/zooms), perfect
analytic AA, no atlas rebuilds or bitmap memory, deterministic quality under
arbitrary transforms, and one draw callback (+ reset) per text run rather than
per-glyph draws.

**Can I make it faster?**
Band counts are the single tuning knob
(`clamp(ceil(2*sqrt(n)), 4, 32)` per axis by default; see
`tests/bench/band_sweep.cpp`). Smaller `MaxInstancesPerFrame` only bounds
memory. The debug overlay's reference-walk heatmap (`DESIGN.md` §6) shows where
fragment time goes.

## Licensing, naming and project status

**What license is imslug under?**
MIT (see `LICENSE`). The ported Slug shader code is by **Eric Lengyel** and is
used under his dual MIT / Apache-2.0 terms with the required credit, and the
patent (US 10,373,352) was dedicated to the public domain. `NOTICE` carries the
full attribution — the Lengyel credit, the stb license, the prior-art projects
(DearWidgets CC0, Slughorn MIT, Sluggish, slug-webgpu, JSlug, osgSlug) and the
test-font licenses. Never remove `NOTICE`; attribution is a hard requirement of
the project.

**Is "imslug" affiliated with Terathon Software or the commercial Slug
Library?**
No. imslug is an unofficial, independent project, not affiliated with or
endorsed by Terathon Software. "Slug" refers to the rendering algorithm by Eric
Lengyel; Slug® is a trademark of Terathon Software LLC. References are
descriptive/nominative. imslug does not use or reverse-engineer the commercial
Slug Library or its tooling.

**Can I use it commercially?**
The licenses (MIT for imslug; MIT/Apache-2.0 for the ported shader code;
patent dedicated to the public domain) permit it. You are responsible for your
own compliance, including the attribution in `NOTICE` and the licenses of any
fonts you ship.

**Where do I report bugs or ask questions?**
Use the issue templates in `.github/ISSUE_TEMPLATE/` (they ask for the version,
ImGui version, GPU/driver, build options and a minimal scene). Rendering
issues are much easier to triage with a `--screenshot` PPM from the example app
and the validation-layer output.

**What is not in v0.1?**
OTF/CFF (above), a desktop GL backend, multi-viewport beyond compatible render
passes, packed color assets, and the `imgui-rs-vulkan-renderer` integration
(prerequisites documented in `docs/BINDINGS.md`). `DESIGN.md` §8 is the
authoritative list.
