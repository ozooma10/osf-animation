import type { BrowserCommands } from "../../app/commands";
import { PLAYER_TOKEN, type ActiveScene, type BrowserState, type CastMember } from "../../app/state";
import type { RouteEventKind, RouteEventModel, RouteModel, RouteTransitionModel } from "../../model";
import { TIMELINE_FPS, timelineFrame } from "../browse/timeline";
import { Empty } from "../shared/Shared";

const LANES: Array<{ kind: RouteEventKind; label: string }> = [
  { kind: "commit", label: "COMMIT" },
  { kind: "marker", label: "MARKERS" },
  { kind: "prop", label: "PROPS" },
  { kind: "sound", label: "AUDIO" },
];

function routeTarget(state: BrowserState): CastMember | null {
  return state.cast.find((member) => member.token === state.routeActorToken) ?? state.cast[0] ?? null;
}

function activePreview(state: BrowserState, route: RouteModel | null, transition: RouteTransitionModel | null): ActiveScene | null {
  if (!route || !transition) return null;
  return state.active?.find((item) => item.inspectionKind === "route"
    && item.routeId === route.id && item.transitionId === transition.id) ?? null;
}

function seconds(value: number): string {
  return `${Math.floor(value / 60)}:${(value % 60).toFixed(2).padStart(5, "0")}`;
}

function routeHaystack(route: RouteModel): string {
  return [route.id, route.sourceFile, ...route.stations.map((station) => station.id),
    ...route.transitions.flatMap((transition) => [transition.id, transition.from, transition.to, transition.layer.clip])]
    .join(" ").toLowerCase();
}

function RouteList({ state, routes, commands }: { state: BrowserState; routes: RouteModel[]; commands: BrowserCommands }) {
  return <aside class="route-list">
    <div class="search-field"><input type="text" value={state.routeSearch}
      onInput={(event) => commands.setRouteSearch(event.currentTarget.value)}
      placeholder="⌕ route · station · clip" autocomplete="off" spellcheck={false}/></div>
    <div class="route-list-summary mono">{routes.length} / {state.routes.length} ROUTES</div>
    <div class="route-list-scroll">
      {routes.map((route) => <button class={`route-row ${route.id === state.selectedRouteId ? "selected" : ""}`}
        onClick={() => commands.selectRoute(route.id)} key={route.id}>
        <span class="route-row-main"><strong>{route.id}</strong><span>{route.sourceFile || "unknown source"}</span></span>
        <span class="route-row-count mono">{route.stations.length}S · {route.transitions.length}T</span>
      </button>)}
      {!routes.length && <Empty>No routes match this search.</Empty>}
    </div>
  </aside>;
}

function TargetPicker({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const target = routeTarget(state);
  return <div class="route-target">
    <div class="route-target-copy"><span class="eb">Preview actor</span><strong>{target?.name ?? "No actor selected"}</strong>
      <span>Routes are one-actor overlays. The debugger uses one member of the current crew.</span></div>
    <div class="route-target-choices">
      {state.cast.map((member) => <button class={target?.token === member.token ? "on" : ""}
        title={`Use ${member.name} for route previews`} onClick={() => commands.selectRouteActor(member.token)} key={member.token}>
        {member.token === PLAYER_TOKEN ? "YOU" : member.name}
      </button>)}
      {!state.cast.length && <button onClick={commands.togglePlayer}>+ PLAYER</button>}
      <button class="quiet" onClick={() => commands.toggleRouteDebugger(false)}>EDIT CREW</button>
    </div>
  </div>;
}

function Topology({ route, selected, commands }: { route: RouteModel; selected: RouteTransitionModel | null; commands: BrowserCommands }) {
  return <section class="route-topology">
    <div class="route-section-head"><span>ROUTE TOPOLOGY</span><span>{route.stations.length} STATIONS · {route.transitions.length} TRANSITIONS</span></div>
    <div class="route-stations">
      {route.stations.map((station) => <div class={`route-station ${station.layer ? "layered" : "zero"}`} key={station.id}>
        <span class="route-node-dot"/><strong>{station.id}</strong>
        <span class="mono">{station.layer ? `${station.layer.mask} · hold ${station.layer.holdAt ?? 1}` : "ZERO-ANIMATION"}</span>
      </div>)}
    </div>
    <div class="route-transitions">
      {route.transitions.map((transition, index) => <button class={`route-edge ${selected?.id === transition.id ? "selected" : ""}`}
        onClick={() => commands.selectRouteTransition(transition.id)} key={transition.id}>
        <span class="route-edge-index mono">{String(index + 1).padStart(2, "0")}</span>
        <span class="route-edge-path"><strong>{transition.from}</strong><i>→</i><strong>{transition.to}</strong><small>{transition.id}</small></span>
        <span class="route-edge-meta mono">{transition.events.length} EVENTS<br/>{transition.interruption === "finish" ? "FINISH" : "XFADE PRE-COMMIT"}</span>
      </button>)}
    </div>
  </section>;
}

function nearestEvent(events: RouteEventModel[], frame: number, direction: -1 | 1): RouteEventModel | null {
  const ordered = [...events].sort((a, b) => a.frame - b.frame);
  return direction < 0
    ? [...ordered].reverse().find((event) => event.frame < frame - 0.001) ?? null
    : ordered.find((event) => event.frame > frame + 0.001) ?? null;
}

function EventLanes({ transition, preview, totalFrames, currentFrame, commands }: {
  transition: RouteTransitionModel;
  preview: ActiveScene | null;
  totalFrames: number;
  currentFrame: number;
  commands: BrowserCommands;
}) {
  const seek = (event: RouteEventModel) => preview && !isUnreachable(event, totalFrames)
    && commands.setPlayback(preview.handle, event.frame / TIMELINE_FPS, true);
  return <section class="route-event-lanes" aria-label="Authored route events">
    {LANES.map((lane) => {
      const events = transition.events.filter((event) => event.kind === lane.kind);
      return <div class="route-event-lane" data-kind={lane.kind} key={lane.kind}>
        <div class="route-lane-head mono"><span>{lane.label}</span><span>{events.length || "—"}</span></div>
        <div class="route-lane-rail">
          <span class="route-playhead" style={{ left: `${Math.min(100, currentFrame / Math.max(1, totalFrames) * 100)}%` }}/>
          {events.map((event, index) => {
            const unreachable = isUnreachable(event, totalFrames);
            const passed = !unreachable && event.frame <= currentFrame + 0.001;
            return <button class={`route-event-mark ${passed ? "passed" : ""} ${unreachable ? "unreachable" : ""}`}
              style={{ left: `${Math.min(100, event.frame / Math.max(1, totalFrames) * 100)}%` }}
              disabled={!preview || unreachable} title={`${event.label} · frame ${event.frame}${event.detail ? ` · ${event.detail}` : ""}`}
              onClick={() => seek(event)} key={`${event.frame}:${event.label}:${index}`}><span/></button>;
          })}
        </div>
        <div class="route-event-list">
          {events.map((event, index) => {
            const unreachable = isUnreachable(event, totalFrames);
            return <button disabled={!preview || unreachable} onClick={() => seek(event)} key={`${event.label}:${index}`}>
              <span><strong>{event.label}</strong><small>{event.detail}</small></span>
              <span class={`mono ${unreachable ? "bad" : ""}`}>F{event.frame}{unreachable ? " · UNREACHABLE" : ""}</span>
            </button>;
          })}
          {!events.length && <span class="route-lane-empty">No authored {lane.label.toLowerCase()} events.</span>}
        </div>
      </div>;
    })}
  </section>;
}

function isUnreachable(event: RouteEventModel, totalFrames: number): boolean {
  // Runtime atFrame marks are strict: a mark at or past the decoded clip end never fires.
  return totalFrames > 0 && event.frame >= totalFrames;
}

function TransitionInspector({ state, route, transition, commands }: {
  state: BrowserState;
  route: RouteModel;
  transition: RouteTransitionModel;
  commands: BrowserCommands;
}) {
  const preview = activePreview(state, route, transition);
  const target = routeTarget(state);
  const hintFrames = transition.layer.durationHint ? timelineFrame(transition.layer.durationHint) : 0;
  const eventFrames = transition.events.reduce((max, event) => Math.max(max, Math.ceil(event.frame) + 1), 1);
  const totalFrames = preview?.duration ? timelineFrame(preview.duration) : Math.max(hintFrames, eventFrames);
  const time = preview ? Math.min(preview.time, preview.duration) : 0;
  const currentFrame = preview ? timelineFrame(time) : 0;
  const before = nearestEvent(transition.events, currentFrame, -1);
  const after = nearestEvent(transition.events, currentFrame, 1);
  const seekEvent = (event: RouteEventModel | null) => {
    if (preview && event && !isUnreachable(event, totalFrames)) commands.setPlayback(preview.handle, event.frame / TIMELINE_FPS, true);
  };
  return <section class="route-inspector">
    <div class="route-transition-title">
      <div><span class="eb">Selected transition</span><h3>{transition.id}</h3><p><strong>{transition.from}</strong><i>→</i><strong>{transition.to}</strong></p></div>
      <div class="route-transition-actions">
        <button class="route-preview-btn" disabled={!state.ready || !target}
          onClick={() => target && commands.inspectRoute(route.id, transition.id, target.token)}>
          ◇ {preview ? "RESTART PREVIEW" : "PREVIEW TRANSITION"}
        </button>
        {preview && <button class="route-stop-btn" onClick={() => commands.stop(preview.handle)}>■ STOP</button>}
      </div>
    </div>
    <div class="route-readouts">
      <span><label>CLIP</label><strong title={transition.layer.clip}>{transition.layer.clip || "—"}</strong></span>
      <span><label>MASK</label><strong>{transition.layer.mask || "—"}</strong></span>
      <span><label>POSE</label><strong>{transition.layer.mode} · {transition.layer.weight.toFixed(2)}</strong></span>
      <span><label>INTERRUPT</label><strong>{transition.interruption}</strong></span>
      <span><label>DURATION</label><strong>{preview?.duration ? `${preview.duration.toFixed(3)}s decoded` : transition.layer.durationHint ? `${transition.layer.durationHint.toFixed(3)}s authored` : "decode on preview"}</strong></span>
    </div>
    <div class={`route-transport ${preview ? "live" : "idle"}`}>
      <button class="route-event-step" disabled={!preview || !before} title="Previous authored event" onClick={() => seekEvent(before)}>◀ EVENT</button>
      <button class="frame-step" disabled={!preview || currentFrame <= 0} title="Previous frame ([)" onClick={() => commands.stepRouteFrame(-1)}>◀|</button>
      <button class={`play-toggle ${preview?.speed ? "" : "paused"}`} disabled={!preview}
        title={preview?.speed ? "Pause preview" : "Play preview — loops with callbacks and audio suppressed"}
        onClick={() => preview && commands.setPlayback(preview.handle, undefined, preview.speed > 0)}>{preview?.speed ? "Ⅱ" : "▶"}</button>
      <button class="frame-step" disabled={!preview || currentFrame >= totalFrames} title="Next frame (])" onClick={() => commands.stepRouteFrame(1)}>|▶</button>
      <button class="route-event-step" disabled={!preview || !after || isUnreachable(after, totalFrames)} title="Next authored event" onClick={() => seekEvent(after)}>EVENT ▶</button>
      <input class="timeline-range" type="range" min="0" max={preview?.duration || Math.max(1 / TIMELINE_FPS, totalFrames / TIMELINE_FPS)}
        step={1 / TIMELINE_FPS} value={time} disabled={!preview}
        aria-label={`Route frame ${currentFrame} of ${totalFrames}`}
        onInput={(event) => preview && commands.setPlayback(preview.handle, Number(event.currentTarget.value), true)}/>
      <span class="route-clock mono"><strong>FRAME {currentFrame} / {totalFrames}</strong><span>{seconds(time)} / {seconds(preview?.duration ?? totalFrames / TIMELINE_FPS)}</span></span>
    </div>
    <EventLanes transition={transition} preview={preview} totalFrames={totalFrames} currentFrame={currentFrame} commands={commands}/>
    <div class="route-debug-boundary"><strong>DEBUG PREVIEW BOUNDARY</strong><span>Pose and OSF-owned props are reconstructed at the selected frame. Commit/marker callbacks, external props, audio, destination-station entry, and consumer state changes are not fired. <kbd>[</kbd> / <kbd>]</kbd> steps one 30 fps frame.</span></div>
  </section>;
}

export function RouteDebuggerPanel({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const routes = state.routes.filter((route) => !state.routeSearch || routeHaystack(route).includes(state.routeSearch));
  const route = state.routes.find((item) => item.id === state.selectedRouteId) ?? routes[0] ?? null;
  const transition = route?.transitions.find((item) => item.id === state.selectedTransitionId) ?? route?.transitions[0] ?? null;
  return <section class="route-debugger" aria-label="Route Debugger">
    <div class="settings-head route-debugger-head">
      <div><p class="eb">Overlay route inspection</p><h2>Route Debugger</h2></div>
      <div class="route-head-actions"><button class="iconbtn" type="button" title="Refresh route catalog" onClick={commands.refreshRoutes}>⟳</button>
        <button class="iconbtn" type="button" title="Close Route Debugger" aria-label="Close Route Debugger" onClick={() => commands.toggleRouteDebugger(false)}>×</button></div>
    </div>
    <div class="route-debugger-body">
      <RouteList state={state} routes={routes} commands={commands}/>
      <main class="route-workspace">
        {!state.routesReceived ? <Empty>Waiting for the route registry…</Empty> : !route ? <Empty>No overlay routes are loaded. Check Imports for rejected route definitions.</Empty> : <>
          <div class="route-identity"><div><span class="eb">Route definition</span><h3>{route.id}</h3><p class="mono">{route.sourceFile || "source unavailable"}</p></div><span class="route-schema mono">ROUTE V1</span></div>
          <TargetPicker state={state} commands={commands}/>
          <Topology route={route} selected={transition} commands={commands}/>
          {transition ? <TransitionInspector state={state} route={route} transition={transition} commands={commands}/>
            : <Empty>This route has no transitions to inspect.</Empty>}
        </>}
      </main>
    </div>
  </section>;
}
