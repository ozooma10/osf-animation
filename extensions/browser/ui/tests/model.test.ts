import { describe, expect, it } from "vitest";
import { diffImportReports, evaluateScene, normalizeCatalog, normalizeImportReport, normalizeScene, safeNormalizeScene } from "../src/model";

describe("scene normalization", () => {
  it("normalizes registry defaults and duration values", () => {
    const scene = normalizeScene({
      id: "test.scene",
      name: "Test Scene",
      folder: "Furniture / Seated",
      roles: [{ name: "lead", filters: { gender: "female" } }],
      placement: "followActor",
      stages: [{ name: "idle", loopSec: "2.5", loops: 0 }],
    });
    expect(scene).toMatchObject({
      id: "test.scene",
      title: "Test Scene",
      species: "human",
      actorCount: 1,
      roles: [{ name: "lead", gender: "female" }],
      policy: { hideApparel: "inherit", playerInputLock: "inherit", fade: "off" },
      folder: "Furniture/Seated",
      worldPlacement: "followActor",
    });
    expect(scene.stages[0]).toMatchObject({
      index: 0,
      loopSec: 2.5,
      loops: 0,
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

  it("uses explicit source kinds without inferring them from old flags, IDs or the catalog lane", () => {
    expect(normalizeScene({
      id: "osf.scene-clip/registered",
      curated: true,
      sourceKind: "derivedDebugAnimation",
    })).toMatchObject({ sourceKind: "derivedDebugAnimation" });
    expect(normalizeScene({
      id: "plain",
      curated: false,
      sourceKind: "curatedAnimation",
    })).toMatchObject({ sourceKind: "curatedAnimation" });
    expect(normalizeScene({ id: "osf.scene-clip/curated", curated: true }).sourceKind).toBe("authoredScene");
    expect(normalizeScene({ id: "osf.scene-clip/harvested" }).sourceKind).toBe("authoredScene");
    expect(normalizeScene({ id: "pack.scene" }).sourceKind).toBe("authoredScene");
    expect(normalizeCatalog([
      { id: "a", sourceKind: "authoredScene" },
      { id: "c", sourceKind: "curatedAnimation" },
      { id: "d", sourceKind: "derivedDebugAnimation" },
      { id: "r", sourceKind: "referenceAnimation" },
    ], true).map(({ sourceKind, library }) => [sourceKind, library])).toEqual([
      ["authoredScene", true],
      ["curatedAnimation", true],
      ["derivedDebugAnimation", true],
      ["referenceAnimation", true],
    ]);
  });

  it("reads canonical placement and policy fields and ignores retired aliases", () => {
    expect(normalizeScene({ placement: "anchorAndPin", inPlace: true,
      hideApparel: false, stripActors: true, playerInputLock: false, lockPlayer: true })).toMatchObject({
      worldPlacement: "anchorAndPin",
      policy: { hideApparel: "off", playerInputLock: "off" },
    });
    expect(normalizeScene({ inPlace: true, stripActors: false, lockPlayer: false })).toMatchObject({
      worldPlacement: "anchorAndPin",
      policy: { hideApparel: "inherit", playerInputLock: "inherit" },
    });
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
    const result = evaluateScene(scene, { castCount: 1, castTokens: [-1], castMatch: null, furnitureToken: null, anchorMatch: null });
    expect(result.gaps).toBe(2);
    expect(result.issues).toEqual(["needs 1 more actor", "needs Barstool"]);
  });

  it("requires the keyed furniture to be in the authoritative match set", () => {
    const mismatch = evaluateScene(scene, {
      castCount: 2,
      castTokens: [-1, 7],
      castMatch: { tokens: [-1, 7], ids: new Set(["bar.scene"]) },
      furnitureToken: 7,
      anchorMatch: { token: 7, ids: new Set() },
    });
    expect(mismatch.anchorGate).toBe(false);

    const match = evaluateScene(scene, {
      castCount: 2,
      castTokens: [-1, 7],
      castMatch: { tokens: [-1, 7], ids: new Set(["bar.scene"]) },
      furnitureToken: 7,
      anchorMatch: { token: 7, ids: new Set(["bar.scene"]) },
    });
    expect(match.gaps).toBe(0);
  });

  it("waits for and enforces the native role-filter result for the exact ordered cast", () => {
    const roleScene = normalizeScene({ id: "roles.scene", actorCount: 1, roles: [{ name: "lead", gender: "female" }] });
    const context = { castCount: 1, castTokens: [-1], furnitureToken: null, anchorMatch: null };

    const pending = evaluateScene(roleScene, { ...context, castMatch: null });
    expect(pending).toMatchObject({ roleMatchKnown: false, roleFilterGate: false, gaps: 1 });
    expect(pending.reason).toContain("Checking whether this cast fits");

    const rejected = evaluateScene(roleScene, { ...context, castMatch: { tokens: [-1], ids: new Set() } });
    expect(rejected).toMatchObject({ roleMatchKnown: true, roleFilterGate: false, gaps: 1 });
    expect(rejected.reason).toContain("doesn't fit the scene roles");

    const accepted = evaluateScene(roleScene, { ...context, castMatch: { tokens: [-1], ids: new Set(["roles.scene"]) } });
    expect(accepted).toMatchObject({ roleMatchKnown: true, roleFilterGate: true, gaps: 0 });

    const stale = evaluateScene(roleScene, { ...context, castMatch: { tokens: [7], ids: new Set(["roles.scene"]) } });
    expect(stale.roleMatchKnown).toBe(false);
  });
});

describe("import report normalization", () => {
  it("fills every field from a sparse native payload", () => {
    const report = normalizeImportReport({
      files: [{ path: "GE/chair.osf.json", file: "chair.osf.json", scenes: 3 }],
	  totals: { files: 1, scenes: 3 },
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
	expect(report.totals).toMatchObject({ files: 1, scenes: 3, errors: 0, parseMs: 0 });
  });

  it("survives a malformed payload instead of throwing at the panel", () => {
    expect(normalizeImportReport(null)).toEqual({ files: [], totals: expect.objectContaining({ files: 0 }) });
    expect(normalizeImportReport({ files: "nope" }).files).toEqual([]);
    const junk = normalizeImportReport({ files: [null, { scenes: -4, parseMs: "x", species: 7 }] });
    expect(junk.files).toHaveLength(2);
    expect(junk.files[1]).toMatchObject({ scenes: 0, parseMs: 0, species: [] });
  });

  it("never reports fewer problems than actually arrived", () => {
    // A count below the shipped lines would claim negative hidden problems.
    const problems = [{ severity: "warn", message: "a" }, { severity: "error", message: "b" }];
    const truncated = normalizeImportReport({ files: [{ problems, problemCount: 40 }] });
    expect(truncated.files[0].problemCount).toBe(40);
    const understated = normalizeImportReport({ files: [{ problems, problemCount: 1 }] });
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
