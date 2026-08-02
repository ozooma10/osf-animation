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
import type { SceneEvaluation, SceneModel, SceneStage, TimelineTrackMark } from "../../model";
import { Dot, Empty, SpeciesFilter } from "../shared/Shared";
import { TIMELINE_FPS, timelineFrame, timelineMarkMoment, timelineTracks } from "./timeline";

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
        <button class="active-main" onClick={() => commands.selectScene(active.sceneId)}><div class="active-headline"><span class="live-dot"/><span class="active-title">{scene ? playableSceneTitle(scene) : active.sceneId}</span>{active.inspection && <span class="inspection-badge mono">PROP PREVIEW</span>}<span class="active-handle mono">#{active.handle}{scene && formatEstimate(scene) ? ` · ${formatEstimate(scene)}` : ""}</span></div>
          {stages.length > 1 && active.stage >= 0 && active.stage < stages.length && <div class="active-stage mono">STAGE {active.stage + 1}/{stages.length} · {stageLabel(scene!, active.stage).toUpperCase()}</div>}
          {!!active.cast.length && <div class="active-cast">{active.cast.map((member) => <span class={`active-actor ${member.player ? "player" : ""}`} key={member.token}>{member.name}{member.player ? " · YOU" : ""}</span>)}</div>}
        </button>
        {stages.length > 1 && <button class="next-mini" title={active.inspection ? "Inspect the next animation in this scene (Space)" : "Advance to the next stage (Space)"}
          onClick={() => commands.advance(active.handle)}>NEXT ▸</button>}
        <button class="stop-mini" onClick={() => commands.stop(active.handle)}>■ STOP</button>
        {active.inspection && scene && <InspectionStages scene={scene} stage={active.stage}
          onPick={(index) => commands.inspectStage(active.sceneId, index)}/>}
        <div class="active-timeline">
          {active.inspection && <button class="frame-step" title="Previous frame (30 fps)" disabled={!active.duration} onClick={() => commands.setPlayback(active.handle, Math.max(0, time - frame), true)}>◀|</button>}
          {!active.inspection && <button class={`play-toggle ${active.speed > 0 ? "" : "paused"}`} title={active.speed > 0 ? "Pause" : "Resume runtime playback"} onClick={() => commands.setPlayback(active.handle, undefined, active.speed > 0)}>{active.speed > 0 ? "Ⅱ" : "▶"}</button>}
          {active.inspection && <button class="frame-step" title="Next frame (30 fps)" disabled={!active.duration} onClick={() => commands.setPlayback(active.handle, Math.min(active.duration, time + frame), true)}>|▶</button>}
          <input class="timeline-range" type="range" min="0" max={active.duration || 1} step={frame} value={time} disabled={!active.inspection || !active.duration}
            aria-label={`Scene time ${clock(time)}, frame ${timelineFrame(time)}, of ${clock(active.duration)}, frame ${timelineFrame(active.duration)}`}
            title={active.inspection ? "Scrub side-effect-free pose and OSF scene props" : "Runtime playback is forward-only; use Inspect to scrub"}
            onInput={(event) => active.inspection && commands.setPlayback(active.handle, Number(event.currentTarget.value), true)}/>
          <span class="timeline-clock mono">
            <span>{clock(time)} / {clock(active.duration)}</span>
            <span class="timeline-frames">FRAME {timelineFrame(time)} / {timelineFrame(active.duration)}</span>
          </span>
          {active.inspection && <InspectionTracks stage={stages.find((stage) => stage.index === active.stage)}
            time={time} duration={active.duration}
            onSeek={(mark) => commands.setPlayback(active.handle, mark.at * active.duration, true)}/>}
        </div>
      </div>;
    })}</div></>;
}

// Chips shown around the inspected stage. A window instead of the whole list: a
// generated pack scene can carry hundreds of stages, and the current one always sits
// inside it, so the strip stays readable without scroll bookkeeping.
const STAGE_WINDOW = 5;

function InspectionStages({ scene, stage, onPick }: {
  scene: SceneModel;
  stage: number;
  onPick: (stageIndex: number) => void;
}) {
  const stages = scene.stages;
  if (stages.length < 2) return null;
  const at = Math.max(0, stages.findIndex((candidate) => candidate.index === stage));
  const start = Math.min(Math.max(0, at - Math.floor(STAGE_WINDOW / 2)), Math.max(0, stages.length - STAGE_WINDOW));
  return <div class="inspect-stages" aria-label="Inspected animation">
    <span class="lbl">INSPECT</span>
    <button class="stage-step" disabled={at <= 0} title="Inspect the previous animation" onClick={() => onPick(stages[at - 1].index)}>◀</button>
    <div class="stage-strip">{stages.slice(start, start + STAGE_WINDOW).map((candidate, offset) => {
      const label = stageLabel(scene, candidate.index);
      const current = candidate.index === stage;
      return <button class={`stage-chip ${current ? "on" : ""}`} key={candidate.index} aria-pressed={current}
        title={current ? `${label} — inspecting` : `Inspect ${label}`}
        onClick={() => { if (!current) onPick(candidate.index); }}>
        <span class="stage-chip-n mono">{start + offset + 1}</span><span class="stage-chip-name">{label}</span>
      </button>;
    })}</div>
    <button class="stage-step" disabled={at >= stages.length - 1} title="Inspect the next animation" onClick={() => onPick(stages[at + 1].index)}>▶</button>
  </div>;
}

function InspectionTracks({ stage, time, duration, onSeek }: {
  stage?: SceneStage;
  time: number;
  duration: number;
  onSeek: (mark: TimelineTrackMark) => void;
}) {
  const lanes = timelineTracks(stage?.tracks ?? []);
  const count = lanes.reduce((sum, lane) => sum + lane.marks.length, 0);
  const playhead = duration > 0 ? Math.max(0, Math.min(100, time / duration * 100)) : 0;
  return <section class="timeline-tracks" aria-label="Authored scene tracks">
    <div class="timeline-tracks-head"><span>AUTHORED TRACKS</span><span>{count || "NONE"}</span></div>
    {!lanes.length && <div class="timeline-tracks-empty">This animation has no authored cues, actions, sounds, or camera marks.</div>}
    {lanes.map((lane) => <div class="timeline-lane" data-kind={lane.kind} key={lane.kind}>
      <div class="timeline-lane-head mono"><span>{lane.label}</span><span>{lane.marks.length}</span></div>
      <div class="timeline-lane-rail">
        <span class="timeline-playhead" style={{ left: `${playhead}%` }}/>
        {lane.marks.map((mark, index) => {
          const moment = timelineMarkMoment(mark, duration);
          const detail = [mark.label, mark.detail, mark.role && `role ${mark.role}`, mark.repeat && "every loop", moment].filter(Boolean).join(" · ");
          return <button class="timeline-marker" style={{ left: `${mark.at * 100}%` }} title={detail}
            aria-label={detail} disabled={!duration} onClick={() => onSeek(mark)} key={`${mark.anchor}:${mark.at}:${index}`}><span/></button>;
        })}
      </div>
      <div class="timeline-events">
        {lane.marks.map((mark, index) => <button disabled={!duration} onClick={() => onSeek(mark)}
          title={[mark.detail, mark.role && `role ${mark.role}`, mark.repeat && "repeats every loop"].filter(Boolean).join(" · ")}
          key={`${mark.label}:${index}`}><strong>{mark.label}</strong><span class="mono">{timelineMarkMoment(mark, duration)}{mark.repeat ? " · LOOP" : ""}{mark.role ? ` · ${mark.role}` : ""}</span></button>)}
      </div>
    </div>)}
  </section>;
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
