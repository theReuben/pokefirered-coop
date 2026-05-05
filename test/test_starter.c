// Native unit tests for the co-op starter coordination flow (Phase 1.5).
//
// Covers behaviour not exercised by test_smoke.c:
//   - Multiplayer_SendStarterPick: sends MP_PKT_STARTER_PICK only when connected
//   - Multiplayer_GetRivalStarterSpecies: returns the species neither player picked
//   - Multiplayer_GetRivalStarterSlot: returns the matching ball slot index
//   - Multiplayer_NativePollPartnerStarterPick: completion predicate for the
//     waitstarterpick script command
//   - Inbound MP_PKT_STARTER_PICK dispatch updates partnerStarterSpecies
//
// All tests use the canonical (non-randomized) starter mapping by leaving
// gCoopSettings.encounterSeed at 0, so Bulbasaur=1, Charmander=4, Squirtle=7.

#include "test_runner.h"
#include "global.h"
#include "multiplayer.h"
#include "constants/multiplayer.h"
#include <string.h>

// VAR_TEMP_2 holds the local player's chosen starter species; the
// PalletTown_ProfessorOaksLab script writes it via setvar before calling
// Multiplayer_SendStarterPick.
#define PLAYER_STARTER_SPECIES VAR_TEMP_2

// Multiplayer_Update calls GhostMapCheck which dereferences gSaveBlock1Ptr,
// so every test that runs a frame must have a valid save block.
static struct SaveBlock1 sTestSave;

static void ResetForStarterTest(void)
{
    Multiplayer_Init();                    // resets ring buffers and state
    // Multiplayer_Init does not zero partnerStarterSpecies (BSS-zero on real
    // hardware where Init runs once at boot). Reset it manually so test
    // ordering can't leak state between cases.
    gMultiplayerState.partnerStarterSpecies = 0;
    gCoopSettings.encounterSeed       = 0; // canonical starters
    gCoopSettings.randomizeEncounters = 1; // randomization toggle is on but seed=0
    VarSet(PLAYER_STARTER_SPECIES, 0);
    memset(&sTestSave, 0, sizeof(sTestSave));
    gSaveBlock1Ptr = &sTestSave;
}

// ---- SendStarterPick -------------------------------------------------------

static void TestSendStarterPickWritesPacketWhenConnected(void)
{
    u8 b;
    ResetForStarterTest();
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    VarSet(PLAYER_STARTER_SPECIES, SPECIES_CHARMANDER);

    Multiplayer_SendStarterPick();

    ASSERT_EQ(Mp_Available(&gMpSendRing), (u8)MP_PKT_SIZE_STARTER_PICK);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, MP_PKT_STARTER_PICK);
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, (u8)(SPECIES_CHARMANDER >> 8));
    Mp_Pop(&gMpSendRing, &b); ASSERT_EQ(b, (u8)(SPECIES_CHARMANDER & 0xFF));
}

static void TestSendStarterPickSuppressedWhenDisconnected(void)
{
    // Solo mode: must not write anything to the ring (no relay to flush to).
    ResetForStarterTest();
    gMultiplayerState.connState = MP_STATE_DISCONNECTED;
    VarSet(PLAYER_STARTER_SPECIES, SPECIES_BULBASAUR);

    Multiplayer_SendStarterPick();

    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

// ---- Inbound MP_PKT_STARTER_PICK dispatch ----------------------------------

static void TestPartnerStarterPickRecvUpdatesState(void)
{
    // The partner sends MP_PKT_STARTER_PICK; ProcessOneRecvPacket should
    // populate partnerStarterSpecies so GetRivalStarterSpecies has both inputs.
    ResetForStarterTest();
    ASSERT_EQ(gMultiplayerState.partnerStarterSpecies, 0);

    Mp_Push(&gMpRecvRing, MP_PKT_STARTER_PICK);
    Mp_Push(&gMpRecvRing, (u8)(SPECIES_SQUIRTLE >> 8));
    Mp_Push(&gMpRecvRing, (u8)(SPECIES_SQUIRTLE & 0xFF));

    Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.partnerStarterSpecies, SPECIES_SQUIRTLE);
    // The receiver must not echo a STARTER_PICK back.
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

static void TestPartnerStarterPickRecvTruncated(void)
{
    // Type byte arrives but the two-byte species payload is missing.
    // ProcessOneRecvPacket should bail without touching state.
    ResetForStarterTest();
    Mp_Push(&gMpRecvRing, MP_PKT_STARTER_PICK);
    Mp_Push(&gMpRecvRing, 0x00); // only one of the two payload bytes

    Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.partnerStarterSpecies, 0);
}

// ---- Rival starter logic ---------------------------------------------------

// All six (player, partner) permutations of the three canonical starters.
// The rival must take whichever species neither player chose.
static void TestRivalStarterUnchosenAllPermutations(void)
{
    ResetForStarterTest();

    // Player picks Bulbasaur, partner picks Charmander → rival = Squirtle
    VarSet(PLAYER_STARTER_SPECIES, SPECIES_BULBASAUR);
    gMultiplayerState.partnerStarterSpecies = SPECIES_CHARMANDER;
    ASSERT_EQ(Multiplayer_GetRivalStarterSpecies(), SPECIES_SQUIRTLE);

    // Player picks Bulbasaur, partner picks Squirtle → rival = Charmander
    gMultiplayerState.partnerStarterSpecies = SPECIES_SQUIRTLE;
    ASSERT_EQ(Multiplayer_GetRivalStarterSpecies(), SPECIES_CHARMANDER);

    // Player picks Charmander, partner picks Bulbasaur → rival = Squirtle
    VarSet(PLAYER_STARTER_SPECIES, SPECIES_CHARMANDER);
    gMultiplayerState.partnerStarterSpecies = SPECIES_BULBASAUR;
    ASSERT_EQ(Multiplayer_GetRivalStarterSpecies(), SPECIES_SQUIRTLE);

    // Player picks Charmander, partner picks Squirtle → rival = Bulbasaur
    gMultiplayerState.partnerStarterSpecies = SPECIES_SQUIRTLE;
    ASSERT_EQ(Multiplayer_GetRivalStarterSpecies(), SPECIES_BULBASAUR);

    // Player picks Squirtle, partner picks Bulbasaur → rival = Charmander
    VarSet(PLAYER_STARTER_SPECIES, SPECIES_SQUIRTLE);
    gMultiplayerState.partnerStarterSpecies = SPECIES_BULBASAUR;
    ASSERT_EQ(Multiplayer_GetRivalStarterSpecies(), SPECIES_CHARMANDER);

    // Player picks Squirtle, partner picks Charmander → rival = Bulbasaur
    gMultiplayerState.partnerStarterSpecies = SPECIES_CHARMANDER;
    ASSERT_EQ(Multiplayer_GetRivalStarterSpecies(), SPECIES_BULBASAUR);
}

static void TestRivalStarterFallbackWhenPartnerUnknown(void)
{
    // Partner hasn't picked yet (partnerStarterSpecies == 0). The rival
    // should still get a starter that is NOT the player's pick — never the
    // same species as the player.
    u16 rival;
    ResetForStarterTest();
    VarSet(PLAYER_STARTER_SPECIES, SPECIES_BULBASAUR);
    gMultiplayerState.partnerStarterSpecies = 0;

    rival = Multiplayer_GetRivalStarterSpecies();
    ASSERT_NE(rival, SPECIES_BULBASAUR);
    ASSERT(rival == SPECIES_CHARMANDER || rival == SPECIES_SQUIRTLE);
}

static void TestRivalStarterSlotMatchesSpecies(void)
{
    // Slot 0 = Bulbasaur, 1 = Charmander, 2 = Squirtle in canonical mapping.
    // GetRivalStarterSlot must return the index whose species == GetRivalStarterSpecies().
    ResetForStarterTest();

    VarSet(PLAYER_STARTER_SPECIES, SPECIES_BULBASAUR);
    gMultiplayerState.partnerStarterSpecies = SPECIES_CHARMANDER;
    ASSERT_EQ(Multiplayer_GetRivalStarterSlot(), 2); // Squirtle is in slot 2

    VarSet(PLAYER_STARTER_SPECIES, SPECIES_BULBASAUR);
    gMultiplayerState.partnerStarterSpecies = SPECIES_SQUIRTLE;
    ASSERT_EQ(Multiplayer_GetRivalStarterSlot(), 1); // Charmander is in slot 1

    VarSet(PLAYER_STARTER_SPECIES, SPECIES_CHARMANDER);
    gMultiplayerState.partnerStarterSpecies = SPECIES_SQUIRTLE;
    ASSERT_EQ(Multiplayer_GetRivalStarterSlot(), 0); // Bulbasaur is in slot 0
}

// ---- NativePollPartnerStarterPick ------------------------------------------
// Drives the waitstarterpick script command — returns TRUE when the partner
// has confirmed (or we're solo and don't need to wait).

static void TestPollOfflineReturnsTrue(void)
{
    ResetForStarterTest();
    gMultiplayerState.connState             = MP_STATE_DISCONNECTED;
    gMultiplayerState.partnerStarterSpecies = 0;
    ASSERT_EQ(Multiplayer_NativePollPartnerStarterPick(), TRUE);
}

static void TestPollConnectedNoPickReturnsFalse(void)
{
    ResetForStarterTest();
    gMultiplayerState.connState             = MP_STATE_CONNECTED;
    gMultiplayerState.partnerStarterSpecies = 0;
    ASSERT_EQ(Multiplayer_NativePollPartnerStarterPick(), FALSE);
}

static void TestPollConnectedAfterPickReturnsTrue(void)
{
    ResetForStarterTest();
    gMultiplayerState.connState             = MP_STATE_CONNECTED;
    gMultiplayerState.partnerStarterSpecies = SPECIES_BULBASAUR;
    ASSERT_EQ(Multiplayer_NativePollPartnerStarterPick(), TRUE);
}

// ---- Entry point -----------------------------------------------------------

int main(void)
{
    TestSendStarterPickWritesPacketWhenConnected();
    TestSendStarterPickSuppressedWhenDisconnected();
    TestPartnerStarterPickRecvUpdatesState();
    TestPartnerStarterPickRecvTruncated();
    TestRivalStarterUnchosenAllPermutations();
    TestRivalStarterFallbackWhenPartnerUnknown();
    TestRivalStarterSlotMatchesSpecies();
    TestPollOfflineReturnsTrue();
    TestPollConnectedNoPickReturnsFalse();
    TestPollConnectedAfterPickReturnsTrue();
    TEST_SUMMARY();
}
