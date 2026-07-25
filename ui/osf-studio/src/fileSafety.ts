export const FILE_LIMITS = {
  json: 5 * 1024 * 1024,
  rig: 8 * 1024 * 1024,
  af: 32 * 1024 * 1024,
  gltf: 128 * 1024 * 1024,
} as const;

export type SupportedFileKind = keyof typeof FILE_LIMITS;

export class FileSafetyError extends Error {
  constructor(public readonly code: string, message: string) {
    super(message);
    this.name = "FileSafetyError";
  }
}

export function assertFileSize(file: Pick<File, "size">, kind: SupportedFileKind): void {
  const limit = FILE_LIMITS[kind];
  if (file.size > limit) {
    throw new FileSafetyError(
      "FILE_TOO_LARGE",
      `${kind.toUpperCase()} files are limited to ${Math.round(limit / 1024 / 1024)} MiB.`,
    );
  }
}

export function extensionOf(filename: string): string {
  const lower = filename.toLowerCase();
  if (lower.endsWith(".osf.json")) return ".osf.json";
  return lower.includes(".") ? `.${lower.split(".").at(-1)}` : "";
}

export function detectAnimationKind(filename: string): "glb" | "gltf" | "af" {
  const extension = extensionOf(filename);
  if (extension === ".glb") return "glb";
  if (extension === ".gltf") return "gltf";
  if (extension === ".af") return "af";
  throw new FileSafetyError(
    "UNSUPPORTED_EXTENSION",
    "Choose a .glb, self-contained .gltf, or Starfield .af animation file.",
  );
}

export function validateGlbSignature(bytes: ArrayBuffer): "glb" | "gzip" {
  if (bytes.byteLength < 4) throw new FileSafetyError("TRUNCATED_FILE", "The GLB is truncated.");
  const view = new Uint8Array(bytes, 0, 4);
  if (view[0] === 0x1f && view[1] === 0x8b) return "gzip";
  if (view[0] === 0x67 && view[1] === 0x6c && view[2] === 0x54 && view[3] === 0x46) return "glb";
  throw new FileSafetyError("INVALID_GLB_SIGNATURE", "The file does not contain a valid GLB or gzip signature.");
}

export function validateEmbeddedGltf(source: string): void {
  let parsed: unknown;
  try {
    parsed = JSON.parse(source);
  } catch {
    throw new FileSafetyError("INVALID_GLTF_JSON", "The glTF JSON could not be parsed.");
  }
  if (!parsed || typeof parsed !== "object") {
    throw new FileSafetyError("INVALID_GLTF_JSON", "The glTF root must be an object.");
  }
  const root = parsed as { buffers?: Array<{ uri?: unknown }>; images?: Array<{ uri?: unknown }> };
  const external = [...(root.buffers ?? []), ...(root.images ?? [])]
    .some((entry) => typeof entry.uri === "string" && !entry.uri.startsWith("data:"));
  if (external) {
    throw new FileSafetyError(
      "EXTERNAL_GLTF_DEPENDENCY",
      "This glTF references external files. Embed its buffers and images, or export it as a GLB.",
    );
  }
}

