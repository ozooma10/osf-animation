import type { BrowserCommands } from "./app/commands";
import { activeScenes, sceneById, sceneTitle, selectedPlayable, stageLabel } from "./app/selectors";
import type { BrowserState } from "./app/state";
import { LiveBar } from "./components/LiveBar";
import { AnchorPanel } from "./features/anchor/AnchorPanel";
import { SceneBrief } from "./features/brief/SceneBrief";
import { BrowsePanel } from "./features/browse/BrowsePanel";
import { CastPanel } from "./features/cast/CastPanel";
import { AnimationWheel } from "./features/wheel/AnimationWheel";
import { SettingsPanel } from "./features/settings/SettingsPanel";

function Status({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  if (!state.ready) return <><span class="lamp" data-state="wait"/><span>waiting for runtime…</span></>;
  const plugin = state.plugin;
  return <><span class="lamp" data-state="ok"/><span id="statusText" title={plugin ? `${plugin.plugin || "OSF Animation"} v${plugin.version || "?"} · stage online` : "OSF Animation · stage online"}>
    OSF {plugin?.version || "?"}{plugin?.ui?.version && <> <span class="sep">·</span> UI {plugin.ui.version}</>}
    {plugin?.ui?.outdated && plugin.ui.nexusUrl && <> <a class="ui-update" href={plugin.ui.nexusUrl} target="_blank" rel="noreferrer" title={`OSF UI v${plugin.ui.tested || "?"} available`} onClick={(event) => { event.preventDefault(); commands.openModPage(plugin.ui!.nexusUrl!); }}>UPDATE</a></>}
  </span></>;
}

function RunningSummary({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const scenes = activeScenes(state);
  if (!scenes.length) return <div class="take-chip take-idle"><div class="take-body"><span class="lbl">SCENE STATUS</span><strong>No scene running</strong></div><span class="take-idle-hint mono">ready to launch</span></div>;
  if (scenes.length > 1) return <div class="take-chip live"><span class="live-dot"/><button class="take-body take-open" onClick={() => commands.setMode("active")}><span class="lbl">RUNNING · {scenes.length} SCENES</span><strong>{scenes.map((scene) => sceneTitle(state, scene.sceneId)).join(" · ")}</strong><span class="take-cast mono">manage in ACTIVE ▸</span></button></div>;
  const active = scenes[0];
  const scene = sceneById(state, active.sceneId);
  return <div class="take-chip live"><span class="live-dot"/><button class="take-body take-open" onClick={() => commands.setMode("active")}><span class="lbl">RUNNING · #{active.handle}{active.player ? " · YOU" : ""}</span><strong>{sceneTitle(state, active.sceneId)}</strong>{!!active.cast.length && <span class="take-cast mono">{active.cast.map((member) => member.name).join(" + ")}</span>}</button>{(scene?.stages.length ?? 0) > 1 && <button class="next-mini" onClick={() => commands.advance(active.handle)}>NEXT ▸</button>}<button class="stop-mini" onClick={() => commands.stop(active.handle)}>■ STOP</button></div>;
}

function Header({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  return <><div class="slate"><div class="slate-cell slate-brand"><div class="brand-lockup"><div class="brand-emblem"><span/></div><div class="brand-meta"><div class="brand-title">Animation Browser</div><div class="brand-sub"><Status state={state} commands={commands}/></div></div></div><div class="brand-tools"><button class="iconbtn" type="button" title="Refresh catalog" onClick={commands.refresh}>⟳</button><button class={`iconbtn settings-btn ${state.settingsOpen ? "on" : ""}`} type="button" title="Browser settings" aria-pressed={state.settingsOpen} onClick={() => commands.toggleSettings()}><svg class="gear-ico" width="13" height="13" viewBox="0 0 14 14" aria-hidden="true"><circle cx="7" cy="7" r="4.35"/><circle cx="7" cy="7" r="1.5"/><path d="M11.35 7L13.15 7M10.08 10.08L11.35 11.35M7 11.35L7 13.15M3.92 10.08L2.65 11.35M2.65 7L0.85 7M3.92 3.92L2.65 2.65M7 2.65L7 0.85M10.08 3.92L11.35 2.65"/></svg><span class="iconbtn-lbl">Settings</span></button><span class="brand-tool-sep"/><button class="iconbtn" type="button" title="Minimize — watch the scene" onClick={() => commands.setMinimized(true)}><svg class="chev-ico" width="10" height="9" viewBox="0 0 10 9" aria-hidden="true"><path d="M1 1l4 3.8L9 1"/><path d="M1.2 8h7.6"/></svg></button></div></div><div class="slate-cell slate-take"><RunningSummary state={state} commands={commands}/></div></div><div class="stripe"><span/><span/><span/></div></>;
}

function MinimizedBar({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  if (!state.minimized) return null;
  const scene = sceneById(state, state.lastSceneId);
  const active = activeScenes(state).find((candidate) => candidate.handle === state.lastHandle);
  const stages = scene?.stages ?? [];
  const canAdvance = stages.length > 1;
  const stage = scene && active && canAdvance && active.stage >= 0 && active.stage < stages.length ? { current: active.stage + 1, total: stages.length, name: stageLabel(scene, active.stage), nextName: active.stage + 1 < stages.length ? stageLabel(scene, active.stage + 1) : undefined } : null;
  return <LiveBar running={!!state.lastHandle} handle={state.lastHandle} title={scene?.title ?? state.lastSceneId} stage={stage} canAdvance={canAdvance} onAdvance={() => commands.advance()} onStop={() => commands.stop()} onExpand={() => commands.setMinimized(false)}/>;
}

function ActorIndicators({ state }: { state: BrowserState }) {
  if (!state.viewVisible || state.wheel || state.minimized) return null;
  const selected = state.cast
    .map((member, index) => ({ member, index }))
    .filter(({ member }) => member.kind !== "player");
  if (!selected.length) return null;

  const byToken = new Map(state.actorIndicators.map((indicator) => [indicator.token, indicator]));
  const projected = selected.filter(({ member }) => byToken.get(member.token)?.visible);
  if (!projected.length) {
    return <div class="selected-actors-hud" aria-live="polite">
      <span class="selected-actors-label mono">SELECTED CREW</span>
      <div class="selected-actors-list">
        {selected.map(({ member, index }) => <span key={member.token} class="selected-actor-chip">
          <span class="world-actor-key">{String.fromCharCode(65 + index)}</span>
          <span class="world-actor-name">{member.name}</span>
          <i/>
        </span>)}
      </div>
    </div>;
  }

  return <div class="world-indicators" aria-hidden="true">
    {projected.map(({ member, index }) => {
      const indicator = byToken.get(member.token)!;
      return <div key={member.token} class="world-actor-indicator"
        style={{ left: `${indicator.x * 100}%`, top: `${indicator.y * 100}%` }}>
        <span class="world-actor-key">{String.fromCharCode(65 + index)}</span>
        <span class="world-actor-name">{member.name}</span>
        <i/>
      </div>;
    })}
  </div>;
}

export function App({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const selected = selectedPlayable(state);
  const compactContext = state.mode !== "active" && !!selected && selected.kind !== "scene"
    && selected.scene.actorCount === 1 && !selected.scene.requiresFurniture;
  return <div class={`stage ${state.pickMode ? "picking" : ""} ${state.settingsOpen ? "settings-mode" : ""} ${compactContext ? "compact-context" : ""}`}>
    <ActorIndicators state={state}/>
    <div class="console"><span class="bracket tl"/><span class="bracket tr"/><span class="bracket bl"/><span class="bracket br"/><div class="grid-overlay"/><Header state={state} commands={commands}/>{state.settingsOpen ? <SettingsPanel state={state} commands={commands}/> : <div class="director"><aside class="rail"><CastPanel state={state} commands={commands}/><AnchorPanel state={state} commands={commands}/></aside><section class="browse"><BrowsePanel state={state} commands={commands}/></section></div>}<footer class={`notice ${state.notice.kind}`} aria-live="polite">{state.notice.text}</footer></div>
    <aside class="brief"><SceneBrief state={state} commands={commands}/></aside>
    <div class="livebar"><MinimizedBar state={state} commands={commands}/></div>
    <div class="wheel"><AnimationWheel state={state} commands={commands}/></div>
    {state.pickMode && <div class="pick-hud mono" aria-live="polite">
      <strong>SELECT {state.pickMode === "actor" ? "ACTOR" : "FURNITURE"}</strong>
      <span>Click it in the world · Esc to cancel</span>
    </div>}
  </div>;
}
