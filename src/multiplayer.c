#include "global.h"
#include "multiplayer.h"
#include "constants/multiplayer.h"
#include "constants/event_object_movement.h"
#include "event_object_movement.h"
#include "event_data.h"

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
        {
            // Phase 3: apply flag on our side if syncable
            (void)flagId;
        }
        break;

    case MP_PKT_VAR_SET:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_VAR_SET - 1)
            return FALSE;
        pkt[0] = typeByte;
        { u8 i; for (i = 1; i < MP_PKT_SIZE_VAR_SET; i++) Mp_Pop(&gMpRecvRing, &pkt[i]); }
        if (Mp_DecodeVarSet(pkt, MP_PKT_SIZE_VAR_SET, &varId, &val))
        {
            (void)varId; (void)val; // Phase 3
        }
        break;

    case MP_PKT_BOSS_READY:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_BOSS_READY - 1)
            return FALSE;
        pkt[0] = typeByte;
        Mp_Pop(&gMpRecvRing, &pkt[1]);
        if (Mp_DecodeBossReady(pkt, MP_PKT_SIZE_BOSS_READY, &bossId))
        {
            (void)bossId; // Phase 5
        }
        break;

    case MP_PKT_BOSS_CANCEL:
        // 1-byte packet — type byte already consumed
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
        // Phase 3: pass fullPkt data to flag/var applicator
        (void)fullPkt;
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
    gMultiplayerState.isInScript         = FALSE;
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

bool32 IsSyncableFlag(u16 flagId)
{
    return (flagId >= SYNC_FLAG_STORY_START    && flagId <= SYNC_FLAG_STORY_END)
        || (flagId >= SYNC_FLAG_ITEMS_START    && flagId <= SYNC_FLAG_ITEMS_END)
        || (flagId >= SYNC_FLAG_BOSSES_START   && flagId <= SYNC_FLAG_BOSSES_END)
        || (flagId >= SYNC_FLAG_TRAINERS_START && flagId <= SYNC_FLAG_TRAINERS_END);
}
