import preact from "@preact/preset-vite";
import { defineConfig, type Plugin } from "vite";
import { createReadStream, existsSync } from "node:fs";
import { resolve } from "node:path";
import { frameHarness } from "./src/dev-harness";

function classicProductionEntry(): Plugin {
  return {
    name: "osf-classic-production-entry",
    enforce: "post",
    transformIndexHtml: {
      order: "post",
      handler(html) {
        return html
          .replace('<script type="module" crossorigin', '<script defer')
          .split(" crossorigin").join("");
      },
    },
  };
}
function liveFixtures(): Plugin {
  const root = resolve(import.meta.dirname, "fixtures", "live");
  return {
    name: "osf-live-fixtures",
    configureServer(server) {
      server.middlewares.use("/live", (request, response, next) => {
        const name = request.url?.replace(/^\//, "").split("?", 1)[0];
        if (!name || !/^[a-z0-9-]+\.json$/i.test(name)) return next();
        // A git-ignored <name>.local.json override wins when present — e.g.
        // tools/generate-catalog-snapshot.py writes catalog.local.json from real
        // pack sources (incl. the compat-packs repo) for richer standalone testing.
        const local = resolve(root, name.replace(/\.json$/i, ".local.json"));
        const file = existsSync(local) ? local : resolve(root, name);
        if (!existsSync(file)) return next();
        response.setHeader("Content-Type", "application/json; charset=utf-8");
        createReadStream(file).pipe(response);
      });
    },
  };
}

export default defineConfig({
  base: "./",
  plugins: [preact(), frameHarness(), liveFixtures(), classicProductionEntry()],
  build: {
    target: "es2018",
    modulePreload: false,
    // Generated output lives under the gitignored build/ tree; the two-level
    // views/<modId>/<viewName>/ tail is the qualified id OSF UI discovers.
    outDir: resolve(import.meta.dirname, "../../build/views/osf.animation/browser"),
    emptyOutDir: true,
    sourcemap: true,
    cssCodeSplit: false,
    rollupOptions: {
      output: {
        format: "iife",
        entryFileNames: "assets/browser.js",
        chunkFileNames: "assets/[name]-[hash].js",
        assetFileNames: (asset) => asset.names.some((name) => name.endsWith(".css"))
          ? "assets/browser.css"
          : "assets/[name]-[hash][extname]",
      },
    },
  },
  server: {
    strictPort: true,
  },
});
