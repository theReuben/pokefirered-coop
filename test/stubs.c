// No-op stubs for GBA engine functions called by multiplayer.c.
// These exist so the native unit tests link cleanly without the full ROM build.
#include "global.h"
#include "event_object_movement.h"

// Global arrays / pointers referenced by multiplayer.c
struct ObjectEvent gObjectEvents[16];
struct SaveBlock1 *gSaveBlock1Ptr;

// Set this to a valid slot (0-15) before a test that exercises spawn; 16 = no slot.
u8 gTestNextSpawnSlot = 16;

// Object event stubs
u8 SpawnSpecialObjectEventParameterized(u16 graphicsId, u8 movementBehavior,
                                         u8 localId, s16 x, s16 y, u8 elevation)
{
    (void)graphicsId; (void)movementBehavior; (void)localId; (void)elevation;
    u8 slot = gTestNextSpawnSlot;
    if (slot < 16)
    {
        gObjectEvents[slot].active = 1;
        gObjectEvents[slot].currentCoords.x = x;
        gObjectEvents[slot].currentCoords.y = y;
    }
    return slot;
}

void RemoveObjectEvent(struct ObjectEvent *objectEvent)
{
    if (objectEvent)
        objectEvent->active = 0;
}

void MoveObjectEventToMapCoords(struct ObjectEvent *objectEvent, s16 x, s16 y)
{
    if (objectEvent)
    {
        objectEvent->currentCoords.x = x;
        objectEvent->currentCoords.y = y;
    }
}

void SetObjectEventDirection(struct ObjectEvent *objectEvent, enum Direction direction)
{
    (void)objectEvent; (void)direction;
}

u8 ObjectEventClearHeldMovementIfFinished(struct ObjectEvent *objectEvent)
{
    if (objectEvent && objectEvent->heldMovementFinished)
    {
        objectEvent->heldMovementActive   = 0;
        objectEvent->heldMovementFinished = 0;
        return 1;
    }
    return 0;
}

bool8 ObjectEventSetHeldMovement(struct ObjectEvent *objectEvent, u8 movementAction)
{
    (void)movementAction;
    if (objectEvent)
        objectEvent->heldMovementActive = 1;
    return TRUE;
}
