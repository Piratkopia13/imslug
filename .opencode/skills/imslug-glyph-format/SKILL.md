---
name: imslug glyph format
description: Curve/band data contract for the imslug glyph pipeline - texel layouts, epsilon/sorting rules, packer/texel budgets, and how to run the reference and packing tests. Load when editing the generator, packer, glyph records, or the CPU reference rasterizer (M1+).
---

# imslug glyph data format

Distilled from `.agent-docs/PLAN.md` §2.1-§2.2 and §4 (the plan is the
authoritative source; this skill is the working checklist).

**All coordinates are em space (1.0 = em), Y-up, per the reference contract.
Never mix in glyph-local pixel units (Sluggish uses those) and never assume
ImGui's Y-down orientation inside the pipeline - convert once at the boundary
and test it.**

## Texel layouts

### Curve texture (one global pool shared by all fonts)
* Format `RGBA16F`, fixed width 4096 texels; the shader row wrap is `log2 = 12`.
* One quadratic Bézier = **2 texels**: `(x1,y1,x2,y2)`, then `(x3,y3,0,0)`.
* Straight lines are encoded `{p1, p2, p2}` - duplicate the endpoint; never a
  degenerate quadratic.
* Connected curves may share the endpoint texel (optional optimization).
* Keep a curve's two texels on one row, or use the per-fetch row-wrap
  `CalcCurveLoc` like Sluggish/Slughorn.

### Band texture (one global pool)
* Format `R16G16_UINT` (`usampler2D` / `Texture2D<uint2>`).
* At glyph base `glyphLoc = (glyphLocX, glyphLocY)`:
  * `glyphLoc + 0 .. bandMaxY` - horizontal band headers `(curveCount, offset)`;
  * `glyphLoc + bandMaxY+1 .. bandMaxY+bandMaxX+1` - vertical band headers;
  * after the headers - curve reference lists `(curveTexX, curveTexY)`.
* Offsets are relative to `glyphLoc` and resolved with the row-wrap formula
  `CalcBandLoc(loc, off) = ((loc.x + off) & 4095, loc.y + ((loc.x + off) >> 12))`.
* Each band's reference list is sorted **descending by max x** (horizontal
  bands) / **descending by max y** (vertical bands) so the shader early-outs
  without missing hits.

### Glyph record (CPU/vertex data, not a texture)
`glyphLoc`, `bandMaxX`, `bandMaxY`, `bandScale = (bands / glyphWidth,
bands / glyphHeight)`, `bandOffset = (-minX*scaleX, -minY*scaleY)`, em-space
bbox, advance, later color-layer info.
Pixel lookup: `bandIndex = clamp(emCoord * bandScale + bandOffset, 0, bandMax)`.
The even-odd flag is bit 12 of the packed `bandMax` word in the reference pixel
shader (the bit 28 in the vertex-shader diagram is a known inconsistency).

## Generation rules that bite if wrong

1. Lines -> `{p1,p2,p2}` (above).
2. **Axis-parallel curves are excluded from the matching-axis bands**: horizontal
   lines never enter horizontal bands, vertical lines never vertical bands -
   rays parallel to them cannot cross them.
3. Band membership uses the **control-point bounding box** (conservative), with
   an epsilon such as `1/1024` em so adjacent bands overlap slightly.
4. Uniform band thickness per glyph; adaptive count
   `n = clamp(ceil(sqrt(2 * curveCount)), 4, 32)` per axis (tuned in M4,
   `sluggish`-style 16 and DearWidgets' adaptive counts are the comparison
   points).
5. Outline extraction: upstream `stb_truetype` (vendored under `third_party/stb/`,
   **not** ImGui's `imstb_truetype.h`). `stbtt_GetGlyphShape` -> lines/quadratics
   -> em space (`/ unitsPerEm`). Metrics mirror ImGui's stb loader so layout
   stays pixel-identical (`AdvanceX`, `X0..Y1` in baked pixel space).
6. Packer: 16-bit capacity guards, row-aligned band headers, growth by doubling,
   dirty-row tracking per frame; overflow -> assert + graceful fallback to
   ImGui's texture path for that glyph/font. Never write outside the pool.
7. Adaptive band counts and packing must be deterministic: same font in -> same
   bytes out, or committed/golden data churns.

## Budgets (typical Latin UI font, ~2000 glyphs)

* Curve pool 50-150 KB, band pool 50-400 KB; uploaded once, incremental after.
* One Slug draw call = 2 textures + 1 instance buffer, regardless of font count.
* CPU pre-pass target <= ~0.5 ms @ 5k glyphs; first-use glyph build <= ~50 us.
* Zero glyph bitmaps for Slug fonts (the whole point).

## Tests to run after generator edits

* `scripts/test.*` - unit tests: bbox/advance vs `stbtt`, superset check that
  every curve appears in every band it can cross, sort order per band,
  packing bounds/row-wrap, plane/contour winding signs.
* The **CPU reference rasterizer is the arbiter** when shader and generator
  disagree: render `g`, `A`, `W`, `é` at 16/32/64 px and compare against
  `stbtt` bitmaps within tolerance (M1 acceptance).
* After shader-affecting format changes: `scripts/gen_shaders.* --check` and the
  goldens (`scripts/golden.*`, M2+).
* Never "fix" a failing invariant by loosening the epsilon or the superset check
  without recording the decision in `.agent-docs/PLAN.md` §11.

## Gotchas

* Emit curves in a stable contour order; flipping winding signs silently breaks
  coverage.
* Em space and Y-up: the sign of `bandOffset.y` is a classic bug source.
* Keep texture width 4096 (`log2 = 12`) in sync with the shader constant; if the
  pool grows in rows, only the y coordinate changes.
