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

// Ring buffer constants
#define MP_RING_SIZE        256    // power-of-2; u8 head/tail wrap naturally
#define MP_RING_MAGIC       0xC0   // sanity sentinel for Tauri to locate buffer

// Fixed packet sizes (type byte included)
#define MP_PKT_SIZE_POSITION    6
#define MP_PKT_SIZE_FLAG_SET    3
#define MP_PKT_SIZE_VAR_SET     5
#define MP_PKT_SIZE_BOSS_READY  2
#define MP_PKT_SIZE_BOSS_CANCEL 1
#define MP_PKT_SIZE_SEED_SYNC   5
#define MP_PKT_SIZE_FULL_SYNC_HDR 3  // type + len_hi + len_lo; data follows

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

// Story range: NPC hide/show (0x028), item ball pickups (0x154),
//              and story quest flags (STORY_FLAGS_START=0x230) through 0x2FF.
// We start at 0x020 (first named FRLG flag) to be safe about the gap 0x020-0x027.
#define SYNC_FLAG_STORY_START       0x020
#define SYNC_FLAG_STORY_END         0x2FF

// Hidden ground items (A-button pickup spots).
// FLAG_HIDDEN_ITEMS_START = 0x3E8; last used item is at offset 190 = 0x4A6.
#define SYNC_FLAG_ITEMS_START       0x3E8
#define SYNC_FLAG_ITEMS_END         0x4A6

// Gym leader + Elite Four + Champion clear flags.
#define SYNC_FLAG_BOSSES_START      0x4B0   // FLAG_DEFEATED_BROCK
#define SYNC_FLAG_BOSSES_END        0x4BC   // FLAG_DEFEATED_CHAMP

// Trainer defeat flags (one bit per trainer, 0x500–0x7FF for FRLG).
// TRAINER_FLAGS_START / TRAINER_FLAGS_END from flags_frlg.h.
#define SYNC_FLAG_TRAINERS_START    0x500
#define SYNC_FLAG_TRAINERS_END      0x7FF

// ---------------------------------------------------------------------------
// FULL_SYNC payload layout — four contiguous byte slices of flags[].
// Applied on the receiver side with OR (union-wins: any flag set by either
// player remains set).
//
//   Payload offset    flags[] byte range          Purpose
//   0..91             [4..95]   (story)            NPC hide/show, items, story quest
//   92..115           [125..148](hidden items)      ground item pickups
//   116..117          [150..151](bosses)            gym leader / E4 / champion clears
//   118..213          [160..255](trainers)          trainer defeat bits
//   Total: 214 bytes
// ---------------------------------------------------------------------------
#define FULL_SYNC_STORY_BYTE_START      (SYNC_FLAG_STORY_START    / 8)  /*  4 */
#define FULL_SYNC_STORY_BYTE_END        (SYNC_FLAG_STORY_END      / 8)  /* 95 */
#define FULL_SYNC_ITEMS_BYTE_START      (SYNC_FLAG_ITEMS_START    / 8)  /* 125 */
#define FULL_SYNC_ITEMS_BYTE_END        (SYNC_FLAG_ITEMS_END      / 8)  /* 148 */
#define FULL_SYNC_BOSSES_BYTE_START     (SYNC_FLAG_BOSSES_START   / 8)  /* 150 */
#define FULL_SYNC_BOSSES_BYTE_END       (SYNC_FLAG_BOSSES_END     / 8)  /* 151 */
#define FULL_SYNC_TRAINERS_BYTE_START   (SYNC_FLAG_TRAINERS_START / 8)  /* 160 */
#define FULL_SYNC_TRAINERS_BYTE_END     (SYNC_FLAG_TRAINERS_END   / 8)  /* 255 */

#define FULL_SYNC_STORY_LEN    \
    (FULL_SYNC_STORY_BYTE_END    - FULL_SYNC_STORY_BYTE_START    + 1)  /* 92  */
#define FULL_SYNC_ITEMS_LEN    \
    (FULL_SYNC_ITEMS_BYTE_END    - FULL_SYNC_ITEMS_BYTE_START    + 1)  /* 24  */
#define FULL_SYNC_BOSSES_LEN   \
    (FULL_SYNC_BOSSES_BYTE_END   - FULL_SYNC_BOSSES_BYTE_START   + 1)  /* 2   */
#define FULL_SYNC_TRAINERS_LEN \
    (FULL_SYNC_TRAINERS_BYTE_END - FULL_SYNC_TRAINERS_BYTE_START + 1)  /* 96  */

// Total data bytes in a FULL_SYNC payload (fits within the 252-byte ring max).
#define FULL_SYNC_PAYLOAD_SIZE \
    (FULL_SYNC_STORY_LEN + FULL_SYNC_ITEMS_LEN + FULL_SYNC_BOSSES_LEN + FULL_SYNC_TRAINERS_LEN) /* 214 */

#endif // GUARD_CONSTANTS_MULTIPLAYER_H
