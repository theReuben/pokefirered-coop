# Pokémon FireRed Co-Op Multiplayer Mod

## Engineering discipline — read before changing anything

@docs/ENGINEERING_DISCIPLINE.md

The imported rules above are binding, same as the DO NOTs below. They were
written from this repo's own commit history; each cites the incident it
prevents. The short version: diagnose mechanisms not symptoms, fix the layer
on the second occurrence of any patch, put state in the right persistence
tier, update every layer of the packet stack together, run `make
check-native` before every commit, and test sync features role-swapped and
under `set_link_chaos`.

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
- `starter_pick` — player chose a starter (species ID: Bulbasaur, Charmander, or Squirtle)

**Inbound (Server → ROM):**
- `role` — whether this client is host or guest
- `partner_position` — other player's position data
- `flag_set` / `var_set` — shared state update from partner
- `full_sync` — complete flag/var state dump (sent on connect)
- `boss_start` — both players ready, begin double battle
- `boss_waiting` — you're ready, partner isn't yet
- `battle_turn` — partner's turn selection
- `partner_connected` / `partner_disconnected`
- `starter_taken` — partner has claimed a starter (species ID); lock that ball in the lab
- `session_settings` — host's session settings (randomize_encounters bool, sent on connect)

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

### Co-op Starter Selection

Both players choose from Oak's three starters; the rival gets whichever one neither player picked.

**Flow:**

1. Both players enter Oak's lab and approach the Poké Balls as normal.
2. When a player selects a starter, the ROM immediately sends a `starter_pick` message with the species ID.
3. The relay server records the pick and broadcasts `starter_taken` to the partner.
4. The partner's ROM grays out / disables that ball so they cannot pick the same one.
5. Once both players have picked, the rival's species is determined: the one remaining starter not chosen by either player.
6. The rival's starter is stored in a save variable (`VAR_RIVAL_STARTER`) and used wherever the rival's first Pokémon is referenced.

**Script changes (`data/maps/PalletTown_OaksLab/scripts.inc`):**

- Intercept the moment the player confirms a ball selection (before the "So, you want X?" confirmation).
- Block the confirmation until `starter_taken` is received (or immediately if partner hasn't connected yet and we're in single-player fallback mode).
- After both players have confirmed, run a new `SetRivalStarter` script command that writes `VAR_RIVAL_STARTER` to the unchosen species.

**Rival starter logic:**

```c
// After both picks are known:
static const u16 sStarters[] = { SPECIES_BULBASAUR, SPECIES_CHARMANDER, SPECIES_SQUIRTLE };

u16 GetRivalStarter(u16 p1Species, u16 p2Species) {
    for (int i = 0; i < 3; i++) {
        if (sStarters[i] != p1Species && sStarters[i] != p2Species)
            return sStarters[i];
    }
    return SPECIES_BULBASAUR; // fallback, should never hit
}
```

Everywhere the rival's starting species is hardcoded (battle setup, overworld scripts), replace it with a `VarGet(VAR_RIVAL_STARTER)` lookup.

**Edge cases:**
- If one player is not yet connected when the other approaches the balls, both balls remain available and the unconnected player's pick defaults so the rival always gets one.
- If both players somehow pick simultaneously before `starter_taken` arrives, the relay server is authoritative: first `starter_pick` received wins; the server sends a `starter_denied` back to the slower player with the conflicting species.

---

### Randomization Settings

Encounter randomization is **on by default**. The setting is configurable in the Tauri app and persisted in the ROM's save data so it survives reloads.

**Tauri app UI:**

- On the host/join screen, the host sees a toggle: "Randomize wild Pokémon (default: on)".
- Guests see the setting as read-only — they inherit whatever the host chose.
- The host's setting is broadcast to the guest via `session_settings` on connect.

**Save data storage:**

Use one of the unused bytes in a custom extra save section rather than touching the existing save layout. Define a dedicated save section (e.g., `SAVE_SECTION_COOP_SETTINGS`, placed in the last available slot):

```c
// include/constants/multiplayer.h
#define SAVE_SECTION_COOP_SETTINGS  14  // use an unused section slot

struct CoopSettings {
    u8  randomizeEncounters : 1;  // 1 = on (default), 0 = off
    u8  padding : 7;
    u32 encounterSeed;            // shared seed set by host; 0 until session starts
};

extern struct CoopSettings gCoopSettings;
```

- On new game, `randomizeEncounters` defaults to `1`.
- On session connect, the host's value in `session_settings` overwrites the local field (guests always mirror host).
- The section is saved and loaded alongside the normal save via the standard `SaveGameData()` / `LoadGameData()` hooks — extend `src/save.c` minimally to include this section.
- The encounter seed is NOT saved (it's session-only; a new seed is generated each session). Only the on/off toggle persists.

**ROM randomization gate:**

```c
void MaybeRandomizeEncounters(u32 seed) {
    if (gCoopSettings.randomizeEncounters)
        RandomizeEncounters(seed);
    // else: use original encounter tables unchanged
}
```

**Note on the "Do NOT change save file format" rule:** That rule applies to the main trainer/Pokémon save data. Adding a single new save section for co-op settings is the minimal-impact approach and is explicitly allowed here. Do not alter existing section layouts.

---

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

Gym leaders and key story battles become **true co-op double battles** where both players control their own Pokémon simultaneously. This is a core feature, not a stretch goal.

**Battle flow:**

1. Both players approach the gym leader trigger → gym script sends `boss_ready` and waits for `boss_start`
2. After `boss_start`, a `waitcoopparty` script command opens the party selection menu on each player's screen — each player picks up to 3 Pokémon
3. Each ROM sends `MP_PKT_PARTY_SYNC` with the selected party; both wait until both parties are received
4. Both ROMs initiate a double battle simultaneously:
   - `BATTLE_TYPE_COOP | BATTLE_TYPE_MULTI | BATTLE_TYPE_INGAME_PARTNER | BATTLE_TYPE_TRAINER | BATTLE_TYPE_DOUBLE`
     (amended 2026-07-03 to match `BattleSetup_StartCoopBattle`: no
     `BATTLE_TYPE_LINK` — the link path would route battle-end through the
     real link-cable teardown handshake, which the virtual relay cannot
     complete; the non-link path also gives us the `Multiplayer_OnBattleEnd`
     hook)
   - Battler 0 = local player (controlled normally)
   - Battler 1 = partner (driven by network — replaces `battle_controller_player_partner.c` AI)
   - Battlers 2 & 3 = gym leader's two Pokémon
5. **Turn sync**: before each turn resolves, both instances exchange `MP_PKT_BATTLE_TURN` (move + target selection). Neither ROM advances the battle engine until both selections are received.
6. **RNG sync**: at battle start, one player (host) generates a 32-bit battle RNG seed and sends it as part of `MP_PKT_PARTY_SYNC`. Both ROMs seed their battle RNG identically so damage rolls and crit checks are in lockstep.

**Partner controller replacement (`src/battle_controller_player_partner.c`):**

- Replace the AI-driven controller with a network-driven one
- On `HandleInputChooseMove`: send local selection via `MP_PKT_BATTLE_TURN`, block until partner's packet arrives, then proceed
- The network poll loop must be non-blocking frame by frame (return early if no packet; the task re-runs next frame)

**Packet definitions:**

- `MP_PKT_PARTY_SYNC = 0x12` — 2-byte header + n×58 bytes (`struct MpWirePartyMon`: species, item, level, ability slot, personality, OT id, moves, PP, hp/maxHP, all five battle stats, status, friendship, nickname) + 4-byte trailing battle RNG seed (host sends a fresh nonzero seed each battle, guests send 0; receiver adopts any nonzero value). Full fidelity is mandatory: `CreateMon()` does not compute stats, so a reconstruction from display-only data leaves the partner's mon at 0 HP and the engine flags battler 2 absent (amended 2026-06-12 — this was the "co-op battle ignores partner input" bug).
- `MP_PKT_BATTLE_TURN = 0x14` — 5 bytes: seq (1), move index (1), target (1), flags (1); ally-side targets are mirrored (0↔2) on receipt because each instance runs its local player as battler 0. seq is 1-255 (never 0), +1 per logical turn, reset per battle in `Multiplayer_SetupCoopBattle`; the receiver applies only seqs strictly newer than the last applied (wraparound-aware), so duplicates and reordered stale turns are no-ops. The state beacon re-carries the cached turn every interval while in a coop battle, which is the loss-recovery path for a dropped turn packet (per the "reliability rides the beacon" rule).
- `MP_PKT_FOLLOWER_GFX = 0x13` — 3 bytes: type (1) + gfx id (2)
- `MP_PKT_ROLE_ASSIGN = 0x1B` — 2 bytes: relay assigns MP_ROLE_HOST/GUEST at session start; required for `GetMultiplayerId()`/`IsLinkMaster()` to differ between the two ROMs

**Script changes:**

- All 15 gym leader scripts: insert `waitcoopparty` between `waitbossstart`/`closemessage` and `trainerbattle_single`
- `trainerbattle_single` remains the battle entry point but battle type flags are overridden in `Multiplayer_SetupCoopBattle()`
- Set `VAR_FRONTIER_FACILITY = FACILITY_MULTI_OR_EREADER` before party menu opens; restore after

**`waitcoopparty` script command (`SCR_OP_WAITCOOPPARTY = 0xE9`):**

- State 0: `SavePlayerParty()` stash + set `coopPartyStashed` (added 2026-07-03 — the selection reorder and the partner mons loaded into `gPlayerParty[3..5]` are destructive; `Multiplayer_OnBattleEnd` writes each selected mon's post-battle state back to its original stash slot and `LoadPlayerParty()`s, so non-participants survive and partner mons are evicted), open party selection menu (`InitChooseHalfPartyForBattle`), set savedCallback → `CB2_CoopPartySelected`, advance to state 1
- State 1: `SetupNativeScript(ctx, Multiplayer_NativePollPartySync)` — blocks until `partnerPartySelectDone`
- `CB2_CoopPartySelected`: reorder `gPlayerParty[0..2]` from `gSelectedOrderFromParty[]`, call `Multiplayer_SendPartySync()`, set state→1, return to field

**RNG lockstep (implemented 2026-06-12):** Both ROMs must produce identical RNG outputs for battle-logic rolls. The original plan ("send `gBattleRngSeed` in `MP_PKT_PARTY_SYNC`") was insufficient as stated: the expansion's battle-logic rolls (`RandomUniform(RNG_*, …)`) draw from the shared `gRngValue` stream, which is also advanced by per-frame visual/animation draws — two free-running instances diverge immediately even with the same seed. The implemented mechanism: `src/multiplayer.c` defines strong overrides of the weak `RandomUniform`/`RandomUniformExcept`/`RandomWeightedArray`/`RandomElementArray` symbols from `src/random.c` (guarded `#if !TESTING` — `test/test_runner.c` installs its own strong versions in TESTING builds). During `BATTLE_TYPE_COOP` battles they draw from a dedicated xorshift32 stream (`gMultiplayerState.coopRngState`), otherwise they fall through to the `*Default` implementations. Seeding: the host mints a seed per battle in `CB2_CoopPartySelected` and ships it in the 4 trailing bytes of `MP_PKT_PARTY_SYNC`; `Multiplayer_SetupCoopBattle` copies the adopted seed into the stream state on both sides. If neither side has a role, both seeds are 0 and the stream's zero-remap constant keeps the sides in lockstep anyway. Known limitation: untagged `Random()`/`Random32()` calls inside battle logic (if any remain upstream) still draw from the shared stream and can diverge; speed-tie resolution interprets mirrored battler indices.

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
- Do NOT change save file format for existing sections — multiplayer session state (position, sync buffers) is not saved; the only exception is the `SAVE_SECTION_COOP_SETTINGS` extra section for persistent user preferences (randomizer toggle)
- Do NOT sync every flag — only explicitly whitelisted ranges
- Do NOT attempt to sync menu state, bag, or PC boxes between players
- Do NOT assume both players are on the same map — the ghost NPC system must handle cross-map gracefully
- Do NOT block the game loop waiting for network — all network reads should be non-blocking with fallback to "no data this frame"

## Build & Test

### Building the ROM
Follow the standard pokeemerald-expansion INSTALL.md. Ensure FRLG mode is enabled in the build config.

### Testing Multiplayer Locally (MCP tools — preferred)

The project uses a headless mGBA MCP server (`tools/mcp_gamestate/server.py`) that lets Claude Code control two emulator instances directly. The relay between them runs automatically inside the server process.

```bash
# In Claude Code — add the MCP server:
claude mcp add --scope project gamestate -- python3 tools/mcp_gamestate/server.py

# Build save states (one-time, or after ROM rebuild):
make build-states
```

MCP tools available: `start_emulator`, `stop_emulator`, `list_instances`, `screenshot`, `press_button`, `wait`, `load_savestate`, `save_savestate`, `advance_text`, `move_steps`, `get_text_state`, `read_memory`, `write_memory`, `battle_diag`, `check_battle_sync`, `start_recording`, `stop_recording`, `stop_recording_side_by_side`, `set_link_chaos`.

#### Link chaos mode — test like the real relay

The in-process MCP relay delivers every packet, in order, every cycle — the
real Tauri/PartyKit WebSocket path does not.  Several shipped bugs (ghost
gender, starter sync) only reproduced on the real relay because tests never
exercised packet loss or reordering.  Use `set_link_chaos(drop, delay, seed)`
to make the MCP relay drop each ROM packet with probability `drop` and hold
each packet one relay cycle (delivering it after newer packets — a reorder)
with probability `delay`.  Pass a nonzero `seed` for reproducible runs;
call with defaults to disable.  Env equivalents: `COOP_CHAOS_DROP`,
`COOP_CHAOS_DELAY`, `COOP_CHAOS_SEED`.

Recommended for any new sync feature: re-run the relevant co-op scenario with
`set_link_chaos(drop=0.3, seed=1)` — the session-state beacon should converge
gender/starter/boss-ready state despite the losses.  Relay-injected control
packets (`PARTNER_CONNECTED`/`DISCONNECTED`, `HOST_MIGRATE`) bypass chaos.

#### Save state catalogue — always load DIFFERENT states for p1 and p2

Loading the same `.ss1` file for both instances causes conflicts: both instances share the same trainer ID and save data, so save-data-dependent logic (starter selection, flag state) immediately diverges in unexpected ways. Use role-specific states from this table:

| State file | Who | Game state |
|---|---|---|
| `oaks_lab.ss1` | P1 | At Bulbasaur ball; VAR_LAB=2; no starter picked yet |
| `p2_oaks_lab.ss1` | P2 | At Charmander ball; VAR_LAB=2; no starter picked yet |
| `p1_rivals_lab.ss1` | P1 | Bulbasaur in party; VAR_LAB=3; rival battle coord trigger live |
| `p2_rivals_lab.ss1` | P2 | Charmander in party; VAR_LAB=3; rival battle coord trigger live |
| `p1_route1.ss1` | P1 | Bulbasaur; Route 1 entry |
| `p2_route1.ss1` | P2 | Charmander; Route 1 entry |
| `p1_forest_trainer.ss1` | P1 | Viridian Forest `(6,23)`, 1 tile S of UNDEFEATED bug catcher Sammy; press UP once → deterministic trainer battle |
| `p2_forest_trainer.ss1` | P2 | Same, Charmander/distinct ID |
| `tall_grass_route1.ss1` | either | Route 1 tall grass (single-player use only) |
| `pewter_gym.ss1` | either | Pewter Gym, Brock visible (single-player use only) |

For the co-op rival battle specifically: start p1 with `p1_rivals_lab.ss1` and p2 with `p2_rivals_lab.ss1`. Both states have `connState=0` so the relay will inject `PARTNER_CONNECTED` automatically.

For the field-trainer fixture (ghost catch-up snap, busy-trainer lock #18a, trainer-approach mirror #18b): start p1 with `p1_forest_trainer.ss1` and p2 with `p2_forest_trainer.ss1` (both `connState=0`). Press UP once on either instance to step into Sammy's cone and start a regular (non-coop) trainer battle while the partner roams the shared map.

#### Sync check protocol — fail fast on desync

Run `check_battle_sync()` immediately after a co-op battle starts (as soon as the battle intro animation is underway). **If it returns `FAIL`, stop the test run immediately** — do not advance the game further or take more screenshots. Report the full `check_battle_sync()` output plus both instances' `battle_diag()` outputs.

Desync red flags — stop testing if any of these are observed:
- `gBattleTypeFlags` or `gBattlersCount` differ between p1 and p2
- One instance is in battle while the other is on the overworld
- Both in battle but `check_battle_sync()` says FAIL (different opponents, different controllers)
- `gBattleCommunication` diverges and stays diverged for more than ~120 frames

#### Recording test runs

Start recording on both instances before navigating, then `stop_recording_side_by_side` at the end. The output MP4 shows P1 (left) and P2 (right) side-by-side, making visual desyncs obvious at a glance. Requires `ffmpeg` on PATH.

```python
# Typical dual-instance test session with recording:
start_recording("p1")
start_recording("p2")
# ... load states, drive inputs, run check_battle_sync() after battle starts ...
stop_recording_side_by_side("test/recordings/rival_battle_run1.mp4")
```

Use `stop_recording(instance_id)` instead if you only need a single-instance video.

### Testing with Relay Server (local PartyKit dev)
```bash
cd relay-server/
npx partykit dev    # starts local relay at ws://localhost:1999
```
Then point two Tauri app instances at `localhost:1999` via the `COOP_RELAY_URL` env var.

## Phase Checklist

- [x] Phase 0: Get pokeemerald-expansion building in FRLG mode, verify clean ROM output
- [x] Phase 1: Ghost NPC — spawn P2 on same map, move from position packets; follower Pokémon ghost included
- [x] Phase 1.5: Co-op starter selection — Oak's lab script, `starter_pick`/`starter_taken` messages, rival gets unchosen third
- [x] Phase 2: Serial hook — ring buffer IPC; relay in MCP server shuttles packets between mGBA instances
- [x] Phase 3: Flag sync — trainer/story flags propagate between instances via `FlagSet`/`VarSet` hooks
- [x] Phase 4: Randomizer — seeded encounter table shuffle with full national dex; `CoopSettings` save section; on by default
- [x] Phase 5: Boss readiness — gym leader scripts wait for both players, then start co-op double battle
- [x] Phase 5.5: Party selection — `waitcoopparty` command, party sync packet, partner party loaded for double battle
- [x] Phase 8: True co-op double battles — partner controller driven by network, turn sync via `MP_PKT_BATTLE_TURN`, RNG seed lockstep; battle reconnect/grace timer
- [ ] Phase 6: Tauri app — ROM + libmgba embedded, ring-buffer serial bridge (`serial_bridge.rs`), WebSocket net adapter, host/join UI built; **needs end-to-end integration test**
- [ ] Phase 7: PartyKit deployment — relay server built (302 lines), URL hardcoded in app (`wss://pokefirered-coop.thereuben.partykit.dev`); **needs live deployment verification**

### Post-launch / resilience (added beyond original spec)
- [x] Heartbeat ping every 2s; relay detects silence and injects `PARTNER_DISCONNECTED`
- [x] Process-death detection — side-channel inject file so disconnect lands even during long wait commands
- [x] Ghost NPC despawns within ~0.5s of partner disconnect
- [x] Host migration on disconnect mid-session
- [x] Battle grace timer — AI fallback after 30s of partner silence mid-battle
- [x] Auto-checkpoint save on every map transition while connected
- [x] Event log replay on reconnect so partner catches up on missed flag/var events
- [x] CI pipeline — ROM build, native unit tests, save-state smoke test, release packaging
