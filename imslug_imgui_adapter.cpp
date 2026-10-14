// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_imgui_adapter.cpp — the ONLY translation unit that includes
// imgui_internal.h (PLAN R1). It implements:
//
//   1. the custom `const ImFontLoader` installed on Slug font sources
//      (ImFontConfig::FontLoader), verified against Dear ImGui v1.92.9b:
//        - FontSrcInit/Destroy: own an ImSlugFont (our copy of the bytes);
//        - FontSrcContainsGlyph: cmap lookup + the synthetic ellipsis;
//        - FontBakedInit: Ascent/Descent with the stb loader's exact formulas;
//        - FontBakedLoadGlyph: analytic metrics + rect-less glyph (PackId stays
//          ImFontAtlasRectId_Invalid, so the atlas never packs/rasterizes and
//          never patches our UVs) + the glyph handle encoding;
//   2. the synthetic ellipsis (three translated dots) so fonts whose cmap
//      lacks U+2026/0x0085 still render elided text without allocating a
//      single bitmap texel;
//   3. UV handle decoding for the pre-pass.
//
// Handle encoding (frozen in M3; documented in imslug_internal.h and the
// imslug-glyph-format skill):
//
//   U = kUvMagicU + 2*slot + 0.25 + 0.5*fracX
//   V = kUvMagicV + 2*slot + 0.25 + 0.5*fracY
//
// where slot is the glyph slab id and fracX/fracY are the corner position
// inside the baked pixel box [X0..X1] x [Y0..Y1] (fracY 0 at the top, like
// ImGui's V0/V1). All four corners carry the same slot; the payload is affine,
// so ImGui's linear interpolation of fine-clipped corners stays inside the slot
// cell and decodes to the exact fraction of the cut edge. Decoding needs no
// atlas state at all, which also makes atlas repacking/growth a non-issue.

#include "imslug_imgui_adapter.h"

#include "imgui_internal.h"
#include "imslug_internal.h"
#include "imslug_packed.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ImSlug
{
    namespace
    {
        // ---------------------------------------------------------------------
        // Per-source / per-bake data
        // ---------------------------------------------------------------------
        // Owned by src->FontLoaderData; freed in FontSrcDestroy.
        struct SlugFontSource
        {
            ImSlugFont* Font;   // our copy of the font bytes
            int DotGlyphIndex;  // '.' or U+FF0E, 0 when absent (ellipsis source)
            uint32_t Magic;     // kSourceMagic, sanity check
        };

        // Lives in ImFontBaked::FontLoaderDatas (ImGui-managed, per baked * source).
        // ImGui does not zero that buffer, so FontBakedInit memsets it.
        struct SlugBakedSrc
        {
            SlugFontSource* Source;
            bool EllipsisValid;
            float EllipsisBakeSize;
            uint64_t Reserved[2];              // keep the size 8-byte friendly
            ImSlugGlyphRecord EllipsisRecord;  // synthetic "..." record (per bake size)
        };

        static const uint32_t kSourceMagic = 0x494d5347u;  // 'IMSG'
        static const int kMaxEllipsisSourceQuads = 21;     // 3 copies of <= 21 dot quads

        // Synthesized ellipsis codepoints (verified against the pinned ImGui:
        // ImFontAtlasBuildSetupFontSpecialGlyphs tries 0x2026 then 0x0085).
        static const ImWchar kEllipsisCodepoints[] = { (ImWchar)0x2026, (ImWchar)0x0085 };

        static bool IsEllipsisCodepoint(ImWchar codepoint)
        {
            return codepoint == kEllipsisCodepoints[0] || codepoint == kEllipsisCodepoints[1];
        }

        // ---------------------------------------------------------------------
        // Handle encoding primitives (shared with the decoder below)
        // ---------------------------------------------------------------------
        float EncodeAxis(float magic, uint32_t slot, float frac)
        {
            return magic + kUvCellSpan * (float)slot + kUvFracBase + kUvFracScale * frac;
        }

        // ---------------------------------------------------------------------
        // Font loader
        // ---------------------------------------------------------------------
        static bool SlugFontSrcInit(ImFontAtlas* atlas, ImFontConfig* src)
        {
            IM_UNUSED(atlas);
            if (src->FontData == nullptr || src->FontDataSize <= 0)
                return false;
            if (src->FontNo < 0)
                return false;

            // We own our own copy of the bytes: ImGui's copy belongs to the atlas
            // and may be freed or kept referenced differently (AddFontFromMemoryTTF
            // with FontDataOwnedByAtlas = false keeps the caller's pointer).
            //
            // M7: the font kind is auto-detected by content, not by extension:
            // data starting with the `.imslug` magic is a packed asset and is
            // loaded without any outline parsing; anything else goes through
            // the stb_truetype path (when this build has one).
            ImSlugFont* font = nullptr;
            if (ImSlugPackedIsContainer(src->FontData, src->FontDataSize))
            {
                char error[256];
                if (!ImSlugValidatePackedMemory(src->FontData, src->FontDataSize, nullptr, error, (int)sizeof(error)))
                    fprintf(stderr, "[imslug] invalid .imslug asset: %s\n", error);
                font = ImSlugCreateFontFromPackedMemory(src->FontData, src->FontDataSize);
            }
#if !defined(IMSLUG_NO_STBTT)
            else
            {
                font = ImSlugCreateFontFromMemory(src->FontData, src->FontDataSize, src->FontNo, /*copyData=*/true);
            }
#else
            else
            {
                fprintf(stderr, "[imslug] this build has no TTF parser (IMSLUG_TTF_PARSER=OFF); only packed .imslug "
                                "assets can be loaded\n");
                return false;
            }
#endif
            if (font == nullptr)
                return false;

            SlugFontSource* source = (SlugFontSource*)IM_ALLOC(sizeof(SlugFontSource));
            if (source == nullptr)
            {
                // The atlas rolls this source back (FontSrcInit contract); release
                // our font copy first or it leaks with the failed source.
                ImSlugDestroyFont(font);
                return false;
            }
            memset(source, 0, sizeof(*source));
            source->Font = font;
            source->Magic = kSourceMagic;
            source->DotGlyphIndex = ImSlugGetGlyphIndex(font, (unsigned int)'.');
            if (source->DotGlyphIndex == 0)
                source->DotGlyphIndex = ImSlugGetGlyphIndex(font, 0xFF0Eu);
            src->FontLoaderData = source;
            return true;
        }

        static void SlugFontSrcDestroy(ImFontAtlas* atlas, ImFontConfig* src)
        {
            IM_UNUSED(atlas);
            SlugFontSource* source = (SlugFontSource*)src->FontLoaderData;
            if (source != nullptr)
            {
                if (source->Magic == kSourceMagic)
                    ImSlugDestroyFont(source->Font);
                IM_FREE(source);
                src->FontLoaderData = nullptr;
            }
        }

        static bool SlugFontSrcContainsGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImWchar codepoint)
        {
            IM_UNUSED(atlas);
            SlugFontSource* source = (SlugFontSource*)src->FontLoaderData;
            if (source == nullptr || source->Magic != kSourceMagic || source->Font == nullptr)
                return false;
            if (ImSlugGetGlyphIndex(source->Font, (unsigned int)codepoint) != 0)
                return true;
            // We can render the ellipsis from its dots (no bitmap), so claim the
            // ellipsis codepoints whenever the source has a dot glyph. This keeps
            // ImFont::EllipsisAutoBake false, so ImGui's texture-compositing
            // ellipsis path (which needs a packed '.' rect) never runs for us.
            return IsEllipsisCodepoint(codepoint) && source->DotGlyphIndex != 0;
        }

        static bool SlugFontBakedInit(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loaderData)
        {
            IM_UNUSED(atlas);
            SlugBakedSrc* bakedSrc = (SlugBakedSrc*)loaderData;
            memset(bakedSrc, 0, sizeof(*bakedSrc));

            SlugFontSource* source = (SlugFontSource*)src->FontLoaderData;
            bakedSrc->Source = (source != nullptr && source->Magic == kSourceMagic) ? source : nullptr;

            if (bakedSrc->Source != nullptr && src->MergeMode == false)
            {
                // ImGui_ImplStbTrueType_FontBakedInit parity (imgui_draw.cpp 4752):
                //   scale_for_layout = ScaleFactor * baked->Size / ExtraSizeScale
                //   Ascent = ImCeil(...), Descent = ImFloor(...)
                ImSlugFontMetrics metrics;
                ImSlugGetFontMetrics(bakedSrc->Source->Font, baked->Size, &metrics);
                baked->Ascent = metrics.Ascent;
                baked->Descent = metrics.Descent;
            }
            return true;
        }

        static void SlugFontBakedDestroy(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loaderData)
        {
            IM_UNUSED(atlas);
            IM_UNUSED(src);
            SlugBakedSrc* bakedSrc = (SlugBakedSrc*)loaderData;
            if (bakedSrc == nullptr)
                return;
            // Frees the glyph slabs (including the synthetic ellipsis) whose
            // record only lives as long as this bake. Keyed on baked->BakedId,
            // never on the ImFontBaked address: ImGui documents those pointers
            // as per-frame only and its BakedPool GC (imgui_draw.cpp ~2823)
            // memcpys surviving bakes to new addresses, so an address key
            // would leak this bake's slabs and, worse, free another live
            // bake's slabs that now sits at the old address.
            ImSlugFreeGlyphSlabsForBake(baked->BakedId);
            memset(bakedSrc, 0, sizeof(*bakedSrc));
        }

        // Ellipsis metrics, replicating ImFontAtlasBuildSetupFontBakedEllipsis
        // (imgui_draw.cpp 3765) in pixel space: three dots with a 1 px gap.
        static bool ComputeEllipsisMetrics(ImSlugFont* font, int dotGlyphIndex, float bakeSize, float pixelsPerEm,
                                           ImSlugGlyphMetrics* outMetrics)
        {
            ImSlugGlyphMetrics dot;
            if (!ImSlugGetGlyphMetricsByIndex(font, dotGlyphIndex, bakeSize, &dot))
                return false;
            if (!(dot.X1 > dot.X0) || !(dot.Y1 > dot.Y0))
                return false;  // invisible dot: no ellipsis to compose

            const float dotSpacing = 1.0f;  // px, as in ImGui
            const float dotStep = (dot.X1 - dot.X0) + dotSpacing;
            outMetrics->X0 = dot.X0;
            outMetrics->Y0 = dot.Y0;
            outMetrics->X1 = dot.X0 + dotStep * 3.0f - dotSpacing;
            outMetrics->Y1 = dot.Y1;
            outMetrics->AdvanceX = ImMax(dot.AdvanceX, outMetrics->X1);
            outMetrics->MinX = outMetrics->MaxX = 0.0f;
            outMetrics->MinY = outMetrics->MaxY = 0.0f;
            outMetrics->AdvanceEm = outMetrics->AdvanceX / pixelsPerEm;
            return true;
        }

        // Builds (once per bake size) the synthetic ellipsis record: three copies
        // of the dot outline translated by the pixel-exact step, converted to em
        // space. The em box then matches the pixel metrics the glyph carries, so
        // placement reconstruction stays exact.
        static bool EnsureEllipsisRecord(SlugBakedSrc* bakedSrc, int dotGlyphIndex, float bakeSize, float pixelsPerEm,
                                         const ImSlugGlyphMetrics& dotMetrics, float advanceEm)
        {
            ImSlugFont* font = bakedSrc->Source->Font;
            if (bakedSrc->EllipsisValid && bakedSrc->EllipsisBakeSize == bakeSize)
                return true;

            ImSlugQuad dotQuads[kMaxEllipsisSourceQuads];
            int dotQuadCount = 0;
            if (!ImSlugGetGlyphOutlineQuads(font, dotGlyphIndex, dotQuads, kMaxEllipsisSourceQuads, &dotQuadCount))
                return false;
            if (dotQuadCount <= 0 || dotQuadCount > kMaxEllipsisSourceQuads / 3)
                return false;

            const float dotStepEm = ((dotMetrics.X1 - dotMetrics.X0) + 1.0f) / pixelsPerEm;
            ImSlugQuad composite[3 * kMaxEllipsisSourceQuads];
            for (int copy = 0; copy < 3; ++copy)
            {
                const float offset = (float)copy * dotStepEm;
                for (int i = 0; i < dotQuadCount; ++i)
                {
                    const ImSlugQuad& q = dotQuads[i];
                    ImSlugQuad& out = composite[copy * dotQuadCount + i];
                    out.X1 = q.X1 + offset;
                    out.Y1 = q.Y1;
                    out.X2 = q.X2 + offset;
                    out.Y2 = q.Y2;
                    out.X3 = q.X3 + offset;
                    out.Y3 = q.Y3;
                }
            }

            ImSlugPools* pools = ImSlugGetPools();
            if (pools == nullptr)
                return false;
            ImSlugGlyphRecord record;
            const ImSlugBuildStatus status =
                ImSlugBuildSyntheticGlyphRecord(font, pools, composite, 3 * dotQuadCount, advanceEm, &record);
            if (status != ImSlugBuildStatus_Ok)
                return false;

            bakedSrc->EllipsisRecord = record;
            bakedSrc->EllipsisValid = true;
            bakedSrc->EllipsisBakeSize = bakeSize;
            return true;
        }

        static bool SlugFontBakedLoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loaderData,
                                           ImWchar codepoint, ImFontGlyph* outGlyph, float* outAdvanceX)
        {
            IM_UNUSED(atlas);
            IM_UNUSED(src);
            SlugBakedSrc* bakedSrc = (SlugBakedSrc*)loaderData;
            if (bakedSrc == nullptr || bakedSrc->Source == nullptr)
                return false;
            ImSlugFont* font = bakedSrc->Source->Font;

            const int glyphIndex = ImSlugGetGlyphIndex(font, (unsigned int)codepoint);
            const bool synthetic =
                (glyphIndex == 0) && IsEllipsisCodepoint(codepoint) && bakedSrc->Source->DotGlyphIndex != 0;
            if (glyphIndex == 0 && !synthetic)
                return false;

            ImSlugFontMetrics fontMetrics;
            ImSlugGetFontMetrics(font, baked->Size, &fontMetrics);
            const float pixelsPerEm = fontMetrics.PixelsPerEm;
            if (!(pixelsPerEm > 0.0f))
                return false;

            ImSlugGlyphMetrics metrics;
            const ImSlugGlyphRecord* record = nullptr;
            const ImSlugColorGlyph* colorGlyph = nullptr;  // M5: non-null for a drawable color glyph
            if (synthetic)
            {
                if (!ComputeEllipsisMetrics(font, bakedSrc->Source->DotGlyphIndex, baked->Size, pixelsPerEm, &metrics))
                    return false;
                if (outAdvanceX != nullptr)
                {
                    *outAdvanceX = metrics.AdvanceX;
                    return true;
                }
                ImSlugGlyphMetrics dotMetrics;
                if (!ImSlugGetGlyphMetricsByIndex(font, bakedSrc->Source->DotGlyphIndex, baked->Size, &dotMetrics))
                    return false;
                const float advanceEm = metrics.AdvanceX / pixelsPerEm;
                if (!EnsureEllipsisRecord(bakedSrc, bakedSrc->Source->DotGlyphIndex, baked->Size, pixelsPerEm,
                                          dotMetrics, advanceEm))
                    return false;
                metrics.AdvanceEm = advanceEm;
                record = &bakedSrc->EllipsisRecord;
            }
            else
            {
                if (!ImSlugGetGlyphMetricsByIndex(font, glyphIndex, baked->Size, &metrics))
                    return false;
                if (outAdvanceX != nullptr)
                {
                    IM_ASSERT(outGlyph == NULL);
                    *outAdvanceX = metrics.AdvanceX;
                    return true;
                }
                ImSlugPools* pools = ImSlugGetPools();
                if (pools == nullptr)
                    return false;
                const ImSlugBuildStatus status = ImSlugGetGlyphRecord(font, pools, (unsigned int)codepoint, &record);
                if (status != ImSlugBuildStatus_Ok || record == nullptr)
                    return false;  // pool limits: ImGui uses its fallback glyph

                // M5: resolve COLR color glyphs (the v0/v1 layer list + paint
                // records). A failure falls back to the monochrome outline.
                const Config* config = ImSlugGetConfig();
                if (config == nullptr || config->EnableColorFonts)
                {
                    const ImSlugColorGlyph* resolved = nullptr;
                    if (ImSlugGetColorGlyph(font, pools, glyphIndex, &resolved) == ImSlugBuildStatus_Ok &&
                        resolved != nullptr && resolved->LayerCount > 0)
                    {
                        // Emoji base glyphs are often empty (Noto Color Emoji's
                        // glyf has no contours): synthesize the layout box from
                        // the layers' union so the glyph is visible and its
                        // advance (hmtx) is unchanged - layout stays identical
                        // to the monochrome path.
                        if (!(metrics.X1 > metrics.X0) || !(metrics.Y1 > metrics.Y0))
                        {
                            const float ascentRounded = floorf(fontMetrics.Ascent + 0.5f);
                            metrics.X0 = floorf(resolved->MinX * pixelsPerEm);
                            metrics.X1 = ceilf(resolved->MaxX * pixelsPerEm);
                            metrics.Y0 = floorf(-resolved->MaxY * pixelsPerEm) + ascentRounded;
                            metrics.Y1 = ceilf(-resolved->MinY * pixelsPerEm) + ascentRounded;
                        }
                        colorGlyph = resolved;
                        record = &resolved->BaseRecord;  // reconstruction uses the covering bbox
                    }
                }
            }
            IM_ASSERT(record != nullptr);

            // Prepare the ImFontGlyph exactly like the stb loader does for the
            // Slug configuration (Oversample 1, RasterizerDensity 1, no snapping).
            outGlyph->Codepoint = codepoint;
            outGlyph->AdvanceX = metrics.AdvanceX;
            outGlyph->X0 = metrics.X0;
            outGlyph->Y0 = metrics.Y0;
            outGlyph->X1 = metrics.X1;
            outGlyph->Y1 = metrics.Y1;
            // Colored glyphs: the pinned v1.92.9b RenderText/RenderChar force
            // RGB=255 (col | ~IM_COL32_A_MASK) and keep the alpha, so ImGui's
            // tint can no longer recolor them - exactly the semantic the color
            // path needs (our layer colors are authoritative, a tint only
            // fades).
            outGlyph->Colored = colorGlyph != nullptr;
            outGlyph->Visible = (metrics.X0 != metrics.X1 && metrics.Y0 != metrics.Y1);

            if (!outGlyph->Visible)
            {
                // Invisible (space and friends): never drawn, no slab needed.
                outGlyph->Colored = false;
                outGlyph->U0 = outGlyph->V0 = outGlyph->U1 = outGlyph->V1 = 0.0f;
                return true;
            }

            uint32_t slot = 0;
            if (ImSlugAllocGlyphSlab(record, colorGlyph, metrics.X0, metrics.Y0, metrics.X1, metrics.Y1, baked->Ascent,
                                     pixelsPerEm, baked->BakedId, &slot) == nullptr)
            {
                // Slab cap reached: fall back to ImGui's fallback glyph. Give back
                // the partially filled glyph state so ImGui does not publish it.
                return false;
            }

            // Corner fractions inside the baked pixel box: 0 at X0/Y0 (top) and 1
            // at X1/Y1, which is exactly what ImGui's RenderText writes into the
            // quad corners.
            outGlyph->U0 = EncodeAxis(kUvMagicU, slot, 0.0f);
            outGlyph->U1 = EncodeAxis(kUvMagicU, slot, 1.0f);
            outGlyph->V0 = EncodeAxis(kUvMagicV, slot, 0.0f);
            outGlyph->V1 = EncodeAxis(kUvMagicV, slot, 1.0f);
            return true;
        }

        const ImFontLoader* GetSlugFontLoader()
        {
            // Not a function-local static: the pointer must be stable and the
            // struct must be mutable in place (ImGui may keep the pointer).
            static ImFontLoader loader;
            if (loader.Name == nullptr)
            {
                loader.Name = "imslug";
                loader.LoaderInit = nullptr;      // required null for per-config loaders
                loader.LoaderShutdown = nullptr;  // (ImGui asserts on both)
                loader.FontSrcInit = SlugFontSrcInit;
                loader.FontSrcDestroy = SlugFontSrcDestroy;
                loader.FontSrcContainsGlyph = SlugFontSrcContainsGlyph;
                loader.FontBakedInit = SlugFontBakedInit;
                loader.FontBakedDestroy = SlugFontBakedDestroy;
                loader.FontBakedLoadGlyph = SlugFontBakedLoadGlyph;
                loader.FontBakedSrcLoaderDataSize = sizeof(SlugBakedSrc);
            }
            return &loader;
        }
    }  // namespace

    // -------------------------------------------------------------------------
    // Adapter interface (imslug_imgui_adapter.h)
    // -------------------------------------------------------------------------
    void ImSlugAdapterPrepareFontConfig(ImFontConfig* cfg)
    {
        IM_ASSERT(cfg != nullptr);
        if (cfg->MergeMode)
        {
            // MergeMode needs ImGui's own per-source scaling; Slug sources are
            // standalone targets in M3.
            IMSLUG_ASSERT(false && "imslug: MergeMode font sources are not supported yet");
            cfg->MergeMode = false;
        }

        // Metric-neutral settings: the analytic bake below mirrors ImGui's stb
        // loader for exactly this configuration (see imslug_glyphgen.cpp).
        cfg->OversampleH = 1;
        cfg->OversampleV = 1;
        cfg->RasterizerDensity = 1.0f;
        cfg->ExtraSizeScale = 1.0f;
        cfg->GlyphOffset = ImVec2(0.0f, 0.0f);
        cfg->PixelSnapH = false;
        cfg->GlyphMinAdvanceX = 0.0f;
        cfg->GlyphMaxAdvanceX = FLT_MAX;

        cfg->FontLoader = GetSlugFontLoader();
        cfg->FontLoaderData = nullptr;  // must be null (ImGui asserts)
        cfg->FontLoaderFlags = 0;
    }

    bool ImSlugAdapterIsFontRegistered(const ImFont* font)
    {
        if (font == nullptr)
            return false;
        const ImFontLoader* loader = GetSlugFontLoader();
        for (const ImFontConfig* src : font->Sources)
            if (src->FontLoader == loader)
                return true;
        return false;
    }

    bool ImSlugAdapterDecodeVertex(float u, float v, ImSlugDecodedVertex* out)
    {
        if (out == nullptr)
            return false;
        out->Slab = nullptr;
        out->FracX = 0.0f;
        out->FracY = 0.0f;

        // Real atlas UVs live in [0,1]; both magic axes are above that, and the V
        // magic differs from the U magic so an axis swap cannot decode.
        if (!isfinite(u) || !isfinite(v))
            return false;
        if (u < kUvMagicU || v < kUvMagicV)
            return false;

        const float au = u - kUvMagicU;
        const float av = v - kUvMagicV;
        const float slotU = floorf((au - kUvFracBase) / kUvCellSpan + 0.5f);
        const float slotV = floorf((av - kUvFracBase) / kUvCellSpan + 0.5f);
        if (slotU != slotV || slotU < 0.0f || slotU >= (float)kMaxGlyphSlabs)
            return false;

        const float fracEncU = (au - slotU * kUvCellSpan - kUvFracBase) / kUvFracScale;
        const float fracEncV = (av - slotV * kUvCellSpan - kUvFracBase) / kUvFracScale;
        if (fracEncU < -kUvFracTolerance || fracEncU > 1.0f + kUvFracTolerance)
            return false;
        if (fracEncV < -kUvFracTolerance || fracEncV > 1.0f + kUvFracTolerance)
            return false;

        const uint32_t slot = (uint32_t)slotU;
        const ImSlugGlyphSlab* slab = ImSlugGetGlyphSlab(slot);
        if (slab == nullptr || slab->Record == nullptr)
            return false;

        out->Slab = slab;
        out->FracX = ImClamp(fracEncU, 0.0f, 1.0f);
        out->FracY = ImClamp(fracEncV, 0.0f, 1.0f);
        return true;
    }

    void ImSlugAdapterShutdown()
    {
        // Per-source/bake data is owned by the ImGui atlas (and released with it);
        // the glyph slabs are released by ImSlug::Shutdown. Nothing to do here
        // besides documenting the contract.
    }
}  // namespace ImSlug
