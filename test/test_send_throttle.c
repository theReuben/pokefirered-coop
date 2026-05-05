// Native unit tests for the position-send throttling in Multiplayer_Update.
//
// Multiplayer_Update increments posFrameCounter each frame and calls
// Multiplayer_SendPosition every 4th frame, but only when connected.
// Tests verify the cadence and that no traffic is generated while disconnected.

#include "test_runner.h"
#include "global.h"
#include "multiplayer.h"
#include "constants/multiplayer.h"
#include <string.h>

static struct SaveBlock1 sTestSave;

static void SetUpConnectedPlayer(void)
{
    Multiplayer_Init();
    memset(gObjectEvents, 0, sizeof(gObjectEvents));
    memset(&sTestSave, 0, sizeof(sTestSave));

    sTestSave.location.mapGroup = 0;
    sTestSave.location.mapNum   = 1;
    gSaveBlock1Ptr              = &sTestSave;

    // SendPosition reads gPlayerAvatar.objectEventId and dereferences
    // gObjectEvents[that slot]. Slot 0 with zero coords is enough.
    gPlayerAvatar.objectEventId = 0;
    gObjectEvents[0].currentCoords.x = 7;
    gObjectEvents[0].currentCoords.y = 11;
    gObjectEvents[0].facingDirection = DIR_SOUTH;

    gMultiplayerState.connState = MP_STATE_CONNECTED;
}

// ---- Throttling cadence ----------------------------------------------------

static void TestNoPositionForFirstThreeFrames(void)
{
    SetUpConnectedPlayer();

    // Three Update calls must not emit a position packet (counter 1, 2, 3).
    Multiplayer_Update();
    Multiplayer_Update();
    Multiplayer_Update();

    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

static void TestPositionEmittedOnFourthFrame(void)
{
    u8 b;
    SetUpConnectedPlayer();

    Multiplayer_Update();
    Multiplayer_Update();
    Multiplayer_Update();
    Multiplayer_Update(); // 4th frame: SendPosition fires

    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)MP_PKT_SIZE_POSITION);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, MP_PKT_POSITION);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 0);   // mapGroup
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 1);   // mapNum
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 7);   // x
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, 11);  // y
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, DIR_SOUTH);
}

static void TestPositionRepeatsEveryFourFrames(void)
{
    // 8 frames → 2 position packets. Drain after each window.
    u8 i, b;

    SetUpConnectedPlayer();
    for (i = 0; i < 4; i++) Multiplayer_Update();
    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)MP_PKT_SIZE_POSITION);
    while (Mp_Pop(&gMpSendRing, &b)) {}

    for (i = 0; i < 4; i++) Multiplayer_Update();
    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)MP_PKT_SIZE_POSITION);
}

// ---- Suppression while disconnected ----------------------------------------

static void TestNoPositionWhenDisconnected(void)
{
    u8 i;
    SetUpConnectedPlayer();
    gMultiplayerState.connState = MP_STATE_DISCONNECTED;

    for (i = 0; i < 16; i++) Multiplayer_Update();

    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

static void TestPosFrameCounterFrozenWhileDisconnected(void)
{
    // Counter must not advance while disconnected; otherwise reconnecting
    // could fire SendPosition immediately and miss its throttle.
    u8 i;
    SetUpConnectedPlayer();
    gMultiplayerState.connState = MP_STATE_DISCONNECTED;
    for (i = 0; i < 10; i++) Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.posFrameCounter, 0);
}

// ---- Connecting state behaves like disconnected (no traffic yet) -----------

static void TestNoPositionWhileConnecting(void)
{
    u8 i;
    SetUpConnectedPlayer();
    gMultiplayerState.connState = MP_STATE_CONNECTING;

    for (i = 0; i < 8; i++) Multiplayer_Update();

    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

// ---- Entry point -----------------------------------------------------------

int main(void)
{
    TestNoPositionForFirstThreeFrames();
    TestPositionEmittedOnFourthFrame();
    TestPositionRepeatsEveryFourFrames();
    TestNoPositionWhenDisconnected();
    TestPosFrameCounterFrozenWhileDisconnected();
    TestNoPositionWhileConnecting();
    TEST_SUMMARY();
}
