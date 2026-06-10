#ifndef GUARD_FOLLOWER_NPC_H
#define GUARD_FOLLOWER_NPC_H

// Minimal mock for native unit tests.

enum FollowerNpcData {
    FNPC_DATA_IN_PROGRESS,
    FNPC_DATA_WARP_END,
    FNPC_DATA_SURF_BLOB,
    FNPC_DATA_COME_OUT_DOOR,
    FNPC_DATA_FORCED_MOVEMENT,
    FNPC_DATA_OBJ_ID,
    FNPC_DATA_CURRENT_SPRITE,
    FNPC_DATA_DELAYED_STATE,
    FNPC_DATA_EVENT_FLAG,
    FNPC_DATA_GFX_ID,
};

bool8 PlayerHasFollowerNPC(void);
u32   GetFollowerNPCData(enum FollowerNpcData data);

#endif // GUARD_FOLLOWER_NPC_H
