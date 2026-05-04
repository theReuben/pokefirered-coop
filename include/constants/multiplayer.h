#ifndef GUARD_CONSTANTS_MULTIPLAYER_H
#define GUARD_CONSTANTS_MULTIPLAYER_H

// Packet types sent/received over the ring buffer interface
#define MP_PKT_POSITION     0x01   // 6 bytes total
#define MP_PKT_FLAG_SET     0x02   // 3 bytes total
#define MP_PKT_VAR_SET      0x03   // 5 bytes total
#define MP_PKT_BOSS_READY   0x04   // 2 bytes total
#define MP_PKT_BOSS_CANCEL  0x05   // 1 byte total
#define MP_PKT_SEED_SYNC    0x06   // 5 bytes total
#define MP_PKT_FULL_SYNC    0x07   // 3+len bytes total (variable)
#define MP_PKT_SCRIPT_LOCK  0x08   // 1 byte — player entered a script interaction
#define MP_PKT_SCRIPT_UNLOCK 0x09  // 1 byte — player left the script interaction
#define MP_PKT_BOSS_START           0x0A   // 1 byte — relay confirms both players ready
#define MP_PKT_PARTNER_CONNECTED    0x0B   // 1 byte — partner joined the session
#define MP_PKT_PARTNER_DISCONNECTED 0x0C   // 1 byte — partner left the session
#define MP_PKT_ITEM_GIVE            0x0D   // 4 bytes — give item to partner (field/gift, not shop)
#define MP_PKT_FLAG_CLEAR           0x0E   // 3 bytes — a syncable flag was cleared locally

// Boss IDs sent in MP_PKT_BOSS_READY packets (ordered by game progression)
#define BOSS_ID_BROCK       1
#define BOSS_ID_MISTY       2
#define BOSS_ID_LT_SURGE    3
#define BOSS_ID_ERIKA       4
#define BOSS_ID_KOGA        5
#define BOSS_ID_SABRINA     6
#define BOSS_ID_BLAINE      7
#define BOSS_ID_GIOVANNI    8
#define BOSS_ID_LORELEI     9
#define BOSS_ID_BRUNO       10
#define BOSS_ID_AGATHA      11
#define BOSS_ID_LANCE       12
#define BOSS_ID_CHAMPION    13
#define BOSS_ID_RIVAL_OAKS_LAB  14
#define BOSS_ID_RIVAL_ROUTE22_1 15
#define BOSS_ID_RIVAL_CERULEAN  16
#define BOSS_ID_RIVAL_SS_ANNE   17
#define BOSS_ID_RIVAL_SILPH     18
#define BOSS_ID_RIVAL_ROUTE22_2 19
#define BOSS_ID_RIVAL_CHAMPION  20

// Script variables repurposed for co-op state (from VAR_UNUSED_0x40F7/0x40F8)
// VAR_COOP_CONNECTED  — read by gym scripts to choose connected vs solo path
// VAR_BOSS_BATTLE_STATE — unused; readiness state is polled via Multiplayer_ScriptCheckBossStart
#define VAR_COOP_CONNECTED      0x40F7
#define VAR_BOSS_BATTLE_STATE   0x40F8

// Ring buffer constants
#define MP_RING_SIZE        256    // power-of-2; u8 head/tail wrap naturally
#define MP_RING_MAGIC       0xC0   // sanity sentinel for Tauri to locate buffer

// Fixed packet sizes (type byte included)
#define MP_PKT_SIZE_POSITION      6
#define MP_PKT_SIZE_FLAG_SET      3
#define MP_PKT_SIZE_VAR_SET       5
#define MP_PKT_SIZE_BOSS_READY    2
#define MP_PKT_SIZE_BOSS_CANCEL   1
#define MP_PKT_SIZE_SEED_SYNC     5
#define MP_PKT_SIZE_FULL_SYNC_HDR 3  // type + len_hi + len_lo; data follows
#define MP_PKT_SIZE_SCRIPT_LOCK   1
#define MP_PKT_SIZE_SCRIPT_UNLOCK 1
#define MP_PKT_SIZE_BOSS_START              1
#define MP_PKT_SIZE_PARTNER_CONNECTED       1
#define MP_PKT_SIZE_PARTNER_DISCONNECTED    1
#define MP_PKT_SIZE_ITEM_GIVE               4  // type + item_hi + item_lo + quantity
#define MP_PKT_SIZE_FLAG_CLEAR              3  // type + flagId_hi + flagId_lo

// Player roles assigned by relay server
#define MP_ROLE_NONE        0
#define MP_ROLE_HOST        1
#define MP_ROLE_GUEST       2

// Multiplayer connection states
#define MP_STATE_DISCONNECTED   0
#define MP_STATE_CONNECTING     1
#define MP_STATE_CONNECTED      2

// Save section for co-op persistent settings (last available slot)
#define SAVE_SECTION_COOP_SETTINGS  14

// ---------------------------------------------------------------------------
// Syncable flag ranges (audited from include/constants/flags_frlg.h)
//
// NOT synced:
//   0x000-0x01F   Temp flags  — cleared every map load; per-instance state
//   0x300-0x38E   Unnamed gap — appears unused; skip to avoid surprises
//   0x390-0x3CF   DAILY_FLAGS — per-player daily event state
//   0x3D8-0x3E7   MYSTERY_GIFT flags — per-player
//   0x800+        SYS_FLAGS   — safari mode, VS seeker, etc.; local state
// ---------------------------------------------------------------------------

// Story range: includes NPC hide/show flags (0x020-0x22F) and named story-
// completion flags (GOT_*, BEAT_*, VISITED_*, 0x230-0x2FF).
// VAR_MAP_SCENE_* sync is disabled, so NPC HIDE flags can sync safely —
// each player still runs their own scripted sequences, and hide/show flags
// just control which NPCs are visible (shared world state).
#define SYNC_FLAG_STORY_START       0x020
#define SYNC_FLAG_STORY_END         0x2FF

// Hidden ground items (A-button pickup spots).
// FLAG_HIDDEN_ITEMS_START = 0x3E8; last used item is at offset 190 = 0x4A6.
#define SYNC_FLAG_ITEMS_START       0x3E8
#define SYNC_FLAG_ITEMS_END         0x4A6

// Gym leader + Elite Four + Champion clear flags.
#define SYNC_FLAG_BOSSES_START      0x4B0   // FLAG_DEFEATED_BROCK
#define SYNC_FLAG_BOSSES_END        0x4BC   // FLAG_DEFEATED_CHAMP

// Badge flags (SYSTEM_FLAGS + 0x7 through + 0xE = 0x867–0x86E).
// These gate Victory Road and post-game content, so both players need them.
#define SYNC_FLAG_BADGES_START      0x867   // FLAG_BADGE01_GET
#define SYNC_FLAG_BADGES_END        0x86E   // FLAG_BADGE08_GET

// Trainer defeat flags (one bit per trainer, 0x500–0x7FF for FRLG).
// TRAINER_FLAGS_START / TRAINER_FLAGS_END from flags_frlg.h.
#define SYNC_FLAG_TRAINERS_START    0x500
#define SYNC_FLAG_TRAINERS_END      0x7FF

// ---------------------------------------------------------------------------
// FULL_SYNC payload layout — five contiguous byte slices of flags[].
// Applied on the receiver side with OR (union-wins: any flag set by either
// player remains set).
//
//   Payload offset    flags[] byte range          Purpose
//   0..91             [4..95]   (story/NPC HIDE)  NPC hide/show + GOT_*/BEAT_*/VISITED_* (0x020-0x2FF)
//   92..115           [125..148](hidden items)     ground item pickups
//   116..117          [150..151](bosses)           gym leader / E4 / champion clears
//   118..213          [160..255](trainers)         trainer defeat bits
//   214..215          [268..269](badges)           badge flags
//   Total: 216 bytes
// ---------------------------------------------------------------------------
#define FULL_SYNC_STORY_BYTE_START      (SYNC_FLAG_STORY_START    / 8)  /* 4  */
#define FULL_SYNC_STORY_BYTE_END        (SYNC_FLAG_STORY_END      / 8)  /* 95 */
// Last byte of NPC HIDE range (0x22F/8=69). ApplyFullSync uses AND below this,
// OR from byte 70 onward (story-completion flags that only accumulate).
#define FULL_SYNC_STORY_HIDE_BYTE_END   (0x22F                    / 8)  /* 69 */
#define FULL_SYNC_ITEMS_BYTE_START      (SYNC_FLAG_ITEMS_START    / 8)  /* 125 */
#define FULL_SYNC_ITEMS_BYTE_END        (SYNC_FLAG_ITEMS_END      / 8)  /* 148 */
#define FULL_SYNC_BOSSES_BYTE_START     (SYNC_FLAG_BOSSES_START   / 8)  /* 150 */
#define FULL_SYNC_BOSSES_BYTE_END       (SYNC_FLAG_BOSSES_END     / 8)  /* 151 */
#define FULL_SYNC_TRAINERS_BYTE_START   (SYNC_FLAG_TRAINERS_START / 8)  /* 160 */
#define FULL_SYNC_TRAINERS_BYTE_END     (SYNC_FLAG_TRAINERS_END   / 8)  /* 255 */
#define FULL_SYNC_BADGES_BYTE_START     (SYNC_FLAG_BADGES_START   / 8)  /* 268 */
#define FULL_SYNC_BADGES_BYTE_END       (SYNC_FLAG_BADGES_END     / 8)  /* 269 */

#define FULL_SYNC_STORY_LEN    \
    (FULL_SYNC_STORY_BYTE_END    - FULL_SYNC_STORY_BYTE_START    + 1)  /* 92  */
#define FULL_SYNC_ITEMS_LEN    \
    (FULL_SYNC_ITEMS_BYTE_END    - FULL_SYNC_ITEMS_BYTE_START    + 1)  /* 24  */
#define FULL_SYNC_BOSSES_LEN   \
    (FULL_SYNC_BOSSES_BYTE_END   - FULL_SYNC_BOSSES_BYTE_START   + 1)  /* 2   */
#define FULL_SYNC_TRAINERS_LEN \
    (FULL_SYNC_TRAINERS_BYTE_END - FULL_SYNC_TRAINERS_BYTE_START + 1)  /* 96  */
#define FULL_SYNC_BADGES_LEN   \
    (FULL_SYNC_BADGES_BYTE_END   - FULL_SYNC_BADGES_BYTE_START   + 1)  /* 2   */

// Total data bytes in a FULL_SYNC payload (fits within the 252-byte ring max).
#define FULL_SYNC_PAYLOAD_SIZE \
    (FULL_SYNC_STORY_LEN + FULL_SYNC_ITEMS_LEN + FULL_SYNC_BOSSES_LEN + FULL_SYNC_TRAINERS_LEN + FULL_SYNC_BADGES_LEN) /* 216 */

// ---------------------------------------------------------------------------
// Syncable variable ranges (FRLG-specific vars)
//
// VAR_MAP_SCENE_* (0x4050–0x408B): per-map story state vars.
//   These track how far through each location's scripted events the players
//   are (e.g. whether Oak has been battled, Rival has been fought at Cerulean,
//   Silph Co. has been cleared).  Both players must share this state so that
//   story-gated NPCs and events remain consistent.
//
// NOT synced:
//   0x4000–0x404F  Per-player step counters, RNG state, bag state, Pokédex
//   0x408C–0x40F6  Frontier/daily/Quest-Log state; all per-player
//   0x40F7          VAR_COOP_CONNECTED — local co-op flag; do not echo
//   0x40F8          VAR_BOSS_BATTLE_STATE — local readiness flag; do not echo
//   0x40F9–0x40FF  Reserved / unused
//   0x8000–0x8014  Special in-RAM vars (RESULT, FACING, etc.) — transient
// ---------------------------------------------------------------------------
#define SYNC_VAR_MAP_SCENE_START    0x4050   // VAR_MAP_SCENE_PALLET_TOWN_OAK
#define SYNC_VAR_MAP_SCENE_END      0x408B   // VAR_MAP_SCENE_MT_MOON_B2F

#endif // GUARD_CONSTANTS_MULTIPLAYER_H
