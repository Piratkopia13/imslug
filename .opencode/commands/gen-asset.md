---
description: Regenerate the committed packed test asset (imslug_gen)
---

The packed test asset regeneration just ran:

!`scripts/gen_asset.bat`

This runs the offline generator (`build/imslug_gen`) over
`tests/fonts/Roboto-Regular.ttf` with the pinned parameters
(`--size-range 8:72 --chars 0x20-0x7E`) and overwrites
`tests/assets/Roboto-ASCII.imslug` plus the two generated headers the GLFW
example embeds as its default fonts:
`example/glfw_vulkan/imslug_example_asset.h` (the packed asset) and
`example/glfw_vulkan/imslug_example_font.h` (the raw TTF, via
`imslug_gen embed`, for ImGui's stock atlas loader). All three are generated
artifacts (like `shaders/imslug_shaders_vulkan.h`): never hand-edit them, and
commit them together with the generator/format/version change that made them
stale.

`imslug_packed_roundtrip` (in-process writer vs runtime TTF generation, bitwise)
and `imslug_asset_fresh` (the CLI regenerates and byte-compares the asset and
both example headers) fail when a committed artifact is stale - run
`scripts/test.* -R packed` afterwards, then `scripts/check_docs.*` as usual.

On Linux/macOS the entry point is `scripts/gen_asset.sh`.
