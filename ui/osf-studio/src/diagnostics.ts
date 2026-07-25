export type DiagnosticCategory =
  | "app"
  | "storage"
  | "editor"
  | "validator"
  | "viewer"
  | "decoder";

export interface DiagnosticEvent {
  at: string;
  category: DiagnosticCategory;
  level: "info" | "warning" | "error";
  code: string;
  message: string;
  details?: Record<string, string | number | boolean | null>;
}

export interface DiagnosticAssetSummary {
  type: string;
  size: number;
  hashPrefix?: string;
}

export interface DiagnosticReport {
  studioVersion: string;
  buildIdentifier: string;
  createdAt: string;
  browser: {
    userAgent: string;
    language: string;
    webgl: boolean;
    webgl2: boolean;
  };
  workspaceSchemaVersion: number;
  assets: DiagnosticAssetSummary[];
  events: DiagnosticEvent[];
}

const MAX_EVENTS = 200;
const events: DiagnosticEvent[] = [];
const assets = new Map<string, DiagnosticAssetSummary>();

function safeMessage(message: string): string {
  return message
    .replace(/[A-Za-z]:\\[^\s"']+/g, "[local path]")
    .replace(/file:\/\/\/[^\s"']+/g, "[local path]")
    .slice(0, 500);
}

export function recordDiagnostic(
  event: Omit<DiagnosticEvent, "at"> & { at?: string },
): void {
  events.push({
    ...event,
    at: event.at ?? new Date().toISOString(),
    message: safeMessage(event.message),
  });
  if (events.length > MAX_EVENTS) events.splice(0, events.length - MAX_EVENTS);
}

export function rememberDiagnosticAsset(
  key: string,
  summary: DiagnosticAssetSummary,
): void {
  assets.set(key, summary);
}

function webglSupport(kind: "webgl" | "webgl2"): boolean {
  try {
    return Boolean(document.createElement("canvas").getContext(kind));
  } catch {
    return false;
  }
}

export function createDiagnosticReport(workspaceSchemaVersion: number): DiagnosticReport {
  return {
    studioVersion: __STUDIO_VERSION__,
    buildIdentifier: __BUILD_IDENTIFIER__,
    createdAt: new Date().toISOString(),
    browser: {
      userAgent: navigator.userAgent,
      language: navigator.language,
      webgl: webglSupport("webgl"),
      webgl2: webglSupport("webgl2"),
    },
    workspaceSchemaVersion,
    assets: [...assets.values()],
    events: [...events],
  };
}

export function downloadDiagnosticReport(workspaceSchemaVersion: number): void {
  const report = createDiagnosticReport(workspaceSchemaVersion);
  const blob = new Blob([`${JSON.stringify(report, null, 2)}\n`], {
    type: "application/json;charset=utf-8",
  });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = `osf-studio-diagnostics-${Date.now()}.json`;
  anchor.click();
  queueMicrotask(() => URL.revokeObjectURL(url));
}

