import { parse as parseJsonc, printParseErrorCode, type ParseError } from "jsonc-parser";

export type JsonPrimitive = string | number | boolean | null;
export type JsonValue = JsonPrimitive | JsonObject | JsonValue[];
export interface JsonObject {
  [key: string]: JsonValue | undefined;
}

export interface StudioDocument {
  id: string;
  filename: string;
  root: JsonObject;
  dirty: boolean;
}

export interface Diagnostic {
  severity: "error" | "warning";
  source: "Studio structural validation" | "Game-dependent validation";
  path: string;
  message: string;
}

export interface SceneLocation {
  documentId: string;
  sceneIndex: number;
}

export const SAMPLE_SCENE: JsonObject = {
  schema: 1,
  pack: "My OSF Pack",
  scenes: [
    {
      id: "author.first-scene",
      name: "First Scene",
      tags: ["example"],
      lockPlayer: true,
      stripActors: true,
      roles: [
        { name: "lead", gender: "any", offset: { x: 0, y: 0, z: 0, heading: 0 } },
      ],
      stages: [
        {
          name: "Main",
          loops: 0,
          clips: [{ file: "OSF/Animations/MyPack/first.glb" }],
        },
      ],
    },
  ],
};

export function isObject(value: unknown): value is JsonObject {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function cloneJson<T extends JsonValue>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}

export class DocumentParseError extends Error {
  constructor(
    public readonly filename: string,
    public readonly line: number,
    public readonly column: number,
    message: string,
  ) {
    super(`${filename}:${line}:${column} — ${message}`);
    this.name = "DocumentParseError";
  }
}

function lineAndColumn(source: string, offset: number): { line: number; column: number } {
  const before = source.slice(0, offset);
  const lines = before.split(/\r\n|\r|\n/);
  return { line: lines.length, column: (lines.at(-1)?.length ?? 0) + 1 };
}

function describeParseError(error: ParseError): string {
  const code = printParseErrorCode(error.error);
  const descriptions: Record<string, string> = {
    InvalidSymbol: "Remove or quote the unexpected symbol.",
    InvalidNumberFormat: "Use a valid JSON number.",
    PropertyNameExpected: "Add a quoted property name.",
    ValueExpected: "Add a JSON value after the property name.",
    ColonExpected: "Add ':' after the property name.",
    CloseBraceExpected: "Add a closing '}'.",
    CloseBracketExpected: "Add a closing ']'.",
    CommaExpected: "Add a comma between values.",
    EndOfFileExpected: "Remove content after the root JSON value.",
  };
  return descriptions[code] ?? `Correct the JSONC syntax (${code}).`;
}

export function parseDocument(filename: string, source: string): StudioDocument {
  const errors: ParseError[] = [];
  const parsed: unknown = parseJsonc(source, errors, {
    allowTrailingComma: true,
    disallowComments: false,
    allowEmptyContent: false,
  });
  if (errors.length) {
    const first = errors[0];
    const location = lineAndColumn(source, first.offset);
    throw new DocumentParseError(filename, location.line, location.column, describeParseError(first));
  }
  if (!isObject(parsed)) throw new DocumentParseError(filename, 1, 1, "The file root must be a JSON object.");
  return {
    id: crypto.randomUUID(),
    filename: filename || "untitled.osf.json",
    root: parsed,
    dirty: false,
  };
}

export function scenesOf(root: JsonObject): JsonObject[] {
  if (Array.isArray(root.scenes)) return root.scenes.filter(isObject);
  return isObject(root) && typeof root.id === "string" ? [root] : [];
}

export function sceneAt(root: JsonObject, index: number): JsonObject | undefined {
  return scenesOf(root)[index];
}

export function replaceScene(root: JsonObject, index: number, next: JsonObject): JsonObject {
  if (Array.isArray(root.scenes)) {
    const scenes = [...root.scenes];
    scenes[index] = next;
    return { ...root, scenes };
  }
  if (index === 0) return next;
  return root;
}

export function removeScene(root: JsonObject, index: number): JsonObject {
  if (!Array.isArray(root.scenes)) return root;
  return { ...root, scenes: root.scenes.filter((_, sceneIndex) => sceneIndex !== index) };
}

export function addScene(root: JsonObject): { root: JsonObject; index: number } {
  const next: JsonObject = {
    id: "author.new-scene",
    name: "New Scene",
    roles: [{}],
    stages: [{ loops: 0, clips: [{ file: "OSF/Animations/clip.glb" }] }],
  };

  if (Array.isArray(root.scenes)) {
    return { root: { ...root, scenes: [...root.scenes, next] }, index: root.scenes.length };
  }

  const existing = scenesOf(root);
  if (existing.length === 1) {
    const envelope: JsonObject = {
      schema: typeof root.schema === "number" ? root.schema : 1,
      scenes: [root, next],
    };
    return { root: envelope, index: 1 };
  }

  return { root: { ...root, schema: 1, scenes: [next] }, index: 0 };
}

function label(path: string, message: string, severity: Diagnostic["severity"] = "error"): Diagnostic {
  return { severity, source: "Studio structural validation", path, message };
}

function validateScene(scene: JsonObject, path: string): Diagnostic[] {
  const issues: Diagnostic[] = [];
  const id = scene.id;
  if (typeof id !== "string" || !id.trim()) issues.push(label(`${path}.id`, "Scene id is required."));
  else if (id.includes("#")) issues.push(label(`${path}.id`, "Scene ids cannot contain #."));

  const roles = scene.roles;
  if (roles !== undefined && !Array.isArray(roles)) {
    issues.push(label(`${path}.roles`, "Scene roles must be an array."));
  }

  const stages = scene.stages;
  const nodes = scene.nodes;
  const clip = scene.clip;
  const shapeCount = Number(typeof clip === "string") + Number(Array.isArray(stages)) + Number(Array.isArray(nodes));
  if (shapeCount === 0) issues.push(label(path, "Scene needs clip, stages, or nodes."));
  if (shapeCount > 1) issues.push(label(path, "Use only one scene shape: clip, stages, or nodes."));

  if (Array.isArray(stages)) {
    if (!stages.length) issues.push(label(`${path}.stages`, "At least one stage is required."));
    stages.forEach((value, stageIndex) => {
      const stagePath = `${path}.stages[${stageIndex}]`;
      const stage = Array.isArray(value) ? { clips: value } : value;
      if (!isObject(stage)) {
        issues.push(label(stagePath, "Stage must be an object or clip array."));
        return;
      }
      if (!Array.isArray(stage.clips) || !stage.clips.length) {
        issues.push(label(`${stagePath}.clips`, "At least one clip is required."));
      } else if (Array.isArray(roles) && roles.length && roles.length !== stage.clips.length) {
        issues.push(label(
          `${stagePath}.clips`,
          `Clip count (${stage.clips.length}) must match role count (${roles.length}).`,
        ));
      }
      if (typeof stage.timer === "number" && stage.timer < 0) {
        issues.push(label(`${stagePath}.timer`, "Timer cannot be negative."));
      }
      if (typeof stage.loops === "number" && (!Number.isInteger(stage.loops) || stage.loops < 0)) {
        issues.push(label(`${stagePath}.loops`, "Loops must be a whole number of zero or greater."));
      }
    });
  }

  if (Array.isArray(nodes)) {
    issues.push(label(
      `${path}.nodes`,
      "Graph scenes are preserved and editable in JSON mode; visual graph editing is coming later.",
      "warning",
    ));
  }
  return issues;
}

export function validateDocuments(documents: StudioDocument[]): Map<string, Diagnostic[]> {
  const result = new Map<string, Diagnostic[]>();
  const seenIds = new Map<string, string>();

  for (const document of documents) {
    const issues: Diagnostic[] = [];
    if (document.root.schema !== 1) {
      issues.push(label("schema", "OSF scene files currently require schema 1."));
    }
    const scenes = scenesOf(document.root);
    if (!scenes.length) issues.push(label("scenes", "No scenes were found in this file."));
    scenes.forEach((scene, index) => {
      const path = Array.isArray(document.root.scenes) ? `scenes[${index}]` : "scene";
      issues.push(...validateScene(scene, path));
      if (typeof scene.id === "string" && scene.id.trim()) {
        const prior = seenIds.get(scene.id);
        if (prior) {
          issues.push(label(`${path}.id`, `Duplicate scene id; it is already used in ${prior}.`));
        } else {
          seenIds.set(scene.id, document.filename);
        }
      }
    });
    result.set(document.id, issues);
  }
  return result;
}

export function serializeDocument(document: StudioDocument): string {
  return `${JSON.stringify(document.root, null, 2)}\n`;
}
