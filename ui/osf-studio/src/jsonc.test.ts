import { describe, expect, it } from "vitest";
import { DocumentParseError, parseDocument, sceneAt, serializeDocument } from "./model";

describe("OSF JSONC", () => {
  it("imports comments and trailing commas and exports canonical JSON", () => {
    const document = parseDocument("commented.osf.json", `{
      // OSF supports comments
      "schema": 1,
      "scenes": [{ "id": "jsonc.scene", "clip": "clip.glb", "future": 7, }],
    }`);
    expect(sceneAt(document.root, 0)?.future).toBe(7);
    const output = serializeDocument(document);
    expect(output).not.toContain("//");
    expect(JSON.parse(output)).toEqual(document.root);
  });

  it("reports filename, line, column, and actionable syntax text", () => {
    try {
      parseDocument("broken.osf.json", "{\n  \"schema\": 1,\n  \"scenes\": [\n}");
      throw new Error("expected parser error");
    } catch (error) {
      expect(error).toBeInstanceOf(DocumentParseError);
      expect((error as Error).message).toMatch(/broken\.osf\.json:4:1/);
      expect((error as Error).message).toMatch(/closing|JSONC syntax|JSON value/i);
    }
  });
});

