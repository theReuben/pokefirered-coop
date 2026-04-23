# Master Plan: Pokémon FireRed Co-Op Multiplayer

## Important Context

This project adds networked 2-player co-op to Pokémon FireRed using the `rh-hideout/pokeemerald-expansion` decomp as a base. The codebase is C compiled for the GBA with agbcc/devkitARM. You are modifying a decompiled game ROM — be careful with every change and always verify the ROM still builds and boots.

### Key Constraints

- **Minimal changes:** Only modify files that are strictly necessary. The decomp codebase is large and fragile.
- **Non-blocking:** All network reads must be non-blocking. Never stall the game loop waiting for data.
- **Idempotent flags:** Flag sync is last-write-wins and idempotent. A flag can only be set, never unset.
- **No save changes:** Multiplayer state is session-only. Do NOT modify the save file format.
- **Test everything:** Every new function in multiplayer.c gets a corresponding C unit test. Every integration point gets a Lua test script.

### Architecture Reference

See CLAUDE.md for the full architecture diagram, message protocol, and file map.

---

## Step 0.1: Enable FRLG Build Mode

The pokeemerald-expansion supports building as FireRed/LeafGreen instead of Emerald. Find the configuration option — it may be a `Makefile` variable, a `config.h` define, or a build flag. Check:
- `Makefile` for a `GAME_VERSION` or `GAME` variable
- `include/config.h` or similar for `#define FIRERED`
- The expansion's documentation or wiki for FRLG build instructions
- The 1.15.0 release notes which introduced FRLG support

Set it and run `make`. Fix any errors. The goal is a clean build producing a FireRed ROM.

## Step 0.3: Set Up Project Structure

Create these files with empty/stub implementations:

```c
// src/multiplayer.c
#include "global.h"
#include "multiplayer.h"

void Multiplayer_Init(void) { }
void Multiplayer_Update(void) { }
```

```c
// include/multiplayer.h
#ifndef GUARD_MULTIPLAYER_H
#define GUARD_MULTIPLAYER_H

void Multiplayer_Init(void);
void Multiplayer_Update(void);

#endif
```

Add `multiplayer.c` to the build. Check how other source files are included — likely in a `Makefile`, `sources.mk`, or similar. Follow the same pattern.

## Step 0.4: Set Up Test Infrastructure

The test framework compiles multiplayer logic with `gcc` on the host (not the GBA cross-compiler). This means you need mock headers that stub out GBA-specific types and functions.

```c
// test/mocks/mock_gba.h
#ifndef MOCK_GBA_H
#define MOCK_GBA_H

#include <stdint.h>
#include <stdbool.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef bool bool8;
typedef bool bool32;

#define TRUE true
#define FALSE false

#endif
```

The test runner should be simple — no external test framework dependencies:

```c
// test/test_runner.c
#include <stdio.h>
#include <stdlib.h>

int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;

#define ASSERT_EQ(a, b) do { \
    tests_run++; \
    if ((a) == (b)) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL: %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); } \
} while(0)

#define ASSERT_TRUE(x)  ASSERT_EQ(!!(x), 1)
#define ASSERT_FALSE(x) ASSERT_EQ(!!(x), 0)
#define ASSERT_NE(a, b) do { \
    tests_run++; \
    if ((a) != (b)) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } \
} while(0)
#define ASSERT_GT(a, b) do { \
    tests_run++; \
    if ((a) > (b)) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL: %s:%d: %s not > %s\n", __FILE__, __LINE__, #a, #b); } \
} while(0)
```

---

## Step 1.1: Study Object Event System

Read these files carefully and take notes:
- `src/event_object_movement.c` — focus on `ObjectEventSetPosition`, movement action tables, sprite animation
- `include/global.fieldmap.h` — the `struct ObjectEvent` definition, especially fields: `localId`, `mapNum`, `mapGroup`, `currentCoords`, `facingDirection`, `movementType`, `graphicsId`
- `src/field_player_avatar.c` — how `gObjectEvents[gPlayerAvatar.objectEventId]` is used

Key questions to answer in docs/object-events.md:
1. How is a new ObjectEvent created on a map?
2. How is an ObjectEvent's position updated each frame?
3. How does collision detection work for ObjectEvents?
4. How are walk animations triggered?
5. What's the difference between a "template" ObjectEvent (defined in map data) and a dynamically spawned one?

## Step 1.3: Implement Ghost NPC Spawn/Despawn

The ghost NPC should be a dynamically created ObjectEvent — NOT defined in any map's event data. Look at how NPCs are added at runtime (e.g., follower Pokémon in the expansion, or event-triggered NPCs).

Key implementation details:
- Reserve an ObjectEvent slot for the ghost (e.g., the last slot in `gObjectEvents`)
- The ghost must have `MOVEMENT_TYPE_NONE` — it doesn't move autonomously
- Set the `disableAnim` flag if it exists, or use a movement type that doesn't auto-walk
- Enable collision by ensuring `singleMovementActive` or the equivalent collision flag is set
- Disable script triggers by NOT assigning a `trainerType` or `script` pointer

## Step 2.5: Implement Serial Send/Receive

The link cable interface on GBA uses memory-mapped I/O registers. The existing `src/link.c` already handles the low-level serial communication. You have two approaches:

**Approach A (recommended):** Hook into the existing link cable callback system. The decomp already has interrupt handlers for serial communication. Add your packet handling alongside or instead of the normal link protocol.

**Approach B:** Create a completely new serial interface that bypasses the existing link system. More isolated but more work.

For either approach, the communication must be:
- Non-blocking on receive (check if data available, return immediately if not)
- Buffered on send (queue packets, send one per frame or batch)
- Error-tolerant (dropped packets should not crash — just skip that update)

## Step 3.2: Hook FlagSet and VarSet

The hook must be surgically precise. In `src/event_data.c`:

```c
// BEFORE (original):
u8 *ptr = GetFlagPointer(id);
if (ptr)
    *ptr |= 1 << (id & 7);

// AFTER (with multiplayer hook):
u8 *ptr = GetFlagPointer(id);
if (ptr) {
    *ptr |= 1 << (id & 7);
    if (!sIsRemoteUpdate && IsSyncableFlag(id))
        Multiplayer_SendFlagSet(id);
}
```

The `sIsRemoteUpdate` guard is critical. Without it, receiving a flag from the partner would trigger another send, creating an infinite loop.

## Step 4.3: Implement Encounter Randomizer

The encounter tables are arrays of structs with species, level min, and level max. The randomizer replaces species in-place using a seeded PRNG. The PRNG must be a simple, deterministic algorithm — NOT the game's built-in RNG which has side effects.

Xorshift32 is ideal:
```c
static u32 sRngState;

void Multiplayer_SeedRng(u32 seed) {
    sRngState = seed;
    if (sRngState == 0) sRngState = 1; // xorshift can't have zero state
}

u32 Multiplayer_NextRandom(void) {
    sRngState ^= sRngState << 13;
    sRngState ^= sRngState >> 17;
    sRngState ^= sRngState << 5;
    return sRngState;
}
```

To get a species: `species = (Multiplayer_NextRandom() % (NUM_SPECIES - 1)) + 1`

Filter out invalid results: SPECIES_NONE (0), SPECIES_EGG, and any species the expansion marks as invalid.

## Step 5.3: Modify Brock's Gym Script

The script format uses a custom assembly-like language. A typical trainer battle looks like:

```
trainerbattle_single TRAINER_BROCK, BrockIntroText, BrockDefeatText
```

You need to modify this to:
1. Show intro dialogue normally
2. Instead of `trainerbattle_single`, call a custom script command that sends BOSS_READY
3. Enter a loop displaying "Waiting for partner..." that checks for BOSS_START
4. On BOSS_START, execute the `trainerbattle_single` command

This likely requires adding a new script command (in `src/scrcmd.c`) or using `waitstate` with a callback that checks multiplayer state.

---

## General Notes for All Steps

- After EVERY code change, run `make` to verify the ROM still builds
- After EVERY test change, run `cd test && make` to verify tests pass
- Commit after each substep with a descriptive message
- If a step requires understanding a system, write documentation BEFORE implementing
- If something seems wrong or unclear, document the question in docs/open-questions.md rather than guessing
