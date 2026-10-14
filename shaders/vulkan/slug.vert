// ============================================================================
// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// slug.vert — Vulkan GLSL vertex shader (source of truth; compiled by
// scripts/gen_shaders.* into the committed SPIR-V header).
//
// Slug shader code Copyright 2017 by Eric Lengyel.
//
// This file is a port/adaptation of the reference Slug vertex shader
// (https://github.com/EricLengyel/Slug, SlugVertexShader.hlsl; dual-licensed
// MIT OR Apache-2.0, both texts in NOTICE section 1). It ports SlugDilate()
// (perspective-correct ~1px screen-space dilation, sample coordinate
// re-derived through the inverse Jacobian) and replaces the reference's five
// per-vertex attribute streams with one per-glyph instance record
// (ImSlugGlyphInstance, imslug_internal.h) plus the quad corner from
// gl_VertexIndex. The auditable mapping table lives in
// shaders/reference/slug.hlsl.
//
// Object space here is screen pixels (Y down): the quad corners are the
// instance's ScreenRect corners and the em sample coordinates come from the
// instance's em bbox + inverse Jacobian. This mirrors the reference layout,
// where `pos` is the object-space quad, `tex` is the em coordinate, `jac` is
// the screen->em Jacobian and the MVP maps object space to clip space.
// ============================================================================

#version 450

precision highp float;
precision highp int;

// ----------------------------------------------------------------------------
// Per-instance input (VK_VERTEX_INPUT_RATE_INSTANCE; the draw uses 4 vertices
// with no vertex-rate attributes, so gl_VertexIndex is the quad corner 0..3).
// Layout mirrors ImSlugGlyphInstance exactly (static_assert'ed in C++).
// loc 0..6 = M2 layout, loc 7 = M5 PaintIndex (see imslug_internal.h).
// ----------------------------------------------------------------------------
layout(location = 0) in vec4 InEmBBox;       // (minX, minY, maxX, maxY) em space, Y up
layout(location = 1) in vec4 InScreenRect;   // (minX, minY, maxX, maxY) screen pixels, Y down
layout(location = 2) in vec4 InJac;          // inverse Jacobian rows: em += (dot(d, .xy), dot(d, .zw))
layout(location = 3) in vec4 InBanding;      // (bandScaleX, bandScaleY, bandOffsetX, bandOffsetY)
layout(location = 4) in uvec2 InGlyphLoc;    // band pool glyph location (x, y)
layout(location = 5) in uint InBandMaxWord;  // bandMaxX | bandMaxY << 16 | E << 28 (D8(g))
layout(location = 6) in vec4 InColor;        // (r, g, b, a), straight alpha
layout(location = 7) in uint InPaintIndex;   // M5: paint record index, 0xFFFFFFFF = plain glyph

layout(push_constant) uniform PushConstants
{
    vec4 MVP0;      // The four rows of the object-space (screen pixels) -> clip MVP.
    vec4 MVP1;
    vec4 MVP2;
    vec4 MVP3;
    vec2 Viewport;   // framebuffer dimensions, in pixels (reference slug_viewport.xy)
    uint DebugFlags; // M4 debug overlay flags (ImSlugVulkanDebugOverlay; 0 = off)
} pc;

// ----------------------------------------------------------------------------
// Varyings (mirror the reference VertexStruct; banding/glyph are constant over
// the glyph, i.e. flat).
// ----------------------------------------------------------------------------
layout(location = 0) out vec2 vTexCoord;      // em-space sample coordinate
layout(location = 1) flat out vec4 vBanding;  // band scale and offset
layout(location = 2) flat out uvec2 vGlyphLoc;
layout(location = 3) flat out uint vBandMaxWord;
layout(location = 4) out vec4 vColor;
layout(location = 5) out vec2 vScreenPos;                     // screen pixels (Y down), undilated
layout(location = 6) flat out vec4 vScreenRect;               // instance ScreenRect (debug bounds)
layout(location = 7) flat out uint vPaintIndex;               // M5 (color layers)

// Reference SlugDilate(): expands the vertex along its object-space normal so
// the quad covers every pixel whose center is within half a pixel of the glyph
// (the coverage ramp is saturate(r + 0.5) in the pixel shader), and re-derives
// the em sample coordinate of the dilated position through the inverse
// Jacobian. `dir` is the unnormalized corner direction (+/-1, +/-1); the
// reference demo passes the same for its quad corners, so each corner moves by
// exactly half a pixel per axis.
void SlugDilate(vec2 pos, vec2 dir, vec2 tex, vec4 jac, out vec2 outPos, out vec2 outTex)
{
    vec2 n = normalize(dir);
    float s = dot(pc.MVP3.xy, pos) + pc.MVP3.w;
    float t = dot(pc.MVP3.xy, n);

    float u = (s * dot(pc.MVP0.xy, n) - t * (dot(pc.MVP0.xy, pos) + pc.MVP0.w)) * pc.Viewport.x;
    float v = (s * dot(pc.MVP1.xy, n) - t * (dot(pc.MVP1.xy, pos) + pc.MVP1.w)) * pc.Viewport.y;

    float s2 = s * s;
    float st = s * t;
    float uv = u * u + v * v;
    vec2 d = dir * (s2 * (st + sqrt(uv)) / (uv - st * st));

    outPos = pos + d;
    outTex = tex + vec2(dot(d, jac.xy), dot(d, jac.zw));
}

void main()
{
    // Quad corner from the vertex index: (0,0), (1,0), (0,1), (1,1) as a
    // triangle strip. `dir` is the outward screen-space corner direction.
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vec2 dir = vec2(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0);

    // The same corner parameterization drives both spaces: em corner c maps to
    // the screen rect corner with the Y flip applied (em Y up, screen Y down),
    // i.e. screen y = mix(ScreenRect.w, ScreenRect.y, c.y).
    vec2 pos = vec2(mix(InScreenRect.x, InScreenRect.z, corner.x), mix(InScreenRect.w, InScreenRect.y, corner.y));
    vec2 tex = mix(InEmBBox.xy, InEmBBox.zw, corner);

    vec2 dilatedPos;
    vec2 dilatedTex;
    SlugDilate(pos, dir, tex, InJac, dilatedPos, dilatedTex);

    gl_Position = vec4(dot(pc.MVP0.xy, dilatedPos) + pc.MVP0.w,
                       dot(pc.MVP1.xy, dilatedPos) + pc.MVP1.w,
                       dot(pc.MVP2.xy, dilatedPos) + pc.MVP2.w,
                       dot(pc.MVP3.xy, dilatedPos) + pc.MVP3.w);

    vTexCoord = dilatedTex;
    vBanding = InBanding;
    vGlyphLoc = InGlyphLoc;
    vBandMaxWord = InBandMaxWord;
    vColor = InColor;
    // Debug overlay inputs (M4): the undilated quad position and the source
    // rectangle, so the fragment shader can outline the quad the run was asked
    // to draw (ImSlugVulkanDebugOverlay_GlyphBounds).
    vScreenPos = pos;
    vScreenRect = InScreenRect;
    // M5 color layers: the instance's paint record (0xFFFFFFFF = plain glyph).
    vPaintIndex = InPaintIndex;
}
