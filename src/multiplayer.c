#include "global.h"
#include "multiplayer.h"
#include "constants/multiplayer.h"

struct MultiplayerState gMultiplayerState;
struct CoopSettings gCoopSettings;

void Multiplayer_Init(void)
{
    gMultiplayerState.role              = MP_ROLE_NONE;
    gMultiplayerState.connState         = MP_STATE_DISCONNECTED;
    gMultiplayerState.ghostObjectEventId = 0xFF;
    gMultiplayerState.bossReadyBossId   = 0;
    gMultiplayerState.isInScript        = FALSE;
    gCoopSettings.randomizeEncounters   = 1;
    gCoopSettings.encounterSeed         = 0;
}

void Multiplayer_Update(void)
{
    // Phase 2: process incoming serial packets and send outgoing ones.
}

void Multiplayer_SpawnGhostNPC(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing)
{
    (void)mapGroup; (void)mapNum; (void)x; (void)y; (void)facing;
    // Phase 1: spawn an ObjectEvent for the ghost NPC.
}

void Multiplayer_DespawnGhost(void)
{
    // Phase 1: remove the ghost ObjectEvent.
    gMultiplayerState.ghostObjectEventId = 0xFF;
}

void Multiplayer_UpdateGhostPosition(u8 x, u8 y, u8 facing)
{
    (void)x; (void)y; (void)facing;
    // Phase 1: move the ghost ObjectEvent to the new position.
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
