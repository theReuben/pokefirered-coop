// Native unit tests for curated story-milestone var sync (Change 2) and the
// milestone-driven checkpoint save (Change 1.2).
//
// Covers the parcel milestone (VAR_MAP_SCENE_VIRIDIAN_CITY_MART -> 1, alias
// FLAG_COOP_GOT_PARCEL): the syncable-var predicate, the exact-write send gate,
// forward-only + idempotent apply, the completeFlag, the save request, the
// join/loss catch-up replay, and the regression that a plain map change no
// longer kicks a per-warp checkpoint save.

#include "test_runner.h"
#include "global.h"
#include "multiplayer.h"
#include "constants/multiplayer.h"
#include "constants/vars.h"
#include "main.h"
#include <string.h>

// MP_SAVE_INIT is a file-local enum in multiplayer.c; its value is 1 (the state
// Multiplayer_RequestCheckpointSave sets from IDLE=0). Mirror it here for asserts.
#define SAVE_IDLE 0
#define SAVE_INIT 1

// A dual-use scene var that must NOT be syncable (the starter/rival lab scenes).
#define VAR_LAB VAR_MAP_SCENE_PALLET_TOWN_PROFESSOR_OAKS_LAB

static struct SaveBlock1 sTestSave;

static void SetUp(void)
{
    Multiplayer_Init();
    memset(gObjectEvents, 0, sizeof(gObjectEvents));
    memset(&sTestSave, 0, sizeof(sTestSave));
    sTestSave.location.mapGroup = 0;
    sTestSave.location.mapNum   = 1;
    gSaveBlock1Ptr              = &sTestSave;
    gPlayerAvatar.objectEventId = 0;
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    gMultiplayerState.gotPartnerGender = TRUE;
    // Clean var/flag state (stub-backed).
    VarSet(VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 0);
    VarSet(VAR_LAB, 0);
}

// ---- Predicates ------------------------------------------------------------

static void TestIsSyncableVar(void)
{
    ASSERT(IsSyncableVar(VAR_MAP_SCENE_VIRIDIAN_CITY_MART));  // the parcel milestone var
    ASSERT(!IsSyncableVar(VAR_LAB));                          // dual-use lab var: never
    ASSERT(!IsSyncableVar(VAR_MAP_SCENE_VIRIDIAN_CITY_MART + 1));
}

static void TestIsMilestoneWrite(void)
{
    ASSERT(Multiplayer_IsMilestoneWrite(VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 1));   // exact milestone
    ASSERT(!Multiplayer_IsMilestoneWrite(VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 2));  // wrong value
    ASSERT(!Multiplayer_IsMilestoneWrite(VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 0));  // not a milestone
    ASSERT(!Multiplayer_IsMilestoneWrite(VAR_LAB, 5));                           // not a milestone var
}

// ---- Apply -----------------------------------------------------------------

static void TestApplyAdvancesForwardAndSetsFlagAndSaves(void)
{
    SetUp();
    gMultiplayerState.saveState = SAVE_IDLE;

    Multiplayer_ApplyMilestoneVar(VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 1);

    ASSERT_EQ(VarGet(VAR_MAP_SCENE_VIRIDIAN_CITY_MART), 1);       // var advanced
    ASSERT(FlagGet(FLAG_COOP_GOT_PARCEL));                        // completeFlag set
    ASSERT_EQ(gMultiplayerState.saveState, SAVE_INIT);           // checkpoint requested
}

static void TestApplyIsForwardOnly(void)
{
    SetUp();
    VarSet(VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 5);                 // partner already ahead
    gMultiplayerState.saveState = SAVE_IDLE;

    Multiplayer_ApplyMilestoneVar(VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 1);

    ASSERT_EQ(VarGet(VAR_MAP_SCENE_VIRIDIAN_CITY_MART), 5);       // not regressed
    ASSERT_EQ(gMultiplayerState.saveState, SAVE_IDLE);          // no-op: no save
}

static void TestApplyIgnoresNonMilestone(void)
{
    SetUp();
    gMultiplayerState.saveState = SAVE_IDLE;

    Multiplayer_ApplyMilestoneVar(VAR_LAB, 5);                   // dual-use var: not a milestone

    ASSERT_EQ(VarGet(VAR_LAB), 0);                               // untouched
    ASSERT_EQ(gMultiplayerState.saveState, SAVE_IDLE);          // no save
}

static void TestOnLocalMilestoneSetsFlagAndSaves(void)
{
    // Sender side: reaching a milestone locally must record it durably even
    // though the local var was written by a script (not via apply).
    SetUp();
    gMultiplayerState.saveState = SAVE_IDLE;

    Multiplayer_OnLocalMilestone(VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 1);

    ASSERT(FlagGet(FLAG_COOP_GOT_PARCEL));                       // durable flag set on sender
    ASSERT_EQ(gMultiplayerState.saveState, SAVE_INIT);          // checkpoint requested
}

static void TestOnLocalMilestoneIgnoresNonMilestone(void)
{
    SetUp();
    gMultiplayerState.saveState = SAVE_IDLE;
    Multiplayer_OnLocalMilestone(VAR_LAB, 5);                    // not a milestone
    ASSERT_EQ(gMultiplayerState.saveState, SAVE_IDLE);          // no save
}

// ---- Catch-up replay -------------------------------------------------------

static void TestCatchupReplaysReachedMilestone(void)
{
    u8 b;
    SetUp();
    VarSet(VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 1);                 // milestone reached locally
    while (Mp_Pop(&gMpSendRing, &b)) {}                          // drain

    Multiplayer_SendMilestoneCatchup();

    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)MP_PKT_SIZE_VAR_SET);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, MP_PKT_VAR_SET);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, (u8)(VAR_MAP_SCENE_VIRIDIAN_CITY_MART >> 8));
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, (u8)(VAR_MAP_SCENE_VIRIDIAN_CITY_MART & 0xFF));
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 0);                   // value hi
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 1);                   // value lo
}

static void TestCatchupSilentWhenNotReached(void)
{
    u8 b;
    SetUp();
    VarSet(VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 0);                 // milestone NOT reached
    while (Mp_Pop(&gMpSendRing, &b)) {}

    Multiplayer_SendMilestoneCatchup();

    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);                    // nothing sent
}

// ---- Regression: no per-warp checkpoint save -------------------------------

static void TestMapChangeDoesNotSave(void)
{
    SetUp();
    gMultiplayerState.saveState        = SAVE_IDLE;
    gMultiplayerState.lastCkptMapNum   = 1;    // matches current: no change yet
    gMultiplayerState.lastCkptMapGroup = 0;

    // Cross a map boundary (mapNum 1 -> 2) and pump one frame.
    sTestSave.location.mapNum = 2;
    Multiplayer_Update();

    // The per-warp save trigger was removed: crossing a boundary must NOT kick a
    // checkpoint. (Durability now rides battle-end / milestone / periodic saves.)
    ASSERT_EQ(gMultiplayerState.saveState, SAVE_IDLE);
    // ...but the map-entry detection still ran (feeds the reconnect event log).
    ASSERT_EQ(gMultiplayerState.lastCkptMapNum, 2);
}

int main(void)
{
    TestIsSyncableVar();
    TestIsMilestoneWrite();
    TestApplyAdvancesForwardAndSetsFlagAndSaves();
    TestApplyIsForwardOnly();
    TestApplyIgnoresNonMilestone();
    TestOnLocalMilestoneSetsFlagAndSaves();
    TestOnLocalMilestoneIgnoresNonMilestone();
    TestCatchupReplaysReachedMilestone();
    TestCatchupSilentWhenNotReached();
    TestMapChangeDoesNotSave();
    TEST_SUMMARY();
}
