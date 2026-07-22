import type { ComponentChildren } from "preact";
import { castHasCreature, castSpecies, speciesLabel } from "../../app/selectors";
import type { BrowserState } from "../../app/state";

export function Empty({ children }: { children: ComponentChildren }) {
  return <div class="bay-empty"><span class="mono">{children}</span></div>;
}

export function Dot({ active = false }: { active?: boolean }) {
  return <span class={`dot ${active ? "go" : ""}`}/>;
}

export function SpeciesFilter({ state, onToggle }: { state: BrowserState; onToggle(): void }) {
  if (!castHasCreature(state) && !state.allSpecies) return null;
  const label = state.allSpecies
    ? "ALL SPECIES"
    : `${[...castSpecies(state)].map(speciesLabel).join(" + ").toUpperCase()} ONLY`;
  return (
    <div class="browse-note species">
      <Dot active={!state.allSpecies}/><span class="lbl">{label}</span>
      <button class={`reveal inline ${state.allSpecies ? "on" : ""}`} onClick={onToggle}>
        {state.allSpecies ? "match cast" : "show all species"}
      </button>
    </div>
  );
}

export function MoveButtons({ index, count, onMove }: {
  index: number;
  count: number;
  onMove(direction: -1 | 1): void;
}) {
  if (count < 2) return null;
  return (
    <span class="chip-moves">
      <button class="chip-move" disabled={index === 0} title="Move up (role earlier)" onClick={() => onMove(-1)}>▲</button>
      <button class="chip-move" disabled={index === count - 1} title="Move down (role later)" onClick={() => onMove(1)}>▼</button>
    </span>
  );
}

