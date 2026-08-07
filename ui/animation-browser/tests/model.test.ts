import { describe, expect, it } from "vitest";
import { diffImportReports, evaluateScene, normalizeCatalog, normalizeImportReport, normalizeRouteCatalog, normalizeScene, safeNormalizeScene } from "../src/model";

describe("scene normalization", () => {
  it("normalizes registry defaults and duration values", () => {
    const scene = normalizeScene({
      id: "test.scene",
      name: "Test Scene",
      folder: "Furniture / Seated",
      roles: [{ name: "lead", filters: { gender: "female" } }],
      inPlace: true,
      stages: [{ name: "idle", loopSec: "2.5", loops: 0, tracks: [
        { kind: "cue", at: 0.25, trackPosition: "fraction", label: "helmet.off", repeat: true },
        { kind: "invalid", at: 2, label: "drop me" },
      ] }],
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
      tracks: [{ kind: "cue", at: 0.25, trackPosition: "fraction", label: "helmet.off", repeat: true }],
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

  it("normalizes the legacy temporal anchor field without retaining spatially ambiguous language", () => {
    const scene = normalizeScene({ id: "legacy.tracks", stages: [{ tracks: [
      { kind: "cue", at: 0, anchor: "enter", label: "legacy" },
      { kind: "cue", at: 1, anchor: "enter", trackPosition: "end", label: "canonical" },
    ] }] });
    expect(scene.stages[0].tracks.map((mark) => mark.trackPosition)).toEqual(["enter", "end"]);
    expect(scene.stages[0].tracks[0]).not.toHaveProperty("anchor");
  });

  it("uses explicit catalog source kinds while preserving legacy bridge inference", () => {
    expect(normalizeScene({
      id: "osf.scene-clip/registered",
      curated: true,
      sourceKind: "derivedDebugAnimation",
    })).toMatchObject({ sourceKind: "derivedDebugAnimation", curated: false });
    expect(normalizeScene({
      id: "plain",
      curated: false,
      sourceKind: "curatedAnimation",
    })).toMatchObject({ sourceKind: "curatedAnimation", curated: true });
    expect(normalizeScene({ id: "osf.scene-clip/curated", curated: true }).sourceKind).toBe("curatedAnimation");
    expect(normalizeScene({ id: "osf.scene-clip/harvested" }).sourceKind).toBe("derivedDebugAnimation");
    expect(normalizeScene({ id: "pack.scene" }).sourceKind).toBe("authoredScene");
    expect(normalizeCatalog([
      { id: "a", sourceKind: "authoredScene" },
      { id: "c", sourceKind: "curatedAnimation" },
      { id: "d", sourceKind: "derivedDebugAnimation" },
      { id: "r", sourceKind: "referenceAnimation" },
    ], true).map(({ sourceKind, library }) => [sourceKind, library])).toEqual([
      ["referenceAnimation", true],
      ["curatedAnimation", true],
      ["derivedDebugAnimation", true],
      ["referenceAnimation", true],
    ]);
  });

  it("prefers canonical placement and policy fields while accepting legacy bridge fields", () => {
    expect(normalizeScene({ placement: "anchorAndPin", inPlace: true,
      hideApparel: false, stripActors: true, playerInputLock: false, lockPlayer: true })).toMatchObject({
      worldPlacement: "anchorAndPin",
      policy: { hideApparel: "off", playerInputLock: "off" },
    });
  });
});

describe("route debugger normalization", () => {
  it("builds ordered transition event lanes from the native route catalog", () => {
    const routes = normalizeRouteCatalog([{
      id: "helmet.route",
      sourceFile: "Suit\\routes.osf.json",
      stations: [{ id: "head" }, { id: "held", layer: { clip: "held.af", mask: "upperBody", holdAt: 1 } }],
      transitions: [{
        id: "head-to-held", from: "head", to: "held",
        layer: { clip: "head_to_held.af", durationHint: 3.3, mask: "upperBody", mode: "override", weight: 1 },
        commit: { frame: 24, id: "helmet.commit" },
        markers: [{ frame: 20, id: "helmet.moving" }],
        props: [{ frame: 18, id: "helmet", attach: true, lifetime: "station",
          attachmentNode: "R_Canonical", node: "R_Legacy" }],
        sounds: [{ frame: 23, spec: "$helmet,move" }],
      }],
    }]);

    expect(routes[0]).toMatchObject({ id: "helmet.route", sourceFile: "Suit/routes.osf.json" });
    expect(routes[0].stations[1].layer).toMatchObject({ clip: "held.af", holdAt: 1 });
    expect(routes[0].transitions[0].interruption).toBe("finish-transition");
    expect(routes[0].transitions[0].events.map((event) => [event.kind, event.frame])).toEqual([
      ["prop", 18], ["marker", 20], ["sound", 23], ["commit", 24],
    ]);
    expect(routes[0].transitions[0].events[0]).toMatchObject({ label: "ATTACH helmet", lifetime: "station", external: false });
    expect(routes[0].transitions[0].events[0].detail).toContain("attachment node R_Canonical");
    expect(routes[0].transitions[0].events[0].detail).not.toContain("R_Legacy");
    expect(routes[0].transitions[0].events[3]).toMatchObject({ external: true });
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
