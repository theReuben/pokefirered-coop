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

#endif // GUARD_MOCK_GLOBAL_H
