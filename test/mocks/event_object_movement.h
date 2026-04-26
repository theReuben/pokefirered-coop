#ifndef GUARD_EVENT_OBJECT_MOVEMENT_H
#define GUARD_EVENT_OBJECT_MOVEMENT_H

// Mock for native unit tests — declares only the functions that multiplayer.c calls.
// The real header cannot be used here because it pulls in GBA-specific types
// (SubspriteTable, etc.) that have no native equivalents.

#include "constants/event_object_movement.h"

u8   SpawnSpecialObjectEventParameterized(u16 graphicsId, u8 movementBehavior,
                                           u8 localId, s16 x, s16 y, u8 elevation);
void RemoveObjectEvent(struct ObjectEvent *objectEvent);
void MoveObjectEventToMapCoords(struct ObjectEvent *objectEvent, s16 x, s16 y);
void SetObjectEventDirection(struct ObjectEvent *objectEvent, enum Direction direction);
u8   ObjectEventClearHeldMovementIfFinished(struct ObjectEvent *objectEvent);
bool8 ObjectEventSetHeldMovement(struct ObjectEvent *objectEvent, u8 movementAction);

#endif // GUARD_EVENT_OBJECT_MOVEMENT_H
