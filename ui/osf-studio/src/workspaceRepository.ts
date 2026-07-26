import { openDB, type IDBPDatabase } from "idb";
import { isObject, scenesOf, type SceneLocation, type StudioDocument } from "./model";
import { recordDiagnostic } from "./diagnostics";

export const WORKSPACE_SCHEMA_VERSION = 2;
export const LEGACY_STORAGE_KEY = "osf-studio.workspace.v1";
export const LEGACY_BACKUP_KEY = "osf-studio.workspace.v1.alpha-backup";

const DATABASE_NAME = "osf-studio";
const WORKSPACE_KEY = "current";

export interface WorkspaceEnvelope {
  schemaVersion: typeof WORKSPACE_SCHEMA_VERSION;
  documents: StudioDocument[];
  selection: SceneLocation;
  exportedBaselines: Record<string, string>;
  updatedAt: string;
}

export interface StoredAsset {
  id: string;
  kind: "rig" | "animation" | "thumbnail";
  name: string;
  mediaType: string;
  size: number;
  sha256: string;
  createdAt: string;
  blob: Blob;
}

export interface WorkspaceRepository {
  load(): Promise<WorkspaceEnvelope | null>;
  save(workspace: WorkspaceEnvelope): Promise<void>;
  saveAsset(asset: StoredAsset): Promise<void>;
  getAsset(id: string): Promise<StoredAsset | null>;
  exportBackup(): Promise<Blob>;
}

export interface RepositoryHandle {
  repository: WorkspaceRepository;
  mode: "indexeddb" | "memory";
  warning?: string;
}

function validDocument(value: unknown): value is StudioDocument {
  if (!isObject(value)) return false;
  if (typeof value.id !== "string" || typeof value.filename !== "string" || !isObject(value.root)) return false;
  return scenesOf(value.root).length > 0;
}

export function validateWorkspace(value: unknown): WorkspaceEnvelope | null {
  if (!isObject(value) || value.schemaVersion !== WORKSPACE_SCHEMA_VERSION) return null;
  if (!Array.isArray(value.documents) || !value.documents.length || !value.documents.every(validDocument)) return null;
  if (!isObject(value.selection) || typeof value.selection.documentId !== "string"
    || typeof value.selection.sceneIndex !== "number") return null;
  const exportedBaselines = isObject(value.exportedBaselines)
    ? Object.fromEntries(Object.entries(value.exportedBaselines).filter((entry): entry is [string, string] =>
      typeof entry[1] === "string"))
    : {};
  return {
    schemaVersion: WORKSPACE_SCHEMA_VERSION,
    documents: value.documents as unknown as StudioDocument[],
    selection: {
      documentId: value.selection.documentId,
      sceneIndex: Math.max(0, Math.floor(value.selection.sceneIndex)),
    },
    exportedBaselines,
    updatedAt: typeof value.updatedAt === "string" ? value.updatedAt : new Date().toISOString(),
  };
}

export function migrateLegacyValue(source: string): WorkspaceEnvelope {
  const parsed: unknown = JSON.parse(source);
  if (!Array.isArray(parsed) || !parsed.length || !parsed.every(validDocument)) {
    throw new Error("The legacy draft did not contain a valid OSF Studio workspace.");
  }
  const documents = parsed.map((document) => ({ ...document, dirty: Boolean(document.dirty) }));
  return {
    schemaVersion: WORKSPACE_SCHEMA_VERSION,
    documents,
    selection: { documentId: documents[0].id, sceneIndex: 0 },
    exportedBaselines: Object.fromEntries(
      documents.filter((document) => !document.dirty)
        .map((document) => [document.id, JSON.stringify(document.root)]),
    ),
    updatedAt: new Date().toISOString(),
  };
}

class IndexedDbWorkspaceRepository implements WorkspaceRepository {
  constructor(private readonly db: IDBPDatabase) {}

  async load(): Promise<WorkspaceEnvelope | null> {
    return validateWorkspace(await this.db.get("workspace", WORKSPACE_KEY));
  }

  async save(workspace: WorkspaceEnvelope): Promise<void> {
    const tx = this.db.transaction("workspace", "readwrite");
    await tx.store.put(workspace, WORKSPACE_KEY);
    await tx.done;
  }

  async saveAsset(asset: StoredAsset): Promise<void> {
    const tx = this.db.transaction("assets", "readwrite");
    await tx.store.put(asset, asset.id);
    await tx.done;
  }

  async getAsset(id: string): Promise<StoredAsset | null> {
    return (await this.db.get("assets", id) as StoredAsset | undefined) ?? null;
  }

  async exportBackup(): Promise<Blob> {
    const workspace = await this.load();
    const assetRows = await this.db.getAll("assets") as StoredAsset[];
    const assetMetadata = assetRows.map(({ blob: _blob, ...metadata }) => metadata);
    return new Blob([`${JSON.stringify({
      format: "osf-studio-backup",
      version: 1,
      workspace,
      assets: assetMetadata,
    }, null, 2)}\n`], { type: "application/json;charset=utf-8" });
  }
}

class MemoryWorkspaceRepository implements WorkspaceRepository {
  private workspace: WorkspaceEnvelope | null = null;
  private readonly assets = new Map<string, StoredAsset>();

  async load(): Promise<WorkspaceEnvelope | null> {
    return this.workspace ? structuredClone(this.workspace) : null;
  }

  async save(workspace: WorkspaceEnvelope): Promise<void> {
    this.workspace = structuredClone(workspace);
  }

  async saveAsset(asset: StoredAsset): Promise<void> {
    this.assets.set(asset.id, asset);
  }

  async getAsset(id: string): Promise<StoredAsset | null> {
    return this.assets.get(id) ?? null;
  }

  async exportBackup(): Promise<Blob> {
    return new Blob([`${JSON.stringify({
      format: "osf-studio-backup",
      version: 1,
      workspace: this.workspace,
      assets: [...this.assets.values()].map(({ blob: _blob, ...metadata }) => metadata),
    }, null, 2)}\n`], { type: "application/json;charset=utf-8" });
  }
}

export function createMemoryWorkspaceRepository(): WorkspaceRepository {
  return new MemoryWorkspaceRepository();
}

async function migrateLegacy(repository: WorkspaceRepository): Promise<WorkspaceEnvelope | null> {
  const source = localStorage.getItem(LEGACY_STORAGE_KEY);
  if (!source) return null;
  localStorage.setItem(LEGACY_BACKUP_KEY, source);
  const migrated = migrateLegacyValue(source);
  await repository.save(migrated);
  const verified = await repository.load();
  if (!verified) throw new Error("The migrated workspace could not be verified after writing.");
  recordDiagnostic({
    category: "storage",
    level: "info",
    code: "LEGACY_MIGRATION_COMPLETE",
    message: "Migrated the legacy local draft into IndexedDB and retained a recovery backup.",
  });
  return verified;
}

export async function createWorkspaceRepository(): Promise<RepositoryHandle> {
  try {
    if (!("indexedDB" in globalThis)) throw new Error("IndexedDB is not available.");
    const db = await openDB(DATABASE_NAME, 3, {
      upgrade(database) {
        if (!database.objectStoreNames.contains("workspace")) database.createObjectStore("workspace");
        if (!database.objectStoreNames.contains("assets")) database.createObjectStore("assets");
        if (!database.objectStoreNames.contains("clips")) database.createObjectStore("clips", { keyPath: "id" });
        if (!database.objectStoreNames.contains("clipSets")) database.createObjectStore("clipSets", { keyPath: "id" });
        if (!database.objectStoreNames.contains("animationProjects")) database.createObjectStore("animationProjects", { keyPath: "id" });
      },
    });
    const repository = new IndexedDbWorkspaceRepository(db);
    if (!await repository.load()) {
      try {
        await migrateLegacy(repository);
      } catch (error) {
        recordDiagnostic({
          category: "storage",
          level: "error",
          code: "LEGACY_MIGRATION_FAILED",
          message: error instanceof Error ? error.message : "Legacy migration failed.",
        });
        // The legacy value and its backup remain untouched.
      }
    }
    return { repository, mode: "indexeddb" };
  } catch (error) {
    recordDiagnostic({
      category: "storage",
      level: "error",
      code: "INDEXEDDB_UNAVAILABLE",
      message: error instanceof Error ? error.message : "IndexedDB is unavailable.",
    });
    return {
      repository: new MemoryWorkspaceRepository(),
      mode: "memory",
      warning: "Browser storage is unavailable. This draft is memory-only; export JSON or a backup before leaving.",
    };
  }
}

export async function sha256(blob: Blob): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", await blob.arrayBuffer());
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

