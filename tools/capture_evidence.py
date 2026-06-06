#!/usr/bin/env python3
"""
tools/capture_evidence.py

Headless evidence capture: boots two emulator instances, waits for ghosts
to appear (Bug 1 fix verification), then saves screenshots to test/evidence/.

Run after a fix commit to generate proof screenshots:
    python3 tools/capture_evidence.py test/evidence/<fix-dir>/

The script is self-contained — no MCP connection needed.
"""

import sys
import os
import shutil
import tempfile
import time
import threading
import argparse
import pathlib

# Allow importing the Instance class and helpers from the MCP server.
TOOLS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(TOOLS_DIR / "mcp_gamestate"))
import server as _s

REPO_ROOT = pathlib.Path(__file__).parent.parent


def _start(iid: str, savestate: str) -> _s.Instance:
    result = _s.start_emulator(iid, savestate=savestate)
    if "not found" in result.lower() or "error" in result.lower():
        print(f"[{iid}] {result}", file=sys.stderr)
        sys.exit(1)
    print(f"[{iid}] started")
    return _s._instances[iid]


def _screenshot(iid: str, dest: pathlib.Path) -> None:
    path = f"/tmp/evidence_screen_{iid}.png"
    r = _s._inst(iid).send_locked({"cmd": "screenshot", "path": path})
    if not r.get("ok"):
        print(f"[{iid}] screenshot failed: {r}", file=sys.stderr)
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(path, dest)
    print(f"[{iid}] saved {dest}")


def _wait_frames(iid: str, n: int) -> None:
    _s._inst(iid).send_locked({"cmd": "wait", "frames": n})


def main() -> None:
    ap = argparse.ArgumentParser(description="Capture fix-evidence screenshots")
    ap.add_argument("outdir", nargs="?",
                    default="test/evidence/latest",
                    help="Directory to write screenshots into (default: test/evidence/latest)")
    ap.add_argument("--p1-state", default="test/lua/states/p1_route1.ss1")
    ap.add_argument("--p2-state", default="test/lua/states/p2_route1.ss1")
    ap.add_argument("--wait", type=int, default=300,
                    help="Frames to wait before screenshot (default 300 = 5s)")
    args = ap.parse_args()

    out = pathlib.Path(args.outdir)
    if not out.is_absolute():
        out = REPO_ROOT / out

    print("Starting emulators...")
    _start("p1", args.p1_state)
    _start("p2", args.p2_state)

    # Start the relay thread (from server.py global).
    relay_thread = threading.Thread(target=_s._relay_loop, daemon=True)
    relay_thread.start()
    print("Relay started")

    print(f"Waiting {args.wait} frames ({args.wait/60:.1f}s) for connection + ghosts...")
    _wait_frames("p1", args.wait)

    _screenshot("p1", out / "p1_after.png")
    _screenshot("p2", out / "p2_after.png")

    print("Done. Stopping emulators.")
    _s.stop_emulator("p1")
    _s.stop_emulator("p2")


if __name__ == "__main__":
    main()
