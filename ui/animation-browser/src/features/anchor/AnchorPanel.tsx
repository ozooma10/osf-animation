import type { BrowserCommands } from "../../app/commands";
import type { BrowserState, FurnitureTarget, NearbyTarget } from "../../app/state";
import { Dot } from "../shared/Shared";

function StepHead({ open, summary, onClick }: { open: boolean; summary: string; onClick(): void }) {
  return <button class="step-head" onClick={onClick}><span class="step-num">2</span><span class="eb">FURNITURE</span><span class="step-note">{summary}</span><span class="chev">{open ? "▾" : "▸"}</span></button>;
}

function AnchorRow({ anchor, keyed, onToggle }: { anchor: NearbyTarget; keyed: FurnitureTarget | null; onToggle(): void }) {
  const active = keyed?.token === anchor.token;
  const total = anchor.sceneCount;
  const custom = anchor.customCount ?? total;
  const library = total != null && custom != null ? Math.max(0, total - custom) : 0;
  return (
    <button class={`near-row ${active ? "active" : ""} ${anchor.marker ? "marker" : ""}`} onClick={onToggle}>
      <span class="near-name">{anchor.name}</span>
      {custom != null && <span class={`near-badge ${custom ? "" : "empty"}`} title="Custom scenes this furniture unlocks">{custom} fit</span>}
      {!!library && <span class="near-badge lib" title="Vanilla library scenes this furniture unlocks">+{library}</span>}
      <span class="near-meta mono">{anchor.distance == null ? "" : `${Math.max(1, Math.round(anchor.distance))}m`}</span>
      <span class={`near-tag ${active ? "added" : ""}`}>{active ? "✓" : "USE"}</span>
    </button>
  );
}

function MatchLabel({ state }: { state: BrowserState }) {
  if (!state.anchorMatch) return <>checking what fits…</>;
  const catalogIds = new Set(state.catalog.map((scene) => scene.id));
  let custom = 0;
  for (const id of state.anchorMatch.ids) if (catalogIds.has(id)) custom++;
  const library = state.anchorMatch.ids.size - custom;
  return <>{custom} custom scene{custom === 1 ? " keys" : "s key"} to this{library > 0 && <span class="lib-note"> · library {library}</span>}</>;
}

export function AnchorPanel({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const keyed = state.furniture;
  if (!state.stepOpen.anchor) {
    return <div class="step closed"><StepHead open={false} summary={keyed?.name ?? "none"} onClick={() => commands.toggleStep("anchor")}/></div>;
  }
  const furniture = state.nearbyFurniture.filter((anchor) => !anchor.marker);
  const markers = state.nearbyFurniture.filter((anchor) => anchor.marker);
  const markersOpen = state.markersOpen || !!keyed && markers.some((anchor) => anchor.token === keyed.token);
  return (
    <div class="step">
      <StepHead open summary="optional" onClick={() => commands.toggleStep("anchor")}/>
      {keyed ? <div class="anchor-slot keyed"><Dot active/><span class="anchor-name">{keyed.name}</span><button class="chip-btn" onClick={commands.clearAnchor}>CLR</button></div>
        : <div class="anchor-slot"><Dot/><span class="anchor-name faint">none — free-space scenes only</span></div>}
      <div class="step-sub"><span class="lbl">NEARBY</span><span class="step-tools">
        <button class="chip-btn" onClick={() => commands.scan("furniture")}>SCAN</button>
        <button class="chip-btn" onClick={() => commands.pick("furniture")}>PICK</button>
      </span></div>
      <div class="near-list">
        {furniture.length ? furniture.map((anchor) => <AnchorRow key={anchor.token} anchor={anchor} keyed={keyed} onToggle={() => commands.toggleAnchor(anchor.token)}/>)
          : <div class="empty-mini"><span class="mono">{markers.length ? "No visible furniture — try the AI markers below." : "Optional. Scan for usable furniture nearby."}</span></div>}
        {!!markers.length && <button class={`reveal ${markersOpen ? "on" : ""}`} onClick={commands.toggleMarkers} title="Invisible AI idle spots — usable like furniture, but invisible in the world">
          {markersOpen ? "▾" : "▸"} {markers.length} AI marker{markers.length === 1 ? "" : "s"} (invisible)
        </button>}
        {markersOpen && markers.map((anchor) => <AnchorRow key={anchor.token} anchor={anchor} keyed={keyed} onToggle={() => commands.toggleAnchor(anchor.token)}/>)}
      </div>
      {keyed && <div class="step-foot"><span class="mono"><MatchLabel state={state}/></span></div>}
    </div>
  );
}

