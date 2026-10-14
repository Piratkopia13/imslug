// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_colr.h — COLR/CPAL color-font ingestion (M5): a minimal SFNT table
// reader over the same font bytes stb_truetype uses (it does not expose COLR or
// CPAL), a COLR v0 layer-list reader, a COLR v1 paint-graph flattener, and the
// per-font color-glyph cache that turns a graph into drawable Slug records plus
// paint-pool records.
//
// Parsing contract (OpenType COLR/CPAL specifications; field layouts verified
// against fontTools' otData and the fonts' own bytes):
//   * all offsets are relative to the beginning of their containing table
//     (BaseGlyphPaintRecord -> BaseGlyphList, Offset24 -> the parent paint);
//   * coordinates are font design units, Y up (the same space as glyf/stbtt);
//     the cache converts to em (divide by unitsPerEm);
//   * F2DOT14 = raw / 16384; Fixed = raw / 65536; angles are degrees/180 per
//     the spec's Angle converters (sweep angles are biased by 180 degrees);
//   * CPAL palette 0 is used (both license-vetted test fonts ship one palette);
//     palette index 0xFFFF = the text foreground color, which for a Colored
//     glyph is white (ImGui forces RGB=255; see the adapter), alpha 1;
//   * COLR v1 variable (PaintVar*) formats are read with their default (non-
//     variation) field values: variable fonts render at the default instance
//     (documented fallback, no variation-store evaluation).
//
// Robustness: every read is bounds-checked against the font bytes; malformed or
// unsupported content never crashes - the affected subtree is skipped, and a
// glyph with no usable layer is reported as "not a color glyph" so callers keep
// the monochrome path (PLAN R11-style graceful fallback).
//
// References: DearWidgets (CC0) documents the same parsing exercise in
// src/api/dear_widgets_slug.cpp; no code was copied from it (see NOTICE).
// Slughorn (MIT) and osgSlug were not used for the reader. The gradient
// evaluation semantics (linear p2 rotation, two-point conical roots, sweep
// angles) follow the OpenType COLR specification text and the WHATWG canvas
// radial-gradient algorithm it references.

#pragma once

#include "imslug_internal.h"

namespace ImSlug
{
    struct ImSlugFont;  // glyphgen.h (the font handle the color cache hangs off)

    // ---------------------------------------------------------------------
    // Per-font color data (opaque)
    // ---------------------------------------------------------------------
    struct ImSlugColorFont;

    // Parses the SFNT table directory and the CPAL/COLR headers. `fontOffset`
    // is the stbtt font offset (ttcf handling), `unitsPerEm` the font's head
    // value. Returns null when the font has no CPAL or no COLR table, or when
    // the tables are malformed beyond use; that is the monochrome fallback.
    ImSlugColorFont* ImSlugColorFontCreate(const void* fontData, int fontDataSize, int fontOffset, int unitsPerEm);
    void ImSlugColorFontDestroy(ImSlugColorFont* colorFont);

    // True when `glyphIndex` has COLR data (v0 layer records or a v1 paint
    // graph). Does not build anything: cheap enough for the bake path.
    bool ImSlugColorFontHasGlyph(const ImSlugColorFont* colorFont, int glyphIndex);

    // ---------------------------------------------------------------------
    // Color glyph cache
    // ---------------------------------------------------------------------
    // Resolves a glyph's COLR data into draw-order layers (one Slug glyph
    // record + one paint-pool record per layer), building and caching it on
    // first use. The layers' records are allocated into `pools` (curve/band
    // texels and paint records) and stay valid for the font's lifetime; a
    // failed build is rolled back and cached as "no color glyph" (the caller
    // falls back to monochrome).
    //
    // Fallbacks (documented, never fatal):
    //   * unsupported paint formats are skipped; a glyph left without layers
    //     reports no color glyph;
    //   * graphs deeper than kMaxColorGraphDepth, with more than
    //     kMaxColorGraphNodes visited paints, more than kMaxColorLayers layers
    //     or cyclic PaintColrGlyph references are truncated (the partial layer
    //     list is still drawn - it is draw-ordered, so leading layers are the
    //     visually dominant ones);
    //   * color lines with more than kMaxColorStops stops keep the first stops;
    //   * composite modes other than SRC_OVER/SRC/DEST/DEST_OVER are folded
    //     into an alpha multiplier when the mask operand is a constant PaintSolid
    //     (exact; this covers all Segoe UI Emoji composites and the shading
    //     composites in Noto Color Emoji) and otherwise approximated as
    //     source-over (see ImSlugColorCompositeMode below).
    ImSlugBuildStatus ImSlugGetColorGlyph(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                          const ImSlugColorGlyph** outColorGlyph);

    // ---------------------------------------------------------------------
    // Instance emission (shared by the ImGui pre-pass and the golden harness)
    // ---------------------------------------------------------------------
    // Emits one instance per layer at the placement (`penX`, `penY`, pixels per
    // em), all with Color = (1, 1, 1, tintA) and PaintIndex = the layer's paint
    // (the shader/CPU sampler resolve the actual color). Returns the number of
    // instances written; `capacity` bounds the write (extra layers are
    // dropped). When `outLayerIndices` is non-null it receives the layer index
    // of every emitted instance (0-based into colorGlyph->Layers), which lets
    // test harnesses evaluate the expected image layer for layer.
    int ImSlugBuildColorGlyphInstances(const ImSlugColorGlyph* colorGlyph, float penX, float penY, float pixelsPerEm,
                                       float tintA, ImSlugGlyphInstance* outInstances, int capacity,
                                       int* outLayerIndices);

    // ---------------------------------------------------------------------
    // CPU paint evaluation (mirror of the slug.frag paint code)
    // ---------------------------------------------------------------------
    // Samples a paint record at a *record-space* em coordinate and returns
    // straight-alpha RGBA in [0,1] (rgb, a = paint alpha). Returns (0,0,0,0)
    // for fragments outside the ClipBox or outside a gradient whose domain is
    // ill-formed/degenerate. The caller multiplies the alpha by the instance
    // tint and the Slug coverage, exactly like the fragment shader.
    void ImSlugSamplePaintRecord(const ImSlugPaintRecord* paint, float emX, float emY, float outRgba[4]);

    // Packs/unpacks the record's 8-bit colors: r | g<<8 | b<<16 | a<<24.
    uint32_t ImSlugPackColorRGBA8(float r, float g, float b, float a);

    // True when `quirks` (diagnostics): the color font has a v0 table, a v1
    // table, or both; the last ImSlugGetColorGlyph fallback reason. Kept for
    // tests/documentation rather than runtime decisions.
    struct ImSlugColorFontStats
    {
        int HasColrV0;
        int HasColrV1;
        int HasClipList;
        int NumPalettes;
        int NumPaletteEntries;
    };
    void ImSlugColorFontGetStats(const ImSlugColorFont* colorFont, ImSlugColorFontStats* outStats);
}  // namespace ImSlug
