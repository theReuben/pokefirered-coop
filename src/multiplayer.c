#include "global.h"
#include "multiplayer.h"
#include "constants/multiplayer.h"
#include "constants/characters.h"
#include "constants/event_object_movement.h"
#include "event_object_movement.h"
#include "event_data.h"
#include "item.h"
#include "random.h"
#include "link.h"
#include "task.h"
#include "pokemon.h"
#include "battle_main.h"
#include "party_menu.h"
#include "overworld.h"
#include "save.h"
#include "constants/vars.h"
#include "constants/battle.h"
#include "constants/battle_frontier.h"
#include "constants/maps.h"
#include "constants/map_event_ids.h"
#include "battle.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
struct MultiplayerState gMultiplayerState;
struct CoopSettings gCoopSettings;

// Ring buffers in EWRAM.  Tauri locates them via the discovery table.
EWRAM_DATA struct MpRingBuf gMpSendRing;
EWRAM_DATA struct MpRingBuf gMpRecvRing;

// Block exchange for coop boss battles: relay shuttles SendBlock data between instances.
EWRAM_DATA struct MpBlockExchange gMpBlockExchange;

// ---------------------------------------------------------------------------
// Async event log — EWRAM to avoid IWRAM pressure.
// Events are accumulated and sent to the partner on reconnect.
// ---------------------------------------------------------------------------
struct MpEventEntry {
    u8 type;
    u8 data[3];
};
EWRAM_DATA static struct MpEventEntry sMpEventLog[MP_EVENT_LOG_SIZE];
EWRAM_DATA static u8 sMpEventLogCount;

// Address discovery table in IWRAM.  Populated once by Multiplayer_Init.
// Tauri scans IWRAM for MP_DISCOVERY_MAGIC at index 0, then reads [1]–[5].
IWRAM_DATA u32 gMpAddrTable[6];

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

u8 Mp_EncodeFlagClear(u8 *out, u16 flagId)
{
    out[0] = MP_PKT_FLAG_CLEAR;
    out[1] = (u8)(flagId >> 8);
    out[2] = (u8)(flagId);
    return MP_PKT_SIZE_FLAG_CLEAR;
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

u8 Mp_EncodeGender(u8 *out, u8 gender)
{
    out[0] = MP_PKT_GENDER;
    out[1] = gender;
    return MP_PKT_SIZE_GENDER;
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

bool8 Mp_DecodeFlagClear(const u8 *in, u8 len, u16 *flagId)
{
    if (len < MP_PKT_SIZE_FLAG_CLEAR)
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

bool8 Mp_DecodeGender(const u8 *in, u8 len, u8 *gender)
{
    if (len < MP_PKT_SIZE_GENDER)
        return FALSE;
    *gender = in[1];
    return TRUE;
}

u8 Mp_EncodeName(u8 *out, const u8 *name)
{
    u32 i;
    out[0] = MP_PKT_NAME;
    for (i = 0; i < PLAYER_NAME_LENGTH; i++)
        out[1 + i] = name[i];
    return MP_PKT_SIZE_NAME;
}

bool8 Mp_DecodeName(const u8 *in, u8 len, u8 *name)
{
    u32 i;
    if (len < MP_PKT_SIZE_NAME)
        return FALSE;
    for (i = 0; i < PLAYER_NAME_LENGTH; i++)
        name[i] = in[1 + i];
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

// Forward declarations for follower ghost helpers (defined later in the file).
static void Multiplayer_SpawnFollowerGhost(void);
static void Multiplayer_DespawnFollowerGhost(void);
static void Multiplayer_UpdateFollowerGhostPosition(void);

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

    // Any packet from the partner proves they are present.
    // Auto-establish the connection without requiring an explicit handshake packet,
    // so save-state reloads reconnect as soon as the first position update arrives.
    if (gMultiplayerState.connState != MP_STATE_CONNECTED
        && typeByte != MP_PKT_PARTNER_DISCONNECTED)
    {
        gMultiplayerState.connState = MP_STATE_CONNECTED;
        Multiplayer_SendGender();
        Multiplayer_SendName();
    }

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
            gMultiplayerState.remoteUpdateThisFrame = TRUE;
            Multiplayer_HandleRemoteFlagSet(flagId);
        }
        break;

    case MP_PKT_FLAG_CLEAR:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_FLAG_CLEAR - 1)
            return FALSE;
        pkt[0] = typeByte;
        { u8 i; for (i = 1; i < MP_PKT_SIZE_FLAG_CLEAR; i++) Mp_Pop(&gMpRecvRing, &pkt[i]); }
        if (Mp_DecodeFlagClear(pkt, MP_PKT_SIZE_FLAG_CLEAR, &flagId))
        {
            gMultiplayerState.remoteUpdateThisFrame = TRUE;
            Multiplayer_HandleRemoteFlagClear(flagId);
        }
        break;

    case MP_PKT_VAR_SET:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_VAR_SET - 1)
            return FALSE;
        pkt[0] = typeByte;
        { u8 i; for (i = 1; i < MP_PKT_SIZE_VAR_SET; i++) Mp_Pop(&gMpRecvRing, &pkt[i]); }
        if (Mp_DecodeVarSet(pkt, MP_PKT_SIZE_VAR_SET, &varId, &val))
        {
            gMultiplayerState.remoteUpdateThisFrame = TRUE;
            Multiplayer_HandleRemoteVarSet(varId, val);
        }
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

    case MP_PKT_PARTNER_CONNECTED:
        gMultiplayerState.connState = MP_STATE_CONNECTED;
        // Reset battle grace timer — partner is back.
        gMultiplayerState.battleGraceTimer = 0;
        // Announce our gender and name so the partner can render our ghost
        // correctly and display our name on interaction.
        Multiplayer_SendGender();
        Multiplayer_SendName();
        // If we're mid-battle and already sent our turn, resend it — partner
        // missed it while disconnected and is now waiting for it.
        if (Multiplayer_IsCoopBattle() && gMultiplayerState.battleTurnSent)
            Multiplayer_SendBattleTurn(
                gMultiplayerState.battleTurnSentMoveSlot,
                gMultiplayerState.battleTurnSentTarget,
                gMultiplayerState.battleTurnSentFlags);
        // Send accumulated event log to bring the reconnecting partner up to date.
        // Host authority rule: on reconnect the host sends full_sync; the guest's
        // locally-set flags are preserved because Multiplayer_ApplyFullSync ORs flags.
        Multiplayer_SendEventLog();
        Multiplayer_ClearEventLog();
        break;

    case MP_PKT_PARTNER_DISCONNECTED:
        gMultiplayerState.connState = MP_STATE_DISCONNECTED;
        Multiplayer_DespawnGhost();
        // Anti-softlock: clear shared battle/script state so we don't hang
        // waiting for a partner who has gone away.
        gMultiplayerState.partnerIsInScript  = FALSE;
        gMultiplayerState.partnerBossId      = 0;
        gMultiplayerState.battleGraceTimer   = 0;
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
        gMultiplayerState.remoteUpdateThisFrame = TRUE;
        Multiplayer_ApplyFullSync(&fullPkt[3], dataLen);
        break;
    }

    case MP_PKT_ITEM_GIVE:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_ITEM_GIVE - 1)
            return FALSE;
        {
            u8 itemHi = 0, itemLo = 0, qty = 0;
            u16 itemId;
            Mp_Pop(&gMpRecvRing, &itemHi);
            Mp_Pop(&gMpRecvRing, &itemLo);
            Mp_Pop(&gMpRecvRing, &qty);
            itemId = ((u16)itemHi << 8) | itemLo;
            if (itemId != ITEM_NONE && qty > 0)
                AddBagItem(itemId, qty);
        }
        break;

    case MP_PKT_STARTER_PICK:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_STARTER_PICK - 1)
            return FALSE;
        {
            static const u16 sBallFlags[3]   = { FLAG_HIDE_BULBASAUR_BALL,
                                                  FLAG_HIDE_SQUIRTLE_BALL,
                                                  FLAG_HIDE_CHARMANDER_BALL };
            static const u8  sBallLocalIds[3] = { LOCALID_BULBASAUR_BALL,
                                                   LOCALID_SQUIRTLE_BALL,
                                                   LOCALID_CHARMANDER_BALL };
            u8 hi = 0, lo = 0, s;
            Mp_Pop(&gMpRecvRing, &hi);
            Mp_Pop(&gMpRecvRing, &lo);
            gMultiplayerState.partnerStarterSpecies = ((u16)hi << 8) | lo;
            // Hide whichever ball the partner took — both persistently (flag) and
            // immediately on the current map (object event removal).
            for (s = 0; s < 3; s++)
            {
                if (Multiplayer_GetRandomizedStarter(s) == gMultiplayerState.partnerStarterSpecies)
                {
                    FlagSet(sBallFlags[s]);
                    RemoveObjectEventByLocalIdAndMap(
                        sBallLocalIds[s],
                        MAP_NUM(MAP_PALLET_TOWN_PROFESSOR_OAKS_LAB),
                        MAP_GROUP(MAP_PALLET_TOWN_PROFESSOR_OAKS_LAB));
                    break;
                }
            }
        }
        break;

    case MP_PKT_GENDER:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_GENDER - 1)
            return FALSE;
        {
            u8 gender = 0;
            Mp_Pop(&gMpRecvRing, &gender);
            Multiplayer_HandleRemoteGender(gender);
        }
        break;

    case MP_PKT_NAME:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_NAME - 1)
            return FALSE;
        {
            u8 name[PLAYER_NAME_LENGTH];
            u8 i;
            for (i = 0; i < PLAYER_NAME_LENGTH; i++)
                Mp_Pop(&gMpRecvRing, &name[i]);
            Multiplayer_HandleRemoteName(name);
        }
        break;

    case MP_PKT_BATTLE_TURN:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_BATTLE_TURN - 1)
            return FALSE;
        {
            u8 moveSlot = 0, target = 0, flags = 0;
            Mp_Pop(&gMpRecvRing, &moveSlot);
            Mp_Pop(&gMpRecvRing, &target);
            Mp_Pop(&gMpRecvRing, &flags);
            Multiplayer_HandleBattleTurn(moveSlot, target, flags);
        }
        break;

    case MP_PKT_FOLLOWER_GFX:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_FOLLOWER_GFX - 1)
            return FALSE;
        {
            u8 hi = 0, lo = 0;
            Mp_Pop(&gMpRecvRing, &hi);
            Mp_Pop(&gMpRecvRing, &lo);
            Multiplayer_HandleRemoteFollowerGfx(((u16)hi << 8) | lo);
        }
        break;

    case MP_PKT_PARTY_SYNC:
        {
            u8 n_mons = 0;
            u8 needed;
            // Peek n_mons without consuming it yet; need 1 (count) + n*MON_SIZE bytes total.
            if (Mp_Available(&gMpRecvRing) < 1)
                return FALSE;
            n_mons = gMpRecvRing.buf[(u8)(gMpRecvRing.tail)];
            if (n_mons > MULTI_PARTY_SIZE) n_mons = MULTI_PARTY_SIZE;
            needed = 1 + n_mons * MP_PKT_PARTY_SYNC_MON_SIZE;
            if (Mp_Available(&gMpRecvRing) < needed)
                return FALSE;
            Mp_Pop(&gMpRecvRing, &n_mons); // consume n_mons byte now that all data is ready
            if (n_mons > MULTI_PARTY_SIZE) n_mons = MULTI_PARTY_SIZE;
            {
                u8 data[MULTI_PARTY_SIZE * MP_PKT_PARTY_SYNC_MON_SIZE];
                u8 j;
                u8 dataLen = n_mons * MP_PKT_PARTY_SYNC_MON_SIZE;
                for (j = 0; j < dataLen; j++)
                    Mp_Pop(&gMpRecvRing, &data[j]);
                Multiplayer_HandleRemotePartySync(data, n_mons);
            }
        }
        break;

    case MP_PKT_PING:
        // 1-byte packet — type already consumed. No-op; relay handles timeout.
        break;

    case MP_PKT_HOST_MIGRATE:
        // Relay says we are now the host (old host disconnected).
        gMultiplayerState.role = MP_ROLE_HOST;
        // Broadcast current state to the reconnecting guest.
        Multiplayer_SendFullSync();
        Multiplayer_SendGender();
        Multiplayer_SendName();
        break;

    case MP_PKT_EVENT_LOG:
        if (Mp_Available(&gMpRecvRing) < 1)
            return FALSE;
        {
            u8 count = 0, i;
            Mp_Pop(&gMpRecvRing, &count);
            if (count > MP_EVENT_LOG_SIZE) count = MP_EVENT_LOG_SIZE;
            if (Mp_Available(&gMpRecvRing) < (u8)(count * MP_PKT_EVENT_ENTRY_SIZE))
                return FALSE;
            for (i = 0; i < count; i++)
            {
                u8 etype = 0, d0 = 0, d1 = 0, d2 = 0;
                Mp_Pop(&gMpRecvRing, &etype);
                Mp_Pop(&gMpRecvRing, &d0);
                Mp_Pop(&gMpRecvRing, &d1);
                Mp_Pop(&gMpRecvRing, &d2);
                // Future: apply partner events locally (display, map tracking, etc.)
                (void)etype; (void)d0; (void)d1; (void)d2;
            }
        }
        break;

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
    // Keep follower ghost 1 tile behind.
    Multiplayer_UpdateFollowerGhostPosition();
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
        // Don't spawn until gender is known — avoids a 1-frame "opposite-of-self"
        // wrong sprite before the partner's MP_PKT_GENDER has been processed.
        if (gMultiplayerState.ghostObjectEventId >= OBJECT_EVENTS_COUNT
            && gMultiplayerState.gotPartnerGender)
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
    gMultiplayerState.ghostObjectEventId    = GHOST_INVALID_SLOT;
    gMultiplayerState.followerGhostObjId    = GHOST_INVALID_SLOT;
    gMultiplayerState.partnerFollowerGfxId  = 0;
    gMultiplayerState.lastSentFollowerGfxId = 0;
    gMultiplayerState.bossReadyBossId    = 0;
    gMultiplayerState.partnerBossId      = 0;
    gMultiplayerState.bossResendTimer    = 0;
    gMultiplayerState.pingTimer          = 0;
    gMultiplayerState.lastCkptMapGroup   = 0xFF;
    gMultiplayerState.lastCkptMapNum     = 0xFF;
    gMultiplayerState.battleGraceTimer   = 0;
    gMultiplayerState.myStarterSpecies   = 0;
    gMultiplayerState.starterResendTimer = 0;
    gMultiplayerState.isInScript         = FALSE;
    gMultiplayerState.partnerIsInScript  = FALSE;
    gMultiplayerState.posFrameCounter    = 0;
    gMultiplayerState.partnerGender      = MALE;
    gMultiplayerState.gotPartnerGender   = FALSE;
    // Default shown if the partner hasn't yet sent their name.
    gMultiplayerState.partnerName[0] = CHAR_QUESTION_MARK;
    gMultiplayerState.partnerName[1] = CHAR_QUESTION_MARK;
    gMultiplayerState.partnerName[2] = CHAR_QUESTION_MARK;
    gMultiplayerState.partnerName[3] = EOS;
    gMultiplayerState.coopBattlePending  = FALSE;
    gCoopSettings.randomizeEncounters    = 1;
#if MP_DEBUG_TEST_SEED
    gCoopSettings.encounterSeed          = MP_DEBUG_TEST_SEED_VALUE;
#else
    gCoopSettings.encounterSeed          = 0;
#endif

    gMpSendRing.head  = 0;
    gMpSendRing.tail  = 0;
    gMpSendRing.magic = MP_RING_MAGIC;
    gMpRecvRing.head  = 0;
    gMpRecvRing.tail  = 0;
    gMpRecvRing.magic = MP_RING_MAGIC;

    // Publish all key addresses so the Tauri bridge can find them by scanning
    // IWRAM for MP_DISCOVERY_MAGIC regardless of build toolchain or IWRAM layout.
    gMpAddrTable[0] = MP_DISCOVERY_MAGIC;
    gMpAddrTable[1] = (u32)&gMultiplayerState;
    gMpAddrTable[2] = (u32)&gMpSendRing;
    gMpAddrTable[3] = (u32)&gMpRecvRing;
    gMpAddrTable[4] = (u32)&gCoopSettings;
    gMpAddrTable[5] = (u32)&gMpBlockExchange;

#if MP_DEBUG_TEST_GHOST
    gMultiplayerState.connState       = MP_STATE_CONNECTED;
    gMultiplayerState.partnerMapGroup = MP_DEBUG_TEST_MAP_GROUP;
    gMultiplayerState.partnerMapNum   = MP_DEBUG_TEST_MAP_NUM;
    gMultiplayerState.targetX         = MP_DEBUG_TEST_X;
    gMultiplayerState.targetY         = MP_DEBUG_TEST_Y;
    gMultiplayerState.targetFacing    = DIR_SOUTH;
#endif
}

// Drain and dispatch the receive ring.  Safe to call every frame from the
// main loop regardless of game state — no overworld globals are touched.
// remoteUpdateThisFrame is cleared here so TryRunOnFrameMapScript (called
// later the same frame, from the overworld callback) sees a fresh flag.
void Multiplayer_PollPackets(void)
{
    if (gMpRecvRing.magic != MP_RING_MAGIC)
        return;
    gMultiplayerState.remoteUpdateThisFrame = FALSE;
    while (ProcessOneRecvPacket()) {}
}

// Overworld-only update: ghost NPC management and outbound position send.
// Called from the overworld game loop, after Multiplayer_PollPackets has
// already consumed incoming packets for this frame.
void Multiplayer_Update(void)
{
    gMultiplayerState.remoteUpdateThisFrame = FALSE;
    while (ProcessOneRecvPacket()) {}

    GhostMapCheck();
    GhostTick();

    if (gMultiplayerState.connState == MP_STATE_CONNECTED)
    {
        gMultiplayerState.posFrameCounter++;
        if (gMultiplayerState.posFrameCounter >= 4)
            gMultiplayerState.posFrameCounter = 0;

        // Heartbeat ping every 120 frames (2 seconds) so the relay can detect
        // silent disconnects and inject PARTNER_DISCONNECTED to the other side.
        gMultiplayerState.pingTimer++;
        if (gMultiplayerState.pingTimer >= 120)
        {
            u8 pingByte = MP_PKT_PING;
            gMultiplayerState.pingTimer = 0;
            MpRing_Write(&gMpSendRing, &pingByte, MP_PKT_SIZE_PING);
        }

        // Auto-checkpoint: save on map change so progress isn't lost on disconnect.
        if (gSaveBlock1Ptr)
        {
            u8 curMapGroup = (u8)gSaveBlock1Ptr->location.mapGroup;
            u8 curMapNum   = (u8)gSaveBlock1Ptr->location.mapNum;
            if (curMapGroup != gMultiplayerState.lastCkptMapGroup
                || curMapNum  != gMultiplayerState.lastCkptMapNum)
            {
                gMultiplayerState.lastCkptMapGroup = curMapGroup;
                gMultiplayerState.lastCkptMapNum   = curMapNum;
                TrySavingData(SAVE_NORMAL);
                Multiplayer_LogEvent(MPEVENT_MAP_ENTERED, curMapGroup, curMapNum, 0);
            }
        }
    }
    if (gMultiplayerState.connState == MP_STATE_CONNECTED &&
        gMultiplayerState.posFrameCounter == 0)
    {
        Multiplayer_SendPosition();
        // Re-send gender with every position tick so the partner's ghost always
        // uses the correct sprite even if the initial gender exchange was missed.
        Multiplayer_SendGender();

        // Broadcast follower graphics ID when it changes so partner can show
        // our follower Pokémon ghost.
        {
            u16 curGfx = 0;
            const struct ObjectEvent *follower = GetFollowerObject();
            if (follower != NULL)
                curGfx = follower->graphicsId;
            if (curGfx != gMultiplayerState.lastSentFollowerGfxId)
                Multiplayer_SendFollowerGfx(curGfx);
        }
    }
}

void Multiplayer_SpawnGhostNPC(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing)
{
    u8 objId;

    if (gMultiplayerState.ghostObjectEventId < OBJECT_EVENTS_COUNT)
        Multiplayer_DespawnGhost();

    objId = SpawnSpecialObjectEventParameterized(
        Multiplayer_GhostGraphicsId(),
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
    Multiplayer_SpawnFollowerGhost();
}

u16 Multiplayer_GhostGraphicsId(void)
{
    u8 ghostGender;
    if (gMultiplayerState.gotPartnerGender)
        ghostGender = gMultiplayerState.partnerGender;
    else
        // Fall back to opposite-of-self so the ghost is always visually distinct.
        ghostGender = (gSaveBlock2Ptr->playerGender == FEMALE) ? MALE : FEMALE;
    return (ghostGender == FEMALE) ? OBJ_EVENT_GFX_GREEN_NORMAL : OBJ_EVENT_GFX_RED_NORMAL;
}

void Multiplayer_SendGender(void)
{
    u8 pkt[MP_PKT_SIZE_GENDER];
    Mp_EncodeGender(pkt, gSaveBlock2Ptr->playerGender);
    MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_GENDER);
}

void Multiplayer_SendName(void)
{
    u8 pkt[MP_PKT_SIZE_NAME];
    Mp_EncodeName(pkt, gSaveBlock2Ptr->playerName);
    MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_NAME);
}

void Multiplayer_HandleRemoteGender(u8 gender)
{
    if (gender != MALE && gender != FEMALE)
        return; // ignore bogus values
    if (gMultiplayerState.gotPartnerGender && gMultiplayerState.partnerGender == gender)
        return; // no change
    gMultiplayerState.partnerGender    = gender;
    gMultiplayerState.gotPartnerGender = TRUE;
    // Force respawn so the next GhostMapCheck picks the correct sprite.
    if (gMultiplayerState.ghostObjectEventId < OBJECT_EVENTS_COUNT)
        Multiplayer_DespawnGhost();
}

void Multiplayer_HandleRemoteName(const u8 *name)
{
    u32 i;
    for (i = 0; i < PLAYER_NAME_LENGTH; i++)
        gMultiplayerState.partnerName[i] = name[i];
    gMultiplayerState.partnerName[PLAYER_NAME_LENGTH] = EOS;
}

// ---------------------------------------------------------------------------
// Follower ghost helpers
// ---------------------------------------------------------------------------

// Compute the tile 1 step behind a given position+facing (where a follower walks).
static void FollowerBehindPos(u8 x, u8 y, u8 facing, u8 *fx, u8 *fy)
{
    *fx = x;
    *fy = y;
    switch (facing)
    {
    case DIR_SOUTH: (*fy)--; break;  // facing south → follower is 1 tile north
    case DIR_NORTH: (*fy)++; break;  // facing north → follower is 1 tile south
    case DIR_WEST:  (*fx)++; break;  // facing west  → follower is 1 tile east
    case DIR_EAST:  (*fx)--; break;  // facing east  → follower is 1 tile west
    }
}

static void Multiplayer_SpawnFollowerGhost(void)
{
    u8 fx, fy, objId;
    if (gMultiplayerState.partnerFollowerGfxId == 0) return;
    FollowerBehindPos(gMultiplayerState.targetX, gMultiplayerState.targetY,
                      gMultiplayerState.targetFacing, &fx, &fy);
    objId = SpawnSpecialObjectEventParameterized(
        (u16)gMultiplayerState.partnerFollowerGfxId,
        MOVEMENT_TYPE_NONE,
        GHOST_FOLLOWER_LOCAL_ID,
        fx, fy,
        GHOST_ELEVATION);
    if (objId >= OBJECT_EVENTS_COUNT)
        return;
    SetObjectEventDirection(&gObjectEvents[objId], gMultiplayerState.targetFacing);
    gMultiplayerState.followerGhostObjId = objId;
}

static void Multiplayer_DespawnFollowerGhost(void)
{
    u8 objId = gMultiplayerState.followerGhostObjId;
    if (objId < OBJECT_EVENTS_COUNT && gObjectEvents[objId].active)
        RemoveObjectEvent(&gObjectEvents[objId]);
    gMultiplayerState.followerGhostObjId = GHOST_INVALID_SLOT;
}

static void Multiplayer_UpdateFollowerGhostPosition(void)
{
    u8 fx, fy;
    u8 objId = gMultiplayerState.followerGhostObjId;
    if (gMultiplayerState.partnerFollowerGfxId == 0)
    {
        if (objId < OBJECT_EVENTS_COUNT)
            Multiplayer_DespawnFollowerGhost();
        return;
    }
    if (objId >= OBJECT_EVENTS_COUNT)
    {
        Multiplayer_SpawnFollowerGhost();
        return;
    }
    FollowerBehindPos(gMultiplayerState.targetX, gMultiplayerState.targetY,
                      gMultiplayerState.targetFacing, &fx, &fy);
    MoveObjectEventToMapCoords(&gObjectEvents[objId], fx, fy);
    SetObjectEventDirection(&gObjectEvents[objId], gMultiplayerState.targetFacing);
}

void Multiplayer_SendFollowerGfx(u16 gfxId)
{
    u8 pkt[MP_PKT_SIZE_FOLLOWER_GFX];
    pkt[0] = MP_PKT_FOLLOWER_GFX;
    pkt[1] = (u8)(gfxId >> 8);
    pkt[2] = (u8)(gfxId & 0xFF);
    MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_FOLLOWER_GFX);
    gMultiplayerState.lastSentFollowerGfxId = gfxId;
}

void Multiplayer_HandleRemoteFollowerGfx(u16 gfxId)
{
    gMultiplayerState.partnerFollowerGfxId = gfxId;
    // If ghost is spawned on our map, immediately update the follower ghost.
    if (gMultiplayerState.ghostObjectEventId < OBJECT_EVENTS_COUNT)
        Multiplayer_UpdateFollowerGhostPosition();
}

void Multiplayer_DespawnGhost(void)
{
    u8 objId = gMultiplayerState.ghostObjectEventId;

    Multiplayer_DespawnFollowerGhost();

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
    u8 objId = gPlayerAvatar.objectEventId;
    const struct ObjectEvent *player;
    u8 mapGroup, mapNum, x, y, facing, len;

    if (objId >= OBJECT_EVENTS_COUNT)
        return;

    player  = &gObjectEvents[objId];
    mapGroup = (u8)gSaveBlock1Ptr->location.mapGroup;
    mapNum   = (u8)gSaveBlock1Ptr->location.mapNum;
    // currentCoords are world-space (MAP tile + MAP_OFFSET=7).
    // SpawnSpecialObjectEventParameterized subtracts MAP_OFFSET internally,
    // so passing world coords places the ghost at the correct tile.
    x       = (u8)player->currentCoords.x;
    y       = (u8)player->currentCoords.y;
    facing  = (u8)player->facingDirection;
    len     = Mp_EncodePosition(pkt, mapGroup, mapNum, x, y, facing);
    MpRing_Write(&gMpSendRing, pkt, len);
}

void Multiplayer_SendFlagSet(u16 flagId)
{
    u8 pkt[MP_PKT_SIZE_FLAG_SET];
    u8 len = Mp_EncodeFlagSet(pkt, flagId);
    MpRing_Write(&gMpSendRing, pkt, len);
}

void Multiplayer_SendFlagClear(u16 flagId)
{
    u8 pkt[MP_PKT_SIZE_FLAG_CLEAR];
    u8 len = Mp_EncodeFlagClear(pkt, flagId);
    MpRing_Write(&gMpSendRing, pkt, len);
}

void Multiplayer_SendVarSet(u16 varId, u16 value)
{
    u8 pkt[MP_PKT_SIZE_VAR_SET];
    u8 len = Mp_EncodeVarSet(pkt, varId, value);
    MpRing_Write(&gMpSendRing, pkt, len);
}

void Multiplayer_OnItemGiven(u16 itemId, u8 quantity)
{
    u8 pkt[MP_PKT_SIZE_ITEM_GIVE];
    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
        return;
    pkt[0] = MP_PKT_ITEM_GIVE;
    pkt[1] = (u8)(itemId >> 8);
    pkt[2] = (u8)(itemId);
    pkt[3] = quantity;
    MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_ITEM_GIVE);
}

// ---------------------------------------------------------------------------
// Starter coordination (Phase 1.5)
// ---------------------------------------------------------------------------

// Slot-to-species mapping: 0=Bulbasaur position, 1=Squirtle position, 2=Charmander position.
u16 Multiplayer_GetStarterForBall0(void) { return Multiplayer_GetRandomizedStarter(0); }
u16 Multiplayer_GetStarterForBall1(void) { return Multiplayer_GetRandomizedStarter(1); }
u16 Multiplayer_GetStarterForBall2(void) { return Multiplayer_GetRandomizedStarter(2); }

void Multiplayer_SendStarterPick(void)
{
    u16 species = VarGet(VAR_TEMP_2); // PLAYER_STARTER_SPECIES alias
    u8 pkt[MP_PKT_SIZE_STARTER_PICK];
    pkt[0] = MP_PKT_STARTER_PICK;
    pkt[1] = (u8)(species >> 8);
    pkt[2] = (u8)(species);
    // Save for periodic resend from the waitstarterpick poll.
    if (species != 0)
    {
        gMultiplayerState.myStarterSpecies   = species;
        gMultiplayerState.starterResendTimer = 0;
    }
    if (gMultiplayerState.connState == MP_STATE_DISCONNECTED)
        return;
    MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_STARTER_PICK);
}

u16 Multiplayer_GetRivalStarterSpecies(void)
{
    u16 mine    = VarGet(VAR_TEMP_2); // PLAYER_STARTER_SPECIES
    u16 partner = gMultiplayerState.partnerStarterSpecies;
    u8 i;
    for (i = 0; i < 3; i++)
    {
        u16 s = Multiplayer_GetRandomizedStarter(i);
        if (s != mine && s != partner)
            return s;
    }
    return Multiplayer_GetRandomizedStarter(0); // fallback
}

u16 Multiplayer_GetRivalStarterSlot(void)
{
    u16 rival = Multiplayer_GetRivalStarterSpecies();
    u8 i;
    for (i = 0; i < 3; i++)
    {
        if (Multiplayer_GetRandomizedStarter(i) == rival)
            return i;
    }
    return 0;
}

// Returns a dispatch key (0/1/2) for RivalBattleDispatch when connected.
// Key 0 = rival has Charmander, 1 = Bulbasaur, 2 = Squirtle
// (matches the existing solo dispatch table: VAR_STARTER_MON 0→Charmander, 1→Bulbasaur, 2→Squirtle)
u16 Multiplayer_GetRivalBattleKey(void)
{
    return ((u16)Multiplayer_GetRivalStarterSlot() + 1u) % 3u;
}

static u16 IsBallTakenByPartner(u8 slot)
{
    u16 partner = gMultiplayerState.partnerStarterSpecies;
    return (u16)(partner != 0 && partner == Multiplayer_GetRandomizedStarter(slot));
}

u16 Multiplayer_IsBall0TakenByPartner(void) { return IsBallTakenByPartner(0); }
u16 Multiplayer_IsBall1TakenByPartner(void) { return IsBallTakenByPartner(1); }
u16 Multiplayer_IsBall2TakenByPartner(void) { return IsBallTakenByPartner(2); }

bool8 Multiplayer_NativePollPartnerStarterPick(void)
{
    // Resend our own pick every 60 frames while connected and still waiting.
    // Guards against packet loss or relay contention dropping the initial send.
    if (gMultiplayerState.connState == MP_STATE_CONNECTED
        && gMultiplayerState.partnerStarterSpecies == 0
        && gMultiplayerState.myStarterSpecies != 0)
    {
        gMultiplayerState.starterResendTimer++;
        if (gMultiplayerState.starterResendTimer >= 60)
        {
            u8 pkt[MP_PKT_SIZE_STARTER_PICK];
            gMultiplayerState.starterResendTimer = 0;
            pkt[0] = MP_PKT_STARTER_PICK;
            pkt[1] = (u8)(gMultiplayerState.myStarterSpecies >> 8);
            pkt[2] = (u8)(gMultiplayerState.myStarterSpecies);
            MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_STARTER_PICK);
        }
    }
    return (bool8)(gMultiplayerState.connState != MP_STATE_CONNECTED
                || gMultiplayerState.partnerStarterSpecies != 0);
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
    // 3-byte header + 216-byte payload = 219 bytes, fits in the 255-byte ring.
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
    for (i = FULL_SYNC_BADGES_BYTE_START; i <= FULL_SYNC_BADGES_BYTE_END; i++)
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

    // Story range: HIDE flags (bytes 4-69) use AND-merge so that if either player
    // has revealed an NPC (cleared its HIDE flag), both see it visible.
    // Story-completion flags (bytes 70-95) use OR-merge as they only accumulate.
    for (i = FULL_SYNC_STORY_BYTE_START; i <= FULL_SYNC_STORY_BYTE_END; i++)
    {
        if (i <= FULL_SYNC_STORY_HIDE_BYTE_END)
            gSaveBlock1Ptr->flags[i] &= payload[offset++];
        else
            gSaveBlock1Ptr->flags[i] |= payload[offset++];
    }
    for (i = FULL_SYNC_ITEMS_BYTE_START; i <= FULL_SYNC_ITEMS_BYTE_END; i++)
        gSaveBlock1Ptr->flags[i] |= payload[offset++];
    for (i = FULL_SYNC_BOSSES_BYTE_START; i <= FULL_SYNC_BOSSES_BYTE_END; i++)
        gSaveBlock1Ptr->flags[i] |= payload[offset++];
    for (i = FULL_SYNC_TRAINERS_BYTE_START; i <= FULL_SYNC_TRAINERS_BYTE_END; i++)
        gSaveBlock1Ptr->flags[i] |= payload[offset++];
    for (i = FULL_SYNC_BADGES_BYTE_START; i <= FULL_SYNC_BADGES_BYTE_END; i++)
        gSaveBlock1Ptr->flags[i] |= payload[offset++];
}

bool32 IsSyncableFlag(u16 flagId)
{
    return (flagId >= SYNC_FLAG_STORY_START    && flagId <= SYNC_FLAG_STORY_END)
        || (flagId >= SYNC_FLAG_ITEMS_START    && flagId <= SYNC_FLAG_ITEMS_END)
        || (flagId >= SYNC_FLAG_BOSSES_START   && flagId <= SYNC_FLAG_BOSSES_END)
        || (flagId >= SYNC_FLAG_TRAINERS_START && flagId <= SYNC_FLAG_TRAINERS_END)
        || (flagId >= SYNC_FLAG_BADGES_START   && flagId <= SYNC_FLAG_BADGES_END);
}

bool32 IsSyncableVar(u16 varId)
{
    // VAR_MAP_SCENE_* (0x4050-0x408B) control per-player scripted cutscene
    // progression.  Each player must run through intro/scene sequences
    // independently, so these must NOT sync.  Syncing them overwrites the
    // partner's scene state mid-sequence (e.g. player A picking a starter
    // advances the lab scene var and blocks player B from choosing).
    // No other var range currently needs cross-player sync; trainer/badge/
    // story state is all stored in flags, not vars.
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

// Returns one of three distinct randomized starter species for slots 0, 1, 2.
// Both players share the same seed so both see the same starters.
// Returns the canonical starter for that slot if randomization is off or seed unset.
u16 Multiplayer_GetRandomizedStarter(u8 slot)
{
    static const u16 sCanonical[3] = { 1, 7, 4 }; // Bulbasaur, Squirtle, Charmander (FRLG ball order)
    u16 results[3];
    u8 i;
    u32 state;

    if (!gCoopSettings.randomizeEncounters || !gCoopSettings.encounterSeed)
        return sCanonical[slot % 3];

    // Pick 3 distinct species by hashing seed with a per-slot salt.
    // Retry with a different salt if a duplicate is drawn.
    for (i = 0; i < 3; i++) {
        u32 salt = 0xDEAD0000u + i;
        u8 attempts = 0;
        do {
            state = gCoopSettings.encounterSeed ^ salt ^ ((u32)attempts << 8);
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            results[i] = (u16)(1u + (state % 493u));
            salt += 0x1337u;
            attempts++;
        } while (attempts < 32 && (i > 0 && (results[i] == results[0] || (i > 1 && results[i] == results[1]))));
    }
    return results[slot % 3];
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

    // Drop any unconsumed coop-battle routing: a boss-ready handshake that
    // didn't lead to a trainerbattle inside this script (e.g., the Pallet Town
    // escort scene, which just walks players into the lab) must not leak the
    // flag into the next script's trainer encounter.
    gMultiplayerState.coopBattlePending = FALSE;

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
    // Don't clobber partnerBossId if the partner already sent BOSS_READY for the
    // same boss before our script ran (issue #2 race: when both players reach
    // the trigger near-simultaneously, the partner's BOSS_READY can land in the
    // recv ring before BossReadyCommon executes, and the old unconditional
    // `partnerBossId = 0` discarded it, leaving both ROMs hanging at
    // waitbossstart).  A mismatched ID is reset so a stale ready from a
    // previous interaction doesn't false-positive the new one.
    if (gMultiplayerState.partnerBossId != bossId)
        gMultiplayerState.partnerBossId = 0;
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
void Multiplayer_BossReady_Champion(void)       { BossReadyCommon(BOSS_ID_CHAMPION); }
void Multiplayer_BossReady_RivalOaksLab(void)   { BossReadyCommon(BOSS_ID_RIVAL_OAKS_LAB); }
void Multiplayer_BossReady_RivalRoute22_1(void) { BossReadyCommon(BOSS_ID_RIVAL_ROUTE22_1); }
void Multiplayer_BossReady_RivalCerulean(void)  { BossReadyCommon(BOSS_ID_RIVAL_CERULEAN); }
void Multiplayer_BossReady_RivalSsAnne(void)    { BossReadyCommon(BOSS_ID_RIVAL_SS_ANNE); }
void Multiplayer_BossReady_RivalSilph(void)     { BossReadyCommon(BOSS_ID_RIVAL_SILPH); }
void Multiplayer_BossReady_RivalRoute22_2(void) { BossReadyCommon(BOSS_ID_RIVAL_ROUTE22_2); }
void Multiplayer_BossReady_RivalChampion(void)      { BossReadyCommon(BOSS_ID_RIVAL_CHAMPION); }
void Multiplayer_BossReady_RivalPokemonTower(void)  { BossReadyCommon(BOSS_ID_RIVAL_POKEMON_TOWER); }
void Multiplayer_BossReady_Escort(void)         { BossReadyCommon(BOSS_ID_ESCORT); }

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
    u8 myBoss = gMultiplayerState.bossReadyBossId;
    bool32 partnerReady;

    if (myBoss == 0)
        return 0; // we haven't even sent BOSS_READY yet

    // Require an EXACT boss-id match: prevents a stale partner ready from a
    // previous interaction (or a partner currently waiting on a different boss)
    // from satisfying our check.
    partnerReady = (gMultiplayerState.partnerBossId == myBoss)
                || (gMultiplayerState.connState != MP_STATE_CONNECTED);

    if (!partnerReady)
        return 0; // still waiting

    // Both ready (or solo) — clear state and tell the script to start battle.
    gMultiplayerState.bossReadyBossId = 0;
    gMultiplayerState.partnerBossId   = 0;
    // Mark the next trainerbattle to route through the coop double-battle
    // path.  Solo runs (not connected) keep the flag clear so the existing
    // single-player battle script fires normally.
    if (gMultiplayerState.connState == MP_STATE_CONNECTED)
        gMultiplayerState.coopBattlePending = TRUE;
    return 1;
}

// ---------------------------------------------------------------------------
// Co-op battle turn sync
// ---------------------------------------------------------------------------

bool32 Multiplayer_IsCoopBattle(void)
{
    return (bool32)((gBattleTypeFlags & BATTLE_TYPE_COOP) != 0);
}

void Multiplayer_SendBattleTurn(u8 moveSlot, u8 target, u8 flags)
{
    u8 pkt[MP_PKT_SIZE_BATTLE_TURN];
    pkt[0] = MP_PKT_BATTLE_TURN;
    pkt[1] = moveSlot;
    pkt[2] = target;
    pkt[3] = flags;
    MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_BATTLE_TURN);
    gMultiplayerState.battleTurnSent         = TRUE;
    gMultiplayerState.battleTurnSentMoveSlot = moveSlot;
    gMultiplayerState.battleTurnSentTarget   = target;
    gMultiplayerState.battleTurnSentFlags    = flags;
}

void Multiplayer_HandleBattleTurn(u8 moveSlot, u8 target, u8 flags)
{
    gMultiplayerState.battleTurnMoveSlot = moveSlot;
    gMultiplayerState.battleTurnTarget   = target;
    gMultiplayerState.battleTurnFlags    = flags;
    gMultiplayerState.battleTurnReceived = TRUE;
}

// Returns 1 if a partner is connected; 0 otherwise.
// Called from gym scripts: 'specialvar VAR_RESULT, Multiplayer_IsConnected'
// to choose the co-op waiting path vs the solo direct-battle path.
u16 Multiplayer_IsConnected(void)
{
    return (gMultiplayerState.connState == MP_STATE_CONNECTED) ? 1u : 0u;
}

// Native script callback for 'waitbossstart' opcode.
// Returns TRUE (resume bytecode) when both players are ready or playing solo.
// Returns FALSE (stay in NATIVE mode, yield) while still waiting.
bool8 Multiplayer_NativePollBossStart(void)
{
    // Resend BOSS_READY every 60 frames while waiting for partner, to recover
    // from the initial send being silently dropped (ring full or relay contention).
    if (gMultiplayerState.bossReadyBossId != 0
        && gMultiplayerState.partnerBossId == 0
        && gMultiplayerState.connState == MP_STATE_CONNECTED)
    {
        gMultiplayerState.bossResendTimer++;
        if (gMultiplayerState.bossResendTimer >= 60)
        {
            gMultiplayerState.bossResendTimer = 0;
            Multiplayer_SendBossReady(gMultiplayerState.bossReadyBossId);
        }
    }
    return (bool8)(Multiplayer_ScriptCheckBossStart() != 0);
}

// ---------------------------------------------------------------------------
// Party selection for co-op boss battles
// ---------------------------------------------------------------------------

// Encode gPlayerParty[0..n-1] as MultiPartnerMenuPokemon and write into dst.
// Returns number of bytes written.
static u8 EncodePartySync(u8 *dst, u8 n_mons)
{
    u8 i;
    u8 *p = dst;
    *p++ = MP_PKT_PARTY_SYNC;
    *p++ = n_mons;
    for (i = 0; i < n_mons; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        struct MultiPartnerMenuPokemon tmp;
        tmp.species     = GetMonData(mon, MON_DATA_SPECIES);
        tmp.heldItem    = GetMonData(mon, MON_DATA_HELD_ITEM);
        GetMonData(mon, MON_DATA_NICKNAME, tmp.nickname);
        tmp.level       = GetMonData(mon, MON_DATA_LEVEL);
        tmp.hp          = GetMonData(mon, MON_DATA_HP);
        tmp.maxhp       = GetMonData(mon, MON_DATA_MAX_HP);
        tmp.status      = GetMonData(mon, MON_DATA_STATUS);
        tmp.personality = GetMonData(mon, MON_DATA_PERSONALITY);
        tmp.gender      = GetMonGender(mon);
        tmp.language    = GetMonData(mon, MON_DATA_LANGUAGE);
        memcpy(p, &tmp, MP_PKT_PARTY_SYNC_MON_SIZE);
        p += MP_PKT_PARTY_SYNC_MON_SIZE;
    }
    return (u8)(p - dst);
}

void Multiplayer_SendPartySync(void)
{
    u8 pkt[MP_PKT_SIZE_PARTY_SYNC_MAX];
    u8 n = (u8)gPlayerPartyCount;
    u8 len;
    if (n > MULTI_PARTY_SIZE) n = MULTI_PARTY_SIZE;
    len = EncodePartySync(pkt, n);
    MpRing_Write(&gMpSendRing, pkt, len);
}

void Multiplayer_HandleRemotePartySync(const u8 *data, u8 n_mons)
{
    u8 i;
    if (n_mons > MULTI_PARTY_SIZE) n_mons = MULTI_PARTY_SIZE;
    for (i = 0; i < n_mons; i++)
    {
        const struct MultiPartnerMenuPokemon *src =
            (const struct MultiPartnerMenuPokemon *)(data + i * MP_PKT_PARTY_SYNC_MON_SIZE);
        gMultiPartnerParty[i] = *src;
        // Mirror into the upper gPlayerParty slots so the battle engine can read
        // HP, moves, and stats for battler 1.
        CreateMon(&gPlayerParty[MULTI_PARTY_SIZE + i], src->species, src->level,
                  src->personality, OTID_STRUCT_PRESET(0));
        SetMonData(&gPlayerParty[MULTI_PARTY_SIZE + i], MON_DATA_NICKNAME, src->nickname);
        SetMonData(&gPlayerParty[MULTI_PARTY_SIZE + i], MON_DATA_HELD_ITEM, &src->heldItem);
    }
    gMultiplayerState.partnerPartySelectDone = TRUE;
}

bool8 Multiplayer_NativePollPartySync(void)
{
    if (gMultiplayerState.connState != MP_STATE_CONNECTED
        || gMultiplayerState.partnerPartySelectDone)
    {
        gMultiplayerState.partnerPartySelectDone = FALSE;
        gMultiplayerState.partySyncResendTimer   = 0;
        return TRUE;
    }
    // Resend every 60 frames in case the single SendPartySync was dropped.
    gMultiplayerState.partySyncResendTimer++;
    if (gMultiplayerState.partySyncResendTimer >= 60)
    {
        gMultiplayerState.partySyncResendTimer = 0;
        Multiplayer_SendPartySync();
    }
    return FALSE;
}

// savedCallback set before opening the party menu.  Called when player confirms picks.
void CB2_CoopPartySelected(void)
{
    struct Pokemon scratch[MULTI_PARTY_SIZE];
    u8 i, n;

    // Count and reorder the selected mons to positions 0..n-1 in gPlayerParty.
    n = 0;
    for (i = 0; i < MAX_FRONTIER_PARTY_SIZE; i++)
    {
        u8 slot = gSelectedOrderFromParty[i];
        if (slot == 0) break;
        scratch[n++] = gPlayerParty[slot - 1];
    }
    // Write selected mons back to front of party; leave the rest unchanged
    // (battle engine only reads up to MULTI_PARTY_SIZE from gPlayerParty[0..]).
    for (i = 0; i < n; i++)
        gPlayerParty[i] = scratch[i];

    // Restore VAR_FRONTIER_FACILITY so it doesn't leak into other menus.
    VarSet(VAR_FRONTIER_FACILITY, 0);

    if (gMultiplayerState.connState == MP_STATE_CONNECTED)
        Multiplayer_SendPartySync();
    else
        gMultiplayerState.partnerPartySelectDone = TRUE; // solo: skip partner wait

    SetMainCallback2(CB2_ReturnToFieldContinueScript);
}

// ---------------------------------------------------------------------------
// Co-op boss battle block relay
// ---------------------------------------------------------------------------

// Per-frame task: if the relay has deposited a partner block, copy it into
// gBlockRecvBuffer and set gBlockReceivedStatus so the battle engine sees it.
static void Task_CoopBattleBlockRelay(u8 taskId)
{
    if (gMpBlockExchange.recvReady)
    {
        u8 from = gMpBlockExchange.fromPlayerIdx;
        if (from < MAX_RFU_PLAYERS)
        {
            memcpy(gBlockRecvBuffer[from], gMpBlockExchange.data, BLOCK_BUFFER_SIZE);
            gBlockReceivedStatus[from] = TRUE;
        }
        gMpBlockExchange.recvReady = 0;
    }
}

// Called from BattleSetup_StartCoopBattle before DoTrainerBattle().
// Clears the exchange buffer, primes the link player table so
// CB2_HandleStartMultiPartnerBattle passes its gReceivedRemoteLinkPlayers
// check, and creates the per-frame relay task.
void Multiplayer_SetupCoopBattle(void)
{
    u32 i;

    memset(&gMpBlockExchange, 0, sizeof(gMpBlockExchange));

    for (i = 0; i < MAX_RFU_PLAYERS; i++)
        gLinkPlayers[i].version = VERSION_EMERALD;
    gReceivedRemoteLinkPlayers = TRUE;

    // If the party sync exchange happened (waitpartysync completed), gMultiPartnerParty
    // and gPlayerParty[MULTI_PARTY_SIZE..] were already populated by Multiplayer_HandleRemotePartySync.
    // Fall back to cloning the local player's party only when playing solo (no sync).
    if (!gMultiplayerState.partnerPartySelectDone && gMultiplayerState.connState != MP_STATE_CONNECTED)
    {
        for (i = 0; i < MULTI_PARTY_SIZE && i < gPlayerPartyCount; i++)
        {
            gMultiPartnerParty[i].species     = GetMonData(&gPlayerParty[i], MON_DATA_SPECIES);
            gMultiPartnerParty[i].heldItem    = GetMonData(&gPlayerParty[i], MON_DATA_HELD_ITEM);
            GetMonData(&gPlayerParty[i], MON_DATA_NICKNAME, gMultiPartnerParty[i].nickname);
            gMultiPartnerParty[i].level       = GetMonData(&gPlayerParty[i], MON_DATA_LEVEL);
            gMultiPartnerParty[i].hp          = GetMonData(&gPlayerParty[i], MON_DATA_HP);
            gMultiPartnerParty[i].maxhp       = GetMonData(&gPlayerParty[i], MON_DATA_MAX_HP);
            gMultiPartnerParty[i].status      = GetMonData(&gPlayerParty[i], MON_DATA_STATUS);
            gMultiPartnerParty[i].personality = GetMonData(&gPlayerParty[i], MON_DATA_PERSONALITY);
            gPlayerParty[MULTI_PARTY_SIZE + i] = gPlayerParty[i];
        }
    }

    CreateTask(Task_CoopBattleBlockRelay, 0);
}

// ---------------------------------------------------------------------------
// Async event log
// ---------------------------------------------------------------------------

void Multiplayer_LogEvent(u8 type, u8 d0, u8 d1, u8 d2)
{
    u8 idx;
    if (sMpEventLogCount >= MP_EVENT_LOG_SIZE)
        return; // log full; oldest events stay
    idx = sMpEventLogCount++;
    sMpEventLog[idx].type    = type;
    sMpEventLog[idx].data[0] = d0;
    sMpEventLog[idx].data[1] = d1;
    sMpEventLog[idx].data[2] = d2;
}

void Multiplayer_ClearEventLog(void)
{
    sMpEventLogCount = 0;
}

void Multiplayer_SendEventLog(void)
{
    u8 pkt[MP_PKT_SIZE_EVENT_LOG_MAX];
    u8 n = sMpEventLogCount;
    u8 i;
    if (n == 0) return;
    pkt[0] = MP_PKT_EVENT_LOG;
    pkt[1] = n;
    for (i = 0; i < n; i++)
    {
        u8 base = (u8)(MP_PKT_EVENT_LOG_HDR + i * MP_PKT_EVENT_ENTRY_SIZE);
        pkt[base + 0] = sMpEventLog[i].type;
        pkt[base + 1] = sMpEventLog[i].data[0];
        pkt[base + 2] = sMpEventLog[i].data[1];
        pkt[base + 3] = sMpEventLog[i].data[2];
    }
    MpRing_Write(&gMpSendRing, pkt, (u8)(MP_PKT_EVENT_LOG_HDR + n * MP_PKT_EVENT_ENTRY_SIZE));
}

// ---------------------------------------------------------------------------
// Auto-checkpoint on battle end
// ---------------------------------------------------------------------------

void Multiplayer_OnBattleEnd(void)
{
    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
        return;
    TrySavingData(SAVE_NORMAL);
    Multiplayer_LogEvent(MPEVENT_CHECKPOINT, 0, 0, 0);
}
