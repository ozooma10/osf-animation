import type { BrowserCommands } from "../../app/commands";
import {
  activeScenes,
  anchorShort,
  comparePlayableGroupKeys,
  comparePlayableItems,
  evaluateForState,
  formatDuration,
  formatEstimate,
  hiddenSceneCount,
  isWheelEmote,
  isWheelStage,
  needsText,
  packKey,
  packLabel,
  playableGroupOpen,
  playableItems,
  playableSceneTitle,
  playableVisible,
  sceneById,
  showUnavailable,
  speciesLabel,
  stageLabel,
  wheelPool,
  type PlayableItem,
} from "../../app/selectors";
import type { BrowseKind, BrowserState } from "../../app/state";
import type { SceneEvaluation, SceneModel } from "../../model";
import { Dot, Empty, SpeciesFilter } from "../shared/Shared";
import { TIMELINE_FPS, timelineFrame } from "./timeline";

interface EvaluatedPlayable {
  item: PlayableItem;
  evaluation: SceneEvaluation;
}

function playableGroupKey(item: PlayableItem): string {
  if (item.kind === "action" && !item.scene.pack && !item.scene.sourceFile) {
    return `action-pack:${item.scene.id.split("/").filter(Boolean)[0] || "actions"}`;
  }
  return packKey(item.scene);
}

function playableGroupLabel(key: string, scenes: readonly SceneModel[]): string {
  if (key.startsWith("action-pack:")) {
    return key.slice("action-pack:".length).replace(/[-_]+/g, " ").toUpperCase();
  }
  const label = packLabel(key, scenes);
  return scenes[0]?.library && /^vanilla-/i.test(key) ? `VANILLA / ${label}` : label;
}

function kindLabel(item: PlayableItem): string {
  if (item.kind === "action") return item.scene.openEnded ? "ACTION · HOLDS" : "ACTION · ENDS";
  if (item.kind === "scene") return "SCENE";
  if (item.stage?.tags.includes("pose")) return "POSE · HOLDS";
  return item.stage?.openEnded ? "LOOP · HOLDS" : "ANIMATION";
}

function itemDuration(item: PlayableItem): string {
  if (!item.stage) return formatEstimate(item.scene);
  return formatDuration(item.stage.loopSec ?? item.stage.estSec);
}

function wheelEligible(item: PlayableItem): boolean {
  return item.stage
    ? isWheelStage(item.scene, item.stage)
    : item.kind === "action" && isWheelEmote(item.scene);
}

function PlayableRow({ state, entry, wheelKeys, commands }: {
  state: BrowserState;
  entry: EvaluatedPlayable;
  wheelKeys: ReadonlySet<string>;
  commands: BrowserCommands;
}) {
  const { item, evaluation } = entry;
  const scene = item.scene;
  const ready = evaluation.gaps === 0;
  const selected = state.selectedId === scene.id && state.selectedStage === (item.stage?.index ?? null);
  const onWheel = wheelKeys.has(item.key);
  const details = [kindLabel(item)];
  if (scene.actorCount > 1) details.push(`${scene.actorCount} actors`);
  if (scene.requiresFurniture) details.push(anchorShort(scene) || "furniture");
  const duration = itemDuration(item);
  if (duration) details.push(duration);

  return <div class={`playable-row ${selected ? "selected" : ""} ${ready ? "" : "dim"}`}>
    <button class={`playable-main libx-row ${selected ? "selected" : ""}`} onClick={() => commands.selectScene(scene.id, item.stage?.index ?? null)}>
      <span class="libx-spine"/>
      <span class="playable-copy">
        <span class="libx-title">{item.title}</span>
        <span class="playable-path mono">{state.filters.debugMode
          ? `${scene.id}${item.stage ? ` · stage ${item.stage.index}` : ""}`
          : item.collection || playableGroupLabel(playableGroupKey(item), [scene])}</span>
      </span>
      <span class="playable-traits mono">{details.join(" · ")}</span>
      {!ready && <span class="row-badge">{needsText(state, scene, evaluation)}</span>}
    </button>
    {wheelEligible(item) && <button class={`playable-wheel ${onWheel ? "on" : ""}`}
      title={onWheel ? "Remove from Quick Access" : "Add to Quick Access"}
      onClick={() => commands.toggleWheelEntry(scene.id, item.stage?.index ?? null)}>
      {onWheel ? "◆" : "◇"}
    </button>}
    <button class={`playable-play ${ready ? "go" : ""}`} disabled={!ready}
      title={ready ? `Play ${item.title}` : evaluation.reason}
      onClick={() => commands.launch(item.stage?.index, item.kind === "animation", scene.id)}>▶</button>
  </div>;
}

function PlayableGroups({ state, entries, wheelKeys, commands, muted = false }: {
  state: BrowserState;
  entries: EvaluatedPlayable[];
  wheelKeys: ReadonlySet<string>;
  commands: BrowserCommands;
  muted?: boolean;
}) {
  const groups = new Map<string, EvaluatedPlayable[]>();
  for (const entry of entries) {
    const key = playableGroupKey(entry.item);
    groups.set(key, [...(groups.get(key) ?? []), entry]);
  }
  return <div class={`playable-groups ${muted ? "dim" : ""}`}>
    {[...groups.entries()].sort(([a], [b]) => comparePlayableGroupKeys(a, b)).map(([key, list]) => {
      const stateKey = `browse:${muted ? "rest:" : ""}${key}`;
      const containsSelection = list.some(({ item }) => item.scene.id === state.selectedId
        && (item.stage?.index ?? null) === state.selectedStage);
      const open = playableGroupOpen(state, stateKey, containsSelection);
      const scenes = list.map(({ item }) => item.scene);
      return <section class="libx-group" key={stateKey}>
        <button class="libx-head" onClick={() => commands.toggleLibraryGroup(stateKey, !open)}>
          <span class="chev">{open ? "▾" : "▸"}</span>
          <span class="libx-name">{playableGroupLabel(key, scenes)}</span>
          <span class="libx-meta mono">{list.length} playable{list.length === 1 ? "" : "s"}</span>
        </button>
        {open && <div class="libx-list">{list.map((entry) =>
          <PlayableRow key={entry.item.key} state={state} entry={entry} wheelKeys={wheelKeys} commands={commands}/>)}</div>}
      </section>;
    })}
  </div>;
}

function BrowseFilters({ state, commands, count, hiddenCount }: {
  state: BrowserState;
  commands: BrowserCommands;
  count: number;
  hiddenCount: number;
}) {
  const kinds: { value: BrowseKind; label: string }[] = [
    { value: "all", label: "ALL" },
    { value: "animation", label: "ANIMATIONS" },
    { value: "action", label: "ACTIONS" },
    { value: "scene", label: "SCENES" },
  ];
  return <>
    <div class="browse-filterbar">
      <div class="kind-switch" aria-label="Playable type">
        {kinds.map(({ value, label }) => <button class={state.browseKind === value ? "on" : ""}
          aria-pressed={state.browseKind === value} onClick={() => commands.setBrowseKind(value)} key={value}>{label}</button>)}
      </div>
      <button class={`filter-chip ${state.libCustomOnly ? "on" : ""}`} onClick={commands.toggleLibraryCustomOnly}>
        {state.libCustomOnly ? "CUSTOM ONLY" : "CUSTOM + VANILLA"}
      </button>
      {!state.filters.search && <button class={`filter-chip ${state.libFull ? "on" : ""}`} onClick={commands.toggleLibraryFull}>
        {state.libFull ? "FULL DETAIL" : "POSES & LOOPS"}
      </button>}
      {(state.showHidden || hiddenCount > 0) && <button class={`filter-chip ${state.showHidden ? "on" : ""}`}
        title={`${state.showHidden ? "Hide" : "Show"} ${hiddenCount} hidden scene${hiddenCount === 1 ? "" : "s"}`}
        aria-pressed={state.showHidden} onClick={commands.toggleHidden}>
        {state.showHidden ? "HIDE HIDDEN" : "SHOW HIDDEN"} · {hiddenCount}
      </button>}
    </div>
    <SpeciesFilter state={state} onToggle={commands.toggleSpecies}/>
    <div class="browse-note"><Dot active/><span class="lbl">PLAYABLE NOW · {count}</span></div>
  </>;
}

function UnifiedBrowser({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  if (!state.catalogReceived && !state.libraryReceived) return <Empty>Waiting for the playable catalog…</Empty>;
  const entries = playableItems(state).filter((item) => playableVisible(state, item))
    .map((item) => ({ item, evaluation: evaluateForState(state, item.scene) }));
  const rank = (a: EvaluatedPlayable, b: EvaluatedPlayable) => {
    return comparePlayableItems(state, a.item, b.item);
  };
  const ready = entries.filter((entry) => entry.evaluation.gaps === 0).sort(rank);
  const rest = entries.filter((entry) => entry.evaluation.gaps > 0).sort(rank);
  const wheelKeys = new Set(wheelPool(state).map((candidate) => candidate.key));
  const unavailable = state.preferences.unavailableScenes;
  const showRest = showUnavailable(state);
  const hiddenCount = hiddenSceneCount(state);

  return <>
    <BrowseFilters state={state} commands={commands} count={ready.length} hiddenCount={hiddenCount}/>
    {ready.length ? <PlayableGroups state={state} entries={ready} wheelKeys={wheelKeys} commands={commands}/>
      : <Empty>No installed playable fits the current cast, furniture, and filters.</Empty>}
    {unavailable === "ask" && !!rest.length && <button class={`reveal ${state.browseAll ? "on" : ""}`}
      onClick={commands.toggleBrowseAll}>{state.browseAll ? "▾" : "▸"} {rest.length} more need a different cast or furniture</button>}
    {showRest && unavailable === "show" && !!rest.length &&
      <div class="browse-note dim"><Dot/><span class="lbl">NEEDS DIFFERENT CAST OR FURNITURE · {rest.length}</span></div>}
    {showRest && <PlayableGroups state={state} entries={rest} wheelKeys={wheelKeys} muted commands={commands}/>}
  </>;
}

function ActiveBrowser({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const scenes = activeScenes(state);
  if (!scenes.length) return <Empty>No playables running.</Empty>;
  return <><div class="browse-note"><Dot active/><span class="lbl">RUNNING · {scenes.length}</span>{scenes.length > 1 && <button class="reveal inline stop-all" onClick={commands.stopAll}>■ STOP ALL</button>}</div>
    <div class="active-list">{scenes.map((active) => {
      const scene = sceneById(state, active.sceneId);
      const stages = scene?.stages ?? [];
      const frame = 1 / TIMELINE_FPS;
      const time = Math.min(active.time, active.duration);
      const clock = (value: number) => `${Math.floor(value / 60)}:${(value % 60).toFixed(2).padStart(5, "0")}`;
      return <div class={`active-card ${state.selectedId === active.sceneId ? "selected" : ""}`} key={active.handle}>
        <button class="active-main" onClick={() => commands.selectScene(active.sceneId)}><div class="active-headline"><span class="live-dot"/><span class="active-title">{scene ? playableSceneTitle(scene) : active.sceneId}</span><span class="active-handle mono">#{active.handle}{scene && formatEstimate(scene) ? ` · ${formatEstimate(scene)}` : ""}</span></div>
          {stages.length > 1 && active.stage >= 0 && active.stage < stages.length && <div class="active-stage mono">STAGE {active.stage + 1}/{stages.length} · {stageLabel(scene!, active.stage).toUpperCase()}</div>}
          {!!active.cast.length && <div class="active-cast">{active.cast.map((member) => <span class={`active-actor ${member.player ? "player" : ""}`} key={member.token}>{member.name}{member.player ? " · YOU" : ""}</span>)}</div>}
        </button>
        {stages.length > 1 && <button class="next-mini" onClick={() => commands.advance(active.handle)}>NEXT ▸</button>}
        <button class="stop-mini" onClick={() => commands.stop(active.handle)}>■ STOP</button>
        <div class="active-timeline">
          <button class="frame-step" title="Previous frame (30 fps)" disabled={!active.duration} onClick={() => commands.setPlayback(active.handle, Math.max(0, time - frame), true)}>◀|</button>
          <button class={`play-toggle ${active.speed > 0 ? "" : "paused"}`} title={active.speed > 0 ? "Pause" : "Resume"} onClick={() => commands.setPlayback(active.handle, undefined, active.speed > 0)}>{active.speed > 0 ? "Ⅱ" : "▶"}</button>
          <button class="frame-step" title="Next frame (30 fps)" disabled={!active.duration} onClick={() => commands.setPlayback(active.handle, Math.min(active.duration, time + frame), true)}>|▶</button>
          <input class="timeline-range" type="range" min="0" max={active.duration || 1} step={frame} value={time} disabled={!active.duration}
            aria-label={`Scene time ${clock(time)}, frame ${timelineFrame(time)}, of ${clock(active.duration)}, frame ${timelineFrame(active.duration)}`}
            onInput={(event) => commands.setPlayback(active.handle, Number(event.currentTarget.value), true)}/>
          <span class="timeline-clock mono">
            <span>{clock(time)} / {clock(active.duration)}</span>
            <span class="timeline-frames">FRAME {timelineFrame(time)} / {timelineFrame(active.duration)}</span>
          </span>
        </div>
      </div>;
    })}</div></>;
}

export function BrowsePanel({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const live = activeScenes(state);
  return <>
    <div class="browse-head">
      <div class="mode-switch">
        <button class={`mode-btn ${state.mode !== "active" ? "on" : ""}`} onClick={() => commands.setMode("scenes")}>BROWSE</button>
        {!!live.length && <button class={`mode-btn live ${state.mode === "active" ? "on" : ""}`} onClick={() => commands.setMode("active")}><span class="live-dot"/>ACTIVE · {live.length}</button>}
      </div>
      <div class="search-field grow"><input type="text" value={state.filters.search} onInput={(event) => commands.setSearch(event.currentTarget.value)} placeholder="⌕ search animations · actions · scenes · tags" autocomplete="off" spellcheck={false}/></div>
    </div>
    <div class="browse-body">{state.mode === "active" ? <ActiveBrowser state={state} commands={commands}/> : <UnifiedBrowser state={state} commands={commands}/>}</div>
  </>;
}
