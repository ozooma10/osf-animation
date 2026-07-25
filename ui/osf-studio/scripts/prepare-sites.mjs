import { cp, mkdir } from "node:fs/promises";
import { resolve } from "node:path";

const project = resolve(import.meta.dirname, "..");
await mkdir(resolve(project, "dist", "server"), { recursive: true });
await cp(resolve(project, "server", "index.js"), resolve(project, "dist", "server", "index.js"));