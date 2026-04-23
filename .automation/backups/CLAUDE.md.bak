# Pokémon FireRed Co-Op Multiplayer Mod

## Project Overview

This is a networked 2-player co-op mod built on top of `rh-hideout/pokeemerald-expansion` with FireRed/LeafGreen mode enabled. Two players play through Kanto simultaneously in a shared world with randomized encounters from all generations.

### Core Principles

- **One shared world:** Both players exist in the same logical game state. Beating a trainer or completing a story event updates both players' games.
- **Independent movement:** Each player controls their own character, moves freely, and can be on different maps.
- **Synced randomization:** Wild encounters are randomized from the full national dex using a shared seed, so both players see the same encounter tables.
- **Boss double battles:** Gym leaders and key story battles require both players to ready up, then fight as a double battle with both players' parties.
- **Simple UX:** End users download one app, press "Host" or "Join", enter a room code, and play. No technical knowledge required.

## Architecture

```
┌─────────────────────┐         ┌─────────────────────┐
│   Player 1 (Tauri)  │         │   Player 2 (Tauri)  │
│  ┌───────────────┐  │         │  ┌───────────────┐  │
│  │  Modified ROM  │  │         │  │  Modified ROM  │  │
│  │  (libmgba)    │  │         │  │  (libmgba)    │  │
│  └──────┬────────┘  │         │  └──────┬────────┘  │
│         │ serial/   │         │         │ serial/   │
│         │ link cable│         │         │ link cable│
│  ┌──────▼────────┐  │         │  ┌──────▼────────┐  │
│  │  Net Adapter   │  │         │  │  Net Adapter   │  │
│  │  (WebSocket)  │  │         │  │  (WebSocket)  │  │
│  └──────┬────────┘  │         │  └──────┬────────┘  │
└─────────┼───────────┘         └─────────┼───────────┘
          │                               │
          │  wss://project.partykit.dev   │
          │         ┌─────────┐           │
          └────────►│  Relay  │◄──────────┘
                    │ Server  │
                    │(PartyKit)│
                    └─────────┘
```

### Communication Flow

1. ROM serializes player state (position, map, flags) into the GBA's serial buffer
2. The Tauri app intercepts serial I/O from libmgba instead of sending it over a real link cable
3. Tauri's net adapter sends it as a WebSocket message to the PartyKit relay server
4. The relay server forwards it to the partner's Tauri app
5. The partner's net adapter writes the data into their ROM's serial receive buffer
6. The partner's ROM reads it and updates the ghost NPC / shared flags / battle state

### Message Protocol

All messages are JSON over WebSocket. The ROM side packs/unpacks binary packets through the serial interface; the Tauri net adapter translates between binary serial data and JSON.

**Outbound (ROM → Server):**
- `position` — player map ID, x, y, facing direction, sprite state (sent every ~4 frames)
- `flag_set` — a trainer/story flag was set (flag ID)
- `var_set` — a script variable changed (var ID + value)
- `boss_ready` — player interacted with a boss trigger and is ready (boss ID)
- `boss_cancel` — player walked away from boss trigger
- `battle_turn` — turn selection during synced double battle (encoded turn data)
- `party_sync` — full party data snapshot (for double battle partner display)

**Inbound (Server → ROM):**
- `role` — whether this client is host or guest
- `partner_position` — other player's position data
- `flag_set` / `var_set` — shared state update from partner
- `full_sync` — complete flag/var state dump (sent on connect)
- `boss_start` — both players ready, begin double battle
- `boss_waiting` — you're ready, partner isn't yet
- `battle_turn` — partner's turn selection
- `partner_connected` / `partner_disconnected`

## Repository Structure (Key Paths)

### ROM Side (pokeemerald-expansion with FRLG mode)

These are the files most relevant to the multiplayer mod. Do NOT modify files unnecessarily — keep changes minimal and well-contained.

**Overworld & Player:**
- `src/event_object_movement.c` — object event (NPC/player) movement and animation
- `src/field_player_avatar.c` — player avatar state, input handling, movement
- `src/overworld.c` — main overworld loop, map loading, state transitions
- `src/event_data.c` — flag and variable get/set functions
- `include/constants/flags.h` — flag ID definitions
- `include/constants/vars.h` — variable ID definitions

**Link/Serial Communication:**
- `src/link.c` — link cable communication layer (THIS IS THE MAIN HOOK POINT)
- `src/link_rfu.c` — wireless adapter communication
- `include/link.h` — link data structures

**Battle System:**
- `src/battle_setup.c` — battle initiation, trainer encounters
- `src/battle_main.c` — core battle loop
- `src/battle_controllers.c` — input handling during battle
- `src/battle_controller_player.c` — player-side battle controller
- `src/battle_controller_player_partner.c` — partner controller (for multi battles)

**Wild Encounters:**
- `src/wild_encounter.c` — wild encounter generation
- `src/data/wild_encounters.h` — encounter table data

**Scripts:**
- `data/maps/*/scripts.inc` — per-map event scripts
- `src/scrcmd.c` — script command implementations
- `src/script.c` — script execution engine

### Relay Server (separate repo)

- `server.ts` — PartyKit relay server (~150 lines)
- `package.json`

### Tauri App Wrapper (separate repo)

- `src-tauri/` — Rust backend (embeds libmgba, intercepts serial, manages WebSocket)
- `src/` — Frontend UI (host/join screen, room code entry)

## Implementation Guidelines

### Adding the Ghost NPC (Player 2's Avatar)

Player 2 appears as a special object event on each map. Key points:

- Create a new object event type (e.g., `OBJ_EVENT_GFX_PLAYER2`) with a distinct sprite
- The ghost NPC is NOT defined in map data — it's spawned dynamically when the partner is on the same map
- Update its position from incoming `partner_position` messages every few frames
- It should be collidable (use existing NPC collision) but NOT interactable (no script trigger)
- When the partner is on a different map, remove the ghost NPC from the current map's object events
- Handle map transitions: when receiving a `partner_position` with a different map ID, despawn the ghost; when the map ID matches again, respawn it

### Shared Flag/Variable Sync

- Hook into `FlagSet()` and `VarSet()` in `src/event_data.c`
- When a flag/var is set locally, also emit it through the serial link as a `flag_set` / `var_set` message
- When receiving a `flag_set` / `var_set` from the partner, call the same functions to apply locally
- NOT all flags should sync — only trainer defeated flags, story progress flags, and item obtained flags
- Create a whitelist of syncable flag ranges to avoid syncing things like player-local state (badge menu, UI flags)
- The `full_sync` message on connect ensures a late-joining player catches up

### Syncable Flag Ranges (Define These)

```c
// in include/constants/multiplayer.h
#define SYNC_FLAG_TRAINERS_START    FLAG_TRAINER_FLAG_START
#define SYNC_FLAG_TRAINERS_END      FLAG_TRAINER_FLAG_END
#define SYNC_FLAG_STORY_START       FLAG_HIDE_... // define range
#define SYNC_FLAG_STORY_END         FLAG_HIDE_... // define range

static inline bool32 IsSyncableFlag(u16 flagId) {
    return (flagId >= SYNC_FLAG_TRAINERS_START && flagId <= SYNC_FLAG_TRAINERS_END)
        || (flagId >= SYNC_FLAG_STORY_START && flagId <= SYNC_FLAG_STORY_END);
}
```

### Script Mutex

Only one player can be in a script interaction at a time. When a player triggers a script:

1. Set a local `gIsInScript` flag
2. While `gIsInScript` is true, the ghost NPC's interaction should be blocked on the partner's side too (send a `script_lock` message)
3. This prevents both players from talking to the same NPC simultaneously and corrupting script state
4. Keep it simple: the lock is advisory, not enforced by the server

### Randomized Encounters

- On session start, the host generates a random seed and sends it to the server
- The server forwards the seed to the guest on connect
- Both ROMs use this seed to deterministically shuffle the encounter tables at runtime
- The shuffle function should be called once during game init, AFTER receiving the seed
- Use species from the full national dex available in the expansion
- Maintain level scaling from the original encounter tables — only randomize species, not levels
- Optionally: ensure each route has a mix of types and evolution stages for variety

```c
// Pseudocode for encounter randomization
void RandomizeEncounters(u32 seed) {
    struct RngState rng;
    SeedRng(&rng, seed);
    for each encounter table:
        for each slot in table:
            slot.species = GetRandomSpecies(&rng);
            // keep slot.minLevel and slot.maxLevel unchanged
}
```

### Boss Double Battles

Gym leaders and key story battles become co-op double battles:

1. Modify gym leader scripts: instead of immediately starting battle, show a "Waiting for partner..." message
2. Send `boss_ready` with the gym leader's ID through the link
3. Wait for `boss_start` from the server (meaning both players are ready)
4. On `boss_start`, both ROMs initiate a double battle:
   - Battle type: `BATTLE_TYPE_DOUBLE | BATTLE_TYPE_MULTI`
   - Player 1 uses their own party (first 3 Pokémon)
   - Player 2's party data is synced via `party_sync` message before battle starts
   - The partner's Pokémon are loaded into the partner trainer slots
5. During battle, turn selections are synced via `battle_turn` messages each round
6. Both ROMs step through the battle in lockstep

**Phase 4 simplification (ship without synced battles first):**
Boss battles can initially work as "both players must defeat the gym leader independently to progress." Both see the same gym leader; both must beat them. The flag sync ensures the gate/story check only passes when the flag is set (which happens for both on first clear). This avoids the entire battle sync problem for v1.

### Link Cable Hook Point

The GBA serial interface is memory-mapped. The key registers:

- `REG_SIOCNT` — serial control register
- `REG_SIODATA8` / `REG_SIODATA32` — serial data registers
- The existing `src/link.c` manages the protocol

For this mod, we intercept at a higher level. Instead of modifying the hardware serial layer, we:

1. Create a new `src/multiplayer.c` module that manages all co-op state
2. In the main game loop (`src/overworld.c`), call `Multiplayer_Update()` each frame
3. `Multiplayer_Update()` reads from and writes to a ring buffer
4. The Tauri app reads/writes this buffer through libmgba's memory-mapped I/O or a custom SIO callback

### Git Workflow

- Work on the `main` branch directly. No pull requests.
- You may create a short-lived branch and manually merge it into `main` if needed.
- Commit and push to remote frequently — after each meaningful unit of work.

## DO NOTs

- Do NOT modify the base battle engine beyond what's needed for multi battle setup
- Do NOT change save file format — multiplayer state is session-only, not saved
- Do NOT sync every flag — only explicitly whitelisted ranges
- Do NOT attempt to sync menu state, bag, or PC boxes between players
- Do NOT assume both players are on the same map — the ghost NPC system must handle cross-map gracefully
- Do NOT block the game loop waiting for network — all network reads should be non-blocking with fallback to "no data this frame"

## Build & Test

### Building the ROM
Follow the standard pokeemerald-expansion INSTALL.md. Ensure FRLG mode is enabled in the build config.

### Testing Multiplayer Locally
Run two instances of mGBA pointed at the built ROM. Use mGBA's built-in link cable networking to connect them locally for early testing before the Tauri app is ready.

### Testing with Relay Server
```bash
cd relay-server/
npx partykit dev    # local dev server
```
Then point two Tauri app instances at `localhost:1999`.

## Phase Checklist

- [ ] Phase 0: Get pokeemerald-expansion building in FRLG mode, verify clean ROM output
- [ ] Phase 1: Ghost NPC — spawn P2 on same map, move from hardcoded test data
- [ ] Phase 2: Serial hook — get two mGBA instances exchanging position data through link cable
- [ ] Phase 3: Flag sync — trainer/story flags propagate between instances
- [ ] Phase 4: Randomizer — seeded encounter table shuffle with full national dex
- [ ] Phase 5: Boss readiness — gym leader scripts wait for both players, then start battle
- [ ] Phase 6: Tauri app — bundle ROM + libmgba + WebSocket net adapter, host/join UI
- [ ] Phase 7: PartyKit deployment — deploy relay server, hardcode URL in app
- [ ] Phase 8: Synced double battles (stretch goal) — real-time battle sync for boss fights
