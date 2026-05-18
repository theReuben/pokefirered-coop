#!/usr/bin/env python3
"""
tools/mcp_gamestate/server.py

MCP server that simulates a human player controlling one or two GBA
emulator instances running pokefirered.gba.

Exposed tools (player-level only — nothing a human couldn't do):
  start_emulator   — boot a game instance
  stop_emulator    — quit a game instance
  list_instances   — see what's running
  screenshot       — look at the screen (returns image)
  press_button     — press a GBA button
  wait             — do nothing for N frames

When both p1 and p2 are running, a background thread automatically
relays multiplayer packets between them so coop networking works
without any manual intervention.

Add to Claude Code:
  claude mcp add --scope project gamestate \\
    -- python3 tools/mcp_gamestate/server.py
"""

import base64
import json
import os
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from typing import Optional

from mcp.server.fastmcp import FastMCP, Image

# ── Paths ─────────────────────────────────────────────────────────────────

REPO_ROOT  = Path(__file__).parent.parent.parent
BRIDGE_LUA = Path(__file__).parent / "bridge.lua"
MEMORY_MAP = REPO_ROOT / "test" / "lua" / "memory_map.lua"
DEFAULT_ROM = REPO_ROOT / "pokefirered.gba"
MGBA_BIN   = Path(os.environ.get("MGBA", "/tmp/mgba-build/mgba-headless"))

CMD_TIMEOUT   = 30   # seconds to wait for any single command response
READY_TIMEOUT = 15   # seconds to wait for emulator to signal ready
RELAY_INTERVAL = 0.05  # seconds between relay polls (~3 frames at 60fps)

# GBA button name → key mask
KEY_MASKS = {
    "A":      0x001,
    "B":      0x002,
    "SELECT": 0x004,
    "START":  0x008,
    "RIGHT":  0x010,
    "LEFT":   0x020,
    "UP":     0x040,
    "DOWN":   0x080,
    "R":      0x100,
    "L":      0x200,
}

# ── Instance management ───────────────────────────────────────────────────

class Instance:
    def __init__(self, iid: str, work_dir: str):
        self.iid       = iid
        self.work_dir  = work_dir
        self.cmd_file  = os.path.join(work_dir, f"cmd_{iid}.json")
        self.resp_file = os.path.join(work_dir, f"resp_{iid}.json")
        self.ready_file = os.path.join(work_dir, f"ready_{iid}")
        self.process: Optional[subprocess.Popen] = None
        self.addrs: dict[str, int] = {}
        self._lock = threading.Lock()  # one command at a time

    def alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def send(self, cmd: dict, timeout: float = CMD_TIMEOUT) -> dict:
        if not self.alive():
            raise RuntimeError(f"Instance '{self.iid}' is not running")

        tmp = self.cmd_file + ".tmp"
        with open(tmp, "w") as f:
            json.dump(cmd, f)
        os.replace(tmp, self.cmd_file)

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if os.path.exists(self.resp_file):
                try:
                    with open(self.resp_file) as f:
                        raw = f.read().strip()
                    os.remove(self.resp_file)
                    return json.loads(raw)
                except (FileNotFoundError, json.JSONDecodeError):
                    pass
            if not self.alive():
                raise RuntimeError(f"Instance '{self.iid}' died while waiting for response")
            time.sleep(0.005)

        raise TimeoutError(
            f"No response from '{self.iid}' after {timeout}s (cmd={cmd.get('cmd')})"
        )

    def send_locked(self, cmd: dict, timeout: float = CMD_TIMEOUT) -> dict:
        """Thread-safe send — acquires the instance lock."""
        with self._lock:
            return self.send(cmd, timeout)

    def try_send_locked(self, cmd: dict, lock_timeout: float = 0.02,
                        cmd_timeout: float = 2.0) -> Optional[dict]:
        """Non-blocking attempt — returns None if lock is busy."""
        if self._lock.acquire(timeout=lock_timeout):
            try:
                return self.send(cmd, cmd_timeout)
            except Exception:
                return None
            finally:
                self._lock.release()
        return None


_instances: dict[str, Instance] = {}
_work_dir: Optional[tempfile.TemporaryDirectory] = None


def _get_work_dir() -> str:
    global _work_dir
    if _work_dir is None:
        _work_dir = tempfile.TemporaryDirectory(prefix="mgba_mcp_")
    return _work_dir.name


def _inst(iid: str) -> Instance:
    if iid not in _instances or not _instances[iid].alive():
        raise ValueError(f"No running instance '{iid}'. Call start_emulator first.")
    return _instances[iid]


def _parse_memory_map(path: Path) -> dict[str, int]:
    """Parse test/lua/memory_map.lua for symbol addresses."""
    addrs: dict[str, int] = {}
    if not path.exists():
        return addrs
    for line in path.read_text().splitlines():
        line = line.strip()
        if line.startswith("M.") and "=" in line:
            try:
                name = line.split("M.")[1].split("=")[0].strip()
                val  = line.split("=")[1].split("--")[0].strip()
                addrs[name] = int(val, 16) if val.startswith("0x") else int(val)
            except (ValueError, IndexError):
                pass
    return addrs

# ── Background relay thread ───────────────────────────────────────────────

# True once we've injected MP_PKT_PARTNER_CONNECTED into both instances for
# the current session. Reset when either instance goes away so a fresh boot
# gets the handshake again.
_connected_pair: bool = False


def _relay_loop() -> None:
    """Continuously relay multiplayer packets and coop battle blocks between p1 and p2."""
    global _connected_pair
    while True:
        time.sleep(RELAY_INTERVAL)
        try:
            p1 = _instances.get("p1")
            p2 = _instances.get("p2")
            both_alive = bool(p1 and p1.alive() and p2 and p2.alive())

            if not both_alive:
                _connected_pair = False
                continue

            # Inject MP_PKT_PARTNER_CONNECTED (0x0B) into both recv rings once
            # per session so each ROM auto-sets connState = MP_STATE_CONNECTED
            # without requiring an in-game handshake packet.
            if not _connected_pair:
                p1.try_send_locked({"cmd": "inject", "bytes": "0B"})
                p2.try_send_locked({"cmd": "inject", "bytes": "0B"})
                _connected_pair = True

            # Ring-buffer packet relay (overworld position, flags, etc.)
            # p1 → p2
            r = p1.try_send_locked({"cmd": "drain"})
            if r and r.get("ok") and r.get("count", 0) > 0:
                p2.try_send_locked({"cmd": "inject", "bytes": r["bytes"]})

            # p2 → p1
            r = p2.try_send_locked({"cmd": "drain"})
            if r and r.get("ok") and r.get("count", 0) > 0:
                p1.try_send_locked({"cmd": "inject", "bytes": r["bytes"]})

            # Block-exchange relay (coop boss battle SendBlock data).
            # p1 sendReady → copy data to p2 recvReady with fromPlayerIdx=0
            r = p1.try_send_locked({"cmd": "read_block_exchange"})
            if r and r.get("ok") and r.get("send_ready", 0):
                p2.try_send_locked({"cmd": "write_block_exchange",
                                    "from_idx": 0, "data": r["data"]})

            # p2 sendReady → copy data to p1 recvReady with fromPlayerIdx=1
            r = p2.try_send_locked({"cmd": "read_block_exchange"})
            if r and r.get("ok") and r.get("send_ready", 0):
                p1.try_send_locked({"cmd": "write_block_exchange",
                                    "from_idx": 1, "data": r["data"]})
        except Exception:
            pass


_relay_thread = threading.Thread(target=_relay_loop, daemon=True)
_relay_thread.start()

# ── MCP server ────────────────────────────────────────────────────────────

mcp = FastMCP(
    "pokemon-player",
    instructions=(
        "Control one or two GBA Pokémon FireRed instances as a human player. "
        "Use start_emulator to boot, screenshot to see the screen, "
        "press_button to press GBA buttons, and wait to let time pass. "
        "For two-player testing use instance_id='p1' and 'p2' — "
        "multiplayer packets are relayed automatically between them."
    ),
)

# ── Tool: start_emulator ──────────────────────────────────────────────────

@mcp.tool()
def start_emulator(instance_id: str = "p1", rom: str = "", savestate: str = "") -> str:
    """Boot a GBA emulator instance running Pokémon FireRed.

    Typical state paths (relative to repo root):
      test/lua/states/oaks_lab.ss1
      test/lua/states/tall_grass_route1.ss1
      test/lua/states/pewter_gym.ss1

    The emulator continues running from the loaded state immediately.
    After loading, call game_state() to confirm the ROM state.

    Args:
        instance_id: 'p1' or 'p2' for two-player testing.
        rom: Path to .gba file. Defaults to pokefirered.gba in repo root.
        savestate: Optional path to a .ss1 save state to load on boot.
    """
    if instance_id in _instances and _instances[instance_id].alive():
        return f"Instance '{instance_id}' is already running."

    rom_path = Path(rom) if rom else DEFAULT_ROM
    if not rom_path.exists():
        return f"ROM not found: {rom_path}"
    if not MGBA_BIN.exists():
        return (
            f"mgba-headless not found at {MGBA_BIN}. "
            "Build with: cmake --fresh -B /tmp/mgba-build "
            "-DBUILD_HEADLESS=ON -DUSE_LUA=ON /tmp/mgba-src && "
            "cmake --build /tmp/mgba-build -j$(sysctl -n hw.ncpu)"
        )

    # Resolve savestate path relative to repo root if not absolute
    savestate_path = ""
    if savestate:
        p = Path(savestate)
        if not p.is_absolute():
            p = REPO_ROOT / p
        savestate_path = str(p)

    inst = Instance(instance_id, _get_work_dir())
    addrs = _parse_memory_map(MEMORY_MAP)
    inst.addrs = addrs

    env = os.environ.copy()
    env["MGBA_BRIDGE_CMD"]   = inst.cmd_file
    env["MGBA_BRIDGE_RESP"]  = inst.resp_file
    env["MGBA_BRIDGE_READY"] = inst.ready_file
    if savestate_path:
        env["MGBA_BRIDGE_SAVESTATE"] = savestate_path
    for name, addr in addrs.items():
        env[f"ADDR_{name.upper()}"] = str(addr)

    inst.process = subprocess.Popen(
        [str(MGBA_BIN), "--script", str(BRIDGE_LUA), str(rom_path)],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    _instances[instance_id] = inst

    deadline = time.monotonic() + READY_TIMEOUT
    while time.monotonic() < deadline:
        if os.path.exists(inst.ready_file):
            break
        if inst.process.poll() is not None:
            return f"Emulator '{instance_id}' exited immediately — check ROM path."
        time.sleep(0.1)
    else:
        inst.process.kill()
        return f"Emulator '{instance_id}' did not signal ready within {READY_TIMEOUT}s."

    addrs_str = ", ".join(f"{k}={hex(v)}" for k, v in addrs.items())
    return (
        f"Instance '{instance_id}' running (pid {inst.process.pid}).\n"
        f"ROM: {rom_path}\n"
        f"Addresses: {addrs_str}"
    )

# ── Tool: stop_emulator ───────────────────────────────────────────────────

@mcp.tool()
def stop_emulator(instance_id: str = "p1") -> str:
    """Stop a running emulator instance.

    Args:
        instance_id: Which instance to stop ('p1' or 'p2').
    """
    inst = _instances.get(instance_id)
    if not inst or not inst.alive():
        return f"Instance '{instance_id}' is not running."
    try:
        inst.send_locked({"cmd": "quit"}, timeout=3)
    except Exception:
        pass
    inst.process.terminate()
    try:
        inst.process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        inst.process.kill()
    return f"Instance '{instance_id}' stopped."

# ── Tool: list_instances ──────────────────────────────────────────────────

@mcp.tool()
def list_instances() -> str:
    """List all emulator instances and whether they are running."""
    if not _instances:
        return "No instances started. Call start_emulator to boot one."
    lines = []
    for iid, inst in _instances.items():
        status = "RUNNING" if inst.alive() else "DEAD"
        lines.append(f"  {iid}: {status}")
    return "Running instances:\n" + "\n".join(lines)

# ── Tool: screenshot ──────────────────────────────────────────────────────

@mcp.tool()
def screenshot(instance_id: str = "p1") -> Image:
    """Take a screenshot of the current game screen.

    Returns the screen image so you can see what the game is showing.
    The GBA screen is 240×160 pixels.

    Args:
        instance_id: Which player's screen to capture ('p1' or 'p2').
    """
    path = f"/tmp/mgba_screen_{instance_id}.png"
    r = _inst(instance_id).send_locked({"cmd": "screenshot", "path": path})
    if not r.get("ok"):
        raise RuntimeError(f"Screenshot failed: {r.get('error', 'unknown')}")
    return Image(data=open(path, "rb").read(), format="png")

# ── Tool: press_button ────────────────────────────────────────────────────

@mcp.tool()
def press_button(
    button: str,
    instance_id: str = "p1",
    hold_frames: int = 8,
    release_frames: int = 8,
) -> str:
    """Press a GBA button.

    Buttons: A, B, START, SELECT, UP, DOWN, LEFT, RIGHT, L, R

    Tips:
    - Menu navigation / dialogue: A or B, defaults are fine (8 frames each)
    - Walking one tile: UP/DOWN/LEFT/RIGHT with hold_frames=16
    - Mashing through text: call press_button A repeatedly

    Args:
        button: Button name (case-insensitive).
        instance_id: Which player to control ('p1' or 'p2').
        hold_frames: How long to hold the button (60 frames = 1 second).
        release_frames: Gap after release before next input is accepted.
    """
    key = button.upper().strip()
    mask = KEY_MASKS.get(key)
    if mask is None:
        return f"Unknown button '{button}'. Valid: {', '.join(KEY_MASKS)}"

    r = _inst(instance_id).send_locked({
        "cmd": "press",
        "mask": mask,
        "hold": hold_frames,
        "release": release_frames,
    })
    if not r.get("ok"):
        return f"Button press failed: {r.get('error', 'unknown')}"
    return f"{instance_id}: pressed {key} (held {hold_frames}f, released {release_frames}f)"

# ── Tool: wait ────────────────────────────────────────────────────────────

@mcp.tool()
def wait(frames: int = 60, instance_id: str = "p1") -> str:
    """Advance the game by N frames without pressing any buttons.

    Use this to:
    - Let animations play out
    - Wait for dialogue / cutscenes to progress automatically
    - Give the game time to reach a stable state after boot

    60 frames = 1 second of game time.

    Args:
        frames: Number of frames to wait (default 60 = 1 second).
        instance_id: Which instance to advance ('p1' or 'p2').
    """
    r = _inst(instance_id).send_locked({"cmd": "wait", "frames": frames})
    if not r.get("ok"):
        return f"Wait failed: {r.get('error', 'unknown')}"
    return f"{instance_id}: waited {frames} frames ({frames/60:.1f}s)"

# ── Tool: get_text_state ─────────────────────────────────────────────────

@mcp.tool()
def get_text_state(instance_id: str = "p1") -> str:
    """Read the current dialogue / text-box state from game memory.

    Returns whether a box is open, whether it is waiting for A, and the
    decoded text content (best-effort from gStringVar4 — all overworld
    messages from ShowFieldMessage are expanded there).

    Use this to drive dialogue advancement reliably instead of guessing
    how many A presses are needed:

        while get_text_state(instance_id)["box_open"]:
            press_button("A", instance_id=instance_id, hold_frames=12, release_frames=120)

    Fields in the return dict (also stringified for display):
        box_open          – True if text is printing, script is waiting, or in a native wait
        printing          – True while character-by-character rendering is active
        waiting_for_input – True when sGlobalScriptContextStatus == CONTEXT_WAITING
        in_native_script  – True when script is blocked in a C native function (waitmessage etc.)
        text              – Decoded ASCII content of gStringVar4

    Args:
        instance_id: Which instance to query ('p1' or 'p2').
    """
    r = _inst(instance_id).send_locked({"cmd": "get_text"})
    if not r.get("ok"):
        return f"get_text failed: {r.get('error', 'unknown')}"

    box_open   = r.get("box_open", False)
    printing   = r.get("printing", False)
    waiting    = r.get("waiting_for_input", False)
    in_native  = r.get("in_native_script", False)
    text       = r.get("text", "").strip()

    if printing:
        status = "printing"
    elif waiting:
        status = "waiting for input"
    elif in_native:
        status = "in native wait"
    elif box_open:
        status = "open"
    else:
        status = "no box"

    out = f"box_open={box_open}, status={status}"
    if text:
        preview = text.replace("\n", " / ")[:80]
        out += f', text="{preview}"'
    return out


# ── Tool: load_savestate ──────────────────────────────────────────────────

@mcp.tool()
def load_savestate(path: str, instance_id: str = "p1") -> str:
    """Load a save state file into the running emulator.

    Typical state paths (relative to repo root):
      test/lua/states/oaks_lab.ss1
      test/lua/states/tall_grass_route1.ss1
      test/lua/states/pewter_gym.ss1

    The emulator continues running from the loaded state immediately.
    After loading, wait ~300 frames before sending inputs (input cooldown).

    Args:
        path: Path to .ss1 file (relative to repo root or absolute).
        instance_id: Which instance to load into ('p1' or 'p2').
    """
    p = Path(path)
    if not p.is_absolute():
        p = REPO_ROOT / p
    if not p.exists():
        return f"Save state not found: {p}"
    r = _inst(instance_id).send_locked({"cmd": "loadstate", "path": str(p)})
    if not r.get("ok"):
        return f"loadstate failed: {r.get('error', 'unknown')}"
    return f"Loaded '{p}' into '{instance_id}'. Wait ~300 frames before sending inputs."


# ── Tool: save_savestate ──────────────────────────────────────────────────

@mcp.tool()
def save_savestate(path: str, instance_id: str = "p1") -> str:
    """Save the current emulator state to a file.

    Typical state paths (relative to repo root):
      test/lua/states/pewter_gym.ss1

    Args:
        path: Destination path for .ss1 file (relative to repo root or absolute).
        instance_id: Which instance to save ('p1' or 'p2').
    """
    p = Path(path)
    if not p.is_absolute():
        p = REPO_ROOT / p
    p.parent.mkdir(parents=True, exist_ok=True)
    r = _inst(instance_id).send_locked({"cmd": "savestate", "path": str(p)})
    if not r.get("ok"):
        return f"savestate failed: {r.get('error', 'unknown')}"
    return f"Saved state to '{p}' ({r.get('size', '?')} bytes)."


# ── Cleanup ───────────────────────────────────────────────────────────────

import atexit

@atexit.register
def _cleanup():
    for inst in _instances.values():
        if inst.alive():
            try:
                inst.process.terminate()
                inst.process.wait(timeout=2)
            except Exception:
                inst.process.kill()
    if _work_dir:
        _work_dir.cleanup()

# ── Entry point ───────────────────────────────────────────────────────────

if __name__ == "__main__":
    mcp.run()
