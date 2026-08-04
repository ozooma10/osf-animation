import { describe, expect, it } from "vitest";
import { diffImportReports, evaluateScene, normalizeImportReport, normalizeScene, safeNormalizeScene } from "../src/model";

describe("scene normalization", () => {
  it("normalizes registry defaults and duration values", () => {
    const scene = normalizeScene({
      id: "test.scene",
      name: "Test Scene",
      folder: "Furniture / Seated",
      roles: [{ name: "lead", filters: { gender: "female" } }],
      inPlace: true,
      stages: [{ name: "idle", loopSec: "2.5", loops: 0, tracks: [
        { kind: "cue", at: 0.25, anchor: "fraction", label: "helmet.off", repeat: true },
        { kind: "invalid", at: 2, label: "drop me" },
      ] }],
    });
    expect(scene).toMatchObject({
      id: "test.scene",
      title: "Test Scene",
      species: "human",
      actorCount: 1,
      roles: [{ name: "lead", gender: "female" }],
      policy: { stripActors: "inherit", lockPlayer: "inherit", fade: "off" },
      folder: "Furniture/Seated",
      inPlace: true,
    });
    expect(scene.stages[0]).toMatchObject({
      index: 0,
      loopSec: 2.5,
      loops: 0,
      tracks: [{ kind: "cue", at: 0.25, anchor: "fraction", label: "helmet.off", repeat: true }],
    });
    expect(scene).not.toHaveProperty("genders");
    expect(scene).not.toHaveProperty("shape");
    expect(scene.roles[0]).not.toHaveProperty("filters");
    expect(scene.roles[0]).not.toHaveProperty("equip");
    expect(scene.policy).not.toHaveProperty("camera");
  });

  it("does not let an invalid record blank a catalog", () => {
    expect(safeNormalizeScene(null)).toBeNull();
    expect(safeNormalizeScene({ id: "ok" })?.id).toBe("ok");
  });
});

describe("scene readiness", () => {
  const scene = normalizeScene({
    id: "bar.scene",
    actorCount: 2,
    requiresFurniture: true,
    anchors: ["Barstool"],
  });

  it("reports missing cast and furniture independently", () => {
    const result = evaluateScene(scene, { castCount: 1, furnitureToken: null, anchorMatch: null });
    expect(result.gaps).toBe(2);
    expect(result.issues).toEqual(["needs 1 more actor", "needs Barstool"]);
  });

  it("requires the keyed furniture to be in the authoritative match set", () => {
    const mismatch = evaluateScene(scene, {
      castCount: 2,
      furnitureToken: 7,
      anchorMatch: { token: 7, ids: new Set() },
    });
    expect(mismatch.anchorGate).toBe(false);

    const match = evaluateScene(scene, {
      castCount: 2,
      furnitureToken: 7,
      anchorMatch: { token: 7, ids: new Set(["bar.scene"]) },
    });
    expect(match.gaps).toBe(0);
  });
});

describe("import report normalization", () => {
  it("fills every field from a sparse native payload", () => {
    const report = normalizeImportReport({
      files: [{ path: "GE/chair.osf.json", file: "chair.osf.json", scenes: 3 }],
	  totals: { files: 1, scenes: 3, routes: 2, declaredRoutes: 3, rejectedRoutes: 1 },
    });
    expect(report.files[0]).toMatchObject({
      path: "GE/chair.osf.json",
      file: "chair.osf.json",
      pack: "",
      library: false,
      scenes: 3,
      nodes: 0,
      species: [],
      problems: [],
      problemCount: 0,
      rejected: false,
    });
	expect(report.totals).toMatchObject({ files: 1, scenes: 3, routes: 2, declaredRoutes: 3, rejectedRoutes: 1, errors: 0, parseMs: 0 });
  });

  it("survives a malformed payload instead of throwing at the panel", () => {
    expect(normalizeImportReport(null)).toEqual({ files: [], totals: expect.objectContaining({ files: 0 }) });
    expect(normalizeImportReport({ files: "nope" }).files).toEqual([]);
    const junk = normalizeImportReport({ files: [null, { scenes: -4, parseMs: "x", species: 7 }] });
    expect(junk.files).toHaveLength(2);
    expect(junk.files[1]).toMatchObject({ scenes: 0, parseMs: 0, species: [] });
  });

  it("never reports fewer problems than actually arrived", () => {
    // A truncating native side sends 2 lines with a count of 40; a buggy one could send a count
    // below the lines it shipped, which would make the panel claim negative hidden problems.
    const truncated = normalizeImportReport({ files: [{ problems: ["[warn] a", "[error] b"], problemCount: 40 }] });
    expect(truncated.files[0].problemCount).toBe(40);
    const understated = normalizeImportReport({ files: [{ problems: ["[warn] a", "[error] b"], problemCount: 1 }] });
    expect(understated.files[0].problemCount).toBe(2);
  });

  it("preserves structured repair guidance and compares reloads", () => {
    const before = normalizeImportReport({ files: [{
      path: "Pack/scenes.osf.json",
      file: "scenes.osf.json",
      declaredScenes: 2,
      scenes: 1,
      rejectedScenes: 1,
      errors: 1,
      problems: [{ severity: "error", code: "scene-invalid", message: "Bad role.", hint: "Fix the role.", scene: "pack.bad", role: "lead" }],
    }] });
    expect(before.files[0].problems[0]).toEqual({
      severity: "error",
      code: "scene-invalid",
      message: "Bad role.",
      hint: "Fix the role.",
      scene: "pack.bad",
      node: "",
      role: "lead",
      clip: "",
    });

    const after = normalizeImportReport({ files: [{ path: "Pack/scenes.osf.json", file: "scenes.osf.json", declaredScenes: 2, scenes: 2 }] });
    expect(diffImportReports(before.files, after.files)).toMatchObject({
      newProblems: [], resolvedProblems: [{ code: "scene-invalid" }], changedFiles: 1, addedFiles: 0, removedFiles: 0,
    });
  });
});
