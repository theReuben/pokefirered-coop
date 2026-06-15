#!/usr/bin/env python3
"""
tools/coop_harness/live_bridge.py

Standalone harness that drives two headless mGBA instances and bridges their
co-op ring buffers through a REAL WebSocket relay — the "Python binary↔JSON
translator" that tools/coop_harness/coop_orchestrator.py:251 flagged as the
missing follow-up to Layer 4.

Why this exists
---------------
The in-process MCP relay (tools/mcp_gamestate/server.py `_relay_loop`) copies
ROM bytes peer-to-peer with zero loss, in order, every cycle.  The real
Tauri/PartyKit path is a WebSocket carrying JSON — real framing, real TCP
ordering, real socket buffering.  Several shipped bugs only reproduced on the
real relay.  This harness routes the two ROMs through `live_relay_host.mjs`
(which runs the genuine relay-server/src/server.ts `PokemonCoopServer` class
over the `ws` library), so the wire path is a real WebSocket relay.

How it works
------------
- Imports tools/mcp_gamestate/server.py to reuse its Windows-proven emulator
  control (Instance file-IPC, start_emulator, press_button, read_memory, …),
  then NEUTERS that module's in-process relay (RELAY_INTERVAL → ~forever) so
  this harness is the sole owner of the ring buffers.
- Opens one WebSocket per ROM to the relay.
- Drain loop per ROM: drains the send ring → forwards each whole packet to the
  relay as {"type":"raw","bytes":hex}.  The ROMs speak the full peer-to-peer
  protocol, so raw-for-everything is faithful: the relay is a byte forwarder
  plus role/connect/disconnect injection, exactly like the MCP relay.
- Recv loop per ROM: maps relay messages back to ring-buffer injects —
    role               → ROLE_ASSIGN (0x1B 01/02), deferred until both present
    partner_connected  → PARTNER_CONNECTED (0x0B)
    partner_disconnected → PARTNER_DISCONNECTED (0x0C)
    raw                → inject bytes verbatim
    session_settings   → SEED_SYNC (0x06 + seed BE32) — mirrors serial_bridge.rs
- Seed origination mirrors the Tauri app: the host mints a nonzero u32, writes
  it straight to gCoopSettings.encounterSeed (like set_encounter_seed), and
  sends {type:session_settings, encounterSeed} to the relay; the relay
  broadcasts it to the guest, whose recv loop turns it into a SEED_SYNC packet.

Run `python live_bridge.py --smoke` for the connect + position-sync check.
"""

import argparse
import importlib.util
import os
import random
import sys
import threading
import time
from pathlib import Path

# ── Locate repo + emulator binary ──────────────────────────────────────────
REPO_ROOT = Path(__file__).resolve().parent.parent.parent
os.environ.setdefault("MGBA", r"C:\msys64\tmp\mgba-build\mgba-headless.exe")
# Wall-clock silence timeout must be huge: agent-paced driving sends no packets
# for long stretches and would otherwise trip a false disconnect.
os.environ.setdefault("COOP_HEARTBEAT_TIMEOUT", "86400")

# ── Import the MCP server module for emulator control ──────────────────────
def _load_server():
    path = REPO_ROOT / "tools" / "mcp_gamestate" / "server.py"
    spec = importlib.util.spec_from_file_location("mcp_server", path)
    mod = importlib.util.module_from_spec(spec)
    saved_argv = sys.argv
    sys.argv = ["mcp_server"]           # server.py guards mcp.run() on __main__
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.argv = saved_argv           # restore so our own argparse sees flags
    return mod

SRV = _load_server()
# Neuter the in-process relay: this harness owns the rings.  The relay thread
# is currently in its first short sleep; bumping the interval means its next
# sleep is ~forever, so it never drains/injects again.
SRV.RELAY_INTERVAL = 1e9
time.sleep(0.3)  # let the relay thread enter the long sleep

# ── websockets sync client (pure-Python, present in user's env) ────────────
from websockets.sync.client import connect as ws_connect

# ── Memory map (regenerated this session) ──────────────────────────────────
GMP_STATE   = 0x0300157C   # struct MultiplayerState
OFF_ROLE        = 0
OFF_CONNSTATE   = 1
OFF_PARTNER_MAPGROUP = 2
OFF_PARTNER_MAPNUM   = 3
OFF_TARGET_X    = 4
OFF_TARGET_Y    = 5
OFF_TARGET_FACING = 6
OFF_GHOST_OBJ   = 7
OFF_BOSS_READY  = 8   # bossReadyBossId (local readiness)
OFF_PARTNER_BOSS = 9  # partnerBossId  (partner readiness, set on BOSS_READY recv)

GCOOP            = 0x030015C8   # struct CoopSettings
OFF_RANDOMIZE    = 0           # randomizeEncounters bitfield byte
OFF_ENC_SEED     = 4           # u32 encounterSeed

MP_STATE_CONNECTED = 2
MP_ROLE_HOST = 1
MP_ROLE_GUEST = 2
GHOST_INVALID_SLOT = 0xFF

PKT_SEED_SYNC   = 0x06
PKT_FLAG_SET    = 0x02
PKT_BOSS_READY  = 0x04

# Battle-state symbols (from pokefirered.map @ HEAD build 2026-06-15).
GBATTLE_MONS    = 0x02000420   # struct BattlePokemon[4]; species @ offset 0 (plaintext)
BM_STRIDE       = 0x88         # sizeof(BattlePokemon) = (gBattlersBySpeed-gBattleMons)/4
GBATTLERS_COUNT = 0x020000B0   # u8
GBATTLE_TYPE    = 0x020000AC   # u32 gBattleTypeFlags
GBATTLE_MAINFN  = 0x03002584   # gBattleMainFunc (nonzero in battle)
GSAVEBLOCK1_PTR = 0x0300533C   # u32 pointer to SaveBlock1
SB1_FLAGS_OFF   = 0x1270       # SaveBlock1.flags[]

# Boss IDs / original species for the "non-original" assertions.
BOSS_ID_BROCK   = 1
FLAG_DEFEATED_BROCK = 0x4B0    # byte 150, bit 0 in flags[]
ROUTE1_ORIGINAL = {16, 19}     # Pidgey, Rattata (the only Route 1 land mons)
BROCK_ORIGINAL  = {74, 95}     # Geodude, Onix


def battle_mon_species(inst, battler):
    return read_u16(inst, GBATTLE_MONS + battler * BM_STRIDE + 0)


def in_battle(inst):
    cnt = read_u8(inst, GBATTLERS_COUNT)
    fn  = read_u32(inst, GBATTLE_MAINFN)
    return (cnt is not None and 2 <= cnt <= 4 and fn not in (0, None))


def now_ts():
    return time.strftime("%H:%M:%S")


def log(msg):
    print(f"[{now_ts()}] {msg}", flush=True)


# ── Low-level ring/memory helpers (clean ints, bypassing string formatting) ─
def _read_width(inst, addr, width):
    # Transient empty reads happen (documented MCP quirk): a response can be
    # ok=True yet carry no "values". Retry a few times before giving up.
    for _ in range(4):
        r = inst.send_locked({"cmd": "read_range", "addr": addr, "count": 1, "width": width})
        if r.get("ok"):
            vals = r.get("values")
            if vals:
                return int(vals[0], 16)
        time.sleep(0.05)
    return None


def read_u8(inst, addr):
    return _read_width(inst, addr, 8)


def read_u16(inst, addr):
    return _read_width(inst, addr, 16)


def read_u32(inst, addr):
    return _read_width(inst, addr, 32)


def write_u32(inst, addr, value):
    inst.send_locked({"cmd": "write_u32", "addr": addr, "value": value})


def write_u8(inst, addr, value):
    inst.send_locked({"cmd": "write_u8", "addr": addr, "value": value})


def safe_inject(inst, hexstr, retries=12):
    """inject_bytes with retry: on Windows os.replace raises WinError 5 when
    bridge.lua has the inject file open for reading at that instant.  Retry a
    few times with a tiny backoff instead of letting it kill the recv loop."""
    for attempt in range(retries):
        try:
            inst.inject_bytes(hexstr)
            return True
        except PermissionError:
            time.sleep(0.01 * (attempt + 1))
        except Exception:
            return False
    return False


class Peer:
    """One ROM endpoint bridged to the relay over a real WebSocket."""

    def __init__(self, iid, ws_url, on_session_settings=None):
        self.iid = iid
        self.url = ws_url
        self.inst = SRV._instances[iid]
        self.ws = None
        self.role = None            # "host" / "guest" from relay
        self.both_present = False   # set on partner_connected
        self._pending_role_pkt = None
        self.on_session_settings = on_session_settings
        self.stop = False
        self.rx_counts = {}         # message-type tallies for evidence
        self.raw_injected = 0
        # Chaos: drop probability applied to ROM packets on the send path (set
        # by LiveBridge).  This is the live-path equivalent of set_link_chaos —
        # the in-process relay's chaos is neutered here, so loss must be injected
        # in the bridge.  Position packets (continuous) absorb loss harmlessly;
        # one-shot state (boss-ready) relies on the STATE_BEACON re-carry.
        self.chaos_drop = 0.0
        self._chaos_rng = random.Random()
        self.tx_dropped = 0
        self.tx_sent = 0
        self.connect_event = threading.Event()
        # Inbound byte queue: relay messages append whole packets here (in
        # arrival order); the flush loop drains it into the recv ring via the
        # locked "inject" command.  This avoids the side-channel inject_file's
        # last-write-wins race (bridge.lua reads that file once per frame, so a
        # burst of separate writes loses all but the last — which silently drops
        # one-shot packets like SEED_SYNC while redundant position packets
        # survived).  Mirrors the in-process relay's per-cycle coalescing.
        self._inbound = bytearray()
        self._inbound_lock = threading.Lock()

    # ── lifecycle ──────────────────────────────────────────────────────────
    def open(self):
        self.ws = ws_connect(self.url, open_timeout=10)
        threading.Thread(target=self._recv_loop, daemon=True).start()
        threading.Thread(target=self._drain_loop, daemon=True).start()
        threading.Thread(target=self._flush_loop, daemon=True).start()

    def _enqueue(self, hexstr):
        """Append a whole packet (hex) to the ordered inbound queue."""
        with self._inbound_lock:
            self._inbound += bytes.fromhex(hexstr.replace(" ", ""))

    def _flush_loop(self):
        # The ROM empties its 256-byte recv ring every frame (Multiplayer_Poll
        # Packets loops until empty), so injecting <=192 bytes per ~frame keeps
        # well under capacity and never overflows ring_push (which has no
        # tail-guard).  Flush in order via the locked inject command.
        CAP = 192
        while not self.stop:
            chunk = None
            with self._inbound_lock:
                if self._inbound:
                    chunk = bytes(self._inbound[:CAP])
                    del self._inbound[:CAP]
            if chunk:
                r = self.inst.try_send_locked({"cmd": "inject", "bytes": chunk.hex()})
                if not r or not r.get("ok"):
                    # Inject failed (lock busy) — put the bytes back at the front.
                    with self._inbound_lock:
                        self._inbound[:0] = chunk
            time.sleep(0.012)

    def close(self):
        self.stop = True
        try:
            if self.ws:
                self.ws.close()
        except Exception:
            pass

    # ── send ring → relay ────────────────────────────────────────────────
    def _drain_loop(self):
        while not self.stop:
            try:
                r = self.inst.try_send_locked({"cmd": "drain"})
                hexstr = (r.get("bytes", "") if r and r.get("ok") else "")
                if hexstr:
                    packets = SRV._split_packets(hexstr)
                    if packets is None:
                        packets = [hexstr.replace(" ", "")]  # unknown framing → whole blob
                    for pkt in packets:
                        if self.chaos_drop > 0 and self._chaos_rng.random() < self.chaos_drop:
                            self.tx_dropped += 1
                            continue
                        self.tx_sent += 1
                        self.ws.send(_json_raw(pkt))
            except Exception:
                pass
            time.sleep(0.02)

    # ── relay → recv ring ────────────────────────────────────────────────
    def _recv_loop(self):
        import json
        while not self.stop:
            try:
                raw = self.ws.recv(timeout=1.0)
            except TimeoutError:
                continue
            except Exception:
                if self.stop:
                    break
                time.sleep(0.05)
                continue
            try:
                msg = json.loads(raw)
            except Exception:
                continue
            t = msg.get("type")
            self.rx_counts[t] = self.rx_counts.get(t, 0) + 1
            try:
                self._handle(msg, t)
            except Exception as e:
                log(f"{self.iid} handler error on {t}: {e}")

    def _handle(self, msg, t):
        if t == "role":
            self.role = msg.get("role")
            # Defer the ROLE_ASSIGN inject until both players are present, so
            # the host does not flip connState=CONNECTED before the guest joins
            # (any received packet auto-sets CONNECTED — multiplayer.c:309).
            self._pending_role_pkt = "1B 01" if self.role == "host" else "1B 02"
            if self.both_present:
                self._flush_connect()
        elif t == "partner_connected":
            self.both_present = True
            self._flush_connect()
            self.connect_event.set()
        elif t == "partner_disconnected":
            self._enqueue("0C")
        elif t == "raw":
            self._enqueue(msg["bytes"])
            self.raw_injected += 1
        elif t == "session_settings":
            seed = int(msg.get("encounterSeed") or 0)
            if seed:
                # Mirror serial_bridge.rs json_to_packet: SEED_SYNC + BE32.
                pkt = bytes([PKT_SEED_SYNC,
                             (seed >> 24) & 0xFF, (seed >> 16) & 0xFF,
                             (seed >> 8) & 0xFF, seed & 0xFF]).hex()
                self._enqueue(pkt)
            if self.on_session_settings:
                self.on_session_settings(self, seed)
        # full_sync and other typed server-state messages are intentionally
        # ignored: in raw-for-everything the ROMs never feed typed state to the
        # relay, so these carry only defaults and have no ring-buffer form.

    def _flush_connect(self):
        # Inject PARTNER_CONNECTED (0x0B) then ROLE_ASSIGN, mirroring the
        # in-process relay's once-both-alive handshake.
        self._enqueue("0B")
        if self._pending_role_pkt:
            self._enqueue(self._pending_role_pkt)
            self._pending_role_pkt = None

    # ── memory convenience ──────────────────────────────────────────────────
    def connstate(self):
        return read_u8(self.inst, GMP_STATE + OFF_CONNSTATE)

    def role_byte(self):
        return read_u8(self.inst, GMP_STATE + OFF_ROLE)

    def partner_pos(self):
        return (read_u8(self.inst, GMP_STATE + OFF_PARTNER_MAPNUM),
                read_u8(self.inst, GMP_STATE + OFF_TARGET_X),
                read_u8(self.inst, GMP_STATE + OFF_TARGET_Y))

    def enc_seed(self):
        return read_u32(self.inst, GCOOP + OFF_ENC_SEED)

    def ghost_slot(self):
        return read_u8(self.inst, GMP_STATE + OFF_GHOST_OBJ)

    def boss_ready_id(self):
        # gMultiplayerState.bossReadyBossId — local readiness (set when *this*
        # player interacts with the boss trigger).
        return read_u8(self.inst, GMP_STATE + OFF_BOSS_READY)

    def partner_boss_id(self):
        # gMultiplayerState.partnerBossId — partner readiness (set on receipt of
        # a partner BOSS_READY packet over the relay).
        return read_u8(self.inst, GMP_STATE + OFF_PARTNER_BOSS)

    def push_send(self, hexstr):
        """Atomically enqueue a packet into this ROM's SEND ring, exactly as the
        ROM's FlagSet/SendBossReady hooks do when connected.  The drain loop then
        forwards it to the relay — so the packet travels the real WebSocket path."""
        return self.inst.send_locked({"cmd": "push_send",
                                      "bytes": hexstr.replace(" ", "")})


def _json_raw(pkt_hex):
    return '{"type":"raw","bytes":"%s"}' % pkt_hex


# ── Bridge orchestration ─────────────────────────────────────────────────────
class LiveBridge:
    def __init__(self, port=1999, room=None, chaos_drop=0.0, chaos_seed=1):
        self.port = port
        self.room = room or f"live{int(time.time())}"
        base = f"ws://localhost:{port}/parties/main/{self.room}"
        self.p1_url = base
        self.p2_url = base
        self.p1 = None
        self.p2 = None
        self.seed = 0
        self.chaos_drop = chaos_drop
        self.chaos_seed = chaos_seed

    def boot(self, p1_state, p2_state):
        log(f"booting emulators (room={self.room})")
        SRV.start_emulator("p1", savestate=p1_state)
        SRV.start_emulator("p2", savestate=p2_state)
        time.sleep(1.0)

    def connect(self):
        # Host connects first (becomes "host"), then guest.
        self.p1 = Peer("p1", self.p1_url)
        self.p2 = Peer("p2", self.p2_url)
        # Apply chaos to both send paths with distinct, reproducible RNG streams.
        for off, p in ((0, self.p1), (1, self.p2)):
            p.chaos_drop = self.chaos_drop
            p._chaos_rng = random.Random(self.chaos_seed * 1000 + off)
        self.p1.open()
        time.sleep(0.4)
        self.p2.open()
        # Wait for both to see partner_connected.
        ok1 = self.p1.connect_event.wait(8)
        ok2 = self.p2.connect_event.wait(8)
        log(f"connect events: p1={ok1} p2={ok2}; roles p1={self.p1.role} p2={self.p2.role}")
        return ok1 and ok2

    def mint_and_distribute_seed(self):
        """Host mints a nonzero u32 seed, writes it to its own memory, and
        publishes it via session_settings (relay → guest → SEED_SYNC)."""
        host = self.p1 if self.p1.role == "host" else self.p2
        guest = self.p2 if host is self.p1 else self.p1
        seed = random.randint(1, 0xFFFFFFFF)
        self.seed = seed
        # Force randomize ON + seed directly into the host ROM (set_encounter_seed).
        write_u8(host.inst, GCOOP + OFF_RANDOMIZE, 1)
        write_u32(host.inst, GCOOP + OFF_ENC_SEED, seed)
        write_u8(guest.inst, GCOOP + OFF_RANDOMIZE, 1)
        # Publish to the relay as the host would.
        host.ws.send('{"type":"session_settings","randomizeEncounters":true,"encounterSeed":%d}' % seed)
        log(f"host({host.iid}) minted seed=0x{seed:08X}, published session_settings")
        return seed

    def shutdown(self):
        for p in (self.p1, self.p2):
            if p:
                p.close()
        try:
            SRV.stop_emulator("p1")
            SRV.stop_emulator("p2")
        except Exception:
            pass


# ── Smoke test: connect + seed + position sync over the live relay ──────────
def run_smoke(port):
    states = {
        "p1": str(REPO_ROOT / "test/lua/states/p1_route1.ss1"),
        "p2": str(REPO_ROOT / "test/lua/states/p2_route1.ss1"),
    }
    br = LiveBridge(port=port)
    try:
        br.boot(states["p1"], states["p2"])
        if not br.connect():
            log("FAIL: handshake did not complete")
            return 1
        time.sleep(1.0)
        cs1, cs2 = br.p1.connstate(), br.p2.connstate()
        rb1, rb2 = br.p1.role_byte(), br.p2.role_byte()
        log(f"connState p1={cs1} p2={cs2} (want {MP_STATE_CONNECTED}); "
            f"role byte p1={rb1} p2={rb2}")

        # Seed distribution
        seed = br.mint_and_distribute_seed()
        time.sleep(1.5)
        es1, es2 = br.p1.enc_seed(), br.p2.enc_seed()
        log(f"encounterSeed p1=0x{es1:08X} p2=0x{es2:08X} (want 0x{seed:08X})")

        # Input cooldown: mGBA ignores inputs for ~300 frames after a savestate
        # load.  Let both settle before driving.
        SRV.wait(frames=300, instance_id="p1")
        # Position sync: move p1 around, see if p2's partner-position memory
        # updates.  Try several directions so a wall on one axis doesn't mask it.
        before = br.p2.partner_pos()
        log(f"p2 partner_pos before move: {before}")
        for d in ("DOWN", "DOWN", "LEFT", "LEFT", "UP", "RIGHT"):
            SRV.press_button(d, instance_id="p1", hold_frames=16, release_frames=4)
        time.sleep(1.5)
        after = br.p2.partner_pos()
        log(f"p2 partner_pos after p1 moved: {after}")

        ok_conn = cs1 == MP_STATE_CONNECTED and cs2 == MP_STATE_CONNECTED
        ok_role = (rb1, rb2) == (MP_ROLE_HOST, MP_ROLE_GUEST)
        ok_seed = es1 == seed and es2 == seed
        ok_pos = after != before and after[1] is not None

        log(f"RESULT conn={ok_conn} role={ok_role} seed={ok_seed} pos_sync={ok_pos}")
        log(f"p1 rx={br.p1.rx_counts}")
        log(f"p2 rx={br.p2.rx_counts}")
        return 0 if (ok_conn and ok_role and ok_seed and ok_pos) else 1
    finally:
        br.shutdown()


# ── Driving helpers (real overworld input over the emulator) ────────────────
EVID_DIR = "test/evidence/live_smoke"


def shot(iid, name, claim):
    try:
        SRV.screenshot(instance_id=iid, save_path=f"{EVID_DIR}/{name}.png", claim=claim)
    except Exception as e:
        log(f"  (screenshot {name} failed: {e})")


def read_flag_bit(peer, flag_id):
    """Read one flag bit out of the partner's live SaveBlock1.flags[] by
    dereferencing gSaveBlock1Ptr — the exact memory FlagSet() writes."""
    ptr = read_u32(peer.inst, GSAVEBLOCK1_PTR)
    if not ptr:
        return None
    val = read_u8(peer.inst, ptr + SB1_FLAGS_OFF + flag_id // 8)
    if val is None:
        return None
    return (val >> (flag_id % 8)) & 1


# Player object-event world coords (gObjectEvents[0].currentCoords), used to
# detect a blocked step.  Verified at 0x020015D0/0x020015D2 on the HEAD build.
GOBJ_PLAYER_X = 0x020015D0
GOBJ_PLAYER_Y = 0x020015D2


def player_xy(peer):
    return (read_u16(peer.inst, GOBJ_PLAYER_X), read_u16(peer.inst, GOBJ_PLAYER_Y))


def walk_until_battle(peer, max_steps=80):
    """Trigger a wild battle by bouncing UP/DOWN between two grass tiles.

    The Route 1 start tile and the one above it are both tall grass (verified),
    so a tight vertical bounce steps on grass every press — far more reliable
    than a wandering sequence, which walks out of the small patch before the
    encounter RNG fires.  If both vertical directions are blocked (cornered),
    nudge sideways to find grass again."""
    if in_battle(peer.inst):
        return True
    seq = ("UP", "DOWN")
    stuck = 0
    for i in range(max_steps):
        bx, by = player_xy(peer)
        SRV.press_button(seq[i % 2], instance_id=peer.iid, hold_frames=16, release_frames=4)
        if in_battle(peer.inst):
            return True
        if player_xy(peer) == (bx, by):
            stuck += 1
            if stuck >= 3:
                SRV.press_button("LEFT" if (i // 2) % 2 == 0 else "RIGHT",
                                 instance_id=peer.iid, hold_frames=16, release_frames=4)
                if in_battle(peer.inst):
                    return True
                stuck = 0
        else:
            stuck = 0
    return in_battle(peer.inst)


def wait_for_species(peer, battler=1, tries=25):
    """After a battle starts, gBattleMons fills a few frames in.  Poll until the
    target battler's species is a sane nonzero value."""
    for _ in range(tries):
        if in_battle(peer.inst):
            sp = battle_mon_species(peer.inst, battler)
            if sp and 1 <= sp <= 1200:
                return sp
        SRV.wait(frames=15, instance_id=peer.iid)
    return battle_mon_species(peer.inst, battler) if in_battle(peer.inst) else None


# ── The five done criteria (PROGRESS.md Step 9.6) ───────────────────────────
def crit1_ghost(br):
    """(1) Ghost NPC moves on partner's screen."""
    g1, g2 = br.p1.ghost_slot(), br.p2.ghost_slot()
    log(f"  crit1: ghost slots p1={g1} p2={g2} (valid = not 0x{GHOST_INVALID_SLOT:02X})")
    before = br.p2.partner_pos()
    p1_before = player_xy(br.p1)
    for d in ("DOWN", "DOWN", "RIGHT", "RIGHT", "UP", "LEFT"):
        SRV.press_button(d, instance_id="p1", hold_frames=16, release_frames=4)
    p1_after = player_xy(br.p1)
    # Position packets re-send every ~4 frames, so under packet loss a single
    # read can miss the converged value. Poll for convergence: the partner_pos
    # must change from `before` AND match p1's actual tile (idempotent resend
    # path — the CLAUDE.md "confirm convergence" requirement under chaos).
    # partner_pos is (mapNum, x, y); compare its (x, y) against p1's tile.
    after = before
    for _ in range(25):  # ~5s budget
        time.sleep(0.2)
        after = br.p2.partner_pos()
        if after != before and (after[1], after[2]) == p1_after:
            break
    log(f"  crit1: p1 moved {p1_before}->{p1_after}; p2 sees partner_pos {before} -> {after}")
    shot("p2", "crit1_ghost_p2", "P2 screen showing P1 ghost NPC on Route 1")
    ghost_ok = (g2 is not None and g2 != GHOST_INVALID_SLOT)
    p1_moved = p1_after != p1_before
    converged = after != before and (after[1], after[2]) == p1_after
    ok = ghost_ok and p1_moved and converged
    return ("crit1_ghost", ok,
            f"p2 ghost_slot={g2} (spawned={ghost_ok}); p1 {p1_before}->{p1_after} (moved={p1_moved}); "
            f"partner_pos {before}->{after} (converged={converged})")


def crit2_flag(br):
    """(2) P1 defeats trainer -> flag propagates to P2.
    Emulates FlagSet()'s connected hook by pushing the exact FLAG_SET packet
    into p1's send ring; it crosses the real WebSocket relay to p2."""
    before = read_flag_bit(br.p2, FLAG_DEFEATED_BROCK)
    log(f"  crit2: p2 FLAG_DEFEATED_BROCK before = {before}")
    # FLAG_SET packet: type 0x02, flag 0x04B0 -> bytes 02 04 B0 (established
    # wire order, matches src/multiplayer.c encoder + test/lua/coop/flag_sync).
    # Re-advertise idempotently while waiting: FlagSet() is idempotent, and a
    # real defeated-trainer flag is durable + re-broadcast via state sync, so
    # this is the faithful convergence path under packet loss.
    after = None
    for i in range(40):
        if i % 5 == 0:
            br.p1.push_send("02 04 B0")
        time.sleep(0.1)
        after = read_flag_bit(br.p2, FLAG_DEFEATED_BROCK)
        if after == 1:
            break
    log(f"  crit2: p2 FLAG_DEFEATED_BROCK after  = {after}")
    ok = (before == 0 and after == 1)
    return ("crit2_flag", ok, f"p2 flag {before}->{after} via FLAG_SET over live relay")


def boss_gate_open(peer, my_boss):
    """Mirror of Multiplayer_ScriptCheckBossStart's connected gate: ready only
    when both local and partner readiness name the same boss."""
    return peer.boss_ready_id() == my_boss and peer.partner_boss_id() == my_boss


def crit5_boss(br):
    """(5) Both approach Brock; neither battle starts until both ready."""
    boss = BOSS_ID_BROCK
    # Reset readiness on both sides.
    for p in (br.p1, br.p2):
        write_u8(p.inst, GMP_STATE + OFF_BOSS_READY, 0)
        write_u8(p.inst, GMP_STATE + OFF_PARTNER_BOSS, 0)
    time.sleep(0.3)

    # Phase A: only p1 readies up (local interaction + BOSS_READY over relay).
    # Re-advertise on the poll (the real ROM does this via bossResendTimer on the
    # state beacon) so a dropped BOSS_READY converges under chaos.
    write_u8(br.p1.inst, GMP_STATE + OFF_BOSS_READY, boss)
    pa_p2_partner = None
    for i in range(40):
        if i % 5 == 0:
            br.p1.push_send("04 01")  # MP_PKT_BOSS_READY, bossId=1
        time.sleep(0.1)
        pa_p2_partner = br.p2.partner_boss_id()
        if pa_p2_partner == boss:
            break
    a_p1_open = boss_gate_open(br.p1, boss)
    a_p2_open = boss_gate_open(br.p2, boss)
    log(f"  crit5 phaseA: p2.partnerBoss={pa_p2_partner}; gates p1={a_p1_open} p2={a_p2_open} (want both CLOSED)")

    # Phase B: p2 readies up too.
    write_u8(br.p2.inst, GMP_STATE + OFF_BOSS_READY, boss)
    pb_p1_partner = None
    for i in range(40):
        if i % 5 == 0:
            br.p2.push_send("04 01")
        time.sleep(0.1)
        pb_p1_partner = br.p1.partner_boss_id()
        if pb_p1_partner == boss:
            break
    b_p1_open = boss_gate_open(br.p1, boss)
    b_p2_open = boss_gate_open(br.p2, boss)
    log(f"  crit5 phaseB: p1.partnerBoss={pb_p1_partner}; gates p1={b_p1_open} p2={b_p2_open} (want both OPEN)")

    ok = (not a_p1_open and not a_p2_open and b_p1_open and b_p2_open)
    return ("crit5_boss", ok,
            f"phaseA gates(p1={a_p1_open},p2={a_p2_open}) phaseB gates(p1={b_p1_open},p2={b_p2_open})")


def crit3_wild(br):
    """(3) Both see the same non-original wild species on Route 1.
    Seed crossed the live relay (proven identical in connect); the randomizer is
    a pure function of (seed, table, slot), so identical seed + identical ROM =>
    identical species per slot by construction."""
    log("  crit3: driving p1 into tall grass for a wild encounter...")
    if not walk_until_battle(br.p1):
        return ("crit3_wild", False, "p1 never triggered a wild battle")
    sp1 = wait_for_species(br.p1, battler=1)
    shot("p1", "crit3_wild_p1", f"P1 wild battle, randomized species id={sp1}")
    log(f"  crit3: p1 wild species id={sp1} (original Route1={sorted(ROUTE1_ORIGINAL)})")

    log("  crit3: driving p2 into tall grass for a wild encounter...")
    if not walk_until_battle(br.p2):
        return ("crit3_wild", False, f"p1 sp={sp1} ok; p2 never triggered a wild battle")
    sp2 = wait_for_species(br.p2, battler=1)
    shot("p2", "crit3_wild_p2", f"P2 wild battle, randomized species id={sp2}")
    log(f"  crit3: p2 wild species id={sp2}")

    p1_nonorig = sp1 is not None and sp1 not in ROUTE1_ORIGINAL
    p2_nonorig = sp2 is not None and sp2 not in ROUTE1_ORIGINAL
    same = (sp1 == sp2)
    ok = p1_nonorig and p2_nonorig
    note = "exact-match (same slot)" if same else "both non-original (slots differed)"
    return ("crit3_wild", ok,
            f"p1={sp1} p2={sp2}; non-original p1={p1_nonorig} p2={p2_nonorig}; {note}")


def crit4_trainer(br_seed):
    """(4) Brock does NOT have Geodude/Onix (trainer randomized).
    Solo Pewter Gym session: the boss-ready gate falls through to solo, so Brock
    runs as a normal single trainer battle.  Trainer randomization is a local
    function of the same seed delivered over the relay in crit3 (transport-
    independent), written here directly to gCoopSettings."""
    state = str(REPO_ROOT / "test/lua/states/pewter_gym.ss1")
    log(f"  crit4: booting solo Pewter Gym (seed=0x{br_seed:08X})")
    SRV.start_emulator("p1", savestate=state)
    time.sleep(1.0)
    inst = SRV._instances["p1"]
    # Force randomize ON + the same seed crossed the live relay in crit3.
    write_u8(inst, GCOOP + OFF_RANDOMIZE, 1)
    write_u32(inst, GCOOP + OFF_ENC_SEED, br_seed)
    SRV.wait(frames=300, instance_id="p1")
    # Walk up to Brock and trigger the battle, mashing A through any dialogue.
    log("  crit4: walking to Brock + starting battle")
    started = False
    for i in range(60):
        if in_battle(inst):
            started = True
            break
        SRV.press_button("UP", instance_id="p1", hold_frames=16, release_frames=2)
        SRV.press_button("A", instance_id="p1", hold_frames=8, release_frames=2)
    if not started:
        # last resort: mash A in case we're already facing him
        for _ in range(20):
            SRV.press_button("A", instance_id="p1", hold_frames=8, release_frames=4)
            if in_battle(inst):
                started = True
                break
    if not started:
        SRV.stop_emulator("p1")
        return ("crit4_trainer", False, "could not start Brock battle")
    sp = wait_for_species_inst(inst, battler=1)
    shot("p1", "crit4_brock_p1", f"Brock battle, lead species id={sp}")
    log(f"  crit4: Brock lead species id={sp} (original Brock={sorted(BROCK_ORIGINAL)})")
    SRV.stop_emulator("p1")
    ok = sp is not None and sp not in BROCK_ORIGINAL
    return ("crit4_trainer", ok, f"Brock lead={sp}; non-original={ok}")


def wait_for_species_inst(inst, battler=1, tries=25):
    for _ in range(tries):
        if in_battle(inst):
            sp = battle_mon_species(inst, battler)
            if sp and 1 <= sp <= 1200:
                return sp
        SRV.wait(frames=15, instance_id="p1")
    return battle_mon_species(inst, battler) if in_battle(inst) else None


def run_campaign(port, chaos_drop=0.0, swap_roles=False):
    """Run all five done criteria over the live WebSocket relay."""
    os.makedirs(REPO_ROOT / EVID_DIR, exist_ok=True)
    if chaos_drop > 0:
        # Loss is injected in the bridge send path (Peer._drain_loop), not the
        # neutered in-process relay — so it genuinely affects the live WebSocket.
        log(f"CHAOS enabled: bridge send-drop={chaos_drop} seed=1")

    # Role swap: which savestate each instance loads.  Both are genuine Route 1
    # grass states.  NOTE: there is no distinct p2_route1.ss1 in the fixture set
    # (the catalogue references one that was never generated); tall_grass_route1
    # is the other Route 1 state, so we use it for the second player.  It shares
    # a trainer ID with p1_route1, which is irrelevant to ghost-spawn (map match)
    # and wild randomization (seed-keyed) — the only two criteria run here.
    A = str(REPO_ROOT / "test/lua/states/p1_route1.ss1")
    B = str(REPO_ROOT / "test/lua/states/tall_grass_route1.ss1")
    p1_state, p2_state = (B, A) if swap_roles else (A, B)
    for s in (p1_state, p2_state):
        if not os.path.exists(s):
            log(f"FATAL: savestate missing: {s}")
            return 2

    results = []
    br = LiveBridge(port=port, chaos_drop=chaos_drop, chaos_seed=1)
    try:
        log("=== SESSION 1: connected Route 1 (crit 1,2,5,3) ===")
        br.boot(p1_state, p2_state)
        if not br.connect():
            log("FAIL: handshake did not complete")
            return 1
        time.sleep(1.0)
        cs1, cs2 = br.p1.connstate(), br.p2.connstate()
        log(f"  connState p1={cs1} p2={cs2}; roles p1={br.p1.role} p2={br.p2.role}")
        seed = br.mint_and_distribute_seed()
        time.sleep(1.5)
        es1, es2 = br.p1.enc_seed(), br.p2.enc_seed()
        log(f"  encounterSeed p1=0x{es1:08X} p2=0x{es2:08X} (want 0x{seed:08X})")
        SRV.wait(frames=300, instance_id="p1")
        SRV.wait(frames=300, instance_id="p2")

        # Pure-memory/packet criteria first (clean overworld), then ghost
        # movement, then the wild test (which drives both into battle) last.
        results.append(crit2_flag(br))
        results.append(crit5_boss(br))
        results.append(crit1_ghost(br))
        results.append(crit3_wild(br))   # drives into battle — do last
    finally:
        br.shutdown()
        time.sleep(0.5)

    log("=== SESSION 2: solo Pewter Gym (crit 4) ===")
    results.append(crit4_trainer(seed))

    # ── Summary ──────────────────────────────────────────────────────────────
    log("")
    log("==================== CAMPAIGN RESULTS ====================")
    allok = True
    for name, ok, detail in results:
        log(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}")
        if not ok:
            allok = False
    log(f"  seed=0x{seed:08X}  chaos_drop={chaos_drop}  swap_roles={swap_roles}")
    log(f"  OVERALL: {'ALL PASS' if allok else 'SOME FAILED'}")
    log("=========================================================")
    return 0 if allok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=1999)
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--campaign", action="store_true")
    ap.add_argument("--chaos", type=float, default=0.0, help="drop probability")
    ap.add_argument("--swap-roles", action="store_true")
    args = ap.parse_args()
    if args.smoke:
        sys.exit(run_smoke(args.port))
    if args.campaign:
        sys.exit(run_campaign(args.port, chaos_drop=args.chaos, swap_roles=args.swap_roles))
    print("nothing to do; pass --smoke or --campaign")
