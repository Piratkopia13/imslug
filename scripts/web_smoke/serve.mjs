// Tiny dependency-free static file server for the web smoke (scripts/web_smoke).
// Serves the assembled site the way GitHub Pages does: correct MIME types
// (application/wasm in particular), no caching, directory index -> index.html.

import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, normalize, resolve, sep } from "node:path";

const MIME = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json",
    ".wasm": "application/wasm",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".svg": "image/svg+xml",
    ".ico": "image/x-icon",
    ".txt": "text/plain; charset=utf-8",
};

export async function startServer(rootDir, { host = "127.0.0.1" } = {}) {
    const root = resolve(rootDir);
    const server = createServer(async (req, res) => {
        try {
            const url = new URL(req.url, "http://localhost");
            let pathname = decodeURIComponent(url.pathname);
            if (pathname === "/favicon.ico") {
                res.writeHead(204);
                res.end();
                return;
            }
            if (pathname.endsWith("/")) {
                pathname += "index.html";
            }
            const filePath = resolve(join(root, normalize(pathname)));
            if (filePath !== root && !filePath.startsWith(root + sep)) {
                res.writeHead(403, { "content-type": "text/plain" });
                res.end("forbidden");
                return;
            }
            const data = await readFile(filePath);
            res.writeHead(200, {
                "content-type": MIME[extname(filePath).toLowerCase()] || "application/octet-stream",
                "cache-control": "no-store",
            });
            res.end(data);
        } catch {
            res.writeHead(404, { "content-type": "text/plain" });
            res.end("not found");
        }
    });
    await new Promise((ok, fail) => {
        server.once("error", fail);
        server.listen(0, host, ok);
    });
    const { port } = server.address();
    return {
        url: `http://${host}:${port}/`,
        close: () => new Promise((done) => server.close(done)),
    };
}
