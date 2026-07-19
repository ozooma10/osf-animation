import type { Plugin } from "vite";

function frameHtml(width: number, height: number, query: string): string {
  return `<!doctype html>
<html><head><meta charset="utf-8"><title>OSF view — ${width}×${height}</title>
<style>
  html,body{margin:0;height:100%;background:#000;overflow:hidden}
  iframe{position:absolute;left:0;top:0;width:${width}px;height:${height}px;border:0;background:#000;transform-origin:0 0}
  #hud{position:fixed;left:12px;bottom:8px;color:#6d7880;font:12px Consolas,monospace;z-index:2;opacity:.8;transition:opacity .6s;pointer-events:none}
</style></head><body>
<iframe src="./${query}"></iframe>
<div id="hud">${width}×${height} in-game viewport, stretched to fill · S = toggle stretch / 1:1</div>
<script>
  const frame=document.querySelector("iframe");let stretch=true;
  function apply(){frame.style.transform=stretch?"scale("+(innerWidth/${width})+","+(innerHeight/${height})+")":"none";document.body.style.overflow=stretch?"hidden":"auto"}
  addEventListener("resize",apply);addEventListener("keydown",e=>{if(e.key==="s"||e.key==="S"){stretch=!stretch;apply()}});
  setTimeout(()=>document.getElementById("hud").style.opacity="0",4000);apply();
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