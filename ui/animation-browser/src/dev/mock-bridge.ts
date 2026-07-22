import type { AnimationBridge, NativeMessageListener } from "../bridge/client";
import { isRecord, type BridgeCommand, type NativeMessage } from "../bridge/contract";
import { WHEEL_MAX, sceneById, wheelPool } from "../app/selectors";
import { PLAYER_TOKEN, type ActiveScene, type BrowserState } from "../app/state";
import { MOCK_ACTORS, MOCK_ANCHORS, MOCK_ANCHOR_MATCH, MOCK_CATALOG, MOCK_LIBRARY } from "./mock-data";

async function fetchFixture(name: "catalog" | "library"): Promise<unknown[] | null> {
  try {
    const response = await fetch(`live/${name}.json`, { cache: "no-store" });
    const value: unknown = response.ok ? await response.json() : null;
    return Array.isArray(value) ? value : null;
  } catch {
    return null;
  }
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
      void fetchFixture("catalog").then((fixture) => this.emit({ type: "osf.animation.catalog.data", payload: this.applyPins(fixture ?? MOCK_CATALOG, false) }));
    } else if (command === "osf.animation.library.get") {
      void fetchFixture("library").then((fixture) => this.emit({ type: "osf.animation.library.data", payload: this.applyPins(fixture ?? MOCK_LIBRARY, true) }));
    } else if (command === "osf.animation.anchorMatch") {
      this.later({ type: "osf.animation.anchorMatch", payload: { token: fields.token, sceneIds: MOCK_ANCHOR_MATCH[Number(fields.token)] ?? [] } }, 70);
    } else if (command === "osf.animation.pickCrosshair") {
      const item = fields.slot === "furniture" ? MOCK_ANCHORS[0] : MOCK_ACTORS[0];
      this.later({ type: "osf.animation.pick", payload: { slot: fields.slot, valid: true, ...item } }, 60);
    } else if (command === "osf.animation.scanNearby") {
      this.later({ type: "osf.animation.scanResults", payload: { kind: fields.kind, items: fields.kind === "furniture" ? MOCK_ANCHORS : MOCK_ACTORS } }, 80);
    } else if (command === "osf.animation.wheel.get") {
      const entries = wheelPool(this.getState()).map(({ scene, stage, title, detail, key }) => ({ scene, stage, title, detail, key }));
      this.later({ type: "osf.animation.wheel.data", payload: { customized: this.getState().wheelCustomized, entries } });
    } else if (command === "osf.animation.launch") {
      this.launch(fields);
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
      void fetchFixture("catalog").then((fixture) => this.emit({ type: "osf.animation.catalog.data", payload: this.applyPins(fixture ?? MOCK_CATALOG, false) }));
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

