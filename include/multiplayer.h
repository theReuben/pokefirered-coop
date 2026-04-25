#ifndef GUARD_MULTIPLAYER_H
#define GUARD_MULTIPLAYER_H

#include "global.h"
#include "constants/multiplayer.h"

struct CoopSettings {
    u8  randomizeEncounters : 1;
    u8  padding : 7;
    u32 encounterSeed;
};

struct MultiplayerState {
    u8  role;           // MP_ROLE_*
    u8  connState;      // MP_STATE_*
    u8  partnerMapGroup;
    u8  partnerMapNum;
    u8  partnerX;
    u8  partnerY;
    u8  partnerFacing;
    u8  ghostObjectEventId; // 0xFF = not spawned
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
void Multiplayer_UpdateGhostPosition(u8 x, u8 y, u8 facing);

// Packet send helpers (stubs — implemented in Phase 2)
void Multiplayer_SendPosition(void);
void Multiplayer_SendFlagSet(u16 flagId);
void Multiplayer_SendVarSet(u16 varId, u16 value);
void Multiplayer_SendBossReady(u8 bossId);
void Multiplayer_SendBossCancel(void);

// Flag sync helpers (stubs — implemented in Phase 3)
bool32 IsSyncableFlag(u16 flagId);

#endif // GUARD_MULTIPLAYER_H
