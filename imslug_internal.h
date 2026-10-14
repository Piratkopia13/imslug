// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_internal.h — implementation-only shared data contract for the glyph
// pipeline (M1): TTF bytes -> em-space quadratics -> curve/band pools -> CPU
// reference rasterizer. This header holds the shared *types and constants*;
// the APIs live in the module headers included at the bottom of this file:
//
//   imslug_packer.h     shared curve/band pools, allocation, dirty rows
//   imslug_glyphgen.h   outline extraction, band construction, font records
//   imslug_refraster.h  CPU coverage rasterizer (arbiter for M2+)
//
// M3+ code includes this one header. See the M1 completion notes in
// .agent-docs/milestones/M1-core-glyph-pipeline.md and the skill
// .opencode/skills/imslug-glyph-format.
//
// Rules for this header (AGENTS.md invariant 4, M1 brief):
//   * never include imgui.h;
//   * no STL containers/strings — plain buffers and explicit alloc/free;
//   * no exceptions: fallible entry points return bool / ImSlugBuildStatus.
// The implementation is C++17.
//
// Data format (contract: PLAN §2.1-2.2; mirrored by the reference shaders in M2):
//   * Em space everywhere: 1.0 = em, Y up, origin at the pen position on the
//     baseline. Never mix font units or pixels into stored geometry.
//   * Curve pool: RGBA16F, fixed width 4096. One quadratic = 2 texels:
//     (x1,y1,x2,y2) then (x3,y3,0,0). Lines are {p1,p3,p3} (never degenerate).
//   * Band pool: R16G16_UINT, fixed width 4096. Per glyph, at glyphLoc:
//       row glyphLoc.y, x = glyphLoc.x + 0 .. bandMaxY  — horizontal band
//       headers (curveCount, offset);
//       row glyphLoc.y, x = glyphLoc.x + bandMaxY+1 .. — vertical band headers;
//       then curve reference lists (curveTexelX, curveTexelY), addressed with
//       CalcBandLoc(glyphLoc, offset).
//     Both header runs live in the glyph's first row, exactly as the reference
//     pixel shader reads them (SlugPixelShader.hlsl: glyphLoc.x + bandIndex.y
//     and glyphLoc.x + bandMax.y + 1 + bandIndex.x, row glyphLoc.y).
//   * The E (even-odd) flag is bit 28 of the packed bandMax word (tex.w), i.e.
//     bit 12 of its high 16-bit half — see ImSlugPackBandMaxWord from the
//     reference vertex/pixel shader comments (PLAN §2.3).
//
// References (attribution: NOTICE section 2/4):
//   * Slug reference shaders, Copyright 2017 Eric Lengyel (MIT OR Apache-2.0):
//     https://github.com/EricLengyel/Slug — SlugPixelShader.hlsl blob
//     170a13f0fe3ad7562877eb89c4afcd36b89bdbce is the math and addressing
//     contract mirrored by imslug_refraster.cpp.
//   * Slughorn (MIT): https://github.com/AlphaPixel/slughorn — render.hpp blob
//     1b87727ce5f68b8b3c2c383ce0991fec9652b3ad was the CPU cross-check for the
//     same math. The *only* code adapted from Slughorn is the degenerate-linear
//     guard expression in the root solvers (render.hpp _solveHorizPoly /
//     _solveVertPoly: divide by b only when |b| >= 1/65536, else t = 0), used
//     where the reference HLSL would divide by zero; its MIT license text is
//     quoted in NOTICE section 4. No other Slughorn code is copied.
//   * Dear ImGui v1.92.9b (imgui_draw.cpp) — the metrics formulas mirrored by
//     imslug_glyphgen.cpp (comments cite the exact lines).

#pragma once

// stb_truetype.h is vendored unmodified under third_party/stb/ (see its
// PROVENANCE.txt). The implementation is compiled in imslug_stb.cpp, in a
// separate target with third-party warning settings. The no-parser build
// (IMSLUG_NO_STBTT, M7) excludes both the header and the implementation: the
// core then only loads packed `.imslug` assets.
#if !defined(IMSLUG_NO_STBTT)
#include "stb_truetype.h"
#endif

#include <stddef.h>
#include <stdint.h>

namespace ImSlug
{
    // Public API types used by pointer in this header (definitions in imslug.h).
    struct Config;

// ---------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------
// ImGui-style assert: active unless NDEBUG. Runtime capacity problems must
// still be reported through return values — never rely on asserts for
// fallback behavior (PLAN R11).
#if !defined(IMSLUG_ASSERT)
#include <assert.h>
#if defined(NDEBUG)
#define IMSLUG_ASSERT(_EXPR) ((void)0)
#else
#define IMSLUG_ASSERT(_EXPR) assert(_EXPR)
#endif
#endif

    // ---------------------------------------------------------------------
    // Format constants (must stay in sync with the future shaders)
    // ---------------------------------------------------------------------
    static const int kPoolWidth = 4096;  // texels per row, both pools
    static const int kPoolWidthLog2 = 12;
    static const int kPoolMaxHeight = 65536;                  // 16-bit texel y coordinate
    static const int kMaxBandCountPerAxis = 32;               // adaptive cap (M4 re-tunes)
    static const int kGlyphMaxBlockTexels = 65535;            // u16 band offsets
    static const float kBandOverlapEpsilon = 1.0f / 1024.0f;  // em (PLAN §2.2)

    // ---------------------------------------------------------------------
    // Pools (one curve pool + one band pool shared by every Slug font)
    // ---------------------------------------------------------------------
    struct ImSlugCurvePool
    {
        uint16_t* Texels;  // Width * Height texels, 4 halves each (RGBA16F)
        int Width;         // kPoolWidth
        int Height;        // allocated rows (grows by doubling)
        int CursorTexels;  // linear allocation cursor (always 2-texel aligned)
        int UsedRows;      // rows written at least once (for the first upload)
        int DirtyRowMin;   // inclusive dirty range since the last clear;
        int DirtyRowMax;   // empty when DirtyRowMin > DirtyRowMax
    };

    struct ImSlugBandPool
    {
        uint16_t* Texels;  // Width * Height texels, 2 halves each (R16G16_UINT)
        int Width;
        int Height;
        int CursorTexels;
        int UsedRows;
        int DirtyRowMin;
        int DirtyRowMax;
    };

    // ---------------------------------------------------------------------
    // M5: paint pool (COLR color layers)
    // ---------------------------------------------------------------------
    // One ImSlugPaintRecord per *drawn layer* of a color glyph (flat v0
    // layers, v1 fills/gradients). Records are built once when the color
    // glyph is first used and live for the process (like glyph records);
    // growth by doubling, dirty-range tracking for incremental uploads. The
    // GPU reads the array as an std430 storage buffer (shaders/vulkan/slug.frag),
    // so the layout below is a hard contract: 16-byte aligned, offsets
    // static_asserted and mirrored by the GLSL struct.
    static const int kMaxColorStops = 32;         // per color line (Noto: <= 13, Segoe: <= 26)
    static const int kMaxColorLayers = 512;       // per color glyph; beyond -> monochrome fallback
    static const int kMaxColorGraphDepth = 32;    // paint nesting cap (fallback: truncate)
    static const int kMaxColorGraphNodes = 4096;  // visited paints per glyph (fallback: truncate)

    enum ImSlugColorPaintType
    {
        ImSlugColorPaintType_Solid = 0,
        ImSlugColorPaintType_LinearGradient = 1,
        ImSlugColorPaintType_RadialGradient = 2,
        ImSlugColorPaintType_SweepGradient = 3,
    };

    enum ImSlugColorExtend
    {
        ImSlugColorExtend_Pad = 0,      // clamp to the stop offsets
        ImSlugColorExtend_Repeat = 1,   // t = fract(t)
        ImSlugColorExtend_Reflect = 2,  // triangle wave
    };

    struct ImSlugPaintRecord
    {
        // Header: (type, extend, stopCount, solidColor|clipDisabled?)
        uint32_t Type;        // ImSlugColorPaintType
        uint32_t Extend;      // ImSlugColorExtend (gradients)
        uint32_t StopCount;   // 1..kMaxColorStops
        uint32_t SolidColor;  // RGBA8, r | g<<8 | b<<16 | a<<24 (type 0)

        // base-em -> paint-local affine (gradients): local = M * base.
        float Xform0[4];  // a, b, tx, unused
        float Xform1[4];  // c, d, ty, unused

        // Gradient geometry in paint-local em. Linear: (p0.xy, p1.xy) then
        // (p2.xy, 0, 0). Radial: (c0.xy, r0) then (c1.xy, r1). Sweep:
        // (center.xy, startDeg, endDeg).
        float Geo0[4];
        float Geo1[4];

        // Base-em clip rectangle (the COLR ClipBox, mapped into base space);
        // disabled when ClipMaxX <= ClipMinX.
        float Clip[4];  // minX, minY, maxX, maxY

        // Color stops, packed two u32 per stop: [2i] = the raw F2DOT14 offset
        // (sign-extended from its low 16 bits), [2i+1] = the color as RGBA8
        // (r | g<<8 | b<<16 | a<<24). Offsets are ascending. One u32 per stop
        // would overlap the offset with the color's alpha byte.
        uint32_t Stops[2 * kMaxColorStops];
    };
    static_assert(sizeof(ImSlugPaintRecord) == 352, "GPU-visible paint layout (slug.frag std430)");
    static_assert(offsetof(ImSlugPaintRecord, Xform0) == 16, "GPU-visible paint layout");
    static_assert(offsetof(ImSlugPaintRecord, Geo0) == 48, "GPU-visible paint layout");
    static_assert(offsetof(ImSlugPaintRecord, Clip) == 80, "GPU-visible paint layout");
    static_assert(offsetof(ImSlugPaintRecord, Stops) == 96, "GPU-visible paint layout");

    struct ImSlugPaintPool
    {
        uint8_t* Bytes;      // Count records of ImSlugPaintRecord
        int Capacity;        // records (grows by doubling)
        int Count;           // records in use
        int DirtyRecordMin;  // inclusive; empty when Min > Max
        int DirtyRecordMax;
    };

    struct ImSlugPools
    {
        ImSlugCurvePool Curves;
        ImSlugBandPool Bands;
        // M5: per-layer paint records for COLR color glyphs (one record per
        // drawn layer: solid fills and gradient definitions). GPU-visible as a
        // storage buffer (ImSlugPaintRecord); the CPU mirror is what tests and
        // the offscreen golden's expected image read through
        // ImSlugSamplePaintRecord. Byte-granular like the pool texels: an
        // append-only record array with dirty-range tracking.
        ImSlugPaintPool Paints;
    };

    // ---------------------------------------------------------------------
    // Glyph record (CPU/vertex data, never a texture)
    // ---------------------------------------------------------------------
    enum ImSlugGlyphFlags
    {
        ImSlugGlyphFlags_None = 0,
        ImSlugGlyphFlags_EvenOdd = 1 << 0,  // E flag; packed to bit 28 of tex.w
    };

    struct ImSlugGlyphRecord
    {
        int32_t BandLocX, BandLocY;      // glyphLoc in the band pool
        int32_t BandMaxX, BandMaxY;      // band count - 1 per axis
        float BandScaleX, BandScaleY;    // bandCount / em extent (bands per em)
        float BandOffsetX, BandOffsetY;  // -emMin * bandScale
        int32_t CurveLocX, CurveLocY;    // first curve texel of the contiguous block
        float MinX, MinY, MaxX, MaxY;    // em-space control-point bbox (Y up)
        float Advance;                   // em-space advance (advance_fu / unitsPerEm)
        uint32_t CurveCount;
        uint32_t Flags;  // ImSlugGlyphFlags
    };

    // ---------------------------------------------------------------------
    // Outline data (em space, Y up)
    // ---------------------------------------------------------------------
    struct ImSlugQuad
    {
        float X1, Y1;  // start point
        float X2, Y2;  // control point
        float X3, Y3;  // end point
    };

    struct ImSlugCubic
    {
        float X1, Y1;
        float X2, Y2;
        float X3, Y3;
        float X4, Y4;
    };

    struct ImSlugQuadBounds
    {
        float MinX, MinY, MaxX, MaxY;  // control-point bbox (of the curve)
    };

    // ---------------------------------------------------------------------
    // M5: color glyphs (COLR/CPAL)
    // ---------------------------------------------------------------------
    // A color glyph is a flattened, draw-order list of layers. Each layer is
    // one Slug glyph record plus a paint record in the pool. Layer `i` is
    // drawn in list order (painter's algorithm), so COLR's src-over compositing
    // is exactly the pipeline's straight-alpha blend; composites whose mode is
    // not representable that way are approximated or folded into the layer
    // alpha (see imslug_colr.cpp for the documented per-mode behavior).
    //
    // The record may be the plain glyph's own record (identity or positive
    // axis-aligned scale+translate layers reuse it) or a synthetic record
    // built from the transformed outline. ScaleX/Y/Offset*Em map record space
    // to the base glyph's em space:
    //   base = (OffsetXEm + recordX * ScaleX, OffsetYEm + recordY * ScaleY)
    // and the paint record's transform/clip are already expressed relative to
    // the *record* space, so the shader needs no extra mapping.
    struct ImSlugColorLayer
    {
        const ImSlugGlyphRecord* Record;
        float ScaleX, ScaleY;        // > 0 (baked records: 1)
        float OffsetXEm, OffsetYEm;  // (baked records: 0)
        uint32_t PaintIndex;         // index into ImSlugPools::Paints
    };

    struct ImSlugColorGlyph
    {
        int GlyphIndex;
        int LayerCount;
        const ImSlugColorLayer* Layers;  // draw order
        float MinX, MinY, MaxX, MaxY;    // union of the layers' record bboxes in base em
        // Bbox-only record covering every layer (and the base glyph's glyf box
        // when one exists). Color glyph slabs use it for fine-clip
        // reconstruction instead of the base glyph record: emoji base glyphs
        // are often empty (Noto Color Emoji), so the glyf record's bbox would
        // not cover the quad ImGui drew. CurveCount/BandMax* are zero; it is
        // never drawn.
        ImSlugGlyphRecord BaseRecord;
    };

    // ---------------------------------------------------------------------
    // Glyph instance (frozen M2 layout; mirrored by shaders/vulkan/slug.vert)
    // ---------------------------------------------------------------------
    // One instance per glyph quad. The vertex shader builds the quad corners
    // from gl_VertexIndex (4 vertices, triangle strip) and reads this record
    // through VK_VERTEX_INPUT_RATE_INSTANCE attributes at locations 0..7:
    //
    //   loc 0  vec4   EmBBox      (MinX, MinY, MaxX, MaxY) em space, Y up
    //   loc 1  vec4   ScreenRect  (MinX, MinY, MaxX, MaxY) screen pixels, Y down
    //   loc 2  vec4   Jac         inverse Jacobian rows: em += (dot(d, .xy), dot(d, .zw))
    //   loc 3  vec4   Banding     (BandScaleX, BandScaleY, BandOffsetX, BandOffsetY)
    //   loc 4  uvec2  GlyphLoc    band pool glyph location (x, y)
    //   loc 5  uint   BandMaxWord bandMaxX | bandMaxY << 16 | E << 28 (D8(g))
    //   loc 6  vec4   Color       straight alpha (r, g, b, a)
    //   loc 7  uint   PaintIndex  M5: ImSlugPools::Paints index, or
    //                             kNoPaintIndex (0xFFFFFFFF) for plain glyphs
    //
    // ScreenRect corners correspond to EmBBox corners with the Y flip applied
    // (em Y up, screen Y down): em (MinX, MinY) maps to (ScreenMinX,
    // ScreenMaxY). The placement must be an axis-aligned scale+translate (no
    // rotation): the shader dilates along the screen-space corner diagonals.
    struct ImSlugGlyphInstance
    {
        float EmMinX, EmMinY, EmMaxX, EmMaxY;
        float ScreenMinX, ScreenMinY, ScreenMaxX, ScreenMaxY;
        float Jac00, Jac01, Jac10, Jac11;
        float BandScaleX, BandScaleY, BandOffsetX, BandOffsetY;
        uint32_t GlyphLocX, GlyphLocY;
        uint32_t BandMaxWord;
        float ColorR, ColorG, ColorB, ColorA;
        uint32_t PaintIndex;  // M5 (instance format v2); see kNoPaintIndex
    };
    static_assert(sizeof(ImSlugGlyphInstance) == 96, "frozen M5 instance layout (shaders + vertex input)");
    static_assert(offsetof(ImSlugGlyphInstance, ScreenMinX) == 16, "frozen instance layout");
    static_assert(offsetof(ImSlugGlyphInstance, Jac00) == 32, "frozen instance layout");
    static_assert(offsetof(ImSlugGlyphInstance, BandScaleX) == 48, "frozen instance layout");
    static_assert(offsetof(ImSlugGlyphInstance, GlyphLocX) == 64, "frozen instance layout");
    static_assert(offsetof(ImSlugGlyphInstance, BandMaxWord) == 72, "frozen instance layout");
    static_assert(offsetof(ImSlugGlyphInstance, ColorR) == 76, "frozen instance layout");
    static_assert(offsetof(ImSlugGlyphInstance, PaintIndex) == 92, "frozen M5 instance layout");
    static const uint32_t kNoPaintIndex = 0xFFFFFFFFu;  // instance.PaintIndex sentinel

    // Fills one instance for `record` placed by an axis-aligned transform:
    //   screen (pixels, Y down) = (penX + emX * pixelsPerEm, penY - emY * pixelsPerEm)
    // where (penX, penY) is the pen position on the baseline. Color is straight
    // alpha; PaintIndex is set to kNoPaintIndex (plain glyph). Empty glyphs
    // (CurveCount == 0) are representable (coverage 0), but callers normally
    // skip them.
    void ImSlugBuildGlyphInstance(const ImSlugGlyphRecord* record, float penX, float penY, float pixelsPerEm, float r,
                                  float g, float b, float a, ImSlugGlyphInstance* outInstance);

    // M5 generalization of the above with per-axis scale (used by color layers
    // whose transform is a positive axis-aligned scale):
    //   screen = (penX + emX * pixelsPerEmX, penY - emY * pixelsPerEmY)
    // The vertex shader keeps this mapping affine through Jac, so the
    // fragment's sample coordinate is still the em coordinate.
    void ImSlugBuildGlyphInstanceScaled(const ImSlugGlyphRecord* record, float penX, float penY, float pixelsPerEmX,
                                        float pixelsPerEmY, float r, float g, float b, float a,
                                        ImSlugGlyphInstance* outInstance);

    // ---------------------------------------------------------------------
    // Glyph handle (M3): the ImDrawVert UV payload that identifies a baked
    // glyph in the draw data.
    // ---------------------------------------------------------------------
    // One baked glyph (per font source, glyph index and bake size) owns a slot;
    // the slot maps to an ImSlugGlyphSlab carrying the size-independent record
    // plus the baked pixel metrics the UV fractions were encoded against. The
    // pre-pass decodes every vertex with ImSlugAdapterDecodeVertex (the
    // ImGui-facing adapter owns the encoding) and rebuilds the placement from
    // the four corner fractions, so no per-frame handle map is needed and atlas
    // repacking/growth cannot invalidate a handle.
    //
    // Encoding (adapter, imslug_imgui_adapter.cpp):
    //   U = kUvMagicU + 2*slot + kUvFracBase + fracX*kUvFracScale
    //   V = kUvMagicV + 2*slot + kUvFracBase + fracY*kUvFracScale
    // where fracX/fracY are the corner positions inside the baked glyph's pixel
    // box [X0..X1]x[Y0..Y1] (fracY grows downward, like ImGui's V0/V1). The
    // whole payload is affine, so ImGui's fine-clip linear interpolation of
    // clipped corners lands inside the same slot cell and decodes to the exact
    // fraction of the clip edge. Anything outside the cell (or out of the magic
    // ranges) is not Slug geometry.
    static const float kUvMagicU = 1.5f;  // clear of atlas UVs ([0,1]) and of V
    static const float kUvMagicV = 2.5f;
    static const float kUvCellSpan = 2.0f;   // slot pitch; one slot owns [0,1] frac
    static const float kUvFracBase = 0.25f;  // frac offset inside the cell
    static const float kUvFracScale = 0.5f;  // frac -> cell scale (0.25 .. 0.75)
    // Accepted decode overhang, in frac units, on top of the encoded 0..1
    // range. Narrow (not the full cell): genuine fine-clip values stay well
    // inside the encoded band even at the largest slot id, where a float ulp
    // is 1/64 of a frac unit (slot 65535: |error| <= ~0.02); 0.5 would accept
    // half the whole slot cell, i.e. a much wider false-positive window for
    // arbitrary geometry UVs that happen to land in the magic ranges.
    static const float kUvFracTolerance = 0.1f;
    static const int kMaxGlyphSlabs = 65536;  // 2*slot + frac stays float-exact

    struct ImSlugGlyphSlab
    {
        const ImSlugGlyphRecord* Record;  // may be null while unused/being freed
        // M5: the resolved color glyph (layers + paint records) when this baked
        // glyph is a COLR color glyph and Config::EnableColorFonts was set when
        // it was loaded; null for monochrome glyphs. The pre-pass emits one
        // instance per layer instead of one for Record. Owned by the font.
        const ImSlugColorGlyph* ColorGlyph;
        // ImFontBaked::BakedId (uint32 in ImGui; widened here) — the stable
        // lifetime key. Never key on the ImFontBaked address: ImGui documents
        // baked pointers as per-frame only and its BakedPool GC memcpys
        // surviving bakes to new addresses, so address keys leak slabs or
        // free a live bake's slabs (see the imslug-glyph-format skill).
        uint64_t BakeId;
        float X0, Y0, X1, Y1;  // baked pixel box (== ImFontGlyph box)
        float Ascent;          // baked ascent (ImFontBaked::Ascent): Y0..Y1 are
                               // line-top relative, em is baseline relative
        float PixelsPerEm;     // baked pixels per em (uniform x/y)
        uint32_t Slot;         // UV slot id (this slab's index)
        bool Alive;
    };

    // ---------------------------------------------------------------------
    // Backend render interface (M3): registered by a backend module (e.g.
    // ImSlug_ImplVulkan_Init) so the core can write instances and the draw
    // callback can record draws without knowing the backend.
    // ---------------------------------------------------------------------
    // One Slug draw batch: a maximal run of text glyphs in a single ImDrawCmd,
    // with instances parked in the backend's per-frame ring. Batches live in
    // the core's per-frame arena; ImSlug::DrawCallback receives one through
    // ImDrawCmd::UserCallbackData.
    static const uint32_t kBatchMagic = 0x494d5342u;  // 'IMSB'

    struct ImSlugBatch
    {
        uint32_t Magic;          // kBatchMagic
        uint32_t FrameIndex;     // backend ring slot the instances live in
        uint32_t FirstInstance;  // slot-relative first instance
        uint32_t InstanceCount;
        float ClipMinX, ClipMinY, ClipMaxX, ClipMaxY;  // ImGui display coordinates
    };

    struct ImSlugRenderIO
    {
        const char* Name;  // for diagnostics; never null
        // The ring slot the backend is currently filling (advances once per
        // frame, before the pre-pass).
        uint32_t (*GetFrameIndex)();
        // Reserves `count` contiguous instance records for the current frame.
        // Returns null (leaving *outFirstInstance untouched) when the frame's
        // instance capacity is exhausted; the pre-pass then drops the run.
        ImSlugGlyphInstance* (*ReserveInstances)(uint32_t count, uint32_t* outFirstInstance);
        // Records the batch's draw into the backend's active command buffer.
        void (*DrawBatch)(const ImSlugBatch* batch);
    };

    // Installs the render interface used by PreprocessDrawData/DrawCallback.
    // Null unregisters. One backend per process (last registration wins).
    void ImSlugSetRenderIO(const ImSlugRenderIO* renderIO);

    // ---------------------------------------------------------------------
    // Glyph slab registry (core-owned; the ImGui-facing adapter allocates)
    // ---------------------------------------------------------------------
    // Null when ImSlug::Init was not called or the cap is exhausted. `bakeId`
    // is the owning ImFontBaked's BakedId (stable for the bake's lifetime;
    // ImGui may move the ImFontBaked struct itself when its BakedPool is
    // compacted): ImSlugFreeGlyphSlabsForBake releases every slab of one bake
    // (the glyph records of a discarded bake die with it).
    ImSlugGlyphSlab* ImSlugAllocGlyphSlab(const ImSlugGlyphRecord* record, const ImSlugColorGlyph* colorGlyph, float x0,
                                          float y0, float x1, float y1, float ascent, float pixelsPerEm,
                                          uint64_t bakeId, uint32_t* outSlot);
    void ImSlugFreeGlyphSlab(uint32_t slot);
    void ImSlugFreeGlyphSlabsForBake(uint64_t bakeId);
    const ImSlugGlyphSlab* ImSlugGetGlyphSlab(uint32_t slot);

    // The process-wide pools glyph records are built into (owned by Init).
    // Null before Init / after Shutdown.
    ImSlugPools* ImSlugGetPools();

    // The active Config (Zeroed before Init/false when not initialized).
    const Config* ImSlugGetConfig();

    // ---------------------------------------------------------------------
    // Build status (capacity guards feed the graceful fallback of PLAN R11)
    // ---------------------------------------------------------------------
    enum ImSlugBuildStatus
    {
        ImSlugBuildStatus_Ok = 0,
        ImSlugBuildStatus_InvalidFont,    // stbtt could not parse the data
        ImSlugBuildStatus_GlyphNotFound,  // codepoint resolves to .notdef (index 0)
        ImSlugBuildStatus_OutOfMemory,
        ImSlugBuildStatus_CurvePoolLimit,  // 16-bit texel y / pool height reached
        ImSlugBuildStatus_BandPoolLimit,   // pool height reached or row growth failed
        ImSlugBuildStatus_GlyphTooLarge,   // band block not representable: a ref
                                           // list longer than one row, or a
                                           // padded block > 65535 texels (u16
                                           // offsets) -- definitive fallback
        ImSlugBuildStatus_InvalidOutline   // non-finite or out-of-range control
                                           // points (M4 robustness; synthetic
                                           // callers only) -- definitive fallback
    };
}  // namespace ImSlug

// Module APIs (the shared types above are visible to all of them).
#include "imslug_packer.h"
#include "imslug_glyphgen.h"
#include "imslug_refraster.h"
#include "imslug_colr.h"
