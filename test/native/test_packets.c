#include "test_runner.h"
#include "global.h"
#include "main.h"   // for gMain.inBattle (trainer-lock lifecycle tests)
#include "multiplayer.h"
#include "constants/multiplayer.h"
#include "constants/battle.h"
#include "pokemon.h"
#include "battle.h"
#include "random.h"
#include <string.h>

// ---- Ring buffer helpers --------------------------------------------------

static void ResetRings(void)
{
    memset(&gMpSendRing, 0, sizeof(gMpSendRing));
    memset(&gMpRecvRing, 0, sizeof(gMpRecvRing));
    gMpSendRing.magic = MP_RING_MAGIC;
    gMpRecvRing.magic = MP_RING_MAGIC;
}

// ---- Ring buffer unit tests -----------------------------------------------

static void TestRingPushPop(void)
{
    u8 out;
    ResetRings();
    ASSERT_EQ(Mp_Push(&gMpSendRing, 0xAB), TRUE);
    ASSERT_EQ(Mp_Available(&gMpSendRing), 1);
    ASSERT_EQ(Mp_Pop(&gMpSendRing, &out), TRUE);
    ASSERT_EQ(out, 0xAB);
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

static void TestRingEmptyPop(void)
{
    u8 out = 0;
    ResetRings();
    ASSERT_EQ(Mp_Pop(&gMpSendRing, &out), FALSE);
    ASSERT_EQ(out, 0); // unchanged
}

static void TestRingFull(void)
{
    u8 i;
    ResetRings();
    // A 256-byte ring holds at most 255 bytes (one slot reserved for empty sentinel).
    for (i = 0; i < 255; i++)
        ASSERT_EQ(Mp_Push(&gMpSendRing, i), TRUE);
    // Next push should fail — ring is full.
    ASSERT_EQ(Mp_Push(&gMpSendRing, 0xFF), FALSE);
    ASSERT_EQ(Mp_Available(&gMpSendRing), 255);
}

static void TestRingWrapAround(void)
{
    u8 i, out;
    ResetRings();
    // Fill then drain half the ring to advance the pointers past 128.
    for (i = 0; i < 200; i++) Mp_Push(&gMpSendRing, i);
    for (i = 0; i < 200; i++) Mp_Pop(&gMpSendRing, &out);
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
    // Now push across the 256-byte wrap boundary.
    for (i = 0; i < 100; i++) ASSERT_EQ(Mp_Push(&gMpSendRing, (u8)(i + 10)), TRUE);
    ASSERT_EQ(Mp_Available(&gMpSendRing), 100);
    for (i = 0; i < 100; i++)
    {
        Mp_Pop(&gMpSendRing, &out);
        ASSERT_EQ(out, (u8)(i + 10));
    }
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

// ---- POSITION encode/decode -----------------------------------------------

static void TestEncodeDecodePosition(void)
{
    u8 buf[MP_PKT_SIZE_POSITION];
    u8 mapGroup, mapNum, x, y, facing;

    u8 n = Mp_EncodePosition(buf, 3, 12, 15, 8, DIR_EAST);
    ASSERT_EQ(n, MP_PKT_SIZE_POSITION);
    ASSERT_EQ(buf[0], MP_PKT_POSITION);
    ASSERT_EQ(buf[1], 3);
    ASSERT_EQ(buf[2], 12);
    ASSERT_EQ(buf[3], 15);
    ASSERT_EQ(buf[4], 8);
    ASSERT_EQ(buf[5], DIR_EAST);

    ASSERT_EQ(Mp_DecodePosition(buf, MP_PKT_SIZE_POSITION,
                                &mapGroup, &mapNum, &x, &y, &facing), TRUE);
    ASSERT_EQ(mapGroup, 3);
    ASSERT_EQ(mapNum, 12);
    ASSERT_EQ(x, 15);
    ASSERT_EQ(y, 8);
    ASSERT_EQ(facing, DIR_EAST);
}

static void TestPositionBoundaryValues(void)
{
    u8 buf[MP_PKT_SIZE_POSITION];
    u8 mapGroup, mapNum, x, y, facing;

    Mp_EncodePosition(buf, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
    ASSERT_EQ(Mp_DecodePosition(buf, MP_PKT_SIZE_POSITION,
                                &mapGroup, &mapNum, &x, &y, &facing), TRUE);
    ASSERT_EQ(mapGroup, 0xFF);
    ASSERT_EQ(mapNum, 0xFF);
    ASSERT_EQ(x, 0xFF);
    ASSERT_EQ(y, 0xFF);
    ASSERT_EQ(facing, 0xFF);

    // All-zero values.
    Mp_EncodePosition(buf, 0, 0, 0, 0, 0);
    ASSERT_EQ(Mp_DecodePosition(buf, MP_PKT_SIZE_POSITION,
                                &mapGroup, &mapNum, &x, &y, &facing), TRUE);
    ASSERT_EQ(mapGroup, 0);
    ASSERT_EQ(x, 0);
}

static void TestPositionTruncated(void)
{
    u8 buf[MP_PKT_SIZE_POSITION];
    u8 mapGroup, mapNum, x, y, facing;

    Mp_EncodePosition(buf, 1, 2, 3, 4, DIR_NORTH);
    // Supply one byte fewer than required.
    ASSERT_EQ(Mp_DecodePosition(buf, MP_PKT_SIZE_POSITION - 1,
                                &mapGroup, &mapNum, &x, &y, &facing), FALSE);
    ASSERT_EQ(Mp_DecodePosition(buf, 0,
                                &mapGroup, &mapNum, &x, &y, &facing), FALSE);
}

// ---- FLAG_SET encode/decode -----------------------------------------------

static void TestEncodeDecodeFlagSet(void)
{
    u8 buf[MP_PKT_SIZE_FLAG_SET];
    u16 flagId;

    u8 n = Mp_EncodeFlagSet(buf, 0x1234);
    ASSERT_EQ(n, MP_PKT_SIZE_FLAG_SET);
    ASSERT_EQ(buf[0], MP_PKT_FLAG_SET);
    ASSERT_EQ(buf[1], 0x12);
    ASSERT_EQ(buf[2], 0x34);

    ASSERT_EQ(Mp_DecodeFlagSet(buf, MP_PKT_SIZE_FLAG_SET, &flagId), TRUE);
    ASSERT_EQ(flagId, 0x1234);
}

static void TestFlagSetBoundaryValues(void)
{
    u8 buf[MP_PKT_SIZE_FLAG_SET];
    u16 flagId;

    Mp_EncodeFlagSet(buf, 0x0000);
    ASSERT_EQ(Mp_DecodeFlagSet(buf, MP_PKT_SIZE_FLAG_SET, &flagId), TRUE);
    ASSERT_EQ(flagId, 0x0000);

    Mp_EncodeFlagSet(buf, 0xFFFF);
    ASSERT_EQ(Mp_DecodeFlagSet(buf, MP_PKT_SIZE_FLAG_SET, &flagId), TRUE);
    ASSERT_EQ(flagId, 0xFFFF);
}

static void TestFlagSetTruncated(void)
{
    u8 buf[MP_PKT_SIZE_FLAG_SET];
    u16 flagId;
    Mp_EncodeFlagSet(buf, 0xABCD);
    ASSERT_EQ(Mp_DecodeFlagSet(buf, MP_PKT_SIZE_FLAG_SET - 1, &flagId), FALSE);
}

// ---- VAR_SET encode/decode ------------------------------------------------

static void TestEncodeDecodeVarSet(void)
{
    u8 buf[MP_PKT_SIZE_VAR_SET];
    u16 varId, value;

    u8 n = Mp_EncodeVarSet(buf, 0x4001, 0xBEEF);
    ASSERT_EQ(n, MP_PKT_SIZE_VAR_SET);
    ASSERT_EQ(buf[0], MP_PKT_VAR_SET);
    ASSERT_EQ(buf[1], 0x40);
    ASSERT_EQ(buf[2], 0x01);
    ASSERT_EQ(buf[3], 0xBE);
    ASSERT_EQ(buf[4], 0xEF);

    ASSERT_EQ(Mp_DecodeVarSet(buf, MP_PKT_SIZE_VAR_SET, &varId, &value), TRUE);
    ASSERT_EQ(varId, 0x4001);
    ASSERT_EQ(value, 0xBEEF);
}

static void TestVarSetTruncated(void)
{
    u8 buf[MP_PKT_SIZE_VAR_SET];
    u16 varId, value;
    Mp_EncodeVarSet(buf, 0x4001, 0xBEEF);
    ASSERT_EQ(Mp_DecodeVarSet(buf, MP_PKT_SIZE_VAR_SET - 1, &varId, &value), FALSE);
}

// ---- BOSS_READY encode/decode ---------------------------------------------

static void TestEncodeDecodeBossReady(void)
{
    u8 buf[MP_PKT_SIZE_BOSS_READY];
    u8 bossId;

    u8 n = Mp_EncodeBossReady(buf, 7);
    ASSERT_EQ(n, MP_PKT_SIZE_BOSS_READY);
    ASSERT_EQ(buf[0], MP_PKT_BOSS_READY);
    ASSERT_EQ(buf[1], 7);

    ASSERT_EQ(Mp_DecodeBossReady(buf, MP_PKT_SIZE_BOSS_READY, &bossId), TRUE);
    ASSERT_EQ(bossId, 7);
}

static void TestBossReadyTruncated(void)
{
    u8 buf[MP_PKT_SIZE_BOSS_READY];
    u8 bossId;
    Mp_EncodeBossReady(buf, 3);
    ASSERT_EQ(Mp_DecodeBossReady(buf, 1, &bossId), FALSE);
}

// ---- BOSS_CANCEL encode ---------------------------------------------------

static void TestEncodeBossCancel(void)
{
    u8 buf[MP_PKT_SIZE_BOSS_CANCEL];
    u8 n = Mp_EncodeBossCancel(buf);
    ASSERT_EQ(n, MP_PKT_SIZE_BOSS_CANCEL);
    ASSERT_EQ(buf[0], MP_PKT_BOSS_CANCEL);
}

// ---- GENDER encode/decode ------------------------------------------------

static void TestEncodeDecodeGenderMale(void)
{
    u8 buf[MP_PKT_SIZE_GENDER];
    u8 gender;

    u8 n = Mp_EncodeGender(buf, MALE);
    ASSERT_EQ(n, MP_PKT_SIZE_GENDER);
    ASSERT_EQ(buf[0], MP_PKT_GENDER);
    ASSERT_EQ(buf[1], MALE);

    ASSERT_EQ(Mp_DecodeGender(buf, MP_PKT_SIZE_GENDER, &gender), TRUE);
    ASSERT_EQ(gender, MALE);
}

static void TestEncodeDecodeGenderFemale(void)
{
    u8 buf[MP_PKT_SIZE_GENDER];
    u8 gender;

    u8 n = Mp_EncodeGender(buf, FEMALE);
    ASSERT_EQ(n, MP_PKT_SIZE_GENDER);
    ASSERT_EQ(buf[0], MP_PKT_GENDER);
    ASSERT_EQ(buf[1], FEMALE);

    ASSERT_EQ(Mp_DecodeGender(buf, MP_PKT_SIZE_GENDER, &gender), TRUE);
    ASSERT_EQ(gender, FEMALE);
}

static void TestDecodeGenderTruncated(void)
{
    u8 buf[MP_PKT_SIZE_GENDER];
    u8 gender;
    Mp_EncodeGender(buf, MALE);
    ASSERT_EQ(Mp_DecodeGender(buf, 1, &gender), FALSE);
    ASSERT_EQ(Mp_DecodeGender(buf, 0, &gender), FALSE);
}

// ---- SEED_SYNC encode/decode ----------------------------------------------

static void TestEncodeDecodeSeedSync(void)
{
    u8 buf[MP_PKT_SIZE_SEED_SYNC];
    u32 seed;

    u8 n = Mp_EncodeSeedSync(buf, 0xDEADBEEF);
    ASSERT_EQ(n, MP_PKT_SIZE_SEED_SYNC);
    ASSERT_EQ(buf[0], MP_PKT_SEED_SYNC);
    ASSERT_EQ(buf[1], 0xDE);
    ASSERT_EQ(buf[2], 0xAD);
    ASSERT_EQ(buf[3], 0xBE);
    ASSERT_EQ(buf[4], 0xEF);

    ASSERT_EQ(Mp_DecodeSeedSync(buf, MP_PKT_SIZE_SEED_SYNC, &seed), TRUE);
    ASSERT_EQ(seed, 0xDEADBEEFu);
}

static void TestSeedSyncBoundaryValues(void)
{
    u8 buf[MP_PKT_SIZE_SEED_SYNC];
    u32 seed;

    Mp_EncodeSeedSync(buf, 0x00000000);
    ASSERT_EQ(Mp_DecodeSeedSync(buf, MP_PKT_SIZE_SEED_SYNC, &seed), TRUE);
    ASSERT_EQ(seed, 0x00000000u);

    Mp_EncodeSeedSync(buf, 0xFFFFFFFF);
    ASSERT_EQ(Mp_DecodeSeedSync(buf, MP_PKT_SIZE_SEED_SYNC, &seed), TRUE);
    ASSERT_EQ(seed, 0xFFFFFFFFu);
}

static void TestSeedSyncTruncated(void)
{
    u8 buf[MP_PKT_SIZE_SEED_SYNC];
    u32 seed;
    Mp_EncodeSeedSync(buf, 0x12345678);
    ASSERT_EQ(Mp_DecodeSeedSync(buf, MP_PKT_SIZE_SEED_SYNC - 1, &seed), FALSE);
    ASSERT_EQ(Mp_DecodeSeedSync(buf, 0, &seed), FALSE);
}

// ---- FULL_SYNC encode/decode ----------------------------------------------

static void TestEncodeDecodeFullSync(void)
{
    const u8 payload[] = { 0x01, 0x02, 0x03, 0xAB, 0xCD };
    u8 buf[3 + sizeof(payload)];
    const u8 *dataOut;
    u16 dataLen;
    u16 i;

    u16 n = Mp_EncodeFullSync(buf, payload, sizeof(payload));
    ASSERT_EQ(n, (u16)(3 + sizeof(payload)));
    ASSERT_EQ(buf[0], MP_PKT_FULL_SYNC);
    ASSERT_EQ(buf[1], 0x00); // high byte of len
    ASSERT_EQ(buf[2], (u8)sizeof(payload));

    ASSERT_EQ(Mp_DecodeFullSync(buf, (u16)(3 + sizeof(payload)), &dataOut, &dataLen), TRUE);
    ASSERT_EQ(dataLen, (u16)sizeof(payload));
    for (i = 0; i < sizeof(payload); i++)
        ASSERT_EQ(dataOut[i], payload[i]);
}

static void TestFullSyncEmptyPayload(void)
{
    u8 buf[3];
    const u8 *dataOut;
    u16 dataLen;

    u16 n = Mp_EncodeFullSync(buf, NULL, 0);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(buf[0], MP_PKT_FULL_SYNC);
    ASSERT_EQ(buf[1], 0);
    ASSERT_EQ(buf[2], 0);

    ASSERT_EQ(Mp_DecodeFullSync(buf, 3, &dataOut, &dataLen), TRUE);
    ASSERT_EQ(dataLen, 0);
}

static void TestFullSyncTruncatedHeader(void)
{
    u8 buf[3] = { MP_PKT_FULL_SYNC, 0, 5 }; // claims 5 bytes but only 3 total
    const u8 *dataOut;
    u16 dataLen;
    // Total available is 3, declared payload is 5 → need 8 total → reject.
    ASSERT_EQ(Mp_DecodeFullSync(buf, 3, &dataOut, &dataLen), FALSE);
    // Buffer too short to even read header.
    ASSERT_EQ(Mp_DecodeFullSync(buf, 2, &dataOut, &dataLen), FALSE);
}

static void TestFullSyncLargerPayload(void)
{
    u8 payload[100];
    u8 buf[103];
    const u8 *dataOut;
    u16 dataLen;
    u16 i;

    for (i = 0; i < 100; i++) payload[i] = (u8)i;
    Mp_EncodeFullSync(buf, payload, 100);
    ASSERT_EQ(buf[1], 0);    // high byte of 100
    ASSERT_EQ(buf[2], 100);  // low byte

    ASSERT_EQ(Mp_DecodeFullSync(buf, 103, &dataOut, &dataLen), TRUE);
    ASSERT_EQ(dataLen, 100);
    for (i = 0; i < 100; i++)
        ASSERT_EQ(dataOut[i], (u8)i);
}

// ---- Ring buffer + send/recv integration ----------------------------------

static void TestSendPositionWritesToRing(void)
{
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    save.location.mapGroup = 2;
    save.location.mapNum   = 9;
    save.location.x        = 5;
    save.location.y        = 7;
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    // Manually set connected state so Multiplayer_SendPosition runs.
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    Multiplayer_SendPosition();

    ASSERT_EQ(Mp_Available(&gMpSendRing), MP_PKT_SIZE_POSITION);
    u8 byte;
    Mp_Pop(&gMpSendRing, &byte);
    ASSERT_EQ(byte, MP_PKT_POSITION);
    Mp_Pop(&gMpSendRing, &byte);
    ASSERT_EQ(byte, 2); // mapGroup
    Mp_Pop(&gMpSendRing, &byte);
    ASSERT_EQ(byte, 9); // mapNum
}

static void TestRecvPacketDispatchesGhostPosition(void)
{
    // Write a POSITION packet into the recv ring and call Multiplayer_Update.
    u8 pkt[MP_PKT_SIZE_POSITION];
    u8 i;
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    save.location.mapGroup = 0;
    save.location.mapNum   = 5;
    gSaveBlock1Ptr = &save;

    Mp_EncodePosition(pkt, 0, 5, 10, 11, DIR_NORTH);

    Multiplayer_Init();
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    for (i = 0; i < MP_PKT_SIZE_POSITION; i++)
        Mp_Push(&gMpRecvRing, pkt[i]);

    // Multiplayer_Update reads the ring and routes position to ghost state.
    Multiplayer_Update();

    ASSERT_EQ(gMultiplayerState.partnerMapGroup, 0);
    ASSERT_EQ(gMultiplayerState.partnerMapNum,   5);
    ASSERT_EQ(gMultiplayerState.targetX,         10);
    ASSERT_EQ(gMultiplayerState.targetY,         11);
    ASSERT_EQ(gMultiplayerState.targetFacing,    DIR_NORTH);
}

static void TestRecvSeedSyncUpdatesSeed(void)
{
    u8 pkt[MP_PKT_SIZE_SEED_SYNC];
    u8 i;

    Mp_EncodeSeedSync(pkt, 0xCAFEBABE);
    Multiplayer_Init();
    ASSERT_EQ(gCoopSettings.encounterSeed, 0u);

    for (i = 0; i < MP_PKT_SIZE_SEED_SYNC; i++)
        Mp_Push(&gMpRecvRing, pkt[i]);

    // processOneRecvPacket runs inside Multiplayer_Update
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;
    Multiplayer_Update();

    ASSERT_EQ(gCoopSettings.encounterSeed, 0xCAFEBABEu);
}

static void TestRecvUnknownTypeDrainsRing(void)
{
    u8 i;
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    // Push an unknown type byte followed by junk.
    Mp_Push(&gMpRecvRing, 0xEE);
    for (i = 0; i < 5; i++) Mp_Push(&gMpRecvRing, 0xFF);

    Multiplayer_Update();
    ASSERT_EQ(Mp_Available(&gMpRecvRing), 0);
}

// ---- PARTY_SYNC wire mon ---------------------------------------------------

static void TestPartyMonWireRoundTrip(void)
{
    struct MpWirePartyMon in, out;
    u8 buf[MP_PKT_PARTY_SYNC_MON_SIZE];
    u8 j;

    memset(&in, 0, sizeof(in));
    memset(&out, 0xEE, sizeof(out));
    in.species     = 0x0004;       // Charmander
    in.heldItem    = 0x00AA;
    in.level       = 5;
    in.abilityNum  = 1;
    in.personality = 0xA551BB52u;
    in.otId        = 0xDEADBEEFu;
    in.moves[0] = 10; in.moves[1] = 45; in.moves[2] = 0x0152; in.moves[3] = 0;
    in.pp[0] = 35; in.pp[1] = 40; in.pp[2] = 15; in.pp[3] = 0;
    in.hp     = 19;
    in.maxHP  = 20;
    in.atk    = 11;
    in.def    = 10;
    in.speed  = 13;
    in.spAtk  = 12;
    in.spDef  = 9;
    in.status = 0x00000040u;       // e.g. 3 turns sleep
    in.friendship = 70;
    in.gender     = 0;
    in.language   = 2;
    memcpy(in.nickname, "CHARMANDE\xFF", MP_WIRE_NICK_LEN);

    ASSERT_EQ(Mp_EncodePartyMon(buf, &in), MP_PKT_PARTY_SYNC_MON_SIZE);
    ASSERT_EQ(Mp_DecodePartyMon(buf, &out), TRUE);

    ASSERT_EQ(out.species, in.species);
    ASSERT_EQ(out.heldItem, in.heldItem);
    ASSERT_EQ(out.level, in.level);
    ASSERT_EQ(out.abilityNum, in.abilityNum);
    ASSERT_EQ(out.personality, in.personality);
    ASSERT_EQ(out.otId, in.otId);
    for (j = 0; j < 4; j++)
    {
        ASSERT_EQ(out.moves[j], in.moves[j]);
        ASSERT_EQ(out.pp[j], in.pp[j]);
    }
    // Regression guard for the absent-battler bug: hp/maxHP and every battle
    // stat must survive the wire — a reconstruction with hp 0 makes
    // TryDoEventsBeforeFirstTurn flag the partner battler absent.
    ASSERT_EQ(out.hp, in.hp);
    ASSERT_EQ(out.maxHP, in.maxHP);
    ASSERT_EQ(out.atk, in.atk);
    ASSERT_EQ(out.def, in.def);
    ASSERT_EQ(out.speed, in.speed);
    ASSERT_EQ(out.spAtk, in.spAtk);
    ASSERT_EQ(out.spDef, in.spDef);
    ASSERT_EQ(out.status, in.status);
    ASSERT_EQ(out.friendship, in.friendship);
    ASSERT_EQ(out.gender, in.gender);
    ASSERT_EQ(out.language, in.language);
    ASSERT_EQ(memcmp(out.nickname, in.nickname, MP_WIRE_NICK_LEN), 0);
}

static void TestSendPartySyncPacketSize(void)
{
    // 2 mons → header(2) + 2*58 + 4 (trailing battle RNG seed) in the send ring.
    Multiplayer_Init();
    gPlayerPartyCount = 2;
    Multiplayer_SendPartySync();
    ASSERT_EQ(Mp_Available(&gMpSendRing),
              MP_PKT_PARTY_SYNC_HDR + 2 * MP_PKT_PARTY_SYNC_MON_SIZE
              + MP_PKT_PARTY_SYNC_SEED_SIZE);
    gPlayerPartyCount = 0;
}

static void TestPartySyncSeedHostSendsGuestZeroes(void)
{
    u8 out = 0;
    u16 i;
    u32 tail;

    // Host with a minted seed → trailing 4 bytes carry it (big-endian).
    Multiplayer_Init();
    gPlayerPartyCount = 1;
    gMultiplayerState.role = MP_ROLE_HOST;
    gMultiplayerState.coopBattleSeed = 0xCAFEF00Du;
    Multiplayer_SendPartySync();
    for (i = 0; i < MP_PKT_PARTY_SYNC_HDR + MP_PKT_PARTY_SYNC_MON_SIZE; i++)
        Mp_Pop(&gMpSendRing, &out); // skip header + mon
    tail = 0;
    for (i = 0; i < MP_PKT_PARTY_SYNC_SEED_SIZE; i++)
    {
        Mp_Pop(&gMpSendRing, &out);
        tail = (tail << 8) | out;
    }
    ASSERT_EQ(tail, 0xCAFEF00Du);

    // Guest always transmits 0 even if it has adopted a seed, so the host can
    // never be talked onto a stream it didn't mint.
    Multiplayer_Init();
    gPlayerPartyCount = 1;
    gMultiplayerState.role = MP_ROLE_GUEST;
    gMultiplayerState.coopBattleSeed = 0xCAFEF00Du;
    Multiplayer_SendPartySync();
    for (i = 0; i < MP_PKT_PARTY_SYNC_HDR + MP_PKT_PARTY_SYNC_MON_SIZE; i++)
        Mp_Pop(&gMpSendRing, &out);
    tail = 0;
    for (i = 0; i < MP_PKT_PARTY_SYNC_SEED_SIZE; i++)
    {
        Mp_Pop(&gMpSendRing, &out);
        tail = (tail << 8) | out;
    }
    ASSERT_EQ(tail, 0u);
    gPlayerPartyCount = 0;
}

static void TestRecvPartySyncAdoptsNonzeroSeed(void)
{
    u8 pkt[MP_PKT_PARTY_SYNC_HDR + MP_PKT_PARTY_SYNC_MON_SIZE + MP_PKT_PARTY_SYNC_SEED_SIZE];
    u16 i;
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = MP_PKT_PARTY_SYNC;
    pkt[1] = 1; // one mon (all-zero wire mon decodes fine; stubs ignore it)
    pkt[sizeof(pkt) - 4] = 0xDE;
    pkt[sizeof(pkt) - 3] = 0xAD;
    pkt[sizeof(pkt) - 2] = 0xBE;
    pkt[sizeof(pkt) - 1] = 0xEF;

    Multiplayer_Init();
    for (i = 0; i < sizeof(pkt); i++)
        Mp_Push(&gMpRecvRing, pkt[i]);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.coopBattleSeed, 0xDEADBEEFu);
    ASSERT_EQ(gMultiplayerState.partnerPartySelectDone, TRUE);

    // A zero seed (guest's packet) must NOT overwrite an adopted one.
    gMultiplayerState.partnerPartySelectDone = FALSE;
    memset(&pkt[sizeof(pkt) - 4], 0, 4);
    for (i = 0; i < sizeof(pkt); i++)
        Mp_Push(&gMpRecvRing, pkt[i]);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.coopBattleSeed, 0xDEADBEEFu);
    ASSERT_EQ(gMultiplayerState.partnerPartySelectDone, TRUE);
}

// ---- BATTLE_TURN sequencing -------------------------------------------------

static void TestSendBattleTurnAssignsSeq(void)
{
    u8 b[MP_PKT_SIZE_BATTLE_TURN];
    u8 i;

    Multiplayer_Init();
    Multiplayer_SendBattleTurn(2, 1, 0);
    ASSERT_EQ(gMultiplayerState.battleTurnSeqOut, 1);
    ASSERT_EQ(gMultiplayerState.battleTurnSent, TRUE);
    ASSERT_EQ(Mp_Available(&gMpSendRing), MP_PKT_SIZE_BATTLE_TURN);
    for (i = 0; i < MP_PKT_SIZE_BATTLE_TURN; i++)
        Mp_Pop(&gMpSendRing, &b[i]);
    ASSERT_EQ(b[0], MP_PKT_BATTLE_TURN);
    ASSERT_EQ(b[1], 1); // seq
    ASSERT_EQ(b[2], 2); // moveSlot
    ASSERT_EQ(b[3], 1); // target
    ASSERT_EQ(b[4], 0); // flags

    // Second logical turn gets the next seq.
    Multiplayer_SendBattleTurn(0, 3, 1);
    ASSERT_EQ(gMultiplayerState.battleTurnSeqOut, 2);
    for (i = 0; i < MP_PKT_SIZE_BATTLE_TURN; i++)
        Mp_Pop(&gMpSendRing, &b[i]);
    ASSERT_EQ(b[1], 2);
    ASSERT_EQ(b[2], 0);
}

static void TestResendBattleTurnKeepsSeq(void)
{
    u8 b[MP_PKT_SIZE_BATTLE_TURN];
    u8 i, out;

    Multiplayer_Init();
    Multiplayer_SendBattleTurn(1, 0, 0);
    while (Mp_Pop(&gMpSendRing, &out)) {} // drain the original send

    // Reconnect-path replay: same seq, same payload, no bump.
    Multiplayer_ResendBattleTurn();
    ASSERT_EQ(gMultiplayerState.battleTurnSeqOut, 1);
    ASSERT_EQ(Mp_Available(&gMpSendRing), MP_PKT_SIZE_BATTLE_TURN);
    for (i = 0; i < MP_PKT_SIZE_BATTLE_TURN; i++)
        Mp_Pop(&gMpSendRing, &b[i]);
    ASSERT_EQ(b[1], 1);
    ASSERT_EQ(b[2], 1);

    // No cached turn → no packet.
    gMultiplayerState.battleTurnSent = FALSE;
    Multiplayer_ResendBattleTurn();
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);
}

static void TestHandleBattleTurnDedupAndOrdering(void)
{
    Multiplayer_Init();

    // seq 0 is "no turn" on the wire — never applied.
    Multiplayer_HandleBattleTurn(0, 1, 1, 0);
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, FALSE);

    // First real turn applies; ally target 0 mirrors to 2.
    Multiplayer_HandleBattleTurn(1, 2, 0, 0);
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, TRUE);
    ASSERT_EQ(gMultiplayerState.battleTurnMoveSlot, 2);
    ASSERT_EQ(gMultiplayerState.battleTurnTarget, 2);
    ASSERT_EQ(gMultiplayerState.battleTurnSeqApplied, 1);

    // Exact duplicate (beacon re-carry / reconnect resend) is a no-op.
    gMultiplayerState.battleTurnReceived = FALSE;
    Multiplayer_HandleBattleTurn(1, 2, 0, 0);
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, FALSE);
    ASSERT_EQ(gMultiplayerState.battleTurnSeqApplied, 1);

    // Newer seq applies (gaps fine); ally target 2 mirrors to 0.
    Multiplayer_HandleBattleTurn(3, 1, 2, 0);
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, TRUE);
    ASSERT_EQ(gMultiplayerState.battleTurnTarget, 0);
    ASSERT_EQ(gMultiplayerState.battleTurnSeqApplied, 3);

    // Stale (reordered) older turn is rejected and doesn't regress seq.
    gMultiplayerState.battleTurnReceived = FALSE;
    Multiplayer_HandleBattleTurn(2, 0, 1, 0);
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, FALSE);
    ASSERT_EQ(gMultiplayerState.battleTurnSeqApplied, 3);

    // Wraparound: applied=255, next turn seq=1 (sender skips 0) is "newer".
    gMultiplayerState.battleTurnSeqApplied = 255;
    Multiplayer_HandleBattleTurn(1, 0, 1, 0);
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, TRUE);
    ASSERT_EQ(gMultiplayerState.battleTurnSeqApplied, 1);
}

static void TestSetupCoopBattleResetsTurnStateAndSeedsRng(void)
{
    Multiplayer_Init();
    gMultiplayerState.battleTurnSeqOut     = 9;
    gMultiplayerState.battleTurnSeqApplied = 7;
    gMultiplayerState.battleTurnSent       = TRUE;
    gMultiplayerState.battleTurnReceived   = TRUE;
    gMultiplayerState.coopBattleSeed       = 0xCAFEF00Du;

    Multiplayer_SetupCoopBattle();
    ASSERT_EQ(gMultiplayerState.battleTurnSeqOut, 0);
    ASSERT_EQ(gMultiplayerState.battleTurnSeqApplied, 0);
    ASSERT_EQ(gMultiplayerState.battleTurnSent, FALSE);
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, FALSE);
    ASSERT_EQ(gMultiplayerState.coopRngState, 0xCAFEF00Du);
}

// ---- STATE_BEACON battle-turn repair ----------------------------------------

static void PushBeacon(u8 turnSeq, u8 moveSlot, u8 target, u8 flags)
{
    u8 pkt[MP_PKT_SIZE_STATE_BEACON];
    u8 i;
    pkt[0] = MP_PKT_STATE_BEACON;
    pkt[1] = 0; // gender MALE
    pkt[2] = 0; // starter hi
    pkt[3] = 0; // starter lo (0 = no pick; ignored)
    pkt[4] = 0; // no boss readiness
    pkt[5] = turnSeq;
    pkt[6] = moveSlot;
    pkt[7] = target;
    pkt[8] = flags;
    for (i = 0; i < MP_PKT_SIZE_STATE_BEACON; i++)
        Mp_Push(&gMpRecvRing, pkt[i]);
}

static void TestBeaconRepairsDroppedBattleTurn(void)
{
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    gBattleTypeFlags = BATTLE_TYPE_COOP;

    // The direct BATTLE_TURN was "dropped"; the partner's beacon re-carries it.
    PushBeacon(1, 2, 1, 0);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, TRUE);
    ASSERT_EQ(gMultiplayerState.battleTurnMoveSlot, 2);
    ASSERT_EQ(gMultiplayerState.battleTurnTarget, 1);
    ASSERT_EQ(gMultiplayerState.battleTurnSeqApplied, 1);

    // The beacon repeats every interval — repeats must not re-arm the flag
    // after the controller consumed the turn.
    gMultiplayerState.battleTurnReceived = FALSE;
    PushBeacon(1, 2, 1, 0);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, FALSE);

    // Next turn's beacon (newer seq) applies.
    PushBeacon(2, 3, 0, 0);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, TRUE);
    ASSERT_EQ(gMultiplayerState.battleTurnMoveSlot, 3);
    ASSERT_EQ(gMultiplayerState.battleTurnTarget, 2); // ally 0 mirrored
    ASSERT_EQ(gMultiplayerState.battleTurnSeqApplied, 2);

    // Outside a coop battle the turn payload is ignored — an overworld
    // instance must never buffer a stale turn for a future battle.
    gBattleTypeFlags = 0;
    gMultiplayerState.battleTurnReceived = FALSE;
    PushBeacon(3, 1, 1, 0);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, FALSE);
    ASSERT_EQ(gMultiplayerState.battleTurnSeqApplied, 2);
}

static void TestBeaconSenderCarriesCachedTurnOnlyInCoopBattle(void)
{
    u8 pkt[MP_PKT_SIZE_STATE_BEACON];
    u8 i, frame;
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    // In a coop battle with a cached turn: beacon bytes 5-8 carry it.
    Multiplayer_Init();
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    gBattleTypeFlags = BATTLE_TYPE_COOP;
    Multiplayer_SendBattleTurn(2, 1, 1);
    { u8 out; while (Mp_Pop(&gMpSendRing, &out)) {} } // drain the turn packet

    for (frame = 0; frame < MP_BEACON_INTERVAL_FRAMES; frame++)
        Multiplayer_Update();
    // Skip anything that isn't the beacon (position packets etc.).
    for (;;)
    {
        ASSERT_EQ(Mp_Pop(&gMpSendRing, &pkt[0]), TRUE);
        if (pkt[0] == MP_PKT_STATE_BEACON)
            break;
    }
    for (i = 1; i < MP_PKT_SIZE_STATE_BEACON; i++)
        Mp_Pop(&gMpSendRing, &pkt[i]);
    ASSERT_EQ(pkt[5], 1); // seq
    ASSERT_EQ(pkt[6], 2); // moveSlot
    ASSERT_EQ(pkt[7], 1); // target
    ASSERT_EQ(pkt[8], 1); // flags

    // Outside battle the same cached turn must NOT ride the beacon.
    gBattleTypeFlags = 0;
    { u8 out; while (Mp_Pop(&gMpSendRing, &out)) {} }
    for (frame = 0; frame < MP_BEACON_INTERVAL_FRAMES; frame++)
        Multiplayer_Update();
    for (;;)
    {
        ASSERT_EQ(Mp_Pop(&gMpSendRing, &pkt[0]), TRUE);
        if (pkt[0] == MP_PKT_STATE_BEACON)
            break;
    }
    for (i = 1; i < MP_PKT_SIZE_STATE_BEACON; i++)
        Mp_Pop(&gMpSendRing, &pkt[i]);
    ASSERT_EQ(pkt[5], 0);
    ASSERT_EQ(pkt[6], 0);
    ASSERT_EQ(pkt[7], 0);
    ASSERT_EQ(pkt[8], 0);
}

static void TestBattleTickPumpsBeaconAndRecv(void)
{
    u8 pkt[MP_PKT_SIZE_STATE_BEACON];
    u8 i, frame;
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    gBattleTypeFlags = BATTLE_TYPE_COOP;

    // Disconnected: inert — nothing written.
    for (frame = 0; frame < 2 * MP_BEACON_INTERVAL_FRAMES; frame++)
        Multiplayer_BattleTick();
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);

    // Connected, in a coop battle, with a cached turn: the beacon (with the
    // turn payload) must flow from BattleTick alone — Multiplayer_Update
    // never runs during a battle (BattleTick is hooked in BattleMainCB2),
    // which is exactly the gap that left the repair channel dark.
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    Multiplayer_SendBattleTurn(1, 1, 0);
    { u8 out; while (Mp_Pop(&gMpSendRing, &out)) {} } // drain the turn packet
    for (frame = 0; frame < MP_BEACON_INTERVAL_FRAMES; frame++)
        Multiplayer_BattleTick();
    for (;;)
    {
        ASSERT_EQ(Mp_Pop(&gMpSendRing, &pkt[0]), TRUE);
        if (pkt[0] == MP_PKT_STATE_BEACON)
            break;
    }
    for (i = 1; i < MP_PKT_SIZE_STATE_BEACON; i++)
        Mp_Pop(&gMpSendRing, &pkt[i]);
    ASSERT_EQ(pkt[5], 1); // cached turn seq
    ASSERT_EQ(pkt[6], 1); // moveSlot

    // BattleTick also drains the recv ring (the partner controller only
    // polls inside its ChooseMove window).
    PushBeacon(2, 3, 1, 0);
    Multiplayer_BattleTick();
    ASSERT_EQ(gMultiplayerState.battleTurnReceived, TRUE);
    ASSERT_EQ(gMultiplayerState.battleTurnMoveSlot, 3);

    gBattleTypeFlags = 0;
}

// ---- STATE_BEACON field-trainer lock repair (Bug #18a) ----------------------

// Push a state beacon carrying a field-trainer lock in the otherwise-idle
// battle-turn bytes (5/6/7) + the present bit in byte 8.  When present is
// FALSE this is a "no lock" beacon (repairs a dropped TRAINER_FREE).
static void PushBeaconBusyTrainer(u8 localId, u8 mapGroup, u8 mapNum, bool8 present)
{
    u8 pkt[MP_PKT_SIZE_STATE_BEACON];
    u8 i;
    pkt[0] = MP_PKT_STATE_BEACON;
    pkt[1] = 0; // gender MALE, no party ack
    pkt[2] = 0; // starter hi
    pkt[3] = 0; // starter lo
    pkt[4] = 0; // no boss readiness
    pkt[5] = localId;
    pkt[6] = mapGroup;
    pkt[7] = mapNum;
    pkt[8] = present ? MP_BEACON_BUSYTRAINER_BIT : 0;
    for (i = 0; i < MP_PKT_SIZE_STATE_BEACON; i++)
        Mp_Push(&gMpRecvRing, pkt[i]);
}

static void TestBeaconRepairsDroppedTrainerBusy(void)
{
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    gBattleTypeFlags = 0; // overworld
    ASSERT_EQ(gMultiplayerState.partnerHasBusyTrainer, FALSE);

    // The direct MP_PKT_TRAINER_BUSY was "dropped"; the partner's beacon
    // re-carries the lock so it converges within one interval (under-lock fix).
    PushBeaconBusyTrainer(5, 3, 16, TRUE);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.partnerHasBusyTrainer, TRUE);
    ASSERT_EQ(gMultiplayerState.partnerBusyTrainerLocalId, 5);
    ASSERT_EQ(gMultiplayerState.partnerBusyTrainerMapGroup, 3);
    ASSERT_EQ(gMultiplayerState.partnerBusyTrainerMapNum, 16);
}

static void TestBeaconRepairsDroppedTrainerFree(void)
{
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    gBattleTypeFlags = 0; // overworld

    // Partner is locked (e.g. from an earlier BUSY), then their battle ended
    // but the MP_PKT_TRAINER_FREE was "dropped".  A clear beacon (present bit
    // off) must release the lock — otherwise B is permanently told "Buzz off!"
    // (the serious over-lock bug).
    gMultiplayerState.partnerHasBusyTrainer      = TRUE;
    gMultiplayerState.partnerBusyTrainerLocalId  = 5;
    gMultiplayerState.partnerBusyTrainerMapGroup = 3;
    gMultiplayerState.partnerBusyTrainerMapNum   = 16;

    PushBeaconBusyTrainer(0, 0, 0, FALSE);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.partnerHasBusyTrainer, FALSE);
}

static void TestSendTrainerBusyStoresCoords(void)
{
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    gMultiplayerState.connState = MP_STATE_CONNECTED;

    // Sending a one-shot BUSY must also cache the trainer identity so the
    // beacon can re-carry it on the loss-recovery path.
    Multiplayer_SendTrainerBusy(7, 3, 16);
    ASSERT_EQ(gMultiplayerState.sentBusyTrainer, TRUE);
    ASSERT_EQ(gMultiplayerState.sentBusyTrainerLocalId, 7);
    ASSERT_EQ(gMultiplayerState.sentBusyTrainerMapGroup, 3);
    ASSERT_EQ(gMultiplayerState.sentBusyTrainerMapNum, 16);
}

static void TestBeaconSenderCarriesBusyTrainerOutsideCoopBattle(void)
{
    u8 pkt[MP_PKT_SIZE_STATE_BEACON];
    u8 i, frame;
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    gBattleTypeFlags = 0; // overworld (not a coop battle)

    // The lock is held from spotting until Multiplayer_OnBattleEnd, independent
    // of gMain.inBattle: SendTrainerBusy fires at spotting (field intro, inBattle
    // still FALSE), so the lock must survive overworld Update frames where
    // inBattle is FALSE.  inBattle is left FALSE here precisely to prove the lock
    // is no longer cleared by an Update-frame poll (the pre-battle-clear bug
    // fixed 2026-06-18).
    Multiplayer_SendTrainerBusy(7, 3, 16);
    { u8 out; while (Mp_Pop(&gMpSendRing, &out)) {} } // drain the one-shot BUSY

    for (frame = 0; frame < MP_BEACON_INTERVAL_FRAMES; frame++)
        Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.sentBusyTrainer, TRUE); // not cleared by Update
    for (;;)
    {
        ASSERT_EQ(Mp_Pop(&gMpSendRing, &pkt[0]), TRUE);
        if (pkt[0] == MP_PKT_STATE_BEACON)
            break;
    }
    for (i = 1; i < MP_PKT_SIZE_STATE_BEACON; i++)
        Mp_Pop(&gMpSendRing, &pkt[i]);
    ASSERT_EQ(pkt[5], 7);  // localId
    ASSERT_EQ(pkt[6], 3);  // mapGroup
    ASSERT_EQ(pkt[7], 16); // mapNum
    ASSERT_EQ(pkt[8], MP_BEACON_BUSYTRAINER_BIT);

    // Battle ends — Multiplayer_OnBattleEnd releases the lock (clears the flag
    // and emits an explicit MP_PKT_TRAINER_FREE), after which the beacon's turn
    // bytes go back to zero (a present-bit-0 beacon, which repairs a dropped
    // TRAINER_FREE on the partner's side).
    { u8 out; while (Mp_Pop(&gMpSendRing, &out)) {} }
    Multiplayer_OnBattleEnd();
    ASSERT_EQ(gMultiplayerState.sentBusyTrainer, FALSE);
    {
        bool32 sawFree = FALSE;
        u8 b;
        while (Mp_Pop(&gMpSendRing, &b))
        {
            if (b == MP_PKT_TRAINER_FREE)
                sawFree = TRUE;
        }
        ASSERT_EQ(sawFree, TRUE); // explicit FREE emitted at battle end
    }
    for (frame = 0; frame < MP_BEACON_INTERVAL_FRAMES; frame++)
        Multiplayer_Update();
    for (;;)
    {
        ASSERT_EQ(Mp_Pop(&gMpSendRing, &pkt[0]), TRUE);
        if (pkt[0] == MP_PKT_STATE_BEACON)
            break;
    }
    for (i = 1; i < MP_PKT_SIZE_STATE_BEACON; i++)
        Mp_Pop(&gMpSendRing, &pkt[i]);
    ASSERT_EQ(pkt[5], 0);
    ASSERT_EQ(pkt[6], 0);
    ASSERT_EQ(pkt[7], 0);
    ASSERT_EQ(pkt[8], 0);
}

static void TestBeaconBusyIgnoredDuringCoopBattle(void)
{
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    gBattleTypeFlags = BATTLE_TYPE_COOP;

    // In a coop battle beacon bytes 5-8 are turn data, NOT a trainer lock —
    // and neither side can hold an overworld lock then.  A beacon whose byte 8
    // happens to have bit 0 set (a turn flag) must never be misread as a lock.
    gMultiplayerState.partnerHasBusyTrainer = FALSE;
    PushBeaconBusyTrainer(5, 3, 16, TRUE);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.partnerHasBusyTrainer, FALSE);

    gBattleTypeFlags = 0;
}

// ---- Trainer-approach cosmetic mirror (Bug #18b, MP_PKT_TRAINER_APPROACH) ----

// Recording stub for the cosmetic player (defined in stubs.c; the real impl is
// in trainer_see.c, which is not part of the native suite).
extern u8 gGhostApproachCalls;
extern u8 gGhostApproachLocalId;
extern u8 gGhostApproachDir;
extern u8 gGhostApproachDist;

static void PushTrainerApproach(u8 localId, u8 mapGroup, u8 mapNum, u8 dir, u8 dist)
{
    Mp_Push(&gMpRecvRing, MP_PKT_TRAINER_APPROACH);
    Mp_Push(&gMpRecvRing, localId);
    Mp_Push(&gMpRecvRing, mapGroup);
    Mp_Push(&gMpRecvRing, mapNum);
    Mp_Push(&gMpRecvRing, dir);
    Mp_Push(&gMpRecvRing, dist);
}

static void TestSendTrainerApproachEncodes(void)
{
    u8 pkt[MP_PKT_SIZE_TRAINER_APPROACH];
    u8 i;
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    Multiplayer_SendTrainerApproach(7, 3, 12, 2, 4);
    for (i = 0; i < MP_PKT_SIZE_TRAINER_APPROACH; i++)
        ASSERT_EQ(Mp_Pop(&gMpSendRing, &pkt[i]), TRUE);
    ASSERT_EQ(pkt[0], MP_PKT_TRAINER_APPROACH);
    ASSERT_EQ(pkt[1], 7);  // localId
    ASSERT_EQ(pkt[2], 3);  // mapGroup
    ASSERT_EQ(pkt[3], 12); // mapNum
    ASSERT_EQ(pkt[4], 2);  // direction
    ASSERT_EQ(pkt[5], 4);  // distance (walk count)
}

static void TestTrainerApproachFiresOnMatchingMap(void)
{
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    save.location.mapGroup = 3;
    save.location.mapNum   = 12;
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    gGhostApproachCalls = 0;

    PushTrainerApproach(7, 3, 12, 2, 4);
    Multiplayer_Update();
    ASSERT_EQ(gGhostApproachCalls, 1);
    ASSERT_EQ(gGhostApproachLocalId, 7);
    ASSERT_EQ(gGhostApproachDir, 2);
    ASSERT_EQ(gGhostApproachDist, 4);
}

static void TestTrainerApproachIgnoredOnOtherMap(void)
{
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    save.location.mapGroup = 3;
    save.location.mapNum   = 99; // partner is on a different map
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    gGhostApproachCalls = 0;

    PushTrainerApproach(7, 3, 12, 2, 4);
    Multiplayer_Update();
    ASSERT_EQ(gGhostApproachCalls, 0); // cosmetic mirror suppressed off-map
}

// ---- Party-sync mutual handshake (asymmetric-loss deadlock fix) -------------

// Push a state beacon whose gender byte (pkt[1]) optionally carries the
// party-sync ack bit.  Battle-turn payload (bytes 5-8) left zero.
static void PushBeaconGenderAck(u8 gender, bool8 partyAck)
{
    u8 pkt[MP_PKT_SIZE_STATE_BEACON];
    u8 i;
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = MP_PKT_STATE_BEACON;
    pkt[1] = (u8)(gender | (partyAck ? MP_BEACON_PARTYACK_BIT : 0));
    for (i = 0; i < MP_PKT_SIZE_STATE_BEACON; i++)
        Mp_Push(&gMpRecvRing, pkt[i]);
}

static void TestBeaconPartyAckSetsPartnerGotMyParty(void)
{
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    gMultiplayerState.connState = MP_STATE_CONNECTED;

    // Plain beacon: gender male (0), no ack.
    PushBeaconGenderAck(0, FALSE);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.partnerGotMyParty, FALSE);
    ASSERT_EQ(gMultiplayerState.partnerGender, 0);

    // Beacon with the ack bit AND a gender change to female (1): the ack
    // latches AND the bit is stripped before decoding — otherwise
    // HandleRemoteGender would reject 0x81 and the gender would stay male.
    PushBeaconGenderAck(1, TRUE);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.partnerGotMyParty, TRUE);
    ASSERT_EQ(gMultiplayerState.partnerGender, 1);
}

static void TestPartySyncHandshakeNeedsMutualAck(void)
{
    struct SaveBlock1 save;
    u8 out;
    u8 i;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    gMultiplayerState.connState = MP_STATE_CONNECTED;

    // Neither flag set: stay in the wait and resend our party every 60 frames.
    while (Mp_Pop(&gMpSendRing, &out)) {}
    for (i = 0; i < 59; i++)
        ASSERT_EQ(Multiplayer_NativePollPartySync(), FALSE);
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);       // no resend before frame 60
    ASSERT_EQ(Multiplayer_NativePollPartySync(), FALSE);
    ASSERT_NE(Mp_Available(&gMpSendRing), 0);        // party resent at frame 60

    // Received the partner's party but no ack yet: STILL waiting.  Exiting on
    // local receipt alone was the asymmetric-loss deadlock.
    gMultiplayerState.gotPartnerParty = TRUE;
    ASSERT_EQ(Multiplayer_NativePollPartySync(), FALSE);

    // Partner's beacon acked our party too: both sides now hold each other's
    // parties, so the wait completes.
    gMultiplayerState.partnerGotMyParty = TRUE;
    ASSERT_EQ(Multiplayer_NativePollPartySync(), TRUE);

    // Solo / disconnected always completes immediately (Init leaves us
    // disconnected), preserving the SetupCoopBattle solo-clone fallback.
    Multiplayer_Init();
    ASSERT_EQ(Multiplayer_NativePollPartySync(), TRUE);
}

static void TestMidBattlePartySyncIgnored(void)
{
    u8 pkt[MP_PKT_PARTY_SYNC_HDR + MP_PKT_PARTY_SYNC_MON_SIZE + MP_PKT_PARTY_SYNC_SEED_SIZE];
    u16 i;
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = MP_PKT_PARTY_SYNC;
    pkt[1] = 1;                       // one mon
    pkt[sizeof(pkt) - 1] = 0x11;      // nonzero seed 0x00000011

    Multiplayer_Init();
    gMultiplayerState.connState      = MP_STATE_CONNECTED;
    // Battle underway with an already-adopted seed.
    gBattleTypeFlags                 = BATTLE_TYPE_COOP;
    gMultiplayerState.coopBattleSeed = 0xCAFEF00Du;

    for (i = 0; i < sizeof(pkt); i++)
        Mp_Push(&gMpRecvRing, pkt[i]);
    Multiplayer_Update();

    // The late packet is drained (framing intact) but NOT applied: rebuilding
    // would clobber the partner's live in-battle party and re-adopting the
    // seed would desync the RNG stream.
    ASSERT_EQ(gMultiplayerState.coopBattleSeed, 0xCAFEF00Du);
    ASSERT_EQ(gMultiplayerState.partnerPartySelectDone, FALSE);
    ASSERT_EQ(Mp_Available(&gMpRecvRing), 0);

    gBattleTypeFlags = 0;
}

static void TestMenuTickDrainsRecvAndBeacons(void)
{
    u8 pkt[MP_PKT_SIZE_STATE_BEACON];
    u8 i, frame;
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();

    // Disconnected: inert — recv not drained, nothing sent.
    PushBeaconGenderAck(0, FALSE);
    Multiplayer_MenuTick();
    ASSERT_NE(Mp_Available(&gMpRecvRing), 0); // still queued (not drained)
    ASSERT_EQ(Mp_Available(&gMpSendRing), 0);

    // Connected: drains the recv ring so it can never overflow while the party
    // menu owns the main callback (the item.c:782 misframe repair).
    Multiplayer_Init();
    gMultiplayerState.connState = MP_STATE_CONNECTED;
    PushBeaconGenderAck(1, TRUE);
    Multiplayer_MenuTick();
    ASSERT_EQ(Mp_Available(&gMpRecvRing), 0);       // drained
    ASSERT_EQ(gMultiplayerState.partnerGotMyParty, TRUE); // beacon applied
    ASSERT_EQ(gMultiplayerState.partnerGender, 1);

    // ...and the heartbeat/beacon keep flowing so the relay's silence detector
    // can't false-disconnect a slow chooser.
    { u8 out; while (Mp_Pop(&gMpSendRing, &out)) {} }
    for (frame = 0; frame < MP_BEACON_INTERVAL_FRAMES; frame++)
        Multiplayer_MenuTick();
    for (;;)
    {
        ASSERT_EQ(Mp_Pop(&gMpSendRing, &pkt[0]), TRUE);
        if (pkt[0] == MP_PKT_STATE_BEACON)
            break;
    }
    for (i = 1; i < MP_PKT_SIZE_STATE_BEACON; i++)
        Mp_Pop(&gMpSendRing, &pkt[i]);
    // Outside a coop battle the turn payload is zero.
    ASSERT_EQ(pkt[5], 0);
}

// ---- Coop battle RNG lockstep -------------------------------------------------

static bool32 RejectNothing(u32 v) { (void)v; return FALSE; }

static void TestCoopRngDeterministicStream(void)
{
    u16 a[8], b[8];
    u32 i;

    Multiplayer_Init();
    gBattleTypeFlags = BATTLE_TYPE_COOP;

    // Two "instances" seeded identically draw identical sequences.
    gMultiplayerState.coopRngState = 0xA5A5A5A5u;
    for (i = 0; i < 8; i++)
        a[i] = (u16)RandomUniform(RNG_NONE, 0, 0xFFFF);
    gMultiplayerState.coopRngState = 0xA5A5A5A5u;
    for (i = 0; i < 8; i++)
        b[i] = (u16)RandomUniform(RNG_NONE, 0, 0xFFFF);
    for (i = 0; i < 8; i++)
        ASSERT_EQ(a[i], b[i]);
    // ...and the stream actually moves.
    ASSERT_EQ(a[0] == a[1] && a[1] == a[2], FALSE);

    // Range contract matches RandomUniformDefault: lo..hi inclusive.
    gMultiplayerState.coopRngState = 1;
    for (i = 0; i < 200; i++)
    {
        u32 v = RandomUniform(RNG_NONE, 3, 7);
        ASSERT_EQ(v >= 3 && v <= 7, TRUE);
    }

    // Zero state never sticks (xorshift32 fixpoint) — remapped internally.
    gMultiplayerState.coopRngState = 0;
    (void)RandomUniform(RNG_NONE, 0, 0xFFFF);
    ASSERT_EQ(gMultiplayerState.coopRngState != 0, TRUE);

    gBattleTypeFlags = 0;
}

static void TestRandomOverridesRouteByBattleType(void)
{
    static const u16 weights[2] = { 1, 1 };
    u16 elems[4] = { 10, 20, 30, 40 };

    Multiplayer_Init();

    // Not in a coop battle: all four overrides fall through to *Default.
    gBattleTypeFlags = 0;
    gTestRandomDefaultCalls = 0;
    (void)RandomUniform(RNG_NONE, 0, 10);
    (void)RandomUniformExcept(RNG_NONE, 0, 10, RejectNothing);
    (void)RandomWeightedArray(RNG_NONE, 2, 2, weights);
    (void)RandomElementArray(RNG_NONE, elems, sizeof(elems[0]), 4);
    ASSERT_EQ(gTestRandomDefaultCalls, 4);

    // In a coop battle: none touch *Default; all draw from the lockstep stream.
    gBattleTypeFlags = BATTLE_TYPE_COOP;
    gMultiplayerState.coopRngState = 0xBEEF1234u;
    gTestRandomDefaultCalls = 0;
    (void)RandomUniform(RNG_NONE, 0, 10);
    (void)RandomUniformExcept(RNG_NONE, 0, 10, RejectNothing);
    (void)RandomWeightedArray(RNG_NONE, 2, 2, weights);
    {
        const void *e = RandomElementArray(RNG_NONE, elems, sizeof(elems[0]), 4);
        // Must point at one of the 4 elements.
        ASSERT_EQ(e >= (const void *)&elems[0] && e <= (const void *)&elems[3], TRUE);
    }
    ASSERT_EQ(gTestRandomDefaultCalls, 0);
    ASSERT_EQ(gMultiplayerState.coopRngState != 0xBEEF1234u, TRUE);

    gBattleTypeFlags = 0;
}

// ---- ROLE_ASSIGN ------------------------------------------------------------

static void TestRecvRoleAssignSetsRole(void)
{
    struct SaveBlock1 save;
    memset(&save, 0, sizeof(save));
    gSaveBlock1Ptr = &save;

    Multiplayer_Init();
    ASSERT_EQ(gMultiplayerState.role, MP_ROLE_NONE);

    Mp_Push(&gMpRecvRing, MP_PKT_ROLE_ASSIGN);
    Mp_Push(&gMpRecvRing, MP_ROLE_GUEST);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.role, MP_ROLE_GUEST);

    // Invalid role value is ignored; current role survives.
    Mp_Push(&gMpRecvRing, MP_PKT_ROLE_ASSIGN);
    Mp_Push(&gMpRecvRing, 7);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.role, MP_ROLE_GUEST);

    Mp_Push(&gMpRecvRing, MP_PKT_ROLE_ASSIGN);
    Mp_Push(&gMpRecvRing, MP_ROLE_HOST);
    Multiplayer_Update();
    ASSERT_EQ(gMultiplayerState.role, MP_ROLE_HOST);
}

// ---- Entry point ----------------------------------------------------------

int main(void)
{
    // Ring buffer
    TestRingPushPop();
    TestRingEmptyPop();
    TestRingFull();
    TestRingWrapAround();

    // POSITION
    TestEncodeDecodePosition();
    TestPositionBoundaryValues();
    TestPositionTruncated();

    // FLAG_SET
    TestEncodeDecodeFlagSet();
    TestFlagSetBoundaryValues();
    TestFlagSetTruncated();

    // VAR_SET
    TestEncodeDecodeVarSet();
    TestVarSetTruncated();

    // BOSS_READY
    TestEncodeDecodeBossReady();
    TestBossReadyTruncated();

    // BOSS_CANCEL
    TestEncodeBossCancel();

    // GENDER
    TestEncodeDecodeGenderMale();
    TestEncodeDecodeGenderFemale();
    TestDecodeGenderTruncated();

    // SEED_SYNC
    TestEncodeDecodeSeedSync();
    TestSeedSyncBoundaryValues();
    TestSeedSyncTruncated();

    // FULL_SYNC
    TestEncodeDecodeFullSync();
    TestFullSyncEmptyPayload();
    TestFullSyncTruncatedHeader();
    TestFullSyncLargerPayload();

    // Integration: send/recv through ring buffers
    TestSendPositionWritesToRing();
    TestRecvPacketDispatchesGhostPosition();
    TestRecvSeedSyncUpdatesSeed();
    TestRecvUnknownTypeDrainsRing();

    // PARTY_SYNC wire mon + ROLE_ASSIGN
    TestPartyMonWireRoundTrip();
    TestSendPartySyncPacketSize();
    TestPartySyncSeedHostSendsGuestZeroes();
    TestRecvPartySyncAdoptsNonzeroSeed();
    TestRecvRoleAssignSetsRole();

    // BATTLE_TURN sequencing + beacon repair
    TestSendBattleTurnAssignsSeq();
    TestResendBattleTurnKeepsSeq();
    TestHandleBattleTurnDedupAndOrdering();
    TestSetupCoopBattleResetsTurnStateAndSeedsRng();
    TestBeaconRepairsDroppedBattleTurn();
    TestBeaconSenderCarriesCachedTurnOnlyInCoopBattle();
    TestBattleTickPumpsBeaconAndRecv();

    // STATE_BEACON field-trainer lock repair (Bug #18a)
    TestBeaconRepairsDroppedTrainerBusy();
    TestBeaconRepairsDroppedTrainerFree();
    TestSendTrainerBusyStoresCoords();
    TestBeaconSenderCarriesBusyTrainerOutsideCoopBattle();
    TestBeaconBusyIgnoredDuringCoopBattle();

    // Trainer-approach cosmetic mirror (Bug #18b)
    TestSendTrainerApproachEncodes();
    TestTrainerApproachFiresOnMatchingMap();
    TestTrainerApproachIgnoredOnOtherMap();

    // Party-sync mutual handshake (asymmetric-loss deadlock fix)
    TestBeaconPartyAckSetsPartnerGotMyParty();
    TestPartySyncHandshakeNeedsMutualAck();
    TestMidBattlePartySyncIgnored();
    TestMenuTickDrainsRecvAndBeacons();

    // Coop battle RNG lockstep
    TestCoopRngDeterministicStream();
    TestRandomOverridesRouteByBattleType();

    TEST_SUMMARY();
}
