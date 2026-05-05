#!/usr/bin/env python3
"""
tools/mcp_gamestate/server.py
MCP server exposing GBA emulator control tools to Claude Code.

Manages one or two mgba-headless instances via file-based IPC through
bridge.lua.  Supports two-instance coop testing by bridging send→recv
rings between player instances.

Add to Claude Code:
  claude mcp add --scope project gamestate \
    -- python3 tools/mcp_gamestate/server.py

Or in .mcp.json:
  {"mcpServers": {"gamestate": {"command": "python3",
    "args": ["tools/mcp_gamestate/server.py"]}}}
"""

import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

from mcp.server.fastmcp import FastMCP

# ── Paths ─────────────────────────────────────────────────────────────────

REPO_ROOT   = Path(__file__).parent.parent.parent
BRIDGE_LUA  = Path(__file__).parent / "bridge.lua"
MEMORY_MAP  = REPO_ROOT / "test" / "lua" / "memory_map.lua"
DEFAULT_ROM = REPO_ROOT / "pokefirered.gba"
MGBA_BIN    = Path(os.environ.get("MGBA", "/tmp/mgba-build/mgba-headless"))

CMD_TIMEOUT   = 30    # seconds to wait for any single command response
READY_TIMEOUT = 15    # seconds to wait for emulator to signal ready

# ── Per-instance state ────────────────────────────────────────────────────

class Instance:
    def __init__(self, iid: str, work_dir: str):
        self.iid      = iid
        self.work_dir = work_dir
        self.cmd_file  = os.path.join(work_dir, f"cmd_{iid}.json")
        self.resp_file = os.path.join(work_dir, f"resp_{iid}.json")
        self.ready_file = os.path.join(work_dir, f"ready_{iid}")
        self.process: Optional[subprocess.Popen] = None
        self.addrs: dict[str, int] = {}

    def alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def send(self, cmd: dict) -> dict:
        if not self.alive():
            raise RuntimeError(f"Instance '{self.iid}' is not running")

        tmp = self.cmd_file + ".tmp"
        with open(tmp, "w") as f:
            json.dump(cmd, f)
        os.replace(tmp, self.cmd_file)

        deadline = time.monotonic() + CMD_TIMEOUT
        while time.monotonic() < deadline:
            if os.path.exists(self.resp_file):
                with open(self.resp_file) as f:
                    raw = f.read().strip()
                try:
                    os.remove(self.resp_file)
                except FileNotFoundError:
                    pass
                return json.loads(raw)
            time.sleep(0.01)

        raise TimeoutError(
            f"No response from '{self.iid}' after {CMD_TIMEOUT}s "
            f"(cmd={cmd.get('cmd')})"
        )

# ── Global instance registry ──────────────────────────────────────────────

_work_dir: Optional[tempfile.TemporaryDirectory] = None
_instances: dict[str, Instance] = {}

def _get_work_dir() -> str:
    global _work_dir
    if _work_dir is None:
        _work_dir = tempfile.TemporaryDirectory(prefix="mgba_mcp_")
    return _work_dir.name

def _inst(iid: str) -> Instance:
    if iid not in _instances:
        raise ValueError(f"No instance '{iid}'. Call start_emulator first.")
    return _instances[iid]

# ── Parse memory_map.lua ──────────────────────────────────────────────────

def _parse_memory_map(lua_path: Path) -> dict[str, int]:
    addrs: dict[str, int] = {}
    if not lua_path.exists():
        return addrs
    pattern = re.compile(r'M\.(\w+)\s*=\s*(0x[0-9A-Fa-f]+)')
    for line in lua_path.read_text().splitlines():
        m = pattern.search(line)
        if m:
            addrs[m.group(1)] = int(m.group(2), 16)
    return addrs

# ── MCP server ────────────────────────────────────────────────────────────

mcp = FastMCP(
    "mgba-gamestate",
    instructions=(
        "Control one or two GBA emulator instances running pokefirered.gba. "
        "Use start_emulator to boot, then game_state/read_memory/inject_packet "
        "to inspect and drive the ROM. For two-player tests use instance_id='p1' "
        "and instance_id='p2' and call bridge_rings to relay packets between them."
    ),
)

# ── Tool: start_emulator ──────────────────────────────────────────────────

@mcp.tool()
def start_emulator(
    instance_id: str = "p1",
    rom: str = "",
    savestate: str = "",
) -> str:
    """Boot a GBA emulator instance.

    Args:
        instance_id: Label for this instance ('p1' or 'p2' for coop tests).
        rom: Path to .gba file. Defaults to pokefirered.gba in repo root.
        savestate: Optional path to a .ss0 save state to load on boot.

    Returns a status message and the addresses loaded from memory_map.lua.
    """
    if instance_id in _instances and _instances[instance_id].alive():
        return f"Instance '{instance_id}' is already running."

    rom_path = Path(rom) if rom else DEFAULT_ROM
    if not rom_path.exists():
        return f"ROM not found: {rom_path}"
    if not MGBA_BIN.exists():
        return (
            f"mgba-headless not found at {MGBA_BIN}. "
            "Build it with: cmake --fresh -B /tmp/mgba-build "
            "-DBUILD_HEADLESS=ON -DUSE_LUA=ON /tmp/mgba-src && "
            "cmake --build /tmp/mgba-build -j$(sysctl -n hw.ncpu)"
        )

    work = _get_work_dir()
    inst = Instance(instance_id, work)

    addrs = _parse_memory_map(MEMORY_MAP)
    inst.addrs = addrs

    env = os.environ.copy()
    env["MGBA_BRIDGE_CMD"]   = inst.cmd_file
    env["MGBA_BRIDGE_RESP"]  = inst.resp_file
    env["MGBA_BRIDGE_READY"] = inst.ready_file
    for name, addr in addrs.items():
        env[f"ADDR_{name.upper()}"] = str(addr)
    if savestate:
        env["MGBA_BRIDGE_SAVESTATE"] = savestate

    cmd = [str(MGBA_BIN), "--script", str(BRIDGE_LUA), str(rom_path)]

    inst.process = subprocess.Popen(
        cmd, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    _instances[instance_id] = inst

    # Wait for ready signal
    deadline = time.monotonic() + READY_TIMEOUT
    while time.monotonic() < deadline:
        if os.path.exists(inst.ready_file):
            break
        if inst.process.poll() is not None:
            return f"Emulator '{instance_id}' exited immediately (check ROM path)."
        time.sleep(0.1)
    else:
        inst.process.kill()
        return f"Emulator '{instance_id}' did not signal ready within {READY_TIMEOUT}s."

    addr_summary = ", ".join(f"{k}=0x{v:08X}" for k, v in sorted(addrs.items()))
    return (
        f"Instance '{instance_id}' running (pid {inst.process.pid}).\n"
        f"ROM: {rom_path}\n"
        f"Addresses: {addr_summary or '(none — run make firered first)'}"
    )

# ── Tool: stop_emulator ───────────────────────────────────────────────────

@mcp.tool()
def stop_emulator(instance_id: str = "p1") -> str:
    """Stop a running emulator instance."""
    if instance_id not in _instances:
        return f"No instance '{instance_id}'."
    inst = _instances.pop(instance_id)
    if inst.alive():
        try:
            inst.send({"cmd": "quit"})
            inst.process.wait(timeout=3)
        except Exception:
            inst.process.kill()
    return f"Instance '{instance_id}' stopped."

# ── Tool: run_frames ──────────────────────────────────────────────────────

@mcp.tool()
def run_frames(count: int = 60, instance_id: str = "p1") -> str:
    """Advance the emulator by N frames (60 fps → 60 frames = 1 second).

    Use small counts (2–10) when checking immediate packet dispatch.
    Use 300+ to let the game reach a stable state after boot.
    """
    r = _inst(instance_id).send({"cmd": "run", "frames": count})
    if r.get("ok"):
        return f"Advanced {r['frames']} frames."
    return f"Error: {r.get('error')}"

# ── Tool: game_state ──────────────────────────────────────────────────────

@mcp.tool()
def game_state(instance_id: str = "p1") -> str:
    """Read structured multiplayer state from the running ROM.

    Returns connState, partner position, ghost NPC slot, ring occupancy,
    and whether ring magic bytes are set (confirming Multiplayer_Init ran).
    """
    r = _inst(instance_id).send({"cmd": "state"})
    if "error" in r and not r.get("ok", True):
        return f"Error: {r['error']}"

    conn_states = {0: "DISCONNECTED", 1: "CONNECTING", 2: "CONNECTED"}
    lines = [
        f"connState:         {conn_states.get(r.get('connState', -1), '?')} ({r.get('connState')})",
        f"role:              {r.get('role')}",
        f"partner map:       group={r.get('partnerMapGroup')} num={r.get('partnerMapNum')}",
        f"partner pos:       x={r.get('targetX')} y={r.get('targetY')} facing={r.get('targetFacing')}",
        f"ghost obj slot:    {r.get('ghostObjId')} (0xFF = none)",
        f"isInScript:        {r.get('isInScript')}",
        f"partnerIsInScript: {r.get('partnerIsInScript')}",
        f"send ring avail:   {r.get('sendAvail', '?')} bytes",
        f"recv ring avail:   {r.get('recvAvail', '?')} bytes",
        f"rings magic ok:    {r.get('ringsMagicOk', '?')} (False = Multiplayer_Init not yet run)",
    ]
    return "\n".join(lines)

# ── Tool: read_memory ─────────────────────────────────────────────────────

@mcp.tool()
def read_memory(address: int, width: int = 32, instance_id: str = "p1") -> str:
    """Read a value from GBA memory.

    Args:
        address: GBA bus address (e.g. 0x0300157C).
        width: 8, 16, or 32 bits.
        instance_id: Which emulator instance.
    """
    cmd_map = {8: "r8", 16: "r16", 32: "r32"}
    cmd = cmd_map.get(width)
    if not cmd:
        return f"Invalid width {width}. Use 8, 16, or 32."
    r = _inst(instance_id).send({"cmd": cmd, "addr": address})
    if r.get("ok"):
        val = r["val"]
        return f"0x{address:08X} = {val} (0x{val & 0xFFFFFFFF:08X})"
    return f"Error: {r.get('error')}"

# ── Tool: write_memory ────────────────────────────────────────────────────

@mcp.tool()
def write_memory(
    address: int, value: int, width: int = 32, instance_id: str = "p1"
) -> str:
    """Write a value to GBA memory.

    Useful for setting up test preconditions (e.g. wiring gSaveBlock1Ptr).
    """
    cmd_map = {8: "w8", 16: "w16", 32: "w32"}
    cmd = cmd_map.get(width)
    if not cmd:
        return f"Invalid width {width}. Use 8, 16, or 32."
    r = _inst(instance_id).send({"cmd": cmd, "addr": address, "val": value})
    if r.get("ok"):
        return f"Wrote {value} (0x{value & 0xFFFFFFFF:08X}) to 0x{address:08X}."
    return f"Error: {r.get('error')}"

# ── Tool: inject_packet ───────────────────────────────────────────────────

@mcp.tool()
def inject_packet(hex_bytes: str, instance_id: str = "p1") -> str:
    """Push raw bytes into the ROM's gMpRecvRing receive buffer.

    hex_bytes: space-separated hex bytes, e.g.:
      "0B"              → MP_PKT_PARTNER_CONNECTED
      "01 03 07 0A 0F 02" → MP_PKT_POSITION mapGroup=3 mapNum=7 x=10 y=15 facing=2
      "02 04 B0"        → MP_PKT_FLAG_SET flagId=0x4B0 (FLAG_DEFEATED_BROCK)

    After injecting, call run_frames(2) to let Multiplayer_PollPackets process them.
    """
    r = _inst(instance_id).send({"cmd": "inject", "bytes": hex_bytes})
    if r.get("ok"):
        return f"Pushed {r['pushed']} byte(s) into recv ring of '{instance_id}'."
    return f"Error: {r.get('error')}"

# ── Tool: drain_send_ring ─────────────────────────────────────────────────

@mcp.tool()
def drain_send_ring(instance_id: str = "p1") -> str:
    """Read and consume all bytes from the ROM's gMpSendRing output buffer.

    Returns the bytes as hex so you can decode what the ROM is broadcasting
    (position updates, flag sets, etc.).  For two-player testing, pipe the
    output of one instance into inject_packet of the other via bridge_rings.
    """
    r = _inst(instance_id).send({"cmd": "drain"})
    if r.get("ok"):
        count = r["count"]
        if count == 0:
            return f"Send ring of '{instance_id}' is empty."
        return f"Drained {count} byte(s) from '{instance_id}': {r['bytes']}"
    return f"Error: {r.get('error')}"

# ── Tool: bridge_rings ────────────────────────────────────────────────────

@mcp.tool()
def bridge_rings(from_id: str = "p1", to_id: str = "p2") -> str:
    """Relay packets between two emulator instances (one direction).

    Drains from_id's send ring and injects the bytes into to_id's recv ring.
    Call this for both directions to simulate the relay server:
      bridge_rings("p1", "p2")
      bridge_rings("p2", "p1")
    Then run_frames on both instances to let them process the packets.
    """
    drain = _inst(from_id).send({"cmd": "drain"})
    if not drain.get("ok"):
        return f"Drain failed on '{from_id}': {drain.get('error')}"
    if drain["count"] == 0:
        return f"Nothing to relay: '{from_id}' send ring is empty."

    inject = _inst(to_id).send({"cmd": "inject", "bytes": drain["bytes"]})
    if not inject.get("ok"):
        return f"Inject failed on '{to_id}': {inject.get('error')}"

    return (
        f"Relayed {drain['count']} byte(s): '{from_id}' → '{to_id}'\n"
        f"Bytes: {drain['bytes']}"
    )

# ── Tool: press_button ───────────────────────────────────────────────────

# GBA button masks (active-high in mGBA setKeys)
BUTTON_MASKS = {
    "A": 0x001, "B": 0x002, "SELECT": 0x004, "START": 0x008,
    "RIGHT": 0x010, "LEFT": 0x020, "UP": 0x040, "DOWN": 0x080,
    "R": 0x100, "L": 0x200,
}

@mcp.tool()
def press_button(
    button: str,
    hold_frames: int = 3,
    release_frames: int = 3,
    instance_id: str = "p1",
) -> str:
    """Press a GBA button for hold_frames then release for release_frames.

    button: A, B, START, SELECT, UP, DOWN, LEFT, RIGHT, L, R
    Advances the emulator by (hold_frames + release_frames) total.

    Use spamA via inject_keys for fast dialogue advancement.
    Use hold_frames=16 for a full tile walk step.
    """
    name = button.upper()
    mask = BUTTON_MASKS.get(name)
    if mask is None:
        valid = ", ".join(sorted(BUTTON_MASKS))
        return f"Unknown button '{button}'. Valid: {valid}"
    r = _inst(instance_id).send({
        "cmd": "press", "mask": mask,
        "hold": hold_frames, "release": release_frames,
    })
    if r.get("ok"):
        return (
            f"Pressed {name} for {r['held']} frames, "
            f"released for {r['released']} frames on '{instance_id}'."
        )
    return f"Error: {r.get('error')}"

@mcp.tool()
def set_keys(mask: int, instance_id: str = "p1") -> str:
    """Set the raw key mask (held buttons bitmask) without advancing frames.

    Useful for holding a direction while calling run_frames repeatedly.
    Call set_keys(0) to release all buttons.
    Mask bits: A=0x001 B=0x002 SELECT=0x004 START=0x008
               RIGHT=0x010 LEFT=0x020 UP=0x040 DOWN=0x080 R=0x100 L=0x200
    """
    r = _inst(instance_id).send({"cmd": "keys", "mask": mask})
    if r.get("ok"):
        return f"Keys set to 0x{r['mask']:03X} on '{instance_id}'."
    return f"Error: {r.get('error')}"

@mcp.tool()
def load_savestate(path: str, instance_id: str = "p1") -> str:
    """Load a save state file into the emulator.

    Typical state paths (relative to repo root):
      test/lua/states/oaks_lab.ss1
      test/lua/states/tall_grass_route1.ss1
      test/lua/states/pewter_gym.ss1

    The emulator continues running from the loaded state immediately.
    After loading, call game_state() to confirm the ROM state.
    """
    abs_path = path if os.path.isabs(path) else str(REPO_ROOT / path)
    if not os.path.exists(abs_path):
        return f"State file not found: {abs_path}"
    r = _inst(instance_id).send({"cmd": "loadstate", "path": abs_path})
    if r.get("ok"):
        return f"Loaded state '{abs_path}' into '{instance_id}'."
    return f"Error: {r.get('error')}"

@mcp.tool()
def save_savestate(path: str, instance_id: str = "p1") -> str:
    """Save the current emulator state to a file.

    Use absolute paths or repo-root-relative paths.
    Useful for creating checkpoints mid-playthrough.
    """
    abs_path = path if os.path.isabs(path) else str(REPO_ROOT / path)
    os.makedirs(os.path.dirname(abs_path), exist_ok=True)
    r = _inst(instance_id).send({"cmd": "savestate", "path": abs_path})
    if r.get("ok"):
        return f"Saved state to '{abs_path}'."
    return f"Error: {r.get('error')}"

# ── Tool: check_flag ─────────────────────────────────────────────────────

@mcp.tool()
def check_flag(flag_id: int, instance_id: str = "p1") -> str:
    """Check whether a save-data flag is set in the running ROM.

    Common flags:
      0x4B0 (1200) = FLAG_DEFEATED_BROCK
      0x500 (1280) = FLAG_DEFEATED_MISTY
      Use include/constants/flags.h for the full list.
    """
    r = _inst(instance_id).send({"cmd": "flag", "id": flag_id})
    if r.get("ok"):
        state = "SET" if r["val"] else "CLEAR"
        return f"Flag 0x{flag_id:03X} ({flag_id}) is {state} in '{instance_id}'."
    return f"Error: {r.get('error')}"

# ── Tool: screenshot ──────────────────────────────────────────────────────

@mcp.tool()
def screenshot(
    path: str = "/tmp/mgba_screenshot.png",
    instance_id: str = "p1",
) -> str:
    """Take a screenshot of the current emulator frame.

    Saves to the given path and returns it so you can view it.
    Note: emu:screenshot() support depends on the mGBA build.
    """
    r = _inst(instance_id).send({"cmd": "screenshot", "path": path})
    if r.get("ok"):
        return r["path"]   # FastMCP will render this as an image if it's a path
    return f"Screenshot failed: {r.get('error')}"

# ── Tool: list_instances ──────────────────────────────────────────────────

@mcp.tool()
def list_instances() -> str:
    """List all running emulator instances and their PIDs."""
    if not _instances:
        return "No instances running. Call start_emulator to boot one."
    lines = []
    for iid, inst in _instances.items():
        status = f"pid={inst.process.pid}" if inst.alive() else "DEAD"
        lines.append(f"  {iid}: {status}")
    return "Running instances:\n" + "\n".join(lines)

# ── Cleanup on exit ────────────────────────────────────────────────────────

def _cleanup(sig=None, frame=None):
    for inst in list(_instances.values()):
        try:
            if inst.alive():
                inst.process.kill()
        except Exception:
            pass
    if _work_dir:
        _work_dir.cleanup()
    sys.exit(0)

signal.signal(signal.SIGTERM, _cleanup)
signal.signal(signal.SIGINT,  _cleanup)

# ── Entry point ────────────────────────────────────────────────────────────

if __name__ == "__main__":
    mcp.run()
