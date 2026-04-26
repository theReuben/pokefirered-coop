#ifndef GUARD_MOCK_GLOBAL_H
#define GUARD_MOCK_GLOBAL_H

// Host-build stubs for GBA types. Used in native C unit tests only.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

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

// Forward declarations for pointer-only uses in event_object_movement.h
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
    u8  graphicsId;
    u8  movementType;
    u8  mapGroup;
    u8  mapNum;
    u8  spriteId;
    u8  _pad2[3];
    struct Coords16 initialCoords;
    struct Coords16 currentCoords;
    struct Coords16 previousCoords;
};

// OBJECT_EVENTS_COUNT comes from constants/global.h; declare the array here.
// The literal 16 must match OBJECT_EVENTS_COUNT in constants/global.h.
extern struct ObjectEvent gObjectEvents[16];

// WarpData and minimal SaveBlock1 for GhostMapCheck
struct WarpData { s8 mapGroup; s8 mapNum; s8 warpId; s16 x; s16 y; };

struct SaveBlock1 {
    u32 _pad0;
    struct WarpData location;
    // Additional fields omitted — multiplayer.c only reads location.
};

extern struct SaveBlock1 *gSaveBlock1Ptr;

// Direction enum — values must match include/constants/global.h exactly.
enum Direction {
    DIR_NONE   = 0,
    DIR_SOUTH  = 1,
    DIR_NORTH  = 2,
    DIR_WEST   = 3,
    DIR_EAST   = 4,
    CARDINAL_DIRECTION_COUNT,
};

#endif // GUARD_MOCK_GLOBAL_H
