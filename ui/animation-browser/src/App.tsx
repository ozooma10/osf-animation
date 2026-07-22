import type { BrowserCommands } from "./app/commands";
import { activeScenes, sceneById, sceneTitle, stageLabel } from "./app/selectors";
import type { BrowserState } from "./app/state";
import { LiveBar } from "./components/LiveBar";
import { AnchorPanel } from "./features/anchor/AnchorPanel";
import { SceneBrief } from "./features/brief/SceneBrief";
import { BrowsePanel } from "./features/browse/BrowsePanel";
import { CastPanel } from "./features/cast/CastPanel";
import { AnimationWheel } from "./features/wheel/AnimationWheel";

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
  return <><div class="slate"><div class="slate-cell slate-brand"><div class="brand-lockup"><div class="brand-emblem"><span/></div><div class="brand-meta"><div class="brand-title">Animation Browser</div><div class="brand-sub"><Status state={state} commands={commands}/></div></div></div><div class="brand-tools"><button class="iconbtn" type="button" title="Refresh catalog" onClick={commands.refresh}>⟳</button><div class="debug-wrap"><span class="lbl">DEBUG</span><button class={`switch ${state.filters.debugMode ? "on" : ""}`} type="button" aria-label="Toggle debug mode" onClick={commands.toggleDebug}><i/></button></div><span class="brand-tool-sep"/><button class="iconbtn" type="button" title="Minimize — watch the scene" onClick={() => commands.setMinimized(true)}><svg class="chev-ico" width="10" height="9" viewBox="0 0 10 9" aria-hidden="true"><path d="M1 1l4 3.8L9 1"/><path d="M1.2 8h7.6"/></svg></button></div></div><div class="slate-cell slate-take"><RunningSummary state={state} commands={commands}/></div></div><div class="stripe"><span/><span/><span/></div></>;
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

export function App({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  return <div class="stage">
    <div class="console"><span class="bracket tl"/><span class="bracket tr"/><span class="bracket bl"/><span class="bracket br"/><div class="grid-overlay"/><Header state={state} commands={commands}/><div class="director"><aside class="rail"><CastPanel state={state} commands={commands}/><AnchorPanel state={state} commands={commands}/></aside><section class="browse"><BrowsePanel state={state} commands={commands}/></section></div><footer class={`notice ${state.notice.kind}`} aria-live="polite">{state.notice.text}</footer></div>
    <aside class="brief"><SceneBrief state={state} commands={commands}/></aside>
    <div class="livebar"><MinimizedBar state={state} commands={commands}/></div>
    <div class="wheel"><AnimationWheel state={state} commands={commands}/></div>
  </div>;
}
