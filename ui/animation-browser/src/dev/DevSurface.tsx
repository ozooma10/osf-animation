import { createRoot, useEffect } from "octane";
import type { DevCommands } from "./debug";
import { DevTools } from "./DevTools";
import { connectHarnessTools } from "./harness-tools";
import { useDevBackdrop } from "./useDevBackdrop";

/**
 * Every dev-only surface, behind one dynamically imported component. main.tsx
 * loads it under an `import.meta.env.DEV` guard, which lets Vite drop this whole
 * graph — DevTools, the backdrop, the harness toolbar channel — from the shipped bundle.
 * Hooks live here rather than in main.tsx so none of them run in game.
 */
export function DevSurface({ commands }: { commands: DevCommands }) {
  useDevBackdrop(true);
  useEffect(() => connectHarnessTools(commands), [commands]);
  return <DevTools commands={commands}/>;
}

export function mountDevSurface(commands: DevCommands) {
  const host = document.createElement("div");
  host.dataset.osfuiDevSurface = "";
  document.body.append(host);
  const root = createRoot(host);
  root.render(() => <DevSurface commands={commands}/>);
  return () => {
    root.unmount();
    host.remove();
  };
}
