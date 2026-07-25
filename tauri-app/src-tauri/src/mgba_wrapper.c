#include "mgba_wrapper.h"
#include <mgba/gba/core.h>
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <stdlib.h>
#include <string.h>

#define GBA_W 240
#define GBA_H 160

// Samples the GBA core accumulates before we drain it. One frame at the GBA's
// native rate is ~550 samples, so this is comfortable headroom.
#define CORE_AUDIO_SAMPLES 2048

// Capacity of our resampled output buffer, in stereo frames. ~170 ms at 48 kHz;
// the Rust side drains it every frame, so it only has to absorb jitter.
#define OUT_AUDIO_FRAMES 8192

#define DEFAULT_OUT_RATE 48000

struct MgbaCtx {
    struct mCore*            core;
    uint32_t                 pixels[GBA_W * GBA_H];
    // Resampled host-rate audio: the core writes at its own rate (clock
    // frequency / sample interval), the resampler converts into `out`.
    struct mAudioBuffer      out;
    struct mAudioResampler   resampler;
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

    // The GBA core copies core->opts.volume into gba->audio.masterVolume (and
    // zeroes it when opts.mute is set) every time the config is loaded. Those
    // options only ever get values from the config map, so without these
    // defaults the core runs at volume 0 — i.e. silent. The config is applied
    // in mgba_reset, after the ROM is loaded, so per-game overrides resolve.
    mCoreInitConfig(ctx->core, NULL);
    mCoreConfigSetDefaultIntValue(&ctx->core->config, "volume", 0x100);
    mCoreConfigSetDefaultIntValue(&ctx->core->config, "mute", 0);

    ctx->core->setAudioBufferSize(ctx->core, CORE_AUDIO_SAMPLES);
    mAudioBufferInit(&ctx->out, OUT_AUDIO_FRAMES, 2);
    mAudioResamplerInit(&ctx->resampler, mINTERPOLATOR_SINC);
    mAudioResamplerSetDestination(&ctx->resampler, &ctx->out, DEFAULT_OUT_RATE);

    return ctx;
}

void mgba_destroy(MgbaCtx* ctx) {
    if (ctx) {
        mAudioResamplerDeinit(&ctx->resampler);
        mAudioBufferDeinit(&ctx->out);
        // core->deinit frees the core itself but only frees opts, not the
        // config we initialised in mgba_create — so drop that first.
        mCoreConfigDeinit(&ctx->core->config);
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
    // Applies the defaults set in mgba_create (notably volume) plus any
    // per-game overrides now that the ROM is loaded.
    mCoreLoadConfig(ctx->core);

    ctx->core->reset(ctx->core);
    mAudioBufferClear(&ctx->out);
}

void mgba_run_frame(MgbaCtx* ctx) {
    ctx->core->runFrame(ctx->core);

    // Convert this frame's core-rate samples into the host-rate output buffer.
    // The source rate is re-read each frame because the GBA changes its sample
    // interval at runtime (the sound bias register).
    mAudioResamplerSetSource(&ctx->resampler,
                             ctx->core->getAudioBuffer(ctx->core),
                             ctx->core->audioSampleRate(ctx->core),
                             true);
    mAudioResamplerProcess(&ctx->resampler);
}

void mgba_set_audio_rate(MgbaCtx* ctx, unsigned rate) {
    if (rate == 0) return;
    mAudioResamplerSetDestination(&ctx->resampler, &ctx->out, rate);
}

size_t mgba_read_audio(MgbaCtx* ctx, int16_t* out, size_t frames) {
    return mAudioBufferRead(&ctx->out, out, frames);
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
