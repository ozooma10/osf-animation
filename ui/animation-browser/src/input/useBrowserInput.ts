import { useEffect } from "preact/hooks";
import type { BrowserCommands } from "../app/commands";
import type { BrowserState } from "../app/state";

const FOCUSABLE = "button, a[href], select, input, [tabindex]";

function visible(element: Element): element is HTMLElement {
  if (!(element instanceof HTMLElement) || element.hidden || ("disabled" in element && element.disabled)) return false;
  if (element.getAttribute("tabindex") === "-1" || !element.getClientRects().length) return false;
  const bounds = element.getBoundingClientRect();
  return bounds.width >= 1 && bounds.height >= 1 && bounds.bottom > 0 && bounds.right > 0
    && bounds.top < innerHeight && bounds.left < innerWidth;
}

function center(element: Element) {
  const bounds = element.getBoundingClientRect();
  return { x: bounds.left + bounds.width / 2, y: bounds.top + bounds.height / 2 };
}

function focusInDirection(direction: "up" | "down" | "left" | "right"): void {
  const items = [...document.querySelectorAll(FOCUSABLE)].filter(visible);
  if (!items.length) return;
  const current = document.activeElement;
  if (!current || current === document.body || !items.includes(current as HTMLElement)) {
    const preferred = document.querySelector(".libx-row.selected") || document.querySelector(".libx-row") || items[0];
    if (visible(preferred)) preferred.focus({ preventScroll: true });
    return;
  }
  const from = center(current);
  let best: HTMLElement | null = null;
  let score = Number.POSITIVE_INFINITY;
  for (const item of items) {
    if (item === current) continue;
    const target = center(item);
    const dx = target.x - from.x;
    const dy = target.y - from.y;
    let along = 0;
    let cross = 0;
    if (direction === "up") { if (dy > -1) continue; along = -dy; cross = Math.abs(dx); }
    else if (direction === "down") { if (dy < 1) continue; along = dy; cross = Math.abs(dx); }
    else if (direction === "left") { if (dx > -1) continue; along = -dx; cross = Math.abs(dy); }
    else { if (dx < 1) continue; along = dx; cross = Math.abs(dy); }
    const candidate = along + cross * 2.2;
    if (candidate < score) { score = candidate; best = item; }
  }
  best?.focus({ preventScroll: true });
  best?.scrollIntoView({ block: "nearest", inline: "nearest" });
}

function isTextInput(element: Element | null): element is HTMLInputElement {
  return element instanceof HTMLInputElement && /^(?:text|search|url|email|tel|number|password|)$/.test(element.type);
}

export function useBrowserInput(state: BrowserState, commands: BrowserCommands, standalone: boolean): void {
  useEffect(() => {
    const keydown = (event: KeyboardEvent) => {
      if (event.altKey || event.ctrlKey || event.metaKey) return;
      const active = document.activeElement;
      if (state.wheel) {
        if (event.key === "Escape") { event.preventDefault(); commands.cancelWheel(); return; }
        const direction = ({ ArrowUp: -1, ArrowLeft: -1, ArrowDown: 1, ArrowRight: 1 } as Record<string, number>)[event.key];
        if (direction) { event.preventDefault(); document.body.classList.add("nav-kb"); const count = state.wheel.entries.length; if (count) commands.focusWheel((state.wheel.focus + direction + count) % count); return; }
        if (event.key === "Enter" || event.key === " " || event.key === "Spacebar") { event.preventDefault(); commands.pickWheel(state.wheel.focus); }
        return;
      }
      if (standalone && !isTextInput(active) && (event.key === "w" || event.key === "W")) {
        window.mockOpenWheel?.(event.key === "w");
        return;
      }
      const direction = ({ ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" } as Record<string, "up" | "down" | "left" | "right">)[event.key];
      if (direction) {
        document.body.classList.add("nav-kb");
        if ((direction === "left" || direction === "right") && active instanceof HTMLInputElement && active.type === "range") return;
        if ((direction === "left" || direction === "right") && active instanceof HTMLSelectElement) return;
        if ((direction === "left" || direction === "right") && isTextInput(active)) return;
        event.preventDefault();
        focusInDirection(direction);
        return;
      }
      if ((event.key === " " || event.key === "Spacebar") && !isTextInput(active) && (state.lastHandle || state.active?.length === 1)) {
        event.preventDefault(); commands.advance(); return;
      }
      if (event.key === "Tab") document.body.classList.add("nav-kb");
      if (event.key === "Escape") commands.requestClose();
    };
    const pointer = () => document.body.classList.remove("nav-kb");
    const context = (event: MouseEvent) => { if (state.wheel) { event.preventDefault(); commands.cancelWheel(); } };
    document.addEventListener("keydown", keydown);
    document.addEventListener("mousedown", pointer);
    document.addEventListener("mousemove", pointer);
    document.addEventListener("contextmenu", context);
    return () => {
      document.removeEventListener("keydown", keydown);
      document.removeEventListener("mousedown", pointer);
      document.removeEventListener("mousemove", pointer);
      document.removeEventListener("contextmenu", context);
    };
  }, [commands, standalone, state.active, state.lastHandle, state.wheel]);

  useEffect(() => {
    const orbit = { dragging: false, x: 0, y: 0, dx: 0, dy: 0, wheel: 0, frame: 0 };
    const worldTarget = (target: EventTarget | null) => !state.wheel && !(target instanceof Element && target.closest(".console, .brief, .livebar"));
    const flush = () => {
      orbit.frame = 0;
      if (orbit.dx || orbit.dy || orbit.wheel) commands.orbit(orbit.dx, orbit.dy, orbit.wheel);
      orbit.dx = orbit.dy = orbit.wheel = 0;
    };
    const queue = () => { if (!orbit.frame) orbit.frame = requestAnimationFrame(flush); };
    const down = (event: MouseEvent) => { if (event.button === 0 && worldTarget(event.target)) { orbit.dragging = true; orbit.x = event.clientX; orbit.y = event.clientY; } };
    const move = (event: MouseEvent) => { if (!orbit.dragging) return; orbit.dx += event.clientX - orbit.x; orbit.dy += event.clientY - orbit.y; orbit.x = event.clientX; orbit.y = event.clientY; queue(); };
    const up = (event: MouseEvent) => { if (event.button === 0) orbit.dragging = false; };
    const wheel = (event: WheelEvent) => { if (!worldTarget(event.target)) return; event.preventDefault(); orbit.wheel += Math.sign(event.deltaY); queue(); };
    document.addEventListener("mousedown", down);
    document.addEventListener("mousemove", move);
    document.addEventListener("mouseup", up);
    document.addEventListener("wheel", wheel, { passive: false });
    return () => {
      document.removeEventListener("mousedown", down);
      document.removeEventListener("mousemove", move);
      document.removeEventListener("mouseup", up);
      document.removeEventListener("wheel", wheel);
      if (orbit.frame) cancelAnimationFrame(orbit.frame);
    };
  }, [commands, state.visibilitySerial, state.wheel]);
}
