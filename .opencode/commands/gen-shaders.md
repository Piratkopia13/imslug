---
description: Verify the committed SPIR-V header is fresh (regenerate with the same script)
---

Freshness check for the committed shader header just ran:

!`scripts/gen_shaders.bat --check`

(Expected in M0: "no shader sources yet (M2)".)

If the check failed, or you changed anything under `shaders/`:

1. run `scripts/gen_shaders.bat` to regenerate `shaders/imslug_shaders_vulkan.h`;
2. review the diff - a churn without a source change is a generator bug, report it;
3. commit the header together with its shader sources.

Never hand-edit the generated header (AGENTS.md invariant 1) and never commit
generated `.spv` intermediates. If `glslangValidator` is reported missing:
install the Vulkan SDK and set `VULKAN_SDK`, or point `GLSLANG_VALIDATOR` at the
executable. On Linux/macOS use `scripts/gen_shaders.sh`.
