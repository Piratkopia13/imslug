// ============================================================================
// imslug - Slug-based GPU text rendering for Dear ImGui.
//
// slug.frag.wgsl - WebGPU WGSL pixel shader. This file is the hand-maintained
// WGSL port of the authoritative GLSL source shaders/vulkan/slug.frag; the two
// are kept in lockstep by review + tests (mapping table and deliberate
// deviations: shaders/reference/wgsl-port.txt). The committed generated header
// shaders/imslug_shaders_webgpu.h embeds this file via scripts/gen_shaders.*.
//
// Slug shader code Copyright 2017 by Eric Lengyel.
//
// This file is a port/adaptation of the reference Slug pixel shader
// (https://github.com/EricLengyel/Slug, SlugPixelShader.hlsl; dual-licensed
// MIT OR Apache-2.0, both texts in NOTICE section 1): CalcRootCode,
// SolveHorizPoly/SolveVertPoly, CalcBandLoc, CalcCoverage and the banded
// SlugRender walk, function for function.
//
// Texture formats (imslug_internal.h): curve pool RGBA16F, band pool R16G16_UINT
// (the reference's uint4 band texel carries the same two u16 fields in .xy).
// Both are read with textureLoad: nearest sampling, no filtering, no samplers,
// manual row-wrap only through CalcBandLoc (the CPU packer guarantees lists
// never straddle a row).
// ============================================================================

// ----------------------------------------------------------------------------
// Fragment inputs (the locations must match slug.vert.wgsl exactly; integers
// are flat by rule).
// ----------------------------------------------------------------------------
struct FragmentIn
{
    @location(0) vTexCoord: vec2<f32>,                       // em-space sample coordinate
    @location(1) @interpolate(flat) vBanding: vec4<f32>,     // (bandScaleX, bandScaleY, bandOffsetX, bandOffsetY)
    @location(2) @interpolate(flat) vGlyphLoc: vec2<u32>,
    @location(3) @interpolate(flat) vBandMaxWord: u32,       // bandMaxX | bandMaxY << 16 | E << 28
    @location(4) vColor: vec4<f32>,
    @location(5) vScreenPos: vec2<f32>,                      // screen pixels (Y down), undilated (debug)
    @location(6) @interpolate(flat) vScreenRect: vec4<f32>,  // source quad (debug bounds)
    @location(7) @interpolate(flat) vPaintIndex: u32,        // paint record index, 0xFFFFFFFF = plain
}

@group(0) @binding(0) var curve_tex: texture_2d<f32>;  // RGBA16F control points
@group(0) @binding(1) var band_tex: texture_2d<u32>;   // R16G16_UINT headers/refs

// One paint record per color layer (ImSlugPaintRecord in imslug_internal.h,
// 352 bytes, std430 in the GLSL; the storage address space gives the identical
// layout here). The instance's vPaintIndex selects one; plain glyphs use
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
    Header: vec4<u32>,            // offset 0
    Xform0: vec4<f32>,            // offset 16
    Xform1: vec4<f32>,            // offset 32
    Geo0: vec4<f32>,              // offset 48
    Geo1: vec4<f32>,              // offset 64
    Clip: vec4<f32>,              // offset 80
    Stops: array<vec2<u32>, 32>,  // offset 96; 8 bytes per stop, the 32-entry cap
}

struct SlugPaintBuffer
{
    Paints: array<SlugPaintRecord>,
}

@group(0) @binding(2) var<storage, read> paints: SlugPaintBuffer;

const kNoPaintIndex: u32 = 0xFFFFFFFFu;
const kPaintTiny: f32 = 1.0e-12;

// The uniform block mirrors shaders/wgsl/slug.vert.wgsl's `pc` (the GLSL push
// constant block); only the debug flags are read here, but the offsets must
// stay in sync (Viewport at 64, DebugFlags at 72).
struct Pc
{
    mvp0: vec4<f32>,
    mvp1: vec4<f32>,
    mvp2: vec4<f32>,
    mvp3: vec4<f32>,
    viewport: vec2<f32>,
    debug_flags: u32,
}

@group(0) @binding(3) var<uniform> pc: Pc;

const kLogBandTextureWidth: u32 = 12u;  // fixed pool width 4096 (imslug_internal.h)
const kBandTextureWidthMask: i32 = 0xFFF;  // 0xFFF == (1 << 12) - 1, the pool width wrap
const kSolverEpsilon: f32 = 1.0 / 65536.0;

// Debug overlay flags (mirror ImSlugVulkanDebugOverlay in
// backends/imslug_impl_vulkan.h; the WebGPU adapter carries the same values).
// RefHeat takes precedence over BandGrid.
const kDebugFlagGlyphBounds: u32 = 1u;  // 1px outline of the drawn quad
const kDebugFlagBandGrid: u32 = 2u;     // H/V band checkerboard per glyph
const kDebugFlagRefHeat: u32 = 4u;      // band reference walk cost heatmap
const kRefHeatScale: f32 = 16.0;        // refs walked -> [0,1] heat

// Reference CalcRootCode(): root eligibility code from the signs of the three
// control-point y values. Eligibility is returned in bits 0 and 8.
fn CalcRootCode(y1: f32, y2: f32, y3: f32) -> u32
{
    let i1 = bitcast<u32>(y1) >> 31u;
    let i2 = bitcast<u32>(y2) >> 30u;
    let i3 = bitcast<u32>(y3) >> 29u;

    var shift = (i2 & 2u) | (i1 & ~2u);
    shift = (i3 & 4u) | (shift & ~4u);

    return ((0x2E74u >> shift) & 0x0101u);
}

// Reference SolveHorizPoly(): solve for the t where the sample-relative
// quadratic y(t) = a t^2 - 2b t + c crosses y = 0, then return the x
// coordinates where C(t) = 0. The nearly-linear branch (|a| < 1/65536) also
// guards b like the CPU arbiter (imslug_refraster.cpp, Slughorn's adaptation):
// when |b| < 1/65536 it returns t = 0 instead of the reference's c * (0.5/0)
// inf/NaN. The guard is reachable because the packer's axis-parallel exclusion
// runs on the fp32 outline while the GPU reads fp16 pool texels: quantization
// can make an admitted curve all-equal on one axis. Keeping the GPU exactly on
// the CPU side of this branch is what the goldens compare.
fn SolveHorizPoly(p12: vec4<f32>, p3: vec2<f32>) -> vec2<f32>
{
    let a = p12.xy - p12.zw * 2.0 + p3;
    let b = p12.xy - p12.zw;
    let ra = 1.0 / a.y;

    let d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
    var t1 = (b.y - d) * ra;
    var t2 = (b.y + d) * ra;

    if (abs(a.y) < kSolverEpsilon)
    {
        // The GLSL writes `t1 = t2 = (|b| >= eps) ? c * (0.5/b) : 0`;
        // WGSL has no chained assignment, so it becomes two assignments.
        t1 = select(0.0, p12.y * (0.5 / b.y), abs(b.y) >= kSolverEpsilon);
        t2 = t1;
    }

    return vec2<f32>((a.x * t1 - b.x * 2.0) * t1 + p12.x, (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

// Reference SolveVertPoly(): same, crossing x = 0.
fn SolveVertPoly(p12: vec4<f32>, p3: vec2<f32>) -> vec2<f32>
{
    let a = p12.xy - p12.zw * 2.0 + p3;
    let b = p12.xy - p12.zw;
    let ra = 1.0 / a.x;

    let d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
    var t1 = (b.x - d) * ra;
    var t2 = (b.x + d) * ra;

    if (abs(a.x) < kSolverEpsilon)
    {
        t1 = select(0.0, p12.x * (0.5 / b.x), abs(b.x) >= kSolverEpsilon);
        t2 = t1;
    }

    return vec2<f32>((a.y * t1 - b.y * 2.0) * t1 + p12.y, (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

// Reference CalcBandLoc(): wrap the u16 list offset to the next row when it
// exceeds the fixed 4096-texel width. (The shift amount is u32: WGSL shift
// operators take an unsigned right-hand side.)
fn CalcBandLoc(glyphLoc: vec2<i32>, offset: u32) -> vec2<i32>
{
    var bandLoc = vec2<i32>(glyphLoc.x + i32(offset), glyphLoc.y);
    bandLoc.y += bandLoc.x >> kLogBandTextureWidth;
    bandLoc.x &= kBandTextureWidthMask;
    return bandLoc;
}

// Reference CalcCoverage(): weighted mean of the two ray coverages vs the
// weaker ray, then the fill rule. The E flag lives at bit 28 of the packed
// bandMax word (= bit 12 of its high 16-bit half, the reference's
// glyphData.w test).
fn CalcCoverage(xcov: f32, ycov: f32, xwgt: f32, ywgt: f32, bandMaxWord: u32) -> f32
{
    var coverage = max(abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, kSolverEpsilon),
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
// Color layer evaluation (mirror of ImSlugSamplePaintRecord in
// imslug_colr.cpp; the CPU mirror is the arbiter the goldens compare against).
// Only reached when the instance carries a valid PaintIndex, so color-free
// text never enters this code (the caller's branch is uniform per instance).
// ============================================================================
fn PaintStopOffset(stop: vec2<u32>) -> f32
{
    var raw = i32(stop.x & 0xFFFFu);
    if (raw >= 0x8000)
    {
        raw -= 0x10000;  // F2DOT14 sign extension
    }
    return f32(raw) * (1.0 / 16384.0);
}

fn UnpackPaintColor(packed: u32) -> vec4<f32>
{
    return vec4<f32>(f32(packed & 0xFFu), f32((packed >> 8u) & 0xFFu), f32((packed >> 16u) & 0xFFu),
                     f32((packed >> 24u) & 0xFFu)) * (1.0 / 255.0);
}

fn ExtendPaintParameter(t: f32, extend: u32) -> f32
{
    if (extend == 1u)
    {
        return t - floor(t);  // repeat
    }
    if (extend == 2u)
    {
        let h = t * 0.5;
        return 1.0 - abs((h - floor(h)) * 2.0 - 1.0);  // reflect
    }
    return clamp(t, 0.0, 1.0);  // pad
}

fn SamplePaintColorLine(index: u32, t: f32) -> vec4<f32>
{
    let count = paints.Paints[index].Header.z;
    if (count == 0u)
    {
        return vec4<f32>(0.0);
    }
    var upper = 1u;
    while (upper < count && PaintStopOffset(paints.Paints[index].Stops[upper]) < t)
    {
        upper += 1u;
    }
    if (upper >= count)
    {
        return UnpackPaintColor(paints.Paints[index].Stops[count - 1u].y);
    }
    let lower = upper - 1u;
    let t0 = PaintStopOffset(paints.Paints[index].Stops[lower]);
    let t1 = PaintStopOffset(paints.Paints[index].Stops[upper]);
    let f = select(0.0, clamp((t - t0) / (t1 - t0), 0.0, 1.0), t1 > t0);
    return mix(UnpackPaintColor(paints.Paints[index].Stops[lower].y),
               UnpackPaintColor(paints.Paints[index].Stops[upper].y), f);
}

// Reads fields straight from the buffer (no by-value struct copy: the 352-byte
// record would otherwise occupy a large register block in every fragment,
// including the color-free path's shader).
fn SamplePaint(index: u32, emCoord: vec2<f32>) -> vec4<f32>
{
    // ClipBox (record em space); disabled when max <= min.
    let clip = paints.Paints[index].Clip;
    if (clip.z > clip.x && clip.w > clip.y)
    {
        if (emCoord.x < clip.x || emCoord.x > clip.z || emCoord.y < clip.y || emCoord.y > clip.w)
        {
            return vec4<f32>(0.0);
        }
    }

    // `paintType` is the GLSL `type` (a WGSL reserved word).
    let paintType = paints.Paints[index].Header.x;
    if (paintType == 0u)  // solid
    {
        return UnpackPaintColor(paints.Paints[index].Header.w);
    }

    let xform0 = paints.Paints[index].Xform0;
    let xform1 = paints.Paints[index].Xform1;
    let localX = xform0.x * emCoord.x + xform0.y * emCoord.y + xform0.z;
    let localY = xform1.x * emCoord.x + xform1.y * emCoord.y + xform1.z;

    var t = 0.0;
    if (paintType == 1u)  // linear: iso-color lines parallel to line p0p2
    {
        let p0 = paints.Paints[index].Geo0.xy;
        let p1 = paints.Paints[index].Geo0.zw;
        let p2 = paints.Paints[index].Geo1.xy;
        let e1 = p1 - p0;
        let e2 = p2 - p0;
        let cross = e1.x * e2.y - e1.y * e2.x;
        let scale = (abs(e1.x) + abs(e1.y)) * (abs(e2.x) + abs(e2.y));
        if ((e1.x == 0.0 && e1.y == 0.0) || (e2.x == 0.0 && e2.y == 0.0) || abs(cross) < kPaintTiny * scale)
        {
            return vec4<f32>(0.0);  // ill-formed per the spec: not rendered
        }
        let d = vec2<f32>(localX, localY) - p0;
        t = (d.x * e2.y - d.y * e2.x) / cross;
    }
    else if (paintType == 2u)  // two-point conical (WHATWG radial algorithm)
    {
        let c0 = paints.Paints[index].Geo0.xy;
        let r0 = paints.Paints[index].Geo0.z;
        let c1 = paints.Paints[index].Geo1.xy;
        let r1 = paints.Paints[index].Geo1.z;
        let d = c1 - c0;
        let dr = r1 - r0;
        let q = vec2<f32>(localX, localY) - c0;
        let a = dot(d, d) - dr * dr;
        let b = -2.0 * (dot(q, d) + r0 * dr);
        let c = dot(q, q) - r0 * r0;
        var found = false;
        var best = 0.0;
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
            let disc = b * b - 4.0 * a * c;
            if (disc >= 0.0)
            {
                let root = sqrt(disc);
                let candidate0 = (-b + root) / (2.0 * a);
                let candidate1 = (-b - root) / (2.0 * a);
                for (var i = 0; i < 2; i += 1)
                {
                    let candidate = select(candidate1, candidate0, i == 0);
                    if (r0 + candidate * dr <= 0.0)
                    {
                        continue;  // the algorithm draws circles with r(omega) > 0
                    }
                    if (!found || candidate > best)
                    {
                        best = candidate;
                        found = true;
                    }
                }
            }
        }
        if (!found)
        {
            return vec4<f32>(0.0);  // outside the cone / both radii zero
        }
        t = best;
    }
    else  // sweep: counter-clockwise degrees from +x
    {
        let center = paints.Paints[index].Geo0.xy;
        let startAngle = paints.Paints[index].Geo0.z;
        let endAngle = paints.Paints[index].Geo0.w;
        let span = endAngle - startAngle;
        if (abs(span) < 1.0e-4)
        {
            if (paints.Paints[index].Header.y != 0u)
            {
                return vec4<f32>(0.0);  // repeat/reflect with equal angles: nothing
            }
            t = 0.0;
        }
        else
        {
            var theta = degrees(atan2(localY - center.y, localX - center.x));
            if (theta < 0.0)
            {
                theta += 360.0;
            }
            t = (theta - startAngle) / span;
        }
    }

    t = ExtendPaintParameter(t, paints.Paints[index].Header.y);
    return SamplePaintColorLine(index, t);
}

@fragment
fn main(frag: FragmentIn) -> @location(0) vec4<f32>
{
    // Reference SlugRender(): effective pixel size of the em square from
    // texcoord derivatives (the vertex shader keeps the mapping affine, so
    // this is exactly the instance's pixels-per-em on both axes).
    let emsPerPixel = fwidth(frag.vTexCoord);
    let pixelsPerEm = 1.0 / emsPerPixel;
    let bandMax = vec2<i32>(i32(frag.vBandMaxWord & 0xFFFFu), i32((frag.vBandMaxWord >> 16u) & 0xFFu));
    let glyphLoc = vec2<i32>(frag.vGlyphLoc);

    // Band indexes from the em coordinate; clamp to [0, bandMax].
    let bandIndex = clamp(vec2<i32>(frag.vTexCoord * frag.vBanding.xy + frag.vBanding.zw),
                          vec2<i32>(0), bandMax);

    var xcov = 0.0;
    var xwgt = 0.0;
    var refCount = 0u;  // debug overlay: band references whose curve was fetched

    // Horizontal band header: (curveCount, offset), then the reference list.
    // (textureLoad on a texture_2d<u32> returns vec4<u32>; the R16G16_UINT pool
    // carries the two fields in .xy, like the reference's uint4 band texel.)
    let hbandData = textureLoad(band_tex, vec2<i32>(glyphLoc.x + bandIndex.y, glyphLoc.y), 0).xy;
    let hbandLoc = CalcBandLoc(glyphLoc, hbandData.y);
    for (var curveIndex = 0; curveIndex < i32(hbandData.x); curveIndex += 1)
    {
        let curveLoc = vec2<i32>(textureLoad(band_tex, vec2<i32>(hbandLoc.x + curveIndex, hbandLoc.y), 0).xy);
        refCount += 1u;

        // Make the control points sample-relative.
        let p12 = textureLoad(curve_tex, curveLoc, 0) - vec4<f32>(frag.vTexCoord, frag.vTexCoord);
        let p3 = textureLoad(curve_tex, vec2<i32>(curveLoc.x + 1, curveLoc.y), 0).xy - frag.vTexCoord;

        // Curves are sorted descending by max x: once the largest control
        // point is more than half a pixel left of the sample, stop.
        if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5)
        {
            break;
        }

        let code = CalcRootCode(p12.y, p12.w, p3.y);
        if (code != 0u)
        {
            let r = SolveHorizPoly(p12, p3) * pixelsPerEm.x;

            if ((code & 1u) != 0u)
            {
                xcov += clamp(r.x + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u)
            {
                xcov -= clamp(r.y + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    var ycov = 0.0;
    var ywgt = 0.0;

    // Vertical band header follows all horizontal ones (bandMax.y + 1 + index).
    let vbandData = textureLoad(band_tex, vec2<i32>(glyphLoc.x + bandMax.y + 1 + bandIndex.x, glyphLoc.y), 0).xy;
    let vbandLoc = CalcBandLoc(glyphLoc, vbandData.y);
    for (var curveIndex = 0; curveIndex < i32(vbandData.x); curveIndex += 1)
    {
        let curveLoc = vec2<i32>(textureLoad(band_tex, vec2<i32>(vbandLoc.x + curveIndex, vbandLoc.y), 0).xy);
        refCount += 1u;
        let p12 = textureLoad(curve_tex, curveLoc, 0) - vec4<f32>(frag.vTexCoord, frag.vTexCoord);
        let p3 = textureLoad(curve_tex, vec2<i32>(curveLoc.x + 1, curveLoc.y), 0).xy - frag.vTexCoord;

        // Sorted descending by max y.
        if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5)
        {
            break;
        }

        let code = CalcRootCode(p12.x, p12.z, p3.x);
        if (code != 0u)
        {
            let r = SolveVertPoly(p12, p3) * pixelsPerEm.y;

            if ((code & 1u) != 0u)
            {
                ycov -= clamp(r.x + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u)
            {
                ycov += clamp(r.y + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    let fill = CalcCoverage(xcov, ycov, xwgt, ywgt, frag.vBandMaxWord);

    // A color layer resolves its own straight-alpha color (solid or
    // gradient); the instance color then only contributes the tint alpha
    // (ImGui forces RGB=255 for Colored glyphs, so a tint fades the layer
    // colors instead of recoloring them). Plain glyphs keep the monochrome
    // path:
    // paintColor = 1, so glyphColor/glyphAlpha are exactly vColor.
    // (WGSL's select() evaluates both arms, so the paint call stays behind an
    // if and color-free text never enters SamplePaint's code.)
    let isColored = frag.vPaintIndex != kNoPaintIndex;
    var paintColor = vec4<f32>(1.0);
    if (isColored)
    {
        paintColor = SamplePaint(frag.vPaintIndex, frag.vTexCoord);
    }
    let glyphColor = select(frag.vColor.rgb, paintColor.rgb, isColored);
    let glyphAlpha = frag.vColor.a * paintColor.a;

    // Debug overlay: rebuild the fragment color for the requested views.
    // Debug views never discard and keep the whole dilated quad visible, so
    // the band/ref diagnostics show up outside the glyph body as well.
    if (pc.debug_flags != 0u)
    {
        var debugColor = vec4<f32>(glyphColor, glyphAlpha * fill);
        if ((pc.debug_flags & kDebugFlagRefHeat) != 0u)
        {
            // Walk cost of this pixel: blue (0 refs) -> red (>= kRefHeatScale).
            let heat = clamp(f32(refCount) / kRefHeatScale, 0.0, 1.0);
            debugColor = vec4<f32>(heat, 0.5 - abs(heat - 0.5), 1.0 - heat, 0.55 + 0.45 * fill);
        }
        else if ((pc.debug_flags & kDebugFlagBandGrid) != 0u)
        {
            // Checkerboard of the glyph's H/V band grid: alternating cell
            // parity picks the tint, so every band is visibly distinct.
            let checker = f32(((bandIndex.x >> 1) + bandIndex.y) & 1);
            debugColor = vec4<f32>(mix(debugColor.rgb, mix(vec3<f32>(0.12, 0.45, 1.0),
                                                           vec3<f32>(1.0, 0.85, 0.15), checker), 0.6),
                                   debugColor.a);
            debugColor.a = 0.45 + 0.55 * fill;
        }
        if ((pc.debug_flags & kDebugFlagGlyphBounds) != 0u)
        {
            // Distance to the nearest source-quad edge, in screen pixels; the
            // position is the undilated one, so the outline sits exactly on the
            // quad ImGui asked the run to draw (the DebugDrawBounds source).
            let edgeDistance = min(frag.vScreenPos - frag.vScreenRect.xy, frag.vScreenRect.zw - frag.vScreenPos);
            if (min(edgeDistance.x, edgeDistance.y) < 1.0)
            {
                debugColor = vec4<f32>(1.0, 0.45, 0.05, 1.0);
            }
        }
        return debugColor;
    }

    // Perf guard from the port checklist: nothing to blend below 0.001.
    if (fill < 0.001)
    {
        discard;
    }

    // Unpremultiplied output: the pipeline blend is stock ImGui's
    // src-alpha / one-minus-src-alpha, so the coverage scales the alpha only
    // (the reference returns color * coverage, which assumes a premultiplied
    // blend state; using it here would square the edge coverage).
    return vec4<f32>(glyphColor, glyphAlpha * fill);
}
