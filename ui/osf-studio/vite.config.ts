import preact from "@preact/preset-vite";
import { defineConfig, type Plugin } from "vite";
import { createReadStream, existsSync, readdirSync } from "node:fs";
import { resolve, sep } from "node:path";

function localRigs(): Plugin {
  const root = resolve(import.meta.dirname, "local", "rigs");
  return {
    name: "osf-local-rigs",
    configureServer(server) {
      server.middlewares.use("/__osf/rigs", (request, response) => {
        const requestPath = decodeURIComponent(request.url?.split("?", 1)[0] ?? "/");
        if (requestPath === "/" || requestPath === "") {
          const rigs: string[] = [];
          const walk = (directory: string, prefix = "") => {
            if (!existsSync(directory)) return;
            for (const entry of readdirSync(directory, { withFileTypes: true })) {
              const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
              if (entry.isDirectory()) walk(resolve(directory, entry.name), relative);
              else if (entry.name.toLowerCase().endsWith(".rig")) rigs.push(relative);
            }
          };
          walk(root);
          response.setHeader("Content-Type", "application/json; charset=utf-8");
          response.end(JSON.stringify(rigs.sort()));
          return;
        }

        const relative = requestPath.replace(/^\/+/, "");
        const file = resolve(root, relative);
        if (!file.startsWith(`${root}${sep}`) || !file.toLowerCase().endsWith(".rig") || !existsSync(file)) {
          response.statusCode = 404;
          response.end("Rig not found");
          return;
        }
        response.setHeader("Content-Type", "application/octet-stream");
        response.setHeader("Cache-Control", "no-store");
        createReadStream(file).pipe(response);
      });
    },
  };
}

export default defineConfig({
  base: "./",
  plugins: [preact(), localRigs()],
  build: {
    target: "es2022",
    outDir: resolve(import.meta.dirname, "dist/client"),
    emptyOutDir: true,
    sourcemap: true,
  },
  server: {
    strictPort: true,
  },
});
