import { GLTFExporter } from "three/examples/jsm/exporters/GLTFExporter.js";
import { decodeRigSkeleton } from "../afDecoder";
import { ClipLibraryRepository, type ClipRecord } from "../clipLibrary";
import { AnimationLoader, type RigSource } from "../viewer/AnimationLoader";
import { buildAnimationClip, type AnimationProject } from "./AnimationProject";

function safeFilename(title: string): string {
  const stem = title.trim().replace(/[<>:"/\\|?*\u0000-\u001f]+/g, "-").replace(/\s+/g, "_");
  return `${stem || "OSF_Animation"}.glb`;
}

export async function exportAnimationProject(
  project: AnimationProject,
  rig: RigSource,
): Promise<File> {
  if (!Object.keys(project.tracks).length) throw new Error("Add at least one bone key before exporting.");
  if (project.rigSha256 !== rig.sha256) throw new Error("The active rig does not match this project.");
  const decoded = decodeRigSkeleton(rig.bytes.slice(0));
  const clip = buildAnimationClip(project);
  const exporter = new GLTFExporter();
  const result = await exporter.parseAsync(decoded.scene, {
    binary: true,
    animations: [clip],
    trs: true,
    onlyVisible: false,
  });
  if (!(result instanceof ArrayBuffer)) throw new Error("Binary GLB export did not produce an ArrayBuffer.");
  return new File([result], safeFilename(project.title), {
    type: "model/gltf-binary",
    lastModified: Date.now(),
  });
}

export async function validateAndAddExportToLibrary(file: File): Promise<ClipRecord> {
  const loaded = await new AnimationLoader().load(file);
  try {
    if (!loaded.clips.length) throw new Error("The exported GLB did not contain an animation.");
    if (!loaded.skeleton.formalBones && !loaded.skeleton.animatedTransforms) {
      throw new Error("The exported GLB did not preserve its animated rig hierarchy.");
    }
  } finally {
    loaded.dispose();
  }
  const library = await ClipLibraryRepository.open();
  try {
    return await library.importClip(file);
  } finally {
    library.close();
  }
}
