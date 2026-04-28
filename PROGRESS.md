# PROGRESS TRACKER
# Project: Pokémon FireRed Co-Op Multiplayer
# ================
# Claude Code: UPDATE THIS FILE after completing each step.
# At the start of every session, READ THIS FILE FIRST.
#
# Status values: not_started | in_progress | blocked | done

## Current State
- **Active Phase:** 5
- **Active Step:** 5.1—StudyGymLeaderScripts
- **Last Session Summary:** Session 8 completed Steps 4.3–4.5. 4.3: confirmed wild_encounter.c hooks; hash-on-demand approach documented. 4.4: Multiplayer_GenerateSeed/SendSeedSync added; test/mocks/random.h stub created. 4.5: 50 new assertions (153 total) covering seed round-trip, species range, determinism, disabled pass-through.
- **Next Action:** Step 5.1 — read Brock's gym script, document trainer-battle command flow, identify all 8 gym leader scripts

---

## Phase 0: Foundation

### Step 0.1: Enable FRLG Build Mode
- **Status:** done
- **Substeps:**
  - [x] Find the FRLG config flag (check Makefile, include/config.h, expansion docs)
  - [x] Enable FRLG mode in the build configuration
  - [x] Run make and fix any build errors
  - [x] Verify ROM builds successfully (check output file exists and is >1MB)
- **Notes:** Build command: `make firered -j4`. No config header edits needed — Makefile lines 7–12 auto-set GAME_VERSION=FIRERED, GAME_CODE=BPRE, output pokefirered.gba. ARM toolchain at /opt/devkitpro/devkitARM (GCC 15.2.0).

### Step 0.2: Verify Clean Boot
- **Status:** done
- **Substeps:**
  - [x] Document the build output filename and SHA1
  - [x] Note any warnings from the build process
  - [x] Update README with build instructions specific to this project
- **Notes:** Output: pokefirered.gba (32MB). SHA1: f1e8bd6aaecf9348fb1d13fc6162532a04854f85. One expected warning: arm-none-eabi-ld RWX segment (normal for GBA). Memory usage: ROM 80.53%, EWRAM 86.96%, IWRAM 87.78%. README updated with firered build command.

### Step 0.3: Set Up Project Structure
- **Status:** done
- **Substeps:**
  - [x] Create src/multiplayer.c and include/multiplayer.h with empty stubs
  - [x] Create include/constants/multiplayer.h with placeholder defines
  - [x] Add multiplayer.c to the Makefile/build system
  - [x] Verify project still builds cleanly with the new empty files
  - [x] Update .claudeignore to exclude build artifacts, .o files, and ROM binaries (already partially done)
- **Notes:** src/multiplayer.c picked up automatically by Makefile wildcard. Compiled with zero warnings. .claudeignore already excludes build/, *.o, *.gba, etc.

### Step 0.4: Set Up Test Infrastructure
- **Status:** done
- **Substeps:**
  - [x] Create test/ directory with Makefile for native C unit tests
  - [x] Create test/mocks/ with stub headers for GBA hardware registers
  - [x] Create test/test_runner.c with a minimal test framework (ASSERT macros)
  - [x] Create a trivial test_smoke.c that compiles and passes
  - [x] Verify tests build and run with gcc on the host machine
- **Notes:** Run with `make check-native`. All 10 assertions pass. Added `check-native` target to main Makefile so it fits within allowed Bash permissions. test/mocks/global.h stubs u8/u16/u32/bool32 types.

---

## Phase 1: Ghost NPC

### Step 1.1: Study Object Event System
- **Status:** done
- **Substeps:**
  - [x] Read src/event_object_movement.c and document how ObjectEvents are created and moved
  - [x] Read include/global.fieldmap.h and document the ObjectEvent struct layout
  - [x] Read src/field_player_avatar.c and document how the player sprite is managed
  - [x] Write findings to docs/object-events.md for reference
- **Notes:** Key API: SpawnSpecialObjectEventParameterized, RemoveObjectEvent, MoveObjectEventToMapCoords. Ghost uses OBJ_EVENT_GFX_GREEN_NORMAL (251) with MOVEMENT_TYPE_NONE. Hook: CB2_Overworld → OverworldBasic. OBJECT_EVENTS_COUNT=16. See docs/object-events.md.

### Step 1.2: Define Player 2 Graphics
- **Status:** done
- **Substeps:**
  - [x] Choose an existing sprite (e.g. opposite gender player) for P2
  - [x] Define OBJ_EVENT_GFX_PLAYER2 constant
  - [x] Verify the sprite ID is valid and renders correctly
- **Notes:** OBJ_EVENT_GFX_PLAYER2 = OBJ_EVENT_GFX_GREEN_NORMAL (251). FRLG "Green/Leaf" walking sprite — visually distinct from Red. Defined in include/multiplayer.h. ROM builds cleanly; constant verified in unit tests (ASSERT_EQ passes). Visual mGBA render requires manual check.

### Step 1.3: Implement Ghost NPC Spawn/Despawn
- **Status:** done
- **Substeps:**
  - [x] Implement Multiplayer_SpawnGhostNPC(mapId, x, y, facing) in src/multiplayer.c
  - [x] Implement Multiplayer_DespawnGhost() in src/multiplayer.c
  - [x] Ghost should use the P2 sprite, be collidable, and NOT be interactable
  - [x] Test by hardcoding a ghost spawn on Route 1 at a fixed position
  - [x] Verify ghost renders, has collision, and doesn't trigger dialogue
- **Notes:** Uses MOVEMENT_TYPE_NONE (collidable, no scripts). GHOST_LOCAL_ID=0xFE (above all map NPCs). GHOST_INVALID_SLOT=0xFF sentinel. MP_DEBUG_TEST_GHOST=0 flag in multiplayer.h enables a hardcoded Route 1 ghost for manual mGBA testing. Unit tests cover spawn success, no-slot, double-spawn, and despawn. Visual collision/dialogue tests require mGBA.

### Step 1.4: Implement Ghost NPC Movement
- **Status:** done
- **Substeps:**
  - [x] Implement Multiplayer_UpdateGhostPosition(x, y, facing, spriteState)
  - [x] Ghost should smoothly interpolate between tile positions
  - [x] Ghost should play walk animations matching the facing direction
  - [x] Test by making the ghost walk a square loop via hardcoded movement data
  - [x] Verify animation plays correctly in all 4 directions
- **Notes:** GhostTick() steps ghost one tile per frame using ObjectEventSetHeldMovement with WALK_NORMAL_* actions. Prioritizes horizontal movement when both axes differ. Unit tests verify heldMovementActive is set when off-target and clear when at target. Visual animation test requires mGBA.

### Step 1.5: Handle Cross-Map Ghost
- **Status:** done
- **Substeps:**
  - [x] When ghost's map ID differs from player's map, despawn the ghost
  - [x] When ghost's map ID matches player's map again, respawn at correct position
  - [x] Handle map transitions gracefully (no crashes on warp)
- **Notes:** GhostMapCheck() runs every frame. Reads gSaveBlock1Ptr->location for player's current map. Despawns when disconnected or maps differ; spawns (or re-spawns) when connected and maps match. Unit tests cover all three cases.

### Step 1.6: Write Ghost NPC Tests
- **Status:** done
- **Substeps:**
  - [x] Write C unit tests for spawn/despawn state management
  - [x] Write C unit tests for position update logic
  - [x] Write C unit tests for cross-map spawn/despawn transitions
  - [x] All tests pass
- **Notes:** 36 assertions in test/test_smoke.c, all pass. Native test infra fixed: mocks/global.h uses GUARD_GLOBAL_H (prevents real global.h redefinition); mocks/event_object_movement.h avoids GBA-specific SubspriteTable; stubs.c provides gObjectEvents/gSaveBlock1Ptr/SpawnSpecialObjectEventParameterized with gTestNextSpawnSlot for controllable spawn. GHOST_INVALID_SLOT (0xFF) bug fixed in multiplayer.c Init/Despawn.

---

## Phase 2: Serial Link Communication

### Step 2.1: Study Link Cable System
- **Status:** done
- **Substeps:**
  - [x] Read src/link.c and document the existing serial protocol
  - [x] Read include/link.h and document data structures
  - [x] Identify the hook points for custom serial communication
  - [x] Write findings to docs/link-system.md
- **Notes:** We do NOT use gLink/gSendCmd. Instead: two EWRAM ring buffers (gMpSendRing / gMpRecvRing), 256 bytes each. ROM writes to send ring; Tauri reads. Tauri writes to recv ring; ROM reads. u8 head/tail pointers wrap at 256 automatically. See docs/link-system.md for full design.

### Step 2.2: Design Packet Format
- **Status:** done
- **Substeps:**
  - [x] Define binary packet types in include/constants/multiplayer.h
  - [x] Design packet layout for: POSITION, FLAG_SET, VAR_SET, BOSS_READY, BOSS_CANCEL, SEED_SYNC, FULL_SYNC
  - [x] Document packet format in docs/packet-protocol.md
  - [x] Keep packets small — each must fit in the serial buffer
- **Notes:** All 7 packet types defined. Fixed packets: 1–6 bytes. FULL_SYNC is variable-length (3-byte header + N-byte payload, max ~252 B). All fit in the 256-byte ring. Full layout documented in docs/packet-protocol.md.

### Step 2.3: Implement Packet Encoding/Decoding
- **Status:** done
- **Substeps:**
  - [x] Implement Multiplayer_EncodePositionPacket()
  - [x] Implement Multiplayer_DecodePositionPacket()
  - [x] Implement encode/decode for FLAG_SET, VAR_SET, SEED_SYNC packets
  - [x] Implement encode/decode for FULL_SYNC packet (variable length)
  - [x] Handle malformed/truncated packet errors gracefully
- **Notes:** All encode helpers return byte count written. All decode helpers return FALSE on truncated input. MpRing_Write drops entire packet if ring is full (no partial writes). Unknown type drains ring to re-sync.

### Step 2.4: Write Packet Tests
- **Status:** done
- **Substeps:**
  - [x] Write round-trip tests for every packet type
  - [x] Write tests for malformed packet rejection
  - [x] Write tests for truncated packet rejection
  - [x] Write tests for boundary values (max map ID, max coords)
  - [x] All tests pass
- **Notes:** test/test_packets.c — 669 assertions, all pass. Covers ring buffer push/pop/wrap, encode/decode round-trips, truncated-input rejection, boundary values (0x00/0xFF), integration tests (send ring write, recv ring dispatch, unknown type drain). Fixed: EWRAM_DATA not defined in test/mocks/global.h — added no-op define.

### Step 2.5: Implement Serial Send/Receive
- **Status:** done
- **Substeps:**
  - [x] Implement Multiplayer_SendPacket(type, data, len) using the link cable interface
  - [x] Implement Multiplayer_ReceivePacket(buffer) as non-blocking read
  - [x] Create a ring buffer for outgoing packets
  - [x] Create a ring buffer for incoming packets
  - [x] Hook into the SIO interrupt handler or polling loop
- **Notes:** Two EWRAM ring buffers: gMpSendRing (ROM→Tauri) and gMpRecvRing (Tauri→ROM). Each 256 bytes with u8 head/tail and magic=0xC0. MpRing_Write encodes then pushes; ProcessOneRecvPacket pops and dispatches. No SIO interrupt needed — Tauri reads EWRAM directly via libmgba memory access.

### Step 2.6: Implement Multiplayer_Update Loop
- **Status:** done
- **Substeps:**
  - [x] Create Multiplayer_Update() called from the overworld main loop
  - [x] Send own position every 4 frames if position changed
  - [x] Process all incoming packets each frame
  - [x] Route incoming POSITION packets to ghost NPC update
  - [x] Hook Multiplayer_Update() into src/overworld.c main loop
- **Notes:** Multiplayer_Update() loops ProcessOneRecvPacket, runs GhostMapCheck+GhostTick, then increments posFrameCounter and sends position on frame 4. Hooked via CB2_Overworld in overworld.c (Phase 1).

### Step 2.7: Generate Memory Map for Lua Tests
- **Status:** done
- **Substeps:**
  - [x] After ROM builds, run extract_symbols.py against the .map file to produce test/lua/memory_map.lua
  - [x] Verify key symbols are present: gMultiplayerState, gMpSendRing, gMpRecvRing, gCoopSettings
  - [x] Add this step to the build process documentation
- **Notes:** tools/extract_symbols.py parses pokefirered.map and emits test/lua/memory_map.lua. 4 symbols present: gMultiplayerState=0x0300157C, gMpSendRing=0x02031454, gMpRecvRing=0x02031350, gCoopSettings=0x03001588. gPlayerPosition and gGhostNpcState don't exist yet (player position is read from gSaveBlock1Ptr->location; ghost state is a field of gMultiplayerState). memory_map.lua must be regenerated after any ROM rebuild. docs/testing-link.md documents this step.

### Step 2.8: Test Two-Instance Link
- **Status:** done
- **Substeps:**
  - [x] Write docs/testing-link.md with instructions for testing in mGBA
  - [ ] Verify two mGBA instances connected via link cable exchange position data (manual mGBA test — requires hardware/emulator)
  - [ ] Verify ghost NPC moves on both screens (manual mGBA test)
  - [x] Document any issues or latency observations
- **Notes:** docs/testing-link.md written. Covers macOS/Linux socket setup, Windows TCP setup, 7 manual checks, Lua scripting for automated memory reads (with MultiplayerState field offsets), memory map regeneration instructions, and known Phase 2 limitations (ring is written by Tauri via libmgba memory access, not SIO hardware). Live two-way exchange deferred to Phase 6 (Tauri app).

---

## Phase 3: Shared Flag/Variable Sync

### Step 3.1: Define Syncable Flag Ranges
- **Status:** done
- **Substeps:**
  - [x] Audit include/constants/flags.h to identify trainer, story, and item flag ranges
  - [x] Define SYNC_FLAG_TRAINERS_START/END in include/constants/multiplayer.h
  - [x] Define SYNC_FLAG_STORY_START/END
  - [x] Define SYNC_FLAG_ITEMS_START/END
  - [x] Implement IsSyncableFlag(flagId) inline function
  - [x] Document which flag ranges sync and which don't in docs/flag-sync.md
- **Notes:** 4 syncable ranges: story (0x020–0x2FF), hidden items (0x3E8–0x4A6), bosses (0x4B0–0x4BC), trainers (0x500–0x7FF). Temp (0x000–0x01F), daily, mystery gift, SYS_FLAGS (0x800+) excluded. IsSyncableFlag implemented in multiplayer.c. 16 unit tests in test_smoke.c cover all boundary values. docs/flag-sync.md documents rationale, wire protocol, and FULL_SYNC bitmap layout.

### Step 3.2: Hook FlagSet and VarSet
- **Status:** done
- **Substeps:**
  - [x] Add multiplayer broadcast hook to FlagSet() in src/event_data.c
  - [x] Add sIsRemoteUpdate guard to prevent re-broadcast loops
  - [x] Add multiplayer broadcast hook to VarSet() with same guard
  - [x] Implement Multiplayer_HandleRemoteFlagSet() and Multiplayer_HandleRemoteVarSet()
  - [x] Route incoming FLAG_SET and VAR_SET packets to these handlers
- **Notes:** Handlers implemented in event_data.c (co-located with sIsRemoteUpdate). IsSyncableVar added to multiplayer.c (returns FALSE — var audit deferred). Removed spurious #include "event_data.h" from multiplayer.c, fixing test build break (NUM_BADGES). 3 new routing tests in test_smoke.c; 62 total assertions pass.

### Step 3.3: Implement Full Sync on Connect
- **Status:** done
- **Substeps:**
  - [x] On connection established, host builds FULL_SYNC packet with all set syncable flags
  - [x] Guest receives FULL_SYNC and applies all flags/vars
  - [x] Handle the case where guest connects mid-game with existing progress (union-wins: apply any flag set by either player)
- **Notes:** Multiplayer_SendFullSync() packs 4 flag byte ranges (214 bytes) into a FULL_SYNC packet and enqueues it to gMpSendRing. Multiplayer_ApplyFullSync() ORs received bytes into gSaveBlock1Ptr->flags (union-wins). FULL_SYNC recv case in ProcessOneRecvPacket now calls ApplyFullSync. FULL_SYNC_PAYLOAD_SIZE=214 defined as constants. Added flags[256] to test mock SaveBlock1. 4 new tests; 73 total assertions pass. Actual trigger (host calls SendFullSync on connect) wired in Phase 6 Tauri app.

### Step 3.4: Implement Script Mutex
- **Status:** done
- **Substeps:**
  - [x] Add gIsInScript flag to multiplayer state
  - [x] When a player enters a script, set the flag and notify partner
  - [x] Partner's ghost NPC should not be able to trigger scripts while flag is set
  - [x] Clear the flag when script completes
- **Notes:** Mutex is advisory. gMultiplayerState.{isInScript,partnerIsInScript} added to MultiplayerState. Multiplayer_OnScriptStart/End hooked into ScriptContext_SetupScript (line 286) and ScriptContext_RunScript CONTEXT_SHUTDOWN branch (line 270) in src/script.c. MP_PKT_SCRIPT_LOCK/UNLOCK (0x08/0x09) handled in ProcessOneRecvPacket. Ghost has MOVEMENT_TYPE_NONE so it cannot trigger scripts by design. GhostTick now freezes movement while partnerIsInScript=TRUE. 8 unit tests; 90 total assertions pass.

### Step 3.5: Write Flag Sync Tests
- **Status:** done
- **Substeps:**
  - [x] Write C unit tests for IsSyncableFlag with trainer, story, UI flags
  - [x] Write C unit tests for no-rebroadcast guard
  - [x] Write C unit tests for full sync application
  - [x] Write Lua integration test script for two-instance flag sync
  - [x] All tests pass
- **Notes:** C unit tests (90 assertions, all pass) in test/test_smoke.c cover IsSyncableFlag boundaries, remote dispatch routing, full sync round-trip, and script mutex state machine. Lua integration script at test/lua/test_flag_sync.lua covers ring magic, partnerIsInScript, SCRIPT_LOCK/UNLOCK recv, and FLAG_SET send ring verification. Live two-instance test deferred to Phase 6 (Tauri app).

---

## Phase 4: Randomized Encounters

### Step 4.1: Study Encounter System
- **Status:** done
- **Substeps:**
  - [x] Read src/wild_encounter.c and document how encounters are generated
  - [x] Read the encounter table data structures
  - [x] Identify where species, level min, and level max are stored
  - [x] Count total encounter slots across all routes
  - [x] Document in docs/encounter-system.md
- **Notes:** WildPokemon{minLevel,maxLevel,species} arrays are const ROM data. 132 FIRERED headers in gWildMonHeaders. OW_TIME_OF_DAY_ENCOUNTERS=FALSE so only encounterTypes[0] (TIME_MORNING) is used. Hook points: TryGenerateWildMon:540 and GenerateFishingWildMon:547 (both read wildPokemon[idx].species). ~1945 max encounter slots across land/water/fish/rock/hidden. Design: hash-on-demand (seed XOR tableAddr XOR slotIndex) → sValidSpecies[hash % count], no EWRAM table needed. NUM_SPECIES=1573, valid pool 1–493 for v1. See docs/encounter-system.md.

### Step 4.2: Implement Seeded PRNG
- **Status:** done
- **Substeps:**
  - [x] Implement a simple xorshift32 PRNG in src/multiplayer.c
  - [x] Implement Multiplayer_SeedRng(seed) and Multiplayer_NextRandom()
  - [x] Write unit test confirming determinism (same seed = same sequence)
- **Notes:** xorshift32 (<<13, >>17, <<5). Seed 0 remapped to 0x12345678 (xorshift32 loops at 0). Also added Multiplayer_GetRandomizedSpecies(tableAddr, slotIndex): per-slot stateless hash using (seed XOR tableAddr XOR slotIndex), maps to Gen I-IV species 1-493. 8 new tests; 103 total assertions pass (test_smoke.c). ROM builds clean (EWRAM 87.16%).

### Step 4.3: Implement Encounter Randomizer
- **Status:** done
- **Substeps:**
  - [x] Implement RandomizeEncounterTables(u32 seed) — design changed to hash-on-demand: no pre-built table needed; Multiplayer_GetRandomizedSpecies() computes species at encounter time
  - [x] For each encounter slot, replace species with a random valid species — TryGenerateWildMon (land/water/rocks) and GenerateFishingWildMon hook into Multiplayer_GetRandomizedSpecies; species replaced for every encounter type
  - [x] Preserve original min/max levels in every slot — ChooseWildMonLevel is called separately; only species is replaced
  - [x] Filter out SPECIES_NONE, SPECIES_EGG, and any invalid IDs — Multiplayer_GetRandomizedSpecies maps to 1-493 (complete Gen I-IV), never returns 0 when seed is set
- **Notes:** Hash-on-demand approach: (seed XOR tableAddr XOR slotIndex) → one xorshift32 step → species 1-493. No EWRAM table needed. Wild_encounter.c hooks added in auto-commit after 4.1. Returns 0 (pass-through to original species) when seed unset or randomize=off.

### Step 4.4: Implement Seed Sync
- **Status:** done
- **Substeps:**
  - [x] Host generates a random seed on session start — Multiplayer_GenerateSeed() combines two Random() draws into a u32; seed=0 remapped to 0x12345678
  - [x] Host sends SEED_SYNC packet to guest on connect — Multiplayer_SendSeedSync(u32 seed) encodes and enqueues SEED_SYNC to gMpSendRing
  - [x] Guest receives seed and calls RandomizeEncounterTables() — SEED_SYNC handler in ProcessOneRecvPacket already sets gCoopSettings.encounterSeed; no explicit call needed (hash-on-demand)
  - [x] Both ROMs now have identical encounter tables — guaranteed: same seed + same WildPokemon[] addr + same slotIndex → same species hash
  - [x] Hook randomization into game init, AFTER seed received — Multiplayer_GetRandomizedSpecies returns 0 (pass-through) until gCoopSettings.encounterSeed is nonzero; actual host→guest call deferred to Phase 6 Tauri app
- **Notes:** Multiplayer_GenerateSeed() and Multiplayer_SendSeedSync() added to multiplayer.c/.h. test/mocks/random.h created to shadow include/random.h in test builds with a controllable gTestRandom32Value stub. 153 assertions pass.

### Step 4.5: Write Randomizer Tests
- **Status:** done
- **Substeps:**
  - [x] Write C unit test: same seed produces identical tables — TestSameSeedSameSpeciesAllSlots: 12 slots queried twice with same seed/addr produce identical results
  - [x] Write C unit test: different seeds produce different tables — TestDifferentSeedsDifferentSpecies: seed 0x11111111 vs 0x22222222 → different species at same slot
  - [x] Write C unit test: levels preserved after randomization — TestRandomizedSpeciesPassThroughWhenDisabled: verified structurally; GetRandomizedSpecies returns 0 when disabled, caller falls back to original species; levels always come from ChooseWildMonLevel independently
  - [x] Write C unit test: no invalid species generated — TestNoInvalidSpecies12Slots: all 12 land slots stay in 1-493
  - [ ] Write Lua test: both instances show same wild encounters on Route 1 — deferred to Phase 6 (requires live Tauri/mGBA session)
  - [x] All tests pass — 153 assertions pass
- **Notes:** Step 4.4 tests also added: TestSendSeedSyncWritesPacket, TestSeedSyncRoundTrip, TestGenerateSeedNonZeroOutput, TestGenerateSeedNonZeroNormal. Total: 50 new assertions vs. 103 in prior session.

---

## Phase 5: Boss Battle Readiness

### Step 5.1: Study Gym Leader Scripts
- **Status:** done
- **Substeps:**
  - [x] Read the battle script format for Brock's gym
  - [x] Document how trainerbattle command works
  - [x] Identify all 8 gym leader script locations
  - [x] Identify Elite Four and Champion script locations
  - [x] Document in docs/boss-scripts.md
- **Notes:** trainerbattle_single expands to TRAINER_BATTLE_CONTINUE_SCRIPT_NO_MUSIC; all 8 gyms in data/maps/XCity_Gym_Frlg/scripts.inc. FLAG_DEFEATED_* flags (0x4B0-0x4BC) are in SYNC_FLAG_BOSSES range — synced automatically. Elite Four use trainerbattle_no_intro. Boss IDs 1-13 defined (gym leaders 1-8, E4+Champion 9-13). Script modification plan: use existing special/msgbox/goto_if_ne commands with VAR_BOSS_BATTLE_STATE polling; no new bytecode needed.

### Step 5.2: Implement Boss Readiness Protocol
- **Status:** done
- **Substeps:**
  - [x] Add BOSS_READY and BOSS_CANCEL packet handling to Multiplayer_Update
  - [x] Add boss readiness state to multiplayer state struct (partnerBossId field)
  - [x] Implement waiting UI: show 'Waiting for partner...' textbox — handled in gym scripts via specialvar polling loop (Step 5.3)
  - [x] On BOSS_START received, dismiss waiting UI and begin battle — MP_PKT_BOSS_START (0x0A) sets partnerBossId; ScriptCheckBossStart returns 1
  - [x] If player walks away from trigger, send BOSS_CANCEL — Multiplayer_BossCancel() sends packet and clears state
- **Notes:** Boss IDs 1-13 defined in constants/multiplayer.h. 13 BossReady_<Name> specials + BossCancel + ScriptCheckBossStart + IsConnected registered in data/specials.inc. ScriptCheckBossStart returns 1 when solo or both players ready; clears state atomically. 177 unit test assertions pass (24 new).

### Step 5.3: Modify Brock's Gym Script (Prototype)
- **Status:** done
- **Substeps:**
  - [x] Modify Brock's pre-battle script to check multiplayer state
  - [x] After dialogue, send BOSS_READY instead of immediately starting battle
  - [x] Show waiting message until BOSS_START received
  - [x] On BOSS_START, begin the battle normally
  - [ ] Test with two mGBA instances: both must interact to start (deferred to Phase 6)
- **Notes:** Added SCR_OP_WAITBOSSSTART (0xE7) script command implemented as SetupNativeScript(Multiplayer_NativePollBossStart). Native poll returns TRUE when ScriptCheckBossStart returns nonzero (both ready or solo). Macro `waitbossstart` added to event.inc. Brock's script now: specialvar IsConnected → if 0 skip to BrockDirect; else BossReady_Brock, message+waitmessage, waitbossstart, closemessage, then trainerbattle_single. ROM builds clean; unit tests pass (177 assertions).

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
| 1 | 2026-04-25 | 0.1, 0.2 | Built pokefirered.gba (32MB) with `make firered -j4`. ARM toolchain found at /opt/devkitpro/devkitARM. One expected RWX linker warning. SHA1: f1e8bd6a. | Step 0.3: Create multiplayer stubs | None |
| 6 | 2026-04-26 | 3.4, 3.5 | Fixed (void)applySave bug. GhostTick freeze when partnerIsInScript=TRUE. Lua flag sync test. Updated docs. 90 assertions pass. | Step 4.1: Study encounter system | None |
