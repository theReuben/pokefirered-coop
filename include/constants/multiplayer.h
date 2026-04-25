#ifndef GUARD_CONSTANTS_MULTIPLAYER_H
#define GUARD_CONSTANTS_MULTIPLAYER_H

// Packet types sent/received over the serial link
#define MP_PKT_POSITION     0x01
#define MP_PKT_FLAG_SET     0x02
#define MP_PKT_VAR_SET      0x03
#define MP_PKT_BOSS_READY   0x04
#define MP_PKT_BOSS_CANCEL  0x05
#define MP_PKT_SEED_SYNC    0x06
#define MP_PKT_FULL_SYNC    0x07

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
