#!/usr/bin/env python3
"""Standalone dev server for views/osf.animation/browser.

Serves the view folder for desktop-browser iteration, plus:
- /live/{catalog,library}.json — the committed snapshot fixtures in the view's
  live/ folder (served as plain static files), so the standalone page runs on
  real catalog data instead of the mock. library.json is regenerated offline by
  tools/generate-library-snapshot.py; catalog.json is a one-time in-game dump.
- Cache-Control: no-store on everything, so edits show up on plain reload
  (no ?v= cache-busting needed).
- /frame — the view in a fixed in-game-sized viewport (default 1600x900, the
  resolution Ultralight renders the overlay at), auto-scaled to fit the window
  (S toggles 1:1). Override with /frame?w=1920&h=1080.

Usage: python tools/view-dev-server.py [port]   (default 8791)
"""

import os
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VIEW_DIR = os.path.join(REPO, "views", "osf.animation", "browser")
LIVE_DIR = os.path.join(VIEW_DIR, "live")


# Fixed-viewport harness: the view laid out at the exact in-game overlay
# resolution, then STRETCHED to fill the window (independent X/Y scale — the
# same thing the OSF UI harness does with the Ultralight surface, so a
# non-16:9 window distorts rather than letterboxes). S toggles true 1:1.
FRAME_TEMPLATE = """<!doctype html>
<html><head><meta charset="utf-8"><title>OSF view — {W}×{H}</title>
<style>
  html, body {{ margin: 0; height: 100%; background: #000; overflow: hidden; }}
  iframe {{ position: absolute; left: 0; top: 0; width: {W}px; height: {H}px; border: 0; background: #000; transform-origin: 0 0; }}
  #hud {{ position: fixed; left: 12px; bottom: 8px; color: #6d7880; font: 12px Consolas, monospace; z-index: 2;
         opacity: .8; transition: opacity .6s; pointer-events: none; }}
</style></head><body>
<iframe src="/"></iframe>
<div id="hud">{W}×{H} in-game viewport, stretched to fill · S = toggle stretch / 1:1</div>
<script>
  const frame = document.querySelector("iframe");
  let stretch = true;
  function apply() {{
    frame.style.transform = stretch ? `scale(${{innerWidth / {W}}}, ${{innerHeight / {H}}})` : "none";
    document.body.style.overflow = stretch ? "hidden" : "auto";
  }}
  addEventListener("resize", apply);
  addEventListener("keydown", (e) => {{ if (e.key === "s" || e.key === "S") {{ stretch = !stretch; apply(); }} }});
  setTimeout(() => {{ document.getElementById("hud").style.opacity = "0"; }}, 4000);
  apply();
</script></body></html>
"""


class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path.rstrip("/") == "/frame":
            qs = parse_qs(parsed.query)
            try:
                w = int(qs.get("w", ["1600"])[0])
                h = int(qs.get("h", ["900"])[0])
            except ValueError:
                w, h = 1600, 900
            body = FRAME_TEMPLATE.format(W=w, H=h).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8791
    handler = partial(Handler, directory=VIEW_DIR)
    print(f"view:  {VIEW_DIR}")
    print(f"live:  {LIVE_DIR}  ({'found' if os.path.isdir(LIVE_DIR) else 'missing - mock fallback'})")
    print(f"open:  http://localhost:{port}/")
    print(f"       http://localhost:{port}/frame  (fixed 1600x900 in-game viewport; ?w=&h= to override)")
    ThreadingHTTPServer(("127.0.0.1", port), handler).serve_forever()


if __name__ == "__main__":
    main()
