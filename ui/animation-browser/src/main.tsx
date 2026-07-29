import { render } from "preact";
import { App } from "./App";
import { useBrowserController } from "./app/controller";
import { DevSurface } from "./dev/DevSurface";
import { useBrowserInput } from "./input/useBrowserInput";
import "./styles/browser.css";

function BrowserRoot() {
  const { state, commands, debugCommands, standalone } = useBrowserController();
  useBrowserInput(state, commands, standalone);
  // import.meta.env.DEV is compile-time false in a production build, so the
  // DevSurface import below is dead code the bundler removes outright.
  return <><App state={state} commands={commands}/>
    {import.meta.env.DEV && standalone && <DevSurface commands={debugCommands}/>}</>;
}

document.body.className = "osf-animation";
render(<BrowserRoot/>, document.getElementById("app")!);
