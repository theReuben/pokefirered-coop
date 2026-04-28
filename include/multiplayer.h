#ifndef GUARD_MULTIPLAYER_H
#define GUARD_MULTIPLAYER_H

#include "global.h"
#include "constants/multiplayer.h"
#include "constants/event_objects.h"

// Ghost NPC uses the FRLG Green (Leaf) walking sprite — visually distinct from Red.
#define OBJ_EVENT_GFX_PLAYER2      OBJ_EVENT_GFX_GREEN_NORMAL
// LocalId 0xFE is above any map-defined NPC (maps rarely use IDs > 10).
#define GHOST_LOCAL_ID             0xFE
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
#define MP_DEBUG_TEST_MAP_GROUP    0   // Pallet Town area map group for Route 1
#define MP_DEBUG_TEST_MAP_NUM      16  // MAP_ROUTE1 index
#define MP_DEBUG_TEST_X            8
#define MP_DEBUG_TEST_Y            5

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

// Var-set packet: [type][varId_hi][varId_lo][value_hi][value_lo]  (5 bytes)
u8 Mp_EncodeVarSet(u8 *out, u16 varId, u16 value);
bool8 Mp_DecodeVarSet(const u8 *in, u8 len, u16 *varId, u16 *value);

// Boss-ready packet: [type][bossId]  (2 bytes)
u8 Mp_EncodeBossReady(u8 *out, u8 bossId);
bool8 Mp_DecodeBossReady(const u8 *in, u8 len, u8 *bossId);

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
// Structures
// ---------------------------------------------------------------------------
struct CoopSettings {
    u8  randomizeEncounters : 1;
    u8  padding : 7;
    u32 encounterSeed;
};

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
};

extern struct MultiplayerState gMultiplayerState;
extern struct CoopSettings gCoopSettings;

// ---------------------------------------------------------------------------
// Core lifecycle
// ---------------------------------------------------------------------------
void Multiplayer_Init(void);
void Multiplayer_Update(void);

// Ghost NPC
void Multiplayer_SpawnGhostNPC(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing);
void Multiplayer_DespawnGhost(void);
void Multiplayer_UpdateGhostPosition(u8 mapGroup, u8 mapNum, u8 x, u8 y, u8 facing);

// Packet send helpers (Phase 2 — now implemented via ring buffer)
void Multiplayer_SendPosition(void);
void Multiplayer_SendFlagSet(u16 flagId);
void Multiplayer_SendVarSet(u16 varId, u16 value);
void Multiplayer_SendBossReady(u8 bossId);
void Multiplayer_SendBossCancel(void);

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
void Multiplayer_BossCancel(void);
// Returns 1 when both players (or solo) are ready to start the boss battle,
// then clears the readiness state.  Called via 'specialvar VAR_RESULT, ...' in scripts.
u16  Multiplayer_ScriptCheckBossStart(void);
// Returns 1 if connected to a partner; 0 otherwise.
// Called via 'specialvar VAR_RESULT, ...' in scripts to choose the connected path.
u16  Multiplayer_IsConnected(void);
// Native callback for SCR_OP_WAITBOSSSTART: returns TRUE when both players ready (or solo).
bool8 Multiplayer_NativePollBossStart(void);

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

// Seed sync (Phase 4) — host calls GenerateSeed() on session start, then
// broadcasts it with SendSeedSync().  Guest's ProcessOneRecvPacket applies the
// received seed to gCoopSettings.encounterSeed automatically.
u32  Multiplayer_GenerateSeed(void);
void Multiplayer_SendSeedSync(u32 seed);

#endif // GUARD_MULTIPLAYER_H
