#ifndef GUARD_MULTIPLAYER_H
#define GUARD_MULTIPLAYER_H

#include "global.h"
#include "constants/multiplayer.h"
#include "constants/event_objects.h"

// Ghost NPC graphics ID is picked at spawn time based on partner gender
// (Multiplayer_GhostGraphicsId).  Until the partner sends MP_PKT_GENDER we
// fall back to the opposite of the local player so the ghost is always
// visually distinct from self.
// LocalId 0xFD is above any map-defined NPC (maps rarely use IDs > 10).
// Must NOT be OBJ_EVENT_ID_FOLLOWER (0xFE) or the engine routes interaction
// to EventScript_Follower instead of our ghost script.
#define GHOST_LOCAL_ID             0xFD
#define GHOST_FOLLOWER_LOCAL_ID    0xFC  // object event local ID for partner's follower Pokémon ghost
// Default elevation for overworld spawns.
#define GHOST_ELEVATION            3
// Sentinel value for ghostObjectEventId when no ghost is spawned.
// Must be >= OBJECT_EVENTS_COUNT (16) to pass the "not spawned" guard,
// and != OBJECT_EVENTS_COUNT so that spawn-failure (which also returns 16)
// doesn't accidentally look like a valid spawned slot.
#define GHOST_INVALID_SLOT         0xFF

// Set to 1 to spawn a test ghost at a hardcoded Route 1 position without network.
// Used to verify Step 1.3 rendering in mGBA.  Always 0 in production.
#define MP_DEBUG_TEST_GHOST        0

// Set to 1 to boot with a fixed encounter seed so randomization can be verified
// in mGBA without a live Tauri session.  Always 0 in production.
#define MP_DEBUG_TEST_SEED         0
#define MP_DEBUG_TEST_SEED_VALUE   0xDEADBEEFu
#define MP_DEBUG_TEST_MAP_GROUP    0   // Pallet Town area map group for Route 1
#define MP_DEBUG_TEST_MAP_NUM      16  // MAP_ROUTE1 index
#define MP_DEBUG_TEST_X            8
#define MP_DEBUG_TEST_Y            5

// Block exchange buffer — EWRAM staging area so the Tauri relay can shuttle
// SendBlock data between the two emulator instances without a real link cable.
// Size matches BLOCK_BUFFER_SIZE in link.h (0x100 = 256 bytes).
#define MP_BLOCK_BUF_SIZE 0x100
struct MpBlockExchange {
    u8  sendReady;               // ROM sets 1 when data[] has a block to relay
    u8  recvReady;               // relay sets 1 when data[] has partner's block
    u8  fromPlayerIdx;           // relay sets: which gBlockRecvBuffer slot to fill
    u8  _pad;
    u8  data[MP_BLOCK_BUF_SIZE]; // raw block bytes
};
extern struct MpBlockExchange gMpBlockExchange;

// ---------------------------------------------------------------------------
// Ring buffer — shared memory interface between ROM and Tauri host app.
//
// gMpSendRing: ROM writes outgoing packets; Tauri reads and forwards to relay.
// gMpRecvRing: Tauri writes incoming packets from relay; ROM reads/processes.
//
// Tauri locates each buffer via ELF symbol; magic==MP_RING_MAGIC is a sanity
// check.  u8 head/tail wrap at MP_RING_SIZE (256) with no modulo needed.
// Empty: head==tail.  Full: (head+1)==tail.
// ---------------------------------------------------------------------------
struct MpRingBuf {
    u8 buf[MP_RING_SIZE]; // circular byte storage
    u8 head;              // producer increments (ROM for send; Tauri for recv)
    u8 tail;              // consumer increments (Tauri for send; ROM for recv)
    u8 magic;             // MP_RING_MAGIC (0xC0) when valid
    u8 _pad;
};

extern struct MpRingBuf gMpSendRing;
extern struct MpRingBuf gMpRecvRing;

// Low-level ring operations (inline for speed; used inside multiplayer.c).
// Returns TRUE if the byte was pushed; FALSE if ring was full.
static inline bool8 Mp_Push(struct MpRingBuf *ring, u8 byte)
{
    u8 next = ring->head + 1; // wraps at 256 automatically (u8 arithmetic)
    if (next == ring->tail)
        return FALSE; // full
    ring->buf[ring->head] = byte;
    ring->head = next;
    return TRUE;
}

// Returns TRUE if a byte was popped into *out; FALSE if ring was empty.
static inline bool8 Mp_Pop(struct MpRingBuf *ring, u8 *out)
{
    if (ring->head == ring->tail)
        return FALSE; // empty
    *out = ring->buf[ring->tail];
    ring->tail++;
    return TRUE;
}

// Bytes available to read.
static inline u8 Mp_Available(const struct MpRingBuf *ring)
{
    return (u8)(ring->head - ring->tail);
}

// ---------------------------------------------------------------------------
// Packet encode/decode helpers.  These write to / read from a caller-supplied
// byte buffer.  Returns the number of bytes consumed/produced (0 on error).
// ---------------------------------------------------------------------------

// Position packet: [type][mapGroup][mapNum][x][y][facing]  (6 bytes)
u8 Mp_EncodePosition(u8 *out, u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing);
bool8 Mp_DecodePosition(const u8 *in, u8 len,
                        u8 *mapGroup, u8 *mapNum, u8 *x, u8 *y, u8 *facing);

// Flag-set packet: [type][flagId_hi][flagId_lo]  (3 bytes)
u8 Mp_EncodeFlagSet(u8 *out, u16 flagId);
bool8 Mp_DecodeFlagSet(const u8 *in, u8 len, u16 *flagId);

// Flag-clear packet: same layout as flag-set, different type byte (0x0E)
u8 Mp_EncodeFlagClear(u8 *out, u16 flagId);
bool8 Mp_DecodeFlagClear(const u8 *in, u8 len, u16 *flagId);

// Var-set packet: [type][varId_hi][varId_lo][value_hi][value_lo]  (5 bytes)
u8 Mp_EncodeVarSet(u8 *out, u16 varId, u16 value);
bool8 Mp_DecodeVarSet(const u8 *in, u8 len, u16 *varId, u16 *value);

// Boss-ready packet: [type][bossId]  (2 bytes)
u8 Mp_EncodeBossReady(u8 *out, u8 bossId);
bool8 Mp_DecodeBossReady(const u8 *in, u8 len, u8 *bossId);

// Gender packet: [type][gender]  (2 bytes).  gender is MALE/FEMALE.
u8 Mp_EncodeGender(u8 *out, u8 gender);
bool8 Mp_DecodeGender(const u8 *in, u8 len, u8 *gender);

// Boss-cancel packet: [type]  (1 byte)
u8 Mp_EncodeBossCancel(u8 *out);

// Seed-sync packet: [type][seed3][seed2][seed1][seed0]  big-endian (5 bytes)
u8 Mp_EncodeSeedSync(u8 *out, u32 seed);
bool8 Mp_DecodeSeedSync(const u8 *in, u8 len, u32 *seed);

// Full-sync packet: [type][len_hi][len_lo][data...] (variable length)
// Encode writes the 3-byte header + dataLen data bytes; returns total bytes written.
u16 Mp_EncodeFullSync(u8 *out, const u8 *data, u16 dataLen);
// Decode parses the header; sets *dataOut to in[3] and *dataLen to declared length.
// Returns FALSE if the buffer is too short for the declared payload.
bool8 Mp_DecodeFullSync(const u8 *in, u16 len, const u8 **dataOut, u16 *dataLen);

// ---------------------------------------------------------------------------
// PARTY_SYNC wire mon — the full battle-relevant snapshot of one party mon.
// The receiving ROM rebuilds a battle-identical mon in the partner half of
// gPlayerParty from these fields.  Display-only data (nickname, gender,
// language) doubles for gMultiPartnerParty.  58 bytes serialized
// (MP_PKT_PARTY_SYNC_MON_SIZE); all multi-byte fields big-endian.
// ---------------------------------------------------------------------------
#define MP_WIRE_NICK_LEN 11  // POKEMON_NAME_LENGTH + 1

struct MpWirePartyMon {
    u16 species;
    u16 heldItem;
    u8  level;
    u8  abilityNum;
    u32 personality;
    u32 otId;
    u16 moves[4];
    u8  pp[4];
    u16 hp;
    u16 maxHP;
    u16 atk;
    u16 def;
    u16 speed;
    u16 spAtk;
    u16 spDef;
    u32 status;
    u8  friendship;
    u8  gender;
    u8  language;
    u8  nickname[MP_WIRE_NICK_LEN];
};

// Serialize/deserialize one wire mon.  Encode returns bytes written
// (MP_PKT_PARTY_SYNC_MON_SIZE); decode returns FALSE only on NULL args.
u8    Mp_EncodePartyMon(u8 *out, const struct MpWirePartyMon *m);
bool8 Mp_DecodePartyMon(const u8 *in, struct MpWirePartyMon *m);

// ---------------------------------------------------------------------------
// Structures
// ---------------------------------------------------------------------------
struct CoopSettings {
    u8  randomizeEncounters : 1;
    u8  padding : 7;
    u32 encounterSeed;
};

// Address discovery table written by Multiplayer_Init so the Tauri host
// can locate every key variable regardless of build toolchain or codebase
// changes that shift IWRAM layout.
//
// Layout: [0] = MP_DISCOVERY_MAGIC
//         [1] = &gMultiplayerState
//         [2] = &gMpSendRing
//         [3] = &gMpRecvRing
//         [4] = &gCoopSettings
//
// Tauri scans IWRAM (0x03000000–0x03008000) in 4-byte strides looking for
// MP_DISCOVERY_MAGIC at [0], then reads [1]–[4] in one shot.
#define MP_DISCOVERY_MAGIC  0xC0DEC0DEu
// [0]=magic [1]=&gMultiplayerState [2]=&gMpSendRing [3]=&gMpRecvRing
// [4]=&gCoopSettings [5]=&gMpBlockExchange
extern u32 gMpAddrTable[6];

struct MultiplayerState {
    u8  role;            // MP_ROLE_*
    u8  connState;       // MP_STATE_*
    u8  partnerMapGroup;
    u8  partnerMapNum;
    u8  targetX;         // target tile X for ghost (last received partner position)
    u8  targetY;         // target tile Y for ghost
    u8  targetFacing;    // facing direction to set when ghost reaches target
    u8  ghostObjectEventId; // GHOST_INVALID_SLOT (0xFF) = not spawned
    u8  bossReadyBossId;    // 0 = not in readiness check; nonzero = we sent BOSS_READY
    u8  partnerBossId;      // 0 = partner not ready; nonzero = partner sent BOSS_READY
    u8  isInScript;         // TRUE while local player is executing a script
    u8  partnerIsInScript;  // TRUE while partner has sent SCRIPT_LOCK
    u8  posFrameCounter;    // counts frames; send position every 4 frames
    u16 partnerStarterSpecies; // 0 until partner sends MP_PKT_STARTER_PICK
    u16 myStarterSpecies;     // saved species from Multiplayer_SendStarterPick for resend
    u8  starterResendTimer;   // state-beacon cadence counter (name kept for layout
                              // stability — tools/mcp_gamestate hardcodes offsets)
    u8  remoteUpdateThisFrame; // set when any remote flag/var applied; cleared next Multiplayer_Update
    u8  partnerGender;         // MALE/FEMALE; meaningful only when gotPartnerGender is TRUE
    u8  gotPartnerGender;      // TRUE once partner has sent MP_PKT_GENDER
    u8  partnerName[PLAYER_NAME_LENGTH + 1]; // received via MP_PKT_NAME; default "???" until known
    u8  coopBattlePending;     // set by ScriptCheckBossStart when a connected boss-ready
                               // handshake completes; consumed by BattleSetup_ConfigureTrainerBattle
                               // to route the NEXT trainerbattle through the coop path
                               // regardless of mode.  Auto-clears once routed.
    u8  partnerPartySelectDone; // TRUE once partner's MP_PKT_PARTY_SYNC has been received and applied
    u8  followerGhostObjId;     // object event slot for partner's follower ghost; GHOST_INVALID_SLOT = none
    u16 partnerFollowerGfxId;   // OBJ_EVENT_GFX_* for partner's lead follower; 0 = no follower
    u16 lastSentFollowerGfxId;  // last gfx ID we sent; used to detect local follower changes
    u8  battleTurnReceived;     // TRUE when MP_PKT_BATTLE_TURN arrived (partner's move selection)
    u8  battleTurnMoveSlot;     // move slot index from partner (0-3)
    u8  battleTurnTarget;       // target battler from partner
    u8  battleTurnFlags;        // reserved flags (gimmick etc.) for future use
    // Reconnect support: cache our last-sent turn so we can resend after partner reconnects
    u8  battleTurnSent;         // TRUE while we've sent a turn and partner hasn't acked it yet
    u8  battleTurnSentMoveSlot;
    u8  battleTurnSentTarget;
    u8  battleTurnSentFlags;
    u8  bossResendTimer;        // retired — BOSS_READY repair now rides the state beacon
    u8  partySyncResendTimer;   // counts frames; resend MP_PKT_PARTY_SYNC every 60 frames while waiting
    // Heartbeat / disconnect resilience (features added in co-op v0.2)
    u8  pingTimer;              // counts frames; sends MP_PKT_PING every 120 frames (2s) when connected
    u8  lastCkptMapGroup;       // map group at last auto-checkpoint save; 0xFF = uninitialised
    u8  lastCkptMapNum;         // map num at last auto-checkpoint save
    u8  _pad2;                  // padding to keep u16 aligned
    u16 battleGraceTimer;       // counts frames partner has been disconnected mid-battle; AI fallback at 1800
    // Trainer-busy: tracks which trainer the partner is currently battling so we
    // suppress their vision cone and show a "Buzz off!" message on A-press.
    u8  partnerHasBusyTrainer;       // TRUE while partner is in a trainer battle
    u8  partnerBusyTrainerLocalId;   // localId of the trainer they're fighting
    u8  partnerBusyTrainerMapGroup;
    u8  partnerBusyTrainerMapNum;
    u8  sentBusyTrainer;             // TRUE after we sent TRAINER_BUSY; cleared when battle ends
    // --- Append new fields below this line ONLY: tools/mcp_gamestate/server.py
    // battle_diag hardcodes byte offsets of the fields above. ---
    // Battle turn reliability: each logical turn gets a sequence number (1-255,
    // never 0) so the state beacon can re-carry the cached turn idempotently.
    u8  battleTurnSeqOut;       // seq assigned to our last-sent turn; 0 = none this battle
    u8  battleTurnSeqApplied;   // seq of the last partner turn we applied (dedup/ordering)
    u8  _pad3[2];
    // Coop battle RNG lockstep: host generates coopBattleSeed per battle and
    // ships it in MP_PKT_PARTY_SYNC; both sides seed coopRngState from it in
    // Multiplayer_SetupCoopBattle.  Tagged battle-logic rolls (RandomUniform
    // et al.) draw from this stream during BATTLE_TYPE_COOP battles so damage
    // rolls / crits / AI choices stay identical across the mirrored sims.
    u32 coopBattleSeed;         // 0 = none adopted; both-zero still converges (fixed fallback)
    u32 coopRngState;           // xorshift32 state; advanced only by coop battle rolls
    // Party-sync mutual handshake (fixes asymmetric-loss deadlock, 2026-06-14).
    // The waitpartysync wait exits only once BOTH are TRUE, so a side keeps
    // resending its party until the partner confirms receipt — not merely
    // until it has received the partner's.  Reset per battle in
    // ScrCmd_waitcoopparty.  gotPartnerParty persists through the battle so the
    // state beacon keeps acking; it is broadcast as MP_BEACON_PARTYACK_BIT.
    u8  gotPartnerParty;        // TRUE once partner's PARTY_SYNC applied this battle
    u8  partnerGotMyParty;      // TRUE once partner's beacon acked our party (latched)
    // Async auto-checkpoint: the map-change/battle-end save runs one flash
    // sector per frame (MP_SAVE_* state machine in multiplayer.c) instead of
    // busy-looping all sectors in a single frame, so it never stalls a scene
    // transition.  0 = idle.  Session scratch, zeroed by Multiplayer_Init.
    u8  saveState;
    // Our own field-trainer lock, remembered so the state beacon can re-carry
    // it (MP_BEACON_BUSYTRAINER_BIT) and repair a dropped MP_PKT_TRAINER_BUSY /
    // TRAINER_FREE on the partner's side.  Set in Multiplayer_SendTrainerBusy,
    // valid while sentBusyTrainer is TRUE.  Session scratch, zeroed by
    // Multiplayer_Init.
    u8  sentBusyTrainerLocalId;
    u8  sentBusyTrainerMapGroup;
    u8  sentBusyTrainerMapNum;
    u8  _pad4[2];               // padding kept as-is (battle_diag offsets)
    // Coop-battle party stash/restore.  ScrCmd_waitcoopparty stashes the full
    // pre-battle party via SavePlayerParty() before the selection menu can
    // reorder gPlayerParty; CB2_CoopPartySelected records the original slot of
    // each selected mon; Multiplayer_OnBattleEnd copies each selected mon's
    // post-battle state back into its original stash slot, then
    // LoadPlayerParty() — restoring non-participants and evicting the partner
    // mons the battle engine placed in gPlayerParty[MULTI_PARTY_SIZE..].
    // Session scratch, zeroed by Multiplayer_Init.
    u8  coopPartyStashed;       // TRUE from waitcoopparty until battle-end restore
    u8  coopSelectedCount;      // number of local mons selected (1..MULTI_PARTY_SIZE)
    u8  coopSelectedSlots[MULTI_PARTY_SIZE]; // original gPlayerParty index per selection
    u8  _pad5[3];               // padding to keep the struct 4-byte aligned
    // Starter-claim handshake (see MP_CLAIM_* in constants/multiplayer.h).
    // Multiplayer_ClaimStarter sends the pick as a claim BEFORE givemon and
    // sets PENDING; resolved to GRANTED/DENIED by MP_PKT_STARTER_VERDICT, by
    // the partner's starter_taken/beacon carrying the same species (denial
    // repair), by disconnect (grant — solo rules), or by timeout (grant
    // backstop).  Session scratch, zeroed by Multiplayer_Init.
    u8  starterClaimState;      // MP_CLAIM_*
    u8  _pad6;
    u16 starterClaimSpecies;    // species being claimed; valid while not IDLE
    u16 starterClaimTimer;      // frames spent PENDING (timeout backstop)
    u8  _pad7[2];               // padding to keep the struct 4-byte aligned
};

extern struct MultiplayerState gMultiplayerState;
extern struct CoopSettings gCoopSettings;

// ---------------------------------------------------------------------------
// Core lifecycle
// ---------------------------------------------------------------------------
void Multiplayer_Init(void);
void Multiplayer_PollPackets(void);
void Multiplayer_Update(void);
// Frame-guarded wrapper — safe to call from multiple engine hooks per frame.
void Multiplayer_UpdateOncePerFrame(void);
// Battle-safe transport pump (poll + ping + state beacon, none of the
// overworld work).  Hooked into BattleMainCB2; no-op outside coop battles.
void Multiplayer_BattleTick(void);
// Menu-safe transport pump (poll + ping + state beacon).  Hooked into
// CB2_UpdatePartyMenu so the recv ring keeps draining while a party menu owns
// the main callback; no-op when disconnected.
void Multiplayer_MenuTick(void);

// Ghost NPC
void Multiplayer_SpawnGhostNPC(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing);
// Returns TRUE if the partner is currently battling the trainer in the given object event slot.
bool32 Multiplayer_IsPartnerBusyWithTrainer(u8 objectEventId);
void Multiplayer_DespawnGhost(void);
void Multiplayer_UpdateGhostPosition(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing);
// Returns the OBJ_EVENT_GFX_* constant to use when spawning the ghost: keyed
// off partnerGender once known, otherwise opposite-of-self.
u16 Multiplayer_GhostGraphicsId(void);

// Packet send helpers (Phase 2 — now implemented via ring buffer)
void Multiplayer_SendPosition(void);
void Multiplayer_SendTrainerBusy(u8 localId, u8 mapGroup, u8 mapNum);
// A field trainer spotted us — tell the partner so they can replay a cosmetic
// "!" + walk on the matching NPC (no controls lock, no battle).  direction is
// the trainer's facing; distance is the number of tiles it walks toward us.
void Multiplayer_SendTrainerApproach(u8 localId, u8 mapGroup, u8 mapNum, u8 direction, u8 distance);
// Partner side: play the controls-free, battle-free approach animation on the
// NPC with the given localId on the current map.  Implemented in trainer_see.c
// (reuses the exclamation-mark field effect + held-movement primitives).
void Multiplayer_PlayGhostTrainerApproach(u8 localId, u8 direction, u8 distance);
void Multiplayer_SendFlagSet(u16 flagId);
void Multiplayer_SendFlagClear(u16 flagId);
void Multiplayer_SendVarSet(u16 varId, u16 value);
void Multiplayer_SendBossReady(u8 bossId);
void Multiplayer_SendBossCancel(void);
// Send local player gender to partner.  Idempotent; safe to call repeatedly.
void Multiplayer_SendGender(void);
// Send local player name to partner.  Called alongside SendGender on connect.
void Multiplayer_SendName(void);
// Apply partner gender received over the wire.  If a ghost is already spawned
// with the wrong sprite, despawns it so the next GhostMapCheck respawns it
// with the correct one.
void Multiplayer_HandleRemoteGender(u8 gender);
// Store partner's name (received via MP_PKT_NAME) in gMultiplayerState.partnerName.
void Multiplayer_HandleRemoteName(const u8 *name);

// Boss readiness protocol (Phase 5).
// Each gym script calls the matching BossReady special, then polls via
// Multiplayer_ScriptCheckBossStart until the partner confirms or they're solo.
// Multiplayer_BossCancel is called if the player walks away without fighting.
void Multiplayer_BossReady_Brock(void);
void Multiplayer_BossReady_Misty(void);
void Multiplayer_BossReady_LtSurge(void);
void Multiplayer_BossReady_Erika(void);
void Multiplayer_BossReady_Koga(void);
void Multiplayer_BossReady_Sabrina(void);
void Multiplayer_BossReady_Blaine(void);
void Multiplayer_BossReady_Giovanni(void);
void Multiplayer_BossReady_Lorelei(void);
void Multiplayer_BossReady_Bruno(void);
void Multiplayer_BossReady_Agatha(void);
void Multiplayer_BossReady_Lance(void);
void Multiplayer_BossReady_Champion(void);
void Multiplayer_BossReady_RivalOaksLab(void);
void Multiplayer_BossReady_RivalRoute22_1(void);
void Multiplayer_BossReady_RivalCerulean(void);
void Multiplayer_BossReady_RivalSsAnne(void);
void Multiplayer_BossReady_RivalSilph(void);
void Multiplayer_BossReady_RivalRoute22_2(void);
void Multiplayer_BossReady_RivalChampion(void);
void Multiplayer_BossReady_RivalPokemonTower(void);
void Multiplayer_BossReady_Escort(void);
void Multiplayer_BossCancel(void);
// Returns 1 when both players (or solo) are ready to start the boss battle,
// then clears the readiness state.  Called via 'specialvar VAR_RESULT, ...' in scripts.
u16  Multiplayer_ScriptCheckBossStart(void);
// Returns 1 if connected to a partner; 0 otherwise.
// Called via 'specialvar VAR_RESULT, ...' in scripts to choose the connected path.
u16  Multiplayer_IsConnected(void);
// Returns 1 if partner has already signalled readiness for the Oak's-Lab rival fight.
u16  Multiplayer_IsPartnerWaitingForBoss_RivalOaksLab(void);
// Native callback for SCR_OP_WAITBOSSSTART: returns TRUE when both players ready (or solo).
// Sets VAR_RESULT = 1 on success, VAR_RESULT = 0 if the player pressed B to cancel.
bool8 Multiplayer_NativePollBossStart(void);

// Co-op boss battle setup — call before DoTrainerBattle() when starting a coop gym fight.
// Sets the link player table, block-exchange state, and creates the relay task.
void Multiplayer_SetupCoopBattle(void);

// Co-op battle turn sync.
// Call Multiplayer_SendBattleTurn from the player controller when the local player
// confirms their move selection; the partner controller polls Multiplayer_PollPackets
// each frame and calls Multiplayer_HandleBattleTurn when the packet arrives.
// SendBattleTurn assigns a fresh sequence number per logical turn; ResendBattleTurn
// re-emits the cached turn unchanged (reconnect path — must NOT bump the seq).
// HandleBattleTurn drops duplicates and stale (reordered) turns by seq, so it is
// safe to feed it from both the direct packet and the repeating state beacon.
void Multiplayer_SendBattleTurn(u8 moveSlot, u8 target, u8 flags);
void Multiplayer_ResendBattleTurn(void);
void Multiplayer_HandleBattleTurn(u8 seq, u8 moveSlot, u8 target, u8 flags);
// Returns TRUE when running a BATTLE_TYPE_COOP battle.
bool32 Multiplayer_IsCoopBattle(void);
// Maps a role-canonical player index (0/1, agreed by both sims) to the LOCAL
// player battler id (0 or 2). Used to make opponent random/tie target picks
// resolve to the same physical mon on both instances despite the mirrored
// battler layout. See the definition for the host/guest mapping.
u32 Multiplayer_CanonicalPlayerTarget(u32 canonicalIdx);
// Coop battle lockstep RNG: next 16-bit draw from the dedicated xorshift32
// stream (seeded from coopBattleSeed in Multiplayer_SetupCoopBattle).
u16 Multiplayer_CoopBattleRandom16(void);

// Follower ghost: partner's lead follower Pokémon displayed 1 tile behind the partner ghost.
void Multiplayer_SendFollowerGfx(u16 gfxId);
void Multiplayer_HandleRemoteFollowerGfx(u16 gfxId);

// Party selection for co-op boss battles.
// CB2_CoopPartySelected: savedCallback called by the party menu when player confirms picks.
//   Reorders gPlayerParty[0..n-1], sends MP_PKT_PARTY_SYNC, returns to field via script.
void CB2_CoopPartySelected(void);
// Send local party selection to partner.  Reads gSelectedOrderFromParty[] and gPlayerParty.
void Multiplayer_SendPartySync(void);
// Apply partner party sync from received packet bytes.  Fills gMultiPartnerParty and gPlayerParty[3..5].
void Multiplayer_HandleRemotePartySync(const u8 *data, u8 n_mons);
// Native callback for SCR_OP_WAITPARTYSYNC: returns TRUE when partner sync received (or solo).
bool8 Multiplayer_NativePollPartySync(void);

// Full sync (Phase 3) — called by host on connect to bring guest up to date.
// Builds a FULL_SYNC packet from the current flag state and enqueues it.
// Receiver calls Multiplayer_ApplyFullSync to OR the payload into its flags.
void Multiplayer_SendFullSync(void);
void Multiplayer_ApplyFullSync(const u8 *payload, u16 payloadLen);

// Flag/var sync helpers
bool32 IsSyncableFlag(u16 flagId);
bool32 IsSyncableVar(u16 varId);   // returns FALSE until var audit in Phase 3

// Remote update handlers — called by ProcessOneRecvPacket when a FLAG_SET or
// VAR_SET arrives from the partner. These set sIsRemoteUpdate before calling
// FlagSet/VarSet so we don't echo the packet back.
void Multiplayer_HandleRemoteFlagSet(u16 flagId);
void Multiplayer_HandleRemoteFlagClear(u16 flagId);
void Multiplayer_HandleRemoteVarSet(u16 varId, u16 value);

// Script mutex — called from ScriptContext_SetupScript / ScriptContext_RunScript.
// Advisory only: sends SCRIPT_LOCK / SCRIPT_UNLOCK to inform the partner.
void Multiplayer_OnScriptStart(void);
void Multiplayer_OnScriptEnd(void);
bool32 Multiplayer_IsPartnerInScript(void);

// Seeded PRNG (xorshift32) — used for the encounter randomizer.
// Multiplayer_SeedRng initialises the state; seed 0 is mapped to a nonzero value.
// Multiplayer_NextRandom advances and returns the next value.
void Multiplayer_SeedRng(u32 seed);
u32  Multiplayer_NextRandom(void);

// Per-slot species hash.  Returns SPECIES_NONE (0) if randomisation is
// disabled or the encounter seed is unset; otherwise a Gen I-IV species (1-493).
// tableAddr is the ROM address of the WildPokemon[] array for this encounter table.
u16  Multiplayer_GetRandomizedSpecies(u32 tableAddr, u8 slotIndex);
u16  Multiplayer_GetRandomizedStarter(u8 slot);

// Seed sync (Phase 4) — host calls GenerateSeed() on session start, then
// broadcasts it with SendSeedSync().  Guest's ProcessOneRecvPacket applies the
// received seed to gCoopSettings.encounterSeed automatically.
u32  Multiplayer_GenerateSeed(void);
void Multiplayer_SendSeedSync(u32 seed);

// Auto-checkpoint — call when a battle ends to trigger a background save.
void Multiplayer_OnBattleEnd(void);

// Event log — async partner event batching.
void Multiplayer_LogEvent(u8 type, u8 d0, u8 d1, u8 d2);
void Multiplayer_SendEventLog(void);
void Multiplayer_ClearEventLog(void);

// Item sync — call after AddBagItem succeeds for field pickups and NPC gifts.
// Do NOT call for shop purchases or Pokémon gifts (eggs, starters, etc.).
// Partner's ROM will call AddBagItem with the same itemId and quantity.
void Multiplayer_OnItemGiven(u16 itemId, u8 quantity);

// Starter coordination (Phase 1.5)
// Call after confirming starter to inform the partner which species was taken.
void Multiplayer_SendStarterPick(void);
// Returns the species for Pokémon in ball slot 0/1/2 (randomized or canonical).
// Slot 0 = Bulbasaur position, 1 = Squirtle position, 2 = Charmander position.
u16 Multiplayer_GetStarterForBall0(void);
u16 Multiplayer_GetStarterForBall1(void);
u16 Multiplayer_GetStarterForBall2(void);
// Returns the species the rival should take (neither player's pick).
u16 Multiplayer_GetRivalStarterSpecies(void);
// Returns 0/1/2 for which ball slot the rival takes (for walk movement dispatch).
u16 Multiplayer_GetRivalStarterSlot(void);
// Returns dispatch key (0=Charmander,1=Bulbasaur,2=Squirtle rival) for RivalBattleDispatch.
u16 Multiplayer_GetRivalBattleKey(void);
// Returns 1 if the partner has already taken ball slot 0/1/2.
u16 Multiplayer_IsBall0TakenByPartner(void);
// Re-derives the three FLAG_HIDE_*_BALL flags from durable pick state.
void Multiplayer_RederiveStarterBallFlags(void);
u16 Multiplayer_IsBall1TakenByPartner(void);
u16 Multiplayer_IsBall2TakenByPartner(void);
// Returns TRUE if partner has picked (or we're offline) — used by waitstarterpick.
bool8 Multiplayer_NativePollPartnerStarterPick(void);
// Claim a starter BEFORE givemon: reads the ball slot (0-2) from
// gSpecialVar_0x8004, sends the pick to the relay as a claim, and arms the
// waitstarterclaim poll.  Grants immediately when offline.
void Multiplayer_ClaimStarter(void);
// Returns TRUE once the claim resolved (GRANTED or DENIED) — used by waitstarterclaim.
bool8 Multiplayer_NativePollStarterClaim(void);
// Returns 1 if the resolved claim was granted, 0 if denied — for specialvar branching.
u16 Multiplayer_GetStarterClaimResult(void);

#endif // GUARD_MULTIPLAYER_H
