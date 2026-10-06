import { useEffect } from "octane";

export function useDevBackdrop(enabled: boolean): void {
  useEffect(() => {
    if (!enabled) return;
    const backdrop = document.createElement("div");
    backdrop.id = "devbackdrop";
    document.body.prepend(backdrop);
    const backdropUrl = new URL("../../fixtures/live/backdrop.jpg", import.meta.url).href;
    const scenes = ["interior", "day", "night", "none"];
    const apply = (scene: string) => {
      backdrop.dataset.scene = scene;
      backdrop.style.backgroundImage = scene === "shot" ? `url(${JSON.stringify(backdropUrl)})` : "";
      try { sessionStorage.setItem("osfDevBackdrop", scene); } catch { /* preview storage may be unavailable */ }
    };
    let saved: string | null = null;
    try { saved = sessionStorage.getItem("osfDevBackdrop"); } catch { /* ignored */ }
    apply(saved && scenes.includes(saved) ? saved : "interior");
    const image = new Image();
    image.onload = () => { scenes.unshift("shot"); if (!saved || saved === "shot") apply("shot"); };
    image.src = backdropUrl;
    const keydown = (event: KeyboardEvent) => {
      if ((event.key !== "b" && event.key !== "B") || document.activeElement instanceof HTMLInputElement) return;
      apply(scenes[(scenes.indexOf(backdrop.dataset.scene || "") + 1) % scenes.length]);
    };
    document.addEventListener("keydown", keydown);
    return () => { document.removeEventListener("keydown", keydown); backdrop.remove(); };
  }, [enabled]);
}

