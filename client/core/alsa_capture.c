/**
 * @file alsa_capture.c
 * @brief ALSA native audio capture implementation (libasound snd_pcm_* API)
 */

#include "alsa_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <alsa/asoundlib.h>

/* ALSA capture context (parameters reflect hardware negotiation) */
struct alsa_capture_ctx {
    snd_pcm_t *pcm;            /* ALSA PCM handle */
    unsigned int sample_rate;  /* negotiated sample rate */
    unsigned int channels;     /* negotiated channel count */
    unsigned int period_size;  /* negotiated period size (frames) */
    unsigned int frame_bytes;  /* bytes per frame (S16_LE: channels*2) */
};

alsa_capture_t *alsa_capture_open(const char *device,
                                  unsigned int sample_rate,
                                  unsigned int channels,
                                  unsigned int period_size)
{
    if (!device) return NULL;

    alsa_capture_t *ctx = (alsa_capture_t *)calloc(1, sizeof(alsa_capture_t));
    if (!ctx) return NULL;

    ctx->sample_rate  = sample_rate;
    ctx->channels     = channels;
    ctx->period_size  = period_size;
    ctx->frame_bytes  = channels * 2;  /* S16_LE: 2 bytes/sample */

    int err;

    err = snd_pcm_open(&ctx->pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "[ALSA] snd_pcm_open(%s) failed: %s\n", device, snd_strerror(err));
        free(ctx);
        return NULL;
    }

    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(ctx->pcm, hw_params);

    snd_pcm_hw_params_set_access(ctx->pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);

    /* S16_LE: required by Vosk */
    snd_pcm_hw_params_set_format(ctx->pcm, hw_params, SND_PCM_FORMAT_S16_LE);

    /* set_rate_near: picks nearest supported rate and writes it back */
    unsigned int req_rate = sample_rate;
    snd_pcm_hw_params_set_rate_near(ctx->pcm, hw_params, &req_rate, 0);
    ctx->sample_rate = req_rate;

    snd_pcm_hw_params_set_channels(ctx->pcm, hw_params, channels);

    snd_pcm_uframes_t req_period = period_size;
    snd_pcm_hw_params_set_period_size_near(ctx->pcm, hw_params, &req_period, 0);
    ctx->period_size = (unsigned int)req_period;

    snd_pcm_uframes_t buffer_size = req_period * 4;
    snd_pcm_hw_params_set_buffer_size_near(ctx->pcm, hw_params, &buffer_size);

    err = snd_pcm_hw_params(ctx->pcm, hw_params);
    if (err < 0) {
        fprintf(stderr, "[ALSA] snd_pcm_hw_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        free(ctx);
        return NULL;
    }

    err = snd_pcm_prepare(ctx->pcm);
    if (err < 0) {
        fprintf(stderr, "[ALSA] snd_pcm_prepare failed: %s\n", snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        free(ctx);
        return NULL;
    }

    fprintf(stdout, "[ALSA] Audio capture initialized successfully: %uHz %uch S16_LE period=%u\n",
            ctx->sample_rate, ctx->channels, ctx->period_size);

    /* warn if negotiated rate differs from requested; upper layer must use ctx->sample_rate */
    if (req_rate != sample_rate) {
        fprintf(stdout, "[ALSA] Note: requested sample rate %u, actually negotiated to %u\n",
                sample_rate, req_rate);
    }

    return ctx;
}

/**
 * @brief Read interleaved PCM frames.
 * @return >0=frames read, 0=XRUN recovered (no data this round), -1=error
 */
int alsa_capture_read(alsa_capture_t *ctx, void *buffer, unsigned int frames)
{
    if (!ctx || !buffer) return -1;

    if (!ctx->pcm) return -1;

    snd_pcm_sframes_t read_frames;

    read_frames = snd_pcm_readi(ctx->pcm, buffer, frames);

    if (read_frames < 0) {
        /* XRUN: snd_pcm_recover does drop+prepare internally; <0 = unrecoverable */
        read_frames = snd_pcm_recover(ctx->pcm, (int)read_frames, 0);
        if (read_frames < 0) {
            fprintf(stderr, "[ALSA] snd_pcm_readi unrecoverable error: %s\n",
                    snd_strerror((int)read_frames));
            return -1;
        }
        /* recovered but data lost this read */
        return 0;
    }

    return (int)read_frames;
}

void alsa_capture_close(alsa_capture_t **pctx)
{
    if (!pctx || !*pctx) return;
    alsa_capture_t *ctx = *pctx;

    if (ctx->pcm) {
        snd_pcm_drop(ctx->pcm);
        snd_pcm_close(ctx->pcm);
    }

    free(ctx);
    *pctx = NULL;
    fprintf(stdout, "[ALSA] Audio device closed\n");
}

/**
 * @brief Stop capture and discard buffered data; prepare() required before reading again.
 */
void alsa_capture_drop(alsa_capture_t *ctx)
{
    if (!ctx) return;

    if (ctx->pcm) {
        snd_pcm_drop(ctx->pcm);
    }
}

/**
 * @brief Re-prepare capture after drop(); 0=success, -1=failure.
 */
int alsa_capture_prepare(alsa_capture_t *ctx)
{
    if (!ctx) return -1;

    if (!ctx->pcm) return -1;

    int err = snd_pcm_prepare(ctx->pcm);
    if (err < 0) {
        fprintf(stderr, "[ALSA] snd_pcm_prepare failed: %s\n", snd_strerror(err));
        return -1;
    }
    return 0;
}

/**
 * @brief Get negotiated sample rate (use this, not the requested value).
 */
unsigned int alsa_capture_get_rate(alsa_capture_t *ctx)
{
    return ctx ? ctx->sample_rate : 0;
}

/**
 * @brief Get negotiated channel count.
 */
unsigned int alsa_capture_get_channels(alsa_capture_t *ctx)
{
    return ctx ? ctx->channels : 0;
}
