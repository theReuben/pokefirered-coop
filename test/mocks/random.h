#ifndef GUARD_RANDOM_H
#define GUARD_RANDOM_H

#include "global.h"

// Controllable stub: tests can set gTestRandom32Value before calling
// Multiplayer_GenerateSeed() to get a deterministic result.
extern u32 gTestRandom32Value;

static inline u32 Random32(void) { return gTestRandom32Value; }
static inline u16 Random(void)   { return (u16)(Random32() >> 16); }

#endif // GUARD_RANDOM_H
