#!/usr/bin/env python3
"""Two-instance co-op test orchestrator.

Boots two headless mGBA processes against the same ROM, each running a
scenario-specific Lua driver, and acts as the relay between their ring
buffers (in-process bridge) — or, with --bridge=partykit, defers to a
real PartyKit dev server (Layer 4).

Each scenario lives at test/lua/coop/<name>/ with p1.lua and p2.lua.
The drivers each load test/lua/coop/_driver.lua which provides:
  - bridge ticks (drain sendRing → outbox file; pull inbox file → recvRing)
  - assertion + result reporting
  - simple input helpers (walkRight/etc.) layered on emu:setKeys

Workdir layout (under $TMPDIR/coop_<scenario>_<rand>/):
  p1/
    inbox.bin    bytes the orchestrator pushed; Lua reads + truncates
    outbox.bin   bytes Lua appended; orchestrator reads + truncates
    result.txt   final RESULT: PASS|FAIL line written at end-of-test
  p2/  (mirror)

Usage:
  tools/coop_harness/coop_orchestrator.py [--bridge=inproc|partykit] \\
      [--rom PATH] [--timeout SECS] SCENARIO_DIR

Where SCENARIO_DIR is a directory containing p1.lua and p2.lua.
"""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DRIVER = REPO_ROOT / "test" / "lua" / "coop" / "_driver.lua"


def find_mgba() -> str:
    # Explicit override via environment variable (used by CI and local setups).
    env_mgba = os.environ.get("MGBA", "")
    if env_mgba and os.path.isfile(env_mgba) and os.access(env_mgba, os.X_OK):
        return env_mgba
    # Standard PATH names.
    for binary in ("mgba-headless", "mgba-qt", "mgba"):
        path = shutil.which(binary)
        if path:
            return path
    # CI headless build path (populated by the build-mgba workflow step).
    ci_headless = "/tmp/mgba-build/mgba-headless"
    if os.path.isfile(ci_headless) and os.access(ci_headless, os.X_OK):
        return ci_headless
    # macOS app bundle fallback.
    macos_bundle = "/Applications/mGBA.app/Contents/MacOS/mGBA"
    if os.path.isfile(macos_bundle) and os.access(macos_bundle, os.X_OK):
        return macos_bundle
    sys.exit("error: mGBA not found (tried PATH, /tmp/mgba-build, and /Applications/mGBA.app)")


def needs_display(mgba: str) -> bool:
    """Return False for headless mGBA builds — they don't need a virtual display."""
    return "headless" not in os.path.basename(mgba).lower()


def find_xvfb() -> str | None:
    """Returns xvfb-run path, or None on macOS/non-Linux where it isn't needed."""
    return shutil.which("xvfb-run")


def spawn_instance(
    *,
    mgba: str,
    xvfb: str | None,
    rom: Path,
    workdir: Path,
    instance_id: str,
    scenario_dir: Path,
) -> subprocess.Popen:
    """Boot one mGBA process running its half of the scenario."""
    inst_dir = workdir / instance_id
    inst_dir.mkdir(parents=True, exist_ok=True)
    (inst_dir / "inbox.bin").write_bytes(b"")
    (inst_dir / "outbox.bin").write_bytes(b"")

    script = scenario_dir / f"{instance_id}.lua"
    if not script.exists():
        sys.exit(f"error: missing {script}")

    env = os.environ.copy()
    # Drivers find their per-instance directory + repo paths via env.
    env["COOP_INSTANCE_DIR"] = str(inst_dir)
    env["COOP_INSTANCE_ID"] = instance_id
    env["LUA_PATH"] = (
        f"{REPO_ROOT}/test/lua/?.lua;"
        f"{REPO_ROOT}/test/lua/coop/?.lua;"
        f"{REPO_ROOT}/test/lua/?/init.lua;;"
    )
    env["LUA_TEST_RESULTS"] = str(inst_dir / "result.txt")

    log_path = inst_dir / "mgba.log"
    log = open(log_path, "w")
    # Headless mGBA is built without SDL/Qt and needs no virtual display.
    # Using xvfb-run with two simultaneous instances causes a display-number
    # race under xvfb-run -a, making both processes fail immediately.
    use_xvfb = xvfb and needs_display(mgba)
    if use_xvfb:
        cmd = [xvfb, "-a", mgba, "-S", str(script), str(rom)]
    else:
        cmd = [mgba, "-S", str(script), str(rom)]
    print(f"  spawning {instance_id}: {' '.join(cmd)}")
    return subprocess.Popen(
        cmd,
        env=env,
        stdout=log,
        stderr=log,
        preexec_fn=os.setsid,
    )


def dump_log(workdir: Path, instance_id: str, *, tail: int = 30) -> None:
    """Print the last `tail` lines of an instance's mGBA log."""
    log_path = workdir / instance_id / "mgba.log"
    try:
        lines = log_path.read_text().splitlines()
    except OSError:
        print(f"  [{instance_id}] (no log file)")
        return
    if not lines:
        print(f"  [{instance_id}] (log is empty)")
        return
    snippet = lines[-tail:]
    print(f"  [{instance_id}] --- last {len(snippet)} lines of mgba.log ---")
    for line in snippet:
        print(f"  [{instance_id}] {line}")


def bridge_tick(workdir: Path) -> None:
    """In-process bridge: copy p1.outbox → p2.inbox and vice versa.

    Read-then-truncate in one open() per box. The Lua driver tolerates
    empty/missing inbox files (treats as no bytes this tick).
    """
    for sender, receiver in (("p1", "p2"), ("p2", "p1")):
        outbox = workdir / sender / "outbox.bin"
        inbox = workdir / receiver / "inbox.bin"
        if not outbox.exists():
            continue
        try:
            data = outbox.read_bytes()
        except OSError:
            continue
        if not data:
            continue
        # Truncate first so a slow Lua reader doesn't double-consume.
        outbox.write_bytes(b"")
        # Append to receiver's inbox so any partially-consumed bytes survive.
        with open(inbox, "ab") as f:
            f.write(data)


def both_finished(workdir: Path) -> bool:
    return (workdir / "p1" / "result.txt").exists() and \
           (workdir / "p2" / "result.txt").exists()


def read_results(workdir: Path) -> tuple[str, str]:
    return (
        (workdir / "p1" / "result.txt").read_text().strip(),
        (workdir / "p2" / "result.txt").read_text().strip(),
    )


def kill_tree(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario_dir", type=Path,
                        help="Path containing p1.lua and p2.lua")
    parser.add_argument("--rom", type=Path, default=REPO_ROOT / "pokefirered.gba")
    parser.add_argument("--bridge", choices=["inproc", "partykit"], default="inproc")
    parser.add_argument("--timeout", type=int, default=120,
                        help="Hard wallclock timeout per scenario (seconds)")
    args = parser.parse_args()

    if args.bridge == "partykit":
        sys.exit("error: --bridge=partykit is provided by "
                 "tools/coop_harness/relay_smoke.py (Layer 4 — relay-only "
                 "JSON protocol smoke). Bridging two ROM instances through "
                 "the real relay also needs a Python binary↔JSON translator "
                 "for the ring buffer protocol; that translator is a "
                 "follow-up to Layer 4.")

    if not args.rom.exists():
        sys.exit(f"error: ROM not found at {args.rom} — run 'make firered' first")
    if not DRIVER.exists():
        sys.exit(f"error: driver missing at {DRIVER}")
    if not (args.scenario_dir / "p1.lua").exists() or \
       not (args.scenario_dir / "p2.lua").exists():
        sys.exit(f"error: scenario {args.scenario_dir} must contain p1.lua and p2.lua")

    mgba = find_mgba()
    xvfb = find_xvfb()  # None on macOS
    workdir = Path(tempfile.mkdtemp(prefix=f"coop_{args.scenario_dir.name}_"))
    print(f"==> Workdir: {workdir}")

    procs: list[subprocess.Popen] = []
    try:
        for inst in ("p1", "p2"):
            procs.append(spawn_instance(
                mgba=mgba, xvfb=xvfb, rom=args.rom,
                workdir=workdir, instance_id=inst,
                scenario_dir=args.scenario_dir,
            ))

        deadline = time.monotonic() + args.timeout
        reported_early_exit: set[int] = set()
        while time.monotonic() < deadline:
            bridge_tick(workdir)
            if both_finished(workdir):
                break
            all_dead = True
            for p, inst in zip(procs, ("p1", "p2")):
                if p.poll() is not None:
                    if p.pid not in reported_early_exit:
                        reported_early_exit.add(p.pid)
                        print(f"  WARN: {inst} exited early (pid {p.pid}, "
                              f"rc {p.returncode})")
                        dump_log(workdir, inst)
                else:
                    all_dead = False
            # If every instance has already exited and no results appeared
            # yet, give them a brief grace period then stop spinning.
            if all_dead and not both_finished(workdir):
                time.sleep(1.0)
                bridge_tick(workdir)
                if not both_finished(workdir):
                    print("  all instances exited without writing results — aborting")
                    break
            time.sleep(0.05)
        else:
            print(f"  TIMEOUT after {args.timeout}s")
            for p, inst in zip(procs, ("p1", "p2")):
                if not (workdir / inst / "result.txt").exists():
                    dump_log(workdir, inst)

        # Drain one last time so any final outbox flush makes it across.
        bridge_tick(workdir)

        if not both_finished(workdir):
            print("  FAIL: one or both instances never wrote a result")
            return 1

        r1, r2 = read_results(workdir)
        print(f"  p1: {r1}")
        print(f"  p2: {r2}")
        if r1.startswith("RESULT: PASS") and r2.startswith("RESULT: PASS"):
            return 0
        return 1
    finally:
        for p in procs:
            kill_tree(p)
        # Leave workdir for inspection on failure; harmless on success.


if __name__ == "__main__":
    sys.exit(main())
