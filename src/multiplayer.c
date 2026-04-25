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
    gMultiplayerState.ghostObjectEventId = OBJECT_EVENTS_COUNT;
    gMultiplayerState.bossReadyBossId    = 0;
    gMultiplayerState.isInScript         = FALSE;
    gCoopSettings.randomizeEncounters    = 1;
    gCoopSettings.encounterSeed          = 0;
}

void Multiplayer_Update(void)
{
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

    gObjectEvents[objId].mapGroup = mapGroup;
    gObjectEvents[objId].mapNum   = mapNum;
    SetObjectEventDirection(&gObjectEvents[objId], facing);
    gMultiplayerState.ghostObjectEventId = objId;
}

void Multiplayer_DespawnGhost(void)
{
    u8 objId = gMultiplayerState.ghostObjectEventId;

    if (objId < OBJECT_EVENTS_COUNT && gObjectEvents[objId].active)
        RemoveObjectEvent(&gObjectEvents[objId]);

    gMultiplayerState.ghostObjectEventId = OBJECT_EVENTS_COUNT;
}

void Multiplayer_UpdateGhostPosition(u8 x, u8 y, u8 facing)
{
    u8 objId = gMultiplayerState.ghostObjectEventId;
    struct ObjectEvent *ghost;

    if (objId >= OBJECT_EVENTS_COUNT || !gObjectEvents[objId].active)
        return;

    ghost = &gObjectEvents[objId];
    MoveObjectEventToMapCoords(ghost, x, y);
    SetObjectEventDirection(ghost, facing);
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
