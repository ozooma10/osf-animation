import { render } from "preact";
import { App } from "./App";
import "./styles/browser.css";
import { startBrowser } from "./legacy/browser.js";

document.body.className = "osf-animation";
render(<App />, document.getElementById("app")!);
startBrowser();
