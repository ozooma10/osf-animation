import { describe, expect, it } from "vitest";
import { evaluateScene, normalizeCatalog, normalizeScene, safeNormalizeScene } from "../src/model";

describe("scene normalization", () => {
  it("normalizes registry defaults and duration values", () => {
    const scene = normalizeScene({
      id: "test.scene",
      name: "Test Scene",
      folder: "Furniture / Seated",
      roles: [{ name: "lead", filters: { gender: "female" } }],
      inPlace: true,
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


  it("uses the three catalog source kinds and recognizes clipLibrary ids", () => {
    expect(normalizeScene({
      id: "osf.scene-clip/registered",
      sourceKind: "curatedAnimation",
    })).toMatchObject({ sourceKind: "curatedAnimation" });
    expect(normalizeScene({
      id: "plain",
      sourceKind: "curatedAnimation",
    })).toMatchObject({ sourceKind: "curatedAnimation" });
    expect(normalizeScene({ id: "osf.scene-clip/curated" }).sourceKind).toBe("curatedAnimation");
    expect(normalizeScene({ id: "pack.scene" }).sourceKind).toBe("authoredScene");
    expect(normalizeCatalog([
      { id: "a", sourceKind: "authoredScene" },
      { id: "c", sourceKind: "curatedAnimation" },
      { id: "r", sourceKind: "referenceAnimation" },
    ], true).map(({ sourceKind, library }) => [sourceKind, library])).toEqual([
      ["referenceAnimation", true],
      ["curatedAnimation", true],
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
