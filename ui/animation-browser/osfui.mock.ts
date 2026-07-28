import { defineMock, type MockContext } from "@osfui/cli";
import { HOST_TOOL_ID, HOST_TOOL_OPTIONS, sendDevTool } from "./src/dev/harness-tools";

// The view runs its own stateful simulator (src/dev/mock-bridge.ts) inside the
// harness iframe, so the scenario stays empty: no command ever reaches this
// mock. What it does contribute are dev controls in the harness shell toolbar,
// which is where debug-only switches belong — an in-view strip would sit on top
// of the layout it is meant to exercise.
export default defineMock({ state: {}, requests: {}, locales: {} });

export function install(ctx: MockContext): void {
  ctx.registerTools([{
    id: HOST_TOOL_ID,
    kind: "select",
    label: "OSF UI host",
    title: "Fake the installed OSF UI host version the browser status line reports",
    options: HOST_TOOL_OPTIONS,
    value: HOST_TOOL_OPTIONS[0].value,
  }], (id, value) => sendDevTool({ id, value }));
}
