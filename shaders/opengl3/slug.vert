// ============================================================================
// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// slug.vert — OpenGL 3.2+ / OpenGL ES 3.0+ GLSL vertex shader.
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
// gl_VertexID.
//
// Object space here is screen pixels (Y down): the quad corners are the
// instance's ScreenRect corners and the em sample coordinates come from the
// instance's em bbox + inverse Jacobian.
// ============================================================================

// ----------------------------------------------------------------------------
// Per-instance inputs (bound to attribute locations 0..7 via glBindAttribLocation
// before linking; divisor = 1).
// ----------------------------------------------------------------------------
in vec4 InEmBBox;       // loc 0: (minX, minY, maxX, maxY) em space, Y up
in vec4 InScreenRect;   // loc 1: (minX, minY, maxX, maxY) screen pixels, Y down
in vec4 InJac;          // loc 2: inverse Jacobian rows: em += (dot(d, .xy), dot(d, .zw))
in vec4 InBanding;      // loc 3: (bandScaleX, bandScaleY, bandOffsetX, bandOffsetY)
in uvec2 InGlyphLoc;    // loc 4: band pool glyph location (x, y)
in uint InBandMaxWord;  // loc 5: bandMaxX | bandMaxY << 16 | E << 28
in vec4 InColor;        // loc 6: (r, g, b, a), straight alpha
in uint InPaintIndex;   // loc 7: paint record index, 0xFFFFFFFF = plain glyph

// ----------------------------------------------------------------------------
// Uniforms (passed via glUniform4f / glUniform2f / glUniform1ui)
// ----------------------------------------------------------------------------
uniform vec4 uMVP0;      // The four rows of the object-space (screen pixels) -> clip MVP.
uniform vec4 uMVP1;
uniform vec4 uMVP2;
uniform vec4 uMVP3;
uniform vec2 uViewport;  // Framebuffer dimensions in pixels.
uniform uint uDebugFlags;// Debug overlay flags (0 = off).

// ----------------------------------------------------------------------------
// Varyings
// ----------------------------------------------------------------------------
out vec2 vTexCoord;
flat out vec4 vBanding;
flat out uvec2 vGlyphLoc;
flat out uint vBandMaxWord;
out vec4 vColor;
out vec2 vScreenPos;
flat out vec4 vScreenRect;
flat out uint vPaintIndex;

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
    float s = dot(uMVP3.xy, pos) + uMVP3.w;
    float t = dot(uMVP3.xy, n);

    float u = (s * dot(uMVP0.xy, n) - t * (dot(uMVP0.xy, pos) + uMVP0.w)) * uViewport.x;
    float v = (s * dot(uMVP1.xy, n) - t * (dot(uMVP1.xy, pos) + uMVP1.w)) * uViewport.y;

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
    int cornerIndex = gl_VertexID & 3;
    vec2 corner = vec2(float(cornerIndex & 1), float((cornerIndex >> 1) & 1));
    vec2 dir = vec2(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0);

    // The same corner parameterization drives both spaces: em corner c maps to
    // the screen rect corner with the Y flip applied (em Y up, screen Y down),
    // i.e. screen y = mix(ScreenRect.w, ScreenRect.y, c.y).
    vec2 pos = vec2(mix(InScreenRect.x, InScreenRect.z, corner.x), mix(InScreenRect.w, InScreenRect.y, corner.y));
    vec2 tex = mix(InEmBBox.xy, InEmBBox.zw, corner);

    vec2 dilatedPos;
    vec2 dilatedTex;
    SlugDilate(pos, dir, tex, InJac, dilatedPos, dilatedTex);

    gl_Position = vec4(dot(uMVP0.xy, dilatedPos) + uMVP0.w,
                       dot(uMVP1.xy, dilatedPos) + uMVP1.w,
                       dot(uMVP2.xy, dilatedPos) + uMVP2.w,
                       dot(uMVP3.xy, dilatedPos) + uMVP3.w);

    vTexCoord = dilatedTex;
    vBanding = InBanding;
    vGlyphLoc = InGlyphLoc;
    vBandMaxWord = InBandMaxWord;
    vColor = InColor;
    vScreenPos = pos;
    vScreenRect = InScreenRect;
    vPaintIndex = InPaintIndex;
}
