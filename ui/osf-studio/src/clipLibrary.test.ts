import { describe, expect, it } from "vitest";
import {
  CLIP_LIBRARY_SCHEMA_VERSION,
  clipSetToOsfStage,
  resolveClipSet,
  type ClipRecord,
  type ClipSet,
} from "./clipLibrary";

const clip = (id: string, path: string): ClipRecord => ({
  schemaVersion: CLIP_LIBRARY_SCHEMA_VERSION,
  id,
  assetId: id,
  title: id,
  sourceFilename: path,
  gamePath: path,
  author: "",
  tags: [],
  actorCount: 1,
  format: "af",
  size: 10,
  sha256: id,
  createdAt: "2026-01-01T00:00:00.000Z",
  updatedAt: "2026-01-01T00:00:00.000Z",
});

const set: ClipSet = {
  schemaVersion: CLIP_LIBRARY_SCHEMA_VERSION,
  id: "clipset:test",
  title: "Conversation",
  author: "",
  tags: [],
  members: [
    { id: "a", role: "speaker", clipId: "one", offset: { x: 0, y: 0, z: 0, heading: 0 } },
    { id: "b", role: "listener", clipId: "two", offset: { x: 0, y: 0.5, z: 0, heading: 180 } },
  ],
  createdAt: "2026-01-01T00:00:00.000Z",
  updatedAt: "2026-01-01T00:00:00.000Z",
};

describe("Clip Library", () => {
  it("resolves multi-actor clip dependencies and emits an OSF-compatible stage", () => {
    const clips = [clip("one", "OSF/one.af"), clip("two", "OSF/two.af")];
    expect(resolveClipSet(set, clips).every((entry) => entry.clip)).toBe(true);
    expect(clipSetToOsfStage(set, clips)).toEqual({
      name: "Conversation",
      clips: [
        { file: "OSF/one.af" },
        { file: "OSF/two.af", offset: { x: 0, y: 0.5, z: 0, heading: 180 } },
      ],
    });
  });

  it("refuses to export a Clip Set with missing dependencies", () => {
    expect(() => clipSetToOsfStage(set, [clip("one", "OSF/one.af")])).toThrow("missing clip dependency");
  });
});
