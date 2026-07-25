import { useCallback, useEffect, useReducer, useRef, useState } from "preact/hooks";
import { recordDiagnostic } from "./diagnostics";
import {
  createHistoryState,
  workspaceReducer,
  type WorkspaceCommand,
  type WorkspaceHistoryState,
} from "./workspaceReducer";
import {
  WORKSPACE_SCHEMA_VERSION,
  createMemoryWorkspaceRepository,
  createWorkspaceRepository,
  type WorkspaceEnvelope,
  type WorkspaceRepository,
} from "./workspaceRepository";

export type PersistenceStatus = "loading" | "saving" | "saved" | "failed";

export interface WorkspaceController {
  state: WorkspaceHistoryState;
  dispatch: (command: WorkspaceCommand) => void;
  ready: boolean;
  persistenceStatus: PersistenceStatus;
  storageWarning?: string;
  exportBackup: () => Promise<void>;
}

function envelope(state: WorkspaceHistoryState): WorkspaceEnvelope {
  return {
    schemaVersion: WORKSPACE_SCHEMA_VERSION,
    documents: state.present.documents,
    selection: state.present.selection,
    exportedBaselines: state.present.exportedBaselines,
    updatedAt: new Date().toISOString(),
  };
}

function downloadBlob(blob: Blob, filename: string): void {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  anchor.click();
  queueMicrotask(() => URL.revokeObjectURL(url));
}

export function useWorkspace(): WorkspaceController {
  const [state, dispatch] = useReducer(workspaceReducer, undefined, () => createHistoryState());
  const [ready, setReady] = useState(false);
  const [persistenceStatus, setPersistenceStatus] = useState<PersistenceStatus>("loading");
  const [storageWarning, setStorageWarning] = useState<string>();
  const repository = useRef<WorkspaceRepository>();
  const currentState = useRef(state);
  const saveTimer = useRef<number>();
  currentState.current = state;

  const saveNow = useCallback(async () => {
    if (!repository.current || !ready) return;
    if (saveTimer.current !== undefined) {
      clearTimeout(saveTimer.current);
      saveTimer.current = undefined;
    }
    setPersistenceStatus("saving");
    try {
      await repository.current.save(envelope(currentState.current));
      setPersistenceStatus("saved");
    } catch (error) {
      recordDiagnostic({
        category: "storage",
        level: "error",
        code: "WORKSPACE_SAVE_FAILED",
        message: error instanceof Error ? error.message : "Workspace save failed.",
      });
      const memory = createMemoryWorkspaceRepository();
      await memory.save(envelope(currentState.current));
      repository.current = memory;
      setStorageWarning("Browser storage reached a limit or failed. This draft is now memory-only; export JSON or a backup before leaving.");
      setPersistenceStatus("failed");
    }
  }, [ready]);

  useEffect(() => {
    let active = true;
    void createWorkspaceRepository().then(async (handle) => {
      if (!active) return;
      repository.current = handle.repository;
      setStorageWarning(handle.warning);
      const loaded = await handle.repository.load();
      if (!active) return;
      if (loaded) {
        dispatch({
          type: "hydrate",
          snapshot: {
            documents: loaded.documents,
            selection: loaded.selection,
            exportedBaselines: loaded.exportedBaselines,
          },
        });
      }
      setReady(true);
      setPersistenceStatus("saved");
    });
    return () => { active = false; };
  }, []);

  useEffect(() => {
    if (!ready) return;
    setPersistenceStatus("saving");
    saveTimer.current = window.setTimeout(() => void saveNow(), 500);
    return () => {
      if (saveTimer.current !== undefined) clearTimeout(saveTimer.current);
    };
  }, [ready, state.present, saveNow]);

  useEffect(() => {
    const flushWhenHidden = () => {
      if (document.visibilityState === "hidden") void saveNow();
    };
    const flushOnPageHide = () => void saveNow();
    document.addEventListener("visibilitychange", flushWhenHidden);
    window.addEventListener("pagehide", flushOnPageHide);
    return () => {
      document.removeEventListener("visibilitychange", flushWhenHidden);
      window.removeEventListener("pagehide", flushOnPageHide);
    };
  }, [saveNow]);

  const modified = state.present.documents.some((document) => document.dirty);
  useEffect(() => {
    const warn = (event: BeforeUnloadEvent) => {
      if (!modified) return;
      event.preventDefault();
      event.returnValue = "";
    };
    window.addEventListener("beforeunload", warn);
    return () => window.removeEventListener("beforeunload", warn);
  }, [modified]);

  const exportBackup = useCallback(async () => {
    if (!repository.current) return;
    await saveNow();
    downloadBlob(await repository.current.exportBackup(), `osf-studio-workspace-${Date.now()}.json`);
  }, [saveNow]);

  return { state, dispatch, ready, persistenceStatus, storageWarning, exportBackup };
}

