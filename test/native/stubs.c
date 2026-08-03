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
#include "main.h"   // for struct Main / gMain stub
#include <stdarg.h>

// Controllable Random32 return value — set in tests before calling
// Multiplayer_GenerateSeed().  Default is non-zero so stub never generates
// a zero seed in tests that don't care about the specific value.
u32 gTestRandom32Value = 0x11223344u;

// ---------------------------------------------------------------------------
// Structured RNG *Default stubs.  multiplayer.c's strong RandomUniform etc.
// fall through to these when not in a coop battle; the call counter lets
// tests assert which path was taken.
// ---------------------------------------------------------------------------
u32 gTestRandomDefaultCalls = 0;

u32 RandomUniformDefault(enum RandomTag tag, u32 lo, u32 hi)
{
    (void)tag; (void)hi;
    gTestRandomDefaultCalls++;
    return lo;
}

u32 RandomUniformExceptDefault(enum RandomTag tag, u32 lo, u32 hi, bool32 (*reject)(u32))
{
    (void)tag; (void)hi; (void)reject;
    gTestRandomDefaultCalls++;
    return lo;
}

u32 RandomWeightedArrayDefault(enum RandomTag tag, u32 sum, u32 n, const u16 *weights)
{
    (void)tag; (void)sum; (void)n; (void)weights;
    gTestRandomDefaultCalls++;
    return 0;
}

const void *RandomElementArrayDefault(enum RandomTag tag, const void *array, size_t size, size_t count)
{
    (void)tag; (void)size; (void)count;
    gTestRandomDefaultCalls++;
    return array;
}

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

void RemoveObjectEventByLocalIdAndMap(u8 localId, u8 mapNum, u8 mapGroup)
{
    (void)localId; (void)mapNum; (void)mapGroup;
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

// Real implementation sets the facing fields AND repaints the sprite animation;
// only the former is observable here.
void ObjectEventTurn(struct ObjectEvent *objectEvent, enum Direction direction)
{
    if (objectEvent)
        objectEvent->facingDirection = direction;
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

// Echo-suppression guard accessor (real impl in event_data.c, not linked here).
// The stub FlagSet/VarSet don't consult the guard, so this is a no-op.
void Multiplayer_SetRemoteUpdate(bool8 on)
{
    (void)on;
}

// FlagSet/FlagGet/FlagClear stubs — backed by the SaveBlock1 flags array.
void FlagSet(u16 flagId)
{
    if (gSaveBlock1Ptr && flagId < 280 * 8)
        gSaveBlock1Ptr->flags[flagId / 8] |= (1 << (flagId % 8));
}
bool8 FlagGet(u16 flagId)
{
    if (gSaveBlock1Ptr && flagId < 280 * 8)
        return (gSaveBlock1Ptr->flags[flagId / 8] >> (flagId % 8)) & 1;
    return FALSE;
}
void FlagClear(u16 flagId)
{
    if (gSaveBlock1Ptr && flagId < 280 * 8)
        gSaveBlock1Ptr->flags[flagId / 8] &= ~(1 << (flagId % 8));
}

// TrySavingData stub — native tests don't exercise real save; just no-op.
u8 TrySavingData(u8 saveType) { (void)saveType; return 0; }

// Incremental link-save primitive stubs.  Counters + a call-order log let the
// async-checkpoint test assert the state machine drives them in the right
// sequence (Init -> WriteSector*N -> ReplaceLastSector -> SetLastSectorSignature).
// gLinkSaveSectorsToWrite controls how many WriteSector calls precede "done".
int gLinkSaveInitCalls = 0;
int gLinkSaveWriteCalls = 0;
int gLinkSaveReplaceCalls = 0;
int gLinkSaveSigCalls = 0;
int gLinkSaveSectorsToWrite = 3;  // test default; real slot is NUM_SECTORS_PER_SLOT
char gLinkSaveOrder[64] = {0};    // appended initials: I W R S
static void LinkSaveLog(char c)
{
    int n = 0;
    while (n < 62 && gLinkSaveOrder[n]) n++;
    if (n < 63) { gLinkSaveOrder[n] = c; gLinkSaveOrder[n + 1] = 0; }
}
void LinkSave_ResetStubCounters(void)
{
    gLinkSaveInitCalls = gLinkSaveWriteCalls = 0;
    gLinkSaveReplaceCalls = gLinkSaveSigCalls = 0;
    gLinkSaveOrder[0] = 0;
}
bool8 LinkFullSave_Init(void) { gLinkSaveInitCalls++; LinkSaveLog('I'); return FALSE; }
bool8 LinkFullSave_WriteSector(void)
{
    gLinkSaveWriteCalls++; LinkSaveLog('W');
    return (gLinkSaveWriteCalls >= gLinkSaveSectorsToWrite) ? TRUE : FALSE;
}
bool8 LinkFullSave_ReplaceLastSector(void) { gLinkSaveReplaceCalls++; LinkSaveLog('R'); return FALSE; }
bool8 LinkFullSave_SetLastSectorSignature(void) { gLinkSaveSigCalls++; LinkSaveLog('S'); return FALSE; }

// VarGet/VarSet stub — temp/special vars (0x8000+) plus the saved game var
// range (0x4000–0x40FF) used by VAR_STARTER_MON / VAR_PARTNER_STARTER /
// VAR_RIVAL_STARTER.
static u16 sVars[16];
static u16 sGameVars[256];
u16 VarGet(u16 varId)
{
    u16 idx = varId - TEMP_VARS_START;
    if (idx < 16)
        return sVars[idx];
    if (varId >= 0x4000 && varId <= 0x40FF)
        return sGameVars[varId - 0x4000];
    return 0;
}
bool8 VarSet(u16 varId, u16 value)
{
    u16 idx = varId - TEMP_VARS_START;
    if (idx < 16) { sVars[idx] = value; return TRUE; }
    if (varId >= 0x4000 && varId <= 0x40FF)
    {
        sGameVars[varId - 0x4000] = value;
        return TRUE;
    }
    return FALSE;
}
// Test helper: wipe the saved game var range between cases.
void TestResetGameVars(void)
{
    memset(sGameVars, 0, sizeof(sGameVars));
}

// Script special-var used by Multiplayer_ClaimStarter (ball slot argument).
u16 gSpecialVar_0x8004;

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

// Saved-party stash, mirroring load_save.c semantics: a PARTY_SIZE snapshot
// separate from the live gPlayerParty.  Backs the coop stash/restore tests.
struct Pokemon gSavedPlayerParty[PARTY_SIZE];
static u8 sSavedPlayerPartyCount;

void SavePlayerPartyMon(u32 index, struct Pokemon *mon)
{
    if (index < PARTY_SIZE)
        gSavedPlayerParty[index] = *mon;
}

struct Pokemon *GetSavedPlayerPartyMon(u32 index)
{
    return &gSavedPlayerParty[index % PARTY_SIZE];
}

u8 *GetSavedPlayerPartyCount(void)
{
    return &sSavedPlayerPartyCount;
}

void SavePlayerParty(void)
{
    int i;
    sSavedPlayerPartyCount = gPlayerPartyCount;
    for (i = 0; i < PARTY_SIZE; i++)
        gSavedPlayerParty[i] = gPlayerParty[i];
}

void LoadPlayerParty(void)
{
    int i;
    gPlayerPartyCount = sSavedPlayerPartyCount;
    for (i = 0; i < PARTY_SIZE; i++)
        gPlayerParty[i] = gSavedPlayerParty[i];
}

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
struct Main gMain; // zeroed; tests set inBattle manually when needed

// Observable recorders so tests can assert the coop party-select cancel guard
// re-opens the menu instead of proceeding into battle with an empty party.
u32 gStubReopenCount = 0;
MainCallback gStubLastCallback = NULL;

void InitChooseHalfPartyForBattle(u8 unused) { (void)unused; gStubReopenCount++; }
void SetMainCallback2(MainCallback callback) { gStubLastCallback = callback; }
void CB2_ReturnToFieldContinueScript(void) {}

const struct UCoords32 gDirectionToVectors[4]; // zeroed; tests don't call movement code

bool8 PlayerHasFollowerNPC(void) { return FALSE; }
u32   GetFollowerNPCData(enum FollowerNpcData data) { (void)data; return 0; }
struct ObjectEvent *GetFollowerObject(void) { return NULL; }

// Records the last Multiplayer_PlayGhostTrainerApproach() invocation so the
// MP_PKT_TRAINER_APPROACH recv-dispatch test can assert the cosmetic mirror was
// triggered with the decoded args. The real implementation lives in
// trainer_see.c, which is not compiled into the native suite.
u8 gGhostApproachCalls;
u8 gGhostApproachLocalId;
u8 gGhostApproachDir;
u8 gGhostApproachDist;
void Multiplayer_PlayGhostTrainerApproach(u8 localId, u8 direction, u8 distance)
{
    gGhostApproachCalls++;
    gGhostApproachLocalId  = localId;
    gGhostApproachDir      = direction;
    gGhostApproachDist     = distance;
}
