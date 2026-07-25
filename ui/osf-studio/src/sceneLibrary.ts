import { strFromU8, strToU8, unzip, zip, type AsyncZippable } from "fflate";
import {
  CLIP_LIBRARY_SCHEMA_VERSION,
  clipSetToOsfStage,
  type ClipLibraryRepository,
  type ClipLibrarySnapshot,
  type ClipRecord,
  type ClipSet,
} from "./clipLibrary";
import { assertFileSize } from "./fileSafety";
import {
  isObject,
  parseDocument,
  scenesOf,
  type JsonObject,
  type JsonValue,
  type StudioDocument,
} from "./model";

export const SCENE_BUNDLE_VERSION = 1;

export type LibraryInsertion =
  | { type: "clip"; clip: ClipRecord }
  | { type: "clipSet"; clipSet: ClipSet; clips: ClipRecord[] };

export interface SceneDependency {
  path: string;
  clip?: ClipRecord;
  status: "available" | "missing" | "incompatible";
}

interface SceneBundleManifest {
  format: "osf-studio-scene-bundle";
  version: typeof SCENE_BUNDLE_VERSION;
  document: {
    filename: string;
    root: JsonObject;
  };
  clips: Array<{
    entry: string;
    record: ClipRecord;
  }>;
}

function normalizedPath(path: string): string {
  return path.trim().replaceAll("\\", "/").replace(/^data\//i, "").toLowerCase();
}

function collectFromValue(value: JsonValue | undefined, paths: Set<string>): void {
  if (Array.isArray(value)) {
    value.forEach((entry) => collectFromValue(entry, paths));
    return;
  }
  if (!isObject(value)) return;
  for (const [key, entry] of Object.entries(value)) {
    if (key === "clip" && typeof entry === "string" && entry.trim()) paths.add(entry);
    if (key === "clips" && Array.isArray(entry)) {
      for (const clip of entry) {
        if (typeof clip === "string" && clip.trim()) paths.add(clip);
        else if (isObject(clip) && typeof clip.file === "string" && clip.file.trim()) paths.add(clip.file);
      }
    }
    collectFromValue(entry, paths);
  }
}

export function collectSceneClipPaths(root: JsonObject): string[] {
  const paths = new Set<string>();
  collectFromValue(root, paths);
  return [...paths];
}

export function resolveSceneDependencies(root: JsonObject, clips: ClipRecord[]): SceneDependency[] {
  const byPath = new Map<string, ClipRecord[]>();
  clips.forEach((clip) => {
    for (const path of new Set([normalizedPath(clip.gamePath), normalizedPath(clip.sourceFilename)])) {
      byPath.set(path, [...(byPath.get(path) ?? []), clip]);
    }
  });
  return collectSceneClipPaths(root).map((path) => {
    const matches = byPath.get(normalizedPath(path)) ?? [];
    const clip = matches[0];
    const status: SceneDependency["status"] = !clip
      ? "missing"
      : matches.length > 1 || clip.actorCount !== 1
        ? "incompatible"
        : "available";
    return { path, clip, status };
  });
}

function slug(value: string): string {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "").slice(0, 40) || "scene";
}

export function sceneFromLibraryInsertion(insertion: LibraryInsertion): JsonObject {
  const timestamp = Date.now().toString(36);
  if (insertion.type === "clip") {
    return {
      id: `library.${slug(insertion.clip.title)}-${timestamp}`,
      name: insertion.clip.title,
      tags: insertion.clip.tags,
      roles: [{ name: "actor1" }],
      stages: [{
        name: insertion.clip.title,
        clips: [{
          file: insertion.clip.gamePath,
          ...(insertion.clip.durationSeconds ? { sec: insertion.clip.durationSeconds } : {}),
        }],
      }],
    };
  }
  const stage = clipSetToOsfStage(insertion.clipSet, insertion.clips);
  return {
    id: `library.${slug(insertion.clipSet.title)}-${timestamp}`,
    name: insertion.clipSet.title,
    tags: insertion.clipSet.tags,
    roles: insertion.clipSet.members.map((member) => ({
      name: member.role,
      ...(Object.values(member.offset).some((value) => value !== 0) ? { offset: member.offset } : {}),
    })),
    stages: [{
      ...stage,
      clips: stage.clips.map((entry, index) => ({
        file: entry.file,
        ...(insertion.clips.find((clip) => clip.id === insertion.clipSet.members[index]?.clipId)?.durationSeconds
          ? { sec: insertion.clips.find((clip) => clip.id === insertion.clipSet.members[index]?.clipId)!.durationSeconds }
          : {}),
      })),
    }],
  };
}

export function appendScene(root: JsonObject, scene: JsonObject): { root: JsonObject; index: number } {
  if (Array.isArray(root.scenes)) {
    return { root: { ...root, scenes: [...root.scenes, scene] }, index: root.scenes.length };
  }
  const existing = scenesOf(root);
  if (existing.length) {
    return {
      root: {
        schema: typeof root.schema === "number" ? root.schema : 1,
        scenes: [root, scene],
      },
      index: 1,
    };
  }
  return { root: { schema: 1, scenes: [scene] }, index: 0 };
}

function zipAsync(entries: AsyncZippable): Promise<Uint8Array> {
  return new Promise((resolve, reject) => {
    zip(entries, { level: 6 }, (error, data) => error ? reject(error) : resolve(data));
  });
}

function unzipAsync(data: Uint8Array): Promise<Record<string, Uint8Array>> {
  let extractedBytes = 0;
  let entries = 0;
  return new Promise((resolve, reject) => {
    unzip(data, {
      filter(file) {
        entries += 1;
        extractedBytes += file.originalSize;
        if (entries > 512) throw new Error("Scene bundle contains too many entries.");
        if (extractedBytes > 256 * 1024 * 1024) throw new Error("Scene bundle expands beyond the 256 MiB safety limit.");
        return file.name === "manifest.json" || file.name.startsWith("assets/");
      },
    }, (error, result) => error ? reject(error) : resolve(result));
  });
}

function validManifest(value: unknown): value is SceneBundleManifest {
  if (!isObject(value)
    || value.format !== "osf-studio-scene-bundle"
    || value.version !== SCENE_BUNDLE_VERSION
    || !isObject(value.document)
    || typeof value.document.filename !== "string"
    || !isObject(value.document.root)
    || !Array.isArray(value.clips)) return false;
  return value.clips.every((entry) => isObject(entry)
    && typeof entry.entry === "string"
    && isObject(entry.record)
    && entry.record.schemaVersion === CLIP_LIBRARY_SCHEMA_VERSION);
}

export async function exportSceneBundle(
  document: StudioDocument,
  snapshot: ClipLibrarySnapshot,
  repository: Pick<ClipLibraryRepository, "getClipFile">,
): Promise<Blob> {
  const dependencies = resolveSceneDependencies(document.root, snapshot.clips);
  const blocked = dependencies.filter((entry) => entry.status !== "available");
  if (blocked.length) {
    throw new Error(`Bundle blocked: ${blocked.map((entry) => `${entry.path} (${entry.status})`).join(", ")}.`);
  }

  const records = [...new Map(dependencies.map((entry) => [entry.clip!.id, entry.clip!])).values()];
  const entries: AsyncZippable = {};
  const manifest: SceneBundleManifest = {
    format: "osf-studio-scene-bundle",
    version: SCENE_BUNDLE_VERSION,
    document: { filename: document.filename, root: document.root },
    clips: [],
  };
  for (const record of records) {
    const file = await repository.getClipFile(record.id);
    if (!file) throw new Error(`Bundle blocked: local bytes are missing for ${record.title}.`);
    const entry = `assets/${record.sha256}.${record.format}`;
    entries[entry] = new Uint8Array(await file.arrayBuffer());
    manifest.clips.push({ entry, record });
  }
  entries["manifest.json"] = strToU8(`${JSON.stringify(manifest, null, 2)}\n`);
  return new Blob([Uint8Array.from(await zipAsync(entries)).buffer], { type: "application/zip" });
}

export async function importSceneBundle(
  file: File,
  repository: Pick<ClipLibraryRepository, "importClip" | "updateClip">,
): Promise<{ document: StudioDocument; importedClips: number }> {
  assertFileSize(file, "bundle");
  const entries = await unzipAsync(new Uint8Array(await file.arrayBuffer()));
  const manifestBytes = entries["manifest.json"];
  if (!manifestBytes || manifestBytes.byteLength > 5 * 1024 * 1024) {
    throw new Error("Scene bundle is missing a valid manifest.json.");
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(strFromU8(manifestBytes));
  } catch {
    throw new Error("Scene bundle manifest is not valid JSON.");
  }
  if (!validManifest(parsed)) throw new Error("Scene bundle manifest is unsupported or malformed.");

  let importedClips = 0;
  for (const dependency of parsed.clips) {
    const bytes = entries[dependency.entry];
    if (!bytes) throw new Error(`Scene bundle is missing ${dependency.entry}.`);
    const source = dependency.record;
    const imported = await repository.importClip(new File([Uint8Array.from(bytes).buffer], source.sourceFilename, {
      type: source.format === "gltf" ? "model/gltf+json" : "application/octet-stream",
    }));
    if (imported.sha256 !== source.sha256) throw new Error(`Scene bundle asset hash mismatch for ${source.title}.`);
    await repository.updateClip({
      ...imported,
      title: source.title,
      gamePath: source.gamePath,
      author: source.author,
      tags: source.tags,
      actorCount: source.actorCount,
      durationSeconds: source.durationSeconds,
      rigId: source.rigId,
      animationCount: source.animationCount,
      boneCount: source.boneCount,
    });
    importedClips += 1;
  }

  return {
    document: parseDocument(parsed.document.filename, JSON.stringify(parsed.document.root)),
    importedClips,
  };
}
