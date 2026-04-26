#include "test_runner.h"
#include "global.h"
#include "multiplayer.h"
#include "constants/event_objects.h"
#include <string.h>

// Declared in stubs.c so tests can control spawn behaviour.
extern u8 gTestNextSpawnSlot;

// ---- Test helpers --------------------------------------------------------

static struct SaveBlock1 sTestSave;

static void ResetAll(void)
{
    Multiplayer_Init();
    memset(gObjectEvents, 0, sizeof(gObjectEvents));
    gSaveBlock1Ptr    = NULL;
    gTestNextSpawnSlot = 16; // no free slot by default
}

static void SetPlayerMap(s8 mapGroup, s8 mapNum)
{
    sTestSave.location.mapGroup = mapGroup;
    sTestSave.location.mapNum   = mapNum;
    gSaveBlock1Ptr              = &sTestSave;
}

// ---- Step 1.2: Sprite constant -------------------------------------------

static void TestSpriteConstant(void)
{
    // OBJ_EVENT_GFX_PLAYER2 must map to the FRLG Green (Leaf) walking sprite.
    ASSERT_EQ(OBJ_EVENT_GFX_PLAYER2, OBJ_EVENT_GFX_GREEN_NORMAL);
    ASSERT_EQ(OBJ_EVENT_GFX_GREEN_NORMAL, 251);
}

// ---- Step 1.2 / 1.3: Init state ------------------------------------------

static void TestInit(void)
{
    ResetAll();
    ASSERT_EQ(gMultiplayerState.role,               MP_ROLE_NONE);
    ASSERT_EQ(gMultiplayerState.connState,          MP_STATE_DISCONNECTED);
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, GHOST_INVALID_SLOT);
    ASSERT_EQ(gMultiplayerState.bossReadyBossId,    0);
    ASSERT_EQ(gMultiplayerState.isInScript,         FALSE);
    ASSERT_EQ(gCoopSettings.randomizeEncounters,    1);
    ASSERT_EQ(gCoopSettings.encounterSeed,          0u);
}

// ---- Step 1.3: Spawn -------------------------------------------------------

static void TestSpawnNoFreeSlot(void)
{
    // Default stub returns slot 16 (no free slot); ghostObjectEventId stays invalid.
    ResetAll();
    Multiplayer_SpawnGhostNPC(0, 5, 3, 4, DIR_SOUTH);
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, GHOST_INVALID_SLOT);
}

static void TestSpawnSuccess(void)
{
    ResetAll();
    gTestNextSpawnSlot = 5;
    Multiplayer_SpawnGhostNPC(0, 5, 3, 4, DIR_SOUTH);
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, 5);
    ASSERT_NE(gObjectEvents[5].active, 0);
    ASSERT_EQ(gObjectEvents[5].mapGroup, 0);
    ASSERT_EQ(gObjectEvents[5].mapNum,   5);
}

static void TestSpawnReplacesPreviousGhost(void)
{
    // Spawning a second time should despawn the old ghost first.
    ResetAll();
    gTestNextSpawnSlot = 3;
    Multiplayer_SpawnGhostNPC(0, 1, 2, 2, DIR_SOUTH);
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, 3);

    gTestNextSpawnSlot = 7;
    Multiplayer_SpawnGhostNPC(0, 2, 4, 4, DIR_NORTH);
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, 7);
    ASSERT_EQ(gObjectEvents[3].active, 0); // old slot deactivated
    ASSERT_NE(gObjectEvents[7].active, 0); // new slot active
}

// ---- Step 1.3: Despawn ----------------------------------------------------

static void TestDespawnClearsId(void)
{
    ResetAll();
    gMultiplayerState.ghostObjectEventId = 3;
    gObjectEvents[3].active = 1;
    Multiplayer_DespawnGhost();
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, GHOST_INVALID_SLOT);
    ASSERT_EQ(gObjectEvents[3].active, 0);
}

static void TestDespawnWhenAlreadyDespawned(void)
{
    // Must not crash and must leave ID as GHOST_INVALID_SLOT.
    ResetAll();
    Multiplayer_DespawnGhost();
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, GHOST_INVALID_SLOT);
}

// ---- Step 1.4: UpdateGhostPosition ----------------------------------------

static void TestUpdateGhostPosition(void)
{
    ResetAll();
    Multiplayer_UpdateGhostPosition(0, 7, 10, 12, DIR_EAST);
    ASSERT_EQ(gMultiplayerState.partnerMapGroup, 0);
    ASSERT_EQ(gMultiplayerState.partnerMapNum,   7);
    ASSERT_EQ(gMultiplayerState.targetX,         10);
    ASSERT_EQ(gMultiplayerState.targetY,         12);
    ASSERT_EQ(gMultiplayerState.targetFacing,    DIR_EAST);
}

static void TestGhostTickMovesWhenOffTarget(void)
{
    // Ghost at (5,5), target at (7,5): GhostTick should request WALK_RIGHT.
    ResetAll();
    gTestNextSpawnSlot = 4;
    Multiplayer_SpawnGhostNPC(0, 1, 5, 5, DIR_SOUTH);
    gMultiplayerState.targetX = 7;
    gMultiplayerState.targetY = 5;
    SetPlayerMap(0, 1);
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    Multiplayer_UpdateGhostPosition(0, 1, 7, 5, DIR_SOUTH);
    Multiplayer_Update();
    // ObjectEventSetHeldMovement stub sets heldMovementActive = 1.
    ASSERT_NE(gObjectEvents[4].heldMovementActive, 0);
}

static void TestGhostTickNoMoveWhenAtTarget(void)
{
    // Ghost already at target: GhostTick should not request any movement.
    ResetAll();
    gTestNextSpawnSlot = 6;
    Multiplayer_SpawnGhostNPC(0, 1, 5, 5, DIR_SOUTH);
    gMultiplayerState.targetX = 5;
    gMultiplayerState.targetY = 5;
    SetPlayerMap(0, 1);
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    Multiplayer_UpdateGhostPosition(0, 1, 5, 5, DIR_SOUTH);
    Multiplayer_Update();
    ASSERT_EQ(gObjectEvents[6].heldMovementActive, 0);
}

// ---- Step 1.5: Cross-map ghost management ----------------------------------

static void TestGhostMapCheckSpawnsOnSameMap(void)
{
    ResetAll();
    SetPlayerMap(0, 5);
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    Multiplayer_UpdateGhostPosition(0, 5, 3, 4, DIR_SOUTH);
    gTestNextSpawnSlot = 9;
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, 9);
    ASSERT_NE(gObjectEvents[9].active, 0);
}

static void TestGhostMapCheckDespawnsOnDifferentMap(void)
{
    // Ghost is active; partner moves to a different map → ghost must despawn.
    ResetAll();
    gMultiplayerState.ghostObjectEventId = 5;
    gObjectEvents[5].active = 1;
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    SetPlayerMap(0, 5);
    Multiplayer_UpdateGhostPosition(0, 6, 3, 4, DIR_SOUTH); // partner on map 6
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, GHOST_INVALID_SLOT);
    ASSERT_EQ(gObjectEvents[5].active, 0);
}

static void TestGhostMapCheckDespawnsWhenDisconnected(void)
{
    ResetAll();
    gMultiplayerState.ghostObjectEventId = 2;
    gObjectEvents[2].active = 1;
    gMultiplayerState.connState = MP_STATE_DISCONNECTED;
    SetPlayerMap(0, 1);
    Multiplayer_UpdateGhostPosition(0, 1, 5, 5, DIR_SOUTH); // same map, but disconnected
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, GHOST_INVALID_SLOT);
    ASSERT_EQ(gObjectEvents[2].active, 0);
}

// ---- Step 3 stub (IsSyncableFlag) -----------------------------------------

static void TestIsSyncableFlagReturnsFalse(void)
{
    // Stub always returns FALSE until Phase 3 fills in the ranges.
    ASSERT_EQ(IsSyncableFlag(0x0000), FALSE);
    ASSERT_EQ(IsSyncableFlag(0xFFFF), FALSE);
}

// ---- Entry point -----------------------------------------------------------

int main(void)
{
    TestSpriteConstant();
    TestInit();
    TestSpawnNoFreeSlot();
    TestSpawnSuccess();
    TestSpawnReplacesPreviousGhost();
    TestDespawnClearsId();
    TestDespawnWhenAlreadyDespawned();
    TestUpdateGhostPosition();
    TestGhostTickMovesWhenOffTarget();
    TestGhostTickNoMoveWhenAtTarget();
    TestGhostMapCheckSpawnsOnSameMap();
    TestGhostMapCheckDespawnsOnDifferentMap();
    TestGhostMapCheckDespawnsWhenDisconnected();
    TestIsSyncableFlagReturnsFalse();
    TEST_SUMMARY();
}
