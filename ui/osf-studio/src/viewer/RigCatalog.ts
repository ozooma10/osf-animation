import { inspectRigMetadata } from "../afDecoder";
import { assertFileSize } from "../fileSafety";
import {
  createWorkspaceRepository,
  sha256,
  type StoredAsset,
  type WorkspaceRepository,
} from "../workspaceRepository";
import type { RigSource } from "./AnimationLoader";

const LAST_RIG_KEY = "osf-studio.last-rig-id";

export interface RigDescriptor {
  id: string;
  name: string;
  sha256: string;
  size: number;
  totalBones: number;
  animatedBones: number;
  source: "served" | "remembered" | "manual";
}

export class RigCatalog {
  private repository?: WorkspaceRepository;

  async initialize(): Promise<{ served: string[]; remembered?: { descriptor: RigDescriptor; rig: RigSource } }> {
    this.repository = (await createWorkspaceRepository()).repository;
    const served = await fetch("/__osf/rigs", { cache: "no-store" })
      .then((response) => response.ok ? response.json() : [])
      .then((value: unknown) => Array.isArray(value)
        ? value.filter((entry): entry is string => typeof entry === "string")
        : [])
      .catch(() => []);
    let rememberedId: string | null = null;
    try { rememberedId = localStorage.getItem(LAST_RIG_KEY); } catch { /* Preference storage may be unavailable. */ }
    if (!rememberedId) return { served };
    const asset = await this.repository.getAsset(rememberedId);
    if (!asset) return { served };
    return { served, remembered: await this.fromAsset(asset, "remembered") };
  }

  async selectManual(file: File): Promise<{ descriptor: RigDescriptor; rig: RigSource }> {
    if (!file.name.toLowerCase().endsWith(".rig")) throw new Error("Choose a .rig file.");
    assertFileSize(file, "rig");
    const hash = await sha256(file);
    const asset: StoredAsset = {
      id: hash,
      kind: "rig",
      name: file.name,
      mediaType: file.type || "application/octet-stream",
      size: file.size,
      sha256: hash,
      createdAt: new Date().toISOString(),
      blob: file,
    };
    await this.repository?.saveAsset(asset);
    try { localStorage.setItem(LAST_RIG_KEY, hash); } catch { /* The rig bytes still remain in IndexedDB. */ }
    return this.fromAsset(asset, "manual");
  }

  async selectServed(name: string): Promise<{ descriptor: RigDescriptor; rig: RigSource }> {
    if (!name || name.includes("\\") || name.split("/").some((part) => !part || part === "." || part === "..")) {
      throw new Error("The served rig name is invalid.");
    }
    const encoded = name.split("/").map(encodeURIComponent).join("/");
    const response = await fetch(`/__osf/rigs/${encoded}`, { cache: "no-store" });
    if (!response.ok) throw new Error(`The local rig service returned ${response.status}.`);
    const blob = await response.blob();
    assertFileSize({ size: blob.size } as File, "rig");
    const hash = await sha256(blob);
    const metadata = inspectRigMetadata(await blob.arrayBuffer());
    return {
      descriptor: {
        id: hash,
        name,
        sha256: hash,
        size: blob.size,
        totalBones: metadata.totalBones,
        animatedBones: metadata.animatedBones,
        source: "served",
      },
      rig: {
        name,
        bytes: await blob.arrayBuffer(),
        sha256: hash,
        totalBones: metadata.totalBones,
      },
    };
  }

  private async fromAsset(
    asset: StoredAsset,
    source: RigDescriptor["source"],
  ): Promise<{ descriptor: RigDescriptor; rig: RigSource }> {
    const bytes = await asset.blob.arrayBuffer();
    const metadata = inspectRigMetadata(bytes);
    return {
      descriptor: {
        id: asset.id,
        name: asset.name,
        sha256: asset.sha256,
        size: asset.size,
        totalBones: metadata.totalBones,
        animatedBones: metadata.animatedBones,
        source,
      },
      rig: { name: asset.name, bytes, sha256: asset.sha256, totalBones: metadata.totalBones },
    };
  }
}

