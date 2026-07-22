import { useEffect } from "preact/hooks";

export function useDevBackdrop(enabled: boolean): void {
  useEffect(() => {
    if (!enabled) return;
    const backdrop = document.createElement("div");
    backdrop.id = "devbackdrop";
    document.body.prepend(backdrop);
    const scenes = ["interior", "day", "night", "none"];
    const apply = (scene: string) => {
      backdrop.dataset.scene = scene;
      backdrop.style.backgroundImage = scene === "shot" ? "url(live/backdrop.jpg)" : "";
      try { sessionStorage.setItem("osfDevBackdrop", scene); } catch { /* storage may be unavailable in Ultralight */ }
    };
    let saved: string | null = null;
    try { saved = sessionStorage.getItem("osfDevBackdrop"); } catch { /* ignored */ }
    apply(saved && scenes.includes(saved) ? saved : "interior");
    const image = new Image();
    image.onload = () => { scenes.unshift("shot"); if (!saved || saved === "shot") apply("shot"); };
    image.src = "live/backdrop.jpg";
    const keydown = (event: KeyboardEvent) => {
      if ((event.key !== "b" && event.key !== "B") || document.activeElement instanceof HTMLInputElement) return;
      apply(scenes[(scenes.indexOf(backdrop.dataset.scene || "") + 1) % scenes.length]);
    };
    document.addEventListener("keydown", keydown);
    return () => { document.removeEventListener("keydown", keydown); backdrop.remove(); };
  }, [enabled]);
}

