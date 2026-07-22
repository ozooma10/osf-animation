import { useEffect, useRef } from "preact/hooks";
import type { BrowserCommands } from "../../app/commands";
import { wheelGeometry } from "../../app/selectors";
import type { BrowserState } from "../../app/state";

export function AnimationWheel({ state, commands }: { state: BrowserState; commands: BrowserCommands }) {
  const focused = useRef<HTMLButtonElement>(null);
  const wheel = state.wheel;
  useEffect(() => focused.current?.focus({ preventScroll: true }), [wheel?.focus, wheel?.entries.length]);
  if (!wheel) return null;
  const entries = wheel.entries;
  const focus = Math.min(wheel.focus, Math.max(0, entries.length - 1));
  const { rx, ry } = wheelGeometry(entries.length);
  return <>
    <div class="wheel-ring" style={{ "--wrx": `${rx}px`, "--wry": `${ry}px` }}>
      {!wheel.received ? <div class="wheel-empty"><span class="mono">Loading animation wheel…</span><button class="chip-btn" onClick={commands.cancelWheel}>CLOSE</button></div>
        : !entries.length ? <div class="wheel-empty"><span class="mono">{state.wheelCustomized ? "Your animation wheel is empty. Reset it from the Animation Browser to restore installed defaults." : `No default animations carry a ${wheel.tagPrefix}* tag.`}</span><button class="chip-btn" onClick={commands.cancelWheel}>CLOSE</button></div>
        : <><div class="wheel-dial"/>{entries.map((entry, index) => {
          const radians = (-90 + 360 / entries.length * index) * Math.PI / 180;
          const transform = `translate(-50%,-50%) translate(${Math.round(Math.cos(radians) * rx)}px,${Math.round(Math.sin(radians) * ry)}px)`;
          return <button ref={index === focus ? focused : undefined} key={entry.key} class={`wheel-slice ${index === focus ? "focused" : ""}`} style={{ transform }} title={entry.detail || entry.title} onMouseOver={() => commands.focusWheel(index)} onClick={() => commands.pickWheel(index)}>{wheel.launching === entry.key ? "▶ " : ""}{entry.title}</button>;
        })}<button class="wheel-hub" title="Close (Esc)" onClick={commands.cancelWheel}><span class="wheel-hub-who">{wheel.target ? `→ ${wheel.target.name}` : "You"}</span><span class={`wheel-hub-status ${wheel.error ? "err" : ""}`}>{wheel.error || (wheel.launching ? "launching…" : "click to close")}</span></button></>}
    </div>
    <div class="wheel-caption mono">ANIMATION WHEEL · ←→ SELECT · ENTER PLAY · ESC CLOSE</div>
  </>;
}

