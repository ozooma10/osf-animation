import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";
import { octane } from "@octanejs/vite-plugin";

const local = (path: string) => fileURLToPath(new URL(path, import.meta.url));

export default defineConfig({
  root: local("./src/views/osf.animation/browser"),
  publicDir: local("./public"),
  base: "./",
  plugins: [octane()],
  build: {
    outDir: local("../../build/osfui-animation-browser/SFSE/Plugins/OSF/UI/views/osf.animation/browser"),
    emptyOutDir: true,
    target: "es2022",
  },
});
