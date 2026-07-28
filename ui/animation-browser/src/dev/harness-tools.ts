// Dev controls this view contributes to the OSF UI harness toolbar. The mock
// module (osfui.mock.ts) registers them with ctx.registerTools and the shell
// renders them beside its own controls; clicks come back to the mock, which
// runs in this same page, so it hands them to the view over a window event.
// Nothing here loads outside the harness/standalone path.

import type { DevCommands, VersionDebugState } from "./debug";

/** Fakes the installed OSF UI host version the browser status line reports. */
export const HOST_TOOL_ID = "ui-host";

export const HOST_TOOL_OPTIONS: { value: VersionDebugState; label: string }[] = [
  { value: "match", label: "host up to date" },
  { value: "old", label: "host older than tested" },
];

export interface DevToolInvocation {
  id: string;
  value?: string | boolean;
}

const CHANNEL = "osf.animation.devtool";

/** Mock side: forward a harness toolbar click to the view. */
export function sendDevTool(invocation: DevToolInvocation): void {
  window.dispatchEvent(new CustomEvent<DevToolInvocation>(CHANNEL, { detail: invocation }));
}

/** View side: apply the default host state, then follow the toolbar. Returns cleanup. */
export function connectHarnessTools(commands: DevCommands): () => void {
  commands.version("none");
  const listener = (event: Event) => {
    const detail = (event as CustomEvent<DevToolInvocation>).detail;
    if (!detail || detail.id !== HOST_TOOL_ID) return;
    const option = HOST_TOOL_OPTIONS.find((candidate) => candidate.value === detail.value);
    commands.version(option?.value ?? "none");
  };
  window.addEventListener(CHANNEL, listener);
  return () => window.removeEventListener(CHANNEL, listener);
}
