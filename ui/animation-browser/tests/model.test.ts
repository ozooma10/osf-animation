import { describe, expect, it } from "vitest";
import { evaluateScene, normalizeScene, safeNormalizeScene } from "../src/model";

describe("scene normalization", () => {
  it("normalizes registry defaults and duration values", () => {
    const scene = normalizeScene({
      id: "test.scene",
      name: "Test Scene",
      roles: [{ name: "lead", filters: { gender: "female" } }],
      stages: [{ name: "idle", loopSec: "2.5", loops: 0 }],
    });
    expect(scene).toMatchObject({
      id: "test.scene",
      title: "Test Scene",
      species: "human",
      actorCount: 1,
      roles: [{ name: "lead", gender: "female" }],
      policy: { stripActors: "inherit", lockPlayer: "inherit", fade: "off" },
    });
    expect(scene.stages[0]).toMatchObject({ index: 0, loopSec: 2.5, loops: 0 });
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
