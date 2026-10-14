---
name: imslug shaders
description: Shader sources of truth, the glslangValidator workflow, the committed SPIR-V header and its freshness check, attribution headers, and golden tests. Load when editing shaders/**, running scripts/gen_shaders.*, or porting reference shader code.
---

# imslug shaders

## Sources of truth

| Path | Role |
|---|---|
| `shaders/vulkan/slug.vert`, `slug.frag` | Authoritative GLSL (Vulkan dialect). Compiled by `scripts/gen_shaders.*`. |
| `shaders/reference/slug.hlsl` | Attributed verbatim copy of Eric Lengyel's reference VS+PS plus the port mapping table (reference function ↔ GLSL function, deliberate deviations). Never compiled. |
| `shaders/imslug_shaders_vulkan.h` | **Generated + committed** SPIR-V word arrays (`imslug_slug_vert_spv` / `imslug_slug_frag_spv` + `_len` word counts). Never edited by hand. |

Generated `.spv` intermediates are gitignored; only the header is committed
together with its sources (PLAN §5.8, AGENTS.md invariant 8).

## Regenerate / verify

* Regenerate: `scripts\gen_shaders.bat` (Windows) / `scripts/gen_shaders.sh`.
* Verify the committed bytes are fresh: add `--check` - it compiles into a temp
  file and diffs against the committed header; it fails when that header is
  missing or stale. Fix by regenerating and committing the header **with** the
  sources.
* Tool resolution: `GLSLANG_VALIDATOR` env var, then `$VULKAN_SDK/Bin` (Windows)
  / `$VULKAN_SDK/bin` (Linux), then `PATH`. A missing tool fails with an
  actionable message.
* Invocation used by both scripts (keep in sync):
  `glslangValidator -V --target-env vulkan1.2 -o <tmp>/<name>.<stage>.spv <src>`
* `--target-env vulkan1.2` emits **SPIR-V 1.5**, which needs an instance/device
  apiVersion of at least 1.2 in the consuming app (Vulkan 1.1 alone rejects it:
  `VUID-VkShaderModuleCreateInfo-pCode-08737`). The example and the golden
  harness use 1.2 (1.3 for dynamic rendering).
* The emitter (`scripts/emit_shader_header.py`) writes, per module,
  `imslug_<stem>_<stage>_spv[]` plus `..._spv_len`. Output is deterministic:
  sorted inputs, no timestamps, LF, 8 words per line. A diff that churns without
  a source change is a generator bug - report it, never hand-patch the header.

## Attribution rules (hard requirement: PLAN §1.2, R10)

* Every shader source file starts with:
  `// Slug shader code Copyright 2017 by Eric Lengyel.`
  plus a short note on what was ported/adapted and under which license
  (the reference is dual MIT / Apache-2.0; both texts live in `NOTICE` §1).
* Port checklist (PLAN Appendix B): `SlugUnpack`/`SlugDilate` (perspective-correct
  1 px dilation via MVP rows, sample coordinate re-derived through the inverse
  Jacobian), `CalcBandLoc` (log2 width 12 / 4096), band header fetch (H then V
  offsets), `CalcRootCode` equivalence classes + linear fallback when
  `|a| < 1/65536` (with the CPU arbiter's `|b| >= 1/65536` guard, R1),
  `SolveHorizPoly`/`SolveVertPoly` winding accumulation with
  coverage weights, `CalcCoverage` combine + even-odd flag, instance-mode VS
  (corner from `gl_VertexIndex`, normal = corner direction), ImGui-exact blend
  state, discard below `fill < 0.001`. All of these are implemented as of M2.
* Do not copy code from projects without a license (`osgSlug`). DearWidgets is
  CC0 and Slughorn is MIT - adapting anything requires a `NOTICE` entry and a
  credit line in the file header.

## Implemented interface (M2; keep in sync with the code)

**Object space = screen pixels (Y down).** The instance carries a screen rect
(px) and an em bbox; the VS corner `c = (gl_VertexIndex & 1, (gl_VertexIndex >>
1) & 1)` drives *both*: `tex = mix(EmBBox.xy, EmBBox.zw, c)` and
`pos = (mix(ScreenMinX, ScreenMaxX, c.x), mix(ScreenMaxY, ScreenMinY, c.y))`
(the Y flip lives here: em max Y is the screen rect's top). The outward corner
direction is `dir = (2 c.x - 1, 1 - 2 c.y)` - **unnormalized** (±1, ±1), like
the reference demo's quad normals; `SlugDilate` normalizes it for the scale
factor and applies it unnormalized, so each corner moves exactly ±0.5 px per
axis in the ortho case (fragment region = rect ± 0.5 px per axis).

**Vertex shader inputs** (one instance record, `VK_VERTEX_INPUT_RATE_INSTANCE`,
`ImSlugGlyphInstance` in `imslug_internal.h`, 92 bytes, mirrored comments):

| loc | GLSL type | VkFormat | field |
|---|---|---|---|
| 0 | vec4 | R32G32B32A32_SFLOAT | EmBBox (minX, minY, maxX, maxY), Y up |
| 1 | vec4 | R32G32B32A32_SFLOAT | ScreenRect (minX, minY, maxX, maxY), Y down |
| 2 | vec4 | R32G32B32A32_SFLOAT | Jac (inverse Jacobian rows) |
| 3 | vec4 | R32G32B32A32_SFLOAT | Banding (scaleX, scaleY, offsetX, offsetY) |
| 4 | uvec2 | R32G32_UINT | GlyphLoc (band pool x, y) |
| 5 | uint | R32_UINT | BandMaxWord (bandMaxX \| bandMaxY<<16 \| E<<28) |
| 6 | vec4 | R32G32B32A32_SFLOAT | Color (straight alpha) |

**Push constants** (72 bytes → **76 bytes in M4**): 4 MVP rows (`vec4`) mapping
*object space (screen px) to clip*, then `vec2 Viewport` = framebuffer size in
pixels (SlugDilate's `dim`), then `uint DebugFlags` (M4 debug overlay,
`ImSlugVulkanDebugOverlay`; 0 = off). The block is declared in **both** stages
(the fragment shader reads only `DebugFlags`), so the pipeline layout's push
constant range uses `VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT`
and `vkCmdPushConstants` must name both (VUID-...-07987 / 01796 - a vertex-only
range or a vertex-only push is a validation error since M4).

**Varyings**: location 0 `vec2 vTexCoord` (em sample coordinate, interpolated),
1 `flat vec4 vBanding`, 2 `flat uvec2 vGlyphLoc`, 3 `flat uint vBandMaxWord`,
4 `vec4 vColor`, 5 `vec2 vScreenPos` (undilated object-space position, M4 -
drives the glyph-bounds outline), 6 `flat vec4 vScreenRect` (the instance's
source quad, M4). Locations 5/6 are only read by the debug views.

**Fragment shader**: `fwidth(vTexCoord)` gives `pixelsPerEm`; `texelFetch` on
`sampler2D curveTexture` (RGBA16F) and `usampler2D bandTexture` (R16G16_UINT,
`texelFetch` returns `uvec4`, the two u16 fields are `.xy`); `CalcCoverage`'s
even-odd test is **bit 28 of the packed bandMax word** (= bit 12 of its high
half, the reference's `glyphData.w` test; PLAN §11 D8(g)) - TTF glyphs use the
nonzero rule (E = 0). Output is **unpremultiplied**:
`vec4(vColor.rgb, vColor.a * fill)` with `discard` when `fill < 0.001`.

**M4 debug overlay (shader side, PLAN §11 D17).** When `pc.DebugFlags != 0` the
shader returns a debug color and never discards, so the whole dilated quad
stays visible: `RefHeat` (bit 2) maps the band-reference walk count of the
fragment to a blue→red ramp (`refCount / 16`), `BandGrid` (bit 1) mixes a
checkerboard of the H/V band indexes into the glyph color, `GlyphBounds` (bit 0)
paints a ~1 px outline wherever the undilated `vScreenPos` is within 1 px of the
source quad edges (`vScreenRect`). `RefHeat` wins over `BandGrid`. The flags
must not change any geometry, placement or the fallback behavior - the golden
test renders the acceptance scene with the seeded bounds and both band views
and checks the outline/grid/heat appear while plain-frame ink never disappears.

**Blend state** (pipeline, not shader): exactly stock `imgui_impl_vulkan` -
color `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`, alpha `ONE`/`ONE_MINUS_SRC_ALPHA`, op
ADD. Because that is a *straight-alpha* blend, the reference's premultiplied
`return vresult.color * coverage` would square the edge coverage; the GLSL
scales the alpha only. (The reference demo pairs its premultiplied output with
a premultiplied blend state.)

## Vulkan conventions that bite (learned porting, keep)

* **Vulkan NDC y points *down*** (row 0 = ndc.y −1). A screen-pixel-space (Y
  down) ortho MVP therefore needs **no** y negation: row 1 = `(0, 2/H, 0, −1)`.
  Negating it (the OpenGL/D3D habit) mirrors the glyphs vertically - an 'H'
  render still looks plausible, which makes this easy to miss; compare against
  the CPU reference with an asymmetric glyph ('g', '@').
* The dilation is **mapping-invariant**: `d = dir * (s²(st + sqrt(uv))/(uv −
  st²))` with `J·d` added to the sample coordinate leaves the affine
  screen→em map unchanged for any `d`; only the fragment region grows.
* GLSL has `fract`, not HLSL's `frac`; `clamp(x,0,1)` is the `saturate`.
* `normalize(vec2(0,0))` is undefined; the shader never sees a zero `dir`.
* The exactly-degenerate linear solve (all control-point y equal) can produce
  `c * (0.5/0)` inf/NaN in the reference form. Band generation excludes
  axis-parallel *fp32* curves from the matching-axis bands, but the pools store
  **fp16**: quantization can make an admitted curve all-equal on one axis
  (review R1). The GLSL therefore carries the CPU arbiter's Slughorn guard in
  the identical form (`|b| >= 1/65536 ? c * (0.5/b) : 0`, see
  `imslug_refraster.cpp`) so GPU and CPU resolve that branch the same way; the
  scan in `tests/test_glyphgen.cpp` reports quantized collisions per font
  (currently 0 for Roboto and Inter, but a new font can trigger it - keep the
  guard).

## Verification after shader changes

1. `scripts/gen_shaders.*` - must succeed.
2. `scripts/gen_shaders.* --check` - must pass on a clean tree.
3. Golden-image tests (`scripts/golden.*`): fixed strings at sizes 12/16/32/64
   and scales 1.0/1.25/2.0 (+ a 3.5x zoom case), GPU vs the M1 CPU rasterizer,
   both the render-pass and dynamic-rendering paths, validation layers on (any
   validation error fails the run). They skip with exit 77 without a device.
4. Fallback check: unregistered fonts still render through ImGui's texture path
   (the non-Slug path must never depend on shader changes).
