---
name: Bug report
about: Something renders wrong, crashes, or misbehaves
title: ''
labels: bug
assignees: ''
---

## What happened

<!-- What did you see, and what did you expect instead? -->

## Reproduction

<!-- Minimal steps. If it is a rendering issue, a --screenshot PPM from the
     example app and the exact flags (scene number, --dpi, --overlay) help a
     lot. -->

## Environment

- imslug version (`IMSLUG_VERSION`, `ImSlug::GetVersion()`, or `imslug_gen --version`):
- Dear ImGui version/tag (or commit):
- OS + compiler:
- GPU + driver version:
- Vulkan SDK / loader version, validation layers enabled?
- Build options (`IMSLUG_TTF_PARSER`, `IMSLUG_BUILD_EXAMPLES`, `IMSLUG_IMGUI_TAG`, ...):
- If the example app reproduces it: scene number and command line:

## Evidence

<!-- Validation-layer output, stderr, screenshots, crash stack, statistics
     (ImSlug::GetStatistics). Redact anything private. -->

```
```

## Checklist

- [ ] I searched existing issues for duplicates.
- [ ] I can reproduce this with the current `main` (or state the exact version).
- [ ] I ran with the Vulkan validation layers enabled (default in the example).
