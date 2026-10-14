// imslug — implementation.
//
// This file holds the backend-independent M3 core:
//   * lifecycle (Init/Shutdown/NewFrame) and the glyph slab registry;
//   * the per-frame arena (batches + instances) that ::PreprocessDrawData
//     fills and ::DrawCallback consumes;
//   * the draw-data pre-pass: text-run classification, fine-clip
//     reconstruction, CmdBuffer rewriting and the ResetRenderState injection.
//
// Constraints (see .agent-docs/milestones/M3-imgui-integration.md):
//   * imgui.h only — every imgui_internal.h use lives in
//     imslug_imgui_adapter.cpp (PLAN R1);
//   * no backend knowledge: instances and draws go through ImSlugRenderIO,
//     registered by a backend module (Vulkan in backends/imslug_impl_vulkan.*);
//   * VtxBuffer/IdxBuffer are never modified; only CmdBuffer is rewritten
//     (in place, through a scratch vector).
//
// The M1/M2 core helpers (ImSlugBuildGlyphInstance, record builders, packer,
// CPU rasterizer) live in imslug_glyphgen/packer/refraster.cpp.

#include "imslug.h"

#include "imslug_imgui_adapter.h"
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
        // Small helpers
        // ---------------------------------------------------------------------
        // The public header pulls in imgui.h only: ImMin/ImMax/ImClamp live in
        // imgui_internal.h, which this file must not include (PLAN R1).
        static inline float ImSlugMin(float a, float b)
        {
            return a < b ? a : b;
        }
        static inline float ImSlugMax(float a, float b)
        {
            return a > b ? a : b;
        }
        static inline unsigned int ImSlugMin(unsigned int a, unsigned int b)
        {
            return a < b ? a : b;
        }

        // ---------------------------------------------------------------------
        // Per-triangle classification (pre-pass detail)
        // ---------------------------------------------------------------------
        // A text glyph is exactly 4 vertices / 6 indices emitted by ImGui as
        //   indices (m, m+1, m+2, m, m+2, m+3)  with  m..m+3 = TL, TR, BR, BL
        // (both ImFont::RenderText and ImFont::RenderChar, verified against
        // v1.92.9b imgui_draw.cpp). Because a command boundary can in principle
        // split the 6 indices, classification is per triangle and a glyph is only
        // admitted when both of its triangles are present, decode, and agree on the
        // slab and the vertex block. Anything else that decodes as a handle is
        // dropped (drawing it through the atlas would sample garbage UVs); plain
        // geometry keeps its exact order and offsets.
        enum TriAction : uint8_t
        {
            TriAction_Geometry = 0,   // keep in a geometry command
            TriAction_Drop = 1,       // Slug-looking but not a complete glyph: not drawn
            TriAction_Glyph = 2,      // first triangle of a complete glyph
            TriAction_GlyphTail = 3,  // second triangle of a complete glyph
        };

        // Placement reconstruction tolerances. Fractions are decode-exact for
        // unclipped corners; clipped corners carry float rounding of the encoded
        // value, so the openness test uses a small epsilon. The placement slack
        // absorbs ImGui's integer glyph box: X0..Y1 are floor/ceil of the exact
        // em box *plus* round(baked ascent) on Y, so the drawn quad can exceed
        // the control-point bbox by up to ~1.5 px per side (M4 fuzz: measured
        // max 1.076 px at 6.5 px, where ceil + round can both contribute). It is
        // a garbage filter, not a correctness gate: the fuzz harness bounds the
        // real placement error separately.
        static const float kFracOpenEpsilon = 1e-3f;  // 0/1 exactness margin
        // A fraction span below this is dominated by the float quantization of
        // the decoded (fine-clipped) fractions: the encoded UV ulp is ~0.002
        // frac units at slot ids of a few thousand, so a fitted slope through
        // such samples can be ~10 % off and amplifies the whole placement. The
        // sliver path therefore uses the single-corner sample for the pen and
        // the glyph's baked ppem for the scale (the drawn size differs from the
        // bake size only by ImGui's integer rounding, <= ~1.5 %). Slivers are
        // common in real UIs (text cut by a clip edge); the M4 fuzz measured
        // the fix: sliver pen error max 2.71 px -> 0.31 px at 100k cases.
        static const float kSpanReliable = 0.1f;      // use the fitted slope at/above this
        static const float kPlacementSlack = 1.6f;    // px; see the note above kFracOpenEpsilon
        static const float kClipEdgeEpsilon = 0.01f;  // px, same-fine-clip comparison

        struct InstanceArena
        {
            ImSlugGlyphInstance* Data;
            int Count;
            int Capacity;
        };

        struct BatchArena
        {
            ImSlugBatch* Data;
            int Count;
            int Capacity;
        };

        struct Core
        {
            bool Initialized;
            Config Cfg;

            ImSlugPools Pools;

            // Glyph slabs: slot id == index. Slots are handed out from a high-water
            // mark or the free list and never shrink, so a slot id stays stable for
            // the process lifetime (the decode path relies on it).
            ImSlugGlyphSlab* Slabs;
            uint32_t SlabCapacity;   // allocated entries
            uint32_t SlabHighWater;  // first never-used slot
            uint32_t* FreeSlots;     // stack of freed slot ids
            int FreeCount;
            int FreeCapacity;

            // Per-frame arena (reset by NewFrame / frame-count change).
            InstanceArena Instances;
            BatchArena Batches;
            int ArenaFrameCount;
            int CommittedBatches;  // batches already copied into the ring this arena

            // Scratch for CmdBuffer rewriting (grown, never shrunk).
            ImDrawCmd* CmdScratch;
            int CmdScratchCapacity;
            uint8_t* TriActions;
            int TriActionsCapacity;
            // M5: per-glyph color layer instances (one glyph's layers at a time).
            ImSlugGlyphInstance* ColorInstances;
            int ColorInstancesCapacity;

            const ImSlugRenderIO* RenderIO;

            Statistics Stats;
            bool WarnedNoBackend;
            bool WarnedNoTextures;
            bool WarnedNoReset;
        };

        Core g_Core;

        // ---------------------------------------------------------------------
        // Growth helpers
        // ---------------------------------------------------------------------
        bool EnsureCapacity(void** data, int* capacity, int required, size_t elementSize)
        {
            if (required <= *capacity)
                return true;
            int newCapacity = (*capacity > 0) ? *capacity : 4096;
            while (newCapacity < required)
                newCapacity *= 2;
            void* newData = realloc(*data, (size_t)newCapacity * elementSize);
            if (newData == nullptr)
                return false;
            *data = newData;
            *capacity = newCapacity;
            return true;
        }

        void ResetArena()
        {
            g_Core.Instances.Count = 0;
            g_Core.Batches.Count = 0;
            g_Core.CommittedBatches = 0;
            g_Core.Stats.LiveBatches = 0;
            g_Core.Stats.Instances = 0;
            g_Core.Stats.DroppedRuns = 0;
            g_Core.Stats.BoundsMismatches = 0;
            g_Core.Stats.ColorLayers = 0;
        }

        // Registration order contract (imslug.h): ImSlug::Init first, fonts after.
        // Slug sources added earlier still work (slabs are allocated at bake time),
        // but the documented order is the supported one - warn loudly in debug.
        void CheckFontRegistrationOrder()
        {
            if (g_Core.Initialized)
                return;
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                fprintf(stderr,
                        "[imslug] AddFontFrom* called before ImSlug::Init(); call ImSlug::Init() first (imslug.h).\n");
            }
            IMSLUG_ASSERT(false && "imslug: call ImSlug::Init() before ImSlug::AddFontFrom*()");
        }

        void NewFrameIfNeeded()
        {
            const int frameCount = ImGui::GetFrameCount();
            if (frameCount != g_Core.ArenaFrameCount)
            {
                ResetArena();
                g_Core.ArenaFrameCount = frameCount;
            }
        }

        bool AppendInstances(const ImSlugGlyphInstance* instances, int count, int* outFirst)
        {
            if (count <= 0)
            {
                *outFirst = 0;
                return true;
            }
            if (!EnsureCapacity((void**)&g_Core.Instances.Data, &g_Core.Instances.Capacity,
                                g_Core.Instances.Count + count, sizeof(ImSlugGlyphInstance)))
                return false;
            *outFirst = g_Core.Instances.Count;
            memcpy(g_Core.Instances.Data + g_Core.Instances.Count, instances,
                   (size_t)count * sizeof(ImSlugGlyphInstance));
            g_Core.Instances.Count += count;
            return true;
        }

        ImSlugBatch* AppendBatch()
        {
            if (!EnsureCapacity((void**)&g_Core.Batches.Data, &g_Core.Batches.Capacity, g_Core.Batches.Count + 1,
                                sizeof(ImSlugBatch)))
                return nullptr;
            ImSlugBatch* batch = &g_Core.Batches.Data[g_Core.Batches.Count++];
            memset(batch, 0, sizeof(*batch));
            batch->Magic = kBatchMagic;
            return batch;
        }

        // ---------------------------------------------------------------------
        // Fine-clip reconstruction
        // ---------------------------------------------------------------------
        // Fits the affine map frac -> screen for one axis from up to four samples
        // (corner fraction, corner screen coordinate). Picks the pair with the
        // widest fraction span, which is the most noise-immune choice. Returns
        // false when all samples share the same fraction (fully degenerate).
        bool FitAxis(const float* frac, const float* screen, int count, float* outOrigin, float* outSlope,
                     float* outSpan)
        {
            int bestI = -1;
            int bestJ = -1;
            float bestSpan = 0.0f;
            for (int i = 0; i < count; ++i)
            {
                for (int j = i + 1; j < count; ++j)
                {
                    const float span = fabsf(frac[j] - frac[i]);
                    if (span > bestSpan)
                    {
                        bestSpan = span;
                        bestI = i;
                        bestJ = j;
                    }
                }
            }
            if (bestI < 0 || bestSpan <= 0.0f)
                return false;

            const float slope = (screen[bestJ] - screen[bestI]) / (frac[bestJ] - frac[bestI]);
            *outOrigin = screen[bestI] - slope * frac[bestI];
            *outSlope = slope;
            *outSpan = bestSpan;
            return true;
        }

        // Reconstructs the placement (pen position + effective pixels per em) of a
        // glyph from its four quad corners. `fracX/fracY` are the decoded corner
        // fractions inside the baked pixel box, `pos` the corner screen positions
        // in ImGui display coordinates. Works for clipped and unclipped quads
        // alike: corner fractions are affine in corner positions, so the fitted map
        // evaluates the full (unclipped) glyph box even when ImGui cropped the
        // geometry.
        bool ReconstructPlacement(const ImSlugGlyphSlab* slab, const ImVec2 pos[4], const float fracX[4],
                                  const float fracY[4], float* outPenX, float* outPenY, float* outPixelsPerEm)
        {
            const ImSlugGlyphRecord* record = slab->Record;
            float quadMinX = pos[0].x, quadMaxX = pos[0].x, quadMinY = pos[0].y, quadMaxY = pos[0].y;
            for (int i = 1; i < 4; ++i)
            {
                quadMinX = ImSlugMin(quadMinX, pos[i].x);
                quadMaxX = ImSlugMax(quadMaxX, pos[i].x);
                quadMinY = ImSlugMin(quadMinY, pos[i].y);
                quadMaxY = ImSlugMax(quadMaxY, pos[i].y);
            }

            const float boxW = slab->X1 - slab->X0;
            const float boxH = slab->Y1 - slab->Y0;
            if (!(boxW > 0.0f) || !(boxH > 0.0f) || !(slab->PixelsPerEm > 0.0f))
                return false;

            float screenX[4], screenY[4];
            for (int i = 0; i < 4; ++i)
            {
                screenX[i] = pos[i].x;
                screenY[i] = pos[i].y;
            }

            float originX = 0.0f, slopeX = 0.0f, spanX = 0.0f;
            float originY = 0.0f, slopeY = 0.0f, spanY = 0.0f;
            // A fully collapsed fraction span (all four decoded fractions
            // bitwise equal) is not an error by itself: ImGui's fine-clip
            // interpolation of a sub-pixel sliver rounds to the corner value
            // when the encoded UV ulp exceeds the interpolated offset (large
            // slot ids). The axis then falls back to the single-corner sample
            // below, exactly like a tiny-but-nonzero span. Two cases still
            // reject the glyph:
            //   * a collapsed *quad* on that axis (ImGui's fine clip produced a
            //     zero/inverted extent there): nothing visible was drawn;
            //   * a collapsed span on *both* axes (no scale source at all).
            const bool fitX = FitAxis(fracX, screenX, 4, &originX, &slopeX, &spanX);
            const bool fitY = FitAxis(fracY, screenY, 4, &originY, &slopeY, &spanY);
            const bool extentX = quadMaxX > quadMinX;
            const bool extentY = quadMaxY > quadMinY;
            if ((!fitX && !fitY) || (!fitX && !extentX) || (!fitY && !extentY))
                return false;

            // Pixels per em from the more reliable axis (uniform text scale), then
            // the pen per axis: the fitted map when the axis has span, or a single
            // sample when the glyph was squeezed into a fraction sliver.
            const bool useFitX = fitX && (!fitY || spanX >= spanY);
            const float bestSpan = useFitX ? spanX : spanY;
            const float ppemFromX = slopeX * slab->PixelsPerEm / boxW;
            const float ppemFromY = slopeY * slab->PixelsPerEm / boxH;
            // A sliver-scale span (below kSpanReliable) is dominated by the float
            // quantization of the decoded fractions: the fitted slope can be off
            // by ~10 %, which then scales the whole placement. The baked ppem is
            // the better estimate there - the drawn size differs from the bake
            // size only by ImGui's integer rounding of the bake size (<= ~1.5 %,
            // ImGui::GetRoundedFontSize), and the single-sample pen below then
            // maps the corner exactly.
            const float pixelsPerEm = bestSpan >= kSpanReliable ? (useFitX ? ppemFromX : ppemFromY) : slab->PixelsPerEm;
            if (!(pixelsPerEm > 0.0f) || !isfinite(pixelsPerEm))
                return false;

            // frac(emX) = (emX * ppemBaked - X0) / boxW for X, and for Y the
            // baked box is line-top relative while em is baseline relative:
            //   emY = (Ascent - localY) / ppemBaked,  fracY = (localY - Y0) / boxH
            const float ppemBaked = slab->PixelsPerEm;
            const float fracAtEmXZero = -slab->X0 / boxW;
            const float fracAtEmYZero = (slab->Ascent - slab->Y0) / boxH;
            float penX, penY;
            if (fitX && spanX >= kSpanReliable)
                penX = originX + slopeX * fracAtEmXZero;
            else
                penX = pos[0].x - ((slab->X0 + fracX[0] * boxW) / ppemBaked) * pixelsPerEm;
            if (fitY && spanY >= kSpanReliable)
                penY = originY + slopeY * fracAtEmYZero;
            else
            {
                // penY = the screen y of emY = 0 (the baseline):
                // screenY(localY) = penY - (Ascent - localY) * scale.
                const float localY = slab->Y0 + fracY[0] * boxH;
                penY = pos[0].y + (slab->Ascent - localY) * (pixelsPerEm / ppemBaked);
            }

            if (!isfinite(penX) || !isfinite(penY))
                return false;

            // The reconstructed (unclipped) box must contain the quad ImGui drew.
            const float minX = penX + record->MinX * pixelsPerEm;
            const float maxX = penX + record->MaxX * pixelsPerEm;
            const float minY = penY - record->MaxY * pixelsPerEm;
            const float maxY = penY - record->MinY * pixelsPerEm;
            if (minX > quadMinX + kPlacementSlack || maxX < quadMaxX - kPlacementSlack ||
                minY > quadMinY + kPlacementSlack || maxY < quadMaxY - kPlacementSlack)
                return false;

            *outPenX = penX;
            *outPenY = penY;
            *outPixelsPerEm = pixelsPerEm;
            return true;
        }

        // R3 fallback for Config::EnableFineClipReconstruction == false: place
        // the glyph by mapping its em box into the quad ImGui actually drew
        // (the CPU fine clip already cropped that quad), with a uniform scale
        // so the whole glyph fits inside the quad and is centered in it. Ink
        // therefore never leaves the fine-clip rectangle even without the run
        // scissor, at the documented cost of a visibly squeezed glyph near a
        // clip edge. Unclipped quads take the exact reconstruction path in
        // EmitGlyph, so the fallback only affects cropped glyphs.
        bool PlaceGlyphFromQuad(const ImSlugGlyphSlab* slab, const ImVec2 pos[4], float* outPenX, float* outPenY,
                                float* outPixelsPerEm)
        {
            const ImSlugGlyphRecord* record = slab->Record;
            float quadL = pos[0].x, quadR = pos[0].x, quadT = pos[0].y, quadB = pos[0].y;
            for (int i = 1; i < 4; ++i)
            {
                quadL = ImSlugMin(quadL, pos[i].x);
                quadR = ImSlugMax(quadR, pos[i].x);
                quadT = ImSlugMin(quadT, pos[i].y);
                quadB = ImSlugMax(quadB, pos[i].y);
            }

            const float emWidth = record->MaxX - record->MinX;
            const float emHeight = record->MaxY - record->MinY;
            const float quadWidth = quadR - quadL;
            const float quadHeight = quadB - quadT;
            if (!(emWidth > 0.0f) || !(emHeight > 0.0f) || !(quadWidth > 0.0f) || !(quadHeight > 0.0f))
                return false;

            const float pixelsPerEm = ImSlugMin(quadWidth / emWidth, quadHeight / emHeight);
            if (!(pixelsPerEm > 0.0f) || !isfinite(pixelsPerEm))
                return false;

            const float emCenterX = 0.5f * (record->MinX + record->MaxX);
            const float emCenterY = 0.5f * (record->MinY + record->MaxY);
            const float penX = 0.5f * (quadL + quadR) - emCenterX * pixelsPerEm;
            const float penY = 0.5f * (quadT + quadB) + emCenterY * pixelsPerEm;
            if (!isfinite(penX) || !isfinite(penY))
                return false;

            *outPenX = penX;
            *outPenY = penY;
            *outPixelsPerEm = pixelsPerEm;
            return true;
        }

        // ---------------------------------------------------------------------
        // Command rewriting
        // ---------------------------------------------------------------------
        struct RewriteState
        {
            ImDrawList* List;
            int OutCount;    // written commands in CmdScratch
            bool OutFailed;  // CmdScratch growth failed: abort without touching the list
            bool RunOpen;
            ImSlugBatch* Batch;                // live run being built
            float ClipL, ClipT, ClipR, ClipB;  // pinned fine-clip edges (+-1e30 = unknown)

            // Grows CmdScratch before every write. A single source command can
            // produce an unbounded number of output commands (N alternating
            // text/geometry runs -> ~3*N commands), so no fixed multiple of the
            // input command count is a bound; EnsureCapacity's doubling makes
            // the per-write growth amortized O(1). On failure the rewrite is
            // abandoned before anything is copied back (the draw list stays
            // untouched, matching the existing out-of-memory behavior).
            bool ReserveOut(int count)
            {
                if (OutFailed)
                    return false;
                if (!EnsureCapacity((void**)&g_Core.CmdScratch, &g_Core.CmdScratchCapacity, OutCount + count,
                                    sizeof(ImDrawCmd)))
                    OutFailed = true;
                return !OutFailed;
            }

            bool FlushGeometry(const ImDrawCmd& src, int firstTriangle, int triangleCount)
            {
                if (triangleCount <= 0)
                    return true;
                if (!ReserveOut(1))
                    return false;
                ImDrawCmd cmd = src;
                cmd.IdxOffset = src.IdxOffset + (unsigned int)firstTriangle * 3u;
                cmd.ElemCount = (unsigned int)triangleCount * 3u;
                g_Core.CmdScratch[OutCount++] = cmd;
                return true;
            }

            bool CloseRun(const ImDrawCmd& src)
            {
                if (!RunOpen)
                    return true;
                if (!ReserveOut(2))
                    return false;
                // The run's scissor is the union of the cropped quads (exactly
                // what ImGui's CPU fine clip produced), never wider than the
                // command's own clip rect (window clipping stays GPU-side).
                Batch->ClipMinX = ImSlugMax(Batch->ClipMinX, src.ClipRect.x);
                Batch->ClipMinY = ImSlugMax(Batch->ClipMinY, src.ClipRect.y);
                Batch->ClipMaxX = ImSlugMin(Batch->ClipMaxX, src.ClipRect.z);
                Batch->ClipMaxY = ImSlugMin(Batch->ClipMaxY, src.ClipRect.w);
                // Callback command: 0 elements, the batch as user data, clip and
                // texture kept as ImGui had them (informational; the backend
                // dispatches the callback before touching either).
                ImDrawCmd callbackCmd = src;
                callbackCmd.ElemCount = 0;
                callbackCmd.UserCallback = &ImSlug::DrawCallback;
                callbackCmd.UserCallbackData = Batch;
                callbackCmd.UserCallbackDataSize = 0;
                callbackCmd.UserCallbackDataOffset = -1;
                g_Core.CmdScratch[OutCount++] = callbackCmd;

                // Immediately after, re-establish the stock render state (R5).
                ImDrawCmd resetCmd;
                resetCmd.ClipRect = src.ClipRect;
                resetCmd.TexRef = src.TexRef;
                resetCmd.IdxOffset = src.IdxOffset + src.ElemCount;  // past the run's indices
                resetCmd.UserCallback = ImGui::GetPlatformIO().DrawCallback_ResetRenderState;
                resetCmd.UserCallbackData = nullptr;
                resetCmd.UserCallbackDataSize = 0;
                resetCmd.UserCallbackDataOffset = -1;
                g_Core.CmdScratch[OutCount++] = resetCmd;

                RunOpen = false;
                Batch = nullptr;
                ClipL = ClipT = -1.0e30f;
                ClipR = ClipB = 1.0e30f;
                return true;
            }
        };

        // Emits one glyph instance for the completed quad at triangle `tri`.
        // Returns true when an instance was appended. `outRestart` is set when the
        // glyph's fine clip is inconsistent with the open run: the caller closes the
        // run and calls again (the second call opens a fresh run).
        bool EmitGlyph(RewriteState& state, const ImDrawVert* verts, unsigned int vtxOffset, const ImDrawIdx* indices,
                       int tri, bool* outRestart)
        {
            *outRestart = false;

            // Block: (m, m+1, m+2, m, m+2, m+3) with m = the minimum index.
            const unsigned int i0 = indices[tri * 3 + 0];
            const unsigned int i1 = indices[tri * 3 + 1];
            const unsigned int i2 = indices[tri * 3 + 2];
            const unsigned int i3 = indices[tri * 3 + 3];
            const unsigned int i4 = indices[tri * 3 + 4];
            const unsigned int i5 = indices[tri * 3 + 5];
            const unsigned int m = ImSlugMin(ImSlugMin(i0, i1), ImSlugMin(i2, ImSlugMin(i3, ImSlugMin(i4, i5))));
            if (i0 != m || i1 != m + 1 || i2 != m + 2 || i3 != m || i4 != m + 2 || i5 != m + 3)
                return false;  // not the ImGui text quad pattern: treat as geometry

            ImSlugDecodedVertex decoded[4];
            ImVec2 pos[4];
            float fracX[4], fracY[4];
            for (int c = 0; c < 4; ++c)
            {
                const ImDrawVert& v = verts[vtxOffset + m + (unsigned int)c];
                if (!ImSlugAdapterDecodeVertex(v.uv.x, v.uv.y, &decoded[c]))
                    return false;
                pos[c] = v.pos;
                fracX[c] = decoded[c].FracX;
                fracY[c] = decoded[c].FracY;
            }
            // All four corners must belong to the same slab (identity is also
            // cross-checked between the two axes inside the decoder).
            const ImSlugGlyphSlab* slab = decoded[0].Slab;
            for (int c = 1; c < 4; ++c)
                if (decoded[c].Slab != slab)
                    return false;

            // Which sides did ImGui's CPU fine clip crop? A corner fraction in the
            // open (0, 1) range means the edge moved inward.
            const float fracL = 0.5f * (fracX[0] + fracX[3]);
            const float fracR = 0.5f * (fracX[1] + fracX[2]);
            const float fracT = 0.5f * (fracY[0] + fracY[1]);
            const float fracB = 0.5f * (fracY[2] + fracY[3]);
            const bool croppedL = fracL > kFracOpenEpsilon && fracL < 1.0f - kFracOpenEpsilon;
            const bool croppedR = fracR > kFracOpenEpsilon && fracR < 1.0f - kFracOpenEpsilon;
            const bool croppedT = fracT > kFracOpenEpsilon && fracT < 1.0f - kFracOpenEpsilon;
            const bool croppedB = fracB > kFracOpenEpsilon && fracB < 1.0f - kFracOpenEpsilon;

            float quadL = pos[0].x, quadR = pos[0].x, quadT = pos[0].y, quadB = pos[0].y;
            for (int c = 1; c < 4; ++c)
            {
                quadL = ImSlugMin(quadL, pos[c].x);
                quadR = ImSlugMax(quadR, pos[c].x);
                quadT = ImSlugMin(quadT, pos[c].y);
                quadB = ImSlugMax(quadB, pos[c].y);
            }

            float penX = 0.0f, penY = 0.0f, pixelsPerEm = 0.0f;
            const bool cropped = croppedL || croppedR || croppedT || croppedB;
            if (cropped && !g_Core.Cfg.EnableFineClipReconstruction)
            {
                // R3 fallback: compress the clipped glyph into the quad ImGui
                // drew (PlaceGlyphFromQuad documents the trade-off).
                if (!PlaceGlyphFromQuad(slab, pos, &penX, &penY, &pixelsPerEm))
                    return false;  // degenerate placement: drop this glyph
            }
            else if (!ReconstructPlacement(slab, pos, fracX, fracY, &penX, &penY, &pixelsPerEm))
            {
                return false;  // degenerate placement: drop this glyph
            }

            // A run shares one scissor: every glyph of the run must be consistent
            // with a single CPU fine-clip rectangle. Cropped sides pin the edge;
            // uncropped sides must stay inside an already-pinned edge.
            if (state.Batch != nullptr)
            {
                const bool consistent =
                    (!croppedL || state.ClipL <= -1.0e30f || fabsf(state.ClipL - quadL) <= kClipEdgeEpsilon) &&
                    (croppedL || state.ClipL <= quadL + kClipEdgeEpsilon) &&
                    (!croppedT || state.ClipT <= -1.0e30f || fabsf(state.ClipT - quadT) <= kClipEdgeEpsilon) &&
                    (croppedT || state.ClipT <= quadT + kClipEdgeEpsilon) &&
                    (!croppedR || state.ClipR >= 1.0e30f || fabsf(state.ClipR - quadR) <= kClipEdgeEpsilon) &&
                    (croppedR || state.ClipR >= quadR - kClipEdgeEpsilon) &&
                    (!croppedB || state.ClipB >= 1.0e30f || fabsf(state.ClipB - quadB) <= kClipEdgeEpsilon) &&
                    (croppedB || state.ClipB >= quadB - kClipEdgeEpsilon);
                if (!consistent)
                {
                    *outRestart = true;
                    return false;
                }
            }

            const ImU32 col = verts[vtxOffset + m].col;
            const float tintR = (float)(col & 0xFFu) / 255.0f;
            const float tintG = (float)((col >> 8) & 0xFFu) / 255.0f;
            const float tintB = (float)((col >> 16) & 0xFFu) / 255.0f;
            const float tintA = (float)((col >> 24) & 0xFFu) / 255.0f;

            // M5: a color glyph draws one instance per layer (draw order), each
            // with the layer's paint record; the color/tint rides the instance
            // alpha only (ImGui forces RGB=255 for Colored glyphs, so layer
            // colors are authoritative and a text tint only fades them).
            const bool useColor = slab->ColorGlyph != nullptr && g_Core.Cfg.EnableColorFonts;
            const ImSlugGlyphInstance* instances = nullptr;
            int instanceCount = 0;

            if (useColor)
            {
                const int layerCount = slab->ColorGlyph->LayerCount;
                if (!EnsureCapacity((void**)&g_Core.ColorInstances, &g_Core.ColorInstancesCapacity, layerCount,
                                    sizeof(ImSlugGlyphInstance)))
                    return false;
                instanceCount = ImSlugBuildColorGlyphInstances(slab->ColorGlyph, penX, penY, pixelsPerEm, tintA,
                                                               g_Core.ColorInstances, layerCount, nullptr);
                instances = g_Core.ColorInstances;
                g_Core.Stats.ColorLayers += (uint32_t)instanceCount;
            }
            else
            {
                ImSlugGlyphInstance instance;
                ImSlugBuildGlyphInstance(slab->Record, penX, penY, pixelsPerEm, tintR, tintG, tintB, tintA, &instance);

                if (g_Core.Cfg.DebugDrawBounds)
                {
                    // Reconstruction: the instance must cover the drawn (possibly
                    // cropped) quad. Fallback: the instance must stay inside it
                    // (PlaceGlyphFromQuad compresses the em box into the quad).
                    // Color glyphs are skipped: their instances are the layer
                    // outlines, which do not need to cover the base glyph's box.
                    bool valid;
                    if (cropped && !g_Core.Cfg.EnableFineClipReconstruction)
                        valid = instance.ScreenMinX >= quadL - kPlacementSlack &&
                                instance.ScreenMaxX <= quadR + kPlacementSlack &&
                                instance.ScreenMinY >= quadT - kPlacementSlack &&
                                instance.ScreenMaxY <= quadB + kPlacementSlack;
                    else
                        valid = instance.ScreenMinX <= quadL + kPlacementSlack &&
                                instance.ScreenMaxX >= quadR - kPlacementSlack &&
                                instance.ScreenMinY <= quadT + kPlacementSlack &&
                                instance.ScreenMaxY >= quadB - kPlacementSlack;
                    if (!valid)
                        g_Core.Stats.BoundsMismatches++;
                    IMSLUG_ASSERT(valid && "imslug: reconstructed glyph bounds do not cover the source quad");
                }

                // The arena holds the instances; CommitBatches() moves them into
                // the backend ring once the whole draw data has been classified.
                ImSlugGlyphInstance singleInstance = instance;
                if (state.Batch == nullptr)
                {
                    state.Batch = AppendBatch();
                    if (state.Batch == nullptr)
                        return false;
                    state.Batch->FrameIndex = g_Core.RenderIO != nullptr ? g_Core.RenderIO->GetFrameIndex() : 0u;
                    state.Batch->ClipMinX = quadL;
                    state.Batch->ClipMinY = quadT;
                    state.Batch->ClipMaxX = quadR;
                    state.Batch->ClipMaxY = quadB;
                }
                else
                {
                    state.Batch->ClipMinX = ImSlugMin(state.Batch->ClipMinX, quadL);
                    state.Batch->ClipMinY = ImSlugMin(state.Batch->ClipMinY, quadT);
                    state.Batch->ClipMaxX = ImSlugMax(state.Batch->ClipMaxX, quadR);
                    state.Batch->ClipMaxY = ImSlugMax(state.Batch->ClipMaxY, quadB);
                }
                int firstInstance = 0;
                if (!AppendInstances(&singleInstance, 1, &firstInstance))
                    return false;
                if (state.Batch->InstanceCount == 0)
                    state.Batch->FirstInstance = (uint32_t)firstInstance;
                state.Batch->InstanceCount++;

                if (croppedL)
                    state.ClipL = quadL;
                if (croppedT)
                    state.ClipT = quadT;
                if (croppedR)
                    state.ClipR = quadR;
                if (croppedB)
                    state.ClipB = quadB;
                state.RunOpen = true;
                return true;
            }

            if (instanceCount <= 0)
                return false;  // no drawable layer: drop the glyph (not a run break)

            // Open or extend the run. The scissor union covers the drawn layers
            // (a layer's outline can extend past the base glyph's box); for a
            // fine-clipped glyph the pinned clip edges stay authoritative, so
            // the layer extents are only folded in for uncropped glyphs.
            float clipL = quadL, clipT = quadT, clipR = quadR, clipB = quadB;
            if (!cropped)
            {
                for (int i = 0; i < instanceCount; ++i)
                {
                    const ImSlugGlyphInstance& layerInstance = instances[i];
                    clipL = ImSlugMin(clipL, layerInstance.ScreenMinX);
                    clipT = ImSlugMin(clipT, layerInstance.ScreenMinY);
                    clipR = ImSlugMax(clipR, layerInstance.ScreenMaxX);
                    clipB = ImSlugMax(clipB, layerInstance.ScreenMaxY);
                }
            }
            if (state.Batch == nullptr)
            {
                state.Batch = AppendBatch();
                if (state.Batch == nullptr)
                    return false;
                state.Batch->FrameIndex = g_Core.RenderIO != nullptr ? g_Core.RenderIO->GetFrameIndex() : 0u;
                state.Batch->ClipMinX = clipL;
                state.Batch->ClipMinY = clipT;
                state.Batch->ClipMaxX = clipR;
                state.Batch->ClipMaxY = clipB;
            }
            else
            {
                state.Batch->ClipMinX = ImSlugMin(state.Batch->ClipMinX, clipL);
                state.Batch->ClipMinY = ImSlugMin(state.Batch->ClipMinY, clipT);
                state.Batch->ClipMaxX = ImSlugMax(state.Batch->ClipMaxX, clipR);
                state.Batch->ClipMaxY = ImSlugMax(state.Batch->ClipMaxY, clipB);
            }

            int firstInstance = 0;
            if (!AppendInstances(instances, instanceCount, &firstInstance))
                return false;
            if (state.Batch->InstanceCount == 0)
                state.Batch->FirstInstance = (uint32_t)firstInstance;
            state.Batch->InstanceCount += (uint32_t)instanceCount;

            // Pin the fine-clip edges this glyph revealed (the base quad's
            // clip governs every layer).
            if (croppedL)
                state.ClipL = quadL;
            if (croppedT)
                state.ClipT = quadT;
            if (croppedR)
                state.ClipR = quadR;
            if (croppedB)
                state.ClipB = quadB;
            state.RunOpen = true;
            return true;
        }

        void PreprocessDrawList(ImDrawList* list)
        {
            const int cmdCount = list->CmdBuffer.Size;
            if (cmdCount == 0)
                return;

            RewriteState state;
            state.List = list;
            state.OutCount = 0;
            state.OutFailed = false;
            state.RunOpen = false;
            state.Batch = nullptr;
            state.ClipL = state.ClipT = -1.0e30f;
            state.ClipR = state.ClipB = 1.0e30f;

            const ImDrawCmd* sourceCommands = list->CmdBuffer.Data;
            const ImDrawVert* verts = list->VtxBuffer.Data;
            const ImDrawIdx* indices = list->IdxBuffer.Data;

            for (int cmdIndex = 0; cmdIndex < cmdCount; ++cmdIndex)
            {
                const ImDrawCmd& src = sourceCommands[cmdIndex];
                if (src.UserCallback != nullptr)
                {
                    // Third-party callbacks (including already-injected reset and
                    // sampler callbacks) pass through untouched, and they break any
                    // open text run.
                    if (!state.CloseRun(src) || !state.ReserveOut(1))
                        return;  // scratch growth failed: leave the list untouched
                    g_Core.CmdScratch[state.OutCount++] = src;
                    continue;
                }

                const int triCount = (int)(src.ElemCount / 3u);
                if (triCount == 0)
                {
                    if (!state.CloseRun(src) || !state.ReserveOut(1))
                        return;
                    g_Core.CmdScratch[state.OutCount++] = src;
                    continue;
                }

                if (!EnsureCapacity((void**)&g_Core.TriActions, &g_Core.TriActionsCapacity, triCount, sizeof(uint8_t)))
                    return;

                // Classify: a triangle is a glyph candidate when its three vertices
                // decode to the same slab; the pair completes when the second
                // triangle matches the ImGui text quad pattern.
                for (int tri = 0; tri < triCount; ++tri)
                    g_Core.TriActions[tri] = TriAction_Geometry;

                for (int tri = 0; tri < triCount; ++tri)
                {
                    if (g_Core.TriActions[tri] != TriAction_Geometry)
                        continue;  // already consumed as a glyph tail
                    ImSlugDecodedVertex d0, d1, d2, e0, e1, e2;
                    const unsigned int a0 = src.VtxOffset + indices[src.IdxOffset + tri * 3 + 0];
                    const unsigned int a1 = src.VtxOffset + indices[src.IdxOffset + tri * 3 + 1];
                    const unsigned int a2 = src.VtxOffset + indices[src.IdxOffset + tri * 3 + 2];
                    const bool d0ok = ImSlugAdapterDecodeVertex(verts[a0].uv.x, verts[a0].uv.y, &d0);
                    const bool d1ok = ImSlugAdapterDecodeVertex(verts[a1].uv.x, verts[a1].uv.y, &d1);
                    const bool d2ok = ImSlugAdapterDecodeVertex(verts[a2].uv.x, verts[a2].uv.y, &d2);
                    if (!(d0ok && d1ok && d2ok))
                    {
                        // Mixed triangle (M4 fuzz): one or two corners decode as
                        // Slug handles, the rest do not - ImGui produced this,
                        // for example with a degenerate (NaN interpolating)
                        // fine clip. Neither path can draw it: geometry would
                        // sample the atlas at the magic UVs, so it is dropped
                        // instead of leaking Slug-looking vertices into the
                        // geometry stream. Fully non-Slug triangles stay
                        // geometry (an app UV can only misfire in the magic
                        // band if all three corners land there, the same
                        // narrow false-positive window as M3).
                        if (d0ok || d1ok || d2ok)
                            g_Core.TriActions[tri] = TriAction_Drop;
                        continue;
                    }
                    bool complete = false;
                    if (tri + 1 < triCount && g_Core.TriActions[tri + 1] == TriAction_Geometry)
                    {
                        const unsigned int b0 = src.VtxOffset + indices[src.IdxOffset + tri * 3 + 3];
                        const unsigned int b1 = src.VtxOffset + indices[src.IdxOffset + tri * 3 + 4];
                        const unsigned int b2 = src.VtxOffset + indices[src.IdxOffset + tri * 3 + 5];
                        bool tailDecoded = false;
                        if (ImSlugAdapterDecodeVertex(verts[b0].uv.x, verts[b0].uv.y, &e0) &&
                            ImSlugAdapterDecodeVertex(verts[b1].uv.x, verts[b1].uv.y, &e1) &&
                            ImSlugAdapterDecodeVertex(verts[b2].uv.x, verts[b2].uv.y, &e2))
                            tailDecoded = true;
                        const unsigned int m = ImSlugMin(ImSlugMin(a0, a1), a2);
                        const bool pattern = (a0 == m && a1 == m + 1 && a2 == m + 2);
                        const bool tailPattern = tailDecoded && (b0 == m && b1 == m + 2 && b2 == m + 3) &&
                                                 (e0.Slab == d0.Slab && e1.Slab == d0.Slab && e2.Slab == d0.Slab);
                        complete = pattern && tailPattern;
                        if (!complete && b0 == m && b1 == m + 2 && b2 == m + 3)
                        {
                            // This quad's tail half cannot complete the glyph:
                            // drop it too, or its Slug UVs would be drawn as
                            // geometry (the head is dropped just below).
                            g_Core.TriActions[tri + 1] = TriAction_Drop;
                        }
                    }
                    if (complete)
                    {
                        g_Core.TriActions[tri] = TriAction_Glyph;
                        g_Core.TriActions[tri + 1] = TriAction_GlyphTail;
                        tri++;
                    }
                    else
                    {
                        // Half a glyph (e.g. a command boundary mid-glyph) or a lone
                        // Slug-looking triangle: drawable through neither path, so
                        // it is dropped.
                        g_Core.TriActions[tri] = TriAction_Drop;
                    }
                }

                // Emit geometry ranges and text runs in order.
                for (int tri = 0; tri < triCount;)
                {
                    const uint8_t action = g_Core.TriActions[tri];
                    if (action == TriAction_Geometry)
                    {
                        if (!state.CloseRun(src))
                            return;
                        int end = tri;
                        while (end < triCount && g_Core.TriActions[end] == TriAction_Geometry)
                            end++;
                        if (!state.FlushGeometry(src, tri, end - tri))
                            return;
                        tri = end;
                        continue;
                    }
                    if (action == TriAction_Drop)
                    {
                        if (!state.CloseRun(src))
                            return;
                        tri++;
                        continue;
                    }

                    // TriAction_Glyph: both triangles; the tail is consumed here.
                    bool restart = false;
                    const bool emitted = EmitGlyph(state, verts, src.VtxOffset, indices + src.IdxOffset, tri, &restart);
                    if (!emitted && restart)
                    {
                        if (!state.CloseRun(src))
                            return;
                        bool restartAgain = false;
                        EmitGlyph(state, verts, src.VtxOffset, indices + src.IdxOffset, tri, &restartAgain);
                    }
                    tri += 2;
                }
                if (!state.CloseRun(src))
                    return;
            }

            if (state.OutFailed)
                return;  // should be unreachable: every write reserved first

            if (state.OutCount > 0)
            {
                ImVector<ImDrawCmd>& cmdBuffer = list->CmdBuffer;
                cmdBuffer.resize(state.OutCount);
                memcpy(cmdBuffer.Data, g_Core.CmdScratch, (size_t)state.OutCount * sizeof(ImDrawCmd));
            }
            else
            {
                // Nothing survived (defensive; keeps the list valid for backends).
                list->CmdBuffer.resize(1);
                list->CmdBuffer[0].ElemCount = 0;
                list->CmdBuffer[0].UserCallback = nullptr;
            }
        }

        void CommitBatches()
        {
            g_Core.Stats.LiveBatches = (uint32_t)g_Core.Batches.Count;
            g_Core.Stats.Instances = (uint32_t)g_Core.Instances.Count;
            if (g_Core.RenderIO == nullptr)
                return;

            // Idempotent per arena reset (imslug.h: "Idempotent per frame"): a
            // later PreprocessDrawData call in the same frame must not commit
            // the same batch twice. After the first commit, batch->FirstInstance
            // is a *ring* index, no longer an index into g_Core.Instances - a
            // second commit would memcpy from that offset (out of bounds) and
            // duplicate the ring allocation. Only batches appended after the
            // last commit (new draw commands added between calls) are committed.
            for (int i = g_Core.CommittedBatches; i < g_Core.Batches.Count; ++i)
            {
                ImSlugBatch* batch = &g_Core.Batches.Data[i];
                if (batch->InstanceCount == 0)
                    continue;
                uint32_t firstInstance = 0;
                ImSlugGlyphInstance* dst = g_Core.RenderIO->ReserveInstances(batch->InstanceCount, &firstInstance);
                if (dst == nullptr)
                {
                    // Out of instance capacity: drop the run's instances. The
                    // command structure stays valid (a callback with nothing to
                    // draw); the whole batch is skipped.
                    g_Core.Stats.DroppedRuns++;
                    IMSLUG_ASSERT(false &&
                                  "imslug: per-frame instance ring exhausted; raise Config::MaxInstancesPerFrame");
                    batch->FirstInstance = 0;
                    batch->InstanceCount = 0;
                    continue;
                }
                const int start = (int)batch->FirstInstance;
                memcpy(dst, g_Core.Instances.Data + start, (size_t)batch->InstanceCount * sizeof(ImSlugGlyphInstance));
                batch->FirstInstance = firstInstance;
            }
            g_Core.CommittedBatches = g_Core.Batches.Count;
        }
    }  // namespace

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------
    const char* GetVersion()
    {
        return IMSLUG_VERSION;
    }

    void ImSlugBuildGlyphInstance(const ImSlugGlyphRecord* record, float penX, float penY, float pixelsPerEm, float r,
                                  float g, float b, float a, ImSlugGlyphInstance* outInstance)
    {
        ImSlugBuildGlyphInstanceScaled(record, penX, penY, pixelsPerEm, pixelsPerEm, r, g, b, a, outInstance);
    }

    void ImSlugBuildGlyphInstanceScaled(const ImSlugGlyphRecord* record, float penX, float penY, float pixelsPerEmX,
                                        float pixelsPerEmY, float r, float g, float b, float a,
                                        ImSlugGlyphInstance* outInstance)
    {
        IMSLUG_ASSERT(record != nullptr && outInstance != nullptr);
        IMSLUG_ASSERT(pixelsPerEmX > 0.0f && pixelsPerEmY > 0.0f);

        const float inverseX = 1.0f / pixelsPerEmX;
        const float inverseY = 1.0f / pixelsPerEmY;

        outInstance->EmMinX = record->MinX;
        outInstance->EmMinY = record->MinY;
        outInstance->EmMaxX = record->MaxX;
        outInstance->EmMaxY = record->MaxY;

        // Screen rect (Y down): the em bbox with the Y flip applied, so the
        // screen rect corner that matches em (MinX, MinY) is (MinX, ScreenMaxY).
        outInstance->ScreenMinX = penX + record->MinX * pixelsPerEmX;
        outInstance->ScreenMinY = penY - record->MaxY * pixelsPerEmY;
        outInstance->ScreenMaxX = penX + record->MaxX * pixelsPerEmX;
        outInstance->ScreenMaxY = penY - record->MinY * pixelsPerEmY;

        // Inverse Jacobian: screen delta -> em delta (em Y up, screen Y down).
        outInstance->Jac00 = inverseX;
        outInstance->Jac01 = 0.0f;
        outInstance->Jac10 = 0.0f;
        outInstance->Jac11 = -inverseY;

        outInstance->BandScaleX = record->BandScaleX;
        outInstance->BandScaleY = record->BandScaleY;
        outInstance->BandOffsetX = record->BandOffsetX;
        outInstance->BandOffsetY = record->BandOffsetY;

        outInstance->GlyphLocX = (uint32_t)record->BandLocX;
        outInstance->GlyphLocY = (uint32_t)record->BandLocY;
        outInstance->BandMaxWord = ImSlugPackBandMaxWord(record->BandMaxX, record->BandMaxY, record->Flags);

        outInstance->ColorR = r;
        outInstance->ColorG = g;
        outInstance->ColorB = b;
        outInstance->ColorA = a;
        outInstance->PaintIndex = kNoPaintIndex;  // M5: plain glyph unless a color layer sets it
    }

    bool Init(const Config& config)
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            IMSLUG_ASSERT(false && "imslug: call ImSlug::Init() with a live ImGui context (see imslug.h)");
            return false;
        }
        if (g_Core.Initialized)
            Shutdown();

        memset(&g_Core, 0, sizeof(g_Core));
        g_Core.Cfg = config;
        ImSlugInitPools(&g_Core.Pools);

        uint32_t slabCap = config.MaxGlyphSlabs != 0 ? config.MaxGlyphSlabs : (uint32_t)kMaxGlyphSlabs;
        if (slabCap > (uint32_t)kMaxGlyphSlabs)
            slabCap = (uint32_t)kMaxGlyphSlabs;  // slots beyond the cap cannot be decoded (see imslug.h)
        g_Core.SlabCapacity = slabCap;
        g_Core.Slabs = (ImSlugGlyphSlab*)calloc(slabCap, sizeof(ImSlugGlyphSlab));
        if (g_Core.Slabs == nullptr)
            return false;

        g_Core.ArenaFrameCount = -1;
        g_Core.Initialized = true;
        ResetArena();
        return true;
    }

    void Shutdown()
    {
        if (!g_Core.Initialized)
            return;
        ImSlugAdapterShutdown();
        ImSlugDestroyPools(&g_Core.Pools);
        free(g_Core.Slabs);
        free(g_Core.FreeSlots);
        free(g_Core.Instances.Data);
        free(g_Core.Batches.Data);
        free(g_Core.CmdScratch);
        free(g_Core.TriActions);
        free(g_Core.ColorInstances);
        memset(&g_Core, 0, sizeof(g_Core));
        g_Core.ArenaFrameCount = -1;
    }

    void NewFrame()
    {
        if (!g_Core.Initialized)
            return;
        ResetArena();
        g_Core.ArenaFrameCount = ImGui::GetFrameCount();
    }

    ImFont* AddFontFromFileTTF(const char* path, float sizePixels, const ImFontConfig* fontConfig,
                               const ImWchar* glyphRanges)
    {
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        IMSLUG_ASSERT(atlas != nullptr && !atlas->Locked);
        if (path == nullptr || atlas == nullptr)
            return nullptr;
        CheckFontRegistrationOrder();

        ImFontConfig cfg;
        if (fontConfig != nullptr)
            cfg = *fontConfig;
        ImSlugAdapterPrepareFontConfig(&cfg);
        return atlas->AddFontFromFileTTF(path, sizePixels, &cfg, glyphRanges);
    }

    ImFont* AddFontFromMemoryTTF(const void* data, int dataSize, float sizePixels, const ImFontConfig* fontConfig,
                                 const ImWchar* glyphRanges)
    {
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        IMSLUG_ASSERT(atlas != nullptr && !atlas->Locked);
        if (data == nullptr || dataSize <= 0 || atlas == nullptr)
            return nullptr;
        CheckFontRegistrationOrder();

        ImFontConfig cfg;
        if (fontConfig != nullptr)
            cfg = *fontConfig;
        ImSlugAdapterPrepareFontConfig(&cfg);
        return atlas->AddFontFromMemoryTTF((void*)data, dataSize, sizePixels, &cfg, glyphRanges);
    }

    bool IsRegistered(const ImFont* font)
    {
        return font != nullptr && ImSlugAdapterIsFontRegistered(font);
    }

    bool IsPackedFontData(const void* data, int dataSize)
    {
        return ImSlugPackedIsContainer(data, dataSize);
    }

    void GetStatistics(Statistics* out)
    {
        if (out == nullptr)
            return;
        *out = g_Core.Stats;
        out->GlyphSlabs = g_Core.SlabHighWater - (uint32_t)g_Core.FreeCount;
    }

    void ImSlugSetRenderIO(const ImSlugRenderIO* renderIO)
    {
        g_Core.RenderIO = renderIO;
    }

    ImSlugPools* ImSlugGetPools()
    {
        return g_Core.Initialized ? &g_Core.Pools : nullptr;
    }

    const Config* ImSlugGetConfig()
    {
        return g_Core.Initialized ? &g_Core.Cfg : nullptr;
    }

    ImSlugGlyphSlab* ImSlugAllocGlyphSlab(const ImSlugGlyphRecord* record, const ImSlugColorGlyph* colorGlyph, float x0,
                                          float y0, float x1, float y1, float ascent, float pixelsPerEm,
                                          uint64_t bakeId, uint32_t* outSlot)
    {
        IMSLUG_ASSERT(record != nullptr && outSlot != nullptr);
        if (!g_Core.Initialized || record == nullptr)
            return nullptr;

        uint32_t slot;
        if (g_Core.FreeCount > 0)
        {
            slot = g_Core.FreeSlots[--g_Core.FreeCount];
        }
        else if (g_Core.SlabHighWater < g_Core.SlabCapacity)
        {
            slot = g_Core.SlabHighWater++;
        }
        else
        {
            return nullptr;  // cap reached: the caller falls back
        }

        ImSlugGlyphSlab* slab = &g_Core.Slabs[slot];
        memset(slab, 0, sizeof(*slab));
        slab->Record = record;
        slab->ColorGlyph = colorGlyph;
        slab->BakeId = bakeId;
        slab->X0 = x0;
        slab->Y0 = y0;
        slab->X1 = x1;
        slab->Y1 = y1;
        slab->Ascent = ascent;
        slab->PixelsPerEm = pixelsPerEm;
        slab->Slot = slot;
        slab->Alive = true;
        *outSlot = slot;
        return slab;
    }

    void ImSlugFreeGlyphSlab(uint32_t slot)
    {
        if (!g_Core.Initialized || slot >= g_Core.SlabHighWater)
            return;
        ImSlugGlyphSlab* slab = &g_Core.Slabs[slot];
        if (!slab->Alive)
            return;
        slab->Alive = false;
        slab->Record = nullptr;
        slab->ColorGlyph = nullptr;
        slab->BakeId = 0;
        if (!EnsureCapacity((void**)&g_Core.FreeSlots, &g_Core.FreeCapacity, g_Core.FreeCount + 1, sizeof(uint32_t)))
            return;  // leak one slot rather than corrupting the registry
        g_Core.FreeSlots[g_Core.FreeCount++] = slot;
    }

    void ImSlugFreeGlyphSlabsForBake(uint64_t bakeId)
    {
        if (!g_Core.Initialized)
            return;
        for (uint32_t slot = 0; slot < g_Core.SlabHighWater; ++slot)
            if (g_Core.Slabs[slot].Alive && g_Core.Slabs[slot].BakeId == bakeId)
                ImSlugFreeGlyphSlab(slot);
    }

    const ImSlugGlyphSlab* ImSlugGetGlyphSlab(uint32_t slot)
    {
        if (!g_Core.Initialized || slot >= g_Core.SlabHighWater)
            return nullptr;
        const ImSlugGlyphSlab* slab = &g_Core.Slabs[slot];
        return slab->Alive ? slab : nullptr;
    }

    void DrawCallback(const ImDrawList* parentList, const ImDrawCmd* cmd)
    {
        IM_UNUSED(parentList);
        if (cmd == nullptr || cmd->UserCallbackData == nullptr)
            return;
        const ImSlugBatch* batch = (const ImSlugBatch*)cmd->UserCallbackData;
        if (batch->Magic != kBatchMagic || batch->InstanceCount == 0)
            return;
        if (g_Core.RenderIO == nullptr || g_Core.RenderIO->DrawBatch == nullptr)
            return;
        g_Core.RenderIO->DrawBatch(batch);
    }

    void PreprocessDrawData(ImDrawData* drawData)
    {
        if (drawData == nullptr || !g_Core.Initialized || !g_Core.Cfg.EnableSlugForRegisteredFonts)
            return;

        if ((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasTextures) == 0)
        {
            if (!g_Core.WarnedNoTextures)
            {
                g_Core.WarnedNoTextures = true;
                fprintf(stderr,
                        "[imslug] backend lacks ImGuiBackendFlags_RendererHasTextures; Slug text rendering is disabled "
                        "(imslug requires the ImGui 1.92 dynamic font atlas).\n");
            }
            IMSLUG_ASSERT(false && "imslug requires a renderer with ImGuiBackendFlags_RendererHasTextures");
            return;
        }
        if (ImGui::GetPlatformIO().DrawCallback_ResetRenderState == nullptr)
        {
            if (!g_Core.WarnedNoReset)
            {
                g_Core.WarnedNoReset = true;
                fprintf(stderr,
                        "[imslug] backend does not provide PlatformIO::DrawCallback_ResetRenderState; Slug text "
                        "rendering is disabled (see imslug-vulkan-backend skill, R5).\n");
            }
            IMSLUG_ASSERT(false && "imslug requires PlatformIO::DrawCallback_ResetRenderState");
            return;
        }
        if (g_Core.RenderIO == nullptr)
        {
            if (!g_Core.WarnedNoBackend)
            {
                g_Core.WarnedNoBackend = true;
                fprintf(stderr,
                        "[imslug] no backend registered (ImSlug_ImplVulkan_Init?); draw data left untouched.\n");
            }
            IMSLUG_ASSERT(false && "imslug: no backend module registered (call ImSlug_ImplVulkan_Init)");
            return;
        }

        NewFrameIfNeeded();
        for (ImDrawList* list : drawData->CmdLists)
            if (list != nullptr)
                PreprocessDrawList(list);
        CommitBatches();
    }
}  // namespace ImSlug
