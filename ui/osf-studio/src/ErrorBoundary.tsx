import { Component, type ComponentChildren } from "preact";
import { downloadDiagnosticReport, recordDiagnostic } from "./diagnostics";
import { WORKSPACE_SCHEMA_VERSION } from "./workspaceRepository";

interface Props {
  scope: "application" | "editor" | "viewer";
  onReturnToEditor?: () => void;
  children: ComponentChildren;
}

interface State {
  error?: Error;
}

export class ErrorBoundary extends Component<Props, State> {
  state: State = {};

  static getDerivedStateFromError(error: Error): State {
    return { error };
  }

  componentDidCatch(error: Error): void {
    recordDiagnostic({
      category: this.props.scope === "viewer" ? "viewer" : "app",
      level: "error",
      code: "SURFACE_CRASH",
      message: error.message,
    });
  }

  render(): ComponentChildren {
    if (!this.state.error) return this.props.children;
    return (
      <section class="recovery-surface" role="alert">
        <span class="eyebrow">{this.props.scope} recovery</span>
        <h2>This surface stopped unexpectedly.</h2>
        <p>Your browser-local workspace has not been cleared. You can retry, return to the editor, or export a sanitized report.</p>
        <code>{this.state.error.message}</code>
        <div class="recovery-actions">
          <button class="primary-button" onClick={() => this.setState({ error: undefined })}>Retry surface</button>
          {this.props.onReturnToEditor && (
            <button class="secondary-button" onClick={() => {
              this.setState({ error: undefined });
              this.props.onReturnToEditor?.();
            }}>Return to editor</button>
          )}
          <button class="secondary-button" onClick={() => downloadDiagnosticReport(WORKSPACE_SCHEMA_VERSION)}>
            Export diagnostics
          </button>
        </div>
      </section>
    );
  }
}

