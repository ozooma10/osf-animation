import { createRoot, useEffect } from "octane";
import { App } from "./App";
import { useBrowserController } from "./app/controller";
import { useBrowserInput } from "./input/useBrowserInput";
import "./styles/browser.css";

function BrowserRoot() {
  const { state, commands, debugCommands, standalone } = useBrowserController();
  useBrowserInput(state, commands, standalone);
  useEffect(() => {
    if (!import.meta.env.DEV || !standalone) return;
    let current = true;
    let dispose: (() => void) | undefined;
    void import("./dev/DevSurface").then(({ mountDevSurface }) => {
      if (current) dispose = mountDevSurface(debugCommands);
    });
    return () => {
      current = false;
      dispose?.();
    };
  }, [debugCommands, standalone]);
  return <App state={state} commands={commands}/>;
}

document.body.className = "osf-animation";
createRoot(document.getElementById("app")!).render(BrowserRoot);
