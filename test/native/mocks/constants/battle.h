#ifndef GUARD_CONSTANTS_BATTLE_H
#define GUARD_CONSTANTS_BATTLE_H

// Minimal mock for native unit tests.
// Provides only the BATTLE_TYPE_* and action constants used by multiplayer.c.
// Avoids pulling in constants/pokemon.h (which is needed for TYPE_ROCK etc.).

#define BATTLE_TYPE_DOUBLE             (1 << 0)
#define BATTLE_TYPE_LINK               (1 << 1)
#define BATTLE_TYPE_MULTI              (1 << 6)
#define BATTLE_TYPE_TRAINER            (1 << 3)
#define BATTLE_TYPE_INGAME_PARTNER     (1 << 10)
#define BATTLE_TYPE_IS_MASTER          (1 << 14)
#define BATTLE_TYPE_SAFARI             (1 << 15)
#define BATTLE_TYPE_FIRST_BATTLE       (1 << 16)
#define BATTLE_TYPE_RECORDED           (1 << 19)
#define BATTLE_TYPE_FRONTIER           (1 << 20)
#define BATTLE_TYPE_GHOST              (1 << 26)
#define BATTLE_TYPE_TOWER_LINK_MULTI   (1 << 27)
#define BATTLE_TYPE_TWO_OPPONENTS      (1 << 28)
#define BATTLE_TYPE_COOP               (1 << 30)

#define BATTLE_TYPE_MORE_THAN_TWO_BATTLERS \
    (BATTLE_TYPE_DOUBLE | BATTLE_TYPE_MULTI | BATTLE_TYPE_INGAME_PARTNER | BATTLE_TYPE_TWO_OPPONENTS)
#define BATTLE_TYPE_PLAYER_HAS_PARTNER \
    (BATTLE_TYPE_MULTI | BATTLE_TYPE_INGAME_PARTNER | BATTLE_TYPE_TOWER_LINK_MULTI)

#define MAX_BATTLERS_COUNT  4
#define FRONTIER_PARTY_SIZE 3

#endif // GUARD_CONSTANTS_BATTLE_H
