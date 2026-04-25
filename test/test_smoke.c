#include "test_runner.h"
#include "global.h"
#include "multiplayer.h"

static void TestMultiplayerInit(void)
{
    Multiplayer_Init();
    ASSERT_EQ(gMultiplayerState.role,               MP_ROLE_NONE);
    ASSERT_EQ(gMultiplayerState.connState,          MP_STATE_DISCONNECTED);
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, 0xFF);
    ASSERT_EQ(gMultiplayerState.bossReadyBossId,    0);
    ASSERT_EQ(gMultiplayerState.isInScript,         FALSE);
    ASSERT_EQ(gCoopSettings.randomizeEncounters,    1);
    ASSERT_EQ(gCoopSettings.encounterSeed,          0u);
}

static void TestIsSyncableFlagReturnsFalse(void)
{
    // Stub always returns FALSE until Phase 3 fills in the ranges.
    ASSERT_EQ(IsSyncableFlag(0x0000), FALSE);
    ASSERT_EQ(IsSyncableFlag(0xFFFF), FALSE);
}

static void TestDespawnClearsId(void)
{
    gMultiplayerState.ghostObjectEventId = 3;
    Multiplayer_DespawnGhost();
    ASSERT_EQ(gMultiplayerState.ghostObjectEventId, 0xFF);
}

int main(void)
{
    TestMultiplayerInit();
    TestIsSyncableFlagReturnsFalse();
    TestDespawnClearsId();
    TEST_SUMMARY();
}
