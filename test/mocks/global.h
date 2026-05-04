#ifndef GUARD_GLOBAL_H
#define GUARD_GLOBAL_H

// Host-build mock for GBA global.h. Used in native C unit tests only.
// Uses GUARD_GLOBAL_H (same guard as the real header) so transitive includes
// of global.h from within ../include/ files pick up this mock instead.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// GBA integer types
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;

typedef volatile u8  vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

typedef u8  bool8;
typedef u16 bool16;
typedef u32 bool32;

#define TRUE  1
#define FALSE 0

// GBA memory-section attributes — no-ops for native host builds.
#define EWRAM_DATA
#define IWRAM_DATA
#define EWRAM_CODE
#define IWRAM_CODE

// Pull in OBJECT_EVENTS_COUNT, IS_FRLG, Direction enum, etc.
// This is a pure-defines/enums file with no GBA hardware dependencies.
#include "constants/global.h"

// Minimal item constants for multiplayer.c native build.
#define ITEM_NONE 0

// Forward declarations for pointer-only uses in headers we'll include later.
struct Sprite;
struct SpriteTemplate;
struct SpriteFrameImage;
struct ObjectEventTemplate;
struct MapPosition;

// Minimal Coords16 struct
struct Coords16 { s16 x; s16 y; };

// Minimal ObjectEvent fields accessed by multiplayer.c.
// Bit fields must mirror the layout in global.fieldmap.h (positions 0-2).
struct ObjectEvent {
    u32 active               : 1; // bit 0
    u32 heldMovementActive   : 1; // bit 1
    u32 heldMovementFinished : 1; // bit 2
    u32 _pad                 : 29;
    u16 graphicsId;
    u8  movementType;
    u8  trainerType;
    u8  localId;
    u8  mapNum;
    u8  mapGroup;
    u8  _elev;
    struct Coords16 initialCoords;
    struct Coords16 currentCoords;
    struct Coords16 previousCoords;
    u16 facingDirection      : 4; // mirrors global.fieldmap.h offset 0x18
    u16 _dirpad              : 12;
};

// Minimal PlayerAvatar fields accessed by multiplayer.c.
struct PlayerAvatar {
    u8 flags;
    u8 transitionFlags;
    u8 runningState;
    u8 tileTransitionState;
    u8 spriteId;
    u8 objectEventId;
};

extern struct PlayerAvatar gPlayerAvatar;

// OBJECT_EVENTS_COUNT comes from constants/global.h (included above).
extern struct ObjectEvent gObjectEvents[OBJECT_EVENTS_COUNT];

// WarpData and minimal SaveBlock1 for GhostMapCheck
struct WarpData { s8 mapGroup; s8 mapNum; s8 warpId; s16 x; s16 y; };

struct SaveBlock1 {
    u32 _pad0;
    struct WarpData location;
    // flags[id/8] up to badge flags at 0x86E (byte 269); use 280 for margin.
    u8  flags[280];
    // Additional fields omitted.
};

extern struct SaveBlock1 *gSaveBlock1Ptr;

// Minimal species constants needed by multiplayer.c starter functions.
#define SPECIES_BULBASAUR   1
#define SPECIES_CHARMANDER  4
#define SPECIES_SQUIRTLE    7

// Minimal VarGet/VarSet interface (backed by a small array in stubs.c).
#define TEMP_VARS_START     0x8000
#define VAR_TEMP_0          (TEMP_VARS_START + 0x0)
#define VAR_TEMP_1          (TEMP_VARS_START + 0x1)
#define VAR_TEMP_2          (TEMP_VARS_START + 0x2)
u16 VarGet(u16 varId);
void VarSet(u16 varId, u16 value);

#endif // GUARD_GLOBAL_H
