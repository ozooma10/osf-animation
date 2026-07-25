import preact from "@preact/preset-vite";
import { defineConfig, type Plugin } from "vite";
import { createReadStream, existsSync, readdirSync, realpathSync, statSync } from "node:fs";
import { resolve, sep } from "node:path";
import packageJson from "./package.json" with { type: "json" };

const RIG_LIMIT = 8 * 1024 * 1024;

function localRigs(): Plugin {
  const root = resolve(import.meta.dirname, "local", "rigs");
  return {
    name: "osf-local-rigs",
    configureServer(server) {
      server.middlewares.use("/__osf/rigs", (request, response) => {
        let requestPath: string;
        try {
          requestPath = decodeURIComponent(new URL(request.url ?? "/", "http://127.0.0.1").pathname);
        } catch {
          response.statusCode = 400;
          response.end("Malformed rig URL");
          return;
        }
        if (requestPath === "/" || requestPath === "") {
          const rigs: string[] = [];
          const walk = (directory: string, prefix = "") => {
            if (!existsSync(directory)) return;
            for (const entry of readdirSync(directory, { withFileTypes: true })) {
              if (entry.isSymbolicLink()) continue;
              const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
              if (entry.isDirectory()) walk(resolve(directory, entry.name), relative);
              else if (entry.name.toLowerCase().endsWith(".rig")) rigs.push(relative);
            }
          };
          try {
            walk(root);
            response.setHeader("Content-Type", "application/json; charset=utf-8");
            response.end(JSON.stringify(rigs.sort()));
          } catch {
            response.statusCode = 500;
            response.end("Rig catalog could not be read");
          }
          return;
        }

        const relative = requestPath.replace(/^\/+/, "");
        const file = resolve(root, relative);
        let realFile: string;
        try {
          realFile = realpathSync(file);
        } catch {
          response.statusCode = 404;
          response.end("Rig not found");
          return;
        }
        const realRoot = existsSync(root) ? realpathSync(root) : root;
        if (!realFile.startsWith(`${realRoot}${sep}`) || !realFile.toLowerCase().endsWith(".rig")) {
          response.statusCode = 404;
          response.end("Rig not found");
          return;
        }
        try {
          if (statSync(realFile).size > RIG_LIMIT) {
            response.statusCode = 413;
            response.end("Rig exceeds the 8 MiB development limit");
            return;
          }
        } catch {
          response.statusCode = 404;
          response.end("Rig not found");
          return;
        }
        response.setHeader("Content-Type", "application/octet-stream");
        response.setHeader("Cache-Control", "no-store");
        const stream = createReadStream(realFile);
        stream.on("error", () => {
          if (!response.headersSent) response.statusCode = 500;
          response.end("Rig could not be read");
        });
        stream.pipe(response);
      });
    },
  };
}

export default defineConfig({
  base: "./",
  plugins: [preact(), localRigs()],
  define: {
    __STUDIO_VERSION__: JSON.stringify(packageJson.version),
    __BUILD_IDENTIFIER__: JSON.stringify(process.env.OSF_STUDIO_BUILD_ID ?? "local"),
  },
  build: {
    target: "es2022",
    outDir: resolve(import.meta.dirname, "dist/client"),
    emptyOutDir: true,
    sourcemap: true,
  },
  test: {
    include: ["src/**/*.test.ts", "tests/**/*.test.ts"],
  },
  server: {
    strictPort: true,
  },
});