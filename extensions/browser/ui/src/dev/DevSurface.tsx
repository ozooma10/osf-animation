import { createRoot } from "octane";
import type { DevCommands } from "./debug";
import { DevTools } from "./DevTools";
import { useDevBackdrop } from "./useDevBackdrop";

/** Loaded behind an `import.meta.env.DEV` guard so Vite drops this graph from the shipped bundle. */
export function DevSurface({ commands }: { commands: DevCommands }) {
  useDevBackdrop(true);
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
