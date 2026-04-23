# Testing Plan: Pokémon FireRed Co-Op

## Overview

Testing spans three layers:

1. **C Unit Tests** — test multiplayer module logic in isolation (packet encoding, flag filtering, randomizer determinism)
2. **mGBA Lua Integration Tests** — automated gameplay tests using mGBA's scripting API to press buttons, read memory, and verify game state
3. **Relay Server Tests** — TypeScript tests for the PartyKit server logic
4. **CI Pipeline** — GitHub Actions runs all of the above on every push


---

## 1. C Unit Tests

The multiplayer module (`src/multiplayer.c`) has pure logic that can be tested without the GBA hardware. We compile a native test binary that links against the multiplayer code with mocked GBA dependencies.

### Structure

```
test/
├── Makefile                    # builds and runs native test binaries
├── mocks/
│   ├── mock_gba.h              # stub GBA hardware registers
│   ├── mock_event_data.h       # stub FlagSet/FlagGet/VarSet/VarGet
│   └── mock_link.h             # stub serial I/O
├── test_packets.c              # packet encode/decode tests
├── test_flag_sync.c            # flag whitelist and sync logic tests
├── test_randomizer.c           # encounter randomizer determinism tests
├── test_ghost_npc.c            # ghost NPC spawn/despawn logic tests
└── test_runner.c               # main() that runs all test suites
```

### Test Cases

#### Packet Encoding/Decoding (`test_packets.c`)

```c
// Test: position packet round-trips correctly
void test_position_packet_roundtrip(void) {
    struct PlayerState state = {
        .mapId = 0x0103,
        .x = 14,
        .y = 27,
        .facing = DIR_LEFT,
        .spriteState = SPRITE_STATE_WALKING
    };

    u8 buffer[PACKET_MAX_SIZE];
    u16 len = Multiplayer_EncodePositionPacket(&state, buffer);

    struct PlayerState decoded;
    Multiplayer_DecodePositionPacket(buffer, len, &decoded);

    ASSERT_EQ(state.mapId, decoded.mapId);
    ASSERT_EQ(state.x, decoded.x);
    ASSERT_EQ(state.y, decoded.y);
    ASSERT_EQ(state.facing, decoded.facing);
    ASSERT_EQ(state.spriteState, decoded.spriteState);
}

// Test: flag_set packet encodes correctly
void test_flag_set_packet(void) {
    u8 buffer[PACKET_MAX_SIZE];
    u16 len = Multiplayer_EncodeFlagSetPacket(FLAG_DEFEATED_BROCK, buffer);

    ASSERT_EQ(buffer[0], PACKET_TYPE_FLAG_SET);

    u16 decodedFlag;
    Multiplayer_DecodeFlagSetPacket(buffer, len, &decodedFlag);
    ASSERT_EQ(decodedFlag, FLAG_DEFEATED_BROCK);
}

// Test: malformed packet returns error
void test_malformed_packet_rejected(void) {
    u8 garbage[] = {0xFF, 0x00};
    int result = Multiplayer_DecodePacket(garbage, sizeof(garbage), NULL);
    ASSERT_EQ(result, PACKET_ERR_INVALID_TYPE);
}

// Test: packet with truncated payload returns error
void test_truncated_packet_rejected(void) {
    u8 buffer[] = {PACKET_TYPE_POSITION, 0x01}; // missing bytes
    int result = Multiplayer_DecodePacket(buffer, sizeof(buffer), NULL);
    ASSERT_EQ(result, PACKET_ERR_TOO_SHORT);
}
```

#### Flag Sync Logic (`test_flag_sync.c`)

```c
// Test: trainer flags are syncable
void test_trainer_flags_syncable(void) {
    ASSERT_TRUE(IsSyncableFlag(FLAG_TRAINER_FLAG_START));
    ASSERT_TRUE(IsSyncableFlag(FLAG_TRAINER_FLAG_START + 100));
    ASSERT_TRUE(IsSyncableFlag(FLAG_TRAINER_FLAG_END));
}

// Test: UI/menu flags are NOT syncable
void test_ui_flags_not_syncable(void) {
    ASSERT_FALSE(IsSyncableFlag(FLAG_SYS_POKEMON_GET));
    ASSERT_FALSE(IsSyncableFlag(FLAG_BADGE01_GET)); // badges sync via story flags, not directly
    // Add other known non-sync flags
}

// Test: remote flag set does not re-broadcast
void test_no_rebroadcast_on_remote_flag(void) {
    gSendPacketCallCount = 0;
    Multiplayer_HandleRemoteFlagSet(FLAG_TRAINER_FLAG_START + 1);
    ASSERT_EQ(gSendPacketCallCount, 0); // should NOT have sent a packet
}

// Test: local flag set DOES broadcast
void test_local_flag_broadcasts(void) {
    gSendPacketCallCount = 0;
    FlagSet(FLAG_TRAINER_FLAG_START + 1); // hooked version
    ASSERT_EQ(gSendPacketCallCount, 1);
}

// Test: full sync applies all flags
void test_full_sync_applies_flags(void) {
    u16 flags[] = {FLAG_TRAINER_FLAG_START, FLAG_TRAINER_FLAG_START + 5, FLAG_TRAINER_FLAG_START + 10};
    Multiplayer_HandleFullSync(flags, 3, NULL, 0);

    ASSERT_TRUE(FlagGet(FLAG_TRAINER_FLAG_START));
    ASSERT_TRUE(FlagGet(FLAG_TRAINER_FLAG_START + 5));
    ASSERT_TRUE(FlagGet(FLAG_TRAINER_FLAG_START + 10));
}
```

#### Randomizer Determinism (`test_randomizer.c`)

```c
// Test: same seed produces identical encounter tables
void test_randomizer_deterministic(void) {
    u32 seed = 0xDEADBEEF;

    // Run randomizer twice with the same seed
    RandomizeEncounterTables(seed);
    u16 species_run1[NUM_ENCOUNTER_SLOTS];
    CopyAllEncounterSpecies(species_run1);

    // Reset encounter tables to defaults
    ResetEncounterTables();

    RandomizeEncounterTables(seed);
    u16 species_run2[NUM_ENCOUNTER_SLOTS];
    CopyAllEncounterSpecies(species_run2);

    for (int i = 0; i < NUM_ENCOUNTER_SLOTS; i++) {
        ASSERT_EQ(species_run1[i], species_run2[i]);
    }
}

// Test: different seeds produce different tables
void test_randomizer_different_seeds(void) {
    RandomizeEncounterTables(0x11111111);
    u16 species_a[NUM_ENCOUNTER_SLOTS];
    CopyAllEncounterSpecies(species_a);

    ResetEncounterTables();

    RandomizeEncounterTables(0x22222222);
    u16 species_b[NUM_ENCOUNTER_SLOTS];
    CopyAllEncounterSpecies(species_b);

    int differences = 0;
    for (int i = 0; i < NUM_ENCOUNTER_SLOTS; i++) {
        if (species_a[i] != species_b[i]) differences++;
    }
    ASSERT_GT(differences, NUM_ENCOUNTER_SLOTS / 2); // most slots should differ
}

// Test: randomizer preserves level ranges
void test_randomizer_preserves_levels(void) {
    // Save original levels
    u8 origMin[NUM_ENCOUNTER_SLOTS], origMax[NUM_ENCOUNTER_SLOTS];
    CopyAllEncounterLevels(origMin, origMax);

    RandomizeEncounterTables(0xCAFEBABE);

    u8 newMin[NUM_ENCOUNTER_SLOTS], newMax[NUM_ENCOUNTER_SLOTS];
    CopyAllEncounterLevels(newMin, newMax);

    for (int i = 0; i < NUM_ENCOUNTER_SLOTS; i++) {
        ASSERT_EQ(origMin[i], newMin[i]);
        ASSERT_EQ(origMax[i], newMax[i]);
    }
}

// Test: no invalid species IDs generated
void test_randomizer_valid_species(void) {
    RandomizeEncounterTables(0x12345678);
    u16 species[NUM_ENCOUNTER_SLOTS];
    CopyAllEncounterSpecies(species);

    for (int i = 0; i < NUM_ENCOUNTER_SLOTS; i++) {
        ASSERT_GT(species[i], SPECIES_NONE);
        ASSERT_LT(species[i], NUM_SPECIES);
        ASSERT_NE(species[i], SPECIES_EGG);
    }
}
```

### Building & Running

```makefile
# test/Makefile
CC = gcc
CFLAGS = -Wall -Wextra -I../include -I./mocks -DTESTING

TESTS = test_packets test_flag_sync test_randomizer test_ghost_npc

all: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t ==="; ./$$t || exit 1; done
	@echo "All tests passed."

test_%: test_%.c ../src/multiplayer.c test_runner.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TESTS)
```

Run with `cd test && make`.


---

## 2. mGBA Lua Integration Tests

mGBA's Lua scripting API lets us automate gameplay: press buttons, advance frames, read/write memory, and use TCP sockets. This is the key tool for testing the ROM mod end-to-end.

### How It Works

1. Start mGBA headlessly (via `xvfb-run mgba-qt` on Linux CI, or `mgba -s` for CLI)
2. Load the ROM and a Lua test script
3. The Lua script presses buttons, waits for game states, reads memory, and reports pass/fail
4. For multiplayer tests: start TWO mGBA instances connected via link cable, each running a test script

### Memory Map (define in a shared Lua module)

You'll need to find the RAM addresses for key game state. These come from the decomp's linker map or `.sym` file.

```lua
-- test/lua/memory_map.lua
-- These addresses must be updated from the build's .sym file

ADDR = {
    -- Player position
    PLAYER_X        = 0x02000000, -- placeholder, find real addr
    PLAYER_Y        = 0x02000004,
    PLAYER_MAP_ID   = 0x02000008,
    PLAYER_FACING   = 0x0200000C,

    -- Ghost NPC
    GHOST_ACTIVE    = 0x02000100, -- placeholder
    GHOST_X         = 0x02000104,
    GHOST_Y         = 0x02000108,
    GHOST_MAP_ID    = 0x0200010C,

    -- Multiplayer state
    MP_CONNECTED    = 0x02000200,
    MP_ROLE         = 0x02000204, -- 0=host, 1=guest
    MP_SEED         = 0x02000208,

    -- Flags (base address of flag array)
    FLAGS_BASE      = 0x02000300,
}

-- Helper: read a flag by ID
function isFlagSet(flagId)
    local byteIndex = math.floor(flagId / 8)
    local bitIndex = flagId % 8
    local byte = emu:read8(ADDR.FLAGS_BASE + byteIndex)
    return (byte & (1 << bitIndex)) ~= 0
end
```

### Test Scripts

#### Test: Ghost NPC appears when partner connects (`test_ghost_spawn.lua`)

```lua
-- Run on Instance 1 (host)
-- Instance 2 runs a script that just walks right 3 tiles after connecting

dofile("test/lua/memory_map.lua")
dofile("test/lua/helpers.lua")

local testName = "ghost_npc_spawn"
local TIMEOUT_FRAMES = 600 -- 10 seconds at 60fps

-- Wait for game to reach the overworld
waitForOverworld(TIMEOUT_FRAMES)

-- Wait for multiplayer connection
waitForCondition(function()
    return emu:read8(ADDR.MP_CONNECTED) == 1
end, TIMEOUT_FRAMES, "multiplayer connection")

-- Wait for ghost NPC to appear on this map
waitForCondition(function()
    return emu:read8(ADDR.GHOST_ACTIVE) == 1
end, TIMEOUT_FRAMES, "ghost NPC spawn")

-- Verify ghost NPC is on the same map as us
local myMap = emu:read16(ADDR.PLAYER_MAP_ID)
local ghostMap = emu:read16(ADDR.GHOST_MAP_ID)
assert(myMap == ghostMap, string.format(
    "FAIL [%s]: ghost map %d != player map %d", testName, ghostMap, myMap
))

-- Wait a moment, then check that ghost position updates
local ghostX1 = emu:read16(ADDR.GHOST_X)
advanceFrames(120) -- wait 2 seconds for partner to move
local ghostX2 = emu:read16(ADDR.GHOST_X)

assert(ghostX1 ~= ghostX2, string.format(
    "FAIL [%s]: ghost position didn't change (stuck at x=%d)", testName, ghostX1
))

reportPass(testName)
```

#### Test: Flag sync between instances (`test_flag_sync.lua`)

```lua
-- Run on Instance 2 (guest)
-- Instance 1 runs a script that defeats a trainer

dofile("test/lua/memory_map.lua")
dofile("test/lua/helpers.lua")

local testName = "flag_sync"
local TRAINER_FLAG = 0x1234 -- replace with actual trainer flag ID
local TIMEOUT_FRAMES = 1800 -- 30 seconds

waitForOverworld(TIMEOUT_FRAMES)
waitForCondition(function()
    return emu:read8(ADDR.MP_CONNECTED) == 1
end, TIMEOUT_FRAMES, "multiplayer connection")

-- Verify flag is NOT set initially
assert(not isFlagSet(TRAINER_FLAG), string.format(
    "FAIL [%s]: trainer flag already set before test", testName
))

-- Wait for flag to be set by partner (Instance 1 is fighting the trainer)
waitForCondition(function()
    return isFlagSet(TRAINER_FLAG)
end, TIMEOUT_FRAMES, "trainer flag sync from partner")

reportPass(testName)
```

#### Test: Randomizer seed sync (`test_seed_sync.lua`)

```lua
-- Run on Instance 2 (guest)
-- Instance 1 hosts with a known seed

dofile("test/lua/memory_map.lua")
dofile("test/lua/helpers.lua")

local testName = "seed_sync"
local EXPECTED_SEED = 0xDEADBEEF
local TIMEOUT_FRAMES = 600

waitForOverworld(TIMEOUT_FRAMES)
waitForCondition(function()
    return emu:read8(ADDR.MP_CONNECTED) == 1
end, TIMEOUT_FRAMES, "multiplayer connection")

-- Check that the seed was received
local seed = emu:read32(ADDR.MP_SEED)
assert(seed == EXPECTED_SEED, string.format(
    "FAIL [%s]: seed 0x%08X != expected 0x%08X", testName, seed, EXPECTED_SEED
))

-- Check that encounter tables match host
-- (Compare a sample of encounter slots against known values for this seed)
-- This requires knowing the encounter table addresses from the .sym file
-- ...

reportPass(testName)
```

### Lua Test Helpers (`test/lua/helpers.lua`)

```lua
function advanceFrames(n)
    for i = 1, n do
        emu:runFrame()
    end
end

function waitForOverworld(timeout)
    -- Wait until the game reaches the overworld
    -- Detect by checking a known game state variable
    local frames = 0
    while frames < timeout do
        emu:runFrame()
        frames = frames + 1
        -- Check for overworld state (callback ID or similar)
        local state = emu:read8(ADDR.GAME_STATE or 0x020000FF)
        if state == 0x01 then -- OVERWORLD state value, find the real one
            return
        end
    end
    error("TIMEOUT waiting for overworld")
end

function waitForCondition(fn, timeout, description)
    local frames = 0
    while frames < timeout do
        emu:runFrame()
        frames = frames + 1
        if fn() then return end
    end
    error(string.format("TIMEOUT after %d frames waiting for: %s", timeout, description))
end

function pressButton(key, holdFrames)
    holdFrames = holdFrames or 4
    for i = 1, holdFrames do
        emu:addKey(key)
        emu:runFrame()
    end
    for i = 1, 4 do
        emu:clearKeys()
        emu:runFrame()
    end
end

function walkDirection(dir, tiles)
    local key = ({
        up = C.GBA_KEY.UP,
        down = C.GBA_KEY.DOWN,
        left = C.GBA_KEY.LEFT,
        right = C.GBA_KEY.RIGHT
    })[dir]
    for i = 1, tiles do
        pressButton(key, 16) -- 16 frames per tile
    end
end

function reportPass(testName)
    console:log(string.format("PASS [%s]", testName))
    -- Write to a results file so CI can parse it
    local f = io.open("test_results.txt", "a")
    f:write(string.format("PASS %s\n", testName))
    f:close()
end

function reportFail(testName, reason)
    console:log(string.format("FAIL [%s]: %s", testName, reason))
    local f = io.open("test_results.txt", "a")
    f:write(string.format("FAIL %s %s\n", testName, reason))
    f:close()
end
```

### Running Lua Tests Locally

```bash
# Single-instance test (ghost NPC with mock data)
mgba-qt -l test/lua/test_ghost_spawn.lua path/to/pokefirered_coop.gba

# Two-instance multiplayer test
# Terminal 1:
mgba-qt --link-type=normal --link-port=1 -l test/lua/test_host.lua pokefirered_coop.gba
# Terminal 2:
mgba-qt --link-type=normal --link-port=2 -l test/lua/test_guest.lua pokefirered_coop.gba
```

### Running Headless on CI

```bash
# Use xvfb for headless rendering on Linux
xvfb-run mgba-qt -l test/lua/test_ghost_spawn.lua pokefirered_coop.gba &
PID=$!
sleep 30  # let test run
kill $PID

# Check results
grep -q "FAIL" test_results.txt && exit 1
echo "All integration tests passed"
```


---

## 3. Relay Server Tests

Standard TypeScript tests using Vitest. Test the PartyKit server logic in isolation with mock WebSocket connections.

### Structure

```
relay-server/
├── server.ts
├── server.test.ts
├── package.json
└── vitest.config.ts
```

### Test Cases (`server.test.ts`)

```typescript
import { describe, it, expect, beforeEach } from 'vitest';

// We test the server logic by simulating the message handling
// PartyKit provides a test harness, or we mock the Room/Connection interfaces

interface MockConnection {
  id: string;
  messages: string[];
  closed: boolean;
  send(msg: string): void;
  close(): void;
}

function createMockConnection(id: string): MockConnection {
  return {
    id,
    messages: [],
    closed: false,
    send(msg: string) { this.messages.push(msg); },
    close() { this.closed = true; },
  };
}

describe('Relay Server', () => {

  describe('Connection Management', () => {
    it('assigns host role to first connection', () => {
      // Simulate first player connecting
      // Verify they receive { type: "role", role: "host" }
    });

    it('assigns guest role to second connection', () => {
      // Simulate second player connecting
      // Verify they receive { type: "role", role: "guest" }
    });

    it('rejects third connection with room_full', () => {
      // Simulate third player connecting
      // Verify they receive { type: "room_full" } and connection is closed
    });

    it('notifies host when guest connects', () => {
      // Verify host receives { type: "partner_connected" }
    });

    it('notifies remaining player on disconnect', () => {
      // Verify partner receives { type: "partner_disconnected" }
    });
  });

  describe('Position Relay', () => {
    it('relays position from host to guest', () => {
      // Host sends position update
      // Verify guest receives partner_position with matching data
    });

    it('does not echo position back to sender', () => {
      // Host sends position
      // Verify host does NOT receive their own position back
    });

    it('stores latest position for late-joining guest', () => {
      // Host sends position, then guest connects
      // Verify guest receives host's position on connect
    });
  });

  describe('Flag Sync', () => {
    it('stores and broadcasts flag_set', () => {
      // Host sends flag_set with flagId 100
      // Verify both players receive the flag_set
      // Verify server state includes the flag
    });

    it('deduplicates flag_set (idempotent)', () => {
      // Send same flag_set twice
      // Verify it's only stored once and only broadcast once
    });

    it('sends full flag state to newly connected guest', () => {
      // Host sets 3 flags, then guest connects
      // Verify guest receives full_sync with all 3 flags
    });

    it('handles both players setting the same flag simultaneously', () => {
      // Both send flag_set for the same flag
      // Verify no errors and flag is set once
    });
  });

  describe('Boss Readiness', () => {
    it('sends boss_waiting when only one player is ready', () => {
      // Host sends boss_ready
      // Verify host receives boss_waiting
    });

    it('sends boss_start to both when both are ready', () => {
      // Host sends boss_ready, guest sends boss_ready
      // Verify both receive boss_start with matching bossId
    });

    it('resets readiness after boss_start', () => {
      // Both ready, boss starts, then one sends boss_ready again
      // Verify they get boss_waiting (not immediate start)
    });

    it('handles boss_cancel correctly', () => {
      // Host sends boss_ready, then boss_cancel
      // Guest sends boss_ready
      // Verify guest gets boss_waiting (not boss_start)
    });
  });

  describe('Battle Turn Relay', () => {
    it('relays turn data from host to guest', () => {
      // Host sends battle_turn with encoded data
      // Verify guest receives it unchanged
    });

    it('does not echo turn data back to sender', () => {
      // Same as position: no echo
    });
  });

  describe('Edge Cases', () => {
    it('handles rapid position updates without dropping', () => {
      // Send 60 position updates in quick succession
      // Verify all are relayed (or latest-wins is acceptable)
    });

    it('handles reconnection gracefully', () => {
      // Guest disconnects, reconnects
      // Verify they receive full_sync with current state
      // Verify they get new role assignment
    });

    it('handles messages from unknown connection', () => {
      // Send message from a connection that wasn't registered
      // Verify no crash, message is ignored
    });
  });
});
```

### Running

```bash
cd relay-server
npm install
npx vitest run
```


---

## 4. CI Pipeline

### GitHub Actions Workflow (`.github/workflows/test.yml`)

```yaml
name: Test

on:
  push:
    branches: [main]
  pull_request:

jobs:
  # ──── C Unit Tests ────
  unit-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install build dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y gcc make

      - name: Run C unit tests
        run: |
          cd test
          make

  # ──── Relay Server Tests ────
  server-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - uses: actions/setup-node@v4
        with:
          node-version: '20'

      - name: Install and test relay server
        run: |
          cd relay-server
          npm install
          npx vitest run

  # ──── ROM Build Verification ────
  rom-build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install devkitARM and build tools
        run: |
          # Install devkitARM (or use a Docker image)
          # This varies by setup — see pokeemerald-expansion INSTALL.md
          sudo apt-get update
          sudo apt-get install -y build-essential binutils-arm-none-eabi

      - name: Build ROM
        run: make

      - name: Verify ROM boots
        run: |
          # Quick sanity: ROM file exists and is reasonable size
          test -f pokefirered_coop.gba
          SIZE=$(stat -f%z pokefirered_coop.gba 2>/dev/null || stat --format=%s pokefirered_coop.gba)
          test $SIZE -gt 1000000  # at least 1MB

      - name: Upload ROM artifact
        uses: actions/upload-artifact@v4
        with:
          name: rom
          path: pokefirered_coop.gba

  # ──── mGBA Integration Tests ────
  integration-tests:
    runs-on: ubuntu-latest
    needs: rom-build
    steps:
      - uses: actions/checkout@v4

      - name: Download ROM
        uses: actions/download-artifact@v4
        with:
          name: rom

      - name: Install mGBA
        run: |
          sudo apt-get update
          sudo apt-get install -y mgba-qt xvfb

      - name: Run single-instance tests
        run: |
          xvfb-run timeout 60 mgba-qt \
            -l test/lua/test_boot.lua \
            pokefirered_coop.gba || true
          grep -q "FAIL" test_results.txt && exit 1

      - name: Run multiplayer sync tests
        run: |
          # Start host instance
          xvfb-run timeout 60 mgba-qt \
            --link-type=normal --link-port=1 \
            -l test/lua/test_host_sync.lua \
            pokefirered_coop.gba &

          # Start guest instance
          xvfb-run timeout 60 mgba-qt \
            --link-type=normal --link-port=2 \
            -l test/lua/test_guest_sync.lua \
            pokefirered_coop.gba &

          wait
          grep -q "FAIL" test_results.txt && exit 1
          echo "All integration tests passed"
```


---

## 5. Test Matrix

| Test | What It Validates | Layer | Automated |
|------|-------------------|-------|-----------|
| Packet round-trip | Encode/decode integrity | C unit | Yes |
| Flag whitelist | Only sync flags are sent | C unit | Yes |
| No rebroadcast | Remote flags don't loop | C unit | Yes |
| Randomizer determinism | Same seed = same tables | C unit | Yes |
| Randomizer level preservation | Levels unchanged | C unit | Yes |
| Valid species only | No SPECIES_NONE or SPECIES_EGG | C unit | Yes |
| ROM boots | Game reaches title screen | mGBA Lua | Yes |
| Ghost NPC spawns | P2 sprite appears | mGBA Lua | Yes |
| Ghost NPC moves | Position updates from partner | mGBA Lua | Yes |
| Ghost NPC collision | Can't walk through P2 | mGBA Lua | Yes |
| Ghost NPC cross-map | Despawns when partner leaves map | mGBA Lua | Yes |
| Flag sync | Trainer flag propagates | mGBA Lua | Yes |
| Full sync on connect | Late joiner gets all flags | mGBA Lua | Yes |
| Seed sync | Guest receives host's seed | mGBA Lua | Yes |
| Boss readiness | Both ready → battle starts | mGBA Lua | Yes |
| Role assignment | Host=first, guest=second | Server | Yes |
| Room capacity | Third player rejected | Server | Yes |
| Position relay | Host→guest, no echo | Server | Yes |
| Flag deduplication | Same flag not stored twice | Server | Yes |
| Disconnect handling | Partner notified | Server | Yes |
| Boss cancel | Cancelled readiness resets | Server | Yes |
| Full playthrough | Beat all 8 gyms co-op | Manual | No (QA) |
| Battle sync | Double battle stays in sync | Manual | No (QA) |


---

## 6. Generating the Memory Map

The Lua tests need real memory addresses. After building the ROM, extract them from the linker output:

```bash
# After `make`, the .sym or .map file contains symbol addresses
# Extract the ones we care about into a Lua file

python3 test/extract_symbols.py \
    pokefirered_coop.map \
    --symbols gPlayerPosition,gGhostNpcState,gMultiplayerState,gSaveBlock1Ptr \
    --output test/lua/memory_map.lua
```

This script parses the `.map` file and generates the `ADDR` table so tests always use correct addresses even as the code changes. Run it as part of the build step before integration tests.


---

## 7. Adding a New Test (Checklist)

When adding a new multiplayer feature, also add:

- [ ] At least one C unit test in `test/test_*.c` for the pure logic
- [ ] A Lua integration test if the feature affects visible game state
- [ ] A relay server test if the feature adds a new message type
- [ ] Update the test matrix table above
- [ ] Verify CI passes before merging
