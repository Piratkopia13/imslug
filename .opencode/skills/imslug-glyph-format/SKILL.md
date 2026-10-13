---
name: imslug glyph format
description: Curve/band data contract for the imslug glyph pipeline - texel layouts, epsilon/sorting rules, packer caps, the frozen core API, the M3 glyph handle encoding/slab registry and how to run the reference/packing tests. Load when editing the generator, packer, glyph records, the CPU reference rasterizer or the handle encoder/decoder (M1+).
---

# imslug glyph data format

Implemented contract as of M1 (`.agent-docs/milestones/M1-core-glyph-pipeline.md`;
constants live in `imslug_internal.h`, the behavior in `imslug_{packer,glyphgen,refraster}.*`).
PLAN §2.1-§2.2/§4.1-§4.3 remain the design source; where this file and the plan
disagree on a detail, the code + this skill are the implemented truth (the M1
brief says the plan is authoritative "in all matters it specifies" — these are
the specifications it delegated).

**All coordinates are em space (1.0 = em), Y-up, origin at the pen position on
the baseline. Never mix in font units or pixels in stored geometry, and never
assume ImGui's Y-down orientation inside the pipeline - convert once at the
boundary (`1/unitsPerEm`, no Y flip: stbtt shapes are already Y-up) and test it
with 'g' (descender) in `tests/test_glyphgen.cpp`.**

## Constants (imslug_internal.h)

* `kPoolWidth = 4096`, `kPoolWidthLog2 = 12` (both pools, fixed width).
* `kPoolMaxHeight = 65536` (16-bit texel y).
* `kGlyphMaxBlockTexels = 65535` (band offsets are u16 relative to glyphLoc).
* `kMaxBandCountPerAxis = 32` (adaptive cap).
* `kBandOverlapEpsilon = 1/1024` em (band membership inflation).
* Solver epsilon `1/65536` (nearly-linear branch and coverage denominator).

## Curve pool (one global pool shared by all fonts)

* Format `RGBA16F`, width 4096; shader row wrap is `log2 = 12`. Stored as fp16
  texels on the CPU too (the reference rasterizer reads the quantized values).
* One quadratic Bézier = **2 texels**: `(x1,y1,x2,y2)`, then `(x3,y3,0,0)`.
* Straight lines are `{p1, p3, p3}` (duplicate endpoint, never degenerate);
  fully degenerate segments (all three points equal) are dropped at extraction.
* A glyph's curves are one **contiguous block** allocated in 2-texel units
  starting on an even x, so a curve's two texels always share a row (the
  reference shader reads `curveLoc` and `curveLoc + 1` with no wrap). The
  record's `CurveLocX/Y` + `CurveCount` address the block; the shader itself
  only uses band reference lists.
* Endpoint sharing between connected curves is **not** implemented (optional
  optimization, skipped in M1 by time-box).

## Band pool (one global pool)

* Format `R16G16_UINT` (`usampler2D` / `Texture2D<uint2>`).
* At glyph base `glyphLoc = (glyphLocX, glyphLocY)`, **all of these live in the
  glyph's first row** - the shader reads them contiguously in x with no wrap:
  * `glyphLoc.x + 0 .. bandMaxY` - horizontal band headers `(curveCount, offset)`;
  * `glyphLoc.x + bandMaxY+1 .. bandMaxY+bandMaxX+1` - vertical band headers
    (`glyphLoc.x + bandMax.y + 1 + bandIndex.x` in the shader);
  * the reference lists after them, `(curveTexX, curveTexY)` per entry.
* **Row rules (the shader never row-wraps a read):** the header run must fit in
  one row, and every band's reference list must fit in one row. The band
  allocator (`ImSlugAllocBandBlock`) pads to the next row wherever a list would
  straddle and returns one u16 `listOffset` per band; `CalcBandLoc` is only
  needed to resolve the list *start*:
  `CalcBandLoc(loc, off) = ((loc.x + off) & 4095, loc.y + ((loc.x + off) >> 12))`.
  A list longer than `kPoolWidth` can never satisfy the rule (padding only moves
  it to a row start), so the allocator **refuses** it and returns
  `ImSlugBuildStatus_GlyphTooLarge`; the caller must not retry it. The allocator
  returns `Ok` / `GlyphTooLarge` (unrepresentable: list > 4096, or padded total
  > `kGlyphMaxBlockTexels`) / `BandPoolLimit` (16-bit height or row growth), and
  leaves the pool cursor untouched on failure.
* Each band's reference list is sorted **descending by max x** (horizontal
  bands) / **max y** (vertical bands) - insertion sort, stable, by the max of
  the three control points, so the shader early-outs
  (`max(max(p.x, p.z), p3.x) * pixelsPerEm < -0.5`) without missing hits.

## Glyph record (ImSlugGlyphRecord, CPU/vertex data)

`BandLocX/Y`, `BandMaxX/Y`, `BandScale = (bandCount / em extent)` per axis,
`BandOffset = -emMin * bandScale`, `CurveLocX/Y` + `CurveCount`, control-point
em bbox `MinX..MaxY`, `Advance` (em), `Flags`.
Lookup: `bandIndex = clamp(int(emCoord * bandScale + bandOffset), 0, bandMax)`
(HLSL `int()` truncation + clamp; equivalent to floor because of the lower clamp).
The E (even-odd) flag is **bit 28 of the packed tex.w word** (bit 12 of its high
16-bit half) - `ImSlugPackBandMaxWord(bandMaxX, bandMaxY, flags)`; the pixel
shader masks `bandMax.y &= 0x00FF` and tests bit 12. The CPU record uses
`ImSlugGlyphFlags_EvenOdd` (bit 0) and the rasterizer maps it.

## Generation rules that bite if wrong

1. Lines -> `{p1,p3,p3}` (above); contour order preserved; stbtt already closes
   contours (`stbtt__close_shape`) - never add another closing segment.
2. **Axis-parallel curves are excluded from the matching-axis bands**: horizontal
   curves (all three control y bitwise equal after em conversion) never enter
   horizontal bands, vertical curves never vertical bands - rays parallel to
   them cannot cross them.
3. Band membership uses the **control-point bounding box** (conservative),
   inflated by `1/1024` em on both ends, inclusive overlap
   (`bbox.min - eps <= band.max && bbox.max + eps >= band.min`).
4. Uniform bands per glyph, adaptive count
   `n = clamp(ceil(sqrt(2 * curveCount)), 4, 32)` per axis - the single tuning
   point is `ImSlugCalcBandCount()` (M4 re-tunes). Empty glyphs (space) use one
   band per axis with `bandScale = 0`; a zero em extent on an axis also forces
   `bandScale = 0` for that axis (all curves there are axis-parallel anyway).
5. Outline extraction: upstream `stb_truetype` (vendored under
   `third_party/stb/`, pinned commit + SHA-256 in `PROVENANCE.txt`, **not**
   ImGui's `imstb_truetype.h`). `stbtt_GetGlyphShape` -> lines/quadratics ->
   em space (`/ unitsPerEm`). Cubics (never produced by glyf) go through
   `ImSlugConvertCubicToQuadratics`.
6. Packer: growth by doubling (32-row base), 16-bit/4096 capacity guards with
   asserts + returned `ImSlugBuildStatus` (`CurvePoolLimit`, `BandPoolLimit`,
   `GlyphTooLarge`, `OutOfMemory`) for the graceful fallback; dirty-row tracking
   (`DirtyRowMin/Max`) + `UsedRows` high-water mark for M2 uploads. Never write
   outside the pool. A **failed glyph build is transactional**: the curve/band
   cursors and dirty/used bookkeeping are rolled back, so non-cached failures
   (`CurvePoolLimit`, `BandPoolLimit`, `OutOfMemory`) can be retried on the next
   call without consuming pool space, and `Ok` always means a valid stable
   record pointer. `GlyphTooLarge` is classified from the allocator's real
   padded total (row padding included) and is definitive (cached; fall back to
   stock ImGui).
7. Deterministic: same font in -> same bytes out (tests compare two builds).
   The ref list order is stable insertion sort; curves are laid out in
   extraction order; band layout is a linear allocator.

## Metrics parity (ImGui v1.92.9b stb loader)

Mirrored in `imslug_glyphgen.cpp` with the config Slug fonts use:
`OversampleH/V = 1`, `RasterizerDensity = 1`, `ExtraSizeScale = 1`,
`GlyphOffset = 0`, no `PixelSnapH`, no advance clamping.

* `scale = stbtt_ScaleForPixelHeight(font, 1.0f) * size` = `size / (hhea.ascent - hhea.descent)`
  (hhea, not OS/2, and not unitsPerEm).
* `AdvanceX = advance_fu * scale`; `record.Advance = advance_fu / unitsPerEm`;
  `PixelsPerEm = scale * unitsPerEm`.
* `X0 = floor(gx0 * scale)`, `X1 = ceil(gx1 * scale)`, `Y0 = floor(-gy1 * scale) + IM_ROUND(Ascent)`,
  `Y1 = ceil(-gy0 * scale) + IM_ROUND(Ascent)` with `Ascent = ImCeil(hhea.ascent * scale)`
  (`IM_ROUND(x) = (int)(x + 0.5f)`, `ImCeil = ceilf`, `ImFloor` = exact floor).
* Empty bitmap box (space) -> `X0 = Y0 = X1 = Y1 = 0` (ImFontGlyph's zeroed
  defaults), `AdvanceX` still set. Missing codepoints: the metrics API returns
  false; ImGui's atlas-level fallback handles the substitution (same as the
  stock loader returning false per source).
* `ImSlugGlyphMetrics.MinX/MinY/MaxX/MaxY` carry the em-space control-point bbox
  (size-independent, exactly the glyph record's values — both go through the
  same extraction and bounds helper); space has all four zero. The metrics query
  returns false if its extraction scratch cannot be allocated.
* `tests/test_imguiparity.cpp` compiles Dear ImGui into a test-only target and
  asserts **exact equality** for every glyph/size pair; `tests/test_metrics.cpp`
  re-derives the formulas from stbtt independently and pins the bbox against the
  record.

## CPU reference rasterizer (the arbiter)

`imslug_refraster.cpp` mirrors `SlugPixelShader.hlsl` function by function
(CalcRootCode table `0x2E74 >> shift & 0x0101`, SolveHoriz/VertPoly with the
`1/65536` nearly-linear branch, CalcBandLoc, CalcCoverage
`max(|xcov*xwgt + ycov*ywgt| / max(xwgt + ywgt, eps), min(|xcov|, |ycov|))` then
saturate (nonzero) or even-odd wrap). Deviations: the exactly-degenerate linear
solve uses the guard expression adapted from Slughorn (MIT, license in NOTICE
§4; no other Slughorn code is used — everything else is a port of the reference
shader); `frac()` is IEEE `x - floor(x)`.
`fwidth` is CPU-impossible, so `pixelsPerEm` is a parameter. Banded and
unbanded (`ImSlugRasterSampleUnbanded`) must agree; banding is only an
acceleration structure. `ImSlugRasterGlyphRect` fills `width*height` floats,
row 0 = top row, pixel centers, `(centerX0, centerY0)` = em coord of pixel
(0,0)'s center, emY decreasing by `1/pixelsPerEm` per row (pinned by
`tests/test_refraster.cpp`).

## Tests to run after generator edits

* `scripts/test.*` - suites: `imslug_glyphgen` (extraction, contours, line
  encoding, cubic bound, band superset/sort/axis-parallel, determinism,
  failed-build rollback/retry), `imslug_packer` (fp16, doubling, pair rows, band
  row padding, list row rule, padded-total classification, guards, dirty rows),
  `imslug_metrics` (stbtt re-derivation, record/metrics bbox parity),
  `imslug_refraster` (math pins + the stbtt coverage gate + banded/unbanded +
  convergence + `ImSlugRasterGlyphRect` convention),
  `imslug_metrics_imgui_parity` (test-only ImGui build, exact equality).
* Coverage gate (`g`, `A`, `W`, `é` at 16/32/64 px, both test fonts): deep
  pixels (3x3 stbtt neighbourhood saturated) must match within 0.05; aggregate
  tripwires are per-size (mean 9%/5%/3%, p99 27%/32%/27% for 16/32/64 px) plus a
  256 px convergence check (mean <= 1.2% and <= 1/4 of the 16 px mean). The
  reference estimator is **not** exact area coverage: mean deviations 1.5-8.3%
  at 16 px come from the sampler's ±0.5 px ramp on edge pixels, not geometry
  (deep pixels are exact and the mean collapses ~40x by 256 px). Change these
  numbers only with measurements + a PLAN §11 entry.
* After shader-affecting format changes: `scripts/gen_shaders.* --check` and the
  goldens (`scripts/golden.*`, M2+).
* Never "fix" a failing invariant by loosening the epsilon or the superset check
  without recording the decision in `.agent-docs/PLAN.md` §11.

## Frozen core API for M2 (imslug_internal.h + modules)

```c
ImSlugFont* ImSlugCreateFontFromMemory(const void* data, int size, int fontIndex, bool copyData);
void        ImSlugDestroyFont(ImSlugFont*);
void        ImSlugGetFontMetrics(const ImSlugFont*, float bakeSizePixels, ImSlugFontMetrics*);
bool        ImSlugGetGlyphMetrics(const ImSlugFont*, unsigned int codepoint, float bakeSizePixels, ImSlugGlyphMetrics*);
int         ImSlugGetGlyphIndex(const ImSlugFont*, unsigned int codepoint);
ImSlugBuildStatus ImSlugGetGlyphRecord(ImSlugFont*, ImSlugPools*, unsigned int codepoint,
                                       const ImSlugGlyphRecord** outRecord);   // lazy + cached
ImSlugBuildStatus ImSlugGetGlyphRecordByIndex(ImSlugFont*, ImSlugPools*, int glyphIndex,
                                              const ImSlugGlyphRecord** outRecord);

void ImSlugInitPools(ImSlugPools*);              // one pool set per atlas, shared by all fonts
void ImSlugDestroyPools(ImSlugPools*);
void ImSlugCurvePoolClearDirtyRows(ImSlugCurvePool*); // after a partial upload consumed one pool
void ImSlugBandPoolClearDirtyRows(ImSlugBandPool*);
void ImSlugPoolClearDirtyRows(ImSlugPools*);     // both pools at once
uint32_t ImSlugPackBandMaxWord(int bandMaxX, int bandMaxY, uint32_t glyphFlags); // tex.w

float ImSlugRasterSample(const ImSlugPools*, const ImSlugGlyphRecord*, float emX, float emY,
                         float pixelsPerEmX, float pixelsPerEmY);                // ground truth
void  ImSlugRasterGlyphRect(const ImSlugPools*, const ImSlugGlyphRecord*, float centerX0, float centerY0,
                            float pixelsPerEm, float* outCoverage, int width, int height);
```

Pools expose `Curves.Texels` (RGBA16F, `4*u16` per texel), `Bands.Texels`
(R16G16_UINT, `2*u16`), `Width`, `Height`, `UsedRows`, `DirtyRowMin/Max` for
the M2 uploads. Records point into the font's cache and stay valid until the
font is destroyed (they are rebuilt if a different `ImSlugPools` is passed).
The pipeline is single-threaded; build scratch buffers live inside `ImSlugFont`.

## M3: glyph handle encoding and slabs (the UV payload)

One baked glyph (per font source, glyph index and *bake size*) owns a **slot**;
the slot indexes `ImSlugGlyphSlab` (core registry, `imslug.cpp`):

```c
struct ImSlugGlyphSlab {
    const ImSlugGlyphRecord* Record;  // outlines/bands, size-independent
    const void* Bake;                 // ImFontBaked* lifetime key (frees the slab)
    float X0, Y0, X1, Y1;             // baked pixel box == ImFontGlyph box (Y down,
                                      // line-top relative: Y0..Y1 include the ascent)
    float Ascent;                     // ImFontBaked::Ascent (line top -> baseline)
    float PixelsPerEm;                // baked pixels per em
    uint32_t Slot; bool Alive;
};
```

The ImGui adapter (`imslug_imgui_adapter.cpp`) bakes the `ImFontGlyph` UVs as

```
U = 1.5 + 2*slot + 0.25 + 0.5*fracX      fracX = (cornerX - X0) / (X1 - X0)
V = 2.5 + 2*slot + 0.25 + 0.5*fracY      fracY = (cornerY - Y0) / (Y1 - Y0)
```

* both axes carry the slot (an axis swap or a mixed pair cannot decode);
  frac 0 is the box's left/top corner, frac 1 the right/bottom one;
* the magic values are clear of atlas UVs (which live in `[0,1]`) and the
  0.25..0.75 inner band leaves room for float rounding of fine-clipped values;
* the encoding is **affine**, so ImGui's fine-clip interpolation of a clipped
  corner stays inside the slot cell and decodes to the exact fraction of the
  cut edge - no per-frame glyph map, and atlas repacking/growth cannot
  invalidate a handle (the decode never touches the atlas);
* slot ids are stable for the process lifetime and released when their
  `ImFontBaked` is discarded (`ImSlugFreeGlyphSlabsForBake`), capped by
  `Config::MaxGlyphSlabs` (default 65536).

The adapter decodes a vertex with `ImSlugAdapterDecodeVertex(u, v, &decoded)`
(slot + both fractions, or "not Slug geometry"). The pre-pass rebuilds the
placement from the four corner fractions with a fitted affine map per axis, then
evaluates it at the record's em bbox corners; `Ascent` bridges ImGui's
line-top Y and em's baseline Y. The sanity slack is ~1 px because `X0..Y1` are
the *integer* (floor/ceil) box around the exact control-point bbox.

## Budgets (typical Latin UI font, ~2000 glyphs)

* Curve pool 50-150 KB, band pool 50-400 KB; uploaded once, incremental after.
* One Slug draw call = 2 textures + 1 instance buffer, regardless of font count.
* CPU pre-pass target <= ~0.5 ms @ 5k glyphs; first-use glyph build <= ~50 us.
* Zero glyph bitmaps for Slug fonts (the whole point).

## Gotchas

* Emit curves in a stable contour order; flipping winding signs silently breaks
  coverage.
* Em space and Y-up: the sign of `bandOffset.y` is a classic bug source.
* Keep texture width 4096 (`log2 = 12`) in sync with the shader constant; if the
  pool grows in rows, only the y coordinate changes.
* The reference shader reads band headers and ref lists **contiguously in x**;
  only the list *start* goes through `CalcBandLoc`. A naive linear allocator
  that lets a list cross a row boundary produces garbage coverage at high band
  indexes (caught by `imslug_packer`).
* `int2(x)` in HLSL truncates toward zero; the CPU rasterizer must use `(int)`,
  not `floorf`, before the clamp.
