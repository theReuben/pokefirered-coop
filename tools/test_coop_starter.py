#!/usr/bin/env python3
"""
tools/test_coop_starter.py

Integration test: Two-player Oak's Lab → starter selection → rival fight.

Both players start from test/lua/states/oaks_lab.ss1 which puts them
directly in the lab at the rival's opening dialogue.

The script uses get_text (box_open / text fields) to drive all dialogue
advancement rather than hardcoded A-press counts, so it is robust to
timing and text-speed variations.

Usage:
    python3 tools/test_coop_starter.py [--p1-only]

WAIT_POINTs mark moments where the coop protocol will require both players
to sync before continuing.  Screenshots are saved to /tmp/coop_test/ for
manual review after a run.
"""

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# ── Config ─────────────────────────────────────────────────────────────────

REPO_ROOT   = Path(__file__).parent.parent
ROM         = REPO_ROOT / "pokefirered.gba"
BRIDGE_LUA  = REPO_ROOT / "tools/mcp_gamestate/bridge.lua"
MEMORY_MAP  = REPO_ROOT / "test/lua/memory_map.lua"
MGBA_BIN    = Path(os.environ.get("MGBA", "/tmp/mgba-build/mgba-headless"))
SHOT_DIR    = Path("/tmp/coop_test")
OAKS_LAB_SS = REPO_ROOT / "test/lua/states/oaks_lab.ss1"

CMD_TIMEOUT   = 30.0
READY_TIMEOUT = 15.0

BUTTON_MASKS = {
    "A": 0x001, "B": 0x002, "SELECT": 0x004, "START": 0x008,
    "RIGHT": 0x010, "LEFT": 0x020, "UP": 0x040, "DOWN": 0x080,
    "R": 0x100, "L": 0x200,
}

# ── Address loader ──────────────────────────────────────────────────────────

def _parse_memory_map(path: Path) -> dict[str, int]:
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

# ── Player class ────────────────────────────────────────────────────────────

class Player:
    def __init__(self, name: str, work_dir: str, addrs: dict[str, int]):
        self.name      = name
        self.cmd_file  = os.path.join(work_dir, f"cmd_{name}.json")
        self.resp_file = os.path.join(work_dir, f"resp_{name}.json")
        self.ready_file = os.path.join(work_dir, f"ready_{name}")
        self.addrs     = addrs
        self.process: subprocess.Popen | None = None

    def start(self, savestate: Path | None = None) -> None:
        env = os.environ.copy()
        env["MGBA_BRIDGE_CMD"]   = self.cmd_file
        env["MGBA_BRIDGE_RESP"]  = self.resp_file
        env["MGBA_BRIDGE_READY"] = self.ready_file
        if savestate:
            env["MGBA_BRIDGE_SAVESTATE"] = str(savestate)
        for name, addr in self.addrs.items():
            env[f"ADDR_{name.upper()}"] = str(addr)

        self.process = subprocess.Popen(
            [str(MGBA_BIN), "--script", str(BRIDGE_LUA), str(ROM)],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        deadline = time.monotonic() + READY_TIMEOUT
        while time.monotonic() < deadline:
            if os.path.exists(self.ready_file):
                return
            if self.process.poll() is not None:
                raise RuntimeError(f"{self.name}: emulator exited immediately")
            time.sleep(0.05)
        self.process.kill()
        raise TimeoutError(f"{self.name}: not ready within {READY_TIMEOUT}s")

    def stop(self) -> None:
        if self.process:
            try:
                self.send({"cmd": "quit"}, timeout=2)
            except Exception:
                pass
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()

    def send(self, cmd: dict, timeout: float = CMD_TIMEOUT) -> dict:
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
            time.sleep(0.005)
        raise TimeoutError(f"{self.name}: no response after {timeout}s (cmd={cmd.get('cmd')})")

    # ── High-level helpers ──────────────────────────────────────────────────

    def wait(self, frames: int) -> None:
        self.send({"cmd": "wait", "frames": frames})

    def press(self, button: str, hold: int = 12, release: int = 60) -> None:
        mask = BUTTON_MASKS[button.upper()]
        self.send({"cmd": "press", "mask": mask, "hold": hold, "release": release})

    def screenshot(self, label: str) -> Path:
        SHOT_DIR.mkdir(exist_ok=True)
        path = SHOT_DIR / f"{self.name}_{label}.png"
        r = self.send({"cmd": "screenshot", "path": str(path)})
        if not r.get("ok"):
            raise RuntimeError(f"screenshot failed: {r.get('error')}")
        return path

    def get_text_state(self) -> dict:
        """Return {box_open, printing, waiting_for_input, in_native_script, text}."""
        return self.send({"cmd": "get_text"})

    def text_has(self, substring: str) -> bool:
        state = self.get_text_state()
        return substring.lower() in state.get("text", "").lower()

    def advance_dialogue(self, button: str = "A", hold: int = 12,
                         release: int = 120, max_presses: int = 60) -> int:
        """Press button until box_open is False.  Returns number of presses."""
        for n in range(max_presses):
            state = self.get_text_state()
            if not state.get("box_open"):
                return n
            self.press(button, hold, release)
        raise AssertionError(
            f"{self.name}: dialogue didn't clear after {max_presses} presses"
        )

    def advance_dialogue_smart(self, max_presses: int = 60) -> int:
        """
        Advance dialogue, declining nickname prompts with B (to avoid
        entering the naming keyboard).  Returns number of A presses used.
        """
        presses = 0
        for _ in range(max_presses):
            state = self.get_text_state()
            if not state.get("box_open"):
                return presses
            text = state.get("text", "").lower()
            if "nickname" in text:
                self.press("B", hold=12, release=120)  # decline nickname
            else:
                self.press("A", hold=12, release=120)
                presses += 1
        raise AssertionError(
            f"{self.name}: dialogue didn't clear after {max_presses} presses"
        )

    def walk(self, direction: str, tiles: int, hold: int = 16, gap: int = 8) -> None:
        for _ in range(tiles):
            self.press(direction, hold=hold, release=gap)

    def dismiss_aide_if_open(self) -> bool:
        """If SAVE tip aide dialogue is showing, dismiss it with B."""
        state = self.get_text_state()
        if state.get("box_open") and "save" in state.get("text", "").lower():
            self.advance_dialogue(button="B")
            return True
        return False


# ── WAIT_POINT ──────────────────────────────────────────────────────────────

def wait_point(label: str, note: str, *players: Player) -> None:
    """
    Sync point: take screenshots of all players and print a summary.
    In production this would block until both players confirm readiness.
    """
    print(f"\n{'='*60}")
    print(f"WAIT_POINT: {label}")
    print(f"  {note}")
    for p in players:
        shot = p.screenshot(f"waitpoint_{label}")
        print(f"  {p.name} screenshot → {shot}")
    print(f"{'='*60}\n")


# ── Phase helpers ───────────────────────────────────────────────────────────

def phase_oak_lab_intro(p: Player) -> None:
    """
    Wait for the input cooldown after save state load, then advance
    through the rival + Oak intro dialogue until the player can move freely.

    Sequence: "Aaaaaaa Gramps!" … "Which one will you choose for yourself?"
    """
    print(f"  {p.name}: waiting 400 frames (input cooldown after save state load)")
    p.wait(400)
    print(f"  {p.name}: advancing intro dialogue…")
    n = p.advance_dialogue_smart(max_presses=40)
    print(f"  {p.name}: intro cleared ({n} A presses)")


def phase_navigate_to_ball(p: Player, ball: str) -> None:
    """
    Navigate from post-dialogue position to the chosen pokéball and press A.

    Ball positions (from empirical testing with oaks_lab.ss1):
      "bulbasaur" → left ball   (RIGHT 5 net, with aide detour)
      "charmander" → center ball (RIGHT 6 net)
      "squirtle"  → right ball  (RIGHT 7 net)

    The aide NPC in the lab triggers a SAVE tip dialogue.  We detect and
    dismiss it automatically using dismiss_aide_if_open().
    """
    ball_steps = {"bulbasaur": 5, "charmander": 6, "squirtle": 7}
    steps = ball_steps[ball.lower()]

    print(f"  {p.name}: navigating to {ball} ball ({steps} net right steps)…")

    # Walk right toward the table — aide may trigger mid-way.
    for i in range(steps):
        p.walk("RIGHT", 1)
        p.dismiss_aide_if_open()

    # Face UP and interact with the ball.
    p.press("UP", hold=2, release=8)   # turn to face the counter
    p.press("A", hold=12, release=60)  # interact with ball
    print(f"  {p.name}: pressed A on {ball} ball")


def phase_pick_starter(p: Player, ball: str) -> None:
    """
    After pressing A on a ball: advance the starter selection dialogue.
    Declines nickname prompts with B to avoid entering the naming keyboard.
    """
    print(f"  {p.name}: advancing starter selection dialogue…")
    n = p.advance_dialogue_smart(max_presses=60)
    print(f"  {p.name}: starter selection done ({n} effective A presses)")
    p.screenshot(f"starter_received_{ball}")


def phase_rival_battle(p: Player) -> None:
    """
    After both players have their starters, the rival challenges you.
    Navigate DOWN toward the lab exit to trigger the rival battle script,
    advance the pre-battle dialogue, then win the battle (Bulbasaur is
    super-effective against Charmander: Vine Whip 2-3 hits).
    """
    print(f"  {p.name}: walking toward lab exit to trigger rival…")
    for _ in range(3):
        p.walk("DOWN", 1)
        p.dismiss_aide_if_open()

    # Wait for rival intercept script or just detect box_open.
    p.wait(60)
    state = p.get_text_state()
    if state.get("box_open"):
        print(f"  {p.name}: rival dialogue detected, advancing…")
        n = p.advance_dialogue_smart(max_presses=20)
        print(f"  {p.name}: pre-battle dialogue done ({n} presses)")

    p.screenshot("rival_battle_start")
    print(f"  {p.name}: in battle — mashing A to win…")
    # Battle: press A through all menus and animations.
    for _ in range(30):
        state = p.get_text_state()
        if not state.get("box_open"):
            p.wait(30)
        p.press("A", hold=12, release=60)
    p.screenshot("after_rival")


# ── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    p1_only = "--p1-only" in sys.argv

    if not ROM.exists():
        sys.exit(f"ROM not found: {ROM}")
    if not MGBA_BIN.exists():
        sys.exit(f"mgba-headless not found: {MGBA_BIN}")
    if not OAKS_LAB_SS.exists():
        sys.exit(f"Save state not found: {OAKS_LAB_SS}")

    SHOT_DIR.mkdir(exist_ok=True)
    addrs = _parse_memory_map(MEMORY_MAP)

    work = tempfile.mkdtemp(prefix="coop_test_")
    p1 = Player("p1", work, addrs)
    p2 = Player("p2", work, addrs) if not p1_only else None

    print("Starting emulators…")
    p1.start(savestate=OAKS_LAB_SS)
    if p2:
        p2.start(savestate=OAKS_LAB_SS)
    print("Both emulators ready.\n")

    try:
        # ── Phase 1: Oak's Lab intro dialogue ─────────────────────────────
        print("Phase 1: clearing Oak's Lab intro dialogue")
        phase_oak_lab_intro(p1)
        if p2:
            phase_oak_lab_intro(p2)

        # WAIT_POINT: both players have cleared the intro and can move freely
        players = (p1, p2) if p2 else (p1,)
        wait_point("both_in_oaks_lab",
                   "Both players can now walk freely — pick your starter!",
                   *players)

        # ── Phase 2: Starter selection ────────────────────────────────────
        print("Phase 2: P1 picks Bulbasaur, P2 picks Squirtle")
        phase_navigate_to_ball(p1, "bulbasaur")
        if p2:
            phase_navigate_to_ball(p2, "squirtle")

        phase_pick_starter(p1, "bulbasaur")

        # WAIT_POINT: P1 has picked, P2 is choosing
        if p2:
            wait_point("p1_picked_p2_choosing",
                       "P1 got Bulbasaur — P2 is choosing Squirtle",
                       p1, p2)
            phase_pick_starter(p2, "squirtle")

        # WAIT_POINT: both starters chosen, rival reveal
        wait_point("both_starters_chosen",
                   "Both starters picked — rival gets Charmander",
                   *players)

        # ── Phase 3: Rival battle ─────────────────────────────────────────
        print("Phase 3: rival battle")
        phase_rival_battle(p1)
        if p2:
            phase_rival_battle(p2)

        print("\n✓ Test complete. Review screenshots in", SHOT_DIR)

    finally:
        p1.stop()
        if p2:
            p2.stop()
        import shutil
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
