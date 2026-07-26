import { openDB, type IDBPDatabase } from "idb";
import type { StoredAsset } from "../workspaceRepository";
import {
  ANIMATION_PROJECT_SCHEMA_VERSION,
  type AnimationProject,
} from "./AnimationProject";

const DATABASE_NAME = "osf-studio";
export const STUDIO_DATABASE_VERSION = 3;

function validProject(value: unknown): value is AnimationProject {
  if (!value || typeof value !== "object") return false;
  const project = value as Partial<AnimationProject>;
  return project.schemaVersion === ANIMATION_PROJECT_SCHEMA_VERSION
    && typeof project.id === "string"
    && typeof project.title === "string"
    && typeof project.rigAssetId === "string"
    && typeof project.rigSha256 === "string"
    && typeof project.fps === "number"
    && typeof project.durationFrames === "number"
    && Boolean(project.tracks && typeof project.tracks === "object");
}

export class AnimationProjectRepository {
  private constructor(private readonly db: IDBPDatabase) {}

  static async open(): Promise<AnimationProjectRepository> {
    const db = await openDB(DATABASE_NAME, STUDIO_DATABASE_VERSION, {
      upgrade(database) {
        if (!database.objectStoreNames.contains("workspace")) database.createObjectStore("workspace");
        if (!database.objectStoreNames.contains("assets")) database.createObjectStore("assets");
        if (!database.objectStoreNames.contains("clips")) database.createObjectStore("clips", { keyPath: "id" });
        if (!database.objectStoreNames.contains("clipSets")) database.createObjectStore("clipSets", { keyPath: "id" });
        if (!database.objectStoreNames.contains("animationProjects")) {
          database.createObjectStore("animationProjects", { keyPath: "id" });
        }
      },
    });
    return new AnimationProjectRepository(db);
  }

  close(): void {
    this.db.close();
  }

  async list(): Promise<AnimationProject[]> {
    const projects = await this.db.getAll("animationProjects") as unknown[];
    return projects.filter(validProject)
      .sort((left, right) => right.updatedAt.localeCompare(left.updatedAt));
  }

  async save(project: AnimationProject): Promise<void> {
    if (!validProject(project)) throw new Error("The animation project is invalid.");
    await this.db.put("animationProjects", project);
  }

  async delete(id: string): Promise<void> {
    await this.db.delete("animationProjects", id);
  }

  async getRigAsset(id: string): Promise<StoredAsset | null> {
    return (await this.db.get("assets", id) as StoredAsset | undefined) ?? null;
  }
}
