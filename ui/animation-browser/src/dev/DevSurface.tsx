import { useEffect } from "preact/hooks";
import type { DevCommands } from "./debug";
import { DevTools } from "./DevTools";
import { connectHarnessTools } from "./harness-tools";
import { useDevBackdrop } from "./useDevBackdrop";

/**
 * Every dev-only surface, behind one component. main.tsx renders it under an
 * `import.meta.env.DEV` guard, which is what lets Vite drop this whole graph —
 * DevTools, the backdrop, the harness toolbar channel — from the shipped bundle.
 * Hooks live here rather than in main.tsx so none of them run in game.
 */
export function DevSurface({ commands }: { commands: DevCommands }) {
  useDevBackdrop(true);
  useEffect(() => connectHarnessTools(commands), [commands]);
  return <DevTools commands={commands}/>;
}
