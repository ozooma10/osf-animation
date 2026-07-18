#!/usr/bin/env python3
"""Standalone dev server for views/osf.animation/browser.

Serves the view folder for desktop-browser iteration, plus:
- /live/<name>.json -> <Documents>\\My Games\\Starfield\\OSF\\ui\\<name>.json — the
  catalog/library snapshots the OSF Animation DLL mirrors on every push, so the
  standalone page runs on the same live data the in-game view sees (the page
  falls back to its mock catalog when no snapshot exists).
- Cache-Control: no-store on everything, so edits and fresh in-game dumps show
  up on plain reload (no ?v= cache-busting needed).
- /frame — the view in a fixed in-game-sized viewport (default 1600x900, the
  resolution Ultralight renders the overlay at), auto-scaled to fit the window
  (S toggles 1:1). Override with /frame?w=1920&h=1080.

Usage: python tools/view-dev-server.py [port]   (default 8791)
"""

import ctypes
import ctypes.wintypes as wt
import os
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VIEW_DIR = os.path.join(REPO, "views", "osf.animation", "browser")


def documents_dir():
    """The user's real Documents folder (honors relocation/OneDrive), with a plain fallback."""
    try:
        class GUID(ctypes.Structure):
            _fields_ = [("d1", wt.DWORD), ("d2", wt.WORD), ("d3", wt.WORD), ("d4", ctypes.c_ubyte * 8)]

        documents = GUID(0xFDD39AD0, 0x238F, 0x46AF,
                         (ctypes.c_ubyte * 8)(0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7))
        out = ctypes.c_wchar_p()
        if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(documents), 0, None, ctypes.byref(out)) == 0:
            try:
                return out.value
            finally:
                ctypes.windll.ole32.CoTaskMemFree(out)
    except Exception:
        pass
    return os.path.join(os.path.expanduser("~"), "Documents")


LIVE_DIR = os.path.join(documents_dir(), "My Games", "Starfield", "OSF", "ui")


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

    def translate_path(self, path):
        clean = path.split("?", 1)[0].split("#", 1)[0]
        if clean.startswith("/live/"):
            # basename() flattens any traversal; only files directly in LIVE_DIR are reachable.
            return os.path.join(LIVE_DIR, os.path.basename(clean[len("/live/"):]))
        return super().translate_path(path)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8791
    handler = partial(Handler, directory=VIEW_DIR)
    print(f"view:  {VIEW_DIR}")
    print(f"live:  {LIVE_DIR}  ({'found' if os.path.isdir(LIVE_DIR) else 'no dump yet - mock fallback'})")
    print(f"open:  http://localhost:{port}/")
    print(f"       http://localhost:{port}/frame  (fixed 1600x900 in-game viewport; ?w=&h= to override)")
    ThreadingHTTPServer(("127.0.0.1", port), handler).serve_forever()


if __name__ == "__main__":
    main()
