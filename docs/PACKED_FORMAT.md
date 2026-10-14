# The `.imslug` packed asset format (v1)

`.imslug` is imslug's offline font asset: a single file holding everything the
renderer needs for a set of glyphs — the ImGui-parity font metrics, the em-space
curve texels, the band acceleration texels and the per-glyph records — so a
runtime can render registered fonts **without parsing an outline** (and, with
`IMSLUG_TTF_PARSER=OFF`, without a TrueType parser at all).

This document is the durable specification of **container version 1**. The
machine-checked layout lives in `imslug_packed.h` (structs + `static_assert`s);
the reader/validator in `imslug_packed.cpp`; the writer in
`tools/imslug_gen/`; the format contract is pinned by `imslug_packed_loader`
(corruption/validation) and `imslug_packed_roundtrip` (bitwise equality with
runtime generation).

## 1. Where assets come from

```sh
imslug_gen font <font.ttf> --size-range 8:72 --chars 0x20-0x7E -o font.imslug
```

`imslug_gen` (built by `scripts/build.*` into `<build>/imslug_gen`) consumes the
**runtime core code**: it creates the same font handle, builds the same glyph
records, bands and pool texels as a runtime `AddFontFromFileTTF` would for the
same glyphs, and serializes them. The round-trip test proves the offline output
and the runtime generation are bitwise identical for the same glyph order
(`imslug_packed_roundtrip`), and the CLI-level freshness test
(`imslug_asset_fresh`) keeps the committed test asset honest.

`--chars` accepts comma/space separated items: a literal UTF-8 character, a
literal range (`a-z`), hex codepoints (`0x20`, `U+20`), hex ranges
(`0x20-0x7E`) and the keyword `all` (every codepoint the font's cmap maps).
Codepoints missing from the font are reported and skipped. `--emit-header
<file.h>` additionally writes a C header embedding the bytes (deterministic, no
timestamps) for apps that embed the asset; `--header-symbol <name>` overrides
the generated symbol base name. `imslug_gen embed <file> --emit-header <out.h>`
writes the same kind of header for any raw file (the example embeds its default
TTF this way). `imslug_gen info <asset>` validates an asset
and prints its identity/pool summary.

`--size-range` is recorded for diagnostics only: **glyph geometry is
size-independent** (em space), so one asset serves every bake size; metrics are
computed from the stored font metrics at whatever size the app bakes.

## 2. Container v1 layout

All integers are **little-endian** (the loader rejects big-endian hosts with a
clear error); all offsets are byte offsets from the file start; `align16(x) =
(x + 15) & ~15`. The writer emits exactly this canonical layout and the reader
requires it (regions in order, no gaps other than zeroed alignment padding, and
the total size must match):

| Region | Offset | Size |
|---|---|---|
| header | 0 | 112 bytes (`ImSlugPackedHeader`) |
| glyph table | `glyphTableOffset` = 112 | `GlyphCount * 92` bytes, sorted by `GlyphIndex` ascending |
| codepoint map | `codepointMapOffset` | `CodepointCount * 8` bytes, sorted by `Codepoint` ascending |
| source name | `sourceNameOffset` | `SourceNameLength + 1` bytes (UTF-8 + NUL) |
| curve blob | `curveBlobOffset` = align16(name end) | `CurveRows * 32768` bytes |
| band blob | `bandBlobOffset` = align16(curve end) | `BandRows * 16384` bytes |

### 2.1 Header (`ImSlugPackedHeader`, 112 bytes)

| Field | Meaning |
|---|---|
| `Magic[8]` | `"IMSLUG\0\0"` — the content sniff `ImSlug::IsPackedFontData` uses |
| `Version` | container version; `1` for this document |
| `HeaderSize` | `sizeof(ImSlugPackedHeader)` = 112 |
| `GeneratorVersion` | `IMSLUG_VERSION_NUM` of the generating library (diagnostics) |
| `Flags` | reserved, must be 0 in v1 |
| `UnitsPerEm` | source font `head.unitsPerEm` |
| `HheaAscent`, `HheaDescent` | source font hhea metrics (font units; used for every metric formula) |
| `SourceSize` | source font file size in bytes |
| `SourceChecksum` | FNV-1a 32-bit over the source font bytes |
| `GlyphCount`, `CodepointCount` | table sizes |
| `CurveRows`, `BandRows` | blob sizes in pool rows (each row is 4096 texels) |
| `GlyphTableOffset` … `BandBlobOffset` | region offsets (canonical values above) |
| `SourceNameOffset`, `SourceNameLength` | source file base name (diagnostics) |
| `SizeMinPxBits`, `SizeMaxPxBits` | `--size-range` as IEEE-754 f32 bit patterns (diagnostics) |
| `Reserved[5]` | must be 0 |

### 2.2 Glyph entry (`ImSlugPackedGlyphEntry`, 92 bytes)

| Field | Meaning |
|---|---|
| `GlyphIndex` | source font glyph id (> 0; unique, ascending in the table) |
| `AdvanceFu` | hmtx advance in font units |
| `BoxX0..BoxY1` | glyf box in font units (`stbtt_GetGlyphBox`; all zero when the glyph has no glyf data) |
| `Record` | the runtime `ImSlugGlyphRecord` (68 bytes), with `BandLoc*`/`CurveLoc*` addressing **this asset's blobs** |

The record's em-space fields (bbox, band scales/offsets, advance, flags, counts)
are exactly what the runtime builder produced; `Box*`/`AdvanceFu` let the loader
reproduce the ImGui-parity pixel metrics (`X0..Y1`, `AdvanceX`) at any bake size
without a parser, using the same formulas as the TTF path (see
`ImSlugImCeil/Floor/Round` in `imslug_font.h`).

### 2.3 Codepoint map (`ImSlugPackedCodepointEntry`, 8 bytes)

`{ Codepoint, GlyphEntry }` — maps a codepoint to a glyph table index. Several
codepoints may share one glyph entry (e.g. space and U+00A0 in Roboto); entries
are unique per glyph.

### 2.4 Pool blobs

The two blobs are **verbatim copies of the runtime pool rows**:

* curve blob: `RGBA16F` texels, 4096 per row, 8 bytes per texel; one quadratic
  Bézier = 2 texels `(x1,y1,x2,y2)` then `(x3,y3,0,0)`, em space, Y up;
* band blob: `R16G16_UINT` texels, 4096 per row, 4 bytes per texel; per glyph at
  `glyphLoc` (its record's `BandLoc*`): the horizontal+vertical band headers in
  the glyph's first row, then the reference lists (curve texel coordinates),
  with the row rules from the glyph-format skill (headers and lists never
  straddle a pool row).

The loader copies each glyph's data into the shared runtime pools on first use,
rebasing the record: the curve block is copied verbatim, the band block is
re-laid out by the runtime allocator, and every reference is patched by the
curve block's linear delta. Nothing about the data is size- or
platform-dependent.

## 3. Loading

The packed path is **auto-detected by content**, not by extension:

* `ImSlug::AddFontFromFileTTF` / `ImSlug::AddFontFromMemoryTTF` (C++),
* `imslug_add_font_from_file_ttf` / `imslug_add_font_from_memory_ttf` (C),
* and therefore any app that passes `.imslug` bytes to these entry points,

all accept a container. `ImSlug::IsPackedFontData(data, size)` /
`imslug_is_packed_font_data` sniff the magic; the loader copies the bytes, fully
validates the container and registers a font whose glyphs are served from the
asset. `sizePixels` is the bake size as usual. In a build with
`IMSLUG_TTF_PARSER=OFF` the packed path is the only font source; the TTF entry
points then fail cleanly (a warning goes to stderr, the font source is rolled
back by Dear ImGui).

**Validation is total and rejects corrupt input as a whole** — never a crash,
never a partially loaded asset, never silent mis-rendering. The validator
(`ImSlugValidatePackedMemory`, C: `imslug_validate_packed_font_data`, with an
optional human-readable reason string) checks the magic/version/header, the
canonical region layout and exact total size, the sorted tables, every glyph
record (pool ranges, band header/list row rules, references addressing the
glyph's own curve block), every referenced curve texel (finite, inside the
64-em envelope) and the source name. Tests truncate the asset at many lengths
and apply hundreds of deterministic random mutations; every invalid input is
rejected with a reason and creation returns `NULL`.

## 4. Determinism and versioning

The format is **deterministic**: for the same font bytes, codepoint set (order
independent), options and source name, the output is byte-identical — glyphs are
built in ascending glyph-index order, tables are sorted, alignment padding is
zeroed, and no timestamps or environment data are embedded. `imslug_gen` is
therefore suitable for reproducible builds; `imslug_asset_fresh` regenerates the
committed test asset through the CLI and byte-compares.

The container is **versioned from day one**: readers reject any `Version` other
than 1 with a clear message, and any semantic change (new fields, changed
meanings) bumps `Version`. Additive, backward-compatible reader features can
land without a bump only when v1 files stay valid.

**Scope of v1: monochrome glyphs.** A packed font has no COLR/CPAL payload, so
color glyphs fall back to their glyf outline exactly like a font without color
tables (for emoji fonts whose glyf is empty, that means they render nothing —
bake such fonts with the TTF path or wait for a future container version). The
committed test asset (`tests/assets/Roboto-ASCII.imslug`, Roboto ASCII
printable) is regenerated with `scripts/gen_asset.*`.

## 5. Related commands and tests

| What | Where |
|---|---|
| Generate an asset | `imslug_gen font …`; committed asset + example headers: `scripts/gen_asset.*` |
| Embed a raw file | `imslug_gen embed <file> --emit-header <out.h>` |
| Inspect an asset | `imslug_gen info <asset>` |
| Round-trip (bitwise) | ctest `imslug_packed_roundtrip` (needs the parser) |
| Loader/validation | ctest `imslug_packed_loader` (runs without the parser) |
| CLI + freshness | ctest `imslug_asset_fresh` |
| GPU rendering from the asset | ctest `imslug_golden_packed` / `…_dynamic` |
| No-parser build | configure with `-DIMSLUG_TTF_PARSER=OFF`; example scene 9 |
| Format structures | `imslug_packed.h`, `imslug_packed.cpp` |
