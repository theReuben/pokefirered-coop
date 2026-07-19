#ifndef GUARD_POKEMON_H
#define GUARD_POKEMON_H

// Minimal mock for native unit tests.
// Replaces include/pokemon.h to avoid pulling in sprite.h, constants/battle.h, etc.

#include <stdarg.h>

struct Pokemon { u8 opaque[100]; };
struct OriginalTrainerId { u8 type; u32 value; };

// Subset of enum MonData — only values used by multiplayer.c.
// Values must match include/pokemon.h (enum starts at 0).
enum MonData {
    MON_DATA_PERSONALITY = 0,
    MON_DATA_STATUS      = 1,
    MON_DATA_OT_ID       = 2,
    MON_DATA_LANGUAGE    = 3,
    MON_DATA_HP          = 10,
    MON_DATA_NICKNAME    = 16,
    MON_DATA_SPECIES     = 18,
    MON_DATA_HELD_ITEM   = 19,
    MON_DATA_MOVE1       = 20,
    MON_DATA_PP1         = 24,
    MON_DATA_FRIENDSHIP  = 39,
    MON_DATA_ABILITY_NUM = 55,
    MON_DATA_LEVEL       = 64,
    MON_DATA_MAX_HP      = 65,
    MON_DATA_ATK         = 66,
    MON_DATA_DEF         = 67,
    MON_DATA_SPEED       = 68,
    MON_DATA_SPATK       = 69,
    MON_DATA_SPDEF       = 70,
};

// The real GetMonData is a 2-or-3-arg macro. Use a variadic wrapper so both
// call sites (with and without the output buffer) compile cleanly.
u32 GetMonData_stub(struct Pokemon *mon, s32 field, ...);
#define GetMonData GetMonData_stub

void SetMonData(struct Pokemon *mon, s32 field, const void *data);
void CreateMon(struct Pokemon *mon, u16 species, u8 level, u32 personality,
               struct OriginalTrainerId otId);
u8   GetMonGender(struct Pokemon *mon);

// OriginalTrainerId convenience constructors.
// OT_ID_PLAYER_ID / OT_ID_PRESET / OT_ID_RANDOM_NO_SHINY are enum values defined in
// constants/pokemon.h (pulled in transitively via constants/battle_frontier.h).
#define OTID_STRUCT_PLAYER_ID       ((struct OriginalTrainerId) {OT_ID_PLAYER_ID, 0})
#define OTID_STRUCT_PRESET(value)   ((struct OriginalTrainerId) {OT_ID_PRESET, value})
#define OTID_STRUCT_RANDOM_NO_SHINY ((struct OriginalTrainerId) {OT_ID_RANDOM_NO_SHINY, 0})

extern struct Pokemon gPlayerParty[PARTY_SIZE * 2];
extern u8 gPlayerPartyCount;

// Saved-party stash API (real declarations live in include/pokemon.h).
// Stub implementations in stubs.c copy to/from gSavedPlayerParty[] so tests
// can exercise the coop stash/restore path.
struct Pokemon *GetSavedPlayerPartyMon(u32 index);
u8 *GetSavedPlayerPartyCount(void);
void SavePlayerPartyMon(u32 index, struct Pokemon *mon);

#endif // GUARD_POKEMON_H
