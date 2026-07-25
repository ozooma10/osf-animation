import {
  addScene,
  parseDocument,
  replaceScene,
  sceneAt,
  scenesOf,
  validateDocuments,
} from "../src/model";
import { describe, expect, it } from "vitest";

describe("OSF Studio document model", () => {
  it("accepts a bare scene without discarding unknown fields", () => {
    const document = parseDocument("wave.osf.json", JSON.stringify({
      schema: 1,
      id: "author.wave",
      clip: "Wave.glb",
      futureField: { enabled: true },
    }));
    const scene = sceneAt(document.root, 0)!;
    const updated = replaceScene(document.root, 0, { ...scene, name: "Wave" });
    expect(updated.futureField).toEqual({ enabled: true });
  });

  it("converts a bare scene to an envelope when adding another scene", () => {
    const document = parseDocument("wave.osf.json", JSON.stringify({
      schema: 1,
      id: "author.wave",
      clip: "Wave.glb",
    }));
    const result = addScene(document.root);
    expect(scenesOf(result.root)).toHaveLength(2);
    expect(result.index).toBe(1);
  });

  it("reports mismatched role and clip counts", () => {
    const document = parseDocument("bad.osf.json", JSON.stringify({
      schema: 1,
      scenes: [{
        id: "author.bad",
        roles: [{}, {}],
        stages: [{ clips: ["only-one.glb"] }],
      }],
    }));
    const issues = validateDocuments([document]).get(document.id)!;
    expect(issues.some((issue) => issue.message.includes("match role count"))).toBe(true);
  });

  it("reports duplicate ids across files", () => {
    const first = parseDocument("one.osf.json", '{"schema":1,"id":"same","clip":"a.glb"}');
    const second = parseDocument("two.osf.json", '{"schema":1,"id":"same","clip":"b.glb"}');
    const issues = validateDocuments([first, second]).get(second.id)!;
    expect(issues.some((issue) => issue.message.includes("Duplicate scene id"))).toBe(true);
  });
});
