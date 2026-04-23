# PROGRESS TRACKER
# Project: Pokémon FireRed Co-Op Multiplayer
# ================
# Claude Code: UPDATE THIS FILE after completing each step.
# At the start of every session, READ THIS FILE FIRST.
#
# Status values: not_started | in_progress | blocked | done

## Current State
- **Active Phase:** 0 — Foundation
- **Active Step:** 0.1 — Enable FRLG Build Mode
- **Last Session Summary:** Not started. Repo is a fork of rh-hideout/pokeemerald-expansion (confirmed by recent commits re: Sheer Force, Toxic Spikes, Embody Aspect fixes). Build environment not yet verified.
- **Next Action:** Confirm FRLG build mode config flag, run `make`, verify ROM builds.

---

## Phase 0: Foundation

### Step 0.1: Enable FRLG Build Mode
- **Status:** not_started
- **Substeps:**
  - [ ] Find the FRLG config flag (check Makefile, include/config.h, expansion docs)
  - [ ] Enable FRLG mode in the build configuration
  - [ ] Run make and fix any build errors
  - [ ] Verify ROM builds successfully (check output file exists and is >1MB)
- **Notes:**

### Step 0.2: Verify Clean Boot
- **Status:** not_started
- **Substeps:**
  - [ ] Document the build output filename and SHA1
  - [ ] Note any warnings from the build process
  - [ ] Update README with build instructions specific to this project
- **Notes:**

### Step 0.3: Set Up Project Structure
- **Status:** not_started
- **Substeps:**
  - [ ] Create src/multiplayer.c and include/multiplayer.h with empty stubs
  - [ ] Create include/constants/multiplayer.h with placeholder defines
  - [ ] Add multiplayer.c to the Makefile/build system
  - [ ] Verify project still builds cleanly with the new empty files
  - [ ] Update .claudeignore to exclude build artifacts, .o files, and ROM binaries (already partially done)
- **Notes:**

### Step 0.4: Set Up Test Infrastructure
- **Status:** not_started
- **Substeps:**
  - [ ] Create test/ directory with Makefile for native C unit tests
  - [ ] Create test/mocks/ with stub headers for GBA hardware registers
  - [ ] Create test/test_runner.c with a minimal test framework (ASSERT macros)
  - [ ] Create a trivial test_smoke.c that compiles and passes
  - [ ] Verify tests build and run with gcc on the host machine
- **Notes:**

---

## Phase 1: Ghost NPC

### Step 1.1: Study Object Event System
- **Status:** not_started
- **Substeps:**
  - [ ] Read src/event_object_movement.c and document how ObjectEvents are created and moved
  - [ ] Read include/global.fieldmap.h and document the ObjectEvent struct layout
  - [ ] Read src/field_player_avatar.c and document how the player sprite is managed
  - [ ] Write findings to docs/object-events.md for reference
- **Notes:**

### Step 1.2: Define Player 2 Graphics
- **Status:** not_started
- **Substeps:**
  - [ ] Choose an existing sprite (e.g. opposite gender player) for P2
  - [ ] Define OBJ_EVENT_GFX_PLAYER2 constant
  - [ ] Verify the sprite ID is valid and renders correctly
- **Notes:**

### Step 1.3: Implement Ghost NPC Spawn/Despawn
- **Status:** not_started
- **Substeps:**
  - [ ] Implement Multiplayer_SpawnGhostNPC(mapId, x, y, facing) in src/multiplayer.c
  - [ ] Implement Multiplayer_DespawnGhost() in src/multiplayer.c
  - [ ] Ghost should use the P2 sprite, be collidable, and NOT be interactable
  - [ ] Test by hardcoding a ghost spawn on Route 1 at a fixed position
  - [ ] Verify ghost renders, has collision, and doesn't trigger dialogue
- **Notes:**

### Step 1.4: Implement Ghost NPC Movement
- **Status:** not_started
- **Substeps:**
  - [ ] Implement Multiplayer_UpdateGhostPosition(x, y, facing, spriteState)
  - [ ] Ghost should smoothly interpolate between tile positions
  - [ ] Ghost should play walk animations matching the facing direction
  - [ ] Test by making the ghost walk a square loop via hardcoded movement data
  - [ ] Verify animation plays correctly in all 4 directions
- **Notes:**

### Step 1.5: Handle Cross-Map Ghost
- **Status:** not_started
- **Substeps:**
  - [ ] When ghost's map ID differs from player's map, despawn the ghost
  - [ ] When ghost's map ID matches player's map again, respawn at correct position
  - [ ] Handle map transitions gracefully (no crashes on warp)
- **Notes:**

### Step 1.6: Write Ghost NPC Tests
- **Status:** not_started
- **Substeps:**
  - [ ] Write C unit tests for spawn/despawn state management
  - [ ] Write C unit tests for position update logic
  - [ ] Write C unit tests for cross-map spawn/despawn transitions
  - [ ] All tests pass
- **Notes:**

---

## Phase 2: Serial Link Communication

### Step 2.1: Study Link Cable System
- **Status:** not_started
- **Substeps:**
  - [ ] Read src/link.c and document the existing serial protocol
  - [ ] Read include/link.h and document data structures
  - [ ] Identify the hook points for custom serial communication
  - [ ] Write findings to docs/link-system.md
- **Notes:**

### Step 2.2: Design Packet Format
- **Status:** not_started
- **Substeps:**
  - [ ] Define binary packet types in include/constants/multiplayer.h
  - [ ] Design packet layout for: POSITION, FLAG_SET, VAR_SET, BOSS_READY, BOSS_CANCEL, SEED_SYNC, FULL_SYNC
  - [ ] Document packet format in docs/packet-protocol.md
  - [ ] Keep packets small — each must fit in the serial buffer
- **Notes:**

### Step 2.3: Implement Packet Encoding/Decoding
- **Status:** not_started
- **Substeps:**
  - [ ] Implement Multiplayer_EncodePositionPacket()
  - [ ] Implement Multiplayer_DecodePositionPacket()
  - [ ] Implement encode/decode for FLAG_SET, VAR_SET, SEED_SYNC packets
  - [ ] Implement encode/decode for FULL_SYNC packet (variable length)
  - [ ] Handle malformed/truncated packet errors gracefully
- **Notes:**

### Step 2.4: Write Packet Tests
- **Status:** not_started
- **Substeps:**
  - [ ] Write round-trip tests for every packet type
  - [ ] Write tests for malformed packet rejection
  - [ ] Write tests for truncated packet rejection
  - [ ] Write tests for boundary values (max map ID, max coords)
  - [ ] All tests pass
- **Notes:**

### Step 2.5: Implement Serial Send/Receive
- **Status:** not_started
- **Substeps:**
  - [ ] Implement Multiplayer_SendPacket(type, data, len) using the link cable interface
  - [ ] Implement Multiplayer_ReceivePacket(buffer) as non-blocking read
  - [ ] Create a ring buffer for outgoing packets
  - [ ] Create a ring buffer for incoming packets
  - [ ] Hook into the SIO interrupt handler or polling loop
- **Notes:**

### Step 2.6: Implement Multiplayer_Update Loop
- **Status:** not_started
- **Substeps:**
  - [ ] Create Multiplayer_Update() called from the overworld main loop
  - [ ] Send own position every 4 frames if position changed
  - [ ] Process all incoming packets each frame
  - [ ] Route incoming POSITION packets to ghost NPC update
  - [ ] Hook Multiplayer_Update() into src/overworld.c main loop
- **Notes:**

### Step 2.7: Generate Memory Map for Lua Tests
- **Status:** not_started
- **Substeps:**
  - [ ] After ROM builds, run extract_symbols.py against the .map file to produce test/lua/memory_map.lua
  - [ ] Verify key symbols are present: gMultiplayerState, gPlayerPosition, gGhostNpcState
  - [ ] Add this step to the build process documentation
- **Notes:**

### Step 2.8: Test Two-Instance Link
- **Status:** not_started
- **Substeps:**
  - [ ] Write docs/testing-link.md with instructions for testing in mGBA
  - [ ] Verify two mGBA instances connected via link cable exchange position data
  - [ ] Verify ghost NPC moves on both screens
  - [ ] Document any issues or latency observations
- **Notes:**

---

## Phase 3: Shared Flag/Variable Sync

### Step 3.1: Define Syncable Flag Ranges
- **Status:** not_started
- **Substeps:**
  - [ ] Audit include/constants/flags.h to identify trainer, story, and item flag ranges
  - [ ] Define SYNC_FLAG_TRAINERS_START/END in include/constants/multiplayer.h
  - [ ] Define SYNC_FLAG_STORY_START/END
  - [ ] Define SYNC_FLAG_ITEMS_START/END
  - [ ] Implement IsSyncableFlag(flagId) inline function
  - [ ] Document which flag ranges sync and which don't in docs/flag-sync.md
- **Notes:**

### Step 3.2: Hook FlagSet and VarSet
- **Status:** not_started
- **Substeps:**
  - [ ] Add multiplayer broadcast hook to FlagSet() in src/event_data.c
  - [ ] Add sIsRemoteUpdate guard to prevent re-broadcast loops
  - [ ] Add multiplayer broadcast hook to VarSet() with same guard
  - [ ] Implement Multiplayer_HandleRemoteFlagSet() and Multiplayer_HandleRemoteVarSet()
  - [ ] Route incoming FLAG_SET and VAR_SET packets to these handlers
- **Notes:**

### Step 3.3: Implement Full Sync on Connect
- **Status:** not_started
- **Substeps:**
  - [ ] On connection established, host builds FULL_SYNC packet with all set syncable flags
  - [ ] Guest receives FULL_SYNC and applies all flags/vars
  - [ ] Handle the case where guest connects mid-game with existing progress (union-wins: apply any flag set by either player)
- **Notes:**

### Step 3.4: Implement Script Mutex
- **Status:** not_started
- **Substeps:**
  - [ ] Add gIsInScript flag to multiplayer state
  - [ ] When a player enters a script, set the flag and notify partner
  - [ ] Partner's ghost NPC should not be able to trigger scripts while flag is set
  - [ ] Clear the flag when script completes
- **Notes:**

### Step 3.5: Write Flag Sync Tests
- **Status:** not_started
- **Substeps:**
  - [ ] Write C unit tests for IsSyncableFlag with trainer, story, UI flags
  - [ ] Write C unit tests for no-rebroadcast guard
  - [ ] Write C unit tests for full sync application
  - [ ] Write Lua integration test script for two-instance flag sync
  - [ ] All tests pass
- **Notes:**

---

## Phase 4: Randomized Encounters

### Step 4.1: Study Encounter System
- **Status:** not_started
- **Substeps:**
  - [ ] Read src/wild_encounter.c and document how encounters are generated
  - [ ] Read the encounter table data structures
  - [ ] Identify where species, level min, and level max are stored
  - [ ] Count total encounter slots across all routes
  - [ ] Document in docs/encounter-system.md
- **Notes:**

### Step 4.2: Implement Seeded PRNG
- **Status:** not_started
- **Substeps:**
  - [ ] Implement a simple xorshift32 PRNG in src/multiplayer.c
  - [ ] Implement Multiplayer_SeedRng(seed) and Multiplayer_NextRandom()
  - [ ] Write unit test confirming determinism (same seed = same sequence)
- **Notes:**

### Step 4.3: Implement Encounter Randomizer
- **Status:** not_started
- **Substeps:**
  - [ ] Implement RandomizeEncounterTables(u32 seed)
  - [ ] For each encounter slot, replace species with a random valid species
  - [ ] Preserve original min/max levels in every slot
  - [ ] Filter out SPECIES_NONE, SPECIES_EGG, and any invalid IDs
- **Notes:**

### Step 4.4: Implement Seed Sync
- **Status:** not_started
- **Substeps:**
  - [ ] Host generates a random seed on session start
  - [ ] Host sends SEED_SYNC packet to guest on connect
  - [ ] Guest receives seed and calls RandomizeEncounterTables()
  - [ ] Both ROMs now have identical encounter tables
  - [ ] Hook randomization into game init, AFTER seed received
- **Notes:**

### Step 4.5: Write Randomizer Tests
- **Status:** not_started
- **Substeps:**
  - [ ] Write C unit test: same seed produces identical tables
  - [ ] Write C unit test: different seeds produce different tables
  - [ ] Write C unit test: levels preserved after randomization
  - [ ] Write C unit test: no invalid species generated
  - [ ] Write Lua test: both instances show same wild encounters on Route 1
  - [ ] All tests pass
- **Notes:**

---

## Phase 5: Boss Battle Readiness

### Step 5.1: Study Gym Leader Scripts
- **Status:** not_started
- **Substeps:**
  - [ ] Read the battle script format for Brock's gym
  - [ ] Document how trainerbattle command works
  - [ ] Identify all 8 gym leader script locations
  - [ ] Identify Elite Four and Champion script locations
  - [ ] Document in docs/boss-scripts.md
- **Notes:**

### Step 5.2: Implement Boss Readiness Protocol
- **Status:** not_started
- **Substeps:**
  - [ ] Add BOSS_READY and BOSS_CANCEL packet handling to Multiplayer_Update
  - [ ] Add boss readiness state to multiplayer state struct
  - [ ] Implement waiting UI: show 'Waiting for partner...' textbox
  - [ ] On BOSS_START received, dismiss waiting UI and begin battle
  - [ ] If player walks away from trigger, send BOSS_CANCEL
- **Notes:**

### Step 5.3: Modify Brock's Gym Script (Prototype)
- **Status:** not_started
- **Substeps:**
  - [ ] Modify Brock's pre-battle script to check multiplayer state
  - [ ] After dialogue, send BOSS_READY instead of immediately starting battle
  - [ ] Show waiting message until BOSS_START received
  - [ ] On BOSS_START, begin the battle normally
  - [ ] Test with two mGBA instances: both must interact to start
- **Notes:**

### Step 5.4: Modify Remaining Gym Leader Scripts
- **Status:** not_started
- **Substeps:**
  - [ ] Apply same pattern to Misty, Lt. Surge, Erika, Koga, Sabrina, Blaine, Giovanni
- **Notes:**

### Step 5.5: Modify Elite Four and Champion
- **Status:** not_started
- **Substeps:**
  - [ ] Apply boss readiness to each Elite Four member
  - [ ] Apply to Champion rival battle
  - [ ] Ensure Victory Road gate checks work with shared flags
- **Notes:**

### Step 5.6: Write Boss Readiness Tests
- **Status:** not_started
- **Substeps:**
  - [ ] Write C unit test for boss ready/cancel state machine
  - [ ] Write Lua test: both players interact → battle starts
  - [ ] Write Lua test: one player cancels → other gets waiting state
  - [ ] All tests pass
- **Notes:**

---

## Phase 6: Relay Server

### Step 6.1: Set Up PartyKit Project
- **Status:** not_started
- **Substeps:**
  - [ ] Create relay-server/ directory in repo root
  - [ ] Initialize PartyKit project with npx partykit init
  - [ ] Implement server.ts with full relay logic (role assignment, position relay, flag sync, boss readiness, disconnect handling)
  - [ ] Implement session_id validation in the handshake: on first connection store the session_id; reject subsequent connections where session_id doesn't match
  - [ ] Add package.json with partykit and vitest dependencies
- **Notes:**

### Step 6.2: Write Relay Server Tests
- **Status:** not_started
- **Substeps:**
  - [ ] Create server.test.ts with Vitest
  - [ ] Test role assignment (host/guest)
  - [ ] Test room capacity (reject 3rd player)
  - [ ] Test session_id validation (reject mismatched session_id)
  - [ ] Test position relay (forward, no echo)
  - [ ] Test flag sync (store, broadcast, deduplicate)
  - [ ] Test full sync on connect
  - [ ] Test boss readiness state machine
  - [ ] Test disconnect/reconnect handling
  - [ ] All tests pass
- **Notes:**

### Step 6.3: Local Integration Test
- **Status:** not_started
- **Substeps:**
  - [ ] Run partykit dev locally
  - [ ] Write a simple WebSocket test client that simulates two players
  - [ ] Verify messages relay correctly end-to-end
  - [ ] Document local testing process in docs/relay-testing.md
- **Notes:**

---

## Phase 7: Tauri App Shell

### Step 7.1: Scaffold Tauri Project
- **Status:** not_started
- **Substeps:**
  - [ ] Create tauri-app/ directory
  - [ ] Initialize Tauri project with React + TypeScript frontend
  - [ ] Set up project structure: src-tauri/ for Rust, src/ for frontend
  - [ ] Verify bare Tauri app builds and launches
- **Notes:**

### Step 7.2: Build Host/Join UI
- **Status:** not_started
- **Substeps:**
  - [ ] Create HostJoin.tsx with Host Game and Join Game buttons
  - [ ] Add save file picker: "New Game" or "Load Save" (opens file dialog for .sav file)
  - [ ] On New Game (host): generate session_id (UUID v4) and encounter_seed (u32), write .coop sidecar alongside chosen .sav path
  - [ ] On Load Save: read .coop sidecar and display session metadata (date created) so player can confirm the right save
  - [ ] Host flow: generate 6-char room code, display it alongside session_id
  - [ ] Join flow: text input for room code, connect button, load .sav + .coop sidecar
  - [ ] Add connection status indicator
  - [ ] Style with a Pokémon-appropriate theme
- **Notes:**

### Step 7.3: Implement WebSocket Client
- **Status:** not_started
- **Substeps:**
  - [ ] Implement net.rs in src-tauri/ — WebSocket client connecting to PartyKit
  - [ ] Room URL format: wss://project.partykit.dev/party/{code}
  - [ ] Include session_id in the connection handshake message
  - [ ] Handle session_mismatch response from server (show error: "This save was started with a different partner")
  - [ ] Handle connection, disconnection, reconnection with backoff
  - [ ] Expose connection state to frontend via Tauri commands
- **Notes:**

### Step 7.4: Embed libmgba
- **Status:** not_started
- **Substeps:**
  - [ ] Add libmgba as a dependency (C library via Rust FFI or existing Rust bindings)
  - [ ] Implement emulator.rs: ROM loading, save file loading/writing, frame stepping, input handling
  - [ ] Render frames to a canvas element in the frontend
  - [ ] Map keyboard input to GBA buttons
  - [ ] Map USB gamepad input to GBA buttons
- **Notes:**

### Step 7.5: Implement Serial Bridge
- **Status:** not_started
- **Substeps:**
  - [ ] Implement serial_bridge.rs — intercept libmgba SIO callbacks
  - [ ] Route outgoing serial data to the WebSocket client
  - [ ] Route incoming WebSocket data to the serial receive buffer
  - [ ] Translate between binary serial packets and JSON WebSocket messages
- **Notes:**

### Step 7.6: Bundle ROM and Handle Saves
- **Status:** not_started
- **Substeps:**
  - [ ] Copy built ROM into tauri-app/rom/ directory
  - [ ] Configure Tauri to include ROM as a bundled resource
  - [ ] Load ROM from bundled resources on app start
  - [ ] On session end, write updated .sav back to the user's chosen file path
  - [ ] Verify end-to-end: app launches, ROM boots, game is playable, save persists across restarts
- **Notes:**

### Step 7.7: End-to-End Test
- **Status:** not_started
- **Substeps:**
  - [ ] Build two copies of the Tauri app
  - [ ] Host a game on one (new game), join on the other (load matching save)
  - [ ] Verify session_id is validated correctly on connect
  - [ ] Verify ghost NPC appears and moves
  - [ ] Verify flag sync works through the relay
  - [ ] Verify .sav is written on exit and reloads correctly next session
  - [ ] Document setup and testing in docs/app-testing.md
- **Notes:**

---

## Phase 8: Deploy & Polish

### Step 8.1: Deploy Relay Server
- **Status:** not_started
- **Substeps:**
  - [ ] Run npx partykit deploy in relay-server/
  - [ ] Note the deployment URL
  - [ ] Hardcode URL in tauri-app/src-tauri/src/net.rs
  - [ ] Add fallback direct-connect option for advanced users
- **Notes:**

### Step 8.2: Set Up CI
- **Status:** not_started
- **Substeps:**
  - [ ] Create .github/workflows/test.yml
  - [ ] Job 1: C unit tests (gcc + make)
  - [ ] Job 2: Relay server tests (Node + Vitest)
  - [ ] Job 3: ROM build verification
  - [ ] Job 4: Run extract_symbols.py to generate test/lua/memory_map.lua
  - [ ] Job 5: mGBA integration tests (headless with xvfb)
  - [ ] Verify all jobs pass on push
- **Notes:**

### Step 8.3: Build Distributable
- **Status:** not_started
- **Substeps:**
  - [ ] Configure Tauri for macOS, Windows, and Linux builds
  - [ ] Set app name, icon, and metadata
  - [ ] Build release binaries for each platform
  - [ ] Test on at least one non-dev machine
- **Notes:**

### Step 8.4: Write Player Documentation
- **Status:** not_started
- **Substeps:**
  - [ ] Create PLAYING.md with instructions for non-technical users
  - [ ] Include: download, install, host a game, join a game, controls
  - [ ] Include: known limitations and troubleshooting
  - [ ] Include: how to continue a saved session (load your .sav + .coop files)
- **Notes:**

---

## Session Log

| Session # | Date | Phase.Step | What was done | What's next | Issues hit |
|---|---|---|---|---|---|
| 1 | | | | | |
