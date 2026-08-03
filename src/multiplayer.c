#include "global.h"
#include "multiplayer.h"
#include "constants/multiplayer.h"
#include "constants/characters.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
#include "event_object_movement.h"
#include "event_data.h"
#include "item.h"
#include "random.h"
#include "link.h"
#include "task.h"
#include "pokemon.h"
#include "load_save.h"
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
#include "main.h"

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
// Partner's synced battle party, held OUT of gPlayerParty until battle setup.
// The partner's MP_PKT_PARTY_SYNC can arrive before the local player has even
// opened their selection menu (the two scripts run unsynchronized), so writing
// it straight into gPlayerParty[MULTI_PARTY_SIZE..] would clobber a >3-mon
// local party before the waitcoopparty stash is taken.  Copied into
// gPlayerParty by Multiplayer_SetupCoopBattle, restored out again by the
// battle-end LoadPlayerParty.
EWRAM_DATA static struct Pokemon sPartnerBattleParty[MULTI_PARTY_SIZE];

// Periodic save/sync cadences driven from Multiplayer_Update (session-tier, reset
// by Multiplayer_Init).  Zero-initialized file statics (ENGINEERING_DISCIPLINE:
// statics must be zero-init or const for the GBA link).
//  - Milestone resync: ~10s re-broadcast so a dropped incremental milestone
//    VAR_SET self-heals without a reconnect (chaos convergence, rule 5), reusing
//    the wired VAR_SET packet so it stays ROM-only.
#define MP_MILESTONE_RESYNC_FRAMES  600
static u16 sMilestoneResyncTimer;

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
static void Multiplayer_PersistStarterOutcome(void);
static void Multiplayer_RequestCheckpointSave(void);

// Apply a partner starter species learned from any packet (one-shot pick or
// state beacon).  Validates against the three legal starters, persists the
// outcome, and hides the matching ball — persistently (flag) and immediately
// on the current map (object event removal).  Idempotent.
static void ApplyPartnerStarterPick(u16 received)
{
    static const u16 sBallFlags[3]    = { FLAG_HIDE_BULBASAUR_BALL,
                                          FLAG_HIDE_SQUIRTLE_BALL,
                                          FLAG_HIDE_CHARMANDER_BALL };
    static const u8  sBallLocalIds[3] = { LOCALID_BULBASAUR_BALL,
                                          LOCALID_SQUIRTLE_BALL,
                                          LOCALID_CHARMANDER_BALL };
    u8 s;
    if (received == 0 || received == gMultiplayerState.partnerStarterSpecies)
        return;
    for (s = 0; s < 3; s++)
    {
        if (Multiplayer_GetRandomizedStarter(s) == received)
        {
            gMultiplayerState.partnerStarterSpecies = received;
            Multiplayer_PersistStarterOutcome();
            FlagSet(sBallFlags[s]);
            RemoveObjectEventByLocalIdAndMap(
                sBallLocalIds[s],
                MAP_NUM(MAP_PALLET_TOWN_PROFESSOR_OAKS_LAB),
                MAP_GROUP(MAP_PALLET_TOWN_PROFESSOR_OAKS_LAB));
            return;
        }
    }
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
        {
            // NOTE: a position packet is NOT evidence the partner is
            // trainer-free, and clearing partnerHasBusyTrainer here broke the
            // lock outright.  The pre-battle "!" + trainer-approach + intro all
            // run on the FIELD, so the partner keeps sending MP_PKT_POSITION
            // (every ~4 frames from Multiplayer_Update) throughout — each one
            // wiping the lock ~4 frames after MP_PKT_TRAINER_BUSY set it, i.e.
            // for exactly the window in which we are most likely to walk into
            // the same trainer's cone.  This is the receiver-side twin of the
            // sender-side clear removed on 2026-06-18 (see the NOTE in
            // Multiplayer_Update).  The lock is cleared only by an explicit
            // MP_PKT_TRAINER_FREE, a state beacon with MP_BEACON_BUSYTRAINER_BIT
            // clear, or partner disconnect — reliability rides the beacon.
            Multiplayer_UpdateGhostPosition(mapGroup, mapNum, x, y, facing);
        }
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
        // missed it while disconnected and is now waiting for it.  Resend, not
        // Send: the cached turn keeps its sequence number, otherwise the
        // partner would treat the replay as a brand-new turn.
        if (Multiplayer_IsCoopBattle() && gMultiplayerState.battleTurnSent)
            Multiplayer_ResendBattleTurn();
        // Send accumulated event log to bring the reconnecting partner up to date.
        Multiplayer_SendEventLog();
        Multiplayer_ClearEventLog();
        // Replay reached story milestones so a late/reconnecting partner (or one
        // that dropped the original incremental VAR_SET under packet loss) catches
        // up.  Both sides send; forward-only apply makes it idempotent and covers
        // a guest that advanced a milestone while the host was disconnected.
        Multiplayer_SendMilestoneCatchup();
        break;

    case MP_PKT_PARTNER_DISCONNECTED:
        gMultiplayerState.connState = MP_STATE_DISCONNECTED;
        Multiplayer_DespawnGhost();
        // Anti-softlock: clear shared battle/script state so we don't hang
        // waiting for a partner who has gone away.
        gMultiplayerState.partnerIsInScript      = FALSE;
        gMultiplayerState.partnerBossId          = 0;
        gMultiplayerState.battleGraceTimer       = 0;
        gMultiplayerState.partnerHasBusyTrainer  = FALSE;
        gMultiplayerState.sentBusyTrainer        = FALSE;
        // The ONLY remaining checkpoint save (autosave is otherwise gone — see
        // the note in Multiplayer_Update).  A dropped session would otherwise
        // lose everything since the player's last menu save, and here the
        // session has already ended, so the flash stall costs no play time.
        Multiplayer_RequestCheckpointSave();
        Multiplayer_LogEvent(MPEVENT_CHECKPOINT, 0, 0, 0);
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
            // Range-check the id: AddBagItem -> GetItemName asserts on
            // itemId >= ITEMS_COUNT (SanitizeItemId, item.c).  A malformed or
            // misframed packet must never assert-crash the receiver, so reject
            // out-of-range ids here alongside the existing ITEM_NONE/qty guard.
            if (itemId != ITEM_NONE && itemId < ITEMS_COUNT && qty > 0)
                AddBagItem(itemId, qty);
        }
        break;

    case MP_PKT_STARTER_PICK:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_STARTER_PICK - 1)
            return FALSE;
        {
            u8 hi = 0, lo = 0;
            Mp_Pop(&gMpRecvRing, &hi);
            Mp_Pop(&gMpRecvRing, &lo);
            ApplyPartnerStarterPick(((u16)hi << 8) | lo);
        }
        break;

    case MP_PKT_STARTER_VERDICT:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_STARTER_VERDICT - 1)
            return FALSE;
        {
            u8 verdict = 0, hi = 0, lo = 0;
            u16 species;
            Mp_Pop(&gMpRecvRing, &verdict);
            Mp_Pop(&gMpRecvRing, &hi);
            Mp_Pop(&gMpRecvRing, &lo);
            species = ((u16)hi << 8) | lo;
            // Only honor a verdict for the claim currently pending — a stray
            // or duplicate verdict (e.g. the relay's idempotent ok answer to
            // the post-givemon SendStarterPick) must not flip settled state.
            if (gMultiplayerState.starterClaimState == MP_CLAIM_PENDING
                && species == gMultiplayerState.starterClaimSpecies)
            {
                if (verdict != 0)
                {
                    gMultiplayerState.starterClaimState = MP_CLAIM_GRANTED;
                }
                else
                {
                    gMultiplayerState.starterClaimState = MP_CLAIM_DENIED;
                    // A denial means the partner owns this species — record it
                    // now (locks the ball, persists VAR_PARTNER_STARTER) so
                    // the bounce is immediate even if their starter_taken is
                    // still in flight.
                    ApplyPartnerStarterPick(species);
                }
            }
        }
        break;

    case MP_PKT_STATE_BEACON:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_STATE_BEACON - 1)
            return FALSE;
        {
            u8 gender = 0, hi = 0, lo = 0, bossId = 0;
            // Turn slot pkt[5..10]: seq + action + p0..p3.  Outside a coop
            // battle the same bytes multiplex the busy-trainer state in
            // pkt[5..8] (seq=localId, action=mapGroup, p0=mapNum, p1=present).
            u8 turnSeq = 0, action = 0, p0 = 0, p1 = 0, p2 = 0, p3 = 0;
            Mp_Pop(&gMpRecvRing, &gender);
            Mp_Pop(&gMpRecvRing, &hi);
            Mp_Pop(&gMpRecvRing, &lo);
            Mp_Pop(&gMpRecvRing, &bossId);
            Mp_Pop(&gMpRecvRing, &turnSeq);
            Mp_Pop(&gMpRecvRing, &action);
            Mp_Pop(&gMpRecvRing, &p0);
            Mp_Pop(&gMpRecvRing, &p1);
            Mp_Pop(&gMpRecvRing, &p2);
            Mp_Pop(&gMpRecvRing, &p3);
            // Bit 7 of the gender byte is the party-sync ack: the partner has
            // received our party this battle, so we can stop resending.  Latch
            // it (cleared per battle in ScrCmd_waitcoopparty) so a reordered
            // stale beacon can't flap it.  Strip it before reading the gender.
            if (gender & MP_BEACON_PARTYACK_BIT)
                gMultiplayerState.partnerGotMyParty = TRUE;
            Multiplayer_HandleRemoteGender(gender & ~MP_BEACON_PARTYACK_BIT);
            // Starter is validated inside; 0 / unknown species are ignored.
            ApplyPartnerStarterPick(((u16)hi << 8) | lo);
            // Repair a dropped BOSS_READY only.  Never clear from a beacon:
            // the drain loop can process a beacon built before the partner's
            // ready in the same frame as the ready itself, and clearing here
            // would undo it before the script poll observes it.  Clearing
            // stays event-driven (BOSS_CANCEL / disconnect / mismatch reset).
            // Only converge readiness while OUT of a coop battle (the pre-battle
            // party-selection window is where a dropped BOSS_READY is repaired).
            // Once the battle is running both sides have already passed, and the
            // sender keeps advertising its ready until its own SetupCoopBattle —
            // applying that here would strand a stale partnerBossId post-battle
            // (the beacon never clears it).  Those same bytes are turn data mid
            // coop battle anyway, guarded by turnSeq below.
            if (bossId != 0 && !Multiplayer_IsCoopBattle())
                gMultiplayerState.partnerBossId = bossId;
            // Repair a dropped MP_PKT_BATTLE_TURN: the sender re-carries its
            // cached turn in every beacon while in a coop battle.  The seq
            // dedup inside HandleBattleTurn makes repeats and reordered stale
            // beacons no-ops.  Gate on our own battle state so an overworld
            // instance never buffers a stale turn for a future battle.
            if (turnSeq != 0 && Multiplayer_IsCoopBattle())
                Multiplayer_HandleBattleAction(turnSeq, action, p0, p1, p2, p3);
            // Trainer-lock repair (overworld only — in a coop battle those same
            // bytes are turn data, and neither side can hold an overworld
            // trainer lock then).  Idempotent last-writer-wins: present bit set
            // re-arms the lock (repairs a dropped MP_PKT_TRAINER_BUSY), present
            // bit clear drops it (repairs a dropped TRAINER_FREE — the permanent
            // over-lock).  A reordered stale beacon self-corrects on the next
            // one, since the sender's beacons carry the bit set for the whole
            // duration it is fighting and clear only afterward.
            if (!Multiplayer_IsCoopBattle())
            {
                // Busy-trainer bytes stay in pkt[5..8]: seq=localId,
                // action=mapGroup, p0=mapNum, p1=present-bit.
                if (p1 & MP_BEACON_BUSYTRAINER_BIT)
                {
                    gMultiplayerState.partnerBusyTrainerLocalId  = turnSeq;
                    gMultiplayerState.partnerBusyTrainerMapGroup = action;
                    gMultiplayerState.partnerBusyTrainerMapNum   = p0;
                    gMultiplayerState.partnerHasBusyTrainer      = TRUE;
                }
                else
                {
                    gMultiplayerState.partnerHasBusyTrainer      = FALSE;
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
            u8 seq = 0, action = 0, p0 = 0, p1 = 0, p2 = 0, p3 = 0;
            Mp_Pop(&gMpRecvRing, &seq);
            Mp_Pop(&gMpRecvRing, &action);
            Mp_Pop(&gMpRecvRing, &p0);
            Mp_Pop(&gMpRecvRing, &p1);
            Mp_Pop(&gMpRecvRing, &p2);
            Mp_Pop(&gMpRecvRing, &p3);
            Multiplayer_HandleBattleAction(seq, action, p0, p1, p2, p3);
        }
        break;

    case MP_PKT_TRAINER_BUSY:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_TRAINER_BUSY - 1)
            return FALSE;
        {
            u8 localId = 0, mapGroup = 0, mapNum = 0;
            Mp_Pop(&gMpRecvRing, &localId);
            Mp_Pop(&gMpRecvRing, &mapGroup);
            Mp_Pop(&gMpRecvRing, &mapNum);
            gMultiplayerState.partnerBusyTrainerLocalId  = localId;
            gMultiplayerState.partnerBusyTrainerMapGroup = mapGroup;
            gMultiplayerState.partnerBusyTrainerMapNum   = mapNum;
            gMultiplayerState.partnerHasBusyTrainer      = TRUE;
        }
        break;

    case MP_PKT_TRAINER_FREE:
        gMultiplayerState.partnerHasBusyTrainer = FALSE;
        break;

    case MP_PKT_TRAINER_APPROACH:
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_TRAINER_APPROACH - 1)
            return FALSE;
        {
            u8 localId = 0, mapGroup = 0, mapNum = 0, direction = 0, distance = 0;
            Mp_Pop(&gMpRecvRing, &localId);
            Mp_Pop(&gMpRecvRing, &mapGroup);
            Mp_Pop(&gMpRecvRing, &mapNum);
            Mp_Pop(&gMpRecvRing, &direction);
            Mp_Pop(&gMpRecvRing, &distance);
            // Cosmetic only: replay the "!" + walk-up on the matching NPC if it's
            // on our current map.  Never locks controls or starts a battle.
            if (gSaveBlock1Ptr
                && mapGroup == (u8)gSaveBlock1Ptr->location.mapGroup
                && mapNum   == (u8)gSaveBlock1Ptr->location.mapNum)
                Multiplayer_PlayGhostTrainerApproach(localId, direction, distance);
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
            // Peek n_mons without consuming it yet; need 1 (count) + n*MON_SIZE
            // + 4 (trailing battle RNG seed) bytes total.
            if (Mp_Available(&gMpRecvRing) < 1)
                return FALSE;
            n_mons = gMpRecvRing.buf[(u8)(gMpRecvRing.tail)];
            if (n_mons > MULTI_PARTY_SIZE) n_mons = MULTI_PARTY_SIZE;
            needed = 1 + n_mons * MP_PKT_PARTY_SYNC_MON_SIZE + MP_PKT_PARTY_SYNC_SEED_SIZE;
            if (Mp_Available(&gMpRecvRing) < needed)
                return FALSE;
            Mp_Pop(&gMpRecvRing, &n_mons); // consume n_mons byte now that all data is ready
            if (n_mons > MULTI_PARTY_SIZE) n_mons = MULTI_PARTY_SIZE;
            {
                u8 data[MULTI_PARTY_SIZE * MP_PKT_PARTY_SYNC_MON_SIZE];
                u8 seedBytes[MP_PKT_PARTY_SYNC_SEED_SIZE];
                u32 battleSeed;
                u8 j;
                u8 dataLen = n_mons * MP_PKT_PARTY_SYNC_MON_SIZE;
                for (j = 0; j < dataLen; j++)
                    Mp_Pop(&gMpRecvRing, &data[j]);
                for (j = 0; j < MP_PKT_PARTY_SYNC_SEED_SIZE; j++)
                    Mp_Pop(&gMpRecvRing, &seedBytes[j]);
                battleSeed = ((u32)seedBytes[0] << 24) | ((u32)seedBytes[1] << 16)
                           | ((u32)seedBytes[2] << 8)  | seedBytes[3];
                // Ignore late/duplicate party packets once the battle is
                // underway.  The mutual-handshake resend (NativePollPartySync)
                // can land a partner's final PARTY_SYNC just after we entered
                // the battle; rebuilding then would overwrite the partner half
                // of gPlayerParty (live HP/PP/status) with the pre-battle
                // snapshot, and re-adopting the seed would desync the RNG
                // stream.  The packet is already fully drained above, so
                // framing is unaffected; gotPartnerParty stays set so our
                // beacon keeps acking.
                if (!Multiplayer_IsCoopBattle())
                {
                    // Only the host transmits a nonzero seed; adopting any
                    // nonzero value keeps both sides on the host's per-battle
                    // stream.
                    if (battleSeed != 0)
                        gMultiplayerState.coopBattleSeed = battleSeed;
                    Multiplayer_HandleRemotePartySync(data, n_mons);
                }
            }
        }
        break;

    case MP_PKT_PING:
        // 1-byte packet — type already consumed. No-op; relay handles timeout.
        break;

    case MP_PKT_ROLE_ASSIGN:
        // Relay assigns our session role on connect.  Without this both ROMs
        // sit at MP_ROLE_NONE and GetMultiplayerId()/IsLinkMaster() give the
        // same answer on both sides — no host exists for seed authority.
        if (Mp_Available(&gMpRecvRing) < MP_PKT_SIZE_ROLE_ASSIGN - 1)
            return FALSE;
        {
            u8 role = 0;
            Mp_Pop(&gMpRecvRing, &role);
            if (role == MP_ROLE_HOST || role == MP_ROLE_GUEST)
                gMultiplayerState.role = role;
        }
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
        // Unknown type — skip this byte and continue.  Draining the whole
        // ring was wrong: one bad byte destroyed every subsequent beacon.
        break;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Ghost NPC — internal helpers
// ---------------------------------------------------------------------------

// Beyond this Chebyshev distance the main ghost teleports to the target instead
// of walking one tile at a time.  A regular (non-coop) battle freezes the ghost
// (GhostTick only runs on the overworld) while the partner keeps moving; on
// battle exit the whole position backlog drains in one frame and the target
// jumps far away.  Without a snap the ghost visibly slides across the gap one
// tile per held movement ("catch-up ghosting", user-reported 2026-06-17).  The
// threshold sits above worst-case running lag (~2-3 tiles) so ordinary movement
// still follows smoothly, but below the typical post-battle gap (10+ tiles).
#define GHOST_SNAP_DISTANCE 5

// Returns the MOVEMENT_ACTION_WALK_NORMAL_* constant that steps `obj` one tile
// toward (tx, ty), or 0xFF if it is already there.
static u8 StepActionToward(const struct ObjectEvent *obj, u8 tx, u8 ty)
{
    s16 dx = (s16)tx - obj->currentCoords.x;
    s16 dy = (s16)ty - obj->currentCoords.y;

    if (dx == 0 && dy == 0)
        return 0xFF; // at target

    // Prioritise horizontal movement when both axes differ to match normal walk feel.
    if (dx > 0)  return MOVEMENT_ACTION_WALK_NORMAL_RIGHT;
    if (dx < 0)  return MOVEMENT_ACTION_WALK_NORMAL_LEFT;
    if (dy > 0)  return MOVEMENT_ACTION_WALK_NORMAL_DOWN;
    return MOVEMENT_ACTION_WALK_NORMAL_UP;
}

// Walks a network-driven ghost object one tile per held movement toward
// (tx, ty), snapping instead when it has fallen too far behind, and facing
// `faceIfIdle` once it arrives.
//
// Both ghosts MUST go through this rather than teleporting per frame: a held
// movement is what drives the walking sprite animation, and a bare
// SetObjectEventDirection only writes the facingDirection field — it never
// touches the sprite, so the object keeps whatever animation frame it was
// spawned with. ObjectEventTurn does the missing StartSpriteAnim half. (This
// pair of omissions is why the follower ghost rendered permanently down-facing
// and never animated, and why partner turns-in-place were invisible.)
static void DriveGhostToward(struct ObjectEvent *obj, u8 tx, u8 ty, u8 faceIfIdle)
{
    s16 dx  = (s16)tx - obj->currentCoords.x;
    s16 dy  = (s16)ty - obj->currentCoords.y;
    s16 adx = dx < 0 ? -dx : dx;
    s16 ady = dy < 0 ? -dy : dy;
    u8 action;

    // Snap on a large positional jump (battle-exit / warp backlog drain) instead
    // of sliding one tile at a time. Checked before the heldMovementActive guard
    // so an in-progress slide is pre-empted.
    if ((adx > ady ? adx : ady) >= GHOST_SNAP_DISTANCE)
    {
        MoveObjectEventToMapCoords(obj, tx, ty);
        ObjectEventTurn(obj, faceIfIdle);
        return;
    }

    ObjectEventClearHeldMovementIfFinished(obj);

    if (obj->heldMovementActive)
        return;

    action = StepActionToward(obj, tx, ty);
    if (action == 0xFF)
    {
        ObjectEventTurn(obj, faceIfIdle);
        return;
    }

    ObjectEventSetHeldMovement(obj, action);
}

// Steps the ghost one tile towards its target each frame.
static void GhostTick(void)
{
    u8 objId = gMultiplayerState.ghostObjectEventId;
    struct ObjectEvent *ghost;

    if (objId >= OBJECT_EVENTS_COUNT || !gObjectEvents[objId].active)
        return;

    // NOTE: the ghost is NOT frozen while the partner is in a script.  It used
    // to early-return on partnerIsInScript, but that broke player-moving
    // cutscenes (bug #15): during the Oak starter escort the partner is in a
    // script AND being walked by it, so freezing left the ghost stuck at the
    // door while the real player advanced.  The ghost simply tracks its target
    // tile here; a stationary NPC interaction produces no target change, so the
    // ghost idles on its own — no freeze needed.  The interaction mutex
    // (Multiplayer_IsPartnerInScript) still blocks talking to the same NPC.
    ghost = &gObjectEvents[objId];

    DriveGhostToward(ghost, gMultiplayerState.targetX, gMultiplayerState.targetY,
                     gMultiplayerState.targetFacing);

    // Keep the follower ghost 1 tile behind.  Ticked EVERY frame, not only on
    // the frames the ghost itself steps: the follower runs its own held
    // movement now, so it needs a tick to finish and to start the next one.
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
        // Only spawn once we know the partner's gender so the sprite is correct
        // from the first frame.  Gender is sent with every position tick, so the
        // ghost appears within one relay cycle (~100 ms) of the partner arriving.
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
    sMilestoneResyncTimer = 0;
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
    gMultiplayerState.coopPartyStashed   = FALSE;
    gMultiplayerState.coopSelectedCount  = 0;
    gMultiplayerState.pingTimer          = 0;
    gMultiplayerState.lastCkptMapGroup   = 0xFF;
    gMultiplayerState.lastCkptMapNum     = 0xFF;
    gMultiplayerState.saveState          = 0; // MP_SAVE_IDLE (enum defined below)
    // Field-trainer lock (session scratch — clear on init/savestate reload so a
    // stale lock never survives a reconnect; the beacon re-derives it).
    gMultiplayerState.sentBusyTrainer          = FALSE;
    gMultiplayerState.partnerHasBusyTrainer    = FALSE;
    gMultiplayerState.sentBusyTrainerLocalId   = 0;
    gMultiplayerState.sentBusyTrainerMapGroup  = 0;
    gMultiplayerState.sentBusyTrainerMapNum    = 0;
    gMultiplayerState.partnerBusyTrainerLocalId  = 0;
    gMultiplayerState.partnerBusyTrainerMapGroup = 0;
    gMultiplayerState.partnerBusyTrainerMapNum   = 0;
    gMultiplayerState.battleGraceTimer   = 0;
    gMultiplayerState.myStarterSpecies      = 0;
    gMultiplayerState.partnerStarterSpecies = 0;
    gMultiplayerState.starterResendTimer    = 0;
    gMultiplayerState.starterClaimState     = MP_CLAIM_IDLE;
    gMultiplayerState.starterClaimSpecies   = 0;
    gMultiplayerState.starterClaimTimer     = 0;
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
    gMultiplayerState.battleTurnReceived   = FALSE;
    gMultiplayerState.battleTurnSent       = FALSE;
    gMultiplayerState.battleTurnSeqOut     = 0;
    gMultiplayerState.battleTurnSeqApplied = 0;
    gMultiplayerState.coopBattleSeed       = 0;
    gMultiplayerState.coopRngState         = 0;
    gMultiplayerState.gotPartnerParty      = FALSE;
    gMultiplayerState.partnerGotMyParty    = FALSE;
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

// Frame-guarded wrapper around Multiplayer_Update.  Multiple engine hooks
// call this — CB2_Overworld and the script engine's native step — so that
// packet draining never depends on which main callback happens to be active
// (the root cause of three separate 'wait command hangs during fade' bugs).
// Only the first call per VBlank frame does work.
void Multiplayer_UpdateOncePerFrame(void)
{
    // Zero-initialized (.bss): the GBA link script discards .data, so a
    // nonzero static initializer here fails to link ("defined in discarded
    // section").  Stores frame+1 so 0 means "never ran" and the first call
    // always proceeds.
    static u32 sLastUpdateFramePlus1;
    if (gMain.vblankCounter1 + 1 == sLastUpdateFramePlus1)
        return;
    sLastUpdateFramePlus1 = gMain.vblankCounter1 + 1;
    Multiplayer_Update();
}

// Heartbeat ping every 120 frames (2s) so the relay can detect silent
// disconnects, plus the idempotent session-state beacon every
// MP_BEACON_INTERVAL_FRAMES: gender + starter pick + boss readiness +
// (in coop battles) the cached battle turn.  The beacon is the repair
// channel for every dropped one-shot exchange — any new reliability need
// extends its payload rather than adding a resend timer.
// Shared by the overworld pump (Multiplayer_Update) and the battle pump
// (Multiplayer_BattleTick); only ever call once per frame.
static void TickPingAndBeacon(void)
{
    gMultiplayerState.pingTimer++;
    if (gMultiplayerState.pingTimer >= 120)
    {
        u8 pingByte = MP_PKT_PING;
        gMultiplayerState.pingTimer = 0;
        MpRing_Write(&gMpSendRing, &pingByte, MP_PKT_SIZE_PING);
    }

    if (++gMultiplayerState.starterResendTimer >= MP_BEACON_INTERVAL_FRAMES)
    {
        u8 pkt[MP_PKT_SIZE_STATE_BEACON];
        gMultiplayerState.starterResendTimer = 0;
        pkt[0] = MP_PKT_STATE_BEACON;
        // Bit 7 = party-sync ack: tells the partner we have its party this
        // battle so it can stop resending (asymmetric-loss deadlock repair).
        // Low bits remain the player gender.
        pkt[1] = gSaveBlock2Ptr->playerGender;
        if (gMultiplayerState.gotPartnerParty)
            pkt[1] |= MP_BEACON_PARTYACK_BIT;
        pkt[2] = (u8)(gMultiplayerState.myStarterSpecies >> 8);
        pkt[3] = (u8)(gMultiplayerState.myStarterSpecies);
        pkt[4] = gMultiplayerState.bossReadyBossId;
        // Re-carry the cached battle turn while in a coop battle so a
        // dropped MP_PKT_BATTLE_TURN converges on the next beacon (the
        // receiver dedups by seq).  Zeroed otherwise — turn_seq 0 means
        // "no cached turn" on the wire.
        if (Multiplayer_IsCoopBattle() && gMultiplayerState.battleTurnSent)
        {
            // Full tagged action so the beacon repairs SWITCH/REPLACE/ITEM
            // turns, not just MOVE: seq + action + p0..p3.
            pkt[5]  = gMultiplayerState.battleTurnSeqOut;
            pkt[6]  = gMultiplayerState.battleTurnSentAction;
            pkt[7]  = gMultiplayerState.battleTurnSentMoveSlot;
            pkt[8]  = gMultiplayerState.battleTurnSentTarget;
            pkt[9]  = gMultiplayerState.battleTurnSentFlags;
            pkt[10] = gMultiplayerState.battleTurnSentP3;
        }
        else if (gMultiplayerState.sentBusyTrainer)
        {
            // Outside a coop battle the turn bytes are idle, so re-carry our
            // field-trainer lock here: a dropped MP_PKT_TRAINER_BUSY re-arms and
            // a dropped TRAINER_FREE clears on the partner within one beacon
            // interval.  The present bit (byte 8) disambiguates from a coop turn.
            // Stays in pkt[5..8] (matching the parse); pkt[9..10] zeroed.
            pkt[5]  = gMultiplayerState.sentBusyTrainerLocalId;
            pkt[6]  = gMultiplayerState.sentBusyTrainerMapGroup;
            pkt[7]  = gMultiplayerState.sentBusyTrainerMapNum;
            pkt[8]  = MP_BEACON_BUSYTRAINER_BIT;
            pkt[9]  = 0;
            pkt[10] = 0;
        }
        else
        {
            pkt[5]  = 0;
            pkt[6]  = 0;
            pkt[7]  = 0;
            pkt[8]  = 0;
            pkt[9]  = 0;
            pkt[10] = 0;
        }
        MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_STATE_BEACON);
    }
}

// Battle-safe multiplayer pump.  During a battle neither the overworld loop
// nor the script engine runs, so NOTHING pumped the transport: no packet
// polling outside the partner controller's ChooseMove window, no heartbeat
// ping (the relay's silence detection went dark for the whole battle), and
// no state beacon (the battle-turn repair channel never fired — found live
// 2026-06-12: a wiped turn was never re-applied).  Called every battle frame
// from BattleMainCB2.  NOT hookable via Task_CoopBattleBlockRelay: battle
// init's ResetTasks() (CB2_InitBattleInternal) destroys that task right
// after Multiplayer_SetupCoopBattle creates it.
// Deliberately excludes the overworld-only work in Multiplayer_Update:
// ghost/object-event management, position sends, starter recovery, and the
// auto-checkpoint (TrySavingData mid-battle is unsafe).
void Multiplayer_BattleTick(void)
{
    // Pump during ANY battle while connected, not only coop battles.  A regular
    // wild/trainer battle blocks the overworld loop too: without pumping here the
    // recv ring fills with the partner's position packets (silently dropped on
    // overflow) and the heartbeat goes dark (false-disconnect risk).  Draining
    // the ring also keeps the ghost's target current, shrinking the gap the
    // GHOST_SNAP_DISTANCE snap covers on battle exit, and lets TickPingAndBeacon
    // re-carry the busy-trainer lock (#18a) during the field-trainer battle.
    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
        return;
    Multiplayer_PollPackets();
    TickPingAndBeacon();
}

// Per-frame transport pump for the party menu, which runs its own main callback
// (CB2_UpdatePartyMenu) so neither Multiplayer_Update (overworld) nor
// Multiplayer_BattleTick (battle) runs.  Without it the recv ring is never
// drained while the menu is open: under sustained beacon + partner party-resend
// traffic it overflows and corrupts, and (separately) the relay's silence
// detector would false-disconnect a slow chooser.  Found 2026-06-14: the
// overflow misframed a packet into a bogus MP_PKT_ITEM_GIVE and asserted in
// item.c:782.  Drains recv + sends ping/beacon only (none of the overworld
// object-event/position work, which is unsafe and irrelevant in a menu).
// Safe for ANY party menu and a no-op when disconnected.
void Multiplayer_MenuTick(void)
{
    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
        return;
    Multiplayer_PollPackets();
    TickPingAndBeacon();
}

// Async auto-checkpoint save state machine.  TrySavingData(SAVE_NORMAL)
// busy-loops all NUM_SECTORS_PER_SLOT (14) flash sectors in a single frame,
// which is the visible stall at scene transitions.  Instead we drive the
// link save's incremental primitives one sector per frame, advanced from
// Multiplayer_Update (once per frame via Multiplayer_UpdateOncePerFrame).
// We deliberately OMIT the SetLinkStandbyCallback/IsLinkTaskFinished steps of
// Task_LinkFullSave: our "link" is the virtual relay, with no real partner to
// perform the standby handshake, so those steps would block forever.  The
// resulting on-disk write is the full save slot — equivalent to
// TrySavingData(SAVE_NORMAL).  gSoftResetDisabled is left untouched to match
// the old synchronous autosave (which also never set it), so a stalled
// machine can never strand soft-reset disabled.
enum {
    MP_SAVE_IDLE = 0,
    MP_SAVE_INIT,
    MP_SAVE_WRITE,
    MP_SAVE_REPLACE,
    MP_SAVE_SIGNATURE,
};

// Kick off an auto-checkpoint save if one isn't already running.  Exactly one
// caller remains: the MP_PKT_PARTNER_DISCONNECTED handler.  The periodic,
// per-warp, story-milestone and battle-end callers were all removed because
// each flash sector's host-side sync stalls the emulator thread.
static void Multiplayer_RequestCheckpointSave(void)
{
    if (gMultiplayerState.saveState == MP_SAVE_IDLE)
        gMultiplayerState.saveState = MP_SAVE_INIT;
}

// Advance the async save by one step.  No-op when idle.  Called unconditionally
// each frame (even while disconnected) so an in-flight save always finishes.
static void Multiplayer_TickAsyncSave(void)
{
    switch (gMultiplayerState.saveState)
    {
    case MP_SAVE_INIT:
        LinkFullSave_Init();
        gMultiplayerState.saveState = MP_SAVE_WRITE;
        break;
    case MP_SAVE_WRITE:
        // Writes ONE sector per call; returns TRUE once the last is written.
        if (LinkFullSave_WriteSector())
            gMultiplayerState.saveState = MP_SAVE_REPLACE;
        break;
    case MP_SAVE_REPLACE:
        LinkFullSave_ReplaceLastSector();
        gMultiplayerState.saveState = MP_SAVE_SIGNATURE;
        break;
    case MP_SAVE_SIGNATURE:
        LinkFullSave_SetLastSectorSignature();
        gMultiplayerState.saveState = MP_SAVE_IDLE;
        break;
    case MP_SAVE_IDLE:
    default:
        break;
    }
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

    // Auto-recover myStarterSpecies from VAR_STARTER_MON when it's been cleared
    // (e.g., save-state reload after Multiplayer_Init zeroed the field).
    // VAR_STARTER_MON lives in the save block and survives reloads.
    // Gated on FLAG_SYS_POKEMON_GET: VAR_STARTER_MON defaults to 0, which is
    // also a valid slot (Bulbasaur), so recovering before the player actually
    // owns a starter fabricates a phantom Bulbasaur pick that the background
    // resend then broadcasts — hiding the partner's Bulbasaur ball.
    if (gMultiplayerState.myStarterSpecies == 0 && FlagGet(FLAG_SYS_POKEMON_GET))
    {
        u16 slot = VarGet(VAR_STARTER_MON);
        if (slot <= 2)
        {
            gMultiplayerState.myStarterSpecies = Multiplayer_GetRandomizedStarter(slot);
            Multiplayer_PersistStarterOutcome();
        }
    }

    // Recover the partner's pick from its persisted var so reloads/reconnects
    // don't depend on the partner re-sending it.  Routed through
    // ApplyPartnerStarterPick so the matching ball is hidden as well.
    if (gMultiplayerState.partnerStarterSpecies == 0)
    {
        u16 saved = VarGet(VAR_PARTNER_STARTER);
        if (saved != 0)
            ApplyPartnerStarterPick(saved);
    }

    // While disconnected, periodically send a ping so the partner's ROM can
    // auto-connect on first packet arrival without needing the relay to inject
    // a bootstrap packet.  Without this, both ROMs stay DISCONNECTED forever
    // because position/gender are only sent in the CONNECTED block below.
    if (gMultiplayerState.connState == MP_STATE_DISCONNECTED)
    {
        gMultiplayerState.pingTimer++;
        if (gMultiplayerState.pingTimer >= 120)
        {
            u8 pingByte = MP_PKT_PING;
            gMultiplayerState.pingTimer = 0;
            MpRing_Write(&gMpSendRing, &pingByte, MP_PKT_SIZE_PING);
        }
    }

    if (gMultiplayerState.connState == MP_STATE_CONNECTED)
    {
        gMultiplayerState.posFrameCounter++;
        if (gMultiplayerState.posFrameCounter >= 4)
            gMultiplayerState.posFrameCounter = 0;

        // Heartbeat ping + idempotent session-state beacon (shared with the
        // mid-battle pump in Multiplayer_BattleTick).
        TickPingAndBeacon();

        // Map-change detection for the reconnect event log.
        //
        // AUTOSAVE IS GONE.  A full checkpoint is a 14-sector flash write, and
        // each sector's host-side flash sync stalls the emulator thread, so
        // wherever a checkpoint fired the game visibly hitched.  The per-warp
        // save was removed first (the "slow area transition" bug); the
        // battle-end, story-milestone and periodic ~5-min saves that replaced
        // it had exactly the same effect one step later — the battle-end one
        // landing on the return-to-overworld fade of every wild battle.
        //
        // Progress is now durable only via the normal in-game save menu, plus
        // one checkpoint on partner disconnect (MP_PKT_PARTNER_DISCONNECTED),
        // where play has already stopped so the stall costs nothing.
        if (gSaveBlock1Ptr)
        {
            u8 curMapGroup = (u8)gSaveBlock1Ptr->location.mapGroup;
            u8 curMapNum   = (u8)gSaveBlock1Ptr->location.mapNum;
            if (curMapGroup != gMultiplayerState.lastCkptMapGroup
                || curMapNum  != gMultiplayerState.lastCkptMapNum)
            {
                gMultiplayerState.lastCkptMapGroup = curMapGroup;
                gMultiplayerState.lastCkptMapNum   = curMapNum;
                Multiplayer_LogEvent(MPEVENT_MAP_ENTERED, curMapGroup, curMapNum, 0);
            }
        }

        // Periodic milestone re-broadcast — self-heals a dropped incremental
        // milestone VAR_SET mid-session (see MP_MILESTONE_RESYNC_FRAMES above).
        sMilestoneResyncTimer++;
        if (sMilestoneResyncTimer >= MP_MILESTONE_RESYNC_FRAMES)
        {
            sMilestoneResyncTimer = 0;
            Multiplayer_SendMilestoneCatchup();
        }
    }
    if (gMultiplayerState.connState == MP_STATE_CONNECTED &&
        gMultiplayerState.posFrameCounter == 0)
    {
        Multiplayer_SendPosition();
        // Gender travels in the state beacon (and the connect-time exchange);
        // the per-frame flood and per-position piggyback are gone.

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

    // NOTE: the field-trainer lock is released in Multiplayer_OnBattleEnd, NOT
    // here.  A `sentBusyTrainer && !gMain.inBattle` poll at this point used to
    // fire during the pre-battle "!"+intro (which runs on the FIELD, inBattle
    // still FALSE) and cleared the lock ~1 frame after SendTrainerBusy set it —
    // before the battle even began — so the partner's vision-cone suppression
    // never engaged (observed 2026-06-18 on the forest-trainer fixture).

    // Advance any in-flight async checkpoint save (no-op when idle).  Run
    // unconditionally so a save started before a disconnect still completes.
    Multiplayer_TickAsyncSave();
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
    ObjectEventTurn(&gObjectEvents[objId], facing);
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

// The follower ghost is, by definition, the partner's lead Pokémon, whose
// overworld graphics id is (species | OBJ_EVENT_MON).  A value of 0 means "no
// follower"; a value lacking the OBJ_EVENT_MON bit is NOT a mon id and would
// resolve to an arbitrary NPC/object graphic (the cuttable-tree / NPC sprite
// symptom in bug #21).  Reject anything that is not a valid mon gfx id so the
// follower ghost only ever renders a Pokémon.
static bool32 Multiplayer_IsValidFollowerGfx(u16 gfxId)
{
    return gfxId != 0 && (gfxId & OBJ_EVENT_MON);
}

static void Multiplayer_SpawnFollowerGhost(void)
{
    u8 fx, fy, objId;
    if (!Multiplayer_IsValidFollowerGfx(gMultiplayerState.partnerFollowerGfxId)) return;
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
    // ObjectEventTurn, not SetObjectEventDirection: the latter sets the facing
    // fields only and leaves the sprite on its spawn animation frame.
    ObjectEventTurn(&gObjectEvents[objId], gMultiplayerState.targetFacing);
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
    if (!Multiplayer_IsValidFollowerGfx(gMultiplayerState.partnerFollowerGfxId))
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
    // Follow the partner GHOST's tile, not the raw network target: the ghost
    // deliberately lags the target by up to a few tiles, and trailing the
    // target directly is what made the follower teleport rather than walk.
    // Falling back to the target keeps the follower sane if the ghost is gone.
    {
        u8 ghostId = gMultiplayerState.ghostObjectEventId;
        u8 leadX, leadY, leadFacing;

        if (ghostId < OBJECT_EVENTS_COUNT && gObjectEvents[ghostId].active)
        {
            leadX      = (u8)gObjectEvents[ghostId].currentCoords.x;
            leadY      = (u8)gObjectEvents[ghostId].currentCoords.y;
            leadFacing = (u8)gObjectEvents[ghostId].facingDirection;
        }
        else
        {
            leadX      = gMultiplayerState.targetX;
            leadY      = gMultiplayerState.targetY;
            leadFacing = gMultiplayerState.targetFacing;
        }

        FollowerBehindPos(leadX, leadY, leadFacing, &fx, &fy);
        // Idle facing is the leader's facing, which from the tile directly
        // behind the leader is also "face the leader" — correct on corners too.
        DriveGhostToward(&gObjectEvents[objId], fx, fy, leadFacing);
    }
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

void Multiplayer_SendTrainerBusy(u8 localId, u8 mapGroup, u8 mapNum)
{
    u8 pkt[MP_PKT_SIZE_TRAINER_BUSY];
    pkt[0] = MP_PKT_TRAINER_BUSY;
    pkt[1] = localId;
    pkt[2] = mapGroup;
    pkt[3] = mapNum;
    MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_TRAINER_BUSY);
    gMultiplayerState.sentBusyTrainer = TRUE;
    // Remember which trainer we locked so the state beacon can re-carry it and
    // repair a dropped BUSY/FREE on the partner's side (MP_BEACON_BUSYTRAINER_BIT).
    gMultiplayerState.sentBusyTrainerLocalId  = localId;
    gMultiplayerState.sentBusyTrainerMapGroup = mapGroup;
    gMultiplayerState.sentBusyTrainerMapNum   = mapNum;
}

void Multiplayer_SendTrainerApproach(u8 localId, u8 mapGroup, u8 mapNum, u8 direction, u8 distance)
{
    u8 pkt[MP_PKT_SIZE_TRAINER_APPROACH];
    pkt[0] = MP_PKT_TRAINER_APPROACH;
    pkt[1] = localId;
    pkt[2] = mapGroup;
    pkt[3] = mapNum;
    pkt[4] = direction;
    pkt[5] = distance;
    MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_TRAINER_APPROACH);
}

bool32 Multiplayer_IsPartnerBusyWithTrainer(u8 objectEventId)
{
    if (!gMultiplayerState.partnerHasBusyTrainer)
        return FALSE;
    return gObjectEvents[objectEventId].localId   == gMultiplayerState.partnerBusyTrainerLocalId &&
           gObjectEvents[objectEventId].mapGroup  == gMultiplayerState.partnerBusyTrainerMapGroup &&
           gObjectEvents[objectEventId].mapNum    == gMultiplayerState.partnerBusyTrainerMapNum;
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
    // Derive the species from VAR_STARTER_MON (the ball slot, written by the
    // pick script just before this special runs) rather than VAR_TEMP_2:
    // the rival battle triggers reuse VAR_TEMP_2 for the player's column, so
    // reading it here broadcasts garbage if call order ever changes.  The
    // FLAG_SYS_POKEMON_GET gate disambiguates slot 0 from "no pick yet".
    u16 species = 0;
    u16 slot = VarGet(VAR_STARTER_MON);
    u8 pkt[MP_PKT_SIZE_STARTER_PICK];
    if (FlagGet(FLAG_SYS_POKEMON_GET) && slot <= 2)
        species = Multiplayer_GetRandomizedStarter((u8)slot);
    if (species == 0)
        return; // nothing valid to announce
    gMultiplayerState.myStarterSpecies   = species;
    gMultiplayerState.starterResendTimer = 0;
    Multiplayer_PersistStarterOutcome();
    if (gMultiplayerState.connState == MP_STATE_DISCONNECTED)
        return;
    pkt[0] = MP_PKT_STARTER_PICK;
    pkt[1] = (u8)(species >> 8);
    pkt[2] = (u8)(species);
    MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_STARTER_PICK);
}

// Persist the partner's pick and, once both picks are known, the rival's
// species into saved game vars.  The saved vars are the durable source of
// truth: they survive save/reload and reconnects, unlike the IWRAM mirrors
// in gMultiplayerState which Multiplayer_Init zeroes.
static void Multiplayer_PersistStarterOutcome(void)
{
    u16 mine    = gMultiplayerState.myStarterSpecies;
    u16 partner = gMultiplayerState.partnerStarterSpecies;
    u8 i;
    if (partner != 0 && VarGet(VAR_PARTNER_STARTER) != partner)
        VarSet(VAR_PARTNER_STARTER, partner);
    if (mine == 0 || partner == 0 || VarGet(VAR_RIVAL_STARTER) != 0)
        return;
    for (i = 0; i < 3; i++)
    {
        u16 s = Multiplayer_GetRandomizedStarter(i);
        if (s != mine && s != partner)
        {
            VarSet(VAR_RIVAL_STARTER, s);
            return;
        }
    }
}

u16 Multiplayer_GetRivalStarterSpecies(void)
{
    // The persisted result wins — it survives save/reload and reconnects.
    u16 saved = VarGet(VAR_RIVAL_STARTER);
    u16 mine, partner;
    u8 i;
    if (saved != 0)
        return saved;
    // Not yet persisted: derive from the session picks.  Use myStarterSpecies
    // (set at pick time) not VAR_TEMP_2: the RivalBattleTrigger scripts
    // overwrite VAR_TEMP_2 with ball position (1/2/3) before this is called.
    mine    = gMultiplayerState.myStarterSpecies;
    partner = gMultiplayerState.partnerStarterSpecies;
    for (i = 0; i < 3; i++)
    {
        u16 s = Multiplayer_GetRandomizedStarter(i);
        if (s != mine && s != partner)
        {
            if (mine != 0 && partner != 0)
                VarSet(VAR_RIVAL_STARTER, s);
            return s;
        }
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

// Re-derive the three starter-ball hide flags from durable state instead of
// trusting whatever an earlier session left in the save.  A ball is hidden
// iff its starter is already taken: ours via VAR_STARTER_MON (gated on
// FLAG_SYS_POKEMON_GET so slot 0's default isn't a pick), the partner's via
// VAR_PARTNER_STARTER.  Called from the lab OnTransition during the
// selection scenes; from scene 3 onward the vanilla removeobject-set flags
// are final and are left untouched.
void Multiplayer_RederiveStarterBallFlags(void)
{
    static const u16 sBallFlags[3] = { FLAG_HIDE_BULBASAUR_BALL,
                                       FLAG_HIDE_SQUIRTLE_BALL,
                                       FLAG_HIDE_CHARMANDER_BALL };
    u16 partner = VarGet(VAR_PARTNER_STARTER);
    u8 i;
    for (i = 0; i < 3; i++)
    {
        FlagClear(sBallFlags[i]);
        if (partner != 0 && Multiplayer_GetRandomizedStarter(i) == partner)
            FlagSet(sBallFlags[i]);
    }
    if (FlagGet(FLAG_SYS_POKEMON_GET))
    {
        u16 slot = VarGet(VAR_STARTER_MON);
        if (slot <= 2)
            FlagSet(sBallFlags[slot]);
    }
}

bool8 Multiplayer_NativePollPartnerStarterPick(void)
{
    // Packet draining is handled by the script engine's native step
    // (RunScriptCommand calls Multiplayer_UpdateOncePerFrame before every
    // native poll), so this runs even when CB2_Overworld isn't active.
    // A dropped initial pick exchange is repaired by the state beacon.
    return (bool8)(gMultiplayerState.connState != MP_STATE_CONNECTED
                || gMultiplayerState.partnerStarterSpecies != 0);
}

// Claim a starter before it is given.  The pick script calls this (via
// special) right after the player's YES, with the ball slot in
// gSpecialVar_0x8004, and then blocks in waitstarterclaim until the claim
// resolves.  The wire message IS the ordinary MP_PKT_STARTER_PICK — the relay
// already arbitrates it (first pick wins); what is new is that we now wait
// for its verdict BEFORE givemon, when bouncing is still cheap.
void Multiplayer_ClaimStarter(void)
{
    u16 slot = gSpecialVar_0x8004;
    u16 species;
    u8 pkt[MP_PKT_SIZE_STARTER_PICK];

    if (slot > 2)
    {
        // Malformed script argument: fail open (grant) — identical to the
        // pre-claim behavior — rather than soft-locking the pick scene.
        gMultiplayerState.starterClaimState = MP_CLAIM_GRANTED;
        gMultiplayerState.starterClaimSpecies = 0;
        return;
    }
    species = Multiplayer_GetRandomizedStarter((u8)slot);
    gMultiplayerState.starterClaimSpecies = species;
    gMultiplayerState.starterClaimTimer = 0;

    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
    {
        gMultiplayerState.starterClaimState = MP_CLAIM_GRANTED; // solo rules
        return;
    }
    if (gMultiplayerState.partnerStarterSpecies == species)
    {
        // Partner's pick already arrived — no relay round-trip needed.
        gMultiplayerState.starterClaimState = MP_CLAIM_DENIED;
        return;
    }
    gMultiplayerState.starterClaimState = MP_CLAIM_PENDING;
    pkt[0] = MP_PKT_STARTER_PICK;
    pkt[1] = (u8)(species >> 8);
    pkt[2] = (u8)(species);
    MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_STARTER_PICK);
}

bool8 Multiplayer_NativePollStarterClaim(void)
{
    if (gMultiplayerState.starterClaimState == MP_CLAIM_GRANTED
     || gMultiplayerState.starterClaimState == MP_CLAIM_DENIED)
        return TRUE;
    if (gMultiplayerState.starterClaimState != MP_CLAIM_PENDING)
    {
        // waitstarterclaim without a preceding claim: fail open.
        gMultiplayerState.starterClaimState = MP_CLAIM_GRANTED;
        return TRUE;
    }
    // Denial repair: the winner's species reaches us via starter_taken or the
    // beacon even if the relay's verdict packet is lost.
    if (gMultiplayerState.partnerStarterSpecies == gMultiplayerState.starterClaimSpecies
     && gMultiplayerState.starterClaimSpecies != 0)
    {
        gMultiplayerState.starterClaimState = MP_CLAIM_DENIED;
        return TRUE;
    }
    // Partner gone mid-claim: solo rules, take it.
    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
    {
        gMultiplayerState.starterClaimState = MP_CLAIM_GRANTED;
        return TRUE;
    }
    // Backstop: relay alive but no verdict and no partner pick after ~10 s.
    // Granting matches the pre-claim behavior; the conflict this could in
    // principle let through also requires the partner's taken/beacon stream
    // to be silent, which the heartbeat timeout would have caught first.
    if (++gMultiplayerState.starterClaimTimer >= MP_CLAIM_TIMEOUT_FRAMES)
    {
        gMultiplayerState.starterClaimState = MP_CLAIM_GRANTED;
        return TRUE;
    }
    return FALSE;
}

u16 Multiplayer_GetStarterClaimResult(void)
{
    return (gMultiplayerState.starterClaimState == MP_CLAIM_DENIED) ? 0 : 1;
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

// ---------------------------------------------------------------------------
// Curated story-milestone var sync
//
// Most VAR_MAP_SCENE_* vars (0x4050-0x408B) are per-player cutscene playback and
// must NOT sync wholesale — forcing a partner's scene var forward can skip a
// per-player sequence (e.g. player A picking a starter advancing player B's lab
// scene past the starter choice).  So instead of a blanket range, we curate a
// small table of (var -> value) MILESTONES that represent genuinely shared world
// progress, and sync only those, forward-only.
//
// Rules enforced by the send/apply paths below:
//   - Only a write whose (varId, value) matches a table row is ever transmitted
//     (Multiplayer_IsMilestoneWrite), so intermediate per-player writes to a
//     table var never leak.
//   - Apply is forward-only (never regress a partner who is ahead) and gated on
//     an optional prereqFlag (deferral: a receiver mid-cutscene is not forced
//     forward; the milestone is re-offered later via the host's connect-time
//     replay in the MP_PKT_PARTNER_CONNECTED handler).
//   - completeFlag, if set, is a syncable story-range flag also set on apply, so
//     the shared gate survives via the robust OR-merge flag path as well.
//
// Only VAR_MAP_SCENE_VIRIDIAN_CITY_MART (0x4057) is listed for now: it is NOT
// dual-use (only the Viridian mart parcel scene writes it), and Oak's Pokédex
// hand-off is gated on it (`goto_if_ge VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 1` in
// PalletTown_ProfessorOaksLab_Frlg/scripts.inc), so syncing value 1 makes the
// partner's Oak recognize the parcel.  The dual-use lab scene var
// (VAR_MAP_SCENE_PALLET_TOWN_PROFESSOR_OAKS_LAB) is deliberately NOT listed.
struct CoopMilestone {
    u16 varId;
    u16 value;
    u16 prereqFlag;   // 0 = none; else receiver must have this flag before applying
    u16 completeFlag; // 0 = none; else also set this syncable story-range flag on apply
};

static const struct CoopMilestone sCoopMilestones[] = {
    { VAR_MAP_SCENE_VIRIDIAN_CITY_MART, 1, 0, FLAG_COOP_GOT_PARCEL },
};

static const struct CoopMilestone *FindMilestone(u16 varId, u16 value)
{
    u32 i;
    for (i = 0; i < ARRAY_COUNT(sCoopMilestones); i++)
    {
        if (sCoopMilestones[i].varId == varId && sCoopMilestones[i].value == value)
            return &sCoopMilestones[i];
    }
    return NULL;
}

bool32 IsSyncableVar(u16 varId)
{
    u32 i;
    for (i = 0; i < ARRAY_COUNT(sCoopMilestones); i++)
    {
        if (sCoopMilestones[i].varId == varId)
            return TRUE;
    }
    return FALSE;
}

// TRUE only when this exact (varId, value) write is a curated milestone.  The
// send path uses this so an intermediate per-player write to a milestone var
// (e.g. an early scene value) is never broadcast.
bool32 Multiplayer_IsMilestoneWrite(u16 varId, u16 value)
{
    return FindMilestone(varId, value) != NULL;
}

// A milestone is now reached on THIS instance (either written locally or applied
// from the partner).  Set the completeFlag (unguarded, so it also propagates to
// the partner via the normal story-flag sync / OR-merge).  Idempotent — callers
// only reach here on the transition, and the !FlagGet guard means the flag
// broadcasts once.
//
// This used to also request a checkpoint save; that is gone with the rest of
// autosave (a milestone often fires right as a cutscene ends, so the flash
// stall was very visible).  The flag itself is still shared live; it just is
// not written to the save file until the player saves.
static void NoteMilestoneReached(const struct CoopMilestone *m)
{
    if (m->completeFlag && !FlagGet(m->completeFlag))
        FlagSet(m->completeFlag);
}

// Sender side: a milestone (varId,value) was just written locally (by a map
// script) and broadcast via VAR_SET.  Record it durably here too so the sender
// gets the same completeFlag + checkpoint the receiver does.
void Multiplayer_OnLocalMilestone(u16 varId, u16 value)
{
    const struct CoopMilestone *m = FindMilestone(varId, value);
    if (m != NULL)
        NoteMilestoneReached(m);
}

// Receiver side: apply a milestone value received from the partner — forward-only,
// prereq-gated, idempotent.
void Multiplayer_ApplyMilestoneVar(u16 varId, u16 value)
{
    const struct CoopMilestone *m = FindMilestone(varId, value);
    if (m == NULL)
        return;                                  // not a recognized milestone
    if (VarGet(varId) >= value)
        return;                                  // forward-only: partner is ahead
    if (m->prereqFlag && !FlagGet(m->prereqFlag))
        return;                                  // deferred until prereq met

    Multiplayer_SetRemoteUpdate(TRUE);
    VarSet(varId, value);                        // guarded: don't echo the VAR_SET
    Multiplayer_SetRemoteUpdate(FALSE);

    NoteMilestoneReached(m);
}

// On (re)connect, replay every milestone this side has already reached so the
// partner converges even if the original incremental VAR_SET was dropped (chaos)
// or the partner joined late.  Called from both sides — apply is forward-only and
// idempotent, so a milestone the partner already has is a no-op, and a milestone
// the guest reached while the host was away still propagates.
void Multiplayer_SendMilestoneCatchup(void)
{
    u32 i;
    if (!gSaveBlock1Ptr)
        return;
    for (i = 0; i < ARRAY_COUNT(sCoopMilestones); i++)
    {
        if (VarGet(sCoopMilestones[i].varId) >= sCoopMilestones[i].value)
            Multiplayer_SendVarSet(sCoopMilestones[i].varId, sCoopMilestones[i].value);
    }
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

    // Same rationale for the boss-ready state itself.  ScriptCheckBossStart now
    // HOLDS bossReadyBossId past the pass (so the state beacon keeps repairing a
    // partner that missed our BOSS_READY under packet loss — chaos-window fix).
    // A gym clears it in Multiplayer_SetupCoopBattle, but a barrier with no
    // trainerbattle (the escort) never gets there, so clear at script end — the
    // handshake always lives within one script, and by its end the local side has
    // long since passed.  Redundant (harmless) for the gym path.
    gMultiplayerState.bossReadyBossId = 0;
    gMultiplayerState.partnerBossId   = 0;

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

    // Both ready (or solo).  Mark the next trainerbattle to route through the
    // coop double-battle path.  Solo runs (not connected) keep the flag clear so
    // the existing single-player battle script fires normally.
    if (gMultiplayerState.connState == MP_STATE_CONNECTED)
    {
        gMultiplayerState.coopBattlePending = TRUE;
        // Do NOT clear bossReadyBossId here.  The state beacon re-carries it
        // (pkt[4]) every interval and it is the ONLY loss-recovery channel for a
        // partner that has not yet received our BOSS_READY.  Clearing the instant
        // THIS side passes stops the beacon advertising "ready" while the partner
        // may still be at partnerBossId==0 under packet loss — the partner then
        // hangs at waitbossstart forever (chaos-window bug, 2026-07-21).  Both
        // sides hold their ready through party selection; Multiplayer_SetupCoopBattle
        // clears it once the battle actually starts, by which point the completed
        // party exchange proves the partner has also passed waitbossstart.
    }
    else
    {
        // Solo: no coop battle, so Multiplayer_SetupCoopBattle never runs.  Clear
        // here or the stale ready leaks into the next boss trigger.
        gMultiplayerState.bossReadyBossId = 0;
        gMultiplayerState.partnerBossId   = 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Co-op battle turn sync
// ---------------------------------------------------------------------------

bool32 Multiplayer_IsCoopBattle(void)
{
    return (bool32)((gBattleTypeFlags & BATTLE_TYPE_COOP) != 0);
}

// Each instance runs its LOCAL player as battler 0 and the partner as battler
// 2, so a raw player-side battler INDEX names a different physical mon on the
// two sims.  When the opponent AI picks a player target by index via the
// lockstep RNG, both sims roll the same value but it resolves to opposite mons
// — the enemy attacks a different Pokemon on each screen and the battle desyncs.
// This maps a *canonical* player index (0 = host's player, 1 = guest's player,
// agreed by both sims) to the LOCAL battler id (0 or 2) that names the same
// physical mon on this instance, so a lockstep roll hits one mon on both sides.
//   HOST : canon 0 -> battler 0 (local), canon 1 -> battler 2 (partner)
//   GUEST: canon 0 -> battler 2 (partner), canon 1 -> battler 0 (local)
// MP_ROLE_NONE falls through to the host mapping — which means if BOTH sims
// read NONE they agree on the WRONG resolution (both pick their own battler 0,
// different physical mons) and the boss AI desyncs.  This is not hypothetical:
// Multiplayer_Init (run on continue/new-game and every save-state reload)
// zeroes gMultiplayerState.role AFTER the relay's one-shot role assignment, so
// role read NONE for whole sessions until the transport was made to re-assert
// it every frame (serial_bridge.rs + tools/mcp_gamestate/server.py role
// heartbeat, 2026-07-21).  The contract is now: the transport keeps role
// non-NONE for the life of the connection; this fallback only covers the
// single-player / pre-connect case where there is no partner sim to disagree.
u32 Multiplayer_CanonicalPlayerTarget(u32 canonicalIdx)
{
    return Multiplayer_CanonicalBattler((canonicalIdx & 1) * 2);
}

// Local battler id -> role-canonical battler id.  Canonically, battler 0 is
// always the HOST's mon and battler 2 the GUEST's; locally each sim runs its
// own player at 0, so the guest's two player slots are swapped relative to
// canon.  Opponent battlers 1/3 are identical on both sims and pass through.
//
// This is the same involution as Multiplayer_CanonicalPlayerTarget (which goes
// the other way but, being an involution over {0,2}, is the same map), so the
// two share one implementation and one role fallback.  Role comes from the
// session tier and is kept non-NONE by the transport heartbeat; when it is NONE
// (single-player / pre-connect) the host mapping is the identity, which is what
// unmirrored single-player code expects.
u32 Multiplayer_CanonicalBattler(u32 localBattler)
{
    if ((localBattler & 1) == 0 && localBattler < MAX_BATTLERS_COUNT
        && gMultiplayerState.role == MP_ROLE_GUEST)
        return 2 - localBattler;
    return localBattler;
}

// Canonical order for iterating candidate battlers during opponent-AI
// deliberation.  The AI's per-target scoring loop consumes lockstep RNG
// draws *inside* each (attacker, target) evaluation; iterating targets in
// LOCAL battler order feeds those draws to opposite physical matchups on
// the two mirrored sims, desyncing both the draw values and — because the
// draw count per matchup is branch-dependent — the stream position itself.
// Iterating in this canonical order (host's player mon, opponent-left,
// guest's player mon, opponent-right) makes step k the same physical
// matchup on both sims.  Opponent battlers 1/3 are unmirrored; even steps
// route through Multiplayer_CanonicalPlayerTarget.
u32 Multiplayer_CoopAiEvalBattler(u32 step)
{
    if (step & 1)
        return step;
    return Multiplayer_CanonicalPlayerTarget(step >> 1);
}

// Remap a sender-local gPlayerParty index onto our index space.  In a coop
// double battle each instance runs its own mons at gPlayerParty[0..2] and the
// partner's at gPlayerParty[3..5]; the sender's own switch/replacement target
// (0-2) is a partner-half mon (3-5) for us, and vice versa.  This is the
// party-slot analogue of the 0<->2 battler-target mirror.  The PARTY_SIZE
// sentinel (and any out-of-range value) passes through unchanged.
static u8 CoopRemapPartyIndex(u8 idx)
{
    if (idx < MULTI_PARTY_SIZE)
        return idx + MULTI_PARTY_SIZE;
    if (idx < PARTY_SIZE)
        return idx - MULTI_PARTY_SIZE;
    return idx;
}

// Generic tagged-action send.  action is MP_TURN_ACT_*; p0..p3 are the
// action-specific payload bytes.  Assigns one fresh sequence number per logical
// turn (0 reserved for "no turn" on the beacon) and caches the full payload so
// Multiplayer_ResendBattleTurn / the state beacon can re-carry it for loss
// recovery.
void Multiplayer_SendBattleAction(u8 action, u8 p0, u8 p1, u8 p2, u8 p3)
{
    u8 seq = (u8)(gMultiplayerState.battleTurnSeqOut + 1);
    if (seq == 0)
        seq = 1;
    gMultiplayerState.battleTurnSeqOut       = seq;
    gMultiplayerState.battleTurnSent         = TRUE;
    gMultiplayerState.battleTurnSentAction   = action;
    // p0..p2 reuse the existing Sent{MoveSlot,Target,Flags} fields (frozen
    // battle_diag offsets); p3 rides the new battleTurnSentP3.
    gMultiplayerState.battleTurnSentMoveSlot = p0;
    gMultiplayerState.battleTurnSentTarget   = p1;
    gMultiplayerState.battleTurnSentFlags    = p2;
    gMultiplayerState.battleTurnSentP3       = p3;
    Multiplayer_ResendBattleTurn();
}

// MOVE-only wrapper — existing move-confirm call sites keep their signature.
void Multiplayer_SendBattleTurn(u8 moveSlot, u8 target, u8 flags)
{
    Multiplayer_SendBattleAction(MP_TURN_ACT_MOVE, moveSlot, target, flags, 0);
}

// Voluntary "Pokémon" switch or forced after-faint replacement.
void Multiplayer_SendBattleSwitch(u8 partyIdx, bool8 isReplace)
{
    Multiplayer_SendBattleAction(isReplace ? MP_TURN_ACT_REPLACE : MP_TURN_ACT_SWITCH,
                                 partyIdx, 0, 0, 0);
}

// "Bag" item use.  16-bit item id split hi/lo across p0/p1; p2 = target party
// index, p3 = move slot sub-selection (Ether/PP restore).
void Multiplayer_SendBattleItem(u16 itemId, u8 target, u8 moveSlot)
{
    Multiplayer_SendBattleAction(MP_TURN_ACT_ITEM, (u8)(itemId >> 8), (u8)(itemId & 0xFF),
                                 target, moveSlot);
}

// Re-emit the cached turn with its original seq.  Used by the reconnect path
// and callable any time battleTurnSent is set; the receiver's seq dedup makes
// duplicates harmless.
void Multiplayer_ResendBattleTurn(void)
{
    u8 pkt[MP_PKT_SIZE_BATTLE_TURN];
    if (!gMultiplayerState.battleTurnSent)
        return;
    pkt[0] = MP_PKT_BATTLE_TURN;
    pkt[1] = gMultiplayerState.battleTurnSeqOut;
    pkt[2] = gMultiplayerState.battleTurnSentAction;
    pkt[3] = gMultiplayerState.battleTurnSentMoveSlot;
    pkt[4] = gMultiplayerState.battleTurnSentTarget;
    pkt[5] = gMultiplayerState.battleTurnSentFlags;
    pkt[6] = gMultiplayerState.battleTurnSentP3;
    MpRing_Write(&gMpSendRing, pkt, MP_PKT_SIZE_BATTLE_TURN);
}

// MOVE-only wrapper — dispatches a MOVE action (state beacon MOVE re-carry,
// existing native tests).
void Multiplayer_HandleBattleTurn(u8 seq, u8 moveSlot, u8 target, u8 flags)
{
    Multiplayer_HandleBattleAction(seq, MP_TURN_ACT_MOVE, moveSlot, target, flags, 0);
}

void Multiplayer_HandleBattleAction(u8 seq, u8 action, u8 p0, u8 p1, u8 p2, u8 p3)
{
    // Dedup + ordering: apply only turns strictly newer than the last applied
    // (wraparound-aware).  diff==0 is a repeat (beacon re-carry, reconnect
    // resend); diff>=0x80 is a stale turn delivered late by a reordering
    // relay.  Either way the engine already has — or has moved past — it.
    u8 diff = (u8)(seq - gMultiplayerState.battleTurnSeqApplied);
    if (seq == 0 || diff == 0 || diff >= 0x80)
        return;
    gMultiplayerState.battleTurnSeqApplied = seq;

    gMultiplayerState.battleTurnAction = action;
    switch (action)
    {
    case MP_TURN_ACT_SWITCH:
    case MP_TURN_ACT_REPLACE:
        // p0 = sender-local party index; remap onto our partner half.
        gMultiplayerState.battleTurnPartyIdx = CoopRemapPartyIndex(p0);
        break;
    case MP_TURN_ACT_ITEM:
        // p0/p1 = item id hi/lo; p2 = target party index (remapped); p3 = move slot.
        gMultiplayerState.battleTurnItemId     = (u16)((p0 << 8) | p1);
        gMultiplayerState.battleTurnItemTarget = CoopRemapPartyIndex(p2);
        gMultiplayerState.battleTurnItemMove   = p3;
        break;
    case MP_TURN_ACT_MOVE:
    default:
        // The partner's sim is mirrored: their own mon is battler 0 on their
        // screen but battler 2 on ours (and vice versa).  Opponent indices
        // (1/3) line up unchanged.  Remap ally-side targets into our index
        // space.  Only MOVE carries a battler target.
        if (p1 == 0)
            p1 = 2;
        else if (p1 == 2)
            p1 = 0;
        gMultiplayerState.battleTurnMoveSlot = p0;
        gMultiplayerState.battleTurnTarget   = p1;
        gMultiplayerState.battleTurnFlags    = p2;
        break;
    }
    gMultiplayerState.battleTurnReceived = TRUE;
}

// ---------------------------------------------------------------------------
// Coop battle RNG lockstep
//
// The expansion's tagged battle-logic rolls (RandomUniform / RandomUniformExcept
// / RandomWeightedArray / RandomElementArray) normally draw from the shared
// gRngValue stream, which is also advanced by per-frame visual/animation draws
// — so two free-running instances diverge immediately even with an identical
// seed.  During BATTLE_TYPE_COOP battles we override the weak symbols from
// src/random.c and route those rolls through a dedicated xorshift32 stream
// seeded identically on both sides (host's seed via MP_PKT_PARTY_SYNC).
// Both sims make the same logic rolls in the same order, so damage / crits /
// status procs / AI choices stay in lockstep; cosmetic Random() draws remain
// free-running and harmless.
//
// Known limitation: untagged Random()/Random32() calls inside battle logic
// (if any remain upstream) still draw from the shared stream and can diverge.
// ---------------------------------------------------------------------------

u16 Multiplayer_CoopBattleRandom16(void)
{
    u32 x = gMultiplayerState.coopRngState;
    if (x == 0)
        x = 0x12345678u; // xorshift32 sticks at 0; remap (same fallback as Multiplayer_SeedRng)
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    gMultiplayerState.coopRngState = x;
    return (u16)(x >> 16);
}

// TESTING builds (make check / battle tests) install their own strong
// RandomUniform etc. in test/test_runner.c; defining ours there would clash.
// Native unit tests compile with TESTING undefined, so they exercise these.
#if !TESTING

u32 RandomUniform(enum RandomTag tag, u32 lo, u32 hi)
{
    if (Multiplayer_IsCoopBattle())
        return lo + (((hi - lo + 1) * Multiplayer_CoopBattleRandom16()) >> 16);
    return RandomUniformDefault(tag, lo, hi);
}

u32 RandomUniformExcept(enum RandomTag tag, u32 lo, u32 hi, bool32 (*reject)(u32))
{
    if (Multiplayer_IsCoopBattle())
    {
        while (TRUE)
        {
            u32 n = lo + (((hi - lo + 1) * Multiplayer_CoopBattleRandom16()) >> 16);
            if (!reject(n))
                return n;
        }
    }
    return RandomUniformExceptDefault(tag, lo, hi, reject);
}

u32 RandomWeightedArray(enum RandomTag tag, u32 sum, u32 n, const u16 *weights)
{
    if (Multiplayer_IsCoopBattle())
    {
        u32 i, targetSum;
        targetSum = (sum * Multiplayer_CoopBattleRandom16()) >> 16;
        for (i = 0; i < n - 1; i++)
        {
            if (targetSum < weights[i])
                return i;
            targetSum -= weights[i];
        }
        return n - 1;
    }
    return RandomWeightedArrayDefault(tag, sum, n, weights);
}

const void *RandomElementArray(enum RandomTag tag, const void *array, size_t size, size_t count)
{
    if (Multiplayer_IsCoopBattle())
        return (const u8 *)array + size * RandomUniform(tag, 0, count - 1);
    return RandomElementArrayDefault(tag, array, size, count);
}

#endif // !TESTING

// Returns 1 if a partner is connected; 0 otherwise.
// Called from gym scripts: 'specialvar VAR_RESULT, Multiplayer_IsConnected'
// to choose the co-op waiting path vs the solo direct-battle path.
u16 Multiplayer_IsConnected(void)
{
    return (gMultiplayerState.connState == MP_STATE_CONNECTED) ? 1u : 0u;
}

// Returns 1 if the partner has already signalled readiness for the Oak's-Lab
// rival battle.  Used by the script to show "Your partner is waiting for you!"
// instead of "Waiting for partner..." when the late player arrives.
u16 Multiplayer_IsPartnerWaitingForBoss_RivalOaksLab(void)
{
    return (gMultiplayerState.partnerBossId == BOSS_ID_RIVAL_OAKS_LAB) ? 1u : 0u;
}

// Native script callback for 'waitbossstart' opcode.
// Returns TRUE (resume bytecode) when both players are ready or playing solo.
// Returns FALSE (stay in NATIVE mode, yield) while still waiting.
bool8 Multiplayer_NativePollBossStart(void)
{
    // Packet draining is handled by the script engine's native step before
    // this poll runs, regardless of the active main callback.

    // B button cancels the wait so the player can back out if the partner is
    // stuck at a different event.  VAR_RESULT = 0 signals cancellation to the
    // calling script; VAR_RESULT = 1 signals a successful start.
    if (JOY_NEW(B_BUTTON))
    {
        Multiplayer_BossCancel();
        VarSet(VAR_RESULT, 0);
        return TRUE;
    }

    // A dropped BOSS_READY is repaired by the state beacon, which carries
    // bossReadyBossId.

    if (Multiplayer_ScriptCheckBossStart())
    {
        VarSet(VAR_RESULT, 1);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Party selection for co-op boss battles
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PARTY_SYNC wire mon serialization (pure byte-level helpers; unit-tested
// natively).  Explicit big-endian field writes — never memcpy a struct onto
// the wire, struct padding would leak into the protocol.
// ---------------------------------------------------------------------------

static void MpPutU16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void MpPutU32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u16  MpGetU16(const u8 *p)  { return (u16)(((u16)p[0] << 8) | p[1]); }
static u32  MpGetU32(const u8 *p)  { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

u8 Mp_EncodePartyMon(u8 *out, const struct MpWirePartyMon *m)
{
    u8 j;
    u8 *p = out;
    MpPutU16(p, m->species);     p += 2;
    MpPutU16(p, m->heldItem);    p += 2;
    *p++ = m->level;
    *p++ = m->abilityNum;
    MpPutU32(p, m->personality); p += 4;
    MpPutU32(p, m->otId);        p += 4;
    for (j = 0; j < 4; j++) { MpPutU16(p, m->moves[j]); p += 2; }
    for (j = 0; j < 4; j++) *p++ = m->pp[j];
    MpPutU16(p, m->hp);          p += 2;
    MpPutU16(p, m->maxHP);       p += 2;
    MpPutU16(p, m->atk);         p += 2;
    MpPutU16(p, m->def);         p += 2;
    MpPutU16(p, m->speed);       p += 2;
    MpPutU16(p, m->spAtk);       p += 2;
    MpPutU16(p, m->spDef);       p += 2;
    MpPutU32(p, m->status);      p += 4;
    *p++ = m->friendship;
    *p++ = m->gender;
    *p++ = m->language;
    memcpy(p, m->nickname, MP_WIRE_NICK_LEN); p += MP_WIRE_NICK_LEN;
    return (u8)(p - out);
}

bool8 Mp_DecodePartyMon(const u8 *in, struct MpWirePartyMon *m)
{
    u8 j;
    const u8 *p = in;
    if (in == NULL || m == NULL)
        return FALSE;
    m->species     = MpGetU16(p); p += 2;
    m->heldItem    = MpGetU16(p); p += 2;
    m->level       = *p++;
    m->abilityNum  = *p++;
    m->personality = MpGetU32(p); p += 4;
    m->otId        = MpGetU32(p); p += 4;
    for (j = 0; j < 4; j++) { m->moves[j] = MpGetU16(p); p += 2; }
    for (j = 0; j < 4; j++) m->pp[j] = *p++;
    m->hp          = MpGetU16(p); p += 2;
    m->maxHP       = MpGetU16(p); p += 2;
    m->atk         = MpGetU16(p); p += 2;
    m->def         = MpGetU16(p); p += 2;
    m->speed       = MpGetU16(p); p += 2;
    m->spAtk       = MpGetU16(p); p += 2;
    m->spDef       = MpGetU16(p); p += 2;
    m->status      = MpGetU32(p); p += 4;
    m->friendship  = *p++;
    m->gender      = *p++;
    m->language    = *p++;
    memcpy(m->nickname, p, MP_WIRE_NICK_LEN);
    return TRUE;
}

// Encode gPlayerParty[0..n-1] as wire mons and write into dst.
// Returns number of bytes written.
static u8 EncodePartySync(u8 *dst, u8 n_mons)
{
    u8 i, j;
    u8 *p = dst;
    *p++ = MP_PKT_PARTY_SYNC;
    *p++ = n_mons;
    for (i = 0; i < n_mons; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        struct MpWirePartyMon w;
        w.species     = GetMonData(mon, MON_DATA_SPECIES);
        w.heldItem    = GetMonData(mon, MON_DATA_HELD_ITEM);
        w.level       = GetMonData(mon, MON_DATA_LEVEL);
        w.abilityNum  = GetMonData(mon, MON_DATA_ABILITY_NUM);
        w.personality = GetMonData(mon, MON_DATA_PERSONALITY);
        w.otId        = GetMonData(mon, MON_DATA_OT_ID);
        for (j = 0; j < 4; j++)
        {
            w.moves[j] = GetMonData(mon, MON_DATA_MOVE1 + j);
            w.pp[j]    = GetMonData(mon, MON_DATA_PP1 + j);
        }
        w.hp         = GetMonData(mon, MON_DATA_HP);
        w.maxHP      = GetMonData(mon, MON_DATA_MAX_HP);
        w.atk        = GetMonData(mon, MON_DATA_ATK);
        w.def        = GetMonData(mon, MON_DATA_DEF);
        w.speed      = GetMonData(mon, MON_DATA_SPEED);
        w.spAtk      = GetMonData(mon, MON_DATA_SPATK);
        w.spDef      = GetMonData(mon, MON_DATA_SPDEF);
        w.status     = GetMonData(mon, MON_DATA_STATUS);
        w.friendship = GetMonData(mon, MON_DATA_FRIENDSHIP);
        w.gender     = GetMonGender(mon);
        w.language   = GetMonData(mon, MON_DATA_LANGUAGE);
        GetMonData(mon, MON_DATA_NICKNAME, w.nickname);
        p += Mp_EncodePartyMon(p, &w);
    }
    // Trailing battle RNG seed: only the host transmits a real value; guests
    // send 0 so the receiver's "adopt nonzero" rule always converges on the
    // host's seed regardless of message order.
    {
        u32 seed = (gMultiplayerState.role == MP_ROLE_HOST)
                       ? gMultiplayerState.coopBattleSeed : 0;
        MpPutU32(p, seed);
        p += MP_PKT_PARTY_SYNC_SEED_SIZE;
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
    u8 i, j;
    if (n_mons > MULTI_PARTY_SIZE) n_mons = MULTI_PARTY_SIZE;
    for (i = 0; i < n_mons; i++)
    {
        struct MpWirePartyMon w;
        struct Pokemon *mon = &sPartnerBattleParty[i];

        if (!Mp_DecodePartyMon(data + i * MP_PKT_PARTY_SYNC_MON_SIZE, &w))
            return;

        // Display copy for the VS screen / party menus.
        gMultiPartnerParty[i].species     = w.species;
        gMultiPartnerParty[i].heldItem    = w.heldItem;
        memcpy(gMultiPartnerParty[i].nickname, w.nickname, MP_WIRE_NICK_LEN);
        gMultiPartnerParty[i].level       = w.level;
        gMultiPartnerParty[i].hp          = w.hp;
        gMultiPartnerParty[i].maxhp       = w.maxHP;
        gMultiPartnerParty[i].status      = w.status;
        gMultiPartnerParty[i].personality = w.personality;
        gMultiPartnerParty[i].gender      = w.gender;
        gMultiPartnerParty[i].language    = w.language;

        // Battle-engine copy, staged in sPartnerBattleParty until
        // Multiplayer_SetupCoopBattle moves it into gPlayerParty.  CreateMon
        // does NOT compute stats (only CreateMonWithIVs does), so every stat
        // field must be written explicitly here — a mon left at 0 HP gets
        // flagged absent by TryDoEventsBeforeFirstTurn and battler 2 silently
        // drops out of the co-op battle (each side then fights a private 1v1).
        CreateMon(mon, w.species, w.level, w.personality, OTID_STRUCT_PRESET(w.otId));
        SetMonData(mon, MON_DATA_NICKNAME, w.nickname);
        SetMonData(mon, MON_DATA_HELD_ITEM, &w.heldItem);
        SetMonData(mon, MON_DATA_ABILITY_NUM, &w.abilityNum);
        SetMonData(mon, MON_DATA_FRIENDSHIP, &w.friendship);
        SetMonData(mon, MON_DATA_LANGUAGE, &w.language);
        for (j = 0; j < 4; j++)
        {
            u16 move = w.moves[j];
            SetMonData(mon, MON_DATA_MOVE1 + j, &move);
            SetMonData(mon, MON_DATA_PP1 + j, &w.pp[j]);
        }
        SetMonData(mon, MON_DATA_MAX_HP, &w.maxHP);
        SetMonData(mon, MON_DATA_HP, &w.hp);
        SetMonData(mon, MON_DATA_ATK, &w.atk);
        SetMonData(mon, MON_DATA_DEF, &w.def);
        SetMonData(mon, MON_DATA_SPEED, &w.speed);
        SetMonData(mon, MON_DATA_SPATK, &w.spAtk);
        SetMonData(mon, MON_DATA_SPDEF, &w.spDef);
        SetMonData(mon, MON_DATA_STATUS, &w.status);
    }
    gMultiplayerState.partnerPartySelectDone = TRUE;
    // Persistent handshake flag: drives the beacon party-sync ack and the
    // waitpartysync exit.  Unlike partnerPartySelectDone (consumed/reset by the
    // solo path), this stays set through the battle and is reset per battle in
    // ScrCmd_waitcoopparty.
    gMultiplayerState.gotPartnerParty = TRUE;
}

bool8 Multiplayer_NativePollPartySync(void)
{
    // Packet draining is handled by the script engine's native step before
    // this poll runs, regardless of the active main callback.
    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
    {
        // Solo / disconnected: nothing to exchange.  Keep the legacy reset so
        // Multiplayer_SetupCoopBattle's solo-clone fallback (which keys on
        // !partnerPartySelectDone) still fires.
        gMultiplayerState.partnerPartySelectDone = FALSE;
        gMultiplayerState.partySyncResendTimer   = 0;
        return TRUE;
    }
    // Mutual handshake: proceed only once we have the partner's party AND the
    // partner's state beacon has confirmed it has ours.  Gating the exit on
    // local receipt alone deadlocked under asymmetric loss — the side that
    // received first stopped resending while the other waited forever
    // (PROGRESS.md "PARTY_SYNC asymmetric-loss deadlock").  Because we stay in
    // this poll (and keep resending below) until partnerGotMyParty is set, the
    // resend survives until the partner actually receives our party.
    if (gMultiplayerState.gotPartnerParty && gMultiplayerState.partnerGotMyParty)
    {
        gMultiplayerState.partySyncResendTimer = 0;
        return TRUE;
    }
    // Resend every 60 frames until the partner acks receipt (beacon bit 7).
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
        if (slot == 0 || n >= MULTI_PARTY_SIZE) break;
        // Remember each pick's original party index so the battle-end restore
        // can write its post-battle state back to the right stash slot.
        gMultiplayerState.coopSelectedSlots[n] = slot - 1;
        scratch[n++] = gPlayerParty[slot - 1];
    }
    gMultiplayerState.coopSelectedCount = n;

    // n == 0 means the player backed out of the selection: pressing B offers a
    // "Cancel battle?" prompt for the FACILITY_MULTI_OR_EREADER menu, and YES
    // runs ClearSelectedPartyOrder() then still invokes THIS exit callback
    // (Task_ClosePartyMenuAndSetCB2 always calls gPartyMenu.exitCallback; the
    // gPartyMenuUseExitCallback flag is not consulted at the close site).  But
    // the coop script is already past waitbossstart and committed to the
    // trainerbattle — there is no abort path back to the overworld from here,
    // and resuming would send a 0-mon PARTY_SYNC and start the double battle
    // with no mon on this side (a desync the partner cannot recover from, since
    // NativePollPartySync has no cancel escape).  Re-open the selection so at
    // least one mon is brought.  This makes cancel a no-op inside a committed
    // coop battle, mirroring the min-1 rule the CONFIRM path already enforces
    // (Task_ValidateChosenHalfParty -> PARTY_MSG_NO_MON_FOR_BATTLE).  Returning
    // before the VAR_FRONTIER_FACILITY reset below keeps the facility set for
    // the re-opened menu across repeated cancels.
    if (n == 0)
    {
        VarSet(VAR_FRONTIER_FACILITY, FACILITY_MULTI_OR_EREADER);
        gMain.savedCallback = CB2_CoopPartySelected;
        InitChooseHalfPartyForBattle(0);
        return;
    }

    // Write selected mons back to front of party; leave the rest unchanged
    // (battle engine only reads up to MULTI_PARTY_SIZE from gPlayerParty[0..]).
    for (i = 0; i < n; i++)
        gPlayerParty[i] = scratch[i];

    // Restore VAR_FRONTIER_FACILITY so it doesn't leak into other menus.
    VarSet(VAR_FRONTIER_FACILITY, 0);

    // Host mints a fresh battle RNG seed once per battle, before the (re)sends
    // in NativePollPartySync — every resend must carry the SAME seed or the
    // guest could adopt a different stream than the host plays.
    if (gMultiplayerState.role == MP_ROLE_HOST)
        gMultiplayerState.coopBattleSeed = Multiplayer_GenerateSeed();

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

    // Boss-ready handshake is complete: the battle engine is being set up, so
    // the party exchange finished, which proves the partner also passed
    // waitbossstart.  Held until here (not cleared in ScriptCheckBossStart) so
    // the state beacon kept re-advertising our readiness to a partner that may
    // not have received our BOSS_READY under packet loss.  Clear now — during
    // the battle pkt[4] must read 0 (the turn re-carry owns pkt[5..8]).
    gMultiplayerState.bossReadyBossId = 0;
    gMultiplayerState.partnerBossId   = 0;

    // Fresh turn-sequence space for this battle.  Both sides reset here, so
    // seq 1 is always the first turn of the current battle.  An early turn
    // packet that lands before this reset is repaired by the partner's state
    // beacon (its seq is newer than the zeroed battleTurnSeqApplied).
    gMultiplayerState.battleTurnSeqOut     = 0;
    gMultiplayerState.battleTurnSeqApplied = 0;
    gMultiplayerState.battleTurnReceived   = FALSE;
    gMultiplayerState.battleTurnSent       = FALSE;

    // Seed the lockstep battle RNG stream.  coopBattleSeed was adopted from
    // the host's PARTY_SYNC; if no role was ever assigned both sides hold 0
    // here and CoopBattleRandom16's zero-remap keeps them on the same fixed
    // stream — still lockstep, just not per-battle fresh.
    gMultiplayerState.coopRngState = gMultiplayerState.coopBattleSeed;

    // If the party sync exchange happened (waitpartysync completed), copy the
    // partner's decoded party from the side buffer into the battle-engine half
    // of gPlayerParty.  This is the ONLY place partner mons enter gPlayerParty
    // — the receive handler keeps them in sPartnerBattleParty so an early or
    // re-sent PARTY_SYNC can never clobber local mons outside a battle.
    // Fall back to cloning the local player's party only when playing solo (no sync).
    if (gMultiplayerState.gotPartnerParty)
    {
        for (i = 0; i < MULTI_PARTY_SIZE; i++)
            gPlayerParty[MULTI_PARTY_SIZE + i] = sPartnerBattleParty[i];
    }
    else if (!gMultiplayerState.partnerPartySelectDone && gMultiplayerState.connState != MP_STATE_CONNECTED)
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
    // Release the field-trainer lock the instant our battle ends.  It is set at
    // spotting in Multiplayer_SendTrainerBusy and held through the approach/intro
    // and the whole battle (the state beacon re-carries it every interval from
    // both Multiplayer_Update pre-battle and Multiplayer_BattleTick during the
    // battle).  This hook fires from ReturnFromBattleToOverworld (battle_main.c)
    // right after gMain.inBattle is set FALSE — the point every NON-link battle
    // passes through.  (It was previously hooked in SetBattleEndCallbacks, a
    // controller func only link battles ever install — dead for every battle
    // type we care about; found live in RB1 run 1, 2026-07-20.)
    // Clear the local flag UNCONDITIONALLY so a disconnect
    // mid-battle can't strand it set (the beacon would re-broadcast a stuck lock
    // on reconnect); only emit the explicit FREE packet while still connected.
    if (gMultiplayerState.sentBusyTrainer)
    {
        gMultiplayerState.sentBusyTrainer = FALSE;
        if (gMultiplayerState.connState == MP_STATE_CONNECTED)
        {
            u8 freeByte = MP_PKT_TRAINER_FREE;
            MpRing_Write(&gMpSendRing, &freeByte, MP_PKT_SIZE_TRAINER_FREE);
        }
    }

    // Restore the party a coop battle rearranged: write each selected mon's
    // post-battle state (exp, level-ups, HP, status) back into its original
    // stash slot, then reload the whole stash — non-participants come back
    // and the partner's mons are evicted from gPlayerParty[MULTI_PARTY_SIZE..].
    // Mirrors CB2_EndDebugBattle's INGAME_PARTNER handling.  Runs regardless
    // of connState: a disconnect mid-battle (grace-timer AI fallback) must
    // still restore, or the partner's mons stay in the local party forever.
    if (gMultiplayerState.coopPartyStashed)
    {
        u8 i;
        for (i = 0; i < gMultiplayerState.coopSelectedCount; i++)
            SavePlayerPartyMon(gMultiplayerState.coopSelectedSlots[i], &gPlayerParty[i]);
        LoadPlayerParty();
        gMultiplayerState.coopPartyStashed = FALSE;
        gMultiplayerState.coopSelectedCount = 0;
    }

    if (gMultiplayerState.connState != MP_STATE_CONNECTED)
        return;
    // NO checkpoint save here.  Even async (one sector/frame), the 14 flash
    // writes queued here landed on the battle-exit fade / return-to-field
    // frames of EVERY battle, wild ones included — the same multi-second hitch
    // the per-warp checkpoint caused on map transitions (user-reported
    // 2026-08-03: "same delay as changing areas occurs when leaving a wild
    // battle").  See the autosave note in Multiplayer_Update.
}
