#include "global.h"
#include "multiplayer.h"
#include "constants/multiplayer.h"
#include "constants/event_object_movement.h"
#include "event_object_movement.h"

struct MultiplayerState gMultiplayerState;
struct CoopSettings gCoopSettings;

void Multiplayer_Init(void)
{
    gMultiplayerState.role               = MP_ROLE_NONE;
    gMultiplayerState.connState          = MP_STATE_DISCONNECTED;
    gMultiplayerState.partnerMapGroup    = 0xFF;
    gMultiplayerState.partnerMapNum      = 0xFF;
    gMultiplayerState.targetX            = 0;
    gMultiplayerState.targetY            = 0;
    gMultiplayerState.targetFacing       = DIR_SOUTH;
    gMultiplayerState.ghostObjectEventId = GHOST_INVALID_SLOT;
    gMultiplayerState.bossReadyBossId    = 0;
    gMultiplayerState.isInScript         = FALSE;
    gCoopSettings.randomizeEncounters    = 1;
    gCoopSettings.encounterSeed          = 0;

#if MP_DEBUG_TEST_GHOST
    // Force partner onto Route 1 so the ghost spawns immediately for testing.
    gMultiplayerState.connState       = MP_STATE_CONNECTED;
    gMultiplayerState.partnerMapGroup = MP_DEBUG_TEST_MAP_GROUP;
    gMultiplayerState.partnerMapNum   = MP_DEBUG_TEST_MAP_NUM;
    gMultiplayerState.targetX         = MP_DEBUG_TEST_X;
    gMultiplayerState.targetY         = MP_DEBUG_TEST_Y;
    gMultiplayerState.targetFacing    = DIR_SOUTH;
#endif
}

// Returns the MOVEMENT_ACTION_WALK_NORMAL_* constant for the step direction,
// or 0xFF if the ghost is already at the target.
static u8 GhostNextStepAction(const struct ObjectEvent *ghost)
{
    s16 dx = (s16)gMultiplayerState.targetX - ghost->currentCoords.x;
    s16 dy = (s16)gMultiplayerState.targetY - ghost->currentCoords.y;

    if (dx == 0 && dy == 0)
        return 0xFF; // at target

    // Prioritise horizontal movement when both axes differ to match normal walk feel.
    if (dx > 0)  return MOVEMENT_ACTION_WALK_NORMAL_RIGHT;
    if (dx < 0)  return MOVEMENT_ACTION_WALK_NORMAL_LEFT;
    if (dy > 0)  return MOVEMENT_ACTION_WALK_NORMAL_DOWN;
    return MOVEMENT_ACTION_WALK_NORMAL_UP;
}

// Steps the ghost one tile towards its target each frame.  Called from Multiplayer_Update.
static void GhostTick(void)
{
    u8 objId = gMultiplayerState.ghostObjectEventId;
    struct ObjectEvent *ghost;
    u8 action;

    if (objId >= OBJECT_EVENTS_COUNT || !gObjectEvents[objId].active)
        return;

    ghost = &gObjectEvents[objId];

    // Release the previous held movement if it has completed.
    ObjectEventClearHeldMovementIfFinished(ghost);

    // If a movement is still in flight, wait for it to finish.
    if (ghost->heldMovementActive)
        return;

    action = GhostNextStepAction(ghost);
    if (action == 0xFF)
    {
        // Ghost has arrived — update facing direction.
        SetObjectEventDirection(ghost, gMultiplayerState.targetFacing);
        return;
    }

    ObjectEventSetHeldMovement(ghost, action);
}

// Spawns or despawns the ghost based on whether the partner's map matches the
// player's current map.  Called once per frame from Multiplayer_Update.
static void GhostMapCheck(void)
{
    u8 playerMapGroup = (u8)gSaveBlock1Ptr->location.mapGroup;
    u8 playerMapNum   = (u8)gSaveBlock1Ptr->location.mapNum;
    bool32 sameMap    = (gMultiplayerState.partnerMapGroup == playerMapGroup
                      && gMultiplayerState.partnerMapNum   == playerMapNum);

    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
    {
        if (gMultiplayerState.ghostObjectEventId < OBJECT_EVENTS_COUNT)
            Multiplayer_DespawnGhost();
        return;
    }

    if (sameMap)
    {
        if (gMultiplayerState.ghostObjectEventId >= OBJECT_EVENTS_COUNT)
        {
            // Partner just arrived on this map — spawn ghost at the known position.
            Multiplayer_SpawnGhostNPC(
                gMultiplayerState.partnerMapGroup,
                gMultiplayerState.partnerMapNum,
                gMultiplayerState.targetX,
                gMultiplayerState.targetY,
                gMultiplayerState.targetFacing);
        }
    }
    else
    {
        if (gMultiplayerState.ghostObjectEventId < OBJECT_EVENTS_COUNT)
            Multiplayer_DespawnGhost();
    }
}

void Multiplayer_Update(void)
{
    GhostMapCheck();
    GhostTick();
    // Phase 2: process incoming serial packets and send outgoing ones.
}

void Multiplayer_SpawnGhostNPC(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing)
{
    u8 objId;

    if (gMultiplayerState.ghostObjectEventId < OBJECT_EVENTS_COUNT)
        Multiplayer_DespawnGhost();

    objId = SpawnSpecialObjectEventParameterized(
        OBJ_EVENT_GFX_PLAYER2,
        MOVEMENT_TYPE_NONE,
        GHOST_LOCAL_ID,
        x, y,
        GHOST_ELEVATION);

    if (objId >= OBJECT_EVENTS_COUNT)
        return; // no free slot

    gObjectEvents[objId].mapGroup = (u8)mapGroup;
    gObjectEvents[objId].mapNum   = (u8)mapNum;
    SetObjectEventDirection(&gObjectEvents[objId], facing);
    gMultiplayerState.ghostObjectEventId = objId;
}

void Multiplayer_DespawnGhost(void)
{
    u8 objId = gMultiplayerState.ghostObjectEventId;

    if (objId < OBJECT_EVENTS_COUNT && gObjectEvents[objId].active)
        RemoveObjectEvent(&gObjectEvents[objId]);

    gMultiplayerState.ghostObjectEventId = GHOST_INVALID_SLOT;
}

void Multiplayer_UpdateGhostPosition(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing)
{
    gMultiplayerState.partnerMapGroup = mapGroup;
    gMultiplayerState.partnerMapNum   = mapNum;
    gMultiplayerState.targetX         = x;
    gMultiplayerState.targetY         = y;
    gMultiplayerState.targetFacing    = facing;
    // GhostMapCheck() and GhostTick() in the next Multiplayer_Update() frame will
    // spawn/despawn/move the ghost as needed.
}

void Multiplayer_SendPosition(void)
{
    // Phase 2: encode and enqueue a MP_PKT_POSITION packet.
}

void Multiplayer_SendFlagSet(u16 flagId)
{
    (void)flagId;
    // Phase 3: encode and enqueue a MP_PKT_FLAG_SET packet.
}

void Multiplayer_SendVarSet(u16 varId, u16 value)
{
    (void)varId; (void)value;
    // Phase 3: encode and enqueue a MP_PKT_VAR_SET packet.
}

void Multiplayer_SendBossReady(u8 bossId)
{
    (void)bossId;
    // Phase 5: encode and enqueue a MP_PKT_BOSS_READY packet.
}

void Multiplayer_SendBossCancel(void)
{
    // Phase 5: encode and enqueue a MP_PKT_BOSS_CANCEL packet.
}

bool32 IsSyncableFlag(u16 flagId)
{
    (void)flagId;
    // Phase 3: return TRUE for flags in SYNC_FLAG_TRAINERS and SYNC_FLAG_STORY ranges.
    return FALSE;
}
