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

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D curveTexture;  // RGBA16F control points
layout(set = 0, binding = 1) uniform usampler2D bandTexture;  // R16G16_UINT headers/refs

const int kLogBandTextureWidth = 12;  // fixed pool width 4096 (imslug_internal.h)
const float kSolverEpsilon = 1.0 / 65536.0;

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
// coordinates where C(t) = 0. The nearly-linear branch (|a| < 1/65536)
// overwrites the roots like the reference (the CPU rasterizer's extra |b|
// guard covers the exactly-degenerate case; band generation excludes
// axis-parallel curves from the matching-axis bands, so the GPU never sees it).
vec2 SolveHorizPoly(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.y;
    float rb = 0.5 / b.y;

    float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
    float t1 = (b.y - d) * ra;
    float t2 = (b.y + d) * ra;

    if (abs(a.y) < kSolverEpsilon)
        t1 = t2 = p12.y * rb;

    return vec2((a.x * t1 - b.x * 2.0) * t1 + p12.x, (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

// Reference SolveVertPoly(): same, crossing x = 0.
vec2 SolveVertPoly(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.x;
    float rb = 0.5 / b.x;

    float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
    float t1 = (b.x - d) * ra;
    float t2 = (b.x + d) * ra;

    if (abs(a.x) < kSolverEpsilon)
        t1 = t2 = p12.x * rb;

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

    // Horizontal band header: (curveCount, offset), then the reference list.
    // (texelFetch on a usampler2D returns uvec4; the R16G16_UINT pool carries
    // the two fields in .xy, like the reference's uint4 band texel.)
    uvec2 hbandData = texelFetch(bandTexture, ivec2(glyphLoc.x + bandIndex.y, glyphLoc.y), 0).xy;
    ivec2 hbandLoc = CalcBandLoc(glyphLoc, hbandData.y);
    for (int curveIndex = 0; curveIndex < int(hbandData.x); ++curveIndex)
    {
        ivec2 curveLoc = ivec2(texelFetch(bandTexture, ivec2(hbandLoc.x + curveIndex, hbandLoc.y), 0).xy);

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

    // Perf guard from the port checklist: nothing to blend below 0.001.
    if (fill < 0.001)
        discard;

    outColor = vColor * fill;
}
