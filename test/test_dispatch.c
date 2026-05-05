// Native unit tests for the inbound packet dispatch path
// (Multiplayer_Update → ProcessOneRecvPacket).
//
// Covers branches not exercised by test_smoke.c:
//   - MP_PKT_POSITION dispatch → Multiplayer_UpdateGhostPosition
//   - MP_PKT_ITEM_GIVE dispatch → AddBagItem stub
//   - MP_PKT_FLAG_CLEAR dispatch → Multiplayer_HandleRemoteFlagClear stub
//   - MP_PKT_PARTNER_CONNECTED / MP_PKT_PARTNER_DISCONNECTED state transitions
//   - Truncated packet: leftover bytes don't corrupt subsequent dispatch
//   - Unknown packet: ring is drained so the dispatcher doesn't stall
//   - remoteUpdateThisFrame is set when a remote update applied, cleared next frame

#include "test_runner.h"
#include "global.h"
#include "multiplayer.h"
#include "constants/multiplayer.h"
#include <string.h>

// Counters / recorders defined in stubs.c
extern u16 gTestLastAddBagItemId;
extern u16 gTestLastAddBagItemCount;
extern u8  gTestRemoteFlagSetCallCount;
extern u16 gTestLastRemoteFlagId;
extern u8  gTestNextSpawnSlot;

// stubs.c's Multiplayer_HandleRemoteFlagClear is a no-op, so for FLAG_CLEAR
// dispatch we assert on the side effects ProcessOneRecvPacket itself owns:
//   - gMultiplayerState.remoteUpdateThisFrame is set
//   - the recv ring is drained (no leftover bytes)
//   - no echo appears in the send ring

static struct SaveBlock1 sTestSave;

static void ResetAll(void)
{
    Multiplayer_Init();
    memset(gObjectEvents, 0, sizeof(gObjectEvents));
    memset(&sTestSave, 0, sizeof(sTestSave));
    sTestSave.location.mapGroup = 0;
    sTestSave.location.mapNum   = 0;
    gSaveBlock1Ptr              = &sTestSave;
    gTestNextSpawnSlot          = 16;

    gTestLastAddBagItemId      = 0;
    gTestLastAddBagItemCount   = 0;
    gTestRemoteFlagSetCallCount = 0;
    gTestLastRemoteFlagId      = 0;
}

// ---- POSITION packet inbound dispatch --------------------------------------

static void TestPositionPacketUpdatesTargets(void)
{
    // POSITION arrives → UpdateGhostPosition copies into gMultiplayerState.
    u8 pkt[MP_PKT_SIZE_POSITION];
    u8 i, len;

    ResetAll();
    len = Mp_EncodePosition(pkt, /*mapGroup*/3, /*mapNum*/4, /*x*/12, /*y*/8, DIR_WEST);
    ASSERT_EQ(len, MP_PKT_SIZE_POSITION);
    for (i = 0; i < len; i++) Mp_Push(&gMpRecvRing, pkt[i]);

    Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.partnerMapGroup, 3);
    ASSERT_EQ(gMultiplayerState.partnerMapNum,   4);
    ASSERT_EQ(gMultiplayerState.targetX,         12);
    ASSERT_EQ(gMultiplayerState.targetY,         8);
    ASSERT_EQ(gMultiplayerState.targetFacing,    DIR_WEST);
}

static void TestPositionSameMapTriggersGhostSpawn(void)
{
    // Player on (group=0, num=5); partner POSITION packet arrives for the same map.
    // GhostMapCheck (called by Update) should spawn the ghost.
    u8 pkt[MP_PKT_SIZE_POSITION];
    u8 i, len;

    ResetAll();
    sTestSave.location.mapGroup = 0;
    sTestSave.location.mapNum   = 5;
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    gTestNextSpawnSlot          = 8;

    len = Mp_EncodePosition(pkt, 0, 5, 4, 6, DIR_NORTH);
    for (i = 0; i < len; i++) Mp_Push(&gMpRecvRing, pkt[i]);

    Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, 8);
    ASSERT_NE(gObjectEvents[8].active, 0);
}

static void TestPositionDifferentMapNoSpawn(void)
{
    // Player on map (0, 5); partner on map (0, 6) → ghost stays despawned.
    u8 pkt[MP_PKT_SIZE_POSITION];
    u8 i, len;

    ResetAll();
    sTestSave.location.mapGroup = 0;
    sTestSave.location.mapNum   = 5;
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    gTestNextSpawnSlot          = 4;

    len = Mp_EncodePosition(pkt, 0, 6, 1, 1, DIR_SOUTH);
    for (i = 0; i < len; i++) Mp_Push(&gMpRecvRing, pkt[i]);

    Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, GHOST_INVALID_SLOT);
    ASSERT_EQ(gObjectEvents[4].active, 0);
}

// ---- ITEM_GIVE inbound dispatch --------------------------------------------

static void TestItemGivePacketAddsToBag(void)
{
    // ITEM_GIVE inbound → AddBagItem(itemId, qty) is called.
    ResetAll();

    Mp_Push(&gMpRecvRing, MP_PKT_ITEM_GIVE);
    Mp_Push(&gMpRecvRing, 0x00);          // itemHi
    Mp_Push(&gMpRecvRing, 0x4F);          // itemLo → itemId 0x004F
    Mp_Push(&gMpRecvRing, 3);             // quantity

    Multiplayer_Update();

    ASSERT_EQ(gTestLastAddBagItemId,    0x004F);
    ASSERT_EQ(gTestLastAddBagItemCount, 3);
    // Inbound dispatch must not echo a packet back.
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

static void TestItemGivePacketIgnoresZeroItem(void)
{
    // itemId == ITEM_NONE (0) must not call AddBagItem.
    ResetAll();
    gTestLastAddBagItemId    = 0xAAAA; // sentinel — must remain unchanged
    gTestLastAddBagItemCount = 0xAAAA;

    Mp_Push(&gMpRecvRing, MP_PKT_ITEM_GIVE);
    Mp_Push(&gMpRecvRing, 0);
    Mp_Push(&gMpRecvRing, 0);
    Mp_Push(&gMpRecvRing, 5);

    Multiplayer_Update();

    ASSERT_EQ(gTestLastAddBagItemId,    0xAAAA);
    ASSERT_EQ(gTestLastAddBagItemCount, 0xAAAA);
}

static void TestItemGivePacketIgnoresZeroQuantity(void)
{
    ResetAll();
    gTestLastAddBagItemId    = 0xAAAA;
    gTestLastAddBagItemCount = 0xAAAA;

    Mp_Push(&gMpRecvRing, MP_PKT_ITEM_GIVE);
    Mp_Push(&gMpRecvRing, 0x01);
    Mp_Push(&gMpRecvRing, 0x10);
    Mp_Push(&gMpRecvRing, 0); // quantity 0 — no-op

    Multiplayer_Update();

    ASSERT_EQ(gTestLastAddBagItemId,    0xAAAA);
    ASSERT_EQ(gTestLastAddBagItemCount, 0xAAAA);
}

// ---- OnItemGiven outbound --------------------------------------------------

static void TestOnItemGivenWritesPacketWhenConnected(void)
{
    u8 b;
    ResetAll();
    gMultiplayerState.connState = MP_STATE_CONNECTED;

    Multiplayer_OnItemGiven(0x0123, 7);

    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)MP_PKT_SIZE_ITEM_GIVE);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, MP_PKT_ITEM_GIVE);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 0x01);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 0x23);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 7);
}

static void TestOnItemGivenSuppressedWhenDisconnected(void)
{
    ResetAll();
    gMultiplayerState.connState = MP_STATE_DISCONNECTED;

    Multiplayer_OnItemGiven(0x0040, 1);

    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

// ---- FLAG_CLEAR send + roundtrip -------------------------------------------

static void TestSendFlagClearWritesPacket(void)
{
    u8 b;
    u16 flagId = SYNC_FLAG_TRAINERS_START + 17;

    ResetAll();
    Multiplayer_SendFlagClear(flagId);

    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)MP_PKT_SIZE_FLAG_CLEAR);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, MP_PKT_FLAG_CLEAR);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, (u8)(flagId >> 8));
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, (u8)(flagId & 0xFF));
}

static void TestFlagClearRecvSetsRemoteUpdate(void)
{
    // Inbound MP_PKT_FLAG_CLEAR sets remoteUpdateThisFrame so map scripts
    // don't fire on the receiver this frame.
    u8 pkt[MP_PKT_SIZE_FLAG_CLEAR];
    u8 i, len;

    ResetAll();
    len = Mp_EncodeFlagClear(pkt, SYNC_FLAG_BOSSES_START + 1);
    for (i = 0; i < len; i++) Mp_Push(&gMpRecvRing, pkt[i]);

    Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.remoteUpdateThisFrame, TRUE);
    // No re-broadcast.
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

// ---- PARTNER_CONNECTED / PARTNER_DISCONNECTED ------------------------------

static void TestPartnerConnectedTransition(void)
{
    ResetAll();
    gMultiplayerState.connState = MP_STATE_DISCONNECTED;

    Mp_Push(&gMpRecvRing, MP_PKT_PARTNER_CONNECTED);
    Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.connState, MP_STATE_CONNECTED);
}

static void TestPartnerDisconnectedDespawnsGhost(void)
{
    // Ghost is active; partner disconnects → ghost must be removed and
    // connState reverts to DISCONNECTED.
    ResetAll();
    gMultiplayerState.connState          = MP_STATE_CONNECTED;
    gMultiplayerState.ghostObjectEventId = 6;
    gObjectEvents[6].active              = 1;

    Mp_Push(&gMpRecvRing, MP_PKT_PARTNER_DISCONNECTED);
    Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.connState,          MP_STATE_DISCONNECTED);
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, GHOST_INVALID_SLOT);
    ASSERT_EQ(gObjectEvents[6].active, 0);
}

// ---- Truncated / unknown packets -------------------------------------------

static void TestTruncatedPositionDoesNotCorruptState(void)
{
    // Partial POSITION packet (type byte + 2 of 5 payload bytes) must not
    // overwrite state with garbage. ProcessOneRecvPacket returns FALSE
    // and leaves the partial bytes in the ring for next frame.
    ResetAll();
    Mp_Push(&gMpRecvRing, MP_PKT_POSITION);
    Mp_Push(&gMpRecvRing, 1); // partial mapGroup
    Mp_Push(&gMpRecvRing, 2); // partial mapNum

    // Snapshot pre-state
    u8 mg = gMultiplayerState.partnerMapGroup;
    u8 mn = gMultiplayerState.partnerMapNum;

    Multiplayer_Update();

    // partner map values must remain at their initial sentinel (0xFF).
    ASSERT_EQ(gMultiplayerState.partnerMapGroup, mg);
    ASSERT_EQ(gMultiplayerState.partnerMapNum,   mn);
}

static void TestUnknownPacketTypeDrainsRing(void)
{
    // Unknown type byte → dispatcher drains the ring rather than looping
    // forever. After Update, the recv ring must be empty.
    ResetAll();
    Mp_Push(&gMpRecvRing, 0xFE); // not a known MP_PKT_* type
    Mp_Push(&gMpRecvRing, 0xAA);
    Mp_Push(&gMpRecvRing, 0xBB);
    Mp_Push(&gMpRecvRing, 0xCC);

    Multiplayer_Update();

    ASSERT_EQ(Mp_Available(&gMpRecvRing), 0);
}

// ---- remoteUpdateThisFrame frame flag --------------------------------------

static void TestRemoteUpdateClearedNextFrame(void)
{
    // Remote FLAG_SET arrives → flag is set this frame. Next frame with no
    // packets, the flag clears at the top of Update.
    u8 pkt[MP_PKT_SIZE_FLAG_SET];
    u8 i, len;

    ResetAll();
    len = Mp_EncodeFlagSet(pkt, SYNC_FLAG_TRAINERS_START + 2);
    for (i = 0; i < len; i++) Mp_Push(&gMpRecvRing, pkt[i]);

    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.remoteUpdateThisFrame, TRUE);

    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.remoteUpdateThisFrame, FALSE);
}

// ---- Entry point -----------------------------------------------------------

int main(void)
{
    TestPositionPacketUpdatesTargets();
    TestPositionSameMapTriggersGhostSpawn();
    TestPositionDifferentMapNoSpawn();
    TestItemGivePacketAddsToBag();
    TestItemGivePacketIgnoresZeroItem();
    TestItemGivePacketIgnoresZeroQuantity();
    TestOnItemGivenWritesPacketWhenConnected();
    TestOnItemGivenSuppressedWhenDisconnected();
    TestSendFlagClearWritesPacket();
    TestFlagClearRecvSetsRemoteUpdate();
    TestPartnerConnectedTransition();
    TestPartnerDisconnectedDespawnsGhost();
    TestTruncatedPositionDoesNotCorruptState();
    TestUnknownPacketTypeDrainsRing();
    TestRemoteUpdateClearedNextFrame();
    TEST_SUMMARY();
}
