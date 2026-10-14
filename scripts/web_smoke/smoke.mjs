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
// Env overrides:
//   IMSLUG_SMOKE_URL      page URL to test (default: serve IMSLUG_SMOKE_SITE)
//   IMSLUG_SMOKE_SITE     site root to serve      (default: build-web/site)
//   IMSLUG_SMOKE_OUT      screenshot path         (default: build-web/smoke.png)
//   IMSLUG_SMOKE_CHROME   Chrome/Chromium executable
//   IMSLUG_SMOKE_ADAPTER  "swiftshader" (default; software, CI-safe) or "native"
//   IMSLUG_SMOKE_FRAMES   frames to wait for      (default: 60)
//   IMSLUG_SMOKE_TIMEOUT_MS  frames timeout       (default: 180000)
//   IMSLUG_SMOKE_DWELL_MS    wait when NO_HOOKS=1 (default: 8000)
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

async function main() {
    const noHooks = env("IMSLUG_SMOKE_NO_HOOKS", "0") === "1";
    const framesTarget = Number(env("IMSLUG_SMOKE_FRAMES", "60"));
    const framesTimeoutMs = Number(env("IMSLUG_SMOKE_TIMEOUT_MS", "180000"));
    const dwellMs = Number(env("IMSLUG_SMOKE_DWELL_MS", "8000"));
    const adapter = env("IMSLUG_SMOKE_ADAPTER", "swiftshader");
    const outPath = resolve(repoRoot, env("IMSLUG_SMOKE_OUT", "build-web/smoke.png"));

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
        "--disable-dev-shm-usage",
        "--no-first-run",
        "--no-default-browser-check",
        "--hide-scrollbars",
        "--mute-audio",
        "--window-size=1280,800",
    ];
    if (adapter === "swiftshader") {
        // Software WebGPU: the CI-safe path for runner machines without a GPU.
        args.push("--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader");
    } else {
        args.push("--enable-unsafe-webgpu");
    }

    console.log(`[web_smoke] url=${url}`);
    console.log(`[web_smoke] chrome=${executablePath} adapter=${adapter} hooks=${noHooks ? "off" : "on"}`);

    const browser = await puppeteer.launch({
        executablePath,
        headless: true,
        args,
        protocolTimeout: Math.max(framesTimeoutMs + 60000, 300000),
    });
    try {
        const page = await browser.newPage();
        await page.setViewport({ width: 1280, height: 800, deviceScaleFactor: 1 });

        const consoleErrors = [];
        const pageErrors = [];
        page.on("console", (message) => {
            if (message.type() !== "error") {
                return;
            }
            const from = message.location() && message.location().url ? message.location().url : "";
            if (from.includes("favicon")) {
                return;
            }
            consoleErrors.push(`${message.text()} (${from})`);
        });
        page.on("pageerror", (error) => pageErrors.push(String(error)));

        await page.goto(url, { waitUntil: "load", timeout: 120000 });

        let state = { frames: 0, ready: false, error: "" };
        if (noHooks) {
            await new Promise((done) => setTimeout(done, dwellMs));
        } else {
            await page.waitForFunction(
                `window.__imslugError || ((window.__imslugFrames || 0) >= ${framesTarget} && window.__imslugReady === true)`,
                { timeout: framesTimeoutMs, polling: 250 });
        }
        state = await page.evaluate(() => ({
            frames: window.__imslugFrames | 0,
            ready: window.__imslugReady === true,
            error: String(window.__imslugError || ""),
        }));

        if (state.error) {
            fail(`page reported an init error: ${state.error}`);
            return;
        }
        if (!noHooks && state.frames < framesTarget) {
            fail(`only ${state.frames}/${framesTarget} frames rendered`);
            return;
        }

        const screenshot = await page.screenshot({ type: "png" });
        await mkdir(dirname(outPath), { recursive: true });
        await writeFile(outPath, screenshot);

        // Decode the screenshot in a browser (no image dependencies in Node)
        // and measure how much of it is actual ink rather than the clear color.
        const analyzer = await browser.newPage();
        const stats = await analyzer.evaluate(async (base64) => {
            const image = new Image();
            image.src = "data:image/png;base64," + base64;
            await image.decode();
            const canvas = document.createElement("canvas");
            canvas.width = image.naturalWidth;
            canvas.height = image.naturalHeight;
            const ctx = canvas.getContext("2d");
            ctx.drawImage(image, 0, 0);
            const data = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
            let sum = 0;
            let ink = 0;
            for (let i = 0; i < data.length; i += 4) {
                const luma = (data[i] + data[i + 1] + data[i + 2]) / 3;
                sum += luma;
                if (luma > 24) {
                    ink += 1;
                }
            }
            const pixels = data.length / 4;
            return { width: canvas.width, height: canvas.height, meanLuma: sum / pixels, inkFraction: ink / pixels };
        }, screenshot.toString("base64"));
        await analyzer.close();

        const ok = stats.inkFraction >= 0.002;
        if (!ok) {
            fail(`canvas looks blank (ink fraction ${stats.inkFraction.toFixed(5)}, mean luma ${stats.meanLuma.toFixed(2)})`);
        }
        if (consoleErrors.length > 0) {
            fail(`${consoleErrors.length} console error(s):\n  ${consoleErrors.join("\n  ")}`);
        }
        if (pageErrors.length > 0) {
            fail(`${pageErrors.length} uncaught page error(s):\n  ${pageErrors.join("\n  ")}`);
        }

        console.log(JSON.stringify({
            ok: process.exitCode !== 1,
            url,
            frames: state.frames,
            ready: state.ready,
            adapter,
            screenshot: outPath,
            ...stats,
            consoleErrors,
            pageErrors,
        }, null, 2));
    } finally {
        await browser.close();
        if (server) {
            await server.close();
        }
    }
}

main().catch((error) => {
    fail(error && error.stack ? error.stack : String(error));
});
