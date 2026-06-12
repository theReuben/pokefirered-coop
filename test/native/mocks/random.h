#ifndef GUARD_RANDOM_H
#define GUARD_RANDOM_H

#include "global.h"
#include <stddef.h>

// Controllable stub: tests can set gTestRandom32Value before calling
// Multiplayer_GenerateSeed() to get a deterministic result.
extern u32 gTestRandom32Value;

static inline u32 Random32(void) { return gTestRandom32Value; }
static inline u16 Random(void)   { return (u16)(Random32() >> 16); }

// Structured RNG interface (subset of include/random.h).  multiplayer.c
// defines strong RandomUniform/RandomUniformExcept/RandomWeightedArray/
// RandomElementArray overrides (coop lockstep stream when in a coop battle,
// *Default otherwise); the Defaults are instrumented stubs in stubs.c so
// tests can assert which path was taken.
enum RandomTag
{
    RNG_NONE,
};

u32 RandomUniform(enum RandomTag tag, u32 lo, u32 hi);
u32 RandomUniformExcept(enum RandomTag tag, u32 lo, u32 hi, bool32 (*reject)(u32));
u32 RandomWeightedArray(enum RandomTag tag, u32 sum, u32 n, const u16 *weights);
const void *RandomElementArray(enum RandomTag tag, const void *array, size_t size, size_t count);

u32 RandomUniformDefault(enum RandomTag tag, u32 lo, u32 hi);
u32 RandomUniformExceptDefault(enum RandomTag tag, u32 lo, u32 hi, bool32 (*reject)(u32));
u32 RandomWeightedArrayDefault(enum RandomTag tag, u32 sum, u32 n, const u16 *weights);
const void *RandomElementArrayDefault(enum RandomTag tag, const void *array, size_t size, size_t count);

// Counts calls into the *Default stubs — lets tests assert that non-coop
// rolls bypass the lockstep stream.
extern u32 gTestRandomDefaultCalls;

#endif // GUARD_RANDOM_H
