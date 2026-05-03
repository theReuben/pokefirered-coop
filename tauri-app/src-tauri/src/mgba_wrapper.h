#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Opaque context bundling an mGBA GBA core + pixel buffer.
typedef struct MgbaCtx MgbaCtx;

// Create a GBA core. Returns NULL on failure.
MgbaCtx* mgba_create(void);

// Destroy the core and free all resources (flushes save data).
void mgba_destroy(MgbaCtx* ctx);

// Load a ROM file. Must be called before mgba_reset.
bool mgba_load_rom(MgbaCtx* ctx, const char* path);

// Attach a save file (creates it if absent).
bool mgba_load_save(MgbaCtx* ctx, const char* path);

// Apply config defaults and reset the CPU/hardware state.
void mgba_reset(MgbaCtx* ctx);

// Run one video frame.
void mgba_run_frame(MgbaCtx* ctx);

// Set the 16-bit GBA key register (active-low bitmask matching KEYINPUT).
void mgba_set_keys(MgbaCtx* ctx, uint32_t keys);

// Returns a pointer to the 240x160 pixel buffer (XBGR8 format:
// R = pixel & 0xFF, G = (pixel >> 8) & 0xFF, B = (pixel >> 16) & 0xFF).
// Valid until the next mgba_run_frame or mgba_destroy call.
const uint32_t* mgba_get_pixels(MgbaCtx* ctx);

// Raw GBA memory read/write (segment = -1 for auto).
uint32_t mgba_read32(MgbaCtx* ctx, uint32_t addr);
void     mgba_write32(MgbaCtx* ctx, uint32_t addr, uint32_t val);
uint8_t  mgba_read8(MgbaCtx* ctx, uint32_t addr);
void     mgba_write8(MgbaCtx* ctx, uint32_t addr, uint8_t val);
