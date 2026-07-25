import { describe, expect, it, vi } from "vitest";
import { CLIP_LIBRARY_SCHEMA_VERSION, type ClipRecord, type ClipSet } from "./clipLibrary";
import {
  appendScene,
  collectSceneClipPaths,
  exportSceneBundle,
  importSceneBundle,
  resolveSceneDependencies,
  sceneFromLibraryInsertion,
} from "./sceneLibrary";
import type { StudioDocument } from "./model";

function clip(id: string, path: string, contents = id): ClipRecord {
  return {
    schemaVersion: CLIP_LIBRARY_SCHEMA_VERSION,
    id: `clip:${id}`,
    assetId: id,
    title: id,
    sourceFilename: `${id}.glb`,
    gamePath: path,
    author: "Tester",
    tags: ["test"],
    actorCount: 1,
    durationSeconds: 2,
    format: "glb",
    size: contents.length,
    sha256: id,
    createdAt: "2026-01-01T00:00:00.000Z",
    updatedAt: "2026-01-01T00:00:00.000Z",
  };
}

describe("scene library integration", () => {
  it("finds scene dependencies by normalized game path", () => {
    const root = { schema: 1, scenes: [{ id: "test", stages: [{ clips: ["OSF\\one.glb", { file: "OSF/two.glb" }] }] }] };
    expect(collectSceneClipPaths(root)).toEqual(["OSF\\one.glb", "OSF/two.glb"]);
    expect(resolveSceneDependencies(root, [clip("one", "OSF/one.glb")]).map((entry) => entry.status))
      .toEqual(["available", "missing"]);
  });

  it("creates a native-compatible scene from a multi-actor Clip Set", () => {
    const clips = [clip("one", "OSF/one.glb"), clip("two", "OSF/two.glb")];
    const set: ClipSet = {
      schemaVersion: CLIP_LIBRARY_SCHEMA_VERSION,
      id: "clipset:test",
      title: "Conversation",
      author: "",
      tags: ["talk"],
      members: [
        { id: "a", role: "speaker", clipId: clips[0].id, offset: { x: 0, y: 0, z: 0, heading: 0 } },
        { id: "b", role: "listener", clipId: clips[1].id, offset: { x: 0, y: 1, z: 0, heading: 180 } },
      ],
      createdAt: "2026-01-01T00:00:00.000Z",
      updatedAt: "2026-01-01T00:00:00.000Z",
    };
    const scene = sceneFromLibraryInsertion({ type: "clipSet", clipSet: set, clips });
    expect(scene.roles).toEqual([
      { name: "speaker" },
      { name: "listener", offset: { x: 0, y: 1, z: 0, heading: 180 } },
    ]);
    expect(scene.stages).toEqual([{
      name: "Conversation",
      clips: [{ file: "OSF/one.glb", sec: 2 }, { file: "OSF/two.glb", sec: 2 }],
    }]);
    expect(appendScene({ schema: 1, scenes: [] }, scene).index).toBe(0);
  });

  it("round-trips a portable scene bundle", async () => {
    const source = clip("hash", "OSF/hash.glb", "glTF");
    source.sha256 = "hash";
    const document: StudioDocument = {
      id: "doc",
      filename: "bundle.osf.json",
      dirty: false,
      root: { schema: 1, id: "bundle", clip: "OSF/hash.glb" },
    };
    const blob = await exportSceneBundle(document, { clips: [source], clipSets: [] }, {
      getClipFile: vi.fn(async () => new File([new TextEncoder().encode("glTF")], "hash.glb")),
    });
    const imported = clip("hash", "hash.glb", "glTF");
    imported.sha256 = "hash";
    const result = await importSceneBundle(new File([blob], "bundle.osfscene"), {
      importClip: vi.fn(async () => imported),
      updateClip: vi.fn(async (record) => record),
    });
    expect(result.document.filename).toBe("bundle.osf.json");
    expect(result.importedClips).toBe(1);
  });
});
