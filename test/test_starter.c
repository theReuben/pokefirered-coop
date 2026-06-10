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
#include "constants/vars.h"   // VAR_STARTER_MON
#include <string.h>

// VAR_TEMP_2 holds the local player's chosen starter species when the picker
// script calls Multiplayer_SendStarterPick.  The rival trigger then overwrites
// it with a ball position (1/2/3), so Multiplayer_GetRivalStarterSpecies reads
// gMultiplayerState.myStarterSpecies instead.
#define PLAYER_STARTER_SPECIES VAR_TEMP_2

// Multiplayer_Update calls GhostMapCheck which dereferences gSaveBlock1Ptr,
// so every test that runs a frame must have a valid save block.
static struct SaveBlock1 sTestSave;

void TestResetGameVars(void);          // from stubs.c

static void ResetForStarterTest(void)
{
    Multiplayer_Init();                    // resets ring buffers and state
    TestResetGameVars();                   // wipe persisted VAR_* outcomes
    // Pre-set CONNECTED so ProcessOneRecvPacket's auto-connect block does not
    // fire and write gender+name packets to the send ring when tests push
    // inbound packets while simulating the already-connected state.
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    // Suppress the gotPartnerGender flood so 'send ring stays empty' asserts hold.
    gMultiplayerState.gotPartnerGender = TRUE;
    // Multiplayer_Init does not zero partnerStarterSpecies (BSS-zero on real
    // hardware where Init runs once at boot). Reset it manually so test
    // ordering can't leak state between cases.
    gMultiplayerState.partnerStarterSpecies = 0;
    gMultiplayerState.myStarterSpecies      = 0;
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
    // The pick script sets FLAG_SYS_POKEMON_GET and VAR_STARTER_MON (slot)
    // before calling the special; SendStarterPick derives the species from
    // those instead of the trigger-shared VAR_TEMP_2.
    FlagSet(FLAG_SYS_POKEMON_GET);
    VarSet(VAR_STARTER_MON, 2); // Charmander ball

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
    FlagSet(FLAG_SYS_POKEMON_GET);
    VarSet(VAR_STARTER_MON, 0); // Bulbasaur ball

    Multiplayer_SendStarterPick();

    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
    // The pick is still cached for the session even though nothing was sent.
    ASSERT_EQ(gMultiplayerState.myStarterSpecies, SPECIES_BULBASAUR);
}

static void TestSendStarterPickNoOpWithoutStarter(void)
{
    // Without FLAG_SYS_POKEMON_GET the special must not fabricate a pick from
    // VAR_STARTER_MON's default 0 (which is also the Bulbasaur slot).
    ResetForStarterTest();
    gMultiplayerState.connState = MP_STATE_CONNECTED;

    Multiplayer_SendStarterPick();

    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
    ASSERT_EQ(gMultiplayerState.myStarterSpecies, 0);
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
static void CheckRivalForPicks(u16 mine, u16 partner, u16 expectedRival)
{
    // Fresh state per permutation: once both picks are known the result is
    // persisted to VAR_RIVAL_STARTER and later calls return the saved value.
    ResetForStarterTest();
    gMultiplayerState.myStarterSpecies      = mine;
    gMultiplayerState.partnerStarterSpecies = partner;
    ASSERT_EQ(Multiplayer_GetRivalStarterSpecies(), expectedRival);
    // The derivation must also have been persisted.
    ASSERT_EQ(VarGet(VAR_RIVAL_STARTER), expectedRival);
    // Persisted value survives the session state being wiped.
    gMultiplayerState.myStarterSpecies      = 0;
    gMultiplayerState.partnerStarterSpecies = 0;
    ASSERT_EQ(Multiplayer_GetRivalStarterSpecies(), expectedRival);
}

static void TestRivalStarterUnchosenAllPermutations(void)
{
    CheckRivalForPicks(SPECIES_BULBASAUR,  SPECIES_CHARMANDER, SPECIES_SQUIRTLE);
    CheckRivalForPicks(SPECIES_BULBASAUR,  SPECIES_SQUIRTLE,   SPECIES_CHARMANDER);
    CheckRivalForPicks(SPECIES_CHARMANDER, SPECIES_BULBASAUR,  SPECIES_SQUIRTLE);
    CheckRivalForPicks(SPECIES_CHARMANDER, SPECIES_SQUIRTLE,   SPECIES_BULBASAUR);
    CheckRivalForPicks(SPECIES_SQUIRTLE,   SPECIES_BULBASAUR,  SPECIES_CHARMANDER);
    CheckRivalForPicks(SPECIES_SQUIRTLE,   SPECIES_CHARMANDER, SPECIES_BULBASAUR);
}

static void TestRivalStarterFallbackWhenPartnerUnknown(void)
{
    // Partner hasn't picked yet (partnerStarterSpecies == 0). The rival
    // should still get a starter that is NOT the player's pick — never the
    // same species as the player.
    u16 rival;
    ResetForStarterTest();
    gMultiplayerState.myStarterSpecies      = SPECIES_BULBASAUR;
    gMultiplayerState.partnerStarterSpecies = 0;

    rival = Multiplayer_GetRivalStarterSpecies();
    ASSERT_NE(rival, SPECIES_BULBASAUR);
    ASSERT(rival == SPECIES_CHARMANDER || rival == SPECIES_SQUIRTLE);
}

static void TestRivalStarterSlotMatchesSpecies(void)
{
    // FRLG ball order (left to right): slot 0=Bulbasaur, 1=Squirtle, 2=Charmander.
    // Matches events.inc positions x=8,9,10 and sCanonical[] in multiplayer.c.
    // GetRivalStarterSlot must return the index whose species == GetRivalStarterSpecies().
    ResetForStarterTest();

    gMultiplayerState.myStarterSpecies      = SPECIES_BULBASAUR;
    gMultiplayerState.partnerStarterSpecies = SPECIES_CHARMANDER;
    ASSERT_EQ(Multiplayer_GetRivalStarterSlot(), 1); // Squirtle is in slot 1

    // Result persists per save; reset before checking a different pick pair.
    ResetForStarterTest();
    gMultiplayerState.myStarterSpecies      = SPECIES_BULBASAUR;
    gMultiplayerState.partnerStarterSpecies = SPECIES_SQUIRTLE;
    ASSERT_EQ(Multiplayer_GetRivalStarterSlot(), 2); // Charmander is in slot 2

    ResetForStarterTest();
    gMultiplayerState.myStarterSpecies      = SPECIES_CHARMANDER;
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

// ---- Persistence: VAR_PARTNER_STARTER / VAR_RIVAL_STARTER -------------------

static void TestPartnerPickReceiptPersistsVar(void)
{
    // Receiving the partner's pick must persist it to VAR_PARTNER_STARTER so
    // a save made afterwards carries the pick across reload/reconnect.
    ResetForStarterTest();

    Mp_Push(&gMpRecvRing, MP_PKT_STARTER_PICK);
    Mp_Push(&gMpRecvRing, (u8)(SPECIES_SQUIRTLE >> 8));
    Mp_Push(&gMpRecvRing, (u8)(SPECIES_SQUIRTLE & 0xFF));

    Multiplayer_Update();

    ASSERT_EQ(VarGet(VAR_PARTNER_STARTER), SPECIES_SQUIRTLE);
    // Rival var stays undetermined until our own pick is known too.
    ASSERT_EQ(VarGet(VAR_RIVAL_STARTER), 0);
}

static void TestBothPicksKnownPersistsRivalVar(void)
{
    // Once both picks are known the rival species is persisted exactly once.
    ResetForStarterTest();

    gMultiplayerState.myStarterSpecies = SPECIES_BULBASAUR;
    Mp_Push(&gMpRecvRing, MP_PKT_STARTER_PICK);
    Mp_Push(&gMpRecvRing, (u8)(SPECIES_CHARMANDER >> 8));
    Mp_Push(&gMpRecvRing, (u8)(SPECIES_CHARMANDER & 0xFF));

    Multiplayer_Update();

    ASSERT_EQ(VarGet(VAR_PARTNER_STARTER), SPECIES_CHARMANDER);
    ASSERT_EQ(VarGet(VAR_RIVAL_STARTER),   SPECIES_SQUIRTLE);
}

static void TestReloadRecoversFromPersistedVars(void)
{
    // Simulated save-state reload: IWRAM session state zeroed, persisted vars
    // intact.  GetRivalStarterSpecies must serve the saved answer immediately
    // and Update must restore the partner pick cache without any packet.
    ResetForStarterTest();
    VarSet(VAR_PARTNER_STARTER, SPECIES_CHARMANDER);
    VarSet(VAR_RIVAL_STARTER,   SPECIES_SQUIRTLE);

    ASSERT_EQ(Multiplayer_GetRivalStarterSpecies(), SPECIES_SQUIRTLE);
    ASSERT_EQ(Multiplayer_GetRivalStarterSlot(), 1);

    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.partnerStarterSpecies, SPECIES_CHARMANDER);
}

// ---- Phantom-pick guard ------------------------------------------------------

static void TestNoPhantomPickBeforeStarterObtained(void)
{
    // VAR_STARTER_MON defaults to 0, which is also slot 0 (Bulbasaur).  With
    // FLAG_SYS_POKEMON_GET clear, Update must NOT fabricate a pick from it,
    // and the background resend must stay silent — previously this broadcast
    // a phantom Bulbasaur pick that hid the partner's Bulbasaur ball.
    int i;
    ResetForStarterTest();

    for (i = 0; i < 130; i++)
        Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.myStarterSpecies, 0);
    // Only heartbeat pings may be in the ring — no MP_PKT_STARTER_PICK.
    {
        u8 b;
        while (Mp_Available(&gMpSendRing))
        {
            Mp_Pop(&gMpSendRing, &b);
            ASSERT_NE(b, MP_PKT_STARTER_PICK);
        }
    }
}

static void TestRecoveryAfterStarterObtained(void)
{
    // With FLAG_SYS_POKEMON_GET set, the recovery path restores the pick from
    // VAR_STARTER_MON (here slot 2 = Charmander) and persists the outcome.
    ResetForStarterTest();
    FlagSet(FLAG_SYS_POKEMON_GET);
    VarSet(VAR_STARTER_MON, 2);
    VarSet(VAR_PARTNER_STARTER, SPECIES_BULBASAUR);

    Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.myStarterSpecies, SPECIES_CHARMANDER);
    ASSERT_EQ(VarGet(VAR_RIVAL_STARTER), SPECIES_SQUIRTLE);
}

// ---- Entry point -----------------------------------------------------------

int main(void)
{
    TestSendStarterPickWritesPacketWhenConnected();
    TestSendStarterPickSuppressedWhenDisconnected();
    TestSendStarterPickNoOpWithoutStarter();
    TestPartnerStarterPickRecvUpdatesState();
    TestPartnerStarterPickRecvTruncated();
    TestRivalStarterUnchosenAllPermutations();
    TestRivalStarterFallbackWhenPartnerUnknown();
    TestRivalStarterSlotMatchesSpecies();
    TestPollOfflineReturnsTrue();
    TestPollConnectedNoPickReturnsFalse();
    TestPollConnectedAfterPickReturnsTrue();
    TestPartnerPickReceiptPersistsVar();
    TestBothPicksKnownPersistsRivalVar();
    TestReloadRecoversFromPersistedVars();
    TestNoPhantomPickBeforeStarterObtained();
    TestRecoveryAfterStarterObtained();
    TEST_SUMMARY();
}
