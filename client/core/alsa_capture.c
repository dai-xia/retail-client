/**
 * @file alsa_capture.c
 * @brief ALSA native audio capture implementation (libasound snd_pcm_* API)
 *
 *
 * OPEN -> SETUP -> PREPARED(ready) -> RUNNING(running)
     ^          |                    |
     +-- drop <-+                XRUN(abnormal state)
 *
 *
 *
 * Complete ALSA capture state machine flow:
 *   snd_pcm_open -> snd_pcm_hw_params set hardware params -> snd_pcm_prepare prepare device -> snd_pcm_readi loop reading PCM
 *
 * [Comparison with arecord subprocess approach]
 * arecord approach path: kernel ALSA buffer -> arecord process memory -> pipe kernel buffer -> QProcess read -> business app
 *   Disadvantages: two data copies, process context switches, high CPU overhead, uncontrollable latency.
 * ALSA native API approach path: kernel ALSA buffer -> snd_pcm_readi() directly copies to application memory
 *   Advantages: reduces 2 data copies + 1 process switch, controllable latency, suitable for embedded speech recognition (Vosk) scenarios.
 *
 * [Core concepts (frequently asked in interviews)]
 * 1. period_size: number of sample frames delivered to user space per hardware DMA interrupt, directly determines audio latency.
 *    Example: period_size=1024, rate=16000 -> single segment latency = 1024 / 16000 = 64ms
 * 2. buffer_size: total frames in kernel ring buffer, generally configured as period_size * N, this code uses N=4.
 *    The kernel ring buffer counters application scheduling jitter, preventing XRUN overflow when data cannot be read in time.
 * 3. XRUN: audio buffer anomaly, divided into overrun (capture: app reads too slow, kernel buffer full, audio lost) / underrun (playback: writes too slow, buffer empty)
 *    After XRUN, PCM device state is abnormal, need to call snd_pcm_recover for state recovery, then re-prepare.
 * 4. snd_pcm_recover: ALSA library's failure recovery interface, handles XRUN and state anomalies, internally performs drop/prepare.
 *
 * [ALSA PCM state summary]
 * OPEN -> SETUP -> PREPARED: reached after calling prepare; calling readi auto-starts DMA entering RUNNING;
 * After XRUN, state becomes XRUN; drop returns to SETUP.
 */

#include "alsa_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* libasound header, all snd_xxx interfaces come from libasound.so */
#include <alsa/asoundlib.h>

/**
 * @brief ALSA capture context structure
 * Stores pcm handle and real parameters after hardware negotiation, upper layer business is unaware of internal details
 */
struct alsa_capture_ctx {
    snd_pcm_t *pcm;           /* ALSA PCM instance handle, represents an opened recording device */
    unsigned int sample_rate;  /* Actual effective sample rate after hardware negotiation (not necessarily equal to input) */
    unsigned int channels;     /* Actual channel count */
    unsigned int period_size;  /* Actual effective period size (frames), how many frames per DMA interrupt */
    unsigned int frame_bytes;  /* Bytes per frame: 1 frame = samples per channel, S16_LE each sample occupies 2 bytes */
};

/* ======================== Public API ======================== */

/**
 * @brief Open and initialize ALSA recording capture device
 * @param device Device name string: "default"/"plughw:0,0"/"hw:0,0"
 *               "default": ALSA pcm-pulse plugin, forwards to pulseaudio service;
 *               "hw:x,y": direct access to sound card hardware, exclusive device;
 *               "plughw:x,y": plug plugin, automatically does format/sample rate conversion.
 * @param sample_rate Requested sample rate, vosk speech recognition fixed 16000Hz
 * @param channels Requested channel count, speech recognition uses mono 1
 * @param period_size Requested period frames, 1024 @16k =64ms
 * @return Returns context pointer on success; NULL on failure
 */
alsa_capture_t *alsa_capture_open(const char *device,
                                  unsigned int sample_rate,
                                  unsigned int channels,
                                  unsigned int period_size)
{
    if (!device) return NULL;

    /* calloc allocates context, memory initialized to 0 */
    alsa_capture_t *ctx = (alsa_capture_t *)calloc(1, sizeof(alsa_capture_t));
    if (!ctx) return NULL;

    /* Save user-requested parameters first; subsequent hardware negotiation will overwrite with real hardware-supported values */
    ctx->sample_rate  = sample_rate;
    ctx->channels     = channels;
    ctx->period_size  = period_size;
    ctx->frame_bytes  = channels * 2;  /* S16_LE format: each sample occupies 2 bytes */

    int err;

    /* 1. Open PCM capture device
     * SND_PCM_STREAM_CAPTURE: recording stream; last flag 0 uses default config
     * Underneath opens /dev/snd/pcmCxDXc character device, libasound internally wraps ioctl, PCM = Pulse-Code Modulation
     */
    err = snd_pcm_open(&ctx->pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        /* snd_strerror: converts alsa error code to readable string */
        fprintf(stderr, "[ALSA] snd_pcm_open(%s) failed: %s\n", device, snd_strerror(err));
        free(ctx);
        return NULL;
    }

    /* 2. Allocate hardware parameter object hw_params
     * snd_pcm_hw_params_alloca: stack-allocated hw_params memory, no manual free needed
     * snd_pcm_hw_params_any: initializes hw_params, reads current pcm device hardware capabilities, clears old config
     * Note: the set_xxx calls below only fill the user-space hw_params struct, **not yet sent to kernel driver**
     */
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    //snd_pcm_hw_params_any() interacts with kernel sound card driver, reads all hardware capabilities supported by this sound card, fills them into the hw_params object:
    snd_pcm_hw_params_any(ctx->pcm, hw_params);

    /* 3. Set hardware parameters (memory fill only, not effective yet) */

    /* Access mode: SND_PCM_ACCESS_RW_INTERLEAVED interleaved mode
     * For multi-channel, samples are stored interleaved L R L R L R; no effect on mono.
     * Corresponds to snd_pcm_readi interface; there is also MMAP mode, directly maps kernel buffer, reducing copies.
     */
    snd_pcm_hw_params_set_access(ctx->pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);

    /* PCM sample format: S16_LE 16-bit signed little-endian, Vosk model requires this input format */
    snd_pcm_hw_params_set_format(ctx->pcm, hw_params, SND_PCM_FORMAT_S16_LE);

    /* Set sample rate: set_rate_near [nearest match]
     * req_rate is both input and output: passes desired sample rate; if hardware doesn't support, automatically selects nearest supported value and writes back to req_rate
     */
    unsigned int req_rate = sample_rate;
    snd_pcm_hw_params_set_rate_near(ctx->pcm, hw_params, &req_rate, 0);
    ctx->sample_rate = req_rate;

    /* Set channel count */
    snd_pcm_hw_params_set_channels(ctx->pcm, hw_params, channels);

    /* Set period_size: frames delivered per DMA interrupt, also matches nearest hardware-supported value */
    snd_pcm_uframes_t req_period = period_size;
    snd_pcm_hw_params_set_period_size_near(ctx->pcm, hw_params, &req_period, 0);
    ctx->period_size = (unsigned int)req_period;

    /* Set kernel ring buffer total size: 4x period, common embedded configuration
     * buffer_size = period * 4, reserves enough buffer to counter application scheduling jitter, reducing overrun probability
     */
    snd_pcm_uframes_t buffer_size = req_period * 4;
    snd_pcm_hw_params_set_buffer_size_near(ctx->pcm, hw_params, &buffer_size);

    /* 4. [Key] Send the whole hw_params to kernel driver, ioctl system call happens here, hardware parameters officially take effect */
    err = snd_pcm_hw_params(ctx->pcm, hw_params);
    if (err < 0) {
        fprintf(stderr, "[ALSA] snd_pcm_hw_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        free(ctx);
        return NULL;
    }

    /* 5. snd_pcm_prepare: switches PCM device to PREPARED ready state, prepares DMA, has not started capturing yet.
     * Subsequent first call to snd_pcm_readi will auto-start DMA and begin recording.
     */
    err = snd_pcm_prepare(ctx->pcm);
    if (err < 0) {
        fprintf(stderr, "[ALSA] snd_pcm_prepare failed: %s\n", snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        free(ctx);
        return NULL;
    }

    fprintf(stdout, "[ALSA] Audio capture initialized successfully: %uHz %uch S16_LE period=%u\n",
            ctx->sample_rate, ctx->channels, ctx->period_size);

    /* Print warning if hardware-negotiated sample rate differs from requested; upper layer must use ctx->sample_rate real value */
    if (req_rate != sample_rate) {
        fprintf(stdout, "[ALSA] Note: requested sample rate %u, actually negotiated to %u\n",
                sample_rate, req_rate);
    }

    return ctx;
}

/**
 * @brief Read one frame of interleaved PCM audio data
 * @param ctx alsa capture context
 * @param buffer User-space buffer to receive PCM data
 * @param frames How many frames expected to read (note: frame count, not bytes)
 * @return >0: actual frames read; 0: XRUN recovered successfully, no data this round; -1: unrecoverable error
 *
 * snd_pcm_readi: reads INTERLEAVED interleaved format PCM.
 * Returns negative for errors, typical error codes:
 *    -EPIPE: XRUN overrun, kernel buffer full, application read too slow, packets lost
 *    -EBADFD: pcm device handle illegal, state abnormal
 */
int alsa_capture_read(alsa_capture_t *ctx, void *buffer, unsigned int frames)
{
    if (!ctx || !buffer) return -1;

    if (!ctx->pcm) return -1;

    snd_pcm_sframes_t read_frames;

    /* Copy PCM data from ALSA kernel ring buffer to user buffer */
    read_frames = snd_pcm_readi(ctx->pcm, buffer, frames);

    if (read_frames < 0) {
        /* XRUN fault recovery: snd_pcm_recover internally handles XRUN errors, performs drop+prepare to recover device state
         * Third parameter 0: non-silent mode, prints error log; 1 is silent, no print.
         * Returns 0 on successful recovery; negative means completely unrecoverable.
         */
        read_frames = snd_pcm_recover(ctx->pcm, (int)read_frames, 0);
        if (read_frames < 0) {
            fprintf(stderr, "[ALSA] snd_pcm_readi unrecoverable error: %s\n",
                    snd_strerror((int)read_frames));
            return -1;
        }
        /* XRUN recovered successfully, but data lost this read, return 0, upper layer ignores this round */
        return 0;
    }

    return (int)read_frames;
}

/**
 * @brief Close ALSA capture device, release all resources, external passes address of context pointer
 * @param pctx Pointer to context pointer, set to NULL inside function
 */
void alsa_capture_close(alsa_capture_t **pctx)
{
    if (!pctx || !*pctx) return;
    alsa_capture_t *ctx = *pctx;

    if (ctx->pcm) {
        /* snd_pcm_drop: stops DMA transfer, discards residual unread data in kernel ring buffer, enters SETUP state */
        snd_pcm_drop(ctx->pcm);
        /* Close pcm handle, release kernel resources */
        snd_pcm_close(ctx->pcm);
    }

    free(ctx);
    *pctx = NULL;
    fprintf(stdout, "[ALSA] Audio device closed\n");
}

/**
 * @brief drop operation: stops DMA recording, discards residual audio in kernel buffer, PCM returns to SETUP state
 * Call scenario: stop listening for speech recognition, don't want to continue reading old buffered data.
 * Note: after drop, cannot call readi directly, need to call alsa_capture_prepare to return to PREPARED state.
 */
void alsa_capture_drop(alsa_capture_t *ctx)
{
    if (!ctx) return;

    if (ctx->pcm) {
        snd_pcm_drop(ctx->pcm);
        /* After drop, PCM is in SETUP state, DMA stopped, kernel buffer data all discarded */
    }
}

/**
 * @brief prepare operation, switches pcm device from SETUP to PREPARED ready state, preparing to start recording
 * Scenario: call after drop, before restarting capture; also needed after XRUN recovery.
 * After prepare completes, next call to snd_pcm_readi will auto-start DMA, beginning audio capture.
 * @return 0 success, -1 failure
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
    /* After prepare: PREPARED state; readi triggers, enters RUNNING, DMA starts moving audio */
    return 0;
}

/**
 * @brief Get real sample rate after hardware negotiation, upper layer must use this value, cannot directly use input parameter
 */
unsigned int alsa_capture_get_rate(alsa_capture_t *ctx)
{
    return ctx ? ctx->sample_rate : 0;
}

/**
 * @brief Get actual channel count
 */
unsigned int alsa_capture_get_channels(alsa_capture_t *ctx)
{
    return ctx ? ctx->channels : 0;
}
