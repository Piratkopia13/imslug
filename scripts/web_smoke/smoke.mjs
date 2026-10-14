#!/usr/bin/env node
// imslug web smoke - boots the assembled web demo in headless Chrome, waits for
// real rendered frames, checks the console and asserts the canvas is not blank.
//
// It is the CI gate for the GitHub Pages job (scripts/web_smoke.sh) and usable
// locally on any OS with Node >= 22 and Chrome installed:
//
//   npm --prefix scripts/web_smoke ci        # once
//   node scripts/web_smoke/smoke.mjs         # serves build-web/site by default
//
// The page must publish the imslug web-shell hooks (example/web/shell.html):
//   window.__imslugFrames (int)   rendered frames since boot
//   window.__imslugReady  (bool)  true after the first successful frame
//   window.__imslugError  (str)   non-empty on a fatal init error
//
// The default run is two passes: the primary pass at deviceScaleFactor 1 and a
// hi-DPI pass at IMSLUG_SMOKE_HIDPI_DPR (default 3, the phone/retina case).
// The hi-DPI pass presses the shared app's M toggle (Slug -> ImGui atlas) and
// requires the two frames to show the same ink layout: Slug text drawn in the
// wrong coordinate space (the framebuffer-scale regression) loses most of its
// ink and fails the comparison even though the canvas is far from blank.
//
// Env overrides:
//   IMSLUG_SMOKE_URL      page URL to test (default: serve IMSLUG_SMOKE_SITE)
//   IMSLUG_SMOKE_SITE     site root to serve      (default: build-web/site)
//   IMSLUG_SMOKE_OUT      screenshot path         (default: build-web/smoke.png;
//                         other passes insert their label: smoke-hidpi.png)
//   IMSLUG_SMOKE_DPR      primary pass deviceScaleFactor (default 1)
//   IMSLUG_SMOKE_HIDPI    "1" (default) adds the hi-DPI pass; "0" disables
//   IMSLUG_SMOKE_HIDPI_DPR  hi-DPI deviceScaleFactor (default 3)
//   IMSLUG_SMOKE_COMPARE_RENDERERS  "1" also runs the Slug-vs-atlas ink
//                         comparison on the primary pass (the hi-DPI pass
//                         always runs it)
//   IMSLUG_SMOKE_CHROME   Chrome/Chromium executable
//   IMSLUG_SMOKE_ADAPTER  "swiftshader" (default; software, local-friendly) or
//                         "native" (real GPU) or "lavapipe" (software Dawn
//                         over Mesa lavapipe; run under `xvfb-run -a` with
//                         VK_DRIVER_FILES pointing at the lavapipe ICD - the
//                         GitHub-runner recipe, since SwiftShader devices
//                         occasionally die there)
//   IMSLUG_SMOKE_FRAMES   frames to wait for      (default: 60)
//   IMSLUG_SMOKE_TIMEOUT_MS  frames timeout       (default: 180000)
//   IMSLUG_SMOKE_DWELL_MS    wait when NO_HOOKS=1 (default: 8000)
//   IMSLUG_SMOKE_RETRIES  whole-run retries (default: 0; CI uses 2 - software
//                         WebGPU on shared runners occasionally loses its
//                         device mid-startup, and a fresh browser recovers)
//   IMSLUG_SMOKE_NO_HOOKS 1   do not require the in-page hooks (generic WebGPU
//                             page smoke: just dwell, screenshot, check pixels)

import process from "node:process";
import { existsSync } from "node:fs";
import { mkdir, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import puppeteer from "puppeteer-core";
import { startServer } from "./serve.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..", "..");

function env(name, fallback) {
    const value = process.env[name];
    return value === undefined || value === "" ? fallback : value;
}

function findChrome() {
    const override = env("IMSLUG_SMOKE_CHROME", "");
    if (override) {
        return override;
    }
    const candidates = {
        win32: [
            "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
            "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
            "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
            "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
        ],
        darwin: [
            "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
            "/Applications/Chromium.app/Contents/MacOS/Chromium",
        ],
        linux: [
            "/usr/bin/google-chrome",
            "/usr/bin/google-chrome-stable",
            "/usr/bin/chromium",
            "/usr/bin/chromium-browser",
        ],
    }[process.platform] || [];
    return candidates.find((path) => existsSync(path)) || "";
}

function fail(message) {
    console.error(`[web_smoke] FAIL: ${message}`);
    process.exitCode = 1;
}

// ---- Slug-vs-atlas comparison budgets ---------------------------------------
// A Slug frame and the same layout through ImGui's font atlas must show the
// same ink: the native ImGui golden test documents the expected AA-only
// differences (layout within 1 px). The comparison masks use a *bright* ink
// threshold so the text dominates the metric (the dim window chrome and fills
// would otherwise dilute it by an order of magnitude), and both budgets are
// relative to the ink of the two frames, so they are viewport-size independent.
// Calibration on the demo scene (SwiftShader; 2400x1500 @ DPR 3, and the same
// numbers at DPR 1 within a few percent):
//   healthy:  miss/ink 4.7%, diff/ink 22%, slug ink = 99.8% of atlas ink
//   broken:   miss/ink 79%,  diff/ink 95%, slug ink = 13% of atlas ink
// A wrong Slug coordinate space loses the Slug text, which trips the ink-ratio
// gate as well as the misses; the atlas ink floor rejects a frame that has no
// text to compare in the first place.
const kComparisonInkLuma = 100;
const kMinComparisonAtlasInkFraction = 0.005;
const kMinComparisonSlugInkRatio = 0.5;
const kMaxRendererMissRelativeToInk = 0.25;
const kMaxRendererDiffRelativeToInk = 0.70;

// ---- Screenshot statistics --------------------------------------------------
// The slug-vs-atlas comparison runs in the analyzer page (Node has no image
// dependencies). It builds the bright-ink masks of the two frames, dilates each
// by one pixel and reports how much ink has no counterpart nearby: ink layout
// agreement with a 1 px tolerance for AA differences, the same metric the
// native ImGui golden test uses, plus the per-pixel luma delta and the
// text-presence ink ratio.
function browserAnalyzer(page, payload) {
    return page.evaluate(async (data) => {
        async function loadImage(base64) {
            const image = new Image();
            image.src = "data:image/png;base64," + base64;
            await image.decode();
            const canvas = document.createElement("canvas");
            canvas.width = image.naturalWidth;
            canvas.height = image.naturalHeight;
            const ctx = canvas.getContext("2d", { willReadFrequently: true });
            ctx.drawImage(image, 0, 0);
            const pixels = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
            return { width: canvas.width, height: canvas.height, pixels };
        }

        const kBlankLuma = 24;    // any visible pixel (the "canvas is not blank" check)
        const kInkLuma = data.inkLuma;  // bright text ink (the comparison masks)
        const kDiffLuma = 32;     // per-pixel luma delta counted as "different"

        function imageStats(image, threshold) {
            const pixels = image.pixels;
            const count = pixels.length / 4;
            let sum = 0;
            let ink = 0;
            const mask = new Uint8Array(count);
            for (let i = 0; i < count; i++) {
                const luma = (pixels[i * 4] + pixels[i * 4 + 1] + pixels[i * 4 + 2]) / 3;
                sum += luma;
                if (luma > threshold) {
                    ink += 1;
                    mask[i] = 1;
                }
            }
            return { meanLuma: sum / count, inkFraction: ink / count, mask, ink };
        }

        // Square 1-px dilation (separable: rows then columns).
        function dilate(mask, width, height) {
            const rows = new Uint8Array(width * height);
            for (let y = 0; y < height; y++) {
                for (let x = 0; x < width; x++) {
                    const i = y * width + x;
                    if (mask[i] === 0 && !(x > 0 && mask[i - 1]) && !(x + 1 < width && mask[i + 1])) {
                        rows[i] = 0;
                    } else {
                        rows[i] = 1;
                    }
                }
            }
            const dilated = new Uint8Array(width * height);
            for (let y = 0; y < height; y++) {
                for (let x = 0; x < width; x++) {
                    const i = y * width + x;
                    const up = y > 0 ? i - width : -1;
                    const down = y + 1 < height ? i + width : -1;
                    if (rows[i] === 1 || (up >= 0 && rows[up] === 1) || (down >= 0 && rows[down] === 1)) {
                        dilated[i] = 1;
                    }
                }
            }
            return dilated;
        }

        const slug = await loadImage(data.slug);
        const slugStats = imageStats(slug, kInkLuma);
        const slugBlank = imageStats(slug, kBlankLuma);
        const result = {
            width: slug.width,
            height: slug.height,
            meanLuma: slugBlank.meanLuma,
            inkFraction: slugBlank.inkFraction,
            slugInkFraction: slugStats.inkFraction,
        };
        if (data.atlas) {
            const atlas = await loadImage(data.atlas);
            if (atlas.width !== slug.width || atlas.height !== slug.height) {
                result.comparisonError = `size mismatch (${slug.width}x${slug.height} vs ${atlas.width}x${atlas.height})`;
                return result;
            }
            const atlasStats = imageStats(atlas, kInkLuma);
            result.atlasInkFraction = atlasStats.inkFraction;
            result.slugInkRatio = atlasStats.ink > 0 ? slugStats.ink / atlasStats.ink : 0.0;

            const pixels = slug.pixels;
            const count = slug.width * slug.height;
            let different = 0;
            for (let i = 0; i < count; i++) {
                const lumaA = (pixels[i * 4] + pixels[i * 4 + 1] + pixels[i * 4 + 2]) / 3;
                const lumaB = (atlas.pixels[i * 4] + atlas.pixels[i * 4 + 1] + atlas.pixels[i * 4 + 2]) / 3;
                if (Math.abs(lumaA - lumaB) > kDiffLuma) {
                    different += 1;
                }
            }
            result.diffFraction = different / count;

            const dilatedSlug = dilate(slugStats.mask, slug.width, slug.height);
            const dilatedAtlas = dilate(atlasStats.mask, slug.width, slug.height);
            let missSlug = 0;  // ink only in the Slug frame
            let missAtlas = 0; // ink only in the atlas frame
            for (let i = 0; i < count; i++) {
                if (slugStats.mask[i] === 1 && dilatedAtlas[i] === 0) {
                    missSlug += 1;
                }
                if (atlasStats.mask[i] === 1 && dilatedSlug[i] === 0) {
                    missAtlas += 1;
                }
            }
            // Ink-relative misses: the honest measure of "how much of the text is
            // not where the other renderer put it", independent of viewport size.
            const combinedInk = Math.max(1, slugStats.ink + atlasStats.ink);
            result.missSlug = missSlug;
            result.missAtlas = missAtlas;
            result.missFraction = (missSlug + missAtlas) / count;
            result.missRelativeToInk = (missSlug + missAtlas) / combinedInk;
            result.diffRelativeToInk = different / combinedInk;
        }
        return result;
    }, payload);
}

// One browser run. Returns { ok, frames, ready, error, consoleErrors,
// pageErrors, stats, screenshotPath, atlasScreenshotPath }; never throws for
// page-level failures.
async function runAttempt({ executablePath, args, url, options }) {
    const result = {
        ok: false,
        label: options.label,
        dpr: options.dpr,
        frames: 0,
        ready: false,
        error: "",
        consoleErrors: [],
        pageErrors: [],
        stats: null,
        screenshotPath: options.outPath,
        atlasScreenshotPath: options.atlasOutPath,
    };

    const browser = await puppeteer.launch({
        executablePath,
        headless: options.headless,
        args,
        protocolTimeout: Math.max(options.framesTimeoutMs + 60000, 300000),
    });
    try {
        const page = await browser.newPage();
        await page.setViewport({
            width: options.viewportWidth,
            height: options.viewportHeight,
            deviceScaleFactor: options.dpr,
        });

        page.on("console", (message) => {
            if (message.type() !== "error") {
                return;
            }
            const from = message.location() && message.location().url ? message.location().url : "";
            if (from.includes("favicon")) {
                return;
            }
            result.consoleErrors.push(`${message.text()} (${from})`);
        });
        page.on("pageerror", (error) => result.pageErrors.push(String(error)));

        await page.goto(url, { waitUntil: "load", timeout: 120000 });

        if (options.noHooks) {
            await new Promise((done) => setTimeout(done, options.dwellMs));
        } else {
            await page.waitForFunction(
                `window.__imslugError || ((window.__imslugFrames || 0) >= ${options.framesTarget} && window.__imslugReady === true)`,
                { timeout: options.framesTimeoutMs, polling: 250 });
        }
        const state = await page.evaluate(() => ({
            frames: window.__imslugFrames | 0,
            ready: window.__imslugReady === true,
            error: String(window.__imslugError || ""),
        }));
        result.frames = state.frames;
        result.ready = state.ready;

        if (state.error) {
            result.error = `page reported an init error: ${state.error}`;
            return result;
        }
        if (!options.noHooks && state.frames < options.framesTarget) {
            result.error = `only ${state.frames}/${options.framesTarget} frames rendered`;
            return result;
        }

        const screenshot = await page.screenshot({ type: "png" });
        await mkdir(dirname(options.outPath), { recursive: true });
        await writeFile(options.outPath, screenshot);

        // Optional Slug-vs-atlas ink comparison (see the file header): toggle the
        // shared app's text renderer (key M) to ImGui's own font system, wait a
        // few frames and screenshot the same scene again. In Slug mode every
        // body-text glyph is an imslug instance; in atlas mode the identical
        // layout is drawn from the font atlas. Slug text placed in the wrong
        // coordinate space (framebuffer-scale bug) loses the Slug ink, so the
        // ink masks stop matching.
        let atlasScreenshot = null;
        if (options.compareRenderers && !options.noHooks) {
            try {
                await page.bringToFront();
                await page.keyboard.press("KeyM");
                await page.waitForFunction(
                    `(window.__imslugFrames | 0) >= ${state.frames + options.compareFrames}`,
                    { timeout: options.framesTimeoutMs, polling: 100 });
                atlasScreenshot = await page.screenshot({ type: "png" });
                await mkdir(dirname(options.atlasOutPath), { recursive: true });
                await writeFile(options.atlasOutPath, atlasScreenshot);
            } catch (error) {
                result.error = `Slug/atlas comparison step failed: ${error && error.message ? error.message : error}`;
                return result;
            }
        }

        // Decode the screenshot(s) in a browser (no image dependencies in Node)
        // and measure how much of them is actual ink rather than the clear color.
        const analyzer = await browser.newPage();
        const stats = await browserAnalyzer(analyzer, {
            slug: screenshot.toString("base64"),
            atlas: atlasScreenshot ? atlasScreenshot.toString("base64") : null,
            inkLuma: kComparisonInkLuma,
        });
        await analyzer.close();
        result.stats = stats;

        if (stats.comparisonError) {
            result.error = stats.comparisonError;
            return result;
        }
        if (stats.inkFraction < 0.002) {
            result.error = `canvas looks blank (ink fraction ${stats.inkFraction.toFixed(5)}, mean luma ${stats.meanLuma.toFixed(2)})`;
            return result;
        }
        if (atlasScreenshot) {
            // The atlas frame is the reference: it must show the text at all
            // (the primary ink check above only rejects an empty canvas).
            if (stats.atlasInkFraction < kMinComparisonAtlasInkFraction) {
                result.error =
                    `atlas frame has too little ink to compare (ink fraction ${stats.atlasInkFraction.toFixed(5)} < ${kMinComparisonAtlasInkFraction})`;
                return result;
            }
            if (stats.slugInkRatio < kMinComparisonSlugInkRatio ||
                stats.missRelativeToInk > kMaxRendererMissRelativeToInk ||
                stats.diffRelativeToInk > kMaxRendererDiffRelativeToInk) {
                result.error = `Slug and atlas frames disagree (Slug ink ${(100.0 * stats.slugInkRatio).toFixed(1)}% of atlas ink ` +
                    `(minimum ${(100.0 * kMinComparisonSlugInkRatio).toFixed(1)}%); ink without a nearby counterpart ` +
                    `${(100.0 * stats.missRelativeToInk).toFixed(2)}% of ink (maximum ${(100.0 * kMaxRendererMissRelativeToInk).toFixed(2)}%); ` +
                    `pixels differing ${(100.0 * stats.diffRelativeToInk).toFixed(2)}% of ink (maximum ${(100.0 * kMaxRendererDiffRelativeToInk).toFixed(2)}%))`;
                return result;
            }
        }
        if (result.consoleErrors.length > 0) {
            result.error = `${result.consoleErrors.length} console error(s)`;
            return result;
        }
        if (result.pageErrors.length > 0) {
            result.error = `${result.pageErrors.length} uncaught page error(s)`;
            return result;
        }

        result.ok = true;
        return result;
    } finally {
        await browser.close();
    }
}

// Appends a suffix to a file name before its extension: smoke.png -> smoke-atlas.png.
function withSuffix(path, suffix) {
    const dot = path.lastIndexOf(".");
    const slash = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
    return dot > slash ? `${path.slice(0, dot)}${suffix}${path.slice(dot)}` : `${path}${suffix}`;
}

async function main() {
    const noHooks = env("IMSLUG_SMOKE_NO_HOOKS", "0") === "1";
    const retries = Number(env("IMSLUG_SMOKE_RETRIES", "0"));
    const base = {
        noHooks,
        framesTarget: Number(env("IMSLUG_SMOKE_FRAMES", "60")),
        framesTimeoutMs: Number(env("IMSLUG_SMOKE_TIMEOUT_MS", "180000")),
        dwellMs: Number(env("IMSLUG_SMOKE_DWELL_MS", "8000")),
    };
    const outPath = resolve(repoRoot, env("IMSLUG_SMOKE_OUT", "build-web/smoke.png"));

    // Passes (see the file header): the primary one at IMSLUG_SMOKE_DPR
    // (default 1) and, unless IMSLUG_SMOKE_HIDPI=0, the hi-DPI phone/retina
    // pass that also compares the Slug and atlas frames. The hi-DPI viewport is
    // smaller so software adapters keep up with the 3x framebuffer.
    const passes = [{
        label: "main",
        dpr: Number(env("IMSLUG_SMOKE_DPR", "1")),
        viewportWidth: 1280,
        viewportHeight: 800,
        compareRenderers: env("IMSLUG_SMOKE_COMPARE_RENDERERS", "0") === "1",
        compareFrames: Number(env("IMSLUG_SMOKE_COMPARE_FRAMES", "10")),
        outPath,
        atlasOutPath: withSuffix(outPath, "-atlas"),
    }];
    if (env("IMSLUG_SMOKE_HIDPI", "1") === "1" && !noHooks) {
        passes.push({
            label: "hidpi",
            dpr: Number(env("IMSLUG_SMOKE_HIDPI_DPR", "3")),
            viewportWidth: 800,
            viewportHeight: 500,
            compareRenderers: true,
            compareFrames: Number(env("IMSLUG_SMOKE_COMPARE_FRAMES", "10")),
            outPath: withSuffix(outPath, "-hidpi"),
            atlasOutPath: withSuffix(outPath, "-hidpi-atlas"),
        });
    }
    const adapter = env("IMSLUG_SMOKE_ADAPTER", "swiftshader");

    const executablePath = findChrome();
    if (!executablePath) {
        fail("no Chrome/Chromium found; set IMSLUG_SMOKE_CHROME=<path to chrome>");
        return;
    }

    let server = null;
    let url = env("IMSLUG_SMOKE_URL", "");
    if (!url) {
        const siteDir = resolve(repoRoot, env("IMSLUG_SMOKE_SITE", "build-web/site"));
        if (!existsSync(resolve(siteDir, "index.html"))) {
            fail(`no site at ${siteDir} (build it first: scripts/build_web.*)`);
            return;
        }
        server = await startServer(siteDir);
        url = server.url;
    }

    const args = [
        "--no-sandbox",
        "--disable-gpu-sandbox",
        "--disable-dev-shm-usage",
        "--no-first-run",
        "--no-default-browser-check",
        "--hide-scrollbars",
        "--mute-audio",
        "--window-size=1280,800",
    ];
    let headless = true;
    if (adapter === "swiftshader") {
        // Software WebGPU: works locally; GitHub runners occasionally lose the
        // SwiftShader device at startup - see the lavapipe mode below.
        args.push("--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader");
    } else if (adapter === "lavapipe") {
        // Software Dawn over Mesa lavapipe (Vulkan): the three.js CI recipe.
        // Run it under `xvfb-run -a` with VK_DRIVER_FILES set to the lavapipe
        // ICD; Chrome runs headed on the virtual display.
        args.push("--enable-unsafe-webgpu", "--enable-features=Vulkan", "--disable-vulkan-surface",
                  "--ignore-gpu-blocklist", "--disable-gpu-driver-bug-workarounds");
        headless = false;
    } else {
        args.push("--enable-unsafe-webgpu");
    }
    base.headless = headless;

    console.log(`[web_smoke] url=${url}`);
    console.log(`[web_smoke] chrome=${executablePath} adapter=${adapter} hooks=${noHooks ? "off" : "on"} retries=${retries}`);

    try {
        for (const pass of passes) {
            const options = { ...base, ...pass };
            console.log(`[web_smoke] pass ${pass.label}: ${pass.viewportWidth}x${pass.viewportHeight} @ DPR ${pass.dpr}` +
                        (pass.compareRenderers ? " (Slug vs atlas)" : ""));
            let last = null;
            let ok = false;
            for (let attempt = 1; attempt <= retries + 1 && !ok; attempt++) {
                if (attempt > 1) {
                    console.log(`[web_smoke]   attempt ${attempt}/${retries + 1} after a 3 s pause`);
                    await new Promise((done) => setTimeout(done, 3000));
                }
                last = await runAttempt({ executablePath, args, url, options });
                if (last.ok) {
                    ok = true;
                } else {
                    console.error(`[web_smoke]   attempt ${attempt} failed: ${last.error}`);
                    if (last.stats) {
                        console.error(`[web_smoke]   stats: ${JSON.stringify(last.stats)}`);
                    }
                    if (last.consoleErrors.length > 0) {
                        console.error(`[web_smoke]   console errors:\n    ${last.consoleErrors.join("\n    ")}`);
                    }
                    if (last.pageErrors.length > 0) {
                        console.error(`[web_smoke]   page errors:\n    ${last.pageErrors.join("\n    ")}`);
                    }
                }
            }
            if (!ok) {
                fail(`pass ${pass.label} failed after ${retries + 1} attempt(s) (last: ${last ? last.error : "no result"})`);
                return;
            }
            console.log(JSON.stringify({
                ok: true,
                pass: pass.label,
                url,
                dpr: pass.dpr,
                frames: last.frames,
                ready: last.ready,
                adapter,
                screenshot: last.screenshotPath,
                atlasScreenshot: last.stats && last.stats.diffFraction !== undefined ? last.atlasScreenshotPath : undefined,
                ...last.stats,
                consoleErrors: last.consoleErrors,
                pageErrors: last.pageErrors,
            }, null, 2));
        }
        console.log(`[web_smoke] all ${passes.length} pass(es) OK`);
    } finally {
        if (server) {
            await server.close();
        }
    }
}

main().catch((error) => {
    fail(error && error.stack ? error.stack : String(error));
});
