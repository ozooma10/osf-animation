#!/usr/bin/env python3
"""Start the Animation Browser's Vite development server.

This compatibility wrapper preserves the existing editor launch configurations.
Vite owns source transforms, the /live fixture route, and the /frame 1600x900
Ultralight-layout harness.

Usage: python tools/view-dev-server.py [port]   (default 8791)
"""

from pathlib import Path
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parent.parent
UI_DIR = REPO / "ui" / "animation-browser"


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "8791"
    npm = shutil.which("npm.cmd" if sys.platform == "win32" else "npm")
    if not npm:
        print("error: npm was not found on PATH", file=sys.stderr)
        return 1
    if not (UI_DIR / "node_modules").is_dir():
        print(f"error: dependencies are missing; run `npm install` in {UI_DIR}", file=sys.stderr)
        return 1
    print(f"source: {UI_DIR}")
    print(f"open:   http://127.0.0.1:{port}/")
    print(f"frame:  http://127.0.0.1:{port}/frame")
    return subprocess.call(
        [npm, "run", "dev", "--", "--port", port],
        cwd=UI_DIR,
    )


if __name__ == "__main__":
    raise SystemExit(main())