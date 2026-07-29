import type { AnimationBridge, NativeMessageListener } from "../bridge/client";
import { isRecord, type BridgeCommand, type NativeMessage } from "../bridge/contract";
import { WHEEL_MAX, sceneById, wheelPool } from "../app/selectors";
import { PREFERENCE_KEYS } from "../app/settings";
import { PLAYER_TOKEN, type ActiveScene, type BrowserPreferences, type BrowserState } from "../app/state";
import { MOCK_ACTORS, MOCK_ANCHORS, MOCK_ANCHOR_MATCH, MOCK_CATALOG, MOCK_LIBRARY } from "./mock-data";

async function fetchFixture(name: "catalog" | "library"): Promise<unknown[] | null> {
  // In the CLI harness these source modules are served through Vite's /@fs/
  // route, so resolving from import.meta.url preserves the existing fixture
  // workflow without adding a second server plugin. Deliberately uncached: the
  // refresh button exists so a regenerated snapshot is picked up without a
  // reload.
  for (const suffix of [".local.json", ".json"]) {
    try {
      // Relative to src/dev/ — two levels up is the project root, which is where
      // fixtures/live/ lives. (Three silently resolved to ui/, so every snapshot
      // 404'd and standalone fell back to the built-in mock catalog.)
      const url = new URL("../../fixtures/live/" + name + suffix, import.meta.url);
      const response = await fetch(url, { cache: "no-store" });
      if (!response.ok) continue;
      const value: unknown = await response.json();
      if (Array.isArray(value)) return value;
    } catch {
      // Try the committed fallback, then use the built-in mock catalog.
    }
  }
  return null;
}

/** Standalone implementation of the same JSON-level behavior the native DLL exposes. */
export class StandaloneBridge implements AnimationBridge {
  readonly standalone = true;
  private readonly listeners = new Set<NativeMessageListener>();
  private readonly timers = new Set<number>();
  private active: ActiveScene[] = [];
  private nextHandle = 42;
  private customized = false;
  private pins: Array<{ scene: string; stage?: number }> = [];
  /** Last catalog payload served, so a wheel edit re-pins it instead of re-fetching. */
  private catalogRaw: unknown[] | null = null;

  constructor(private readonly getState: () => BrowserState) {}

  subscribe(listener: NativeMessageListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private emit(message: NativeMessage): void {
    for (const listener of this.listeners) listener(message);
  }

  private later(message: NativeMessage, delay = 40): void {
    const timer = window.setTimeout(() => { this.timers.delete(timer); this.emit(message); }, delay);
    this.timers.add(timer);
  }

  private emitCatalog(raw: unknown[]): void {
    this.catalogRaw = raw;
    this.emit({ type: "osf.animation.catalog.data", payload: this.applyPins(raw, false) });
  }

  private applyPins(raw: unknown[], library: boolean): unknown[] {
    return raw.map((value) => {
      if (!isRecord(value)) return value;
      const scene = String(value.id || "");
      if (!library) return { ...value, wheelCustomized: this.customized, pinned: this.pins.findIndex((pin) => pin.scene === scene && pin.stage == null) + 1 };
      return {
        ...value,
        wheelCustomized: this.customized,
        stages: Array.isArray(value.stages) ? value.stages.map((stage: unknown, index: number) => isRecord(stage)
          ? { ...stage, pinned: this.pins.findIndex((pin) => pin.scene === scene && pin.stage === (Number.isInteger(stage.index) ? stage.index : index)) + 1 }
          : stage) : [],
      };
    });
  }

  send(command: BridgeCommand, fields: Record<string, unknown> = {}): void {
    if (command === "osf.animation.catalog.get") {
      // The engine piggybacks its identity (and the player's sex) on the catalog request.
      this.later({ type: "osf.animation.version", payload: { plugin: "OSF Animation", version: "0.0.0-dev", playerSex: "female" } }, 20);
      void fetchFixture("catalog").then((fixture) => this.emitCatalog(fixture ?? MOCK_CATALOG));
    } else if (command === "osf.animation.library.get") {
      void fetchFixture("library").then((fixture) => this.emit({ type: "osf.animation.library.data", payload: this.applyPins(fixture ?? MOCK_LIBRARY, true) }));
    } else if (command === "osf.animation.anchorMatch") {
      this.later({ type: "osf.animation.anchorMatch", payload: { token: fields.token, sceneIds: MOCK_ANCHOR_MATCH[Number(fields.token)] ?? [] } }, 70);
    } else if (command === "osf.animation.pickCrosshair" || command === "osf.animation.pickScreen") {
      const item = fields.slot === "furniture" ? MOCK_ANCHORS[0] : MOCK_ACTORS[0];
      this.later({ type: "osf.animation.pick", payload: { slot: fields.slot, valid: true, ...item } }, 60);
    } else if (command === "osf.animation.scanNearby") {
      this.later({ type: "osf.animation.scanResults", payload: { kind: fields.kind, items: fields.kind === "furniture" ? MOCK_ANCHORS : MOCK_ACTORS } }, 80);
    } else if (command === "osf.animation.projectPickables") {
      const width = Number(fields.width) || 1280;
      const height = Number(fields.height) || 720;
      const items = fields.slot === "furniture"
        ? [
          { x: 0.58, y: 0.52, cx: 0.58, cy: 0.58, rx: width * 0.05, ry: height * 0.08 },
          { x: 0.72, y: 0.60, cx: 0.72, cy: 0.66, rx: width * 0.06, ry: height * 0.09 },
          { x: 0.88, y: 0.44, cx: 0.88, cy: 0.50, rx: width * 0.04, ry: height * 0.07 },
        ]
        : [
          { x: 0.62, y: 0.30, cx: 0.62, cy: 0.42, rx: width * 0.055, ry: height * 0.16 },
          { x: 0.82, y: 0.38, cx: 0.82, cy: 0.48, rx: width * 0.045, ry: height * 0.13 },
        ];
      this.later({ type: "osf.animation.pickTargets", payload: { slot: fields.slot, items } }, 5);
    } else if (command === "osf.animation.projectActors") {
      const tokens = Array.isArray(fields.tokens) ? fields.tokens.map(Number) : [];
      this.later({ type: "osf.animation.actorIndicators", payload: {
        items: tokens.map((token, index) => ({ token, x: 0.68 + index * 0.12, y: 0.34 + index * 0.06, visible: true })),
      } }, 5);
    } else if (command === "osf.animation.wheel.get") {
      const entries = wheelPool(this.getState()).map(({ scene, stage, title, detail, key }) => ({ scene, stage, title, detail, key }));
      this.later({ type: "osf.animation.wheel.data", payload: { customized: this.getState().wheelCustomized, entries } });
    } else if (command === "osf.animation.launch") {
      this.launch(fields);
    } else if (command === "settings.get") {
      // Derived from PREFERENCE_KEYS so the mock host stays exhaustive by
      // construction — a new setting cannot be forgotten here.
      const preferences = this.getState().preferences;
      const values = Object.fromEntries(Object.entries(PREFERENCE_KEYS)
        .map(([field, key]) => [key, preferences[field as keyof BrowserPreferences]]));
      this.later({ type: "settings.data", payload: { mods: [{ id: "osf.animation", values }] } }, 10);
    } else if (command === "settings.set" && fields.mod === "osf.animation" && typeof fields.key === "string") {
      this.later({ type: "settings.changed", payload: { mod: fields.mod, key: fields.key, value: fields.value } }, 10);
    } else if (command === "osf.animation.stop") {
      this.active = this.active.filter((scene) => scene.handle !== Number(fields.handle));
      this.later({ type: "osf.animation.activeScenes", payload: { scenes: this.active } });
    } else if (command === "osf.animation.advance") {
      this.advance(Number(fields.handle));
    } else if (command === "osf.animation.wheel.set") {
      this.customized = !fields.reset;
      this.pins = fields.reset ? [] : Array.isArray(fields.entries)
        ? fields.entries.filter(isRecord).slice(0, WHEEL_MAX).map((entry) => ({ scene: String(entry.scene || ""), ...(Number.isInteger(entry.stage) ? { stage: entry.stage } : {}) }))
        : [];
      // Pins are a presentation layer over the catalog already served — re-reading
      // the snapshot (up to ~600KB) on every pin or reorder buys nothing.
      this.emitCatalog(this.catalogRaw ?? MOCK_CATALOG);
    } else if (command === "osf.animation.closed") {
      this.active = this.active.filter((scene) => !scene.player);
    } else if (command === "osf.animation.opened") {
      this.later({ type: "osf.animation.activeScenes", payload: { scenes: this.active } }, 50);
      const target = new URLSearchParams(location.search).get("target");
      if (target === "actor" || target === "furniture") this.later({ type: "osf.animation.openTarget", payload: { slot: target, ...(target === "actor" ? MOCK_ACTORS[0] : MOCK_ANCHORS[0]) } }, 60);
    } else if (command === "osf.animation.requestClose") {
      this.later({ type: "ui.visibility", payload: { visible: false } }, 60);
    }
  }

  openWheel(withTarget = true): void {
    this.emit({ type: "osf.animation.mode", payload: { mode: "wheel", tagPrefix: "player.emote.", target: withTarget ? { token: 601, name: "Sarah Morgan" } : null } });
  }

  private launch(fields: Record<string, unknown>): void {
    const sceneId = String(fields.sceneId || "");
    if (sceneId === "emote.facepalm") {
      this.later({ type: "osf.animation.launchResult", payload: { ok: false, error: "No room in front of the actor (mock error)." } }, 80);
      return;
    }
    const tokens = Array.isArray(fields.castTokens) ? fields.castTokens.map(Number) : [];
    this.active = this.active.filter((scene) => !scene.cast.some((member) => tokens.includes(member.token)));
    const handle = this.nextHandle++;
    const options = isRecord(fields.opts) ? fields.opts : {};
    this.active.push({
      handle,
      sceneId,
      stage: Number(options.stage) || 0,
      player: tokens.includes(PLAYER_TOKEN),
      cast: tokens.map((token) => ({ token, name: token === PLAYER_TOKEN ? "Player" : MOCK_ACTORS.find((actor) => actor.token === token)?.name ?? "actor", player: token === PLAYER_TOKEN })),
    });
      this.later({ type: "osf.animation.launchResult", payload: { ok: true, handle, sceneId } }, 80);
    this.later({ type: "osf.animation.activeScenes", payload: { scenes: this.active } }, 130);
  }

  private advance(handle: number): void {
    const scene = this.active.find((candidate) => candidate.handle === handle);
    if (scene) {
      scene.stage++;
      const definition = sceneById(this.getState(), scene.sceneId);
      if (definition?.stages.length && scene.stage >= definition.stages.length) this.active = this.active.filter((candidate) => candidate !== scene);
    }
    this.later({ type: "osf.animation.activeScenes", payload: { scenes: this.active } });
  }

  dispose(): void {
    for (const timer of this.timers) clearTimeout(timer);
    this.timers.clear();
    this.listeners.clear();
  }
}

