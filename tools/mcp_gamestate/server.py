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
        self.iid         = iid
        self.work_dir    = work_dir
        self.cmd_file    = os.path.join(work_dir, f"cmd_{iid}.json")
        self.resp_file   = os.path.join(work_dir, f"resp_{iid}.json")
        self.ready_file  = os.path.join(work_dir, f"ready_{iid}")
        self.inject_file = os.path.join(work_dir, f"inject_{iid}.hex")
        self.process: Optional[subprocess.Popen] = None
        self.addrs: dict[str, int] = {}
        self._lock = threading.Lock()  # one command at a time

    def alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def inject_bytes(self, hex_str: str) -> None:
        """Write hex bytes to the side-channel inject file (no lock needed).

        The bridge reads INJECT_FILE every frame regardless of its current
        command state, so this lands even during a long wait/press command.
        """
        tmp = self.inject_file + ".tmp"
        with open(tmp, "w") as f:
            f.write(hex_str)
        os.replace(tmp, self.inject_file)

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
# Pending bytes that were drained but failed to inject on the previous cycle.
# Keyed by destination instance_id ("p1" or "p2").
_pending_inject: dict[str, str] = {}

# Heartbeat / disconnect detection state.
# Tracks when we last received non-empty bytes from each instance.
_last_heard: dict[str, float] = {}
# Tracks instances that have already had PARTNER_DISCONNECTED injected so we
# only fire the event once per disconnect (until they reconnect).
_disconnect_injected: set = set()
# Which instance is currently the host (used for host-migration).
_host_instance: str = "p1"

# Silence threshold: if an instance hasn't sent bytes in this many seconds,
# consider it disconnected and notify the partner.
_HEARTBEAT_TIMEOUT = 6.0


def _relay_loop() -> None:
    """Continuously relay multiplayer packets and coop battle blocks between p1 and p2."""
    global _connected_pair, _host_instance
    while True:
        time.sleep(RELAY_INTERVAL)
        try:
            p1 = _instances.get("p1")
            p2 = _instances.get("p2")
            both_alive = bool(p1 and p1.alive() and p2 and p2.alive())

            if not both_alive:
                if _connected_pair:
                    # One instance just died — notify the survivor via the side-channel
                    # inject file (no lock needed, lands on the very next frame).
                    p1_alive = bool(p1 and p1.alive())
                    p2_alive = bool(p2 and p2.alive())
                    if p1_alive and not p2_alive:
                        if "p2" not in _disconnect_injected:
                            _disconnect_injected.add("p2")
                            if _host_instance == "p2":
                                p1.inject_bytes("17")
                                _host_instance = "p1"
                            p1.inject_bytes("0C")
                    elif p2_alive and not p1_alive:
                        if "p1" not in _disconnect_injected:
                            _disconnect_injected.add("p1")
                            if _host_instance == "p1":
                                p2.inject_bytes("17")
                                _host_instance = "p2"
                            p2.inject_bytes("0C")
                _connected_pair = False
                _pending_inject.clear()
                _disconnect_injected.clear()
                _last_heard.clear()
                continue

            # Inject MP_PKT_PARTNER_CONNECTED (0x0B) into both recv rings once
            # per session so each ROM auto-sets connState = MP_STATE_CONNECTED
            # without requiring an in-game handshake packet.
            if not _connected_pair:
                p1.inject_bytes("0B")
                p2.inject_bytes("0B")
                _connected_pair = True
                # Initialise last-heard timestamps so we don't immediately trigger
                # a false-positive disconnect on session start.
                now = time.time()
                _last_heard.setdefault("p1", now)
                _last_heard.setdefault("p2", now)

            def relay_bytes(src: "Instance", dst: "Instance", dst_id: str) -> None:
                """Drain src send ring → inject into dst recv ring, with retry buffering."""
                src_id = src.iid
                pending = _pending_inject.pop(dst_id, "")
                r = src.try_send_locked({"cmd": "drain"})
                new_bytes = (r.get("bytes", "") if r and r.get("ok") else "")
                # Update heartbeat timestamp whenever the instance sends anything.
                if new_bytes:
                    _last_heard[src_id] = time.time()
                    # Instance is alive again — clear its disconnect entry.
                    if src_id in _disconnect_injected:
                        _disconnect_injected.discard(src_id)
                        # Notify the current host that the partner reconnected.
                        dst.inject_bytes("0B")
                combined = (pending + (" " if pending and new_bytes else "") + new_bytes).strip()
                if combined:
                    result = dst.try_send_locked({"cmd": "inject", "bytes": combined})
                    if not result or not result.get("ok"):
                        # Inject failed (lock busy); buffer bytes for next cycle.
                        _pending_inject[dst_id] = combined

            # Ring-buffer packet relay (overworld position, flags, etc.)
            relay_bytes(p1, p2, "p2")  # p1 → p2
            relay_bytes(p2, p1, "p1")  # p2 → p1

            # Disconnect detection: if an instance has been silent too long, tell
            # the partner it's gone so the ROM can release waiting state.
            now = time.time()
            instance_map = {"p1": p1, "p2": p2}
            for iid, inst in instance_map.items():
                partner_iid = "p2" if iid == "p1" else "p1"
                partner_inst = instance_map[partner_iid]
                last = _last_heard.get(iid, now)
                if now - last > _HEARTBEAT_TIMEOUT and iid not in _disconnect_injected:
                    _disconnect_injected.add(iid)
                    # Host migration: if the silent instance is the host, promote
                    # the partner to host before sending disconnect.
                    if iid == _host_instance:
                        partner_inst.inject_bytes("17")
                        _host_instance = partner_iid
                    # Inject PARTNER_DISCONNECTED (0x0C) into the partner's recv ring.
                    partner_inst.inject_bytes("0C")

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
    env["MGBA_BRIDGE_CMD"]    = inst.cmd_file
    env["MGBA_BRIDGE_RESP"]  = inst.resp_file
    env["MGBA_BRIDGE_READY"] = inst.ready_file
    env["MGBA_BRIDGE_INJECT"] = inst.inject_file
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
def screenshot(instance_id: str = "p1", save_path: str = "", claim: str = "") -> Image:
    """Take a screenshot of the current game screen.

    Returns the screen image so you can see what the game is showing.
    The GBA screen is 240×160 pixels.

    Args:
        instance_id: Which player's screen to capture ('p1' or 'p2').
        save_path: Optional path to also save the PNG on disk (e.g.
            'test/evidence/fix_ghost_p1.png').  Relative paths are
            resolved from the repo root.  Parent directories are created
            automatically.  When provided, a JSON sidecar is also written
            alongside the PNG capturing text-box state at capture time.
        claim: One-sentence description of what this screenshot proves.
            Written into the JSON sidecar for use by verify-evidence.
    """
    path = f"/tmp/mgba_screen_{instance_id}.png"
    r = _inst(instance_id).send_locked({"cmd": "screenshot", "path": path})
    if not r.get("ok"):
        raise RuntimeError(f"Screenshot failed: {r.get('error', 'unknown')}")
    data = open(path, "rb").read()
    if save_path:
        import shutil, pathlib, json, datetime
        dest = pathlib.Path(save_path)
        if not dest.is_absolute():
            dest = pathlib.Path(__file__).parent.parent.parent / dest
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(path, dest)
        tr = _inst(instance_id).send_locked({"cmd": "get_text"})
        sidecar = {
            "instance_id": instance_id,
            "captured_at": datetime.datetime.utcnow().isoformat() + "Z",
            "claim": claim,
            "box_open": tr.get("box_open", False),
            "printing": tr.get("printing", False),
            "waiting_for_input": tr.get("waiting_for_input", False),
            "in_native_script": tr.get("in_native_script", False),
            "text": tr.get("text", "").strip(),
        }
        dest.with_suffix(".json").write_text(json.dumps(sidecar, indent=2))
    return Image(data=data, format="png")

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


# ── Tool: advance_text ───────────────────────────────────────────────────

@mcp.tool()
def advance_text(
    instance_id: str = "p1",
    max_frames: int = 600,
    press_interval: int = 30,
    max_presses: int = 40,
) -> str:
    """Advance through dialogue by pressing A until the text box closes.

    More efficient than repeated press_button(A) + get_text_state calls —
    the entire sequence runs inside the emulator in one round-trip.

    Use this whenever you need to dismiss overworld dialogue, NPC text,
    or any message box.  Call once; it returns when the box is gone.

    Args:
        instance_id:    Which instance to control ('p1' or 'p2').
        max_frames:     Give up after this many frames (default 600 = ~10s).
        press_interval: Press A every N frames while box is open (default 30).
        max_presses:    Hard cap on A presses before giving up (default 40).
    """
    inst = _inst(instance_id)
    timeout = max_frames / 60.0 + 15
    r = inst.send_locked({
        "cmd": "advance_text",
        "max_frames": max_frames,
        "press_interval": press_interval,
        "max_presses": max_presses,
    }, timeout=timeout)
    if not r.get("ok"):
        return f"advance_text failed: {r.get('error', 'unknown')}"
    result  = r.get("result", "?")
    presses = r.get("presses", 0)
    return f"{instance_id}: text box {result} after {presses} A-presses"


# ── Tool: move_steps ──────────────────────────────────────────────────────

@mcp.tool()
def move_steps(sequence: str, instance_id: str = "p1") -> str:
    """Move the player a fixed number of tiles in each direction.

    Executes the entire movement atomically in one round-trip, saving many
    press_button + wait calls.

    sequence format: comma-separated "DIRECTION:COUNT" pairs (case-insensitive).
    Example: "LEFT:3,DOWN:3" moves 3 tiles left then 3 tiles down.
    Valid directions: LEFT, RIGHT, UP, DOWN.

    Args:
        sequence:    Movement sequence, e.g. 'LEFT:3,DOWN:3'.
        instance_id: Which instance to control ('p1' or 'p2').
    """
    import re as _re
    total_steps = sum(int(n) for n in _re.findall(r':(\d+)', sequence))
    timeout = total_steps * (16 + 8) / 60.0 + 10
    inst = _inst(instance_id)
    r = inst.send_locked({"cmd": "move_steps", "sequence": sequence.upper()},
                         timeout=max(timeout, CMD_TIMEOUT))
    if not r.get("ok"):
        return f"move_steps failed: {r.get('error', 'unknown')}"
    return (f"{instance_id}: moved {r.get('steps', '?')} steps "
            f"→ tile ({r.get('x', '?')}, {r.get('y', '?')})")


# ── Tool: read_memory ────────────────────────────────────────────────────

def _resolve_addr(inst: "Instance", address: str) -> int:
    """Resolve a symbol name or hex/decimal string to an integer address."""
    # Try numeric first (handles "0x..." and plain integers)
    try:
        return int(address, 0)
    except ValueError:
        pass
    # Try symbol lookup from memory_map.lua
    addr = inst.addrs.get(address)
    if addr is not None:
        return addr
    raise ValueError(
        f"Unknown address '{address}'. Use a hex address (e.g. '0x03002558') "
        f"or a symbol from memory_map.lua (e.g. 'gBattlerControllerFuncs'). "
        f"Known symbols: {', '.join(sorted(inst.addrs))}"
    )


@mcp.tool()
def read_memory(
    address: str,
    instance_id: str = "p1",
    count: int = 1,
    width: int = 32,
) -> str:
    """Read raw GBA memory and return hex values.

    address can be a symbol name from memory_map.lua (e.g. 'gBattlerControllerFuncs')
    or a hex address string (e.g. '0x03002558').

    Args:
        address:     Symbol name or hex address.
        instance_id: Which instance to read from ('p1' or 'p2').
        count:       Number of values to read (default 1, max 256).
        width:       Bits per value: 8, 16, or 32 (default 32).
    """
    inst = _inst(instance_id)
    addr = _resolve_addr(inst, address)
    r = inst.send_locked({"cmd": "read_range", "addr": addr, "count": count, "width": width})
    if not r.get("ok"):
        return f"read_memory failed: {r.get('error', 'unknown')}"
    values = r.get("values", [])
    stride = width // 8
    lines = []
    for i, v in enumerate(values):
        lines.append(f"  [{i}] 0x{addr + i*stride:08X} = 0x{v}")
    return f"{address} (width={width}):\n" + "\n".join(lines)


# ── Tool: battle_diag ─────────────────────────────────────────────────────

# Thumb function addresses as stored in controller tables (address | 1).
# Built once from memory_map.lua data; refreshed when start_emulator is called.
_FUNC_NAMES: dict[int, str] = {}

def _build_func_table(addrs: dict[str, int]) -> dict[int, str]:
    """Map Thumb function pointer values → human-readable names."""
    table: dict[int, str] = {}
    func_symbols = [
        "HandleChooseMoveAfterDma3",
        "HandleInputChooseMove",
        "SetControllerToPlayer",
        "SetControllerToPlayerPartner",
        "SetControllerToOpponent",
        "PlayerHandleChooseMove",
        "BeginBattleIntro",
    ]
    for sym in func_symbols:
        if sym in addrs:
            raw = addrs[sym]
            table[raw] = sym          # plain address
            table[raw | 1] = sym      # Thumb bit set (how GBA stores it)
    return table


@mcp.tool()
def battle_diag(instance_id: str = "p1") -> str:
    """One-shot battle state diagnostic — replaces screenshot-based inference.

    Reads controller function pointers, battle state machine status,
    and key multiplayer flags directly from GBA memory. Use this whenever
    the battle appears frozen or behaves unexpectedly.

    Args:
        instance_id: Which instance to inspect ('p1' or 'p2').
    """
    inst = _inst(instance_id)
    addrs = inst.addrs
    func_names = _build_func_table(addrs)

    def read32(sym_or_addr, offset=0):
        base = _resolve_addr(inst, str(sym_or_addr)) if isinstance(sym_or_addr, str) else sym_or_addr
        r = inst.send_locked({"cmd": "read_range", "addr": base + offset, "count": 1, "width": 32})
        return int(r.get("values", ["0"])[0], 16) if r.get("ok") else None

    def read8(sym_or_addr, offset=0):
        base = _resolve_addr(inst, str(sym_or_addr)) if isinstance(sym_or_addr, str) else sym_or_addr
        r = inst.send_locked({"cmd": "read_range", "addr": base + offset, "count": 1, "width": 8})
        return int(r.get("values", ["0"])[0], 16) if r.get("ok") else None

    def read_arr8(sym_or_addr, count, offset=0):
        base = _resolve_addr(inst, str(sym_or_addr)) if isinstance(sym_or_addr, str) else sym_or_addr
        r = inst.send_locked({"cmd": "read_range", "addr": base + offset, "count": count, "width": 8})
        return [int(v, 16) for v in r.get("values", [])] if r.get("ok") else []

    def read_arr32(sym_or_addr, count, offset=0):
        base = _resolve_addr(inst, str(sym_or_addr)) if isinstance(sym_or_addr, str) else sym_or_addr
        r = inst.send_locked({"cmd": "read_range", "addr": base + offset, "count": count, "width": 32})
        return [int(v, 16) for v in r.get("values", [])] if r.get("ok") else []

    def fname(ptr):
        name = func_names.get(ptr, "")
        return f"0x{ptr:08X} ({name})" if name else f"0x{ptr:08X} (?)"

    lines = [f"=== battle_diag [{instance_id}] ==="]

    # ── gBattlersCount / gBattleTypeFlags ────────────────────────────────
    if "gBattlersCount" in addrs:
        bc = read8("gBattlersCount")
        lines.append(f"gBattlersCount      = {bc}")
    if "gBattleTypeFlags" in addrs:
        bf = read32("gBattleTypeFlags")
        if bf is not None:
            MULTI   = bf & (1 << 4)
            DOUBLE  = bf & (1 << 8)
            LINK    = bf & (1 << 1)
            INGAME  = bf & (1 << 17)
            COOP    = bf & (1 << 30) if bf else 0
            lines.append(f"gBattleTypeFlags    = 0x{bf:08X}  "
                         f"(MULTI={'Y' if MULTI else 'N'} DOUBLE={'Y' if DOUBLE else 'N'} "
                         f"LINK={'Y' if LINK else 'N'} INGAME_PARTNER={'Y' if INGAME else 'N'})")

    # ── gBattleMainFunc ──────────────────────────────────────────────────
    if "gBattleMainFunc" in addrs:
        mf = read32("gBattleMainFunc")
        lines.append(f"gBattleMainFunc     = {fname(mf) if mf is not None else '?'}")

    # ── gBattlerControllerFuncs[0..3] ────────────────────────────────────
    if "gBattlerControllerFuncs" in addrs:
        ctrl = read_arr32("gBattlerControllerFuncs", 4)
        lines.append("gBattlerControllerFuncs:")
        roles = ["player_left", "opponent_left", "player_right", "opponent_right"]
        for i, (ptr, role) in enumerate(zip(ctrl, roles)):
            lines.append(f"  [battler {i} / {role}] = {fname(ptr)}")

    # ── gAbsentBattlerFlags / gBattlerPositions ──────────────────────────
    if "gAbsentBattlerFlags" in addrs:
        ab = read8("gAbsentBattlerFlags")
        lines.append(f"gAbsentBattlerFlags = 0b{ab:04b}  "
                     f"(battlers absent: {[i for i in range(4) if ab is not None and ab & (1<<i)]})")
    if "gBattlerPositions" in addrs:
        pos = read_arr8("gBattlerPositions", 4)
        pos_names = ["player_left","opponent_left","player_right","opponent_right"]
        lines.append(f"gBattlerPositions   = {[pos_names[p] if p < 4 else p for p in pos]}")

    # ── gBattleCommunication ─────────────────────────────────────────────
    if "gBattleCommunication" in addrs:
        bc_arr = read_arr8("gBattleCommunication", 8)
        STATE_NAMES = {0:"TURN_START", 1:"BEFORE_ACTION", 2:"WAIT_ACTION",
                       3:"SELECTION_SCRIPT", 4:"CONFIRMED_STANDBY", 5:"CONFIRMED"}
        decoded = [STATE_NAMES.get(v, str(v)) for v in bc_arr]
        lines.append(f"gBattleCommunication= {decoded}")

    # ── gMultiplayerState key fields ─────────────────────────────────────
    if "gMultiplayerState" in addrs:
        mp_base = addrs["gMultiplayerState"]
        # Struct offsets (see include/multiplayer.h + PLAYER_NAME_LENGTH=7)
        offsets = {
            "connState":              1,
            "partnerPartySelectDone": 31,
            "battleTurnReceived":     38,
            "battleTurnSent":         42,
            "battleGraceTimer_lo":    52,  # u16 — read low byte
        }
        mp_vals = {k: read8(mp_base, off) for k, off in offsets.items()}
        CONN = {0: "DISCONNECTED", 1: "CONNECTED", 2: "CONNECTING"}
        lines.append("gMultiplayerState:")
        lines.append(f"  connState             = {CONN.get(mp_vals['connState'], mp_vals['connState'])}")
        lines.append(f"  partnerPartySelectDone= {bool(mp_vals['partnerPartySelectDone'])}")
        lines.append(f"  battleTurnReceived    = {bool(mp_vals['battleTurnReceived'])}")
        lines.append(f"  battleTurnSent        = {bool(mp_vals['battleTurnSent'])}")

    return "\n".join(lines)


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
