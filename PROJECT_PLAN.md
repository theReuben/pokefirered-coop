# Project Plan: Pokémon FireRed Co-Op Multiplayer

## Three Repos

This project spans three repositories:

1. **ROM Mod** — fork of `rh-hideout/pokeemerald-expansion` (FRLG mode)
2. **Relay Server** — PartyKit project (~150 lines TypeScript)
3. **App Shell** — Tauri desktop app (Rust + web frontend)


---

## Phase 0: Foundation
**Goal:** Verify the base builds and runs correctly.

### Tasks
- Fork `rh-hideout/pokeemerald-expansion`
- Follow INSTALL.md to set up the build toolchain (devkitARM, agbcc)
- Enable FRLG build mode (check expansion docs for the config flag)
- Build the ROM successfully, confirm SHA1 match or clean boot in mGBA
- Place CLAUDE.md in repo root

### Done When
- `make` produces a bootable FireRed ROM in mGBA
- The game plays through the intro and you can walk around Pallet Town


---

## Phase 1: Ghost NPC
**Goal:** A second player sprite appears on the map and can be moved programmatically.

### Tasks
- Define a new object event graphics ID for Player 2 (e.g. the rival sprite or opposite gender player)
- Write `src/multiplayer.c` and `include/multiplayer.h`:
  - `Multiplayer_Init()` — called on game boot
  - `Multiplayer_SpawnGhostNPC(mapId, x, y)` — adds P2 object event to current map
  - `Multiplayer_UpdateGhostPosition(x, y, facing)` — moves P2 each frame
  - `Multiplayer_DespawnGhost()` — removes P2 from map
- Hook `Multiplayer_Init()` into the game's init sequence
- For testing: hardcode ghost NPC at a fixed position on Route 1 and verify it renders
- Then: make the ghost NPC move on a loop (walk a square) to test animation

### Key Files
- `src/event_object_movement.c` — understand how object events move
- `src/field_player_avatar.c` — reference for player sprite setup
- `include/global.fieldmap.h` — ObjectEvent struct definition

### Done When
- A second player sprite is visible on Route 1
- It has collision (you can't walk through it)
- It plays walk animations when moving


---

## Phase 2: Serial Link Communication
**Goal:** Two mGBA instances exchange player position data through the link cable.

### Tasks
- Study `src/link.c` to understand the existing serial protocol
- Design a minimal binary packet format:
  ```
  [1 byte: packet type] [payload]
  
  Type 0x01 POSITION: [2 bytes mapId] [2 bytes x] [2 bytes y] [1 byte facing] [1 byte spriteState]
  Type 0x02 FLAG_SET: [2 bytes flagId]
  Type 0x03 VAR_SET:  [2 bytes varId] [2 bytes value]
  Type 0x04 BOSS_READY: [2 bytes bossId]
  Type 0x05 BOSS_CANCEL: (no payload)
  Type 0x06 SEED_SYNC: [4 bytes seed]
  Type 0x10 FULL_SYNC: [variable: flag bitfield + var pairs]
  ```
- Implement `Multiplayer_SendPacket(type, data, len)` and `Multiplayer_ReceivePacket(buffer)` in `src/multiplayer.c`
- Hook into the overworld main loop: call `Multiplayer_Update()` each frame which:
  1. Sends own position if it changed
  2. Reads any incoming packets
  3. Updates ghost NPC position from received position packets
- Test with mGBA's link cable networking (two instances connected locally)

### Key Decisions
- Use the link cable in MULTI mode (4-player) or NORMAL mode (2-player)? NORMAL is simpler.
- Packet send rate: every 4 frames (15 packets/sec at 60fps) is plenty for tile-based movement

### Done When
- Two mGBA windows connected via link cable
- Player 1's movement appears as the ghost NPC in Player 2's game, and vice versa
- Movement is smooth with no desync or corruption


---

## Phase 3: Shared Flag/Variable Sync
**Goal:** Beating a trainer in one game marks them as beaten in the other.

### Tasks
- Hook `FlagSet()` in `src/event_data.c`:
  ```c
  void FlagSet(u16 id) {
      // existing flag set logic...
      if (IsSyncableFlag(id))
          Multiplayer_SendFlagSet(id);
  }
  ```
- Define syncable flag ranges in `include/constants/multiplayer.h`
- Handle incoming `FLAG_SET` packets: call `FlagSet()` but with a guard to prevent re-broadcasting (infinite loop)
  ```c
  static bool8 sIsRemoteUpdate = FALSE;
  
  void Multiplayer_HandleFlagSet(u16 flagId) {
      sIsRemoteUpdate = TRUE;
      FlagSet(flagId);
      sIsRemoteUpdate = FALSE;
  }
  ```
- Same pattern for `VarSet()`
- On connect, the host sends a `FULL_SYNC` packet containing all set syncable flags
- The guest applies them on receipt

### Edge Cases
- Both players beat the same trainer simultaneously → both send FLAG_SET, both receive it, but FlagSet is idempotent so no problem
- Player triggers script while receiving a flag update → flag updates should apply immediately regardless of script state (they're just data writes)

### Done When
- P1 beats Bug Catcher on Route 1 → P2 sees the trainer disappear (or become non-battleable)
- P2 picks up an item → P1 sees the item ball gone
- Late-joining player receives all existing flag state on connect


---

## Phase 4: Randomized Encounters
**Goal:** Wild encounters draw from all generations using a shared seed.

### Tasks
- On session start (host creates room), generate a 32-bit random seed
- Send the seed as a `SEED_SYNC` packet to the guest on connect
- Write `RandomizeEncounterTables(u32 seed)`:
  - Iterate over all wild encounter table entries
  - For each slot, replace the species with a random species from the full national dex
  - Keep the original min/max levels
  - Use a seeded PRNG (e.g. a simple xorshift) for deterministic output
  - Optional: weight by BST so routes with low-level encounters get basic-stage Pokémon, and late-game routes get evolved/legendaries
- Call `RandomizeEncounterTables()` after the seed is received, before gameplay begins
- Both players now encounter the same randomized species on each route

### Constraints
- The expansion already has all species data (stats, sprites, cries, movesets) — no need to add Pokémon manually
- Some species may look odd at very low levels (e.g. Mewtwo at level 3) — consider filtering by evolution stage based on the route's level range
- Legendary/mythical Pokémon could be excluded from random encounters and reserved for special events

### Done When
- Both players see the same randomized encounters on Route 1
- Encounters include Pokémon from multiple generations
- Level scaling matches the original route difficulty


---

## Phase 5: Boss Battle Readiness
**Goal:** Gym leaders require both players to be present and ready before the battle begins.

### Tasks
- Modify each gym leader's script (in `data/maps/*/scripts.inc`):
  - After the pre-battle dialogue, instead of `trainerbattle`, show "Waiting for partner..."
  - Send `BOSS_READY` packet with the gym leader's trainer ID
  - Enter a wait loop checking for `BOSS_START` response
  - On `BOSS_START`, begin the battle
  - If the player walks away, send `BOSS_CANCEL`
- For v1 (no battle sync): both players fight the gym leader independently as a normal battle. The flag sync ensures both must win for progress.
- For v2 (synced double battle): use `BATTLE_TYPE_DOUBLE | BATTLE_TYPE_MULTI`, sync partner's party, sync turn selections each round

### Gym Leader Scripts to Modify
- Brock (Pewter Gym)
- Misty (Cerulean Gym)
- Lt. Surge (Vermilion Gym)
- Erika (Celadon Gym)
- Koga (Fuchsia Gym)
- Sabrina (Saffron Gym)
- Blaine (Cinnabar Gym)
- Giovanni (Viridian Gym)
- Elite Four (Victory Road check + each E4 member)
- Champion (rival final battle)

### Done When
- Walking up to Brock shows "Waiting for partner..."
- When both players interact with Brock, both see "Battle starting!"
- (v1) Each player fights Brock normally; beating him sets the flag for both
- (v2, stretch) Both players participate in a synced double battle against Brock


---

## Phase 6: Tauri App Shell
**Goal:** A single downloadable app that non-technical players can use.

### Architecture
```
tauri-app/
├── src-tauri/
│   ├── src/
│   │   ├── main.rs          — app entry point
│   │   ├── emulator.rs      — libmgba embedding and lifecycle
│   │   ├── serial_bridge.rs — intercept SIO, translate to/from WebSocket
│   │   └── net.rs           — WebSocket client (connects to PartyKit)
│   ├── Cargo.toml
│   └── tauri.conf.json
├── src/
│   ├── App.tsx              — main UI
│   ├── HostJoin.tsx         — host/join screen with room code
│   ├── GameView.tsx         — emulator display canvas
│   └── styles.css
├── rom/
│   └── pokefirered_coop.gba — bundled ROM (built from the mod repo)
└── package.json
```

### Key Components
- **Emulator embedding:** Use libmgba's C API via Rust FFI. Render frames to a canvas via the frontend.
- **Serial bridge:** Register a custom SIO handler with libmgba that routes serial data to/from the WebSocket connection instead of a real link cable.
- **Room code flow:**
  1. User clicks "Host" → app generates a 6-char alphanumeric code
  2. App connects to `wss://your-project.partykit.dev/party/{code}`
  3. App displays the code for the user to share
  4. Partner clicks "Join" → enters the code → connects to the same room
  5. PartyKit relay server pairs them automatically
  6. Both apps start the emulator and begin gameplay

### Input Handling
- Map keyboard/controller input to GBA button presses via libmgba's input API
- Support both keyboard (WASD/arrows + ZXC) and USB gamepad

### Done When
- Double-click the app, see a title screen with "Host Game" / "Join Game"
- Host generates a room code, guest enters it
- Both players see the game running with the other player's ghost NPC moving in real-time
- No emulator setup, no ROM files, no configuration required from the players


---

## Phase 7: PartyKit Deployment
**Goal:** The relay server is live and the app connects to it.

### Tasks
- Initialize PartyKit project: `npx partykit init relay-server`
- Drop in the server.ts from the project (already drafted)
- Deploy: `npx partykit deploy`
- Note the URL (e.g. `your-project.partykit.dev`)
- Hardcode this URL in the Tauri app's `net.rs`
- Test end-to-end: two app instances connecting through the live relay

### Monitoring
- PartyKit dashboard shows active rooms and connections
- Add basic logging in the server for debugging (room created, player joined, player disconnected)

### Done When
- Two Tauri app instances on different machines connect via room code
- Position and flag data syncs through the live PartyKit server


---

## Phase 8: Synced Double Battles (Stretch Goal)
**Goal:** Boss fights are true co-op double battles with both players' Pokémon.

### Tasks
- Before battle, both ROMs exchange full party data via `party_sync`:
  - Species, level, stats, moves, current HP, status, held item for each party member
  - This is a large packet (~200 bytes per Pokémon × 6 = ~1.2KB)
- Load partner's party into the multi-battle partner slots
- Use existing multi-battle / tag battle infrastructure in the expansion
- Each turn:
  1. Both players select their moves locally
  2. Each sends a `battle_turn` packet with their selection
  3. Both ROMs wait until they have both selections
  4. Both ROMs execute the turn with identical selections → identical results (deterministic)
- Battle end: flag is set, syncs normally

### Risks
- Battle RNG must be deterministic and seeded identically — any desync means the battles diverge
- Edge cases: player fainting, switching, using items, running
- This is the hardest feature in the entire project — save it for after everything else works

### Done When
- Both players enter a gym battle together
- Each controls their own Pokémon on one side of a 2v2 battle
- Turn selections sync correctly, battle plays out identically on both screens


---

## Risk Register

| Risk | Impact | Mitigation |
|------|--------|------------|
| pokeemerald-expansion FRLG mode is unstable | Blocks everything | Test thoroughly in Phase 0. Fall back to Emerald (Hoenn) if FRLG mode has showstopper bugs |
| Link cable protocol too slow for position sync | Choppy ghost NPC movement | Reduce send rate, interpolate position client-side |
| Flag sync causes script corruption | Softlocks, broken progression | Whitelist syncable flags narrowly, test every gym/story beat with both players |
| Tauri + libmgba integration is complex | Delays Phase 6 significantly | Start with mGBA's standalone networking for all ROM testing; Tauri is packaging only |
| Battle sync diverges | Desynced battles, wrong outcomes | Defer to Phase 8, use independent battles (Phase 5 v1) as the default |
| PartyKit free tier limits | Can't host enough sessions | Traffic will be minimal (2 players, tiny packets). If limits hit, move to Cloudflare Durable Objects directly |


---

## Tech Stack Summary

| Component | Technology |
|-----------|-----------|
| ROM base | rh-hideout/pokeemerald-expansion (FRLG mode) |
| ROM language | C (compiled with agbcc / devkitARM) |
| Emulator | libmgba (embedded via C API) |
| App shell | Tauri (Rust backend + web frontend) |
| Frontend | React + TypeScript |
| Relay server | PartyKit (TypeScript, deployed to Cloudflare edge) |
| Dev testing | mGBA desktop app with link cable networking |
