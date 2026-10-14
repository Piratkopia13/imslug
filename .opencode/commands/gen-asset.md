---
description: Regenerate the committed packed test asset (imslug_gen, M7)
---

The packed test asset regeneration just ran:

!`scripts/gen_asset.bat`

This runs the M7 offline generator (`build/imslug_gen`) over
`tests/fonts/Roboto-Regular.ttf` with the pinned parameters
(`--size-range 8:72 --chars 0x20-0x7E`) and overwrites
`tests/assets/Roboto-ASCII.imslug`. The asset is a generated artifact (like
`shaders/imslug_shaders_vulkan.h`): never hand-edit it, and commit it together
with the generator/format/version change that made it stale.

`imslug_packed_roundtrip` (in-process writer vs runtime TTF generation, bitwise)
and `imslug_asset_fresh` (the CLI regenerates and byte-compares) both fail when
the committed asset is stale - run `scripts/test.* -R packed` afterwards, then
`scripts/gen_shaders.* --check`/`scripts/check_docs.*` as usual.

On Linux/macOS the entry point is `scripts/gen_asset.sh`.
