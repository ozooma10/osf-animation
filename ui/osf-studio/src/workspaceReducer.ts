import {
  SAMPLE_SCENE,
  addScene,
  cloneJson,
  removeScene,
  replaceScene,
  sceneAt,
  scenesOf,
  type JsonObject,
  type SceneLocation,
  type StudioDocument,
} from "./model";

export interface WorkspaceSnapshot {
  documents: StudioDocument[];
  selection: SceneLocation;
  exportedBaselines: Record<string, string>;
}

export interface WorkspaceHistoryState {
  present: WorkspaceSnapshot;
  past: WorkspaceSnapshot[];
  future: WorkspaceSnapshot[];
  lastEdit?: { key: string; at: number };
}

export type WorkspaceCommand =
  | { type: "hydrate"; snapshot: WorkspaceSnapshot }
  | { type: "select"; selection: SceneLocation }
  | { type: "newDocument"; document?: StudioDocument; at?: number }
  | { type: "importDocuments"; documents: StudioDocument[]; at?: number }
  | { type: "removeDocument"; documentId: string; at?: number }
  | { type: "addScene"; documentId: string; at?: number }
  | { type: "removeScene"; documentId: string; sceneIndex: number; at?: number }
  | { type: "patchScene"; documentId: string; sceneIndex: number; patch: Partial<JsonObject>; editKey?: string; at?: number }
  | { type: "replaceDocument"; documentId: string; root: JsonObject; editKey?: string; at?: number }
  | { type: "markExported"; documentId: string }
  | { type: "undo" }
  | { type: "redo" };

const HISTORY_LIMIT = 50;
const COALESCE_MS = 500;

export function createDefaultSnapshot(): WorkspaceSnapshot {
  const document: StudioDocument = {
    id: crypto.randomUUID(),
    filename: "my-pack.osf.json",
    root: cloneJson(SAMPLE_SCENE),
    dirty: false,
  };
  return {
    documents: [document],
    selection: { documentId: document.id, sceneIndex: 0 },
    exportedBaselines: { [document.id]: JSON.stringify(document.root) },
  };
}

export function createHistoryState(snapshot = createDefaultSnapshot()): WorkspaceHistoryState {
  return { present: repairSelection(snapshot), past: [], future: [] };
}

export function repairSelection(snapshot: WorkspaceSnapshot): WorkspaceSnapshot {
  const documents = snapshot.documents.length ? snapshot.documents : createDefaultSnapshot().documents;
  const selected = documents.find((document) => document.id === snapshot.selection.documentId) ?? documents[0];
  const sceneCount = Math.max(1, scenesOf(selected.root).length);
  return {
    ...snapshot,
    documents,
    selection: {
      documentId: selected.id,
      sceneIndex: Math.min(Math.max(0, snapshot.selection.sceneIndex), sceneCount - 1),
    },
  };
}

function dirtyDocument(document: StudioDocument, root: JsonObject, baselines: Record<string, string>): StudioDocument {
  return { ...document, root, dirty: JSON.stringify(root) !== baselines[document.id] };
}

function commit(
  state: WorkspaceHistoryState,
  next: WorkspaceSnapshot,
  editKey?: string,
  at = Date.now(),
): WorkspaceHistoryState {
  const coalesced = Boolean(
    editKey && state.lastEdit?.key === editKey && at - state.lastEdit.at <= COALESCE_MS,
  );
  return {
    present: repairSelection(next),
    past: coalesced ? state.past : [...state.past, state.present].slice(-HISTORY_LIMIT),
    future: [],
    lastEdit: editKey ? { key: editKey, at } : undefined,
  };
}

export function workspaceReducer(
  state: WorkspaceHistoryState,
  command: WorkspaceCommand,
): WorkspaceHistoryState {
  const current = state.present;
  switch (command.type) {
    case "hydrate":
      return createHistoryState(command.snapshot);
    case "select":
      return { ...state, present: repairSelection({ ...current, selection: command.selection }), lastEdit: undefined };
    case "newDocument": {
      const document = command.document ?? {
        id: crypto.randomUUID(),
        filename: "untitled.osf.json",
        root: cloneJson(SAMPLE_SCENE),
        dirty: true,
      };
      return commit(state, {
        ...current,
        documents: [...current.documents, document],
        selection: { documentId: document.id, sceneIndex: 0 },
      }, undefined, command.at);
    }
    case "importDocuments":
      if (!command.documents.length) return state;
      return commit(state, {
        ...current,
        documents: [...current.documents, ...command.documents],
        exportedBaselines: {
          ...current.exportedBaselines,
          ...Object.fromEntries(command.documents.map((document) => [document.id, JSON.stringify(document.root)])),
        },
        selection: { documentId: command.documents[0].id, sceneIndex: 0 },
      }, undefined, command.at);
    case "removeDocument": {
      if (current.documents.length <= 1) return state;
      const documents = current.documents.filter((document) => document.id !== command.documentId);
      const exportedBaselines = { ...current.exportedBaselines };
      delete exportedBaselines[command.documentId];
      return commit(state, {
        documents,
        exportedBaselines,
        selection: { documentId: documents[0].id, sceneIndex: 0 },
      }, undefined, command.at);
    }
    case "addScene": {
      const document = current.documents.find((entry) => entry.id === command.documentId);
      if (!document) return state;
      const result = addScene(document.root);
      return commit(state, {
        ...current,
        documents: current.documents.map((entry) => entry.id === document.id
          ? dirtyDocument(entry, result.root, current.exportedBaselines)
          : entry),
        selection: { documentId: document.id, sceneIndex: result.index },
      }, undefined, command.at);
    }
    case "removeScene": {
      const document = current.documents.find((entry) => entry.id === command.documentId);
      if (!document || scenesOf(document.root).length <= 1) return state;
      const root = removeScene(document.root, command.sceneIndex);
      return commit(state, {
        ...current,
        documents: current.documents.map((entry) => entry.id === document.id
          ? dirtyDocument(entry, root, current.exportedBaselines)
          : entry),
        selection: { documentId: document.id, sceneIndex: Math.max(0, command.sceneIndex - 1) },
      }, undefined, command.at);
    }
    case "patchScene": {
      const document = current.documents.find((entry) => entry.id === command.documentId);
      const scene = document && sceneAt(document.root, command.sceneIndex);
      if (!document || !scene) return state;
      const root = replaceScene(document.root, command.sceneIndex, { ...scene, ...command.patch });
      return commit(state, {
        ...current,
        documents: current.documents.map((entry) => entry.id === document.id
          ? dirtyDocument(entry, root, current.exportedBaselines)
          : entry),
      }, command.editKey, command.at);
    }
    case "replaceDocument":
      return commit(state, {
        ...current,
        documents: current.documents.map((document) => document.id === command.documentId
          ? dirtyDocument(document, command.root, current.exportedBaselines)
          : document),
      }, command.editKey, command.at);
    case "markExported": {
      const document = current.documents.find((entry) => entry.id === command.documentId);
      if (!document) return state;
      const baseline = JSON.stringify(document.root);
      return {
        ...state,
        present: {
          ...current,
          exportedBaselines: { ...current.exportedBaselines, [document.id]: baseline },
          documents: current.documents.map((entry) => entry.id === document.id
            ? { ...entry, dirty: false }
            : entry),
        },
        lastEdit: undefined,
      };
    }
    case "undo": {
      const prior = state.past.at(-1);
      if (!prior) return state;
      return {
        present: repairSelection(prior),
        past: state.past.slice(0, -1),
        future: [state.present, ...state.future].slice(0, HISTORY_LIMIT),
      };
    }
    case "redo": {
      const next = state.future[0];
      if (!next) return state;
      return {
        present: repairSelection(next),
        past: [...state.past, state.present].slice(-HISTORY_LIMIT),
        future: state.future.slice(1),
      };
    }
  }
}

