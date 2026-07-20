import type { Plugin } from "vite";

// The game-true reference frame, mirroring OSF UI's harness Stage.tsx.
//
// The view is laid out at exactly width×height and then UNIFORMLY scaled to fit
// the window, letterboxed — never stretched per-axis. A non-uniform stretch
// would make every proportion on screen a lie the moment the window is not 16:9,
// which is precisely when you are trying to judge a layout. There is no upscale
// cap: filling a 1080p window at 1.2x is the real in-game text size, and clamping
// to 1:1 would render everything smaller here than in game.
//
// 1600x900 is the reference composition, not the only in-game resolution — the
// game resizes the page to the output aspect, so the view still has to survive
// reflow. `S` drops to 1:1 for that kind of inspection.
function frameHtml(width: number, height: number, query: string): string {
  return `<!doctype html>
<html><head><meta charset="utf-8"><title>OSF view — ${width}×${height}</title>
<style>
  html,body{margin:0;height:100%;background:#05070b;overflow:hidden}
  iframe{position:absolute;left:0;top:0;width:${width}px;height:${height}px;border:0;
    background:#05070b;transform-origin:0 0;outline:1px dashed #2b333d}
  #hud{position:fixed;left:12px;bottom:8px;color:#6d7880;font:12px Consolas,monospace;
    z-index:2;opacity:.8;transition:opacity .6s;pointer-events:none}
</style></head><body>
<iframe src="./${query}"></iframe>
<div id="hud">${width}×${height} reference stage, uniform fit · S = toggle fit / 1:1</div>
<script>
  const frame=document.querySelector("iframe"),hud=document.getElementById("hud");let fit=true;
  function apply(){
    if(fit){
      const s=Math.min(innerWidth/${width},innerHeight/${height});
      frame.style.transform="scale("+s+")";
      frame.style.left=Math.max(0,(innerWidth-${width}*s)/2)+"px";
      frame.style.top=Math.max(0,(innerHeight-${height}*s)/2)+"px";
      hud.textContent="${width}×${height} reference stage · "+(Math.round(s*100))+"% · S = toggle fit / 1:1";
    }else{
      frame.style.transform="none";frame.style.left="0";frame.style.top="0";
      hud.textContent="${width}×${height} at 1:1 — scroll to inspect · S = toggle fit / 1:1";
    }
    document.body.style.overflow=fit?"hidden":"auto";
  }
  addEventListener("resize",apply);
  addEventListener("keydown",e=>{if(e.key==="s"||e.key==="S"){fit=!fit;apply();hud.style.opacity=".8";setTimeout(()=>hud.style.opacity="0",4000)}});
  setTimeout(()=>hud.style.opacity="0",4000);apply();
</script></body></html>`;
}

export function frameHarness(): Plugin {
  return {
    name: "osf-frame-harness",
    configureServer(server) {
      server.middlewares.use((request, response, next) => {
        const url = new URL(request.url || "/", "http://osf.local");
        if (url.pathname.replace(/\/$/, "") !== "/frame") return next();
        const width = Math.max(320, Number.parseInt(url.searchParams.get("w") || "1600", 10) || 1600);
        const height = Math.max(180, Number.parseInt(url.searchParams.get("h") || "900", 10) || 900);
        url.searchParams.delete("w");
        url.searchParams.delete("h");
        const query = url.searchParams.size ? `?${url.searchParams.toString()}` : "";
        response.statusCode = 200;
        response.setHeader("Content-Type", "text/html; charset=utf-8");
        response.setHeader("Cache-Control", "no-store");
        response.end(frameHtml(width, height, query));
      });
    },
  };
}