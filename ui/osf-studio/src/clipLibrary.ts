import { openDB, type IDBPDatabase } from "idb";
import {
  assertFileSize,
  detectAnimationKind,
  validateEmbeddedGltf,
  validateGlbSignature,
} from "./fileSafety";
import { sha256, type StoredAsset } from "./workspaceRepository";

export const CLIP_LIBRARY_SCHEMA_VERSION = 1;
const DATABASE_NAME = "osf-studio";
const DATABASE_VERSION = 2;

export type ClipFormat = "af" | "glb" | "gltf";

export interface ClipRecord {
  schemaVersion: typeof CLIP_LIBRARY_SCHEMA_VERSION;
  id: string;
  assetId: string;
  title: string;
  sourceFilename: string;
  gamePath: string;
  author: string;
  tags: string[];
  actorCount: number;
  durationSeconds?: number;
  format: ClipFormat;
  size: number;
  sha256: string;
  rigId?: string;
  createdAt: string;
  updatedAt: string;
}

export interface ClipSetMember {
  id: string;
  role: string;
  clipId: string;
  offset: { x: number; y: number; z: number; heading: number };
}

export interface ClipSet {
  schemaVersion: typeof CLIP_LIBRARY_SCHEMA_VERSION;
  id: string;
  title: string;
  author: string;
  tags: string[];
  members: ClipSetMember[];
  createdAt: string;
  updatedAt: string;
}

export interface ClipLibrarySnapshot {
  clips: ClipRecord[];
  clipSets: ClipSet[];
}

export interface ClipDependency {
  member: ClipSetMember;
  clip?: ClipRecord;
}

function titleFromFilename(filename: string): string {
  return filename.replace(/\.(?:gltf|glb|af)$/i, "").replace(/[_-]+/g, " ").trim() || "Untitled clip";
}

function normalizeClip(record: ClipRecord): ClipRecord {
  return {
    ...record,
    title: record.title.trim() || titleFromFilename(record.sourceFilename),
    gamePath: record.gamePath.trim() || record.sourceFilename,
    author: record.author.trim(),
    tags: [...new Set(record.tags.map((tag) => tag.trim().toLowerCase()).filter(Boolean))],
    actorCount: Math.max(1, Math.floor(record.actorCount || 1)),
    durationSeconds: record.durationSeconds && record.durationSeconds > 0
      ? record.durationSeconds
      : undefined,
    updatedAt: new Date().toISOString(),
  };
}

function normalizeSet(set: ClipSet): ClipSet {
  return {
    ...set,
    title: set.title.trim() || "Untitled Clip Set",
    author: set.author.trim(),
    tags: [...new Set(set.tags.map((tag) => tag.trim().toLowerCase()).filter(Boolean))],
    members: set.members.map((member, index) => ({
      ...member,
      role: member.role.trim() || `actor${index + 1}`,
      offset: {
        x: Number(member.offset?.x) || 0,
        y: Number(member.offset?.y) || 0,
        z: Number(member.offset?.z) || 0,
        heading: Number(member.offset?.heading) || 0,
      },
    })),
    updatedAt: new Date().toISOString(),
  };
}

async function validateAnimation(file: File, format: ClipFormat): Promise<void> {
  assertFileSize(file, format === "af" ? "af" : "gltf");
  if (format === "glb") validateGlbSignature(await file.arrayBuffer());
  if (format === "gltf") validateEmbeddedGltf(await file.text());
}

export class ClipLibraryRepository {
  private constructor(private readonly db: IDBPDatabase) {}

  static async open(): Promise<ClipLibraryRepository> {
    const db = await openDB(DATABASE_NAME, DATABASE_VERSION, {
      upgrade(database) {
        if (!database.objectStoreNames.contains("workspace")) database.createObjectStore("workspace");
        if (!database.objectStoreNames.contains("assets")) database.createObjectStore("assets");
        if (!database.objectStoreNames.contains("clips")) database.createObjectStore("clips", { keyPath: "id" });
        if (!database.objectStoreNames.contains("clipSets")) database.createObjectStore("clipSets", { keyPath: "id" });
      },
    });
    return new ClipLibraryRepository(db);
  }

  close(): void {
    this.db.close();
  }

  async load(): Promise<ClipLibrarySnapshot> {
    const [clips, clipSets] = await Promise.all([
      this.db.getAll("clips") as Promise<ClipRecord[]>,
      this.db.getAll("clipSets") as Promise<ClipSet[]>,
    ]);
    return {
      clips: clips.sort((a, b) => b.updatedAt.localeCompare(a.updatedAt)),
      clipSets: clipSets.sort((a, b) => b.updatedAt.localeCompare(a.updatedAt)),
    };
  }

  async importClip(file: File): Promise<ClipRecord> {
    const format = detectAnimationKind(file.name);
    await validateAnimation(file, format);
    const hash = await sha256(file);
    const id = `clip:${hash}`;
    const existing = await this.db.get("clips", id) as ClipRecord | undefined;
    if (existing) return existing;
    const now = new Date().toISOString();
    const record: ClipRecord = {
      schemaVersion: CLIP_LIBRARY_SCHEMA_VERSION,
      id,
      assetId: hash,
      title: titleFromFilename(file.name),
      sourceFilename: file.name,
      gamePath: file.name,
      author: "",
      tags: [],
      actorCount: 1,
      format,
      size: file.size,
      sha256: hash,
      createdAt: now,
      updatedAt: now,
    };
    const asset: StoredAsset = {
      id: hash,
      kind: "animation",
      name: file.name,
      mediaType: file.type || "application/octet-stream",
      size: file.size,
      sha256: hash,
      createdAt: now,
      blob: file,
    };
    const tx = this.db.transaction(["clips", "assets"], "readwrite");
    await Promise.all([tx.objectStore("clips").put(record), tx.objectStore("assets").put(asset)]);
    await tx.done;
    return record;
  }

  async updateClip(record: ClipRecord): Promise<ClipRecord> {
    const normalized = normalizeClip(record);
    await this.db.put("clips", normalized);
    return normalized;
  }

  async deleteClip(id: string): Promise<void> {
    const clipSets = await this.db.getAll("clipSets") as ClipSet[];
    const usedBy = clipSets.filter((set) => set.members.some((member) => member.clipId === id));
    if (usedBy.length) {
      throw new Error(`Remove this clip from ${usedBy.map((set) => set.title).join(", ")} before deleting it.`);
    }
    const clip = await this.db.get("clips", id) as ClipRecord | undefined;
    const tx = this.db.transaction(["clips", "assets"], "readwrite");
    await tx.objectStore("clips").delete(id);
    if (clip) await tx.objectStore("assets").delete(clip.assetId);
    await tx.done;
  }

  async getClipFile(id: string): Promise<File | null> {
    const clip = await this.db.get("clips", id) as ClipRecord | undefined;
    if (!clip) return null;
    const asset = await this.db.get("assets", clip.assetId) as StoredAsset | undefined;
    if (!asset) return null;
    return new File([asset.blob], clip.sourceFilename, {
      type: asset.mediaType,
      lastModified: new Date(asset.createdAt).getTime(),
    });
  }

  async createClipSet(): Promise<ClipSet> {
    const now = new Date().toISOString();
    const set: ClipSet = {
      schemaVersion: CLIP_LIBRARY_SCHEMA_VERSION,
      id: `clipset:${crypto.randomUUID()}`,
      title: "New Clip Set",
      author: "",
      tags: [],
      members: [],
      createdAt: now,
      updatedAt: now,
    };
    await this.db.put("clipSets", set);
    return set;
  }

  async updateClipSet(set: ClipSet): Promise<ClipSet> {
    const normalized = normalizeSet(set);
    await this.db.put("clipSets", normalized);
    return normalized;
  }

  async deleteClipSet(id: string): Promise<void> {
    await this.db.delete("clipSets", id);
  }
}

export function resolveClipSet(set: ClipSet, clips: ClipRecord[]): ClipDependency[] {
  const byId = new Map(clips.map((clip) => [clip.id, clip]));
  return set.members.map((member) => ({ member, clip: byId.get(member.clipId) }));
}

export function clipSetToOsfStage(set: ClipSet, clips: ClipRecord[]) {
  const dependencies = resolveClipSet(set, clips);
  const missing = dependencies.filter((entry) => !entry.clip);
  if (missing.length) throw new Error(`Clip Set has ${missing.length} missing clip dependenc${missing.length === 1 ? "y" : "ies"}.`);
  return {
    name: set.title,
    clips: dependencies.map(({ clip, member }) => ({
      file: clip!.gamePath,
      ...(Object.values(member.offset).some((value) => value !== 0) ? { offset: member.offset } : {}),
    })),
  };
}

export function exportLibraryManifest(snapshot: ClipLibrarySnapshot): Blob {
  return new Blob([`${JSON.stringify({
    format: "osf-studio-clip-library",
    version: CLIP_LIBRARY_SCHEMA_VERSION,
    exportedAt: new Date().toISOString(),
    clips: snapshot.clips.map(({ assetId: _assetId, ...clip }) => clip),
    clipSets: snapshot.clipSets,
  }, null, 2)}\n`], { type: "application/json;charset=utf-8" });
}
