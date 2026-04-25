# Object Event System — Research Notes (Phase 1)

## ObjectEvent struct (`include/global.fieldmap.h:249`, size = 0x24)

| Offset | Field | Notes |
|--------|-------|-------|
| 0x00 | `active:1` | 1 = slot in use |
| 0x00 | `inanimate:1` | 1 = no walk animation |
| 0x00 | `invisible:1` | 1 = hidden |
| 0x00 | `isPlayer:1` | 1 = this is the player |
| 0x04 | `graphicsId` (u16) | Sprite/animation set to use |
| 0x06 | `movementType` (u8) | One of `MOVEMENT_TYPE_*` |
| 0x08 | `localId` (u8) | Map-scoped NPC ID |
| 0x09 | `mapNum` (u8) | Map this NPC belongs to |
| 0x0A | `mapGroup` (u8) | Map group |
| 0x0C | `initialCoords` (Coords16) | Spawn coords |
| 0x10 | `currentCoords` (Coords16) | Current tile |
| 0x14 | `previousCoords` (Coords16) | Previous tile |
| 0x18 | `facingDirection:4` | DIR_NORTH/SOUTH/EAST/WEST |
| 0x23 | `spriteId` (u8) | Index into `gSprites[]` |

Global array: `gObjectEvents[OBJECT_EVENTS_COUNT]` where `OBJECT_EVENTS_COUNT = 16`

## Key API functions (`include/event_object_movement.h`)

### Spawning
```c
// Spawn by parameters — returns objectEventId (>= OBJECT_EVENTS_COUNT on failure)
u8 SpawnSpecialObjectEventParameterized(
    u16 graphicsId,       // e.g. OBJ_EVENT_GFX_GREEN_NORMAL
    u8  movementBehavior, // MOVEMENT_TYPE_NONE for ghost
    u8  localId,          // unique per-map ID; use 0xFE for ghost
    s16 x, s16 y,         // MAP tile coordinates (function subtracts MAP_OFFSET internally)
    u8  elevation);       // usually 3
```

### Removal
```c
void RemoveObjectEvent(struct ObjectEvent *objectEvent);
void RemoveObjectEventByLocalIdAndMap(u8 localId, u8 mapNum, u8 mapGroup);
```

### Position and facing
```c
void MoveObjectEventToMapCoords(struct ObjectEvent *objectEvent, s16 x, s16 y);
void SetObjectEventDirection(struct ObjectEvent *objectEvent, enum Direction direction);
void ObjectEventTurn(struct ObjectEvent *objectEvent, enum Direction direction); // with anim
```

### Lookup
```c
u8 GetObjectEventIdByLocalIdAndMap(u8 localId, u8 mapNum, u8 mapGroup);
// returns OBJECT_EVENTS_COUNT if not found
```

## Ghost NPC Design

- **Graphics**: `OBJ_EVENT_GFX_GREEN_NORMAL` (251) — Green/Leaf walking sprite (FRLG female protagonist). This uses the full walk-cycle animation and is a distinct color from Red.
- **Movement type**: `MOVEMENT_TYPE_NONE` (0) — ghost does not self-move; position driven by `Multiplayer_UpdateGhostPosition()`.
- **Local ID**: `0xFE` (254) — above any map-defined NPC (maps rarely use IDs > 10).
- **Collision**: Object events have tile collision by default. Ghost NPC will be naturally collidable.
- **Not interactable**: Script pointer in the template is set to a no-op or NULL; `trainerType = TRAINER_TYPE_NONE`.

## Hook Point for Multiplayer_Update

`CB2_Overworld()` → `OverworldBasic()` is called every frame (`src/overworld.c:1827`).
Call `Multiplayer_Update()` at the top of `CB2_Overworld`, before `OverworldBasic()`, to process incoming packets and update the ghost position each frame.

## Slot Pressure

With `OBJECT_EVENTS_COUNT = 16`, maps can have at most 15 NPCs + the ghost. FRLG maps typically have 6–10 NPCs so there is headroom.
