import type { BrowserCommands } from "../../app/commands";
import { useRef } from "preact/hooks";
import {
  anchorFull,
  evaluateForState,
  formatDuration,
  formatEstimate,
  isEmote,
  isWheelEmote,
  isWheelStage,
  sceneById,
  stageClean,
  wheelKey,
  wheelPool,
} from "../../app/selectors";
import type { BrowserState } from "../../app/state";
import type { SceneEvaluation, SceneModel, SceneStage } from "../../model";
import { MoveButtons } from "../shared/Shared";

function WheelControls({ state, scene, stage, wide, commands }: {
  state: BrowserState;
  scene: SceneModel;
  stage?: SceneStage;
  wide?: boolean;
  commands: BrowserCommands;
}) {
  if (stage ? !isWheelStage(scene, stage) : !isWheelEmote(scene)) return null;
  if (state.wheelCustomized && !state.libraryReceived) return null;
  const stageIndex = stage?.index ?? null;
  const pool = wheelPool(state);
  const index = pool.findIndex((item) => item.key === wheelKey(scene.id, stageIndex));
  const onWheel = index >= 0;
  return <span class={`anim-wheel-controls ${wide ? "wide" : ""}`}>
    <button class={`pin-btn ${onWheel ? "on" : ""} ${wide ? "" : "compact"}`} title={onWheel ? "Remove from the animation wheel" : "Add to the animation wheel"} onClick={() => commands.toggleWheelEntry(scene.id, stageIndex)}>
      {wide ? onWheel ? "◆ ON WHEEL" : "◇ ADD TO WHEEL" : onWheel ? "◆" : "◇"}
    </button>
    {onWheel && state.wheelCustomized && <>
      <span class="wheel-order mono">{index + 1}/{pool.length}</span>
      <button class="pin-btn compact" disabled={index <= 0} title="Move earlier on wheel" onClick={() => commands.moveWheelEntry(scene.id, stageIndex, -1)}>←</button>
      <button class="pin-btn compact" disabled={index >= pool.length - 1} title="Move later on wheel" onClick={() => commands.moveWheelEntry(scene.id, stageIndex, 1)}>→</button>
    </>}
  </span>;
}

function RoleMap({ state, scene, evaluation, commands }: { state: BrowserState; scene: SceneModel; evaluation: SceneEvaluation; commands: BrowserCommands }) {
  const dragFrom = useRef(-1);
  const actorCount = evaluation.actorCount;
  if (actorCount < 2 || !state.cast.length) return null;
  const slots = Math.max(actorCount, state.cast.length);
  return <div class="info-box"><div class="lbl">ROLES · {actorCount}</div><div class="role-map">
    {Array.from({ length: slots }, (_, index) => {
      const inScene = index < actorCount;
      const role = inScene ? scene.roles[index] : null;
      const member = state.cast[index];
      return <div class={`role-row ${inScene ? "" : "bench"} ${member ? "" : "unfilled"}`} key={index}>
        <span class="role-name">{inScene ? role?.name || `role ${index + 1}` : "bench"}{role?.gender && role.gender !== "any" && <span class="role-g">{role.gender}</span>}</span><span class="role-arrow">→</span>
        {member ? <span class={`castline ${member.kind === "player" ? "player" : ""}`} tabindex={0} draggable={state.cast.length > 1}
          onDragStart={(event) => { dragFrom.current = index; if (event.dataTransfer) { event.dataTransfer.effectAllowed = "move"; try { event.dataTransfer.setData("text/plain", String(index)); } catch { /* some Ultralight builds disallow setData */ } } }}
          onDragOver={(event) => { if (dragFrom.current >= 0) { event.preventDefault(); if (event.dataTransfer) event.dataTransfer.dropEffect = "move"; } }}
          onDrop={(event) => { if (dragFrom.current < 0) return; event.preventDefault(); const bounds = event.currentTarget.getBoundingClientRect(); commands.reorderMember(dragFrom.current, index, event.clientY - bounds.top > bounds.height / 2); dragFrom.current = -1; }}
          onDragEnd={() => { dragFrom.current = -1; }}
          onKeyDown={(event) => { if (event.altKey && (event.key === "ArrowUp" || event.key === "ArrowDown")) { event.preventDefault(); commands.moveMember(index, event.key === "ArrowDown" ? 1 : -1); } }}>
          <span class="castline-name">{member.name}</span><MoveButtons index={index} count={state.cast.length} onMove={(direction) => commands.moveMember(index, direction)}/></span>
          : <span class="role-empty mono">add an actor →</span>}
      </div>;
    })}
  </div>{state.cast.length > actorCount && <div class="role-note mono">{state.cast.length - actorCount} extra beyond this scene's roles — trim in CREW</div>}</div>;
}

function AnimationList({ state, scene, canPlay, commands }: { state: BrowserState; scene: SceneModel; canPlay: boolean; commands: BrowserCommands }) {
  if (!scene.stages.length) return null;
  const clean = scene.library ? scene.stages.filter(stageClean) : scene.stages;
  const noise = scene.library ? scene.stages.filter((stage) => !stageClean(stage)) : [];
  const folded = clean.length > 0 && noise.length > 0 && !state.briefFullAnims;
  const shown = clean.length ? folded ? clean : [...clean, ...noise] : scene.stages;
  return <div class="info-box"><div class="lbl">ANIMATIONS · {shown.length}{folded ? ` OF ${scene.stages.length}` : ""}</div><div class="anim-list">
    {shown.map((stage) => {
      const label = stage.name || (isEmote(scene) ? scene.stages.length === 1 ? scene.title : `Part ${stage.index + 1}` : `Stage ${stage.index}`);
      const loop = formatDuration(stage.loopSec);
      const duration = loop || formatDuration(stage.estSec);
      return <div class="anim-row" key={stage.index}><div class="anim-main"><span class="anim-name">{label}</span><div class="anim-tags">{stage.tags.slice(0, 3).map((tag) => <span class="pill" key={tag}>{tag}</span>)}</div></div>
        {duration && <span class="anim-dur" title={loop ? "Loop length" : "Stage time"}>{duration}{stage.openEnded ? "∞" : ""}</span>}
        <WheelControls state={state} scene={scene} stage={stage} commands={commands}/>
        <button class="anim-play" disabled={!canPlay} title="Play this animation" onClick={() => commands.launch(stage.index)}>▶</button>
      </div>;
    })}
    {!!clean.length && !!noise.length && <button class={`reveal anim-fold ${state.briefFullAnims ? "on" : ""}`} onClick={commands.toggleBriefAnimations}>{folded ? `+ ${noise.length} transition${noise.length === 1 ? "" : "s"} & layers ▸` : "poses & loops only"}</button>}
  </div></div>;
}

function Overrides({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const options = state.opts;
  const tweaks: string[] = [];
  if (options.strip !== "-1") tweaks.push(`strip ${options.strip === "1" ? "on" : "off"}`);
  if (options.lock !== "-1") tweaks.push(`lock ${options.lock === "1" ? "on" : "off"}`);
  if (options.camera) tweaks.push(`cam ${options.camera.replace("thirdperson_hold", "3rd person").replace("scene_orbit", "orbit").replace("freefly", "free fly").replace("vanity_orbit", "vanity")}`);
  if (Number(options.speed) !== 1) tweaks.push(`${Number(options.speed).toFixed(1)}x`);
  return <div class={`overrides ${state.optsOpen ? "open" : ""}`}>
    <button class="overrides-head" title={`${state.optsOpen ? "Collapse" : "Expand"} start overrides`} onClick={commands.toggleOptions}><span class="chev">{state.optsOpen ? "▾" : "▸"}</span><span class="lbl">START OVERRIDES</span>{!state.optsOpen && <span class={`overrides-sum mono ${tweaks.length ? "hot" : ""}`}>{tweaks.length ? tweaks.join(" · ") : "defaults"}</span>}</button>
    {state.optsOpen && <div class="override-grid">
      <label class="override"><span class="lbl">STRIP</span><select class="select" value={options.strip} onChange={(event) => commands.setOption("strip", event.currentTarget.value)}><option value="-1">Inherit</option><option value="1">On</option><option value="0">Off</option></select></label>
      <label class="override"><span class="lbl">LOCK PLAYER</span><select class="select" value={options.lock} onChange={(event) => commands.setOption("lock", event.currentTarget.value)}><option value="-1">Inherit</option><option value="1">On</option><option value="0">Off</option></select></label>
      <label class="override"><span class="lbl">CAMERA</span><select class="select" value={options.camera} onChange={(event) => commands.setOption("camera", event.currentTarget.value)}><option value="">Inherit</option><option value="thirdperson_hold">Third person</option><option value="scene_orbit">Scene orbit</option><option value="freefly">Free fly</option><option value="vanity_orbit">Vanity orbit</option></select></label>
      <label class="override"><span class="lbl">SPEED <b>{Number(options.speed).toFixed(1)}x</b></span><input class="range" type="range" min="0.1" max="3" step="0.1" value={options.speed} onInput={(event) => commands.setOption("speed", event.currentTarget.value)}/></label>
    </div>}
  </div>;
}

function Diagnostics({ scene, evaluation }: { scene: SceneModel; evaluation: SceneEvaluation }) {
  const rows = [
    ["weight · priority", `${scene.weight} · ${scene.priority}`],
    ["anchor", `${scene.requiresFurniture ? anchorFull(scene) || "required" : "free-space"} · ${evaluation.anchorGate ? "pass" : "fail"}`],
    ["stages · shape", `${scene.stages.length} · ${scene.shape.kind}`],
    ["policies", `strip ${scene.policy.stripActors} · lock ${scene.policy.lockPlayer} · fade ${scene.policy.fade} · cam ${scene.policy.camera}`],
    ["est duration", formatEstimate(scene) || "unmeasured"],
  ];
  return <div class="info-box hud"><div class="lbl">DIAGNOSTICS</div><div class="kv-list">{rows.map(([key, value]) => <div class="kv" key={key}><span class="k">{key}</span><span class="v">{value}</span></div>)}</div></div>;
}

export function SceneBrief({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const scene = sceneById(state, state.selectedId);
  if (!scene) return <div class="brief-empty"><span class="mono">Nothing selected.</span></div>;
  const evaluation = evaluateForState(state, scene);
  const ready = evaluation.gaps === 0;
  const emote = isEmote(scene);
  const canPlay = state.ready && ready;
  const anchor = scene.requiresFurniture
    ? state.furniture ? evaluation.anchorGate ? `on ${state.furniture.name}` : `this furniture doesn't fit${anchorFull(scene) ? ` (needs ${anchorFull(scene)})` : ""}` : anchorFull(scene) ? `needs ${anchorFull(scene)}` : "needs furniture"
    : "free-space";
  const summary = emote ? `quick action · self-terminating · ${anchor}` : `${evaluation.seated}/${evaluation.actorCount || "?"} crew · ${anchor}`;
  const wholeWheel = isWheelEmote(scene);
  const reason = !state.ready ? "Engine not connected." : ready ? "" : evaluation.reason;
  return <>
    <div class={`brief-status ${ready ? "" : "warn"}`}><span class="dot"/><p class="eb">{emote ? ready ? "EMOTE · READY TO PLAY" : "EMOTE · NEEDS ONE ACTOR" : ready ? "READY TO LAUNCH" : "NOT SEATABLE YET"}</p></div>
    <div class="brief-title">{scene.title}{formatEstimate(scene) && <span class="card-dur">{formatEstimate(scene)}</span>}</div>
    {state.filters.debugMode && <div class="mono wrap brief-src">{scene.id} · {scene.sourceFile || "live registry"}</div>}
    <div class={`brief-line ${ready ? "" : "warn"}`}><span class="mono">{summary}</span></div>
    {(wholeWheel || state.wheelCustomized) && <div class="brief-pin">{wholeWheel && <WheelControls state={state} scene={scene} wide commands={commands}/>} {state.wheelCustomized && <button class="pin-btn reset" title="Restore installed default animations" onClick={commands.resetWheel}>RESET DEFAULTS</button>}</div>}
    <div class="brief-scroll"><RoleMap state={state} scene={scene} evaluation={evaluation} commands={commands}/><AnimationList state={state} scene={scene} canPlay={canPlay} commands={commands}/>{state.filters.debugMode && <Diagnostics scene={scene} evaluation={evaluation}/>}</div>
    <div class="brief-foot">{!(emote && !state.filters.debugMode) && <Overrides state={state} commands={commands}/>}<div class="launch-stack">
      {reason && <div class="mono wrap" style={{ color: "var(--text-faint)", textAlign: "center" }}>{reason}</div>}
      {canPlay ? <button class="launch-btn go" onClick={() => commands.launch()}>▶ {emote ? "Play Emote" : scene.library ? "Play Animation Set" : "Launch Scene"}</button> : <button class="launch-btn blocked" disabled>{!state.ready ? "Engine Offline" : `Blocked · ${evaluation.gaps} gap${evaluation.gaps > 1 ? "s" : ""}`}</button>}
      {!!state.lastHandle && <button class="stop-btn" onClick={() => commands.stop()}>■ Stop #{state.lastHandle}</button>}
    </div></div>
  </>;
}
