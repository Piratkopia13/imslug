// ============================================================================
// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// slug.frag — OpenGL 3.2+ / OpenGL ES 3.0+ GLSL pixel shader.
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
// Paint records are read from a 2D integer texture (paintTexture, GL_RGBA32UI,
// width = 22 texels, 1 row per 352-byte ImSlugPaintRecord) to avoid SSBO
// requirements on GL 3.2 / GLES 3.0.
// ============================================================================

in vec2 vTexCoord;      // em-space sample coordinate
flat in vec4 vBanding;  // (bandScaleX, bandScaleY, bandOffsetX, bandOffsetY)
flat in uvec2 vGlyphLoc;
flat in uint vBandMaxWord;  // bandMaxX | bandMaxY << 16 | E << 28
in vec4 vColor;
in vec2 vScreenPos;         // screen pixels (Y down), undilated (debug)
flat in vec4 vScreenRect;   // source quad (debug bounds)
flat in uint vPaintIndex;   // paint record index, 0xFFFFFFFF = plain

#if !defined(GL_ES) && (__VERSION__ < 330)
#extension GL_ARB_shader_bit_encoding : enable
#extension GL_ARB_gpu_shader5 : enable
#endif

#ifdef GL_ES
layout(location = 0) out vec4 outColor;
#else
out vec4 outColor;
#endif

uniform sampler2D curveTexture;   // unit 0: RGBA16F control points
uniform usampler2D bandTexture;   // unit 1: R16G16_UINT headers/refs
uniform usampler2D paintTexture;  // unit 2: GL_RGBA32UI, 22 texels wide, 1 row per ImSlugPaintRecord

uniform uint uDebugFlags;

const uint kNoPaintIndex = 0xFFFFFFFFu;
const int kMaxPaintStops = 32;
const float kPaintTiny = 1.0e-12;

const int kLogBandTextureWidth = 12;  // fixed pool width 4096 (imslug_internal.h)
const float kSolverEpsilon = 1.0 / 65536.0;

// Debug overlay flags (mirror ImSlugOpenGL3DebugOverlay).
const uint kDebugFlagGlyphBounds = 1u;  // 1px outline of the drawn quad
const uint kDebugFlagBandGrid = 2u;     // H/V band checkerboard per glyph
const uint kDebugFlagRefHeat = 4u;      // band reference walk cost heatmap
const float kRefHeatScale = 16.0;       // refs walked -> [0,1] heat

#if !defined(GL_ES) && (__VERSION__ < 330)
float imslugUintBitsToFloat(uint u)
{
    float s = (u >> 31u) != 0u ? -1.0 : 1.0;
    int e = int((u >> 23u) & 0xFFu);
    uint m = u & 0x7FFFFFu;
    if (e == 0) return s * exp2(-126.0) * (float(m) * exp2(-23.0));
    return s * exp2(float(e - 127)) * (1.0 + float(m) * exp2(-23.0));
}
vec4 imslugUintBitsToFloat4(uvec4 u)
{
    return vec4(
        imslugUintBitsToFloat(u.x),
        imslugUintBitsToFloat(u.y),
        imslugUintBitsToFloat(u.z),
        imslugUintBitsToFloat(u.w));
}
#else
#define imslugUintBitsToFloat(u)  uintBitsToFloat(u)
#define imslugUintBitsToFloat4(u) uintBitsToFloat(u)
#endif

// Reference CalcRootCode(): root eligibility code from the signs of the three
// control-point y values. Eligibility is returned in bits 0 and 8.
uint CalcRootCode(float y1, float y2, float y3)
{
#if !defined(GL_ES) && (__VERSION__ < 330)
    uint i1 = (y1 < 0.0 || (y1 == 0.0 && 1.0 / y1 < 0.0)) ? 1u : 0u;
    uint i2 = (y2 < 0.0 || (y2 == 0.0 && 1.0 / y2 < 0.0)) ? 2u : 0u;
    uint i3 = (y3 < 0.0 || (y3 == 0.0 && 1.0 / y3 < 0.0)) ? 4u : 0u;
#else
    uint i1 = floatBitsToUint(y1) >> 31U;
    uint i2 = floatBitsToUint(y2) >> 30U;
    uint i3 = floatBitsToUint(y3) >> 29U;
#endif

    uint shift = (i2 & 2U) | (i1 & ~2U);
    shift = (i3 & 4U) | (shift & ~4U);

    return ((0x2E74U >> shift) & 0x0101U);
}

// Reference SolveHorizPoly(): solve for the t where the sample-relative
// quadratic y(t) = a t^2 - 2b t + c crosses y = 0, then return the x
// coordinates where C(t) = 0.
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
// glyphData.w test).
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

vec4 UnpackPaintColor(uint packedColor)
{
    return vec4(float(packedColor & 0xFFu),
                float((packedColor >> 8u) & 0xFFu),
                float((packedColor >> 16u) & 0xFFu),
                float((packedColor >> 24u) & 0xFFu)) * (1.0 / 255.0);
}

float DecodeF2Dot14(uint rawWord)
{
    int raw = int(rawWord & 0xFFFFu);
    if (raw >= 0x8000)
        raw -= 0x10000;
    return float(raw) * (1.0 / 16384.0);
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

uvec2 FetchPaintStop(int pRow, int stopIdx)
{
    uvec4 pair = texelFetch(paintTexture, ivec2(6 + (stopIdx >> 1), pRow), 0);
    return ((stopIdx & 1) == 0) ? pair.xy : pair.zw;
}

vec4 SamplePaintColorLine(int pRow, uint stopCount, float t)
{
    if (stopCount == 0u)
        return vec4(0.0);

    uvec2 st0 = FetchPaintStop(pRow, 0);
    float stop0Offset = DecodeF2Dot14(int(st0.x));
    vec4 stop0Color = UnpackPaintColor(st0.y);
    if (t <= stop0Offset || stopCount == 1u)
        return stop0Color;

    for (uint i = 1u; i < stopCount && i < uint(kMaxPaintStops); ++i)
    {
        uvec2 sti = FetchPaintStop(pRow, int(i));
        float stopOffset = DecodeF2Dot14(int(sti.x));
        vec4 stopColor = UnpackPaintColor(sti.y);
        if (t <= stopOffset)
        {
            float span = stopOffset - stop0Offset;
            float factor = (span > 0.0) ? clamp((t - stop0Offset) / span, 0.0, 1.0) : 0.0;
            return mix(stop0Color, stopColor, factor);
        }
        stop0Offset = stopOffset;
        stop0Color = stopColor;
    }
    return stop0Color;
}

vec4 SamplePaint(uint paintIndex, vec2 emCoord)
{
    int pRow = int(paintIndex);
    uvec4 header = texelFetch(paintTexture, ivec2(0, pRow), 0);
    vec4 clip = imslugUintBitsToFloat4(texelFetch(paintTexture, ivec2(5, pRow), 0));
    if (clip.z > clip.x && clip.w > clip.y)
    {
        if (emCoord.x < clip.x || emCoord.x > clip.z || emCoord.y < clip.y || emCoord.y > clip.w)
            return vec4(0.0);
    }

    uint type = header.x;
    if (type == 0u)  // solid
        return UnpackPaintColor(header.w);

    vec4 xform0 = imslugUintBitsToFloat4(texelFetch(paintTexture, ivec2(1, pRow), 0));
    vec4 xform1 = imslugUintBitsToFloat4(texelFetch(paintTexture, ivec2(2, pRow), 0));
    float localX = xform0.x * emCoord.x + xform0.y * emCoord.y + xform0.z;
    float localY = xform1.x * emCoord.x + xform1.y * emCoord.y + xform1.z;

    vec4 geo0 = imslugUintBitsToFloat4(texelFetch(paintTexture, ivec2(3, pRow), 0));
    vec4 geo1 = imslugUintBitsToFloat4(texelFetch(paintTexture, ivec2(4, pRow), 0));

    float t = 0.0;
    if (type == 1u)  // linear
    {
        vec2 p0 = geo0.xy;
        vec2 p1 = geo0.zw;
        vec2 p2 = geo1.xy;
        vec2 e1 = p1 - p0;
        vec2 e2 = p2 - p0;
        float crossVal = e1.x * e2.y - e1.y * e2.x;
        float scale = (abs(e1.x) + abs(e1.y)) * (abs(e2.x) + abs(e2.y));
        if ((e1.x == 0.0 && e1.y == 0.0) || (e2.x == 0.0 && e2.y == 0.0) || abs(crossVal) < kPaintTiny * scale)
            return vec4(0.0);
        vec2 d = vec2(localX, localY) - p0;
        t = (d.x * e2.y - d.y * e2.x) / crossVal;
    }
    else if (type == 2u)  // two-point conical (radial)
    {
        vec2 c0 = geo0.xy;
        float r0 = geo0.z;
        vec2 c1 = geo1.xy;
        float r1 = geo1.z;
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
                        continue;
                    if (!found || candidate > best)
                    {
                        best = candidate;
                        found = true;
                    }
                }
            }
        }
        if (!found)
            return vec4(0.0);
        t = best;
    }
    else  // sweep
    {
        vec2 center = geo0.xy;
        float startAngle = geo0.z;
        float endAngle = geo0.w;
        float span = endAngle - startAngle;
        if (abs(span) < 1.0e-4)
        {
            if (header.y != 0u)
                return vec4(0.0);
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

    t = ExtendPaintParameter(t, header.y);
    return SamplePaintColorLine(pRow, header.z, t);
}

void main()
{
    vec2 emsPerPixel = fwidth(vTexCoord);
    vec2 pixelsPerEm = 1.0 / emsPerPixel;
    ivec2 bandMax = ivec2(int(vBandMaxWord & 0xFFFFu), int((vBandMaxWord >> 16) & 0xFFu));
    ivec2 glyphLoc = ivec2(vGlyphLoc);

    ivec2 bandIndex = clamp(ivec2(vTexCoord * vBanding.xy + vBanding.zw), ivec2(0), bandMax);

    uvec2 hbandData = texelFetch(bandTexture, ivec2(glyphLoc.x + bandIndex.y, glyphLoc.y), 0).xy;
    ivec2 hbandLoc = CalcBandLoc(glyphLoc, hbandData.y);

    float xcov = 0.0;
    float xwgt = 0.0;
    int refCount = 0;

    for (int curveIndex = 0; curveIndex < int(hbandData.x); ++curveIndex)
    {
        ivec2 curveLoc = ivec2(texelFetch(bandTexture, ivec2(hbandLoc.x + curveIndex, hbandLoc.y), 0).xy);
        ++refCount;
        vec4 p12 = texelFetch(curveTexture, curveLoc, 0) - vec4(vTexCoord, vTexCoord);
        vec2 p3 = texelFetch(curveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - vTexCoord;

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

    uvec2 vbandData = texelFetch(bandTexture, ivec2(glyphLoc.x + bandMax.y + 1 + bandIndex.x, glyphLoc.y), 0).xy;
    ivec2 vbandLoc = CalcBandLoc(glyphLoc, vbandData.y);

    for (int curveIndex = 0; curveIndex < int(vbandData.x); ++curveIndex)
    {
        ivec2 curveLoc = ivec2(texelFetch(bandTexture, ivec2(vbandLoc.x + curveIndex, vbandLoc.y), 0).xy);
        ++refCount;
        vec4 p12 = texelFetch(curveTexture, curveLoc, 0) - vec4(vTexCoord, vTexCoord);
        vec2 p3 = texelFetch(curveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - vTexCoord;

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

    bool isColored = vPaintIndex != kNoPaintIndex;
    vec4 paintColor = isColored ? SamplePaint(vPaintIndex, vTexCoord) : vec4(1.0);
    vec3 glyphColor = isColored ? paintColor.rgb : vColor.rgb;
    float glyphAlpha = vColor.a * paintColor.a;

    if (uDebugFlags != 0u)
    {
        vec4 debugColor = vec4(glyphColor, glyphAlpha * fill);
        if ((uDebugFlags & kDebugFlagRefHeat) != 0u)
        {
            float heat = clamp(float(refCount) / kRefHeatScale, 0.0, 1.0);
            debugColor = vec4(heat, 0.5 - abs(heat - 0.5), 1.0 - heat, 0.55 + 0.45 * fill);
        }
        else if ((uDebugFlags & kDebugFlagBandGrid) != 0u)
        {
            float checker = float(((bandIndex.x >> 1) + bandIndex.y) & 1);
            debugColor.rgb =
                mix(debugColor.rgb, mix(vec3(0.12, 0.45, 1.0), vec3(1.0, 0.85, 0.15), checker), 0.6);
            debugColor.a = 0.45 + 0.55 * fill;
        }
        if ((uDebugFlags & kDebugFlagGlyphBounds) != 0u)
        {
            vec2 edgeDistance = min(vScreenPos - vScreenRect.xy, vScreenRect.zw - vScreenPos);
            if (min(edgeDistance.x, edgeDistance.y) < 1.0)
                debugColor = vec4(1.0, 0.45, 0.05, 1.0);
        }
        outColor = debugColor;
        return;
    }

    if (fill < 0.001)
        discard;

    outColor = vec4(glyphColor, glyphAlpha * fill);
}
