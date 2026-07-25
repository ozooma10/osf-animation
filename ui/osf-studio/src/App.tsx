import { lazy, Suspense } from "preact/compat";
import { useState } from "preact/hooks";
import { ErrorBoundary } from "./ErrorBoundary";
import { SceneEditor } from "./SceneEditor";

const AnimationViewer = lazy(() => import("./viewer/AnimationViewerSurface").then((module) => ({
  default: module.AnimationViewer,
})));

export function App() {
  const [surface, setSurface] = useState<"editor" | "viewer">("editor");
  return (
    <main class="studio-shell">
      <header class="topbar">
        <div class="brand">
          <div class="brand-mark">OSF</div>
          <div>
            <h1>Studio</h1>
            <p>{surface === "editor" ? "Scene authoring workspace" : "Animation inspection workspace"}</p>
          </div>
        </div>
        <nav class="surface-switch" aria-label="Studio workspace">
          <button class={surface === "editor" ? "active" : ""} onClick={() => setSurface("editor")}>
            <span>01</span> Scene editor
          </button>
          <button class={surface === "viewer" ? "active" : ""} onClick={() => setSurface("viewer")}>
            <span>02</span> Animation viewer
          </button>
        </nav>
        {surface === "viewer" && <div class="viewer-privacy"><i /> Local preview · files never uploaded</div>}
      </header>
      {surface === "editor" ? (
        <ErrorBoundary scope="editor"><SceneEditor /></ErrorBoundary>
      ) : (
        <ErrorBoundary scope="viewer" onReturnToEditor={() => setSurface("editor")}>
          <Suspense fallback={<section class="surface-loading">Loading animation viewer…</section>}>
            <AnimationViewer />
          </Suspense>
        </ErrorBoundary>
      )}
    </main>
  );
}