#ifndef GUARD_OVERWORLD_H
#define GUARD_OVERWORLD_H

// Minimal mock for native unit tests.

typedef u8 mapsec_u8_t;
struct UCoords32 { s32 x; s32 y; };

extern const struct UCoords32 gDirectionToVectors[];

void CB2_ReturnToFieldContinueScript(void);

#endif // GUARD_OVERWORLD_H
