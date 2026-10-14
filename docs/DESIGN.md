# imslug — design notes

Durable notes on how imslug renders text, the format and performance numbers
behind the defaults, and the contracts an integrator can rely on. The runnable
contracts (exact field layouts, shader sources, test procedures) live in the
repository skills:

* `.opencode/skills/imslug-glyph-format/SKILL.md` — curve/band data, packing,
  metrics parity, glyph handles;
* `.opencode/skills/imslug-vulkan-backend/SKILL.md` — Vulkan pipeline,
  uploads, callbacks, validation;
* `.opencode/skills/imslug-shaders/SKILL.md` — shader sources, SPIR-V
  generation, porting rules.

## 1. Rendering model

imslug renders text with the Slug algorithm (Eric Lengyel; see `NOTICE`): a
font's quadratic outlines are stored once in two small textures and every glyph
is drawn as one instanced quad whose fragment shader solves coverage
analytically. There are **no glyph bitmaps**: fonts registered with
`ImSlug::AddFont*` never rasterize or pack into ImGui's atlas, and the atlas is
free for the fonts that stay on the stock path.

Integration is intentionally thin (see `README.md` and `imslug.h`):

```
ImGui::Render();
ImSlug_ImplVulkan_UploadPools(cmd);          // outside the render pass
... begin render pass ...
ImSlug_ImplVulkan_RenderDrawData(drawData, cmd);
```

The pre-pass (`ImSlug::PreprocessDrawData`, run by `RenderDrawData`) leaves
every non-text command untouched and rewrites each maximal run of Slug glyphs
into one callback command plus one stock reset command. Consequences:

* **Draw calls:** one callback (+ its reset) per contiguous run; never per
  glyph. A 5k-glyph stress frame is 2 commands.
* **Vertex traffic:** the draw data still carries ImGui's text vertices (they
  are rewritten to a callback with `ElemCount = 0`); the GPU never reads them.
  Removing them ("vertex compaction") was evaluated and deliberately skipped —
  it cannot change GPU time and only rewrites transient buffers; the numbers
  are in §5.
* **Instances:** one 92-byte record per glyph in a per-frame ring
  (`Config::MaxInstancesPerFrame`, default 16384). Ring exhaustion drops the
  overflowing run (release-safe; debug asserts) and is counted in
  `Statistics::DroppedRuns`.

## 2. Data and defaults

One curve pool (RGBA16F, 4096 texels wide) and one band pool (R16G16_UINT)
serve every Slug font; allocation is monotonic with dirty-row tracking, so
steady-state uploads are only the rows new glyphs touched (usually 0 after the
first frames). A full Latin UI font costs ~110–150 KB (curves) + ~140–240 KB
(bands) once; the fetched CJK subset ~780 KB total. Glyph records, band layout
and the UV handle encoding are documented in the glyph-format skill.

**Band count.** Bands are the acceleration structure for the fragment walk: a
uniform H/V grid per glyph whose reference lists are sorted so the shader can
stop early. The default is

```
bandCount = clamp(ceil(2 * sqrt(curveCount)), 4, 32)   // per axis
```

chosen in M4 from `tests/bench/band_sweep.cpp` measurements (8–72 px, Roboto /
Inter / CJK subset; mean references walked per pixel): the previous
`ceil(sqrt(2n))` walked 6.86–9.25 references per pixel; the new default walks
6.12–8.25 (~11 % fewer, ~15 % better p99/max tails) for ~25 % more band-pool
memory, which stays inside the memory budget. Larger coefficients (2.5×, 3×)
cost ~2× the memory for ~5 % more; the cap stays 32 because both band header
runs must fit in the glyph's first pool row. The single tuning point is
`ImSlugCalcBandCount()`.

**Metrics parity.** Glyphs use the exact metrics ImGui's stb loader bakes with
the config Slug fonts enforce (`OversampleH/V = 1`, `RasterizerDensity = 1`,
`ExtraSizeScale = 1`, `GlyphOffset = 0`, no `PixelSnapH`), so Slug and atlas
text lay out identically — pinned by `tests/test_imguiparity.cpp`.

## 3. Fine clipping

ImGui clips text on the CPU for axis-aligned cases by moving the quad's corners
and interpolating the UVs. Slug's UVs encode the corner position inside the
glyph's pixel box (an affine handle), so the pre-pass reconstructs the glyph's
**unclipped** placement from the clipped corners and lets the scissor cut it —
text keeps its true position and shape at clip edges instead of being squeezed.

`Config::EnableFineClipReconstruction = false` selects the conservative
fallback: a clipped glyph is compressed into the quad ImGui actually drew.
Unclipped glyphs are placed identically in both modes; the fallback exists as a
safety valve and as the second pass of the fine-clip fuzz harness
(`tests/test_fuzz.cpp`), which also proves the reconstruction's placement error
stays within a documented bound (≤ 1.75 px coverage gap, ≤ 0.02 px pen error
for unclipped runs; see the test output for the per-class bounds).

`Config::DebugDrawBounds` validates every reconstructed instance against the
drawn quad (`Statistics::BoundsMismatches`, debug assert); on the Vulkan
backend it also seeds the glyph-bounds debug overlay (§6).

**Sliver precision.** The UV handle encoding is affine, so ImGui's fine-clip
interpolation decodes to the cut-edge fraction; at sub-pixel slivers the decoded
fraction carries the encoding's float quantization (the ulp is ~0.002 fraction
units at slot ids of a few thousand). Sliver-scale fraction spans (< 0.1) are
therefore placed from the single-corner sample and the glyph's baked ppem
instead of the fitted affine map; a fully collapsed fraction span is accepted
while the quad still has extent on that axis, and a zero/inverted fine-clip
extent on an axis (or a collapse on both axes) drops the glyph. M4 fuzz at
100k cases: sliver pen error max 0.31 px, coverage gap max 1.26 px (bounds 1.0 /
1.75 px), no drops outside the degenerate-extent class.

## 4. Performance

Measured 2026-10-14, RTX 5080 (Vulkan 1.4.341), 1024×640 headless offscreen,
`scripts/bench.*`; host Ryzen 9 7950X3D, Release. Numbers are machine-specific
and are meant to be re-measured, not extrapolated.

### CPU pre-pass (`imslug_tests_bench --cpu`)

| scene | instances | pre-pass mean | notes |
|---|---|---|---|
| 5k glyphs (38×132 @17 px) | 4421 | **0.305 ms** | target ≤ 0.5 ms |
| 20k glyphs | 17684 | 1.260 ms | linear, ~63 ns/glyph |
| 3.6k Hanzi (CJK subset) | 3600 | 0.250 ms | heavier records |
| typical UI frame | 2144 | 0.152 ms | text interleaved with widgets |
| lazy glyph build | — | **6.8 µs/glyph** | target ≤ 50 µs/glyph |

Steady-state pool upload is 0–10 KB/frame once the font's glyphs are built
(272 KB first upload for Latin, 784 KB for the CJK subset).

### GPU (`imslug_tests_bench --gpu`, timestamp queries, same device/scene)

| scene | Slug | atlas (stock) | ratio |
|---|---|---|---|
| 38×132 @17 px (4421 glyphs) | 0.046 ms | 0.018 ms | 2.57× |
| 50×160 @12 px (7052 glyphs) | 0.056 ms | 0.025 ms | 2.25× |
| 20×60 @32 px (1059 glyphs) | 0.022 ms | 0.008 ms | 2.78× |
| typical UI frame (2144 glyphs + widgets) | 0.026 ms | 0.015 ms | 1.76× |

(Medians; the numbers move by ~5 % between runs on the same machine. The
harness prints the raw `--all` table into `<build dir>\bench\bench_report.md`.)

Slug's fragment shader is deliberately heavier per text pixel than sampling an
atlas (band header fetches plus ~6.1 reference walks per pixel, each with two
texel fetches, versus one bilinear texture fetch) — the measured Δ for pure
text is 2.3–2.8×, and in a production-shaped frame the text share bounds it to
**+0.011 ms** at 1024×640 (under 0.2 % of a 60 Hz frame). Treat the GPU
criterion as *frame-time parity with a bounded, text-share-proportional Δ*,
not per-pixel parity. In exchange, Slug fonts allocate no glyph bitmaps, upload
~0.3–0.8 MB once instead of atlas pages, and add no draw calls beyond one
callback + reset per run.

### Measuring performance

```
scripts\bench.bat --cpu          # CPU numbers, no Vulkan device required
scripts\bench.bat --gpu          # GPU numbers + device name (validation on)
scripts\bench.bat --all          # both, report in <build dir>\bench\bench_report.md
```

Manual cross-check (any GPU):

1. Close other GPU work. Run `--gpu` with validation (the default) and confirm
   the device line names the expected GPU and every Slug row reports
   `instances > 0` (a zero there means the row did not render Slug text and the
   harness warns).
2. Repeat with `--no-validation`; GPU times should match within noise (the
   layer slows CPU-side recording, not the measured fragment work).
3. For density scaling add `--lines 50 --columns 160 --size 12` and
   `--lines 20 --columns 60 --size 32`.
4. Quote mean/median with the device name, frame size and instance count;
   compare only against the atlas row of the same run. The R8 small-size table
   (`--cpu`) compares the estimator against the atlas rasterizer at 8–32 px.

Budgets in the harness are informational; CI runs the CPU mode.

## 5. Vertex compaction (decision)

Removing the consumed text vertices/indices from the draw buffers was evaluated
in M4 and **skipped**: the rewritten commands are callbacks with zero elements,
so the GPU never reads those vertices — compaction cannot reduce GPU time. It
would only shrink per-frame buffers that ImGui rebuilds from scratch every
frame (345 KB + 52 KB at 5k glyphs, ~24 MB/s of CPU writes at 60 fps) at the
cost of rewriting every remaining command's vertex/index offsets, including
third-party callbacks that captured them. Revisit only with evidence of a
vertex-bandwidth-bound workload.

## 6. Debug overlay

`ImSlug_ImplVulkan_SetDebugOverlay(flags)` (with `GetDebugOverlay()` for UI
state) renders diagnostic views on top of normal text; `Config::DebugDrawBounds`
seeds the glyph-bounds flag at `ImSlug_ImplVulkan_Init`:

| flag | view |
|---|---|
| `ImSlugVulkanDebugOverlay_GlyphBounds` | 1 px outline of every glyph's *drawn* quad (after fine clipping) — the same rectangle `DebugDrawBounds` validates against, so a generator bug shows as ink that does not fill its outline |
| `ImSlugVulkanDebugOverlay_BandGrid` | checkerboard of each glyph's H/V band grid |
| `ImSlugVulkanDebugOverlay_RefHeat` | heatmap of the band-reference walk cost per pixel (blue = 0, red = ≥ 16 refs fetched) |

Views combine; `RefHeat` takes precedence over `BandGrid`. The overlay is a
rendering aid only — it never changes geometry, placement, draw order or the
atlas fallback. The example app toggles them with F1/F2/F3 (and
`--overlay <none|bounds|grid|refs>` for screenshots); the ImGui golden test
covers the three views against the acceptance scene.

## 7. Small sizes (hinting policy)

Slug is unhinted by design, and so is ImGui's stb loader: neither snaps stems
to the pixel grid, and both anti-alias every edge. The M4 evaluation measured
the estimator against stbtt bitmaps at 8–32 px (table printed by `--cpu`):
at ≥ 16 px the peak (stem) coverage is exact and the mean difference is
comparable to larger sizes; below ~12 px stems lose up to 0.14 peak coverage.
A 4× supersample below 16 ppem (the osgSlug precedent) was evaluated and
**not** shipped: it improves the mean by only 5–10 % for 16× the fragment cost
in the densest-text regime, and it cannot converge to the atlas path anyway
(ray-coverage combination is not exact area coverage, and the atlas samples a
filtered integer-grid bitmap). Practical advice: pick sizes ≥ 13–16 px for body
text, prefer grid-aligned pen positions (the ImGui default; clearing
`ImDrawListFlags_TextNoPixelSnap` makes every path softer), and measure with
the R8 table for the specific font.

## 8. Known limitations

* Text costs more GPU time than the atlas path (§4) — by design; tune bands
  with `tests/bench/band_sweep.cpp` if a workload needs less.
* Multi-viewport secondary viewports are deferred (see the Vulkan backend
  skill): the adapter cannot discover a foreign render pass at callback time.
* ImGui master (1.93 WIP) reworked the font loader; the pinned v1.92.9b is the
  supported version (the daily smoke job tracks drift).
* OTF/CFF fonts are not parsed (TTF `glyf` only for now); unsupported fonts
  stay on the stock atlas path.
* Color fonts (§9) cover COLR v0/v1 with the documented paint fallbacks; a
  color glyph costs one instance per layer, so emoji-heavy frames are
  instance-hungry (the ring drops overflowing runs gracefully).
* Non-BMP codepoints (most emoji, U+1F600+) need `IMGUI_USE_WCHAR32` in the
  ImGui build; with the default 16-bit `ImWchar` they cannot be addressed at
  all (the emoji scene and ImGui-level tests therefore use BMP emoji).

## 9. Color fonts (COLR v0 / v1)

`imslug_colr.{h,cpp}` reads the `COLR`/`CPAL` tables from the same font bytes
stb_truetype uses (an SFNT directory scan plus a bounds-checked reader; every
malformed or unsupported case falls back to the monochrome outline — never a
crash). A color glyph is resolved once per font/glyph into a draw-order list of
layers (`ImSlugColorGlyph`): one Slug glyph record per layer (the glyph's own
record when the transform is a positive axis-aligned scale+translate, otherwise
a synthetic record built from the transformed outline) plus one paint record in
a new shared pool (`ImSlugPools::Paints`, GPU-visible as a storage buffer).

The pre-pass emits one instance per layer, all sharing the base glyph's
reconstructed pen/ppem; the fragment shader evaluates the layer's paint
(solid / linear / radial / sweep with pad/repeat/reflect and the COLR ClipBox)
and modulates the alpha by the instance tint, so `ImFontGlyph::Colored`'s
"ignore the tint RGB, keep the alpha" semantics are honored exactly (ImGui
forces RGB=255 for Colored glyphs; see the pinned `RenderText`/`RenderChar`).
The instance record grew by one `uint32_t` (`PaintIndex`, format v2, 96 bytes)
and the paint records are 352 bytes (`ImSlugPaintRecord`, std430); both layouts
are static_asserted on the C++ side and mirrored in `slug.frag`.

Semantics and fallbacks (PLAN §11 D20/D21):

* COLR v0 layer lists and COLR v1 paint graphs; `PaintVar*` reads the default
  (non-variation) values; variable-font instances are not evaluated.
* Gradients: linear (the spec's p2 rotation point), two-point conical radial
  (the WHATWG algorithm the spec references), sweep (counter-clockwise degrees,
  BiasedAngle); ill-formed gradients (degenerate/parallel points) are skipped
  per the spec. Color stops are F2DOT14 offsets with CPAL colors; color lines
  longer than 32 stops keep the first 32.
* Composites: SRC_OVER / SRC / DEST / DEST_OVER are draw-order exact;
  SRC_IN / DEST_IN / SRC_OUT / DEST_OUT with a constant `PaintSolid` mask fold
  the mask alpha into the layer paint (exact for single-layer sources; the
  two license-vetted test fonts' composites all take this path or draw-order);
  every other mode (SRC_ATOP/XOR and the separable/non-separable blends,
  including SOFT_LIGHT) is approximated as source-over and documented here.
* ClipBoxes (ClipList format 1 ranges) are applied per fragment in record
  space; clip edges are hard (no antialiasing at the clip boundary, which sits
  outside the ink for both test fonts).
* PaintColrGlyph cycles are refused (an in-flight glyph stack); graphs deeper
  than 32 paints / 512 layers per glyph are truncated to the drawable prefix.
* Empty base outlines (Noto Color Emoji's emoji glyphs have no `glyf`
  contours): the layout box is synthesized from the layer union with the same
  floor/ceil + rounded-ascent formulas as the metrics path, so text layout
  (advances, wrapping) is unchanged; only the ink box differs from the empty
  monochrome outline.

Verification: `imslug_colr` (synthetic COLR/CPAL tables on Roboto + the fetched
Noto font + the CPU paint sampler), `imslug_emoji_golden` (30 GPU-vs-CPU cases
at 16–64 px and 1×–3.5× zoom; measured worst mean 0.031 LSB, worst channel 21
LSB on isolated discontinuity ties, validation clean) and the ImGui-level
`imslug_imgui` color tests (Colored marking, per-layer instances, tint alpha,
layout parity).

## 10. Offline assets and deployment (M7)

Runtime font registration has two sources of glyph data, both feeding the same
`ImSlugFont` API and the same shared pools:

* **TTF at runtime** (`AddFontFromFileTTF`/`AddFontFromMemoryTTF`): outline
  extraction + band construction on first use (the default; zero deployment
  steps);
* **Packed `.imslug` assets** (auto-detected by content): glyph records, curve
  texels and band texels are copied from the baked container into the shared
  pools on first use — no outline parsing at all.

The offline generator (`imslug_gen`, `tools/imslug_gen/`) runs the *same* core
code as the runtime path (font handle, band construction, packer) and
serializes the result; `imslug_packed_roundtrip` proves the two are bitwise
identical, so the choice is purely a deployment trade-off. The container v1 is
versioned, deterministic and fully validated on load
([`PACKED_FORMAT.md`](PACKED_FORMAT.md)); `imslug_gen info` inspects and
validates an asset, `--emit-header` embeds one in a C build.

A build configured with `-DIMSLUG_TTF_PARSER=OFF` compiles the core (and the
whole app) **without stb_truetype**: only packed assets load, TTF sources fail
cleanly with a warning, and the example's scene 9 / the no-parser CI job
demonstrate rendering entirely from baked data. Typical uses: shipping a font
subset without the parser, reproducible builds, and startup-time savings
(packed loading is a copy, not an extraction).
