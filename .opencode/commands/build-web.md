---
description: Build the WebGPU browser demo (Emscripten + the GitHub Pages site)
---

Web demo build entry point just ran:

!`scripts/build_web.bat`

It sources `emsdk_env.bat` when the emsdk root is known, runs `emcmake cmake`
into `build-web/` (`-DIMSLUG_BUILD_EXAMPLES=ON -DIMSLUG_BACKEND_VULKAN=OFF
-DIMSLUG_BUILD_TESTS=OFF -DIMSLUG_BUILD_C_EXAMPLE=OFF -DIMSLUG_BUILD_TOOLS=OFF`),
builds the `imslug_example_web` target and assembles `build-web/site/`
(`index.html`, `index.js`, `index.wasm`, `.nojekyll`). When the fetched CJK
subset and a native `imslug_gen` are present it first embeds the subset through
`imslug_gen embed` and passes `-DIMSLUG_CJK_EMBED_HEADER=...`; otherwise the
demo builds with the embedded Roboto fonts only and the CJK scene shows its
"font not embedded" message.

Report: the configure defines actually used, the built target, the site path,
and whether the CJK subset was embedded. On failure, quote the first real
compiler/linker error (Emscripten port drift usually shows up as an
`emdawnwebgpu`/`contrib.glfw3` include or link error - see the
`imslug-webgpu-backend` skill before guessing).

To verify the page in a real browser, run `scripts/web_smoke.sh` (Linux/macOS)
or `node scripts/web_smoke/smoke.mjs` (any OS with Node + Chrome).

On Linux/macOS the entry point is `scripts/build_web.sh`.
