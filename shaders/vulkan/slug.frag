// ============================================================================
// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// slug.frag — Vulkan GLSL pixel shader (source of truth; compiled by
// scripts/gen_shaders.* into the committed SPIR-V header).
//
// Slug shader code Copyright 2017 by Eric Lengyel.
//
// This file is a port/adaptation of the reference Slug pixel shader
// (https://github.com/EricLengyel/Slug, SlugPixelShader.hlsl; dual-licensed
// MIT OR Apache-2.0, both texts in NOTICE section 1): CalcRootCode,
// SolveHorizPoly/SolveVertPoly, CalcBandLoc, CalcCoverage and the banded
// SlugRender walk, function for function. The auditable mapping table lives in
// shaders/reference/slug.hlsl.
//
// Texture formats (imslug_internal.h): curve pool RGBA16F, band pool R16G16_UINT
// (the reference's uint4 band texel carries the same two u16 fields in .xy).
// Both are read with texelFetch: nearest sampling, no filtering, manual
// row-wrap only through CalcBandLoc (the CPU packer guarantees lists never
// straddle a row).
// ============================================================================

#version 450

precision highp float;
precision highp int;

layout(location = 0) in vec2 vTexCoord;      // em-space sample coordinate
layout(location = 1) flat in vec4 vBanding;  // (bandScaleX, bandScaleY, bandOffsetX, bandOffsetY)
layout(location = 2) flat in uvec2 vGlyphLoc;
layout(location = 3) flat in uint vBandMaxWord;  // bandMaxX | bandMaxY << 16 | E << 28
layout(location = 4) in vec4 vColor;
layout(location = 5) in vec2 vScreenPos;         // screen pixels (Y down), undilated (debug)
layout(location = 6) flat in vec4 vScreenRect;   // source quad (debug bounds)
layout(location = 7) flat in uint vPaintIndex;   // M5: paint record index, 0xFFFFFFFF = plain

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D curveTexture;  // RGBA16F control points
layout(set = 0, binding = 1) uniform usampler2D bandTexture;  // R16G16_UINT headers/refs

// M5: one paint record per color layer (ImSlugPaintRecord in imslug_internal.h,
// 352 bytes, std430). The instance's vPaintIndex selects one; plain glyphs use
// 0xFFFFFFFF and never touch the buffer.
//   Header.x Type (0 solid, 1 linear, 2 radial, 3 sweep)
//   Header.y Extend (0 pad, 1 repeat, 2 reflect)
//   Header.z StopCount
//   Header.w SolidColor (RGBA8: r | g<<8 | b<<16 | a<<24)
//   Xform0/1: record em -> paint-local affine rows (a, b, tx, -) / (c, d, ty, -)
//   Geo0/Geo1: linear (p0.xy, p1.xy) + (p2.xy, -, -); radial (c0.xy, r0, -) +
//              (c1.xy, r1, -); sweep (center.xy, startDeg, endDeg)
//   Clip: record-space ClipBox (minX, minY, maxX, maxY; disabled when max <= min)
//   Stops: two u32 per stop: .x = raw F2Dot14 offset, .y = RGBA8 color
struct SlugPaintRecord
{
    uvec4 Header;
    vec4 Xform0;
    vec4 Xform1;
    vec4 Geo0;
    vec4 Geo1;
    vec4 Clip;
    uvec2 Stops[32];
};

layout(std430, set = 0, binding = 2) readonly buffer SlugPaintBuffer
{
    SlugPaintRecord Paints[];
};

const uint kNoPaintIndex = 0xFFFFFFFFu;
const int kMaxPaintStops = 32;
const float kPaintTiny = 1.0e-12;

// The push constant block mirrors shaders/vulkan/slug.vert; only the debug
// flags are read here, but the offsets must stay in sync (Viewport at 64).
layout(push_constant) uniform PushConstants
{
    vec4 MVP0;
    vec4 MVP1;
    vec4 MVP2;
    vec4 MVP3;
    vec2 Viewport;
    uint DebugFlags;
} pc;

const int kLogBandTextureWidth = 12;  // fixed pool width 4096 (imslug_internal.h)
const float kSolverEpsilon = 1.0 / 65536.0;

// M4 debug overlay flags (mirror ImSlugVulkanDebugOverlay in
// backends/imslug_impl_vulkan.h). RefHeat takes precedence over BandGrid.
const uint kDebugFlagGlyphBounds = 1u;  // 1px outline of the drawn quad
const uint kDebugFlagBandGrid = 2u;     // H/V band checkerboard per glyph
const uint kDebugFlagRefHeat = 4u;      // band reference walk cost heatmap
const float kRefHeatScale = 16.0;       // refs walked -> [0,1] heat

// Reference CalcRootCode(): root eligibility code from the signs of the three
// control-point y values. Eligibility is returned in bits 0 and 8.
uint CalcRootCode(float y1, float y2, float y3)
{
    uint i1 = floatBitsToUint(y1) >> 31U;
    uint i2 = floatBitsToUint(y2) >> 30U;
    uint i3 = floatBitsToUint(y3) >> 29U;

    uint shift = (i2 & 2U) | (i1 & ~2U);
    shift = (i3 & 4U) | (shift & ~4U);

    return ((0x2E74U >> shift) & 0x0101U);
}

// Reference SolveHorizPoly(): solve for the t where the sample-relative
// quadratic y(t) = a t^2 - 2b t + c crosses y = 0, then return the x
// coordinates where C(t) = 0. The nearly-linear branch (|a| < 1/65536) also
// guards b like the CPU arbiter (imslug_refraster.cpp, Slughorn's adaptation):
// when |b| < 1/65536 it returns t = 0 instead of the reference's c * (0.5/0)
// inf/NaN. The guard is reachable because the packer's axis-parallel exclusion
// runs on the fp32 outline while the GPU reads fp16 pool texels: quantization
// can make an admitted curve all-equal on one axis. Keeping the GPU exactly on
// the CPU side of this branch is what the goldens compare; see R1 in the M2
// review notes.
vec2 SolveHorizPoly(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.y;

    float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
    float t1 = (b.y - d) * ra;
    float t2 = (b.y + d) * ra;

    if (abs(a.y) < kSolverEpsilon)
        t1 = t2 = (abs(b.y) >= kSolverEpsilon) ? p12.y * (0.5 / b.y) : 0.0;

    return vec2((a.x * t1 - b.x * 2.0) * t1 + p12.x, (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

// Reference SolveVertPoly(): same, crossing x = 0.
vec2 SolveVertPoly(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.x;

    float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
    float t1 = (b.x - d) * ra;
    float t2 = (b.x + d) * ra;

    if (abs(a.x) < kSolverEpsilon)
        t1 = t2 = (abs(b.x) >= kSolverEpsilon) ? p12.x * (0.5 / b.x) : 0.0;

    return vec2((a.y * t1 - b.y * 2.0) * t1 + p12.y, (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

// Reference CalcBandLoc(): wrap the u16 list offset to the next row when it
// exceeds the fixed 4096-texel width.
ivec2 CalcBandLoc(ivec2 glyphLoc, uint offset)
{
    ivec2 bandLoc = ivec2(glyphLoc.x + int(offset), glyphLoc.y);
    bandLoc.y += bandLoc.x >> kLogBandTextureWidth;
    bandLoc.x &= (1 << kLogBandTextureWidth) - 1;
    return bandLoc;
}

// Reference CalcCoverage(): weighted mean of the two ray coverages vs the
// weaker ray, then the fill rule. The E flag lives at bit 28 of the packed
// bandMax word (= bit 12 of its high 16-bit half, the reference's
// glyphData.w test; see PLAN section 11 D8(g)).
float CalcCoverage(float xcov, float ycov, float xwgt, float ywgt, uint bandMaxWord)
{
    float coverage = max(abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, kSolverEpsilon),
                         min(abs(xcov), abs(ycov)));

    if ((bandMaxWord & 0x10000000u) == 0u)
    {
        coverage = clamp(coverage, 0.0, 1.0);  // nonzero fill rule (saturate)
    }
    else
    {
        coverage = 1.0 - abs(1.0 - fract(coverage * 0.5) * 2.0);  // even-odd fill rule
    }
    return coverage;
}

// ============================================================================
// M5 color layer evaluation (mirror of ImSlugSamplePaintRecord in
// imslug_colr.cpp; the CPU mirror is the arbiter the goldens compare against).
// Only reached when the instance carries a valid PaintIndex, so color-free
// text never enters this code (the caller's branch is uniform per instance).
// ============================================================================
float PaintStopOffset(uvec2 stop)
{
    int raw = int(stop.x & 0xFFFFu);
    if (raw >= 0x8000)
        raw -= 0x10000;  // F2DOT14 sign extension
    return float(raw) * (1.0 / 16384.0);
}

vec4 UnpackPaintColor(uint packed)
{
    return vec4(float(packed & 0xFFu), float((packed >> 8) & 0xFFu), float((packed >> 16) & 0xFFu),
                float((packed >> 24) & 0xFFu)) * (1.0 / 255.0);
}

float ExtendPaintParameter(float t, uint extend)
{
    if (extend == 1u)
        return t - floor(t);  // repeat
    if (extend == 2u)
    {
        float h = t * 0.5;
        return 1.0 - abs((h - floor(h)) * 2.0 - 1.0);  // reflect
    }
    return clamp(t, 0.0, 1.0);  // pad
}

vec4 SamplePaintColorLine(uint index, float t)
{
    uint count = Paints[index].Header.z;
    if (count == 0u)
        return vec4(0.0);
    uint upper = 1u;
    while (upper < count && PaintStopOffset(Paints[index].Stops[upper]) < t)
        ++upper;
    if (upper >= count)
        return UnpackPaintColor(Paints[index].Stops[count - 1u].y);
    uint lower = upper - 1u;
    float t0 = PaintStopOffset(Paints[index].Stops[lower]);
    float t1 = PaintStopOffset(Paints[index].Stops[upper]);
    float f = (t1 > t0) ? clamp((t - t0) / (t1 - t0), 0.0, 1.0) : 0.0;
    return mix(UnpackPaintColor(Paints[index].Stops[lower].y), UnpackPaintColor(Paints[index].Stops[upper].y), f);
}

// Reads fields straight from the buffer (no by-value struct copy: the 352-byte
// record would otherwise occupy a large register block in every fragment,
// including the color-free path's shader).
vec4 SamplePaint(uint index, vec2 emCoord)
{
    // ClipBox (record em space); disabled when max <= min.
    vec4 clip = Paints[index].Clip;
    if (clip.z > clip.x && clip.w > clip.y)
    {
        if (emCoord.x < clip.x || emCoord.x > clip.z || emCoord.y < clip.y || emCoord.y > clip.w)
            return vec4(0.0);
    }

    uint type = Paints[index].Header.x;
    if (type == 0u)  // solid
        return UnpackPaintColor(Paints[index].Header.w);

    vec4 xform0 = Paints[index].Xform0;
    vec4 xform1 = Paints[index].Xform1;
    float localX = xform0.x * emCoord.x + xform0.y * emCoord.y + xform0.z;
    float localY = xform1.x * emCoord.x + xform1.y * emCoord.y + xform1.z;

    float t = 0.0;
    if (type == 1u)  // linear: iso-color lines parallel to line p0p2
    {
        vec2 p0 = Paints[index].Geo0.xy;
        vec2 p1 = Paints[index].Geo0.zw;
        vec2 p2 = Paints[index].Geo1.xy;
        vec2 e1 = p1 - p0;
        vec2 e2 = p2 - p0;
        float cross = e1.x * e2.y - e1.y * e2.x;
        float scale = (abs(e1.x) + abs(e1.y)) * (abs(e2.x) + abs(e2.y));
        if ((e1.x == 0.0 && e1.y == 0.0) || (e2.x == 0.0 && e2.y == 0.0) || abs(cross) < kPaintTiny * scale)
            return vec4(0.0);  // ill-formed per the spec: not rendered
        vec2 d = vec2(localX, localY) - p0;
        t = (d.x * e2.y - d.y * e2.x) / cross;
    }
    else if (type == 2u)  // two-point conical (WHATWG radial algorithm)
    {
        vec2 c0 = Paints[index].Geo0.xy;
        float r0 = Paints[index].Geo0.z;
        vec2 c1 = Paints[index].Geo1.xy;
        float r1 = Paints[index].Geo1.z;
        vec2 d = c1 - c0;
        float dr = r1 - r0;
        vec2 q = vec2(localX, localY) - c0;
        float a = dot(d, d) - dr * dr;
        float b = -2.0 * (dot(q, d) + r0 * dr);
        float c = dot(q, q) - r0 * r0;
        bool found = false;
        float best = 0.0;
        if (abs(a) <= 1.0e-9 * (dot(d, d) + dr * dr + 1.0))
        {
            if (abs(b) > 1.0e-20)
            {
                best = -c / b;
                found = r0 + best * dr > 0.0;
            }
        }
        else
        {
            float disc = b * b - 4.0 * a * c;
            if (disc >= 0.0)
            {
                float root = sqrt(disc);
                float candidate0 = (-b + root) / (2.0 * a);
                float candidate1 = (-b - root) / (2.0 * a);
                for (int i = 0; i < 2; ++i)
                {
                    float candidate = (i == 0) ? candidate0 : candidate1;
                    if (r0 + candidate * dr <= 0.0)
                        continue;  // the algorithm draws circles with r(omega) > 0
                    if (!found || candidate > best)
                    {
                        best = candidate;
                        found = true;
                    }
                }
            }
        }
        if (!found)
            return vec4(0.0);  // outside the cone / both radii zero
        t = best;
    }
    else  // sweep: counter-clockwise degrees from +x
    {
        vec2 center = Paints[index].Geo0.xy;
        float startAngle = Paints[index].Geo0.z;
        float endAngle = Paints[index].Geo0.w;
        float span = endAngle - startAngle;
        if (abs(span) < 1.0e-4)
        {
            if (Paints[index].Header.y != 0u)
                return vec4(0.0);  // repeat/reflect with equal angles: nothing
            t = 0.0;
        }
        else
        {
            float theta = degrees(atan(localY - center.y, localX - center.x));
            if (theta < 0.0)
                theta += 360.0;
            t = (theta - startAngle) / span;
        }
    }

    t = ExtendPaintParameter(t, Paints[index].Header.y);
    return SamplePaintColorLine(index, t);
}

void main()
{
    // Reference SlugRender(): effective pixel size of the em square from
    // texcoord derivatives (the vertex shader keeps the mapping affine, so
    // this is exactly the instance's pixels-per-em on both axes).
    vec2 emsPerPixel = fwidth(vTexCoord);
    vec2 pixelsPerEm = 1.0 / emsPerPixel;
    ivec2 bandMax = ivec2(int(vBandMaxWord & 0xFFFFu), int((vBandMaxWord >> 16) & 0xFFu));
    ivec2 glyphLoc = ivec2(vGlyphLoc);

    // Band indexes from the em coordinate; clamp to [0, bandMax].
    ivec2 bandIndex = clamp(ivec2(vTexCoord * vBanding.xy + vBanding.zw), ivec2(0), bandMax);

    float xcov = 0.0;
    float xwgt = 0.0;
    uint refCount = 0u;  // debug overlay: band references whose curve was fetched

    // Horizontal band header: (curveCount, offset), then the reference list.
    // (texelFetch on a usampler2D returns uvec4; the R16G16_UINT pool carries
    // the two fields in .xy, like the reference's uint4 band texel.)
    uvec2 hbandData = texelFetch(bandTexture, ivec2(glyphLoc.x + bandIndex.y, glyphLoc.y), 0).xy;
    ivec2 hbandLoc = CalcBandLoc(glyphLoc, hbandData.y);
    for (int curveIndex = 0; curveIndex < int(hbandData.x); ++curveIndex)
    {
        ivec2 curveLoc = ivec2(texelFetch(bandTexture, ivec2(hbandLoc.x + curveIndex, hbandLoc.y), 0).xy);
        ++refCount;

        // Make the control points sample-relative.
        vec4 p12 = texelFetch(curveTexture, curveLoc, 0) - vec4(vTexCoord, vTexCoord);
        vec2 p3 = texelFetch(curveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - vTexCoord;

        // Curves are sorted descending by max x: once the largest control
        // point is more than half a pixel left of the sample, stop.
        if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5)
            break;

        uint code = CalcRootCode(p12.y, p12.w, p3.y);
        if (code != 0U)
        {
            vec2 r = SolveHorizPoly(p12, p3) * pixelsPerEm.x;

            if ((code & 1U) != 0U)
            {
                xcov += clamp(r.x + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1U)
            {
                xcov -= clamp(r.y + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    float ycov = 0.0;
    float ywgt = 0.0;

    // Vertical band header follows all horizontal ones (bandMax.y + 1 + index).
    uvec2 vbandData = texelFetch(bandTexture, ivec2(glyphLoc.x + bandMax.y + 1 + bandIndex.x, glyphLoc.y), 0).xy;
    ivec2 vbandLoc = CalcBandLoc(glyphLoc, vbandData.y);
    for (int curveIndex = 0; curveIndex < int(vbandData.x); ++curveIndex)
    {
        ivec2 curveLoc = ivec2(texelFetch(bandTexture, ivec2(vbandLoc.x + curveIndex, vbandLoc.y), 0).xy);
        ++refCount;
        vec4 p12 = texelFetch(curveTexture, curveLoc, 0) - vec4(vTexCoord, vTexCoord);
        vec2 p3 = texelFetch(curveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - vTexCoord;

        // Sorted descending by max y.
        if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5)
            break;

        uint code = CalcRootCode(p12.x, p12.z, p3.x);
        if (code != 0U)
        {
            vec2 r = SolveVertPoly(p12, p3) * pixelsPerEm.y;

            if ((code & 1U) != 0U)
            {
                ycov -= clamp(r.x + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1U)
            {
                ycov += clamp(r.y + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    float fill = CalcCoverage(xcov, ycov, xwgt, ywgt, vBandMaxWord);

    // M5: a color layer resolves its own straight-alpha color (solid or
    // gradient); the instance color then only contributes the tint alpha
    // (ImGui forces RGB=255 for Colored glyphs, so a tint fades the layer
    // colors instead of recoloring them). Plain glyphs keep the M2 path:
    // paintColor = 1, so glyphColor/glyphAlpha are exactly vColor.
    const bool isColored = vPaintIndex != kNoPaintIndex;
    vec4 paintColor = isColored ? SamplePaint(vPaintIndex, vTexCoord) : vec4(1.0);
    vec3 glyphColor = isColored ? paintColor.rgb : vColor.rgb;
    float glyphAlpha = vColor.a * paintColor.a;

    // M4 debug overlay: rebuild the fragment color for the requested views.
    // Debug views never discard and keep the whole dilated quad visible, so
    // the band/ref diagnostics show up outside the glyph body as well.
    if (pc.DebugFlags != 0u)
    {
        vec4 debugColor = vec4(glyphColor, glyphAlpha * fill);
        if ((pc.DebugFlags & kDebugFlagRefHeat) != 0u)
        {
            // Walk cost of this pixel: blue (0 refs) -> red (>= kRefHeatScale).
            const float heat = clamp(float(refCount) / kRefHeatScale, 0.0, 1.0);
            debugColor = vec4(heat, 0.5 - abs(heat - 0.5), 1.0 - heat, 0.55 + 0.45 * fill);
        }
        else if ((pc.DebugFlags & kDebugFlagBandGrid) != 0u)
        {
            // Checkerboard of the glyph's H/V band grid: alternating cell
            // parity picks the tint, so every band is visibly distinct.
            const float checker = float(((bandIndex.x >> 1) + bandIndex.y) & 1);
            debugColor.rgb =
                mix(debugColor.rgb, mix(vec3(0.12, 0.45, 1.0), vec3(1.0, 0.85, 0.15), checker), 0.6);
            debugColor.a = 0.45 + 0.55 * fill;
        }
        if ((pc.DebugFlags & kDebugFlagGlyphBounds) != 0u)
        {
            // Distance to the nearest source-quad edge, in screen pixels; the
            // position is the undilated one, so the outline sits exactly on the
            // quad ImGui asked the run to draw (the DebugDrawBounds source).
            const vec2 edgeDistance = min(vScreenPos - vScreenRect.xy, vScreenRect.zw - vScreenPos);
            if (min(edgeDistance.x, edgeDistance.y) < 1.0)
                debugColor = vec4(1.0, 0.45, 0.05, 1.0);
        }
        outColor = debugColor;
        return;
    }

    // Perf guard from the port checklist: nothing to blend below 0.001.
    if (fill < 0.001)
        discard;

    // Unpremultiplied output: the pipeline blend is stock ImGui's
    // src-alpha / one-minus-src-alpha, so the coverage scales the alpha only
    // (the reference returns color * coverage, which assumes a premultiplied
    // blend state; using it here would square the edge coverage).
    outColor = vec4(glyphColor, glyphAlpha * fill);
}
