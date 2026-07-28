import { useState } from "preact/hooks";
import type { DevCommands, WheelDebugState } from "./debug";

export function DevTools({ commands }: { commands: DevCommands }) {
  const [wheel, setWheel] = useState<WheelDebugState>({ count: 12, pins: false, target: true, error: false, loading: false });
  const [wheelActive, setWheelActive] = useState(false);
  const updateWheel = (next: WheelDebugState, active = true) => { setWheel(next); setWheelActive(active); commands.wheel(active ? next : null); };
  return <div class="wheeldbg">
    <span class="wheeldbg-title">WHEEL DEBUG</span>
    <button title="Fewer slices" onClick={() => updateWheel({ ...wheel, count: Math.max(0, wheel.count - 1) })}>−</button>
    <span class="wheeldbg-n">{wheelActive ? wheel.count : "live"}</span>
    <button title="More candidates (wheel remains capped at 12)" onClick={() => updateWheel({ ...wheel, count: Math.min(24, wheel.count + 1) })}>+</button>
    <button class={wheel.pins ? "on" : ""} onClick={() => updateWheel({ ...wheel, pins: !wheel.pins })}>PINS×3</button>
    <button class={wheel.target ? "on" : ""} onClick={() => updateWheel({ ...wheel, target: !wheel.target })}>TARGET</button>
    <button class={wheel.error ? "on" : ""} onClick={() => updateWheel({ ...wheel, error: !wheel.error })}>ERROR</button>
    <button class={wheel.loading ? "on" : ""} onClick={() => updateWheel({ ...wheel, loading: !wheel.loading })}>LOADING</button>
    <button onClick={() => updateWheel({ count: 12, pins: false, target: true, error: false, loading: false }, false)}>RESET</button>
  </div>;
}
