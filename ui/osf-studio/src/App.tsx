import { lazy, Suspense } from "preact/compat";
import { useState } from "preact/hooks";
import { ErrorBoundary } from "./ErrorBoundary";
import { SceneEditor } from "./SceneEditor";

const AnimationViewer = lazy(() => import("./viewer/AnimationViewerSurface").then((module) => ({
  default: module.AnimationViewer,
})));
const ClipLibrary = lazy(() => import("./ClipLibrarySurface").then((module) => ({
  default: module.ClipLibrary,
})));

type Surface = "editor" | "library" | "viewer";

const SURFACE_COPY: Record<Surface, string> = {
  editor: "Scene authoring workspace",
  library: "Reusable clip repository",
  viewer: "Animation inspection workspace",
};

export function App() {
  const [surface, setSurface] = useState<Surface>("editor");
  const [previewClipId, setPreviewClipId] = useState<string>();

  function showViewer() {
    setPreviewClipId(undefined);
    setSurface("viewer");
  }

  return (
    <main class="studio-shell">
      <header class="topbar">
        <div class="brand">
          <div class="brand-mark">OSF</div>
          <div>
            <h1>Studio</h1>
            <p>{SURFACE_COPY[surface]}</p>
          </div>
        </div>
        <nav class="surface-switch" aria-label="Studio workspace">
          <button class={surface === "editor" ? "active" : ""} onClick={() => setSurface("editor")}>
            <span>01</span> Scene editor
          </button>
          <button class={surface === "library" ? "active" : ""} onClick={() => setSurface("library")}>
            <span>02</span> Clip library
          </button>
          <button class={surface === "viewer" ? "active" : ""} onClick={showViewer}>
            <span>03</span> Animation viewer
          </button>
        </nav>
        {surface !== "editor" && <div class="viewer-privacy"><i /> Local only · never uploaded</div>}
      </header>
      {surface === "editor" && (
        <ErrorBoundary scope="editor"><SceneEditor /></ErrorBoundary>
      )}
      {surface === "library" && (
        <ErrorBoundary scope="library" onReturnToEditor={() => setSurface("editor")}>
          <Suspense fallback={<section class="surface-loading">Loading clip library…</section>}>
            <ClipLibrary onPreviewClip={(clipId) => {
              setPreviewClipId(clipId);
              setSurface("viewer");
            }} />
          </Suspense>
        </ErrorBoundary>
      )}
      {surface === "viewer" && (
        <ErrorBoundary scope="viewer" onReturnToEditor={() => setSurface("editor")}>
          <Suspense fallback={<section class="surface-loading">Loading animation viewer…</section>}>
            <AnimationViewer initialClipId={previewClipId} />
          </Suspense>
        </ErrorBoundary>
      )}
    </main>
  );
}