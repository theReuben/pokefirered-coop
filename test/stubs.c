// No-op stubs for GBA engine functions called by multiplayer.c.
// These exist so the native unit tests link cleanly without the full ROM build.
#include "global.h"
#include "multiplayer.h"
#include "event_object_movement.h"
#include "item.h"
#include "random.h"
#include "pokemon.h"
#include "battle_main.h"
#include "battle.h"
#include "party_menu.h"
#include "overworld.h"
#include "follower_npc.h"
#include <stdarg.h>

// Controllable Random32 return value — set in tests before calling
// Multiplayer_GenerateSeed().  Default is non-zero so stub never generates
// a zero seed in tests that don't care about the specific value.
u32 gTestRandom32Value = 0x11223344u;

// Global arrays / pointers referenced by multiplayer.c
struct ObjectEvent gObjectEvents[16];
struct SaveBlock1 *gSaveBlock1Ptr;
struct SaveBlock2 sTestSaveBlock2;
struct SaveBlock2 *gSaveBlock2Ptr = &sTestSaveBlock2;
struct PlayerAvatar gPlayerAvatar;

// Set this to a valid slot (0-15) before a test that exercises spawn; 16 = no slot.
u8 gTestNextSpawnSlot = 16;

// Object event stubs
u8 SpawnSpecialObjectEventParameterized(u16 graphicsId, u8 movementBehavior,
                                         u8 localId, s16 x, s16 y, u8 elevation)
{
    (void)graphicsId; (void)movementBehavior; (void)localId; (void)elevation;
    u8 slot = gTestNextSpawnSlot;
    if (slot < 16)
    {
        gObjectEvents[slot].active = 1;
        gObjectEvents[slot].currentCoords.x = x;
        gObjectEvents[slot].currentCoords.y = y;
    }
    return slot;
}

void RemoveObjectEvent(struct ObjectEvent *objectEvent)
{
    if (objectEvent)
        objectEvent->active = 0;
}

void MoveObjectEventToMapCoords(struct ObjectEvent *objectEvent, s16 x, s16 y)
{
    if (objectEvent)
    {
        objectEvent->currentCoords.x = x;
        objectEvent->currentCoords.y = y;
    }
}

void SetObjectEventDirection(struct ObjectEvent *objectEvent, enum Direction direction)
{
    (void)objectEvent; (void)direction;
}

u8 ObjectEventClearHeldMovementIfFinished(struct ObjectEvent *objectEvent)
{
    if (objectEvent && objectEvent->heldMovementFinished)
    {
        objectEvent->heldMovementActive   = 0;
        objectEvent->heldMovementFinished = 0;
        return 1;
    }
    return 0;
}

bool8 ObjectEventSetHeldMovement(struct ObjectEvent *objectEvent, u8 movementAction)
{
    (void)movementAction;
    if (objectEvent)
        objectEvent->heldMovementActive = 1;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Stubs for event_data.c functions (not compiled in test builds).
// These let test_smoke and test_packets link without the full ROM source.
// ---------------------------------------------------------------------------

// Counters exposed so test_smoke.c can verify correct dispatch.
u8  gTestRemoteFlagSetCallCount = 0;
u16 gTestLastRemoteFlagId       = 0;
u8  gTestRemoteVarSetCallCount  = 0;
u16 gTestLastRemoteVarId        = 0;
u16 gTestLastRemoteVarValue     = 0;

void Multiplayer_HandleRemoteFlagSet(u16 flagId)
{
    gTestRemoteFlagSetCallCount++;
    gTestLastRemoteFlagId = flagId;
}

void Multiplayer_HandleRemoteFlagClear(u16 flagId)
{
    (void)flagId;
}

void Multiplayer_HandleRemoteVarSet(u16 varId, u16 value)
{
    gTestRemoteVarSetCallCount++;
    gTestLastRemoteVarId    = varId;
    gTestLastRemoteVarValue = value;
}

// VarGet/VarSet stub — backed by a small array indexed from TEMP_VARS_START.
static u16 sVars[16];
u16 VarGet(u16 varId)
{
    u16 idx = varId - TEMP_VARS_START;
    return (idx < 16) ? sVars[idx] : 0;
}
bool8 VarSet(u16 varId, u16 value)
{
    u16 idx = varId - TEMP_VARS_START;
    if (idx < 16) { sVars[idx] = value; return TRUE; }
    return FALSE;
}

// AddBagItem stub — records the last call for test assertions.
u16 gTestLastAddBagItemId    = 0;
u16 gTestLastAddBagItemCount = 0;

bool32 AddBagItem(u16 itemId, u16 count)
{
    gTestLastAddBagItemId    = itemId;
    gTestLastAddBagItemCount = count;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Stubs for link.c / task.c globals referenced by the coop-battle block relay.
// These exist only so the native unit tests link cleanly; the tests don't
// exercise the relay task path.
// ---------------------------------------------------------------------------
struct LinkPlayer { u16 version; u16 lp_field_2; u32 trainerId; u8 name[11]; u8 gender; u8 language; u32 linkType; u16 id; u16 trainerCode; };
struct LinkPlayer gLinkPlayers[5];
bool8 gReceivedRemoteLinkPlayers;
u8 gBlockReceivedStatus[5];
u16 gBlockRecvBuffer[5][0x100 / 2];

u8 CreateTask(void (*func)(u8), u8 priority)
{
    (void)func; (void)priority;
    return 0;
}

// ---------------------------------------------------------------------------
// Pokemon / battle stubs — native build only; not exercised by current tests.
// ---------------------------------------------------------------------------
struct Pokemon gPlayerParty[PARTY_SIZE * 2];
u8 gPlayerPartyCount;
struct MultiPartnerMenuPokemon gMultiPartnerParty[MULTI_PARTY_SIZE];
u32 gBattleTypeFlags;

u32 GetMonData_stub(struct Pokemon *mon, s32 field, ...)
{
    (void)mon; (void)field;
    va_list ap; va_start(ap, field); va_end(ap);
    return 0;
}

u8 GetMonGender(struct Pokemon *mon) { (void)mon; return 0; }

void SetMonData(struct Pokemon *mon, s32 field, const void *data)
{
    (void)mon; (void)field; (void)data;
}

void CreateMon(struct Pokemon *mon, u16 species, u8 level, u32 personality,
               struct OriginalTrainerId otId)
{
    (void)mon; (void)species; (void)level; (void)personality; (void)otId;
}

// ---------------------------------------------------------------------------
// Party menu / overworld / follower stubs.
// ---------------------------------------------------------------------------
u8 gSelectedOrderFromParty[MAX_FRONTIER_PARTY_SIZE];

void InitChooseHalfPartyForBattle(u8 unused) { (void)unused; }
void SetMainCallback2(MainCallback callback) { (void)callback; }
void CB2_ReturnToFieldContinueScript(void) {}

const struct UCoords32 gDirectionToVectors[4]; // zeroed; tests don't call movement code

bool8 PlayerHasFollowerNPC(void) { return FALSE; }
u32   GetFollowerNPCData(enum FollowerNpcData data) { (void)data; return 0; }
