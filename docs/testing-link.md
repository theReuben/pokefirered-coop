# Testing the Link (Two-Instance)

How to verify that two ROM instances exchange position data and the ghost
NPC moves correctly, before the Tauri app is ready.

---

## Prerequisites

- `pokefirered.gba` built (`make firered -j4`)
- mGBA with link-cable networking support (any recent release)
- `test/lua/memory_map.lua` up to date (see [Regenerating the memory map](#regenerating-the-memory-map))

---

## Running Two mGBA Instances via Link Cable

mGBA supports local link-cable emulation over a Unix socket or TCP.

### macOS / Linux (socket)

Open two terminals:

```
# Instance 1 (Player 1)
mgba pokefirered.gba --link-host

# Instance 2 (Player 2) — connects to Instance 1
mgba pokefirered.gba --link-client
```

mGBA will show a "Link connected" message in the status bar when the
connection is established.

### Windows (TCP)

Use mGBA's **Tools → Multiplayer** window instead:

1. Open Instance 1, go to Tools → Multiplayer → Start server (port 7110).
2. Open Instance 2, go to Tools → Multiplayer → Connect to `127.0.0.1:7110`.

---

## What to Check Manually

Once both instances are linked and the ROM has booted:

1. **Ring buffer initialised** — in mGBA's memory viewer, navigate to
   `0x02031454` (gMpSendRing) and `0x02031350` (gMpRecvRing). The byte at
   offset +258 (`magic` field) should read `0xC0`.

2. **Position packets sent** — move Player 1 around on Route 1. Watch
   the `gMpSendRing` buffer in the memory viewer: bytes should be written
   every 4 frames (head pointer advances by 6 per step).

3. **Ghost NPC appears** — if both ROMs are on the same map, the ghost NPC
   (Green/Leaf sprite) should appear on Player 2's screen at Player 1's
   tile position. Verify with the debug ghost flag first:
   set `MP_DEBUG_TEST_GHOST=1` in `include/multiplayer.h`, rebuild, and
   boot — a ghost should appear on Route 1 at tile (8, 5) immediately.

4. **Ghost moves** — walk Player 1 to a new tile. On Player 2's screen the
   ghost should walk to match within ~1 frame (one tile per Multiplayer_Update call).

5. **Cross-map despawn** — warp Player 1 to a different map. The ghost
   should disappear from Player 2's screen immediately.

6. **No collision with ghost** — Player 2 should be unable to walk through
   the ghost NPC (MOVEMENT_TYPE_NONE is collidable).

7. **No dialogue on ghost** — pressing A while facing the ghost should
   produce no dialogue or interaction.

---

## Lua Scripting for Automated Checks

mGBA supports Lua scripts that can read memory and assert conditions.
See [mGBA Scripting API](https://mgba.io/docs/scripting.html).

Load `test/lua/memory_map.lua` in a script to get symbol addresses:

```lua
local sym = require("memory_map")

-- Check ring magic
local sendMagic = memory.read_u8(sym.gMpSendRing + 258)
assert(sendMagic == 0xC0, "gMpSendRing magic mismatch")

-- Check ghost state (ghostObjectEventId field at offset 7 in MultiplayerState)
local ghostId = memory.read_u8(sym.gMultiplayerState + 7)
print("ghostObjectEventId = " .. ghostId)
```

The offsets for `MultiplayerState` fields are:

| Field | Offset |
|---|---|
| role | 0 |
| connState | 1 |
| partnerMapGroup | 2 |
| partnerMapNum | 3 |
| targetX | 4 |
| targetY | 5 |
| targetFacing | 6 |
| ghostObjectEventId | 7 |
| bossReadyBossId | 8 |
| isInScript | 9 |
| partnerIsInScript | 10 |
| posFrameCounter | 11 |

### Flag Sync and Script Mutex Tests

A dedicated Lua test script is available at `test/lua/test_flag_sync.lua`.
Load it in the scripting console to verify:

- Ring buffer magic bytes are correct
- `partnerIsInScript` starts `FALSE`
- Writing `SCRIPT_LOCK` to `gMpRecvRing` sets `partnerIsInScript` after one frame
- Writing `SCRIPT_UNLOCK` clears it
- Defeating a trainer writes a `FLAG_SET` packet to `gMpSendRing`

---

## Regenerating the Memory Map

Run this after every ROM rebuild:

```bash
python3 tools/extract_symbols.py pokefirered.map test/lua/memory_map.lua
```

This re-parses the linker map and updates `test/lua/memory_map.lua` with
current addresses for `gMultiplayerState`, `gMpSendRing`, `gMpRecvRing`,
and `gCoopSettings`.

---

## Known Limitations (Phase 2)

- The ROM does not yet read from `gMpRecvRing` via actual link cable
  hardware bytes — it reads from the EWRAM ring directly. Tauri will write
  to this ring via libmgba memory access. For mGBA link tests, you need
  to write to the ring buffer manually via the memory viewer or a Lua script.
- Full two-way live exchange requires the Tauri app (Phase 6). The mGBA
  link tests in this phase focus on verifying the ring buffer layout and
  ghost NPC rendering.
