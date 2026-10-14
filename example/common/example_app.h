// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// example/common/example_app.h — shared example application (platform-free).
//
// The options, state, scene set and per-frame ImGui builder shared by the
// platform shells: example/glfw_vulkan/main.cpp (GLFW + Vulkan) today and the
// WebGPU browser shell later. The shell owns its window/canvas, its GPU
// context and everything desktop-only (the --screenshot/--novsync/--help CLI,
// GPU timestamps, present-mode control, validation layers); it implements the
// ExamplePlatform hooks and calls ExampleAppInit once before the first frame.
// This header stays free of GLFW/Vulkan/WebGPU so a shell without a
// filesystem or a GPU timestamp readout can reuse the same frame builder and
// the same scene set:
//   * --font / IMSLUG_EXAMPLE_FONT are resolved by the shell, which passes
//     ExampleAppOptions::FontPath / FontPathIsPacked (null = the embedded
//     defaults a web build always uses);
//   * the fetched-font probes in the CJK/emoji/packed scenes are guarded with
//     #ifndef __EMSCRIPTEN__ and degrade to the embedded asset/message;
//   * desktop-only UI rows are hidden when their hook is null.
//
// The scene switcher stays the source of truth for keys 1-9/P, +/- and the
// M toggle (it polls ImGui keys); the shell maps its platform keys onto the
// ExampleApp* actions below for Esc/F1-F3/M/0.

#pragma once

#include "imgui.h"

#include <stdint.h>

// Window defaults: the shell creates its window/canvas with these.
constexpr int kExampleWindowWidth = 1280;
constexpr int kExampleWindowHeight = 800;
constexpr const char* kExampleWindowTitle = "imslug - Slug text";

// Scene set / font-size limits (CLI + switcher).
constexpr int kExampleSceneCount = 10;
constexpr int kExampleMinSize = 12;
constexpr int kExampleMaxSize = 48;
constexpr int kExampleDefaultSize = 17;
constexpr int kExamplePerfHistory = 240;  // frame-ms ring for the performance panel

// Logical debug-overlay flags; the shell maps them to its backend module's
// constants (ImSlugVulkanDebugOverlay_* / the WebGPU mirror).
enum ExampleDebugOverlay
{
    ExampleDebugOverlay_GlyphBounds = 1u << 0,
    ExampleDebugOverlay_BandGrid = 1u << 1,
    ExampleDebugOverlay_RefHeat = 1u << 2
};

// Options: the shell fills these from the command line (ExampleAppParseArg
// recognizes the shared flags) and from its own desktop-only flags, then
// ExampleAppInit copies and clamps them into the shared state.
struct ExampleAppOptions
{
    const char* FontPath;    // TTF/.imslug override; null = the embedded defaults
    bool FontPathIsPacked;   // FontPath is a packed `.imslug` asset (shell-sniffed)
    int BaseSize;            // base font size in px
    int SceneNumber;         // 1-based scene from the CLI (0 = default)
    float DpiScale;          // style.FontScaleDpi
    bool AtlasText;          // start with ImGui's dynamic fonts (--renderer atlas)
    uint32_t DebugOverlay;   // initial ExampleDebugOverlay flags (--overlay)
    bool DebugOverlayGiven;  // --overlay was passed (else the Config seed stays)
};

// Platform services: the shell installs them once (ExampleAppInit). A null
// hook is the seam for a capability a shell cannot provide (e.g. the web
// shell has no GPU timestamps and no vsync toggle): the shared UI hides the
// corresponding row instead of showing a wrong state.
struct ExamplePlatform
{
    // imslug backend debug overlay (get/apply the logical flags). Required.
    uint32_t (*GetDebugOverlay)(void* user);
    void (*SetDebugOverlay)(void* user, uint32_t flags);
    // The switcher's "validation: <state>" line; null = the line is hidden.
    const char* (*GetValidationState)(void* user);
    // Performance panel: whole-frame GPU time from timestamp queries. Returns
    // false when the queue has no timestamps (the panel prints "n/a");
    // *outSlots == 0 while the first samples are still warming up.
    bool (*GetGpuFrameMs)(void* user, float* outMs, int* outSlots);
    // Performance panel: the vsync checkbox and the present-mode label.
    bool (*GetVsync)(void* user);
    void (*ToggleVsync)(void* user);
    const char* (*GetPresentModeName)(void* user);
    void* user;
};

// The single shared instance (the scenes are laid out like the original
// example's global App: one process, one window).
struct ExampleAppState
{
    ImFont* SlugFont;    // rendered by Slug
    ImFont* StockFont;   // rendered through the atlas (mixed-font scene)
    ImFont* CjkFont;     // fetched CJK subset (stress scene; may stay null)
    ImFont* EmojiFont;   // fetched Noto Color Emoji (color scene; may stay null)
    ImFont* PackedFont;  // pre-baked .imslug asset (packed scene; may stay null)
    bool CjkAttempted;
    bool EmojiAttempted;
    bool PackedAttempted;
    bool PackedFromFile;  // packed scene: loaded the committed file, not the embedded copy
    float EmojiSize;      // emoji scene: emoji text size (12..96 px)
    float EmojiZoom;      // emoji scene: single-glyph zoom (32..220 px)
    bool FontsDirty;      // re-add fonts at the current size next frame
    int BaseSize;
    int Scene;
    float DpiScale;
    bool ShowDemoStats;

    // Text renderer: false = Slug registered fonts, true = the stock
    // registration through ImGui's dynamic font system (--renderer, M).
    bool AtlasText;

    // Performance panel: wall-clock frame ms ring.
    float FrameMsHistory[kExamplePerfHistory];
    int FrameMsCursor;
    int FrameMsCount;

    // Draw-data shape captured right after ImGui::Render() (the panel runs
    // while the live draw data is rebuilt, so it reads this copy).
    int LastCmdLists;
    int LastDrawCommands;
    int LastVtxCount;
    int LastIdxCount;

    // Heavy demo scene state (sliders; the text wall rebuilds on change).
    int PerfLines;
    int PerfChars;
    float PerfScale;

    // Font override (--font / IMSLUG_EXAMPLE_FONT), resolved by the shell;
    // null = the fonts embedded in the binary. FontPathIsPacked selects the
    // stock atlas source exactly like the original in-process file sniffing.
    const char* FontPath;
    bool FontPathIsPacked;

    // Frame/shutdown flow: the shell increments FramesRendered per presented
    // frame (it also drives the widgets plot) and checks ShouldClose.
    int FramesRendered;
    bool ShouldClose;
};

// The shared instance the shell and the scenes read/write.
ExampleAppState* ExampleAppGetState();

// Defaults matching the standalone example's CLI defaults.
void ExampleAppOptionsDefaults(ExampleAppOptions* options);

// Parses one shared CLI flag (and its value). Returns the number of argv
// entries consumed (1 or 2), or 0 when argv[argIndex] is not a shared flag:
// the shell then tries its own flags and reports unknown ones. Malformed
// values print the same diagnostics as before and exit(1). The shared flags
// are --font, --size, --renderer, --scene, --dpi and --overlay.
int ExampleAppParseArg(int argc, char** argv, int argIndex, ExampleAppOptions* options);

// Installs the shared state: copies/clamps the options, stores the platform
// hooks. Call after the shell's CLI parsing and before the first frame (the
// fonts are added later, when the shell sets FontsDirty after its backend
// module exists).
void ExampleAppInit(const ExampleAppOptions* options, const ExamplePlatform* platform);

// Re-adds the Slug/atlas fonts at the current base size (shell: when
// FontsDirty); a failed Slug font sets ShouldClose.
void ExampleAppRebuildFonts();

// One ImGui frame: NewFrame, the scene switcher and the active scene.
void ExampleAppBuildFrame();

// Captures the draw-data shape for the performance panel; call right after
// ImGui::Render() and before the Slug pre-pass rewrites the draw data.
void ExampleAppCaptureDrawStats();

// Shared key actions (the shell maps its platform keys onto these); the
// scene/renderer/size keys are polled by the switcher itself.
bool ExampleAppToggleDebugOverlay(uint32_t flag);
void ExampleAppSetTextRenderer(bool atlas);
void ExampleAppResetFonts();  // base size back to the default, DPI 1.0
