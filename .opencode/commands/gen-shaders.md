---
description: Verify both committed shader headers are fresh (SPIR-V + WGSL; regenerate with the same script)
---

Freshness check for both committed shader headers just ran:

!`scripts/gen_shaders.bat --check`

If the check failed, or you changed anything under `shaders/`:

1. run `scripts/gen_shaders.bat` to regenerate `shaders/imslug_shaders_vulkan.h`
   (SPIR-V, from `shaders/vulkan/`) and `shaders/imslug_shaders_webgpu.h`
   (embedded NUL-terminated WGSL, from `shaders/wgsl/`; pure python);
2. review the diff - a churn without a source change is a generator bug, report it;
3. commit each header together with its shader sources (invariant 8).

The SPIR-V half needs `glslangValidator` - install the Vulkan SDK and set
`VULKAN_SDK`, or point `GLSLANG_VALIDATOR` at the executable. A half whose source
directory is empty is skipped with a message. Never hand-edit either generated
header (AGENTS.md invariant 1) and never commit generated `.spv` intermediates.
On Linux/macOS use `scripts/gen_shaders.sh`.
