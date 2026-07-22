import type { BrowserCommands } from "../../app/commands";
import {
  activeScenes,
  anchorShort,
  animationList,
  browseVisible,
  castHasCreature,
  castSpecies,
  cleanStages,
  emoteCatalog,
  evaluateForState,
  fitsKeyedAnchor,
  filteredLibrary,
  formatEstimate,
  isEmote,
  libraryRank,
  matchesSearch,
  needsText,
  packKey,
  packLabel,
  sceneById,
  sceneCatalog,
  setQuality,
  speciesLabel,
  speciesVisible,
  stageLabel,
} from "../../app/selectors";
import type { BrowserState } from "../../app/state";
import type { SceneEvaluation, SceneModel } from "../../model";
import { Dot, Empty, SpeciesFilter } from "../shared/Shared";

interface EvaluatedScene { scene: SceneModel; evaluation: SceneEvaluation }

function SceneRow({ state, item, playable, commands }: {
  state: BrowserState;
  item: EvaluatedScene;
  playable: boolean;
  commands: BrowserCommands;
}) {
  const { scene, evaluation } = item;
  const details = [`${scene.actorCount || 1} role${scene.actorCount === 1 ? "" : "s"}`];
  if (scene.requiresFurniture) details.push(`on ${anchorShort(scene) || "furniture"}`);
  const duration = formatEstimate(scene);
  if (duration) details.push(duration);
  return (
    <button class={`libx-row ${state.selectedId === scene.id ? "selected" : ""}`} onClick={() => commands.selectScene(scene.id)}>
      <span class="libx-spine"/><span class="libx-title">{scene.title}</span>
      {scene.pinned > 0 && <span class="libx-pinmark" title="On the animation wheel">◆</span>}
      <span class="libx-meta mono">{state.filters.debugMode ? scene.id : details.join(" · ")}</span>
      <span class={`row-badge ${playable ? "go" : ""}`}>{playable ? "READY" : needsText(state, scene, evaluation)}</span>
    </button>
  );
}

function SceneGroups({ state, items, playable, commands }: {
  state: BrowserState;
  items: EvaluatedScene[];
  playable: boolean;
  commands: BrowserCommands;
}) {
  const groups = new Map<string, EvaluatedScene[]>();
  for (const item of items) {
    const key = packKey(item.scene);
    groups.set(key, [...(groups.get(key) ?? []), item]);
  }
  const entries = [...groups.entries()];
  const flat = entries.length === 1 || entries.length > 8 && items.length / entries.length < 1.5;
  if (flat) return <div class={`row-list ${playable ? "" : "dim"}`}>{items.map((item) => <SceneRow key={item.scene.id} state={state} item={item} playable={playable} commands={commands}/>)}</div>;
  const searching = !!state.filters.search;
  const fewRows = items.length <= 14;
  return <div class={`row-list ${playable ? "" : "dim"}`}>{entries.map(([key, list]) => {
    const stateKey = `${playable ? "" : "rest:"}${key}`;
    const containsSelection = list.length <= 30 && list.some((item) => item.scene.id === state.selectedId);
    const open = state.scnOpen.get(stateKey) ?? (searching || fewRows || containsSelection);
    return <div class="libx-group" key={stateKey}>
      <button class="libx-head" onClick={() => commands.toggleSceneGroup(stateKey, !open)}>
        <span class="chev">{open ? "▾" : "▸"}</span><span class="libx-name">{packLabel(key, list.map((item) => item.scene))}</span>
        <span class="libx-meta mono">{list.length} SCENE{list.length === 1 ? "" : "S"}</span>
      </button>
      {open && <div class="libx-list">{list.map((item) => <SceneRow key={item.scene.id} state={state} item={item} playable={playable} commands={commands}/>)}</div>}
    </div>;
  })}</div>;
}

function ScenesBrowser({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  if (!state.catalogReceived) return <Empty>Waiting for the catalog…</Empty>;
  const scenes = sceneCatalog(state);
  if (!scenes.length) return <Empty>No authored scenes installed — emotes and the animation library are ready to play.<br/><button class="chip-btn" onClick={() => commands.setMode("library")}>OPEN ANIMATIONS ▸</button></Empty>;
  const evaluated = scenes.filter((scene) => matchesSearch(state, scene) && speciesVisible(state, scene))
    .map((scene) => ({ scene, evaluation: evaluateForState(state, scene) }));
  const anchorFits = (item: EvaluatedScene) => Number(!!state.furniture && item.scene.requiresFurniture && item.evaluation.anchorGate);
  const rank = (a: EvaluatedScene, b: EvaluatedScene) => anchorFits(b) - anchorFits(a)
    || b.scene.priority - a.scene.priority || b.scene.weight - a.scene.weight || a.scene.title.localeCompare(b.scene.title);
  const playable = evaluated.filter((item) => item.evaluation.gaps === 0).sort(rank);
  const rest = evaluated.filter((item) => item.evaluation.gaps > 0).sort(rank);
  return <>
    <SpeciesFilter state={state} onToggle={commands.toggleSpecies}/>
    <div class="browse-note"><Dot active/><span class="lbl">PLAYABLE NOW · {playable.length}</span></div>
    {playable.length ? <SceneGroups state={state} items={playable} playable commands={commands}/>
      : <div class="bay-empty"><span class="mono">{state.furniture || state.cast.length > 1 ? "No scene pack fits this exact crew + furniture." : "No solo scenes in your installed packs."}</span><button class="chip-btn" onClick={() => commands.setMode("library")}>OPEN ANIMATIONS ▸</button></div>}
    {!!rest.length && <button class={`reveal ${state.browseAll ? "on" : ""}`} onClick={commands.toggleBrowseAll}>{state.browseAll ? "▾" : "▸"} {rest.length} more need a different crew or furniture</button>}
    {state.browseAll && <SceneGroups state={state} items={rest} playable={false} commands={commands}/>} 
  </>;
}

function LibraryRow({ state, scene, cleanTier, commands }: { state: BrowserState; scene: SceneModel; cleanTier: boolean; commands: BrowserCommands }) {
  const stages = cleanTier ? cleanStages(scene) : scene.stages;
  const fits = fitsKeyedAnchor(state, scene);
  return <button class={`libx-row ${state.selectedId === scene.id ? "selected" : ""} ${fits === false ? "dim" : ""}`} onClick={() => commands.selectScene(scene.id)}>
    <span class="libx-spine"/><span class="libx-title">{scene.title.replace(/^Vanilla · /, "")}</span>
    {scene.stages.some((stage) => stage.pinned > 0) && <span class="libx-pinmark" title="Contains an animation on the wheel">◆</span>}
    {scene.requiresFurniture && <span class={`libx-anchor ${fits === true ? "fit" : fits === false ? "nofit" : ""}`} title={scene.anchors.length ? `Needs: ${scene.anchors.join(" / ")}` : "Needs matching furniture"}>FURN</span>}
    <span class="libx-meta mono">{state.filters.debugMode ? scene.id : `${stages.length} anim${stages.length === 1 ? "" : "s"}`}</span>
  </button>;
}

function VanillaSourceToggle({ filtered, onToggle }: { filtered: boolean; onToggle(): void }) {
  return <button class={`source-toggle ${filtered ? "filtered" : ""}`} onClick={onToggle} title={filtered ? "Show vanilla animations" : "Hide vanilla animations"} aria-label={filtered ? "Vanilla animations hidden" : "Vanilla animations shown"} aria-pressed={filtered}>
    <span>VANILLA</span><i class="source-toggle-switch" aria-hidden="true"><i/></i>
  </button>;
}

function LibraryBrowser({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const emotes = emoteCatalog(state).filter((scene) => matchesSearch(state, scene) && speciesVisible(state, scene));
  const matchKnown = !!state.furniture && state.anchorMatch?.token === state.furniture.token;
  const fitFocus = matchKnown && !state.libShowAll;
  const cleanTier = !matchKnown && !state.libFull && !state.filters.search;
  const grouped = new Map<string, SceneModel[]>();
  const library = filteredLibrary(state);
  for (const scene of library) {
    if (!matchesSearch(state, scene) || !speciesVisible(state, scene)) continue;
    if (fitFocus && !state.anchorMatch?.ids.has(scene.id)) continue;
    if (cleanTier && !cleanStages(scene).length) continue;
    const key = packKey(scene);
    grouped.set(key, [...(grouped.get(key) ?? []), scene]);
  }
  const quality = cleanTier ? setQuality : () => 0;
  for (const list of grouped.values()) list.sort((a, b) => libraryRank(state, a) - libraryRank(state, b) || quality(a) - quality(b) || a.title.localeCompare(b.title));
  const groups = [...grouped.entries()].sort((a, b) => {
    const rank = (list: SceneModel[]) => list.reduce((sum, scene) => sum + libraryRank(state, scene), 0) / list.length;
    return rank(a[1]) - rank(b[1]) || Math.min(...a[1].map(quality)) - Math.min(...b[1].map(quality)) || a[0].localeCompare(b[0]);
  });
  const speciesLibrary = library.filter((scene) => speciesVisible(state, scene));
  const clips = speciesLibrary.reduce((count, scene) => count + scene.stages.length, 0);
  const cleanClips = speciesLibrary.reduce((count, scene) => count + cleanStages(scene).length, 0);
  return <>
    <SpeciesFilter state={state} onToggle={commands.toggleSpecies}/>
    {matchKnown ? <div class="browse-note"><Dot active/><span class="lbl">{state.furniture!.name} · {speciesLibrary.filter((scene) => state.anchorMatch!.ids.has(scene.id)).length} SETS FIT</span><button class={`reveal inline ${state.libShowAll ? "on" : ""}`} onClick={commands.toggleLibraryShowAll}>{state.libShowAll ? "show fitting only" : "show all"}</button><VanillaSourceToggle filtered={state.libCustomOnly} onToggle={commands.toggleLibraryCustomOnly}/></div>
      : <div class="browse-note"><Dot/><span class="lbl">{cleanTier ? `ANIMATION LIBRARY · ${cleanClips} POSES & LOOPS` : `ANIMATION LIBRARY · ${clips} CLIPS IN ${speciesLibrary.length} SETS`}</span>{!state.filters.search && <button class={`reveal inline ${state.libFull ? "on" : ""}`} onClick={commands.toggleLibraryFull}>{state.libFull ? "poses & loops only" : `full library · ${clips} clips`}</button>}<VanillaSourceToggle filtered={state.libCustomOnly} onToggle={commands.toggleLibraryCustomOnly}/></div>}
    {!!emotes.length && <div class="libx-group emotes"><div class="libx-head static"><span class="emote-mark">✦</span><span class="libx-name">EMOTES</span><span class="libx-meta mono">{emotes.length} QUICK ACTION{emotes.length === 1 ? "" : "S"}</span></div><div class="libx-list">
      {emotes.map((scene) => <button key={scene.id} class={`libx-row emote ${state.selectedId === scene.id ? "selected" : ""}`} onClick={() => commands.selectScene(scene.id)}><span class="libx-spine"/><span class="libx-title">{scene.title}</span>{scene.pinned > 0 && <span class="libx-pinmark">◆</span>}<span class="libx-meta mono">{state.filters.debugMode ? scene.id : ["emote", formatEstimate(scene)].filter(Boolean).join(" · ")}</span></button>)}
    </div></div>}
    {!state.libraryReceived ? <Empty>Loading the animation library…</Empty> : groups.length ? groups.map(([key, list]) => {
      const open = !!state.filters.search || matchKnown || state.libOpen.has(key);
      const count = list.reduce((sum, scene) => sum + (cleanTier ? cleanStages(scene) : scene.stages).length, 0);
      return <div class="libx-group" key={key}><button class="libx-head" onClick={() => commands.toggleLibraryGroup(key)}><span class="chev">{open ? "▾" : "▸"}</span><span class="libx-name">{packLabel(key, list)}</span><span class="libx-meta mono">{list.length} set{list.length === 1 ? "" : "s"} · {count} anim{count === 1 ? "" : "s"}</span></button>{open && <div class="libx-list">{list.map((scene) => <LibraryRow key={scene.id} state={state} scene={scene} cleanTier={cleanTier} commands={commands}/>)}</div>}</div>;
    }) : !emotes.length && <Empty>{castHasCreature(state) && !state.allSpecies ? `No ${[...castSpecies(state)].map(speciesLabel).join(" / ")} animations in the library.` : "Nothing in the library matches the filter."}</Empty>}
  </>;
}

function ActiveBrowser({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const scenes = activeScenes(state);
  if (!scenes.length) return <Empty>No scenes running.</Empty>;
  return <><div class="browse-note"><Dot active/><span class="lbl">RUNNING · {scenes.length} SCENE{scenes.length === 1 ? "" : "S"}</span>{scenes.length > 1 && <button class="reveal inline stop-all" onClick={commands.stopAll}>■ STOP ALL</button>}</div>
    <div class="active-list">{scenes.map((active) => {
      const scene = sceneById(state, active.sceneId);
      const stages = scene?.stages ?? [];
      return <div class={`active-card ${state.selectedId === active.sceneId ? "selected" : ""}`} key={active.handle}>
        <button class="active-main" onClick={() => commands.selectScene(active.sceneId)}><div class="active-headline"><span class="live-dot"/><span class="active-title">{scene?.title ?? active.sceneId}</span><span class="active-handle mono">#{active.handle}{scene && formatEstimate(scene) ? ` · ${formatEstimate(scene)}` : ""}</span></div>
          {stages.length > 1 && active.stage >= 0 && active.stage < stages.length && <div class="active-stage mono">STAGE {active.stage + 1}/{stages.length} · {stageLabel(scene!, active.stage).toUpperCase()}</div>}
          {!!active.cast.length && <div class="active-cast">{active.cast.map((member) => <span class={`active-actor ${member.player ? "player" : ""}`} key={member.token}>{member.name}{member.player ? " · YOU" : ""}</span>)}</div>}
        </button>
        {stages.length > 1 && <button class="next-mini" onClick={() => commands.advance(active.handle)}>NEXT ▸</button>}
        <button class="stop-mini" onClick={() => commands.stop(active.handle)}>■ STOP</button>
      </div>;
    })}</div></>;
}

export function BrowsePanel({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const live = activeScenes(state);
  const scenes = sceneCatalog(state);
  const emotes = emoteCatalog(state);
  const animationCount = state.libraryReceived ? emotes.length + filteredLibrary(state).reduce((sum, scene) => sum + scene.stages.length, 0) : `${emotes.length}+`;
  return <>
    <div class="browse-head">
      <div class="mode-switch">
        <button class={`mode-btn ${state.mode === "scenes" ? "on" : ""}`} onClick={() => commands.setMode("scenes")}>SCENES · {scenes.length}</button>
        <button class={`mode-btn ${state.mode === "library" ? "on" : ""}`} onClick={() => commands.setMode("library")}>ANIMATIONS · {animationCount}</button>
        {!!live.length && <button class={`mode-btn live ${state.mode === "active" ? "on" : ""}`} onClick={() => commands.setMode("active")}><span class="live-dot"/>ACTIVE · {live.length}</button>}
      </div>
      <div class="search-field grow"><input type="text" value={state.filters.search} onInput={(event) => commands.setSearch(event.currentTarget.value)} placeholder="⌕ search scenes · animations · tags" autocomplete="off" spellcheck={false}/></div>
    </div>
    <div class="browse-body">{state.mode === "active" ? <ActiveBrowser state={state} commands={commands}/> : state.mode === "library" ? <LibraryBrowser state={state} commands={commands}/> : <ScenesBrowser state={state} commands={commands}/>}</div>
  </>;
}
