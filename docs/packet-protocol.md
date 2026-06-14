# Multiplayer Packet Protocol

Binary packet format for the co-op ring-buffer interface.
All values are big-endian except where noted.

## Transport Layer

Two EWRAM ring buffers connect the ROM to the Tauri host app:

| Buffer | Writer | Reader |
|---|---|---|
| `gMpSendRing` | ROM | Tauri → relay server |
| `gMpRecvRing` | Tauri (from relay) | ROM |

Each buffer is 256 bytes with `u8` head/tail pointers that wrap at 256
naturally via unsigned 8-bit overflow.  The `magic` field (`0xC0`) lets
the Tauri app verify it has found the correct buffer in EWRAM.

## Packet Types

Every packet begins with a 1-byte type field.

| Hex | Name | Total size | Notes |
|---|---|---|---|
| `0x01` | POSITION | 6 B | Sent every 4 frames when connected |
| `0x02` | FLAG_SET | 3 B | Trainer/story flag was set |
| `0x03` | VAR_SET | 5 B | Script variable changed |
| `0x04` | BOSS_READY | 2 B | Player is ready to start boss battle |
| `0x05` | BOSS_CANCEL | 1 B | Player walked away from boss trigger |
| `0x06` | SEED_SYNC | 5 B | Encounter randomization seed |
| `0x07` | FULL_SYNC | 3 + N B | Complete flag/var state dump (variable) |

All fixed-size packets fit in a single 256-byte ring buffer pass.
FULL_SYNC payloads up to ~250 bytes also fit in one ring pass.

---

## Packet Layouts

### POSITION (0x01) — 6 bytes

```
Byte 0: type = 0x01
Byte 1: mapGroup
Byte 2: mapNum
Byte 3: x  (tile X, 0–99)
Byte 4: y  (tile Y, 0–99)
Byte 5: facing  (DIR_SOUTH=1, DIR_NORTH=2, DIR_WEST=3, DIR_EAST=4)
```

Sent by the ROM every 4 frames while connected.
`mapGroup` and `mapNum` together uniquely identify the current map.
On receipt, the ghost NPC is spawned/moved/despawned as needed.

### FLAG_SET (0x02) — 3 bytes

```
Byte 0: type = 0x02
Byte 1: flagId >> 8  (high byte)
Byte 2: flagId & 0xFF
```

Emitted whenever a syncable flag is set locally.
On receipt, the remote ROM calls `FlagSet(flagId)`.
Only flags in the trainer/story whitelist are sent (see Phase 3).

### VAR_SET (0x03) — 5 bytes

```
Byte 0: type = 0x03
Byte 1: varId >> 8
Byte 2: varId & 0xFF
Byte 3: value >> 8
Byte 4: value & 0xFF
```

Emitted for syncable script variable changes.
On receipt, the remote ROM calls `VarSet(varId, value)`.

### BOSS_READY (0x04) — 2 bytes

```
Byte 0: type = 0x04
Byte 1: bossId  (gym leader / story boss identifier)
```

Sent when a player interacts with the boss readiness trigger.
The relay server collects both players' `boss_ready` messages and
sends `boss_start` once both have arrived (handled in Phase 5).

### BOSS_CANCEL (0x05) — 1 byte

```
Byte 0: type = 0x05
```

Sent when a player walks away from the boss trigger before both
players have confirmed ready.

### SEED_SYNC (0x06) — 5 bytes

```
Byte 0: type = 0x06
Byte 1: seed >> 24
Byte 2: (seed >> 16) & 0xFF
Byte 3: (seed >> 8)  & 0xFF
Byte 4: seed & 0xFF
```

Host sends this on session start; guest receives it and stores it in
`gCoopSettings.encounterSeed`.  Big-endian u32.

### FULL_SYNC (0x07) — variable length

```
Byte 0:   type = 0x07
Byte 1:   dataLen >> 8
Byte 2:   dataLen & 0xFF
Byte 3…N: data[0..dataLen-1]
```

Sent by the host to a guest on connect, containing a snapshot of all
set syncable flags and variables.  The guest applies this dump before
starting gameplay so late joiners catch up.

`dataLen` is the number of payload bytes following the 3-byte header.
Maximum payload ≈ 172 bytes (trainer flag bitmap), well within the
256-byte ring.

---

## Error Handling

- **Truncated packets**: if fewer bytes are available than the declared
  packet size, `ProcessOneRecvPacket()` returns `FALSE` and leaves the
  partial bytes in the ring.  On the next frame the ROM retries.
- **Unknown type**: the receiver drains the entire ring to re-sync.
  This is safe because all senders are under our control.
- **Ring full**: `MpRing_Write()` drops the packet silently rather than
  writing a partial packet.  The sender may retry on the next frame.

---

## Design Constraints

- All fixed-size packets ≤ 6 bytes — a position burst every 4 frames
  consumes only 6 B/frame × 60 fps / 4 = 90 B/s, well under the ring.
- FULL_SYNC is the only variable-length packet; it is sent once on
  connect, not repeatedly.
- No framing delimiter is needed — each type byte implicitly defines the
  packet length (or the 2-byte length field for FULL_SYNC).

---

## Later additions (canonical list: `include/constants/multiplayer.h`)

Packet types `0x08`–`0x1B` were added after this document was first
written; the constants header is the authoritative size table (mirrored
in `serial_bridge.rs`, `relay-server/src/server.ts`, and the MCP
server's `_PKT_SIZES`).  Several are documented here because their
layouts are non-obvious:

### PARTY_SYNC (0x12) — 2 + n×58 + 4 bytes

```
Byte 0: type = 0x12
Byte 1: n_mons (≤ 3)
Then n_mons × 58-byte wire mons (struct MpWirePartyMon, big-endian):
  +0  species     u16     +14 moves[4]  u16×4    +30 atk    u16
  +2  heldItem    u16     +22 pp[4]     u8×4     +32 def    u16
  +4  level       u8      +26 hp        u16      +34 speed  u16
  +5  abilityNum  u8      +28 maxHP     u16      +36 spAtk  u16
  +6  personality u32                            +38 spDef  u16
  +10 otId        u32                            +40 status u32
  +44 friendship u8   +45 gender u8   +46 language u8   +47 nickname[11]
Then a trailing u32 (big-endian): coop battle RNG seed.
```

The receiving ROM rebuilds a battle-identical mon in the partner half of
`gPlayerParty` from these fields.  Every stat must be on the wire:
`CreateMon()` in this expansion does **not** compute stats, and a
reconstructed mon left at 0 HP is flagged absent by
`TryDoEventsBeforeFirstTurn` — battler 2 then silently drops out of the
co-op battle.

The trailing seed feeds the coop battle RNG lockstep stream: the host
mints a fresh nonzero seed per battle (`CB2_CoopPartySelected`), guests
always send 0, and the receiver adopts any nonzero value.  Resends of
the same battle's PARTY_SYNC carry the same seed.

### BATTLE_TURN (0x14) — 5 bytes

```
Byte 0: type = 0x14
Byte 1: seq       (1-255, never 0; +1 per logical turn, reset per battle)
Byte 2: move slot (0-3)
Byte 3: target battler (sender's index space; ally 0↔2 mirrored on receipt)
Byte 4: flags     (bit 0 = gimmick)
```

The receiver applies a turn only if its seq is strictly newer than the
last applied seq (wraparound-aware), so duplicates and reordered stale
turns are no-ops.  Loss recovery: the sender's STATE_BEACON re-carries
the cached turn (same seq) every beacon interval while in a coop battle.

### STATE_BEACON (0x1A) — 9 bytes

```
Byte 0: type = 0x1A
Byte 1: gender | ack<<7      Byte 5: turn_seq  (0 = no cached battle turn)
Byte 2: starter species hi  Byte 6: move slot
Byte 3: starter species lo  Byte 7: target battler
Byte 4: boss-ready id       Byte 8: flags
```

Sent every 16 frames while connected.  Idempotent repair channel for all
dropped one-shot exchanges (gender, starter pick, boss readiness, battle
turn).  Bytes 5-8 are zero unless the sender is in a coop battle with an
unconsumed cached turn; the receiver feeds them through the same seq
dedup as a direct BATTLE_TURN and ignores them when not in a coop battle
itself.

Byte 1 bit 7 (`MP_BEACON_PARTYACK_BIT`) is the **party-sync ack**: set once
the sender has received the partner's PARTY_SYNC for the current battle.  The
low bits remain the player gender (receivers strip bit 7 before decoding it).
PARTY_SYNC has no resend timer of its own — its reliability rides this beacon
bit: `waitpartysync` keeps resending the local party every 60 frames and does
not complete until the local side both has the partner's party AND sees this
ack, so the resend survives until the partner confirms receipt.  Without it,
the side that received first stopped resending while the other waited forever
(asymmetric-loss deadlock, fixed 2026-06-14).  Packed into a spare bit so the
wire size stays 9 bytes — bridge/relay/MCP framing tables are unchanged.

### ROLE_ASSIGN (0x1B) — 2 bytes

```
Byte 0: type = 0x1B
Byte 1: role  (1 = MP_ROLE_HOST, 2 = MP_ROLE_GUEST)
```

Relay → ROM only, sent at session start (and re-sent after savestate
loads / reconnects on the MCP relay).  Gives `GetMultiplayerId()` /
`IsLinkMaster()` a real answer; without it both ROMs sit at
`MP_ROLE_NONE` and neither side is link master.
