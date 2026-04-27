#include "global.h"
#include "multiplayer.h"
#include "constants/multiplayer.h"
#include "constants/event_object_movement.h"
#include "event_object_movement.h"
#include "random.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
struct MultiplayerState gMultiplayerState;
struct CoopSettings gCoopSettings;

// Ring buffers in EWRAM.  Tauri locates them via ELF symbol + magic check.
EWRAM_DATA struct MpRingBuf gMpSendRing;
EWRAM_DATA struct MpRingBuf gMpRecvRing;

// ---------------------------------------------------------------------------
// Encode helpers — write a packet into a flat byte buffer.
// Returns the number of bytes written (0 never occurs for valid inputs).
// ---------------------------------------------------------------------------

u8 Mp_EncodePosition(u8 *out, u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing)
{
    out[0] = MP_PKT_POSITION;
    out[1] = mapGroup;
    out[2] = mapNum;
    out[3] = x;
    out[4] = y;
    out[5] = facing;
    return MP_PKT_SIZE_POSITION;
}

u8 Mp_EncodeFlagSet(u8 *out, u16 flagId)
{
    out[0] = MP_PKT_FLAG_SET;
    out[1] = (u8)(flagId >> 8);
    out[2] = (u8)(flagId);
    return MP_PKT_SIZE_FLAG_SET;
}

u8 Mp_EncodeVarSet(u8 *out, u16 varId, u16 value)
{
    out[0] = MP_PKT_VAR_SET;
    out[1] = (u8)(varId >> 8);
    out[2] = (u8)(varId);
    out[3] = (u8)(value >> 8);
    out[4] = (u8)(value);
    return MP_PKT_SIZE_VAR_SET;
}

u8 Mp_EncodeBossReady(u8 *out, u8 bossId)
{
    out[0] = MP_PKT_BOSS_READY;
    out[1] = bossId;
    return MP_PKT_SIZE_BOSS_READY;
}

u8 Mp_EncodeBossCancel(u8 *out)
{
    out[0] = MP_PKT_BOSS_CANCEL;
    return MP_PKT_SIZE_BOSS_CANCEL;
}

u8 Mp_EncodeSeedSync(u8 *out, u32 seed)
{
    out[0] = MP_PKT_SEED_SYNC;
    out[1] = (u8)(seed >> 24);
    out[2] = (u8)(seed >> 16);
    out[3] = (u8)(seed >> 8);
    out[4] = (u8)(seed);
    return MP_PKT_SIZE_SEED_SYNC;
}

// ---------------------------------------------------------------------------
// Decode helpers — read a packet from a flat byte buffer.
// in[0] is the type byte; len is the total number of bytes available.
// Returns TRUE on success, FALSE if the buffer is too short.
// ---------------------------------------------------------------------------

bool8 Mp_DecodePosition(const u8 *in, u8 len,
                        u8 *mapGroup, u8 *mapNum, u8 *x, u8 *y, u8 *facing)
{
    if (len < MP_PKT_SIZE_POSITION)
        return FALSE;
    *mapGroup = in[1];
    *mapNum   = in[2];
    *x        = in[3];
    *y        = in[4];
    *facing   = in[5];
    return TRUE;
}

bool8 Mp_DecodeFlagSet(const u8 *in, u8 len, u16 *flagId)
{
    if (len < MP_PKT_SIZE_FLAG_SET)
        return FALSE;
    *flagId = ((u16)in[1] << 8) | in[2];
    return TRUE;
}

bool8 Mp_DecodeVarSet(const u8 *in, u8 len, u16 *varId, u16 *value)
{
    if (len < MP_PKT_SIZE_VAR_SET)
        return FALSE;
    *varId  = ((u16)in[1] << 8) | in[2];
    *value  = ((u16)in[3] << 8) | in[4];
    return TRUE;
}

bool8 Mp_DecodeBossReady(const u8 *in, u8 len, u8 *bossId)
{
    if (len < MP_PKT_SIZE_BOSS_READY)
        return FALSE;
    *bossId = in[1];
    return TRUE;
}

bool8 Mp_DecodeSeedSync(const u8 *in, u8 len, u32 *seed)
{
    if (len < MP_PKT_SIZE_SEED_SYNC)
        return FALSE;
    *seed = ((u32)in[1] << 24) | ((u32)in[2] << 16) | ((u32)in[3] << 8) | in[4];
    return TRUE;
}

u16 Mp_EncodeFullSync(u8 *out, const u8 *data, u16 dataLen)
{
    u16 i;
    out[0] = MP_PKT_FULL_SYNC;
    out[1] = (u8)(dataLen >> 8);
    out[2] = (u8)(dataLen);
    for (i = 0; i < dataLen; i++)
        out[3 + i] = data[i];
    return (u16)(MP_PKT_SIZE_FULL_SYNC_HDR + dataLen);
}

bool8 Mp_DecodeFullSync(const u8 *in, u16 len, const u8 **dataOut, u16 *dataLen)
{
    u16 declared;
    if (len < MP_PKT_SIZE_FULL_SYNC_HDR)
        return FALSE;
    declared = ((u16)in[1] << 8) | in[2];
    if ((u16)(MP_PKT_SIZE_FULL_SYNC_HDR + declared) > len)
        return FALSE;
    *dataOut  = &in[3];
    *dataLen  = declared;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Push a flat packet (already encoded) into a ring buffer byte-by-byte.
// Drops the packet silently if the ring is full.
// ---------------------------------------------------------------------------
static void MpRing_Write(struct MpRingBuf *ring, const u8 *data, u8 len)
{
    u8 i;
    // Check space first so we don't write a partial packet.
    if ((u8)(MP_RING_SIZE - 1 - Mp_Available(ring)) < len)
        return; // not enough space — drop
    for (i = 0; i < len; i++)
        Mp_Push(ring, data[i]);
}

// ---------------------------------------------------------------------------
// Read one complete packet from gMpRecvRing and dispatch it.
// Returns TRUE if a packet was processed.
// ---------------------------------------------------------------------------
static bool8 ProcessOneRecvPacket(void)
{
    // Largest fixed packet is POSITION (6 bytes).
    // FULL_SYNC is variable; we use a separate local for it.
    u8 pkt[MP_PKT_SIZE_POSITION];
    u8 typeByte;
    u8 mapGroup, mapNum, x, y, facing;
    u16 flagId, varId, val;
    u8 bossId;
    u32 seed;

    if (!Mp_Pop(&gMpRecvRing, &typeByte))
        return FALSE; // nothing to read

    switch (typeByte)
    {
    case MP_PKT_POSITION:
        // Read remaining 5 bytes
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_POSITION - 1)
            return FALSE; // truncated — drop and desync (rare)
        pkt[0] = typeByte;
        { u8 i; for (i = 1; i < MP_PKT_SIZE_POSITION; i++) Mp_Pop(&gMpRecvRing, &pkt[i]); }
        if (Mp_DecodePosition(pkt, MP_PKT_SIZE_POSITION, &mapGroup, &mapNum, &x, &y, &facing))
            Multiplayer_UpdateGhostPosition(mapGroup, mapNum, x, y, facing);
        break;

    case MP_PKT_FLAG_SET:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_FLAG_SET - 1)
            return FALSE;
        pkt[0] = typeByte;
        { u8 i; for (i = 1; i < MP_PKT_SIZE_FLAG_SET; i++) Mp_Pop(&gMpRecvRing, &pkt[i]); }
        if (Mp_DecodeFlagSet(pkt, MP_PKT_SIZE_FLAG_SET, &flagId))
            Multiplayer_HandleRemoteFlagSet(flagId);
        break;

    case MP_PKT_VAR_SET:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_VAR_SET - 1)
            return FALSE;
        pkt[0] = typeByte;
        { u8 i; for (i = 1; i < MP_PKT_SIZE_VAR_SET; i++) Mp_Pop(&gMpRecvRing, &pkt[i]); }
        if (Mp_DecodeVarSet(pkt, MP_PKT_SIZE_VAR_SET, &varId, &val))
            Multiplayer_HandleRemoteVarSet(varId, val);
        break;

    case MP_PKT_BOSS_READY:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_BOSS_READY - 1)
            return FALSE;
        pkt[0] = typeByte;
        Mp_Pop(&gMpRecvRing, &pkt[1]);
        if (Mp_DecodeBossReady(pkt, MP_PKT_SIZE_BOSS_READY, &bossId))
            gMultiplayerState.partnerBossId = bossId;
        break;

    case MP_PKT_BOSS_CANCEL:
        // 1-byte packet — type byte already consumed.
        // Partner walked away; clear their readiness so our script loop keeps waiting.
        gMultiplayerState.partnerBossId = 0;
        break;

    case MP_PKT_BOSS_START:
        // Relay server confirmed both players ready.  Treat as if partner sent BOSS_READY
        // for whatever boss we're currently waiting on.
        if (gMultiplayerState.bossReadyBossId != 0)
            gMultiplayerState.partnerBossId = gMultiplayerState.bossReadyBossId;
        break;

    case MP_PKT_SCRIPT_LOCK:
        gMultiplayerState.partnerIsInScript = TRUE;
        break;

    case MP_PKT_SCRIPT_UNLOCK:
        gMultiplayerState.partnerIsInScript = FALSE;
        break;

    case MP_PKT_SEED_SYNC:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_SEED_SYNC - 1)
            return FALSE;
        pkt[0] = typeByte;
        { u8 i; for (i = 1; i < MP_PKT_SIZE_SEED_SYNC; i++) Mp_Pop(&gMpRecvRing, &pkt[i]); }
        if (Mp_DecodeSeedSync(pkt, MP_PKT_SIZE_SEED_SYNC, &seed))
            gCoopSettings.encounterSeed = seed;
        break;

    case MP_PKT_FULL_SYNC:
    {
        // Read 2-byte length header, then payload.
        // Max payload that fits in a 256-byte ring: 252 bytes.
        u8 lenHi = 0, lenLo = 0;
        u16 dataLen;
        u8 fullPkt[3 + 252]; // header + max payload
        u16 i;

        if (Mp_Available(&gMpRecvRing) < 2)
            return FALSE; // truncated header
        Mp_Pop(&gMpRecvRing, &lenHi);
        Mp_Pop(&gMpRecvRing, &lenLo);
        dataLen = ((u16)lenHi << 8) | lenLo;

        if (dataLen > 252 || Mp_Available(&gMpRecvRing) < (u8)dataLen)
            return FALSE; // truncated or oversized payload
        fullPkt[0] = typeByte;
        fullPkt[1] = lenHi;
        fullPkt[2] = lenLo;
        for (i = 0; i < dataLen; i++)
            Mp_Pop(&gMpRecvRing, &fullPkt[3 + i]);
        Multiplayer_ApplyFullSync(&fullPkt[3], dataLen);
        break;
    }

    default:
        // Unknown type — can't recover sync; drain ring to avoid stall.
        while (Mp_Pop(&gMpRecvRing, &typeByte)) {}
        break;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Ghost NPC — internal helpers
// ---------------------------------------------------------------------------

// Returns the MOVEMENT_ACTION_WALK_NORMAL_* constant for the step direction,
// or 0xFF if the ghost is already at the target.
static u8 GhostNextStepAction(const struct ObjectEvent *ghost)
{
    s16 dx = (s16)gMultiplayerState.targetX - ghost->currentCoords.x;
    s16 dy = (s16)gMultiplayerState.targetY - ghost->currentCoords.y;

    if (dx == 0 && dy == 0)
        return 0xFF; // at target

    // Prioritise horizontal movement when both axes differ to match normal walk feel.
    if (dx > 0)  return MOVEMENT_ACTION_WALK_NORMAL_RIGHT;
    if (dx < 0)  return MOVEMENT_ACTION_WALK_NORMAL_LEFT;
    if (dy > 0)  return MOVEMENT_ACTION_WALK_NORMAL_DOWN;
    return MOVEMENT_ACTION_WALK_NORMAL_UP;
}

// Steps the ghost one tile towards its target each frame.
static void GhostTick(void)
{
    u8 objId = gMultiplayerState.ghostObjectEventId;
    struct ObjectEvent *ghost;
    u8 action;

    if (objId >= OBJECT_EVENTS_COUNT || !gObjectEvents[objId].active)
        return;

    // Freeze ghost movement while partner is in a script interaction.
    if (gMultiplayerState.partnerIsInScript)
        return;

    ghost = &gObjectEvents[objId];

    ObjectEventClearHeldMovementIfFinished(ghost);

    if (ghost->heldMovementActive)
        return;

    action = GhostNextStepAction(ghost);
    if (action == 0xFF)
    {
        SetObjectEventDirection(ghost, gMultiplayerState.targetFacing);
        return;
    }

    ObjectEventSetHeldMovement(ghost, action);
}

// Spawns or despawns the ghost based on whether the partner's map matches.
static void GhostMapCheck(void)
{
    u8 playerMapGroup = (u8)gSaveBlock1Ptr->location.mapGroup;
    u8 playerMapNum   = (u8)gSaveBlock1Ptr->location.mapNum;
    bool32 sameMap    = (gMultiplayerState.partnerMapGroup == playerMapGroup
                      && gMultiplayerState.partnerMapNum   == playerMapNum);

    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
    {
        if (gMultiplayerState.ghostObjectEventId < OBJECT_EVENTS_COUNT)
            Multiplayer_DespawnGhost();
        return;
    }

    if (sameMap)
    {
        if (gMultiplayerState.ghostObjectEventId >= OBJECT_EVENTS_COUNT)
        {
            Multiplayer_SpawnGhostNPC(
                gMultiplayerState.partnerMapGroup,
                gMultiplayerState.partnerMapNum,
                gMultiplayerState.targetX,
                gMultiplayerState.targetY,
                gMultiplayerState.targetFacing);
        }
    }
    else
    {
        if (gMultiplayerState.ghostObjectEventId < OBJECT_EVENTS_COUNT)
            Multiplayer_DespawnGhost();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Multiplayer_Init(void)
{
    gMultiplayerState.role               = MP_ROLE_NONE;
    gMultiplayerState.connState          = MP_STATE_DISCONNECTED;
    gMultiplayerState.partnerMapGroup    = 0xFF;
    gMultiplayerState.partnerMapNum      = 0xFF;
    gMultiplayerState.targetX            = 0;
    gMultiplayerState.targetY            = 0;
    gMultiplayerState.targetFacing       = DIR_SOUTH;
    gMultiplayerState.ghostObjectEventId = GHOST_INVALID_SLOT;
    gMultiplayerState.bossReadyBossId    = 0;
    gMultiplayerState.partnerBossId      = 0;
    gMultiplayerState.isInScript         = FALSE;
    gMultiplayerState.partnerIsInScript  = FALSE;
    gMultiplayerState.posFrameCounter    = 0;
    gCoopSettings.randomizeEncounters    = 1;
    gCoopSettings.encounterSeed          = 0;

    gMpSendRing.head  = 0;
    gMpSendRing.tail  = 0;
    gMpSendRing.magic = MP_RING_MAGIC;
    gMpRecvRing.head  = 0;
    gMpRecvRing.tail  = 0;
    gMpRecvRing.magic = MP_RING_MAGIC;

#if MP_DEBUG_TEST_GHOST
    gMultiplayerState.connState       = MP_STATE_CONNECTED;
    gMultiplayerState.partnerMapGroup = MP_DEBUG_TEST_MAP_GROUP;
    gMultiplayerState.partnerMapNum   = MP_DEBUG_TEST_MAP_NUM;
    gMultiplayerState.targetX         = MP_DEBUG_TEST_X;
    gMultiplayerState.targetY         = MP_DEBUG_TEST_Y;
    gMultiplayerState.targetFacing    = DIR_SOUTH;
#endif
}

void Multiplayer_Update(void)
{
    // Process all pending incoming packets (one per frame is fine for low-rate data).
    while (ProcessOneRecvPacket()) {}

    GhostMapCheck();
    GhostTick();

    // Send our position every 4 frames if connected.
    if (gMultiplayerState.connState == MP_STATE_CONNECTED)
    {
        gMultiplayerState.posFrameCounter++;
        if (gMultiplayerState.posFrameCounter >= 4)
        {
            gMultiplayerState.posFrameCounter = 0;
            Multiplayer_SendPosition();
        }
    }
}

void Multiplayer_SpawnGhostNPC(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing)
{
    u8 objId;

    if (gMultiplayerState.ghostObjectEventId < OBJECT_EVENTS_COUNT)
        Multiplayer_DespawnGhost();

    objId = SpawnSpecialObjectEventParameterized(
        OBJ_EVENT_GFX_PLAYER2,
        MOVEMENT_TYPE_NONE,
        GHOST_LOCAL_ID,
        x, y,
        GHOST_ELEVATION);

    if (objId >= OBJECT_EVENTS_COUNT)
        return; // no free slot

    gObjectEvents[objId].mapGroup = (u8)mapGroup;
    gObjectEvents[objId].mapNum   = (u8)mapNum;
    SetObjectEventDirection(&gObjectEvents[objId], facing);
    gMultiplayerState.ghostObjectEventId = objId;
}

void Multiplayer_DespawnGhost(void)
{
    u8 objId = gMultiplayerState.ghostObjectEventId;

    if (objId < OBJECT_EVENTS_COUNT && gObjectEvents[objId].active)
        RemoveObjectEvent(&gObjectEvents[objId]);

    gMultiplayerState.ghostObjectEventId = GHOST_INVALID_SLOT;
}

void Multiplayer_UpdateGhostPosition(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing)
{
    gMultiplayerState.partnerMapGroup = mapGroup;
    gMultiplayerState.partnerMapNum   = mapNum;
    gMultiplayerState.targetX         = x;
    gMultiplayerState.targetY         = y;
    gMultiplayerState.targetFacing    = facing;
}

void Multiplayer_SendPosition(void)
{
    u8 pkt[MP_PKT_SIZE_POSITION];
    u8 mapGroup = (u8)gSaveBlock1Ptr->location.mapGroup;
    u8 mapNum   = (u8)gSaveBlock1Ptr->location.mapNum;
    u8 x        = (u8)gSaveBlock1Ptr->location.x;
    u8 y        = (u8)gSaveBlock1Ptr->location.y;
    u8 len;

    len = Mp_EncodePosition(pkt, mapGroup, mapNum, x, y, DIR_SOUTH);
    MpRing_Write(&gMpSendRing, pkt, len);
}

void Multiplayer_SendFlagSet(u16 flagId)
{
    u8 pkt[MP_PKT_SIZE_FLAG_SET];
    u8 len = Mp_EncodeFlagSet(pkt, flagId);
    MpRing_Write(&gMpSendRing, pkt, len);
}

void Multiplayer_SendVarSet(u16 varId, u16 value)
{
    u8 pkt[MP_PKT_SIZE_VAR_SET];
    u8 len = Mp_EncodeVarSet(pkt, varId, value);
    MpRing_Write(&gMpSendRing, pkt, len);
}

void Multiplayer_SendBossReady(u8 bossId)
{
    u8 pkt[MP_PKT_SIZE_BOSS_READY];
    u8 len = Mp_EncodeBossReady(pkt, bossId);
    MpRing_Write(&gMpSendRing, pkt, len);
}

void Multiplayer_SendBossCancel(void)
{
    u8 pkt[MP_PKT_SIZE_BOSS_CANCEL];
    u8 len = Mp_EncodeBossCancel(pkt);
    MpRing_Write(&gMpSendRing, pkt, len);
}

// ---------------------------------------------------------------------------
// Full sync — build and send, or apply on receipt
// ---------------------------------------------------------------------------

void Multiplayer_SendFullSync(void)
{
    // 3-byte header + 214-byte payload = 217 bytes, fits in the 255-byte ring.
    u8 pkt[MP_PKT_SIZE_FULL_SYNC_HDR + FULL_SYNC_PAYLOAD_SIZE];
    u8 payload[FULL_SYNC_PAYLOAD_SIZE];
    u16 offset = 0;
    u16 i;

    if (!gSaveBlock1Ptr)
        return;

    for (i = FULL_SYNC_STORY_BYTE_START; i <= FULL_SYNC_STORY_BYTE_END; i++)
        payload[offset++] = gSaveBlock1Ptr->flags[i];
    for (i = FULL_SYNC_ITEMS_BYTE_START; i <= FULL_SYNC_ITEMS_BYTE_END; i++)
        payload[offset++] = gSaveBlock1Ptr->flags[i];
    for (i = FULL_SYNC_BOSSES_BYTE_START; i <= FULL_SYNC_BOSSES_BYTE_END; i++)
        payload[offset++] = gSaveBlock1Ptr->flags[i];
    for (i = FULL_SYNC_TRAINERS_BYTE_START; i <= FULL_SYNC_TRAINERS_BYTE_END; i++)
        payload[offset++] = gSaveBlock1Ptr->flags[i];

    Mp_EncodeFullSync(pkt, payload, offset);
    MpRing_Write(&gMpSendRing, pkt, (u8)(MP_PKT_SIZE_FULL_SYNC_HDR + offset));
}

void Multiplayer_ApplyFullSync(const u8 *payload, u16 payloadLen)
{
    u16 offset = 0;
    u16 i;

    if (!gSaveBlock1Ptr || payloadLen != FULL_SYNC_PAYLOAD_SIZE)
        return;

    // OR into our flags so any flag set by either player remains set (union-wins).
    for (i = FULL_SYNC_STORY_BYTE_START; i <= FULL_SYNC_STORY_BYTE_END; i++)
        gSaveBlock1Ptr->flags[i] |= payload[offset++];
    for (i = FULL_SYNC_ITEMS_BYTE_START; i <= FULL_SYNC_ITEMS_BYTE_END; i++)
        gSaveBlock1Ptr->flags[i] |= payload[offset++];
    for (i = FULL_SYNC_BOSSES_BYTE_START; i <= FULL_SYNC_BOSSES_BYTE_END; i++)
        gSaveBlock1Ptr->flags[i] |= payload[offset++];
    for (i = FULL_SYNC_TRAINERS_BYTE_START; i <= FULL_SYNC_TRAINERS_BYTE_END; i++)
        gSaveBlock1Ptr->flags[i] |= payload[offset++];
}

bool32 IsSyncableFlag(u16 flagId)
{
    return (flagId >= SYNC_FLAG_STORY_START    && flagId <= SYNC_FLAG_STORY_END)
        || (flagId >= SYNC_FLAG_ITEMS_START    && flagId <= SYNC_FLAG_ITEMS_END)
        || (flagId >= SYNC_FLAG_BOSSES_START   && flagId <= SYNC_FLAG_BOSSES_END)
        || (flagId >= SYNC_FLAG_TRAINERS_START && flagId <= SYNC_FLAG_TRAINERS_END);
}

// Var sync audit deferred to Phase 3 step 3.2+; no vars synced yet.
bool32 IsSyncableVar(u16 varId)
{
    (void)varId;
    return FALSE;
}

// ---------------------------------------------------------------------------
// Seeded PRNG — xorshift32
// State is kept in EWRAM; seed 0 is forbidden (xorshift32 loops at 0).
// ---------------------------------------------------------------------------

static u32 sMpRngState;

void Multiplayer_SeedRng(u32 seed)
{
    sMpRngState = seed ? seed : 0x12345678u;
}

u32 Multiplayer_NextRandom(void)
{
    sMpRngState ^= sMpRngState << 13;
    sMpRngState ^= sMpRngState >> 17;
    sMpRngState ^= sMpRngState << 5;
    return sMpRngState;
}

// Per-slot hash: deterministically maps (seed, ROM table address, slot) to a
// species in the Gen I-IV national dex (1-493).  Does NOT advance sMpRngState
// so encounter order has no effect on results.
// Returns 0 (SPECIES_NONE) if randomization is disabled or seed is unset.
u16 Multiplayer_GetRandomizedSpecies(u32 tableAddr, u8 slotIndex)
{
    u32 state;
    if (!gCoopSettings.randomizeEncounters || !gCoopSettings.encounterSeed)
        return 0;
    state = gCoopSettings.encounterSeed ^ tableAddr ^ (u32)slotIndex;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    // Map to species 1-493 (complete Gen I-IV national dex, no invalid IDs).
    return (u16)(1u + (state % 493u));
}

// ---------------------------------------------------------------------------
// Script mutex — advisory lock so each player knows when the other is
// executing a script interaction (prevents both talking to the same NPC).
// ---------------------------------------------------------------------------

void Multiplayer_OnScriptStart(void)
{
    u8 pkt;

    if (gMultiplayerState.isInScript)
        return; // already locked; don't double-send

    gMultiplayerState.isInScript = TRUE;

    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
        return;

    pkt = MP_PKT_SCRIPT_LOCK;
    MpRing_Write(&gMpSendRing, &pkt, MP_PKT_SIZE_SCRIPT_LOCK);
}

void Multiplayer_OnScriptEnd(void)
{
    u8 pkt;

    if (!gMultiplayerState.isInScript)
        return; // was not locked; don't double-send

    gMultiplayerState.isInScript = FALSE;

    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
        return;

    pkt = MP_PKT_SCRIPT_UNLOCK;
    MpRing_Write(&gMpSendRing, &pkt, MP_PKT_SIZE_SCRIPT_UNLOCK);
}

bool32 Multiplayer_IsPartnerInScript(void)
{
    return gMultiplayerState.partnerIsInScript;
}

// ---------------------------------------------------------------------------
// Seed sync (Phase 4) — host generates and broadcasts the encounter seed.
// ---------------------------------------------------------------------------

u32 Multiplayer_GenerateSeed(void)
{
    // Combine two 16-bit Random() draws into one 32-bit seed.
    // Seed 0 is forbidden (Multiplayer_GetRandomizedSpecies treats it as
    // "no seed yet" and returns 0 / pass-through).
    u32 seed = ((u32)Random() << 16) | Random();
    return seed ? seed : 0x12345678u;
}

void Multiplayer_SendSeedSync(u32 seed)
{
    u8 pkt[MP_PKT_SIZE_SEED_SYNC];
    u8 len = Mp_EncodeSeedSync(pkt, seed);
    MpRing_Write(&gMpSendRing, pkt, len);
}

// ---------------------------------------------------------------------------
// Boss readiness protocol (Phase 5)
// ---------------------------------------------------------------------------

static void BossReadyCommon(u8 bossId)
{
    gMultiplayerState.bossReadyBossId = bossId;
    gMultiplayerState.partnerBossId   = 0; // reset partner state for fresh check
    if (gMultiplayerState.connState == MP_STATE_CONNECTED)
        Multiplayer_SendBossReady(bossId);
}

void Multiplayer_BossReady_Brock(void)    { BossReadyCommon(BOSS_ID_BROCK); }
void Multiplayer_BossReady_Misty(void)    { BossReadyCommon(BOSS_ID_MISTY); }
void Multiplayer_BossReady_LtSurge(void)  { BossReadyCommon(BOSS_ID_LT_SURGE); }
void Multiplayer_BossReady_Erika(void)    { BossReadyCommon(BOSS_ID_ERIKA); }
void Multiplayer_BossReady_Koga(void)     { BossReadyCommon(BOSS_ID_KOGA); }
void Multiplayer_BossReady_Sabrina(void)  { BossReadyCommon(BOSS_ID_SABRINA); }
void Multiplayer_BossReady_Blaine(void)   { BossReadyCommon(BOSS_ID_BLAINE); }
void Multiplayer_BossReady_Giovanni(void) { BossReadyCommon(BOSS_ID_GIOVANNI); }
void Multiplayer_BossReady_Lorelei(void)  { BossReadyCommon(BOSS_ID_LORELEI); }
void Multiplayer_BossReady_Bruno(void)    { BossReadyCommon(BOSS_ID_BRUNO); }
void Multiplayer_BossReady_Agatha(void)   { BossReadyCommon(BOSS_ID_AGATHA); }
void Multiplayer_BossReady_Lance(void)    { BossReadyCommon(BOSS_ID_LANCE); }
void Multiplayer_BossReady_Champion(void) { BossReadyCommon(BOSS_ID_CHAMPION); }

void Multiplayer_BossCancel(void)
{
    if (gMultiplayerState.bossReadyBossId == 0)
        return; // not in a readiness check; nothing to cancel

    gMultiplayerState.bossReadyBossId = 0;
    gMultiplayerState.partnerBossId   = 0;

    if (gMultiplayerState.connState == MP_STATE_CONNECTED)
        Multiplayer_SendBossCancel();
}

// Returns 1 when both players are ready to start (or when playing solo), then
// clears readiness state.  Called each frame from the gym script wait loop via
// 'specialvar VAR_RESULT, Multiplayer_ScriptCheckBossStart'.
u16 Multiplayer_ScriptCheckBossStart(void)
{
    bool32 partnerReady;

    if (gMultiplayerState.bossReadyBossId == 0)
        return 0; // we haven't even sent BOSS_READY yet

    partnerReady = (gMultiplayerState.partnerBossId != 0)
                || (gMultiplayerState.connState != MP_STATE_CONNECTED);

    if (!partnerReady)
        return 0; // still waiting

    // Both ready (or solo) — clear state and tell the script to start battle.
    gMultiplayerState.bossReadyBossId = 0;
    gMultiplayerState.partnerBossId   = 0;
    return 1;
}

// Returns 1 if a partner is connected; 0 otherwise.
// Called from gym scripts: 'specialvar VAR_RESULT, Multiplayer_IsConnected'
// to choose the co-op waiting path vs the solo direct-battle path.
u16 Multiplayer_IsConnected(void)
{
    return (gMultiplayerState.connState == MP_STATE_CONNECTED) ? 1u : 0u;
}
