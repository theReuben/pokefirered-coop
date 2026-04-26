#include "test_runner.h"
#include "global.h"
#include "multiplayer.h"
#include "constants/multiplayer.h"
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

    TEST_SUMMARY();
}
