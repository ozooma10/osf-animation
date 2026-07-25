import { render } from "preact";
import { App } from "./App";
import { ErrorBoundary } from "./ErrorBoundary";
import "./styles.css";
import "./stabilization.css";

render(<ErrorBoundary scope="application"><App /></ErrorBoundary>, document.getElementById("app")!);
