// ============================================================================
// imslug - Slug-based GPU text rendering for Dear ImGui.
//
// slug.vert.wgsl - WebGPU WGSL vertex shader. This file is the hand-maintained
// WGSL port of the authoritative GLSL source shaders/vulkan/slug.vert; the two
// are kept in lockstep by review + tests (mapping table:
// shaders/reference/wgsl-port.txt). The committed generated header
// shaders/imslug_shaders_webgpu.h embeds this file via scripts/gen_shaders.*.
//
// Slug shader code Copyright 2017 by Eric Lengyel.
//
// This file is a port/adaptation of the reference Slug vertex shader
// (https://github.com/EricLengyel/Slug, SlugVertexShader.hlsl; dual-licensed
// MIT OR Apache-2.0, both texts in NOTICE section 1). It ports SlugDilate()
// (perspective-correct ~1px screen-space dilation, sample coordinate
// re-derived through the inverse Jacobian) and replaces the reference's five
// per-vertex attribute streams with one per-glyph instance record
// (ImSlugGlyphInstance, imslug_internal.h) plus the quad corner from the
// vertex index.
//
// Object space here is screen pixels (Y down): the quad corners are the
// instance's ScreenRect corners and the em sample coordinates come from the
// instance's em bbox + inverse Jacobian. This mirrors the reference layout,
// where `pos` is the object-space quad, `tex` is the em coordinate, `jac` is
// the screen->em Jacobian and the MVP maps object space to clip space.
// ============================================================================

// ----------------------------------------------------------------------------
// Per-instance input (vertex step mode "instance"; the draw uses 4 vertices
// with no vertex-rate attributes, so the vertex index is the quad corner 0..3).
// Layout mirrors ImSlugGlyphInstance exactly (static_assert'ed in C++).
// location 0..6 = base layout, location 7 = PaintIndex (see imslug_internal.h).
// ----------------------------------------------------------------------------
struct InstanceIn
{
    @location(0) InEmBBox: vec4<f32>,      // (minX, minY, maxX, maxY) em space, Y up
    @location(1) InScreenRect: vec4<f32>,  // (minX, minY, maxX, maxY) screen pixels, Y down
    @location(2) InJac: vec4<f32>,         // inverse Jacobian rows: em += (dot(d, .xy), dot(d, .zw))
    @location(3) InBanding: vec4<f32>,     // (bandScaleX, bandScaleY, bandOffsetX, bandOffsetY)
    @location(4) InGlyphLoc: vec2<u32>,    // band pool glyph location (x, y)
    @location(5) InBandMaxWord: u32,       // bandMaxX | bandMaxY << 16 | E << 28
    @location(6) InColor: vec4<f32>,       // (r, g, b, a), straight alpha
    @location(7) InPaintIndex: u32,        // paint record index, 0xFFFFFFFF = plain glyph
}

// The GLSL push constant block as an 80-byte uniform record (WebGPU has no
// push constants): the four MVP rows at offsets 0/16/32/48, Viewport at 64 and
// DebugFlags at 72 (plus 4 bytes of padding to the 16-byte struct alignment).
struct Pc
{
    mvp0: vec4<f32>,      // The four rows of the object-space (screen pixels) -> clip MVP.
    mvp1: vec4<f32>,
    mvp2: vec4<f32>,
    mvp3: vec4<f32>,
    viewport: vec2<f32>,  // framebuffer dimensions, in pixels (reference slug_viewport.xy)
    debug_flags: u32,     // debug overlay flags (mirror of ImSlugVulkanDebugOverlay; 0 = off)
}

@group(0) @binding(3) var<uniform> pc: Pc;

// ----------------------------------------------------------------------------
// Varyings (mirror the reference VertexStruct; banding/glyph are constant over
// the glyph, i.e. flat).
// ----------------------------------------------------------------------------
struct VertexOut
{
    @builtin(position) position: vec4<f32>,
    @location(0) vTexCoord: vec2<f32>,                       // em-space sample coordinate
    @location(1) @interpolate(flat) vBanding: vec4<f32>,     // band scale and offset
    @location(2) @interpolate(flat) vGlyphLoc: vec2<u32>,
    @location(3) @interpolate(flat) vBandMaxWord: u32,
    @location(4) vColor: vec4<f32>,
    @location(5) vScreenPos: vec2<f32>,                      // screen pixels (Y down), undilated
    @location(6) @interpolate(flat) vScreenRect: vec4<f32>,  // instance ScreenRect (debug bounds)
    @location(7) @interpolate(flat) vPaintIndex: u32,        // color layers
}

// Reference SlugDilate(): expands the vertex along its object-space normal so
// the quad covers every pixel whose center is within half a pixel of the glyph
// (the coverage ramp is saturate(r + 0.5) in the pixel shader), and re-derives
// the em sample coordinate of the dilated position through the inverse
// Jacobian. `dir` is the unnormalized corner direction (+/-1, +/-1); the
// reference demo passes the same for its quad corners, so each corner moves by
// exactly half a pixel per axis.
// (WGSL has no `out` parameters: the two results come back through function
// pointers, so the call shape stays that of the GLSL original.)
fn SlugDilate(pos: vec2<f32>, dir: vec2<f32>, tex: vec2<f32>, jac: vec4<f32>,
              outPos: ptr<function, vec2<f32>>, outTex: ptr<function, vec2<f32>>)
{
    let n = normalize(dir);
    let s = dot(pc.mvp3.xy, pos) + pc.mvp3.w;
    let t = dot(pc.mvp3.xy, n);

    let u = (s * dot(pc.mvp0.xy, n) - t * (dot(pc.mvp0.xy, pos) + pc.mvp0.w)) * pc.viewport.x;
    let v = (s * dot(pc.mvp1.xy, n) - t * (dot(pc.mvp1.xy, pos) + pc.mvp1.w)) * pc.viewport.y;

    let s2 = s * s;
    let st = s * t;
    let uv = u * u + v * v;
    let d = dir * (s2 * (st + sqrt(uv)) / (uv - st * st));

    *outPos = pos + d;
    *outTex = tex + vec2<f32>(dot(d, jac.xy), dot(d, jac.zw));
}

@vertex
fn main(@builtin(vertex_index) vertexIndex: u32, inst: InstanceIn) -> VertexOut
{
    // Quad corner from the vertex index: (0,0), (1,0), (0,1), (1,1) as a
    // triangle strip. `dir` is the outward screen-space corner direction.
    let corner = vec2<f32>(f32(vertexIndex & 1u), f32((vertexIndex >> 1u) & 1u));
    let dir = vec2<f32>(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0);

    // The same corner parameterization drives both spaces: em corner c maps to
    // the screen rect corner with the Y flip applied (em Y up, screen Y down),
    // i.e. screen y = mix(ScreenRect.w, ScreenRect.y, c.y).
    let pos = vec2<f32>(mix(inst.InScreenRect.x, inst.InScreenRect.z, corner.x),
                        mix(inst.InScreenRect.w, inst.InScreenRect.y, corner.y));
    let tex = mix(inst.InEmBBox.xy, inst.InEmBBox.zw, corner);

    var dilatedPos: vec2<f32>;
    var dilatedTex: vec2<f32>;
    SlugDilate(pos, dir, tex, inst.InJac, &dilatedPos, &dilatedTex);

    var result: VertexOut;
    result.position = vec4<f32>(dot(pc.mvp0.xy, dilatedPos) + pc.mvp0.w,
                                dot(pc.mvp1.xy, dilatedPos) + pc.mvp1.w,
                                dot(pc.mvp2.xy, dilatedPos) + pc.mvp2.w,
                                dot(pc.mvp3.xy, dilatedPos) + pc.mvp3.w);

    result.vTexCoord = dilatedTex;
    result.vBanding = inst.InBanding;
    result.vGlyphLoc = inst.InGlyphLoc;
    result.vBandMaxWord = inst.InBandMaxWord;
    result.vColor = inst.InColor;
    // Debug overlay inputs: the undilated quad position and the source
    // rectangle, so the fragment shader can outline the quad the run was asked
    // to draw (ImSlugVulkanDebugOverlay_GlyphBounds).
    result.vScreenPos = pos;
    result.vScreenRect = inst.InScreenRect;
    // Color layers: the instance's paint record (0xFFFFFFFF = plain glyph).
    result.vPaintIndex = inst.InPaintIndex;
    return result;
}
