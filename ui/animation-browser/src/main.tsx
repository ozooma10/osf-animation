import { render } from "preact";
import { useEffect } from "preact/hooks";
import { App } from "./App";
import { useBrowserController } from "./app/controller";
import { useDevBackdrop } from "./dev/useDevBackdrop";
import { DevTools } from "./dev/DevTools";
import { connectHarnessTools } from "./dev/harness-tools";
import { useBrowserInput } from "./input/useBrowserInput";
import "./styles/browser.css";

function BrowserRoot() {
  const { state, commands, debugCommands, standalone } = useBrowserController();
  useBrowserInput(state, commands, standalone);
  useDevBackdrop(standalone);
  useEffect(() => standalone ? connectHarnessTools(debugCommands) : undefined, [standalone, debugCommands]);
  return <><App state={state} commands={commands}/>{standalone && <DevTools commands={debugCommands}/>}</>;
}

document.body.className = "osf-animation";
render(<BrowserRoot/>, document.getElementById("app")!);
