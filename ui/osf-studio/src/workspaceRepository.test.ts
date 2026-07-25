import { describe, expect, it } from "vitest";
import {
  LEGACY_BACKUP_KEY,
  WORKSPACE_SCHEMA_VERSION,
  createMemoryWorkspaceRepository,
  migrateLegacyValue,
  validateWorkspace,
} from "./workspaceRepository";

const legacy = JSON.stringify([{
  id: "doc",
  filename: "legacy.osf.json",
  root: { schema: 1, scenes: [{ id: "legacy.scene", clip: "clip.glb" }] },
  dirty: true,
}]);

describe("workspace persistence", () => {
  it("migrates a valid legacy draft into a versioned envelope", () => {
    const migrated = migrateLegacyValue(legacy);
    expect(migrated.schemaVersion).toBe(WORKSPACE_SCHEMA_VERSION);
    expect(migrated.documents[0].filename).toBe("legacy.osf.json");
    expect(migrated.selection.documentId).toBe("doc");
  });

  it("rejects corrupt or unrecoverable legacy values", () => {
    expect(() => migrateLegacyValue("{")).toThrow();
    expect(() => migrateLegacyValue("[]")).toThrow(/valid OSF Studio workspace/);
    expect(LEGACY_BACKUP_KEY).toContain("alpha-backup");
  });

  it("rejects unsupported workspace schema versions", () => {
    expect(validateWorkspace({ schemaVersion: 999, documents: [] })).toBeNull();
  });

  it("provides working save, load, assets, and backup in memory fallback", async () => {
    const repository = createMemoryWorkspaceRepository();
    const workspace = migrateLegacyValue(legacy);
    await repository.save(workspace);
    await repository.saveAsset({
      id: "abc",
      kind: "rig",
      name: "skeleton.rig",
      mediaType: "application/octet-stream",
      size: 3,
      sha256: "abc",
      createdAt: "2026-01-01T00:00:00.000Z",
      blob: new Blob([new Uint8Array([1, 2, 3])]),
    });
    expect((await repository.load())?.documents).toHaveLength(1);
    expect((await repository.getAsset("abc"))?.size).toBe(3);
    expect(await (await repository.exportBackup()).text()).not.toContain("1,2,3");
  });
});

