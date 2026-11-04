---
name: imslug opengl backend
description: OpenGL 3.2+ / OpenGL ES 3.0+ backend contract for imslug - the frozen standalone core (textures, VBOs, instancing, state save/restore) and the ImGui adapter, plus C11 bindings, GL loader mechanics, shader generation, and test suites. Load when working in backends/imslug_impl_opengl3.*, backends/imslug_c_opengl3.cpp, shaders/opengl3/**, or tests/golden/test_*opengl3.cpp.
---

# imslug OpenGL 3.2+ / OpenGL ES 3.0+ backend contract

Two layers:

* **Standalone core** — `backends/imslug_impl_opengl3.*`: the ImGui-free OpenGL core
  (pool textures, uploads, instanced VBO, shader program, VAO, and draw commands
  dispatched with caller-provided viewport and scissor settings). Minimal dynamic
  GL loader resolves entry points via a caller-supplied function pointer resolver
  (e.g. `glfwGetProcAddress`, `SDL_GL_GetProcAddress`, `eglGetProcAddress`,
  `wglGetProcAddress`, or `glXGetProcAddress`); no third-party loader headers
  (such as `glad`, `glew`, etc.) are exposed or required.
* **ImGui adapter** — the `ImSlug_ImplOpenGL3_*` interface in `backends/imslug_impl_opengl3.*`:
  renders `ImDrawData` seamlessly before or alongside `ImGui_ImplOpenGL3_RenderDrawData`,
  uploading pool updates, setting projection matrices, and dispatching text batches.
* **C API mirror** — `backends/imslug_c_opengl3.cpp` with `IMSLUG_C_OPENGL3` in `imslug_c.h`:
  pure C11 function declarations and types for all core and adapter calls.

OpenGL code lives in `backends/` only; the core remains backend-independent
(AGENTS.md invariant 7). Authoritative shaders live in `shaders/opengl3/` and
generate the committed header `shaders/imslug_shaders_opengl3.h`.

## Frozen core API (`backends/imslug_impl_opengl3.h`)

```cpp
typedef void (*ImSlugOpenGL3Proc)(void);
typedef ImSlugOpenGL3Proc (*ImSlugOpenGL3GetProcAddressFn)(const char* procName, void* userData);

struct ImSlugOpenGL3InitInfo
{
    const char*                   GlslVersion;          // e.g. "#version 150", "#version 330 core", "#version 300 es", or nullptr (auto-detect)
    uint32_t                      MaxInstancesPerFrame; // 0 => 16384 (adaptive growth); nonzero = hard cap
    ImSlugOpenGL3GetProcAddressFn GetProcAddress;       // optional: custom proc resolver
    void*                         UserData;             // optional: user data passed to GetProcAddress
};

bool ImSlugOpenGL3Init(const ImSlugOpenGL3InitInfo* info);
void ImSlugOpenGL3Shutdown();

// Uploads curve and band pool updates to GL textures. Must run before drawing.
bool ImSlugOpenGL3UploadPools(ImSlugPools* pools);

uint32_t ImSlugOpenGL3GetFramesInFlight();
uint32_t ImSlugOpenGL3GetInstanceCapacity();
ImSlugGlyphInstance* ImSlugOpenGL3GetInstanceData(uint32_t frameIndex);
bool ImSlugOpenGL3ReserveInstances(uint32_t frameIndex, uint32_t requiredInstances);
void ImSlugOpenGL3UploadInstances(uint32_t frameIndex, uint32_t instanceCount);

struct ImSlugOpenGL3DrawInfo
{
    float        MVP[16];          // 4x4 row-major projection matrix
    float        ViewportWidth;    // viewport width in pixels
    float        ViewportHeight;   // viewport height in pixels
    int          ScissorX;
    int          ScissorY;
    uint32_t     ScissorW;
    uint32_t     ScissorH;
    uint32_t     FrameIndex;
    uint32_t     FirstInstance;
    uint32_t     InstanceCount;
    uint32_t     DebugFlags;       // ImSlugOpenGL3DebugOverlay bitmask
};

void ImSlugOpenGL3Draw(const ImSlugOpenGL3DrawInfo* drawInfo);

bool ImSlugOpenGL3CreateDeviceObjects();
void ImSlugOpenGL3DestroyDeviceObjects();

void ImSlugOpenGL3BuildOrthoMVP(
    float left, float top, float right, float bottom,
    float outMVP[16],
    bool flipY = false);
```

## ImGui adapter API (`backends/imslug_impl_opengl3.h`)

```cpp
bool     ImSlug_ImplOpenGL3_Init(const char* glsl_version = nullptr);
bool     ImSlug_ImplOpenGL3_InitFromInfo(const ImSlugOpenGL3InitInfo* info);
void     ImSlug_ImplOpenGL3_Shutdown();
bool     ImSlug_ImplOpenGL3_UploadPools();
void     ImSlug_ImplOpenGL3_RenderDrawData(ImDrawData* draw_data);
void     ImSlug_ImplOpenGL3_SetDebugOverlay(uint32_t overlay_flags);
uint32_t ImSlug_ImplOpenGL3_GetDebugOverlay();
```

## Texture pool layouts

1. **`curveTexture`** (Texture Unit 0, `sampler2D curveTexture`):
   - Internal format: `GL_RGBA32F`
   - Dimensions: 4096 texels wide $\times$ `Curves.Capacity` rows
   - Filtering: `GL_NEAREST`, clamp to edge
   - Pixel data: packed Bézier control points in 32-bit floating point.
2. **`bandTexture`** (Texture Unit 1, `usampler2D bandTexture`):
   - Internal format: `GL_RG16UI`
   - Dimensions: 4096 texels wide $\times$ `Bands.Capacity` rows
   - Filtering: `GL_NEAREST`, clamp to edge
   - Pixel data: band header and curve index lists.
3. **`paintTexture`** (Texture Unit 2, `usampler2D paintTexture`):
   - Internal format: `GL_RGBA32UI`
   - Dimensions: 22 texels wide (352 bytes per record) $\times$ `Paints.Capacity` rows
   - Filtering: `GL_NEAREST`, clamp to edge
   - Pixel data: COLR v1 gradient records (color stops, transforms, radii, angles).

## Instancing and vertex attributes

Quads are rendered with `glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count)`.
Attribute locations are bound explicitly prior to linking via `glBindAttribLocation`:

| Location | Name | Type | Divisor | Description |
|---|---|---|---|---|
| 0 | `inCorner` | `vec2` | 0 | Unit quad corner coordinate `(0..1, 0..1)` |
| 1 | `inScreenRect` | `vec4` | 1 | Screen-space bounding rectangle `(minX, minY, maxX, maxY)` |
| 2 | `inEmRect` | `vec4` | 1 | Font design em-space box `(minX, minY, maxX, maxY)` |
| 3 | `inEmScale` | `vec2` | 1 | Font units-per-em inverse scale `(1/upem, 1/upem)` |
| 4 | `inGlyphLoc` | `ivec2` | 1 | Glyph band coordinate in `bandTexture` |
| 5 | `inBandMax` | `ivec2` | 1 | Band counts `(maxH, maxV)` |
| 6 | `inColor` | `vec4` | 1 | RGBA vertex tint / fallback fill color |
| 7 | `inPaintIndex` | `uint` | 1 | Index into `paintTexture` row (0 = monochrome/tint) |

## Pipeline & state management

The backend automatically saves and restores active OpenGL state during `ImSlug_ImplOpenGL3_RenderDrawData` and `ImSlugOpenGL3Draw`:
- Active program, texture unit 0/1/2 bindings, vertex array object, array buffer binding.
- Blend mode, equation, cull face, depth test, and scissor test states.
- Scissor rectangle and viewport dimensions.
- Blending mode: `glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` with `glBlendEquation(GL_FUNC_ADD)` (straight-alpha compositing).

## Tests

- `imslug_tests_golden_opengl3`: Monochrome rendering across size and scale matrix against CPU reference rasterizer.
- `imslug_tests_imgui_golden_opengl3`: End-to-end ImGui integration at 1x, 2x, 3x framebuffer scales + debug overlay verification.
- `imslug_tests_emoji_golden_opengl3`: COLR v1 color font rendering and gradient evaluation verified against CPU paint rasterizer.
