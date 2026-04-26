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

// Syncable flag ranges (filled in during Phase 3 — flag audit)
// Placeholder values; updated when flags.h is audited.
#define SYNC_FLAG_TRAINERS_START    0x0000
#define SYNC_FLAG_TRAINERS_END      0x0000
#define SYNC_FLAG_STORY_START       0x0000
#define SYNC_FLAG_STORY_END         0x0000

#endif // GUARD_CONSTANTS_MULTIPLAYER_H
