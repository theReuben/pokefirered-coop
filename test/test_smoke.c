#include "test_runner.h"
#include "global.h"
#include "multiplayer.h"
#include "constants/event_objects.h"
#include <string.h>

// Declared in stubs.c so tests can control spawn behaviour.
extern u8 gTestNextSpawnSlot;

// Stub counters for remote handler dispatch tests.
extern u8  gTestRemoteFlagSetCallCount;
extern u16 gTestLastRemoteFlagId;
extern u8  gTestRemoteVarSetCallCount;
extern u16 gTestLastRemoteVarId;
extern u16 gTestLastRemoteVarValue;

// Controllable Random32 return value for Multiplayer_GenerateSeed tests.
extern u32 gTestRandom32Value;

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

// ---- Step 3.1: IsSyncableFlag ---------------------------------------------

static void TestIsSyncableFlag(void)
{
    // Temp flags (0x000-0x01F): NOT syncable.
    ASSERT_EQ(IsSyncableFlag(0x0000), FALSE);
    ASSERT_EQ(IsSyncableFlag(0x001F), FALSE);

    // Story range (0x020-0x2FF): syncable.
    ASSERT_EQ(IsSyncableFlag(0x020),  TRUE);
    ASSERT_EQ(IsSyncableFlag(0x028),  TRUE);  // FLAG_HIDE_BULBASAUR_BALL
    ASSERT_EQ(IsSyncableFlag(0x230),  TRUE);  // STORY_FLAGS_START
    ASSERT_EQ(IsSyncableFlag(0x2FF),  TRUE);  // last story flag

    // Gap between story and items: NOT syncable.
    ASSERT_EQ(IsSyncableFlag(0x300),  FALSE);
    ASSERT_EQ(IsSyncableFlag(0x3E7),  FALSE);

    // Hidden items (0x3E8-0x4A6): syncable.
    ASSERT_EQ(IsSyncableFlag(0x3E8),  TRUE);  // FLAG_HIDDEN_ITEMS_START
    ASSERT_EQ(IsSyncableFlag(0x4A6),  TRUE);  // last hidden item
    ASSERT_EQ(IsSyncableFlag(0x4A7),  FALSE); // just past range

    // Boss clear flags (0x4B0-0x4BC): syncable.
    ASSERT_EQ(IsSyncableFlag(0x4B0),  TRUE);  // FLAG_DEFEATED_BROCK
    ASSERT_EQ(IsSyncableFlag(0x4BC),  TRUE);  // FLAG_DEFEATED_CHAMP
    ASSERT_EQ(IsSyncableFlag(0x4BD),  FALSE); // unused past bosses

    // Trainer flags (0x500-0x7FF): syncable.
    ASSERT_EQ(IsSyncableFlag(0x500),  TRUE);  // TRAINER_FLAGS_START
    ASSERT_EQ(IsSyncableFlag(0x7FF),  TRUE);  // TRAINER_FLAGS_END

    // SYS_FLAGS (0x800+): NOT syncable.
    ASSERT_EQ(IsSyncableFlag(0x800),  FALSE);
    ASSERT_EQ(IsSyncableFlag(0xFFFF), FALSE);
}

// ---- Step 3.2: FLAG_SET / VAR_SET recv routing ----------------------------

// Helper: reset rings and dispatch counters.
static void ResetDispatch(void)
{
    gMpSendRing.head = gMpSendRing.tail = 0;
    gMpRecvRing.head = gMpRecvRing.tail = 0;
    gMpSendRing.magic = MP_RING_MAGIC;
    gMpRecvRing.magic = MP_RING_MAGIC;
    gTestRemoteFlagSetCallCount = 0;
    gTestLastRemoteFlagId       = 0;
    gTestRemoteVarSetCallCount  = 0;
    gTestLastRemoteVarId        = 0;
    gTestLastRemoteVarValue     = 0;
}

static void TestRemoteFlagSetRouting(void)
{
    // Push a FLAG_SET packet into the recv ring; Multiplayer_Update should
    // dispatch it to Multiplayer_HandleRemoteFlagSet exactly once.
    u8 pkt[MP_PKT_SIZE_FLAG_SET];
    u8 i;

    Multiplayer_Init();
    ResetDispatch();
    // connState stays DISCONNECTED — no position packet will pollute send ring.

    Mp_EncodeFlagSet(pkt, SYNC_FLAG_TRAINERS_START + 5);
    for (i = 0; i < MP_PKT_SIZE_FLAG_SET; i++)
        Mp_Push(&gMpRecvRing, pkt[i]);

    Multiplayer_Update();

    ASSERT_EQ(gTestRemoteFlagSetCallCount, 1);
    ASSERT_EQ(gTestLastRemoteFlagId, SYNC_FLAG_TRAINERS_START + 5);
    // Send ring must be empty — no re-broadcast.
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

static void TestRemoteVarSetRouting(void)
{
    u8 pkt[MP_PKT_SIZE_VAR_SET];
    u8 i;

    Multiplayer_Init();
    ResetDispatch();

    Mp_EncodeVarSet(pkt, 0x4001, 0x0007);
    for (i = 0; i < MP_PKT_SIZE_VAR_SET; i++)
        Mp_Push(&gMpRecvRing, pkt[i]);

    Multiplayer_Update();

    ASSERT_EQ(gTestRemoteVarSetCallCount, 1);
    ASSERT_EQ(gTestLastRemoteVarId,    0x4001);
    ASSERT_EQ(gTestLastRemoteVarValue, 0x0007);
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

static void TestMultipleFlagSetsRouted(void)
{
    // Two FLAG_SET packets in the recv ring; both should be dispatched.
    u8 pkt[MP_PKT_SIZE_FLAG_SET];
    u8 i;

    Multiplayer_Init();
    ResetDispatch();

    Mp_EncodeFlagSet(pkt, SYNC_FLAG_BOSSES_START);
    for (i = 0; i < MP_PKT_SIZE_FLAG_SET; i++) Mp_Push(&gMpRecvRing, pkt[i]);

    Mp_EncodeFlagSet(pkt, SYNC_FLAG_BOSSES_START + 1);
    for (i = 0; i < MP_PKT_SIZE_FLAG_SET; i++) Mp_Push(&gMpRecvRing, pkt[i]);

    Multiplayer_Update();

    // Both dispatched; last call carries the second flag.
    ASSERT_EQ(gTestRemoteFlagSetCallCount, 2);
    ASSERT_EQ(gTestLastRemoteFlagId, SYNC_FLAG_BOSSES_START + 1);
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

// ---- Step 3.3: Full sync build and apply ----------------------------------

static struct SaveBlock1 sSyncSave;

static void TestFullSyncSendBuildsPacket(void)
{
    // Set a trainer flag and a story flag, then call SendFullSync.
    // The send ring should contain a FULL_SYNC packet with those bits set.
    u8 typeByte, lenHi, lenLo;
    u16 dataLen;
    u8 payload[FULL_SYNC_PAYLOAD_SIZE];
    u16 i;

    Multiplayer_Init();
    ResetDispatch();
    memset(&sSyncSave, 0, sizeof(sSyncSave));
    gSaveBlock1Ptr = &sSyncSave;

    // Set flag 0x500 (first trainer flag) — byte 160, bit 0
    sSyncSave.flags[SYNC_FLAG_TRAINERS_START / 8] |= (1 << (SYNC_FLAG_TRAINERS_START & 7));
    // Set flag 0x020 (first story flag) — byte 4, bit 0
    sSyncSave.flags[SYNC_FLAG_STORY_START / 8] |= (1 << (SYNC_FLAG_STORY_START & 7));

    Multiplayer_SendFullSync();

    // Recv ring is still empty; send ring has the packet.
    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)(MP_PKT_SIZE_FULL_SYNC_HDR + FULL_SYNC_PAYLOAD_SIZE));

    // Parse: type byte
    Mp_Pop(&gMpSendRing, &typeByte);
    ASSERT_EQ(typeByte, MP_PKT_FULL_SYNC);

    // Length
    Mp_Pop(&gMpSendRing, &lenHi);
    Mp_Pop(&gMpSendRing, &lenLo);
    dataLen = ((u16)lenHi << 8) | lenLo;
    ASSERT_EQ(dataLen, (u16)FULL_SYNC_PAYLOAD_SIZE);

    // Drain payload
    for (i = 0; i < dataLen; i++)
        Mp_Pop(&gMpSendRing, &payload[i]);

    // Story flag byte 4 should have bit 0 set.
    ASSERT_NE(payload[0], 0); // story offset 0 = flags byte 4

    // Trainer flag byte 160 is at payload offset FULL_SYNC_STORY_LEN+FULL_SYNC_ITEMS_LEN+FULL_SYNC_BOSSES_LEN
    ASSERT_NE(payload[FULL_SYNC_STORY_LEN + FULL_SYNC_ITEMS_LEN + FULL_SYNC_BOSSES_LEN], 0);
}

static void TestFullSyncApplyORsFlags(void)
{
    // Build a payload with a trainer flag set; apply it to a save block
    // that already has a story flag set.  Both flags should remain set.
    u8 payload[FULL_SYNC_PAYLOAD_SIZE];
    u16 trainerPayloadOffset;

    memset(payload, 0, sizeof(payload));
    memset(&sSyncSave, 0, sizeof(sSyncSave));
    gSaveBlock1Ptr = &sSyncSave;

    // Pre-set story flag 0x020 in sSyncSave.
    sSyncSave.flags[SYNC_FLAG_STORY_START / 8] |= (1 << (SYNC_FLAG_STORY_START & 7));

    // Payload has trainer flag 0x500 set (at payload byte FULL_SYNC_STORY_LEN+FULL_SYNC_ITEMS_LEN+FULL_SYNC_BOSSES_LEN).
    trainerPayloadOffset = FULL_SYNC_STORY_LEN + FULL_SYNC_ITEMS_LEN + FULL_SYNC_BOSSES_LEN;
    payload[trainerPayloadOffset] |= (1 << (SYNC_FLAG_TRAINERS_START & 7));

    Multiplayer_ApplyFullSync(payload, FULL_SYNC_PAYLOAD_SIZE);

    // Story flag should still be set (was pre-set, payload was 0 for that byte).
    ASSERT_NE(sSyncSave.flags[SYNC_FLAG_STORY_START / 8] & (1 << (SYNC_FLAG_STORY_START & 7)), 0);
    // Trainer flag should now be set (ORed in from payload).
    ASSERT_NE(sSyncSave.flags[SYNC_FLAG_TRAINERS_START / 8] & (1 << (SYNC_FLAG_TRAINERS_START & 7)), 0);
}

static void TestFullSyncApplyRejectsWrongLength(void)
{
    u8 payload[FULL_SYNC_PAYLOAD_SIZE + 1];
    memset(payload, 0xFF, sizeof(payload));
    memset(&sSyncSave, 0, sizeof(sSyncSave));
    gSaveBlock1Ptr = &sSyncSave;

    // Wrong length — should be a no-op.
    Multiplayer_ApplyFullSync(payload, FULL_SYNC_PAYLOAD_SIZE + 1);

    // All flags should remain 0.
    ASSERT_EQ(sSyncSave.flags[SYNC_FLAG_TRAINERS_START / 8], 0);
}

static void TestFullSyncRoundTrip(void)
{
    // Send → recv → apply: story flag 0x030 should appear on the other side.
    struct SaveBlock1 applyBlock;
    u8 b;
    u16 i, pktLen;

    memset(&sSyncSave, 0, sizeof(sSyncSave));
    gSaveBlock1Ptr = &sSyncSave;

    // Set flag 0x030 on the sender side.
    sSyncSave.flags[0x030 / 8] |= (1 << (0x030 & 7));

    Multiplayer_Init();
    ResetDispatch();
    Multiplayer_SendFullSync();

    pktLen = (u16)Mp_Available(&gMpSendRing);
    ASSERT_EQ(pktLen, (u16)(MP_PKT_SIZE_FULL_SYNC_HDR + FULL_SYNC_PAYLOAD_SIZE));

    // Copy send ring bytes into recv ring.
    for (i = 0; i < pktLen; i++)
    {
        Mp_Pop(&gMpSendRing, &b);
        Mp_Push(&gMpRecvRing, b);
    }

    // Switch gSaveBlock1Ptr to a clean block to simulate the receiver.
    memset(&applyBlock, 0, sizeof(applyBlock));
    gSaveBlock1Ptr = &applyBlock;

    Multiplayer_Update(); // dispatches FULL_SYNC via ProcessOneRecvPacket

    ASSERT_NE(applyBlock.flags[0x030 / 8] & (1 << (0x030 & 7)), 0);
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0); // no re-broadcast
}

// ---- Step 3.4: Script mutex state machine ---------------------------------

static void TestScriptStartSetsFlag(void)
{
    Multiplayer_Init();
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    ResetDispatch();

    ASSERT_EQ(gMultiplayerState.isInScript, FALSE);
    Multiplayer_OnScriptStart();
    ASSERT_EQ(gMultiplayerState.isInScript, TRUE);
    // SCRIPT_LOCK packet should be in the send ring.
    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)MP_PKT_SIZE_SCRIPT_LOCK);
    { u8 b; Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, MP_PKT_SCRIPT_LOCK); }
}

static void TestScriptEndClearsFlag(void)
{
    Multiplayer_Init();
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    gMultiplayerState.isInScript = TRUE;
    ResetDispatch();

    Multiplayer_OnScriptEnd();
    ASSERT_EQ(gMultiplayerState.isInScript, FALSE);
    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)MP_PKT_SIZE_SCRIPT_UNLOCK);
    { u8 b; Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, MP_PKT_SCRIPT_UNLOCK); }
}

static void TestScriptLockNoDuplicateSend(void)
{
    // Calling OnScriptStart twice should only send one packet.
    Multiplayer_Init();
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    ResetDispatch();

    Multiplayer_OnScriptStart();
    Multiplayer_OnScriptStart(); // second call — no-op
    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)MP_PKT_SIZE_SCRIPT_LOCK);
}

static void TestScriptLockUnlockNoDuplicateUnlock(void)
{
    // OnScriptEnd when not in script should not send.
    Multiplayer_Init();
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    gMultiplayerState.isInScript = FALSE;
    ResetDispatch();

    Multiplayer_OnScriptEnd();
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

static void TestScriptLockDisconnectedNoSend(void)
{
    // When disconnected, SCRIPT_LOCK should still set the flag but not send.
    Multiplayer_Init();
    // connState stays DISCONNECTED
    ResetDispatch();

    Multiplayer_OnScriptStart();
    ASSERT_EQ(gMultiplayerState.isInScript, TRUE);
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

static void TestPartnerScriptLockRecv(void)
{
    // Receiving SCRIPT_LOCK sets partnerIsInScript.
    u8 pkt = MP_PKT_SCRIPT_LOCK;
    Multiplayer_Init();
    ResetDispatch();
    ASSERT_EQ(gMultiplayerState.partnerIsInScript, FALSE);

    Mp_Push(&gMpRecvRing, pkt);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.partnerIsInScript, TRUE);
    ASSERT_EQ(Multiplayer_IsPartnerInScript(), TRUE);
}

static void TestPartnerScriptUnlockRecv(void)
{
    u8 pkt = MP_PKT_SCRIPT_UNLOCK;
    Multiplayer_Init();
    gMultiplayerState.partnerIsInScript = TRUE;
    ResetDispatch();

    Mp_Push(&gMpRecvRing, pkt);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.partnerIsInScript, FALSE);
    ASSERT_EQ(Multiplayer_IsPartnerInScript(), FALSE);
}

static void TestGhostFreezesDuringPartnerScript(void)
{
    // Ghost should not step towards its target while partner is in a script.
    ResetAll();
    gTestNextSpawnSlot = 4;
    Multiplayer_SpawnGhostNPC(0, 1, 5, 5, DIR_SOUTH);
    gMultiplayerState.targetX = 8; // off-target so GhostTick would normally move
    gMultiplayerState.targetY = 5;
    SetPlayerMap(0, 1);
    gMultiplayerState.connState          = MP_STATE_CONNECTED;
    gMultiplayerState.partnerIsInScript  = TRUE;
    Multiplayer_UpdateGhostPosition(0, 1, 8, 5, DIR_SOUTH);

    Multiplayer_Update();

    // heldMovementActive must stay 0 — ghost should NOT have been asked to move.
    ASSERT_EQ(gObjectEvents[4].heldMovementActive, 0);
}

// ---- Step 4.2: Seeded PRNG --------------------------------------------------

static void TestRngSameSeedSameSequence(void)
{
    u32 seq1[5], seq2[5];
    int i;

    Multiplayer_SeedRng(0xDEADBEEFu);
    for (i = 0; i < 5; i++)
        seq1[i] = Multiplayer_NextRandom();

    Multiplayer_SeedRng(0xDEADBEEFu);
    for (i = 0; i < 5; i++)
        seq2[i] = Multiplayer_NextRandom();

    for (i = 0; i < 5; i++)
        ASSERT_EQ(seq1[i], seq2[i]);
}

static void TestRngDifferentSeedsDifferentSequences(void)
{
    u32 a, b;
    Multiplayer_SeedRng(0x00000001u);
    a = Multiplayer_NextRandom();
    Multiplayer_SeedRng(0x00000002u);
    b = Multiplayer_NextRandom();
    ASSERT(a != b);
}

static void TestRngZeroSeedNonZero(void)
{
    // Seeding with 0 must produce a nonzero first output (not stuck at 0).
    Multiplayer_SeedRng(0);
    ASSERT(Multiplayer_NextRandom() != 0);
}

static void TestGetRandomizedSpeciesOffWhenDisabled(void)
{
    // When randomizeEncounters=0, must return SPECIES_NONE (0).
    Multiplayer_Init();
    gCoopSettings.randomizeEncounters = 0;
    gCoopSettings.encounterSeed = 0xABCD1234u;
    ASSERT_EQ(Multiplayer_GetRandomizedSpecies(0x08100000u, 0), 0);
}

static void TestGetRandomizedSpeciesOffWhenNoSeed(void)
{
    // Seed=0 means unsynced — must return SPECIES_NONE.
    Multiplayer_Init();
    gCoopSettings.randomizeEncounters = 1;
    gCoopSettings.encounterSeed = 0;
    ASSERT_EQ(Multiplayer_GetRandomizedSpecies(0x08100000u, 3), 0);
}

static void TestGetRandomizedSpeciesInRange(void)
{
    u16 species;
    Multiplayer_Init();
    gCoopSettings.randomizeEncounters = 1;
    gCoopSettings.encounterSeed = 0x11223344u;
    species = Multiplayer_GetRandomizedSpecies(0x08100000u, 5);
    ASSERT(species >= 1);
    ASSERT(species <= 493);
}

static void TestGetRandomizedSpeciesDeterministic(void)
{
    u16 s1, s2;
    Multiplayer_Init();
    gCoopSettings.randomizeEncounters = 1;
    gCoopSettings.encounterSeed = 0xCAFEBABEu;
    s1 = Multiplayer_GetRandomizedSpecies(0x08200000u, 7);
    s2 = Multiplayer_GetRandomizedSpecies(0x08200000u, 7);
    ASSERT_EQ(s1, s2);
}

static void TestGetRandomizedSpeciesDiffSlotsDiffSpecies(void)
{
    // Two different slots in the same table should (very likely) differ.
    u16 s0, s11;
    Multiplayer_Init();
    gCoopSettings.randomizeEncounters = 1;
    gCoopSettings.encounterSeed = 0x55AA55AAu;
    s0  = Multiplayer_GetRandomizedSpecies(0x08300000u, 0);
    s11 = Multiplayer_GetRandomizedSpecies(0x08300000u, 11);
    // Not a hard requirement but they should differ for a good hash.
    ASSERT(s0 != s11);
}

// ---- Step 4.4: Seed sync -------------------------------------------------------

static void TestSendSeedSyncWritesPacket(void)
{
    Multiplayer_Init();
    ResetDispatch();
    Multiplayer_SendSeedSync(0xDEADBEEFu);
    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)MP_PKT_SIZE_SEED_SYNC);
    {
        u8 b;
        Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, MP_PKT_SEED_SYNC);
        Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 0xDE);
        Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 0xAD);
        Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 0xBE);
        Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 0xEF);
    }
}

static void TestSeedSyncRoundTrip(void)
{
    // Send → recv ring → Multiplayer_Update dispatch: encounterSeed must match.
    u8 b;
    u8 i;
    u8 pktLen;

    Multiplayer_Init();
    ResetDispatch();

    Multiplayer_SendSeedSync(0x12345678u);
    pktLen = Mp_Available(&gMpSendRing);
    ASSERT_EQ(pktLen, (u8)MP_PKT_SIZE_SEED_SYNC);

    for (i = 0; i < pktLen; i++)
    {
        Mp_Pop(&gMpSendRing, &b);
        Mp_Push(&gMpRecvRing, b);
    }

    Multiplayer_Update();
    ASSERT_EQ(gCoopSettings.encounterSeed, 0x12345678u);
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0); // no re-broadcast
}

static void TestGenerateSeedNonZeroOutput(void)
{
    // When both Random32 draws return 0 the fallback must kick in.
    gTestRandom32Value = 0;
    ASSERT(Multiplayer_GenerateSeed() != 0);
}

static void TestGenerateSeedNonZeroNormal(void)
{
    // A typical non-zero Random32 value must produce a non-zero seed.
    gTestRandom32Value = 0x55AA55AAu;
    ASSERT(Multiplayer_GenerateSeed() != 0);
}

// ---- Step 4.5: Randomizer correctness tests ------------------------------------

static void TestSameSeedSameSpeciesAllSlots(void)
{
    // Querying the same 12 slots twice with the same seed/addr must be identical.
    u16 first[12], second[12];
    int i;

    Multiplayer_Init();
    gCoopSettings.randomizeEncounters = 1;
    gCoopSettings.encounterSeed = 0xDEAD1234u;

    for (i = 0; i < 12; i++)
        first[i] = Multiplayer_GetRandomizedSpecies(0x08100000u, (u8)i);
    for (i = 0; i < 12; i++)
        second[i] = Multiplayer_GetRandomizedSpecies(0x08100000u, (u8)i);
    for (i = 0; i < 12; i++)
        ASSERT_EQ(first[i], second[i]);
}

static void TestDifferentSeedsDifferentSpecies(void)
{
    // Changing the seed must change the species for the same slot.
    u16 s1, s2;

    Multiplayer_Init();
    gCoopSettings.randomizeEncounters = 1;
    gCoopSettings.encounterSeed = 0x11111111u;
    s1 = Multiplayer_GetRandomizedSpecies(0x08100000u, 3);
    gCoopSettings.encounterSeed = 0x22222222u;
    s2 = Multiplayer_GetRandomizedSpecies(0x08100000u, 3);
    ASSERT(s1 != s2);
}

static void TestNoInvalidSpecies12Slots(void)
{
    // All 12 land-encounter slots must produce species in 1-493.
    int i;

    Multiplayer_Init();
    gCoopSettings.randomizeEncounters = 1;
    gCoopSettings.encounterSeed = 0xFEEDBEEFu;

    for (i = 0; i < 12; i++)
    {
        u16 s = Multiplayer_GetRandomizedSpecies(0x08100000u, (u8)i);
        ASSERT(s >= 1);
        ASSERT(s <= 493);
    }
}

static void TestRandomizedSpeciesPassThroughWhenDisabled(void)
{
    // Levels are never touched by the randomizer — verified structurally:
    // GetRandomizedSpecies returns 0 when disabled, and the caller falls back
    // to the original species from the encounter table (levels come from
    // ChooseWildMonLevel, which is independent of species).
    Multiplayer_Init();
    gCoopSettings.randomizeEncounters = 0;
    gCoopSettings.encounterSeed = 0xFFFFFFFFu;
    ASSERT_EQ(Multiplayer_GetRandomizedSpecies(0x08100000u, 0), 0);
    ASSERT_EQ(Multiplayer_GetRandomizedSpecies(0x08100000u, 11), 0);
}

// ---------------------------------------------------------------------------
// Boss readiness protocol tests
// ---------------------------------------------------------------------------

static void TestBossReadySoloProceeds(void)
{
    // When disconnected, CheckBossStart returns 1 immediately after BossReady
    ResetAll();
    gMultiplayerState.connState = MP_STATE_DISCONNECTED;

    ASSERT_EQ(Multiplayer_ScriptCheckBossStart(), 0); // not ready yet (bossReadyBossId==0)

    Multiplayer_BossReady_Brock();
    ASSERT_EQ(gMultiplayerState.bossReadyBossId, BOSS_ID_BROCK);

    ASSERT_EQ(Multiplayer_ScriptCheckBossStart(), 1); // solo: proceeds immediately
    // State cleared after returning 1
    ASSERT_EQ(gMultiplayerState.bossReadyBossId, 0);
    ASSERT_EQ(gMultiplayerState.partnerBossId, 0);
}

static void TestBossReadyConnectedWaitsForPartner(void)
{
    // When connected, CheckBossStart returns 0 until partner sends BOSS_READY
    ResetAll();
    gMultiplayerState.connState = MP_STATE_CONNECTED;

    Multiplayer_BossReady_Misty();
    ASSERT_EQ(gMultiplayerState.bossReadyBossId, BOSS_ID_MISTY);
    ASSERT_EQ(gMultiplayerState.partnerBossId, 0);

    ASSERT_EQ(Multiplayer_ScriptCheckBossStart(), 0); // partner not ready yet
    // State must survive the poll (not cleared on 0 return)
    ASSERT_EQ(gMultiplayerState.bossReadyBossId, BOSS_ID_MISTY);
}

static void TestBossReadyBothReadyProceeds(void)
{
    // Once partner sends BOSS_READY, CheckBossStart returns 1
    ResetAll();
    gMultiplayerState.connState       = MP_STATE_CONNECTED;
    gMultiplayerState.bossReadyBossId = BOSS_ID_BROCK;
    gMultiplayerState.partnerBossId   = BOSS_ID_BROCK; // simulate received BOSS_READY

    ASSERT_EQ(Multiplayer_ScriptCheckBossStart(), 1);
    // Both cleared after start
    ASSERT_EQ(gMultiplayerState.bossReadyBossId, 0);
    ASSERT_EQ(gMultiplayerState.partnerBossId, 0);
}

static void TestBossReadyCancelClearsState(void)
{
    // Multiplayer_BossCancel resets state and sends a cancel packet
    ResetAll();
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    Multiplayer_BossReady_Erika();

    ASSERT_NE(gMultiplayerState.bossReadyBossId, 0);

    Multiplayer_BossCancel();
    ASSERT_EQ(gMultiplayerState.bossReadyBossId, 0);
    ASSERT_EQ(gMultiplayerState.partnerBossId, 0);
    // A BOSS_CANCEL byte should be in the send ring (plus the earlier BOSS_READY)
    ASSERT_NE(Mp_Available(&gMpSendRing), 0);
}

static void TestBossReadyPartnerCancelRecv(void)
{
    // Receiving BOSS_CANCEL from partner clears partnerBossId.
    Multiplayer_Init();
    ResetDispatch();
    gSaveBlock1Ptr = &sTestSave;
    gMultiplayerState.partnerBossId = BOSS_ID_BROCK;

    Mp_Push(&gMpRecvRing, MP_PKT_BOSS_CANCEL);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.partnerBossId, 0);
}

static void TestBossStartPacketRecv(void)
{
    // Receiving MP_PKT_BOSS_START sets partnerBossId to match our bossReadyBossId.
    Multiplayer_Init();
    ResetDispatch();
    gSaveBlock1Ptr = &sTestSave;
    gMultiplayerState.connState       = MP_STATE_CONNECTED;
    gMultiplayerState.bossReadyBossId = BOSS_ID_KOGA;

    Mp_Push(&gMpRecvRing, MP_PKT_BOSS_START);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.partnerBossId, BOSS_ID_KOGA);
    // CheckBossStart should now return 1
    ASSERT_EQ(Multiplayer_ScriptCheckBossStart(), 1);
}

static void TestBossReadyIdempotent(void)
{
    // Calling BossReady again resets partnerBossId so stale state doesn't trigger start.
    ResetAll();
    gMultiplayerState.connState     = MP_STATE_CONNECTED;
    gMultiplayerState.partnerBossId = BOSS_ID_BROCK; // stale from a previous fight

    Multiplayer_BossReady_Brock(); // re-enter readiness check
    ASSERT_EQ(gMultiplayerState.partnerBossId, 0); // stale partner state cleared
    ASSERT_EQ(gMultiplayerState.bossReadyBossId, BOSS_ID_BROCK);
}

static void TestIsConnectedReflectsState(void)
{
    ResetAll();
    gMultiplayerState.connState = MP_STATE_DISCONNECTED;
    ASSERT_EQ(Multiplayer_IsConnected(), 0);

    gMultiplayerState.connState = MP_STATE_CONNECTING;
    ASSERT_EQ(Multiplayer_IsConnected(), 0);

    gMultiplayerState.connState = MP_STATE_CONNECTED;
    ASSERT_EQ(Multiplayer_IsConnected(), 1);
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
    TestIsSyncableFlag();
    TestRemoteFlagSetRouting();
    TestRemoteVarSetRouting();
    TestMultipleFlagSetsRouted();
    TestFullSyncSendBuildsPacket();
    TestFullSyncApplyORsFlags();
    TestFullSyncApplyRejectsWrongLength();
    TestFullSyncRoundTrip();
    TestScriptStartSetsFlag();
    TestScriptEndClearsFlag();
    TestScriptLockNoDuplicateSend();
    TestScriptLockUnlockNoDuplicateUnlock();
    TestScriptLockDisconnectedNoSend();
    TestPartnerScriptLockRecv();
    TestPartnerScriptUnlockRecv();
    TestGhostFreezesDuringPartnerScript();
    TestRngSameSeedSameSequence();
    TestRngDifferentSeedsDifferentSequences();
    TestRngZeroSeedNonZero();
    TestGetRandomizedSpeciesOffWhenDisabled();
    TestGetRandomizedSpeciesOffWhenNoSeed();
    TestGetRandomizedSpeciesInRange();
    TestGetRandomizedSpeciesDeterministic();
    TestGetRandomizedSpeciesDiffSlotsDiffSpecies();
    TestSendSeedSyncWritesPacket();
    TestSeedSyncRoundTrip();
    TestGenerateSeedNonZeroOutput();
    TestGenerateSeedNonZeroNormal();
    TestSameSeedSameSpeciesAllSlots();
    TestDifferentSeedsDifferentSpecies();
    TestNoInvalidSpecies12Slots();
    TestRandomizedSpeciesPassThroughWhenDisabled();
    TestBossReadySoloProceeds();
    TestBossReadyConnectedWaitsForPartner();
    TestBossReadyBothReadyProceeds();
    TestBossReadyCancelClearsState();
    TestBossReadyPartnerCancelRecv();
    TestBossStartPacketRecv();
    TestBossReadyIdempotent();
    TestIsConnectedReflectsState();
    TEST_SUMMARY();
}
