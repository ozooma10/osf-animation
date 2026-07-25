import { describe, expect, it, vi } from "vitest";
import { parseDocument, sceneAt } from "./model";
import {
  createHistoryState,
  repairSelection,
  workspaceReducer,
  type WorkspaceSnapshot,
} from "./workspaceReducer";

function snapshot(): WorkspaceSnapshot {
  const document = parseDocument("fixture.osf.json", JSON.stringify({
    schema: 1,
    scenes: [{ id: "fixture.scene", clip: "clip.glb", unknown: { keep: true } }],
  }));
  return {
    documents: [document],
    selection: { documentId: document.id, sceneIndex: 0 },
    exportedBaselines: { [document.id]: JSON.stringify(document.root) },
  };
}

describe("workspaceReducer", () => {
  it("coalesces consecutive edits to the same field within 500 ms", () => {
    const initial = createHistoryState(snapshot());
    const documentId = initial.present.documents[0].id;
    const first = workspaceReducer(initial, {
      type: "patchScene", documentId, sceneIndex: 0, patch: { name: "A" }, editKey: "name", at: 100,
    });
    const second = workspaceReducer(first, {
      type: "patchScene", documentId, sceneIndex: 0, patch: { name: "AB" }, editKey: "name", at: 550,
    });
    expect(second.past).toHaveLength(1);
    const undone = workspaceReducer(second, { type: "undo" });
    expect(sceneAt(undone.present.documents[0].root, 0)?.name).toBeUndefined();
  });

  it("starts another undo entry after 500 ms and retains unknown fields", () => {
    const initial = createHistoryState(snapshot());
    const documentId = initial.present.documents[0].id;
    const first = workspaceReducer(initial, {
      type: "patchScene", documentId, sceneIndex: 0, patch: { name: "A" }, editKey: "name", at: 100,
    });
    const second = workspaceReducer(first, {
      type: "patchScene", documentId, sceneIndex: 0, patch: { name: "B" }, editKey: "name", at: 601,
    });
    expect(second.past).toHaveLength(2);
    expect(sceneAt(second.present.documents[0].root, 0)?.unknown).toEqual({ keep: true });
  });

  it("repairs a missing document and an out-of-range scene selection", () => {
    const value = snapshot();
    const repaired = repairSelection({
      ...value,
      selection: { documentId: "missing", sceneIndex: 999 },
    });
    expect(repaired.selection.documentId).toBe(value.documents[0].id);
    expect(repaired.selection.sceneIndex).toBe(0);
  });

  it("caps undo history at 50 entries", () => {
    vi.spyOn(Date, "now").mockReturnValue(1);
    let state = createHistoryState(snapshot());
    const documentId = state.present.documents[0].id;
    for (let index = 0; index < 60; index += 1) {
      state = workspaceReducer(state, {
        type: "patchScene",
        documentId,
        sceneIndex: 0,
        patch: { name: String(index) },
        editKey: `name:${index}`,
        at: index,
      });
    }
    expect(state.past).toHaveLength(50);
  });
});

