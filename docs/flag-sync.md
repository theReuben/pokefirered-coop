# Flag Sync Design

Explains which game flags are synced between players in the co-op mod and why.

---

## Principles

- **One shared world**: trainer battles, story events, and item pickups are
  shared. If Player 1 beats a trainer, Player 2's game reflects the defeat too.
- **Per-player state is not synced**: badges earned in the UI, the PC box,
  bag items, active effects like safari mode — these remain independent.
- **Temp flags are never synced**: `FLAG_TEMP_*` (0x000–0x01F) are cleared
  every time a map loads and represent transient per-instance dialogue state.

---

## Syncable Flag Ranges

Defined in `include/constants/multiplayer.h`. `IsSyncableFlag()` in
`src/multiplayer.c` tests all ranges.

| Range | Hex | Name | What's in it |
|---|---|---|---|
| Story | 0x020 – 0x2FF | `SYNC_FLAG_STORY_START/END` | NPC hide/show flags, overworld item ball pickups, story quest events (Got HM, rescued Fuji, etc.) |
| Hidden items | 0x3E8 – 0x4A6 | `SYNC_FLAG_ITEMS_START/END` | Ground-item pickup flags (A-button spots) |
| Bosses | 0x4B0 – 0x4BC | `SYNC_FLAG_BOSSES_START/END` | Gym leaders (Brock–Giovanni) + Elite Four + Champion |
| Trainers | 0x500 – 0x7FF | `SYNC_FLAG_TRAINERS_START/END` | Individual trainer defeat flags |

---

## Ranges Explicitly NOT Synced

| Range | Reason |
|---|---|
| 0x000 – 0x01F (TEMP_FLAGS) | Cleared on map load; transient per-instance state |
| 0x300 – 0x38E (unnamed gap) | Unused in vanilla FRLG; skipping avoids surprises |
| 0x390 – 0x3CF (DAILY_FLAGS) | Per-player daily event state |
| 0x3D8 – 0x3E7 (MYSTERY_GIFT) | Per-player event tickets |
| 0x800+ (SYS_FLAGS) | Local: safari mode, VS seeker charging, etc. |

---

## Flag Sync Wire Protocol

### On connect (FULL_SYNC)

The host sends a `FULL_SYNC` packet (type `0x07`) containing a bitmap of all
currently-set syncable flags. The bitmap is packed: each flag is one bit,
and only syncable flags (tested via `IsSyncableFlag`) are included.

The compact representation encodes four ranges as separate bitmaps:

```
FULL_SYNC payload layout (variable length):
  [0]     : range count (always 4)
  For each range (story, items, bosses, trainers):
    [+0]  : rangeStart_hi
    [+1]  : rangeStart_lo
    [+2]  : rangeEnd_hi
    [+3]  : rangeEnd_lo
    [+4]  : byteCount (ceil((rangeEnd - rangeStart + 1) / 8))
    [+5…] : packed flag bits (bit 0 of byte 0 = rangeStart)
```

Maximum payload:
- Story range (0x020–0x2FF): 736 flags = 92 bytes
- Items range (0x3E8–0x4A6): 191 flags = 24 bytes
- Bosses range (0x4B0–0x4BC): 13 flags = 2 bytes
- Trainers range (0x500–0x7FF): 768 flags = 96 bytes
- Headers: 4 × 5 = 20 bytes
- Total: ≤ 234 bytes — fits in one ring-buffer pass (256 bytes)

### Incremental updates (FLAG_SET)

After connect, whenever a syncable flag is set locally, the ROM sends a
`FLAG_SET` packet (type `0x02`) containing the 2-byte flag ID.

On receipt, the remote ROM calls `FlagSet(flagId)` only if `IsSyncableFlag`
returns true (defence-in-depth guard).

### No-rebroadcast guard

A boolean `sIsRemoteUpdate` in `src/event_data.c` prevents the remote
`FlagSet()` call from triggering another outgoing `FLAG_SET` packet,
which would cause an infinite echo loop.

---

## IsSyncableFlag

```c
bool32 IsSyncableFlag(u16 flagId)
{
    return (flagId >= SYNC_FLAG_STORY_START    && flagId <= SYNC_FLAG_STORY_END)
        || (flagId >= SYNC_FLAG_ITEMS_START    && flagId <= SYNC_FLAG_ITEMS_END)
        || (flagId >= SYNC_FLAG_BOSSES_START   && flagId <= SYNC_FLAG_BOSSES_END)
        || (flagId >= SYNC_FLAG_TRAINERS_START && flagId <= SYNC_FLAG_TRAINERS_END);
}
```

---

## Edge Cases

- **Late joiner**: guest receives `FULL_SYNC` on connect and applies all
  flags the host already has set. Union-wins: if the guest already has a
  flag set, it stays set.
- **Simultaneous flag set**: both players defeat the same trainer in the
  same second. Both send `FLAG_SET`; both apply the remote set. Idempotent
  — `FlagSet` on an already-set flag is a no-op.
- **Reconnect**: on reconnect, a fresh `FULL_SYNC` is exchanged. The guest
  applies host's current state; the host applies guest's state via the
  `FULL_SYNC` sent by the guest.
