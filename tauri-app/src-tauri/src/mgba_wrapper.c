#include "mgba_wrapper.h"
#include <mgba/gba/core.h>
#include <mgba/core/core.h>
#include <stdlib.h>
#include <string.h>

#define GBA_W 240
#define GBA_H 160

struct MgbaCtx {
    struct mCore* core;
    uint32_t      pixels[GBA_W * GBA_H];
};

MgbaCtx* mgba_create(void) {
    MgbaCtx* ctx = calloc(1, sizeof(MgbaCtx));
    if (!ctx) return NULL;

    ctx->core = GBACoreCreate();
    if (!ctx->core) { free(ctx); return NULL; }

    if (!ctx->core->init(ctx->core)) {
        ctx->core->deinit(ctx->core);
        free(ctx);
        return NULL;
    }

    ctx->core->setVideoBuffer(ctx->core, ctx->pixels, GBA_W);
    return ctx;
}

void mgba_destroy(MgbaCtx* ctx) {
    if (ctx) {
        ctx->core->deinit(ctx->core);
        free(ctx);
    }
}

bool mgba_load_rom(MgbaCtx* ctx, const char* path) {
    return mCoreLoadFile(ctx->core, path);
}

bool mgba_load_save(MgbaCtx* ctx, const char* path) {
    return mCoreLoadSaveFile(ctx->core, path, false);
}

void mgba_reset(MgbaCtx* ctx) {
    mCoreInitConfig(ctx->core, NULL);
    ctx->core->reset(ctx->core);
}

void mgba_run_frame(MgbaCtx* ctx) {
    ctx->core->runFrame(ctx->core);
}

void mgba_set_keys(MgbaCtx* ctx, uint32_t keys) {
    ctx->core->setKeys(ctx->core, keys);
}

const uint32_t* mgba_get_pixels(MgbaCtx* ctx) {
    return ctx->pixels;
}

uint32_t mgba_read32(MgbaCtx* ctx, uint32_t addr) {
    return ctx->core->rawRead32(ctx->core, addr, -1);
}

void mgba_write32(MgbaCtx* ctx, uint32_t addr, uint32_t val) {
    ctx->core->rawWrite32(ctx->core, addr, -1, val);
}

uint8_t mgba_read8(MgbaCtx* ctx, uint32_t addr) {
    return (uint8_t)ctx->core->rawRead8(ctx->core, addr, -1);
}

void mgba_write8(MgbaCtx* ctx, uint32_t addr, uint8_t val) {
    ctx->core->rawWrite8(ctx->core, addr, -1, val);
}
