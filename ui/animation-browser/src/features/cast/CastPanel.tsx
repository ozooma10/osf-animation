import type { BrowserCommands } from "../../app/commands";
import { animationList, busyTokens, hasPlayer, sceneCatalog, speciesLabel } from "../../app/selectors";
import type { BrowserState, CastMember } from "../../app/state";

function StepHead({ open, summary, onClick }: { open: boolean; summary: string; onClick(): void }) {
  return (
    <button class="step-head" onClick={onClick}>
      <span class="step-num">1</span><span class="eb">CREW</span>
      <span class="step-note">{summary}</span><span class="chev">{open ? "▾" : "▸"}</span>
    </button>
  );
}

function Face() {
  return <span class="near-face blank" aria-hidden="true"><svg viewBox="0 0 24 24"><circle cx="12" cy="9" r="4"/><path d="M4 22c0-4.4 3.6-7 8-7s8 2.6 8 7"/></svg></span>;
}

function CastChip({ member, index, live, commands }: {
  member: CastMember;
  index: number;
  live: boolean;
  commands: BrowserCommands;
}) {
  const player = member.kind === "player";
  return (
    <span class={`castline ${player ? "player" : ""}`}>
      <span class="cast-key">{String.fromCharCode(65 + index)}</span>
      <span class="castline-name">{member.name}</span>
      {!player && member.species !== "human" && (
        <span class="cast-species" title="Skeleton family — the library filters to its animations">
          {speciesLabel(member.species)}
        </span>
      )}
      {live && <span class="cast-busy" title="Currently in a running scene — launching on them replaces it">LIVE</span>}
      <button class="chip-x" title={player ? "Remove player (NPC-only scene)" : "Remove from crew"}
        onClick={() => player ? commands.togglePlayer() : commands.removeMember(index)}>×</button>
    </span>
  );
}

export function CastPanel({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const open = state.stepOpen.cast;
  const summary = state.cast.length ? state.cast.map((member) => member.name).join(" + ") : "No cast";
  if (!open) return <div class="step closed"><StepHead open={false} summary={summary} onClick={() => commands.toggleStep("cast")}/></div>;

  const busy = busyTokens(state);
  const fitting = sceneCatalog(state).filter((scene) => scene.actorCount === state.cast.length).length;
  const animationNote = state.cast.length === 1
    ? state.libraryReceived ? ` · animations ${animationList(state).length}` : " · + animations"
    : "";
  return (
    <div class="step">
      <StepHead open summary={`${state.cast.length} on deck`} onClick={() => commands.toggleStep("cast")}/>
      <div class="cast-stack">
        {state.cast.map((member, index) => <CastChip key={member.token} member={member} index={index} live={busy.has(member.token)} commands={commands}/>)}
        {!hasPlayer(state) && <button class="castline ghost" title="Add player back to the crew" onClick={commands.togglePlayer}>＋ Player</button>}
      </div>
      {state.cast.length > 1 && <div class="cast-order-hint mono">A·B·C order sets roles — arrange in ROLES →</div>}
      <div class="step-sub"><span class="lbl">NEARBY</span><span class="step-tools">
        <button class="chip-btn" onClick={() => commands.scan("actor")}>SCAN</button>
        <button class="chip-btn" onClick={() => commands.pick("actor")}>PICK</button>
      </span></div>
      <div class="near-list">
        {state.nearbyActors.length ? state.nearbyActors.map((actor) => {
          const added = state.cast.some((member) => member.token === actor.token);
          return (
            <button key={actor.token} class={`near-row ${added ? "active" : ""}`} onClick={() => commands.toggleActor(actor.token)}>
              <Face/><span class="near-name">{actor.name}</span>
              {busy.has(actor.token) && <span class="cast-busy" title="Currently in a running scene">LIVE</span>}
              <span class="near-meta mono">{actor.distance == null ? "" : `${Math.max(1, Math.round(actor.distance))}m`}</span>
              <span class={`near-tag ${added ? "added" : ""}`}>{added ? "✓" : "ADD"}</span>
            </button>
          );
        }) : <div class="empty-mini"><span class="mono">Scan, or aim at someone before opening and PICK.</span></div>}
      </div>
      {state.catalogReceived && <div class="step-foot"><span class="mono">
        {fitting} scene{fitting === 1 ? "" : "s"} fit {state.cast.length} actor{state.cast.length === 1 ? "" : "s"}{animationNote}
      </span></div>}
    </div>
  );
}

