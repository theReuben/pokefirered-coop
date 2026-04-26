#ifndef GUARD_MULTIPLAYER_H
#define GUARD_MULTIPLAYER_H

#include "global.h"
#include "constants/multiplayer.h"
#include "constants/event_objects.h"

// Ghost NPC uses the FRLG Green (Leaf) walking sprite — visually distinct from Red.
#define OBJ_EVENT_GFX_PLAYER2      OBJ_EVENT_GFX_GREEN_NORMAL
// LocalId 0xFE is above any map-defined NPC (maps rarely use IDs > 10).
#define GHOST_LOCAL_ID             0xFE
// Default elevation for overworld spawns.
#define GHOST_ELEVATION            3
// Sentinel value for ghostObjectEventId when no ghost is spawned.
// Must be >= OBJECT_EVENTS_COUNT (16) to pass the "not spawned" guard,
// and != OBJECT_EVENTS_COUNT so that spawn-failure (which also returns 16)
// doesn't accidentally look like a valid spawned slot.
#define GHOST_INVALID_SLOT         0xFF

// Set to 1 to spawn a test ghost at a hardcoded Route 1 position without network.
// Used to verify Step 1.3 rendering in mGBA.  Always 0 in production.
#define MP_DEBUG_TEST_GHOST        0
#define MP_DEBUG_TEST_MAP_GROUP    0   // Pallet Town area map group for Route 1
#define MP_DEBUG_TEST_MAP_NUM      16  // MAP_ROUTE1 index
#define MP_DEBUG_TEST_X            8
#define MP_DEBUG_TEST_Y            5

struct CoopSettings {
    u8  randomizeEncounters : 1;
    u8  padding : 7;
    u32 encounterSeed;
};

struct MultiplayerState {
    u8  role;            // MP_ROLE_*
    u8  connState;       // MP_STATE_*
    u8  partnerMapGroup;
    u8  partnerMapNum;
    u8  targetX;         // target tile X for ghost (last received partner position)
    u8  targetY;         // target tile Y for ghost
    u8  targetFacing;    // facing direction to set when ghost reaches target
    u8  ghostObjectEventId; // OBJECT_EVENTS_COUNT = not spawned
    u8  bossReadyBossId;    // 0 = not in readiness check
    u8  isInScript;
};

extern struct MultiplayerState gMultiplayerState;
extern struct CoopSettings gCoopSettings;

// Core lifecycle
void Multiplayer_Init(void);
void Multiplayer_Update(void);

// Ghost NPC
void Multiplayer_SpawnGhostNPC(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing);
void Multiplayer_DespawnGhost(void);
void Multiplayer_UpdateGhostPosition(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing);

// Packet send helpers (stubs — implemented in Phase 2)
void Multiplayer_SendPosition(void);
void Multiplayer_SendFlagSet(u16 flagId);
void Multiplayer_SendVarSet(u16 varId, u16 value);
void Multiplayer_SendBossReady(u8 bossId);
void Multiplayer_SendBossCancel(void);

// Flag sync helpers (stubs — implemented in Phase 3)
bool32 IsSyncableFlag(u16 flagId);

#endif // GUARD_MULTIPLAYER_H
