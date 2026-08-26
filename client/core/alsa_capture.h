#ifndef ALSA_CAPTURE_H
#define ALSA_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ALSA audio capture context (opaque)
 */
typedef struct alsa_capture_ctx alsa_capture_t;

/**
 * @brief Initialize ALSA audio capture
 * @param device     ALSA device name, e.g. "default", "hw:0,0", "plughw:0,0"
 * @param sample_rate Sample rate (Vosk requires 16000)
 * @param channels   Number of channels (1=mono)
 * @param period_size Period size (samples read per frame, 1024 recommended)
 * @return Capture context, NULL on failure
 *
 * Key points:
 *   ALSA (Advanced Linux Sound Architecture) is the standard Linux audio framework:
 *   - Compared with arecord process approach:
 *     arecord: ALSA kernel buffer -> arecord process -> pipe -> QProcess -> app
 *     ALSA API: ALSA kernel buffer -> snd_pcm_readi() -> app (no intermediate steps)
 *   - Key concepts:
 *     period: number of samples transferred per hardware interrupt, determines latency
 *     buffer: ring buffer size, usually N times the period
 *     hw_params: hardware parameters (sample rate/format/channels), set during negotiation
 *   - MMAP mode:
 *     Like V4L2 MMAP, ALSA also supports mmap to read audio buffers
 *     But audio data is small (16kHz*2B=32KB/s), readi mode is sufficient
 *     MMAP is mainly used for low-latency professional audio scenarios
 */
alsa_capture_t *alsa_capture_open(const char *device,
                                  unsigned int sample_rate,
                                  unsigned int channels,
                                  unsigned int period_size);

/**
 * @brief Read one frame of PCM audio data
 * @param ctx    Capture context
 * @param buffer Output buffer
 * @param frames Number of frames to read (1 frame = channels samples)
 * @return Actual frames read, 0=no data, -1=error
 *
 * @note Returned PCM data format: S16_LE (16-bit signed little-endian), can be sent to Vosk directly
 */
int alsa_capture_read(alsa_capture_t *ctx, void *buffer, unsigned int frames);

/**
 * @brief Close ALSA capture and release resources
 */
void alsa_capture_close(alsa_capture_t **ctx);

/**
 * @brief Stop capture and clear buffer (without closing device)
 *
 * Calls snd_pcm_drop to stop DMA transfer and flush residual data in the ring buffer,
 * solving the "ALSA buffer overrun after stopping reads" problem.
 * Used: when speech recognition pauses capture, to prevent XRUN caused by full ALSA buffer.
 * Must call alsa_capture_prepare() afterwards to restart capture.
 */
void alsa_capture_drop(alsa_capture_t *ctx);

/**
 * @brief Re-prepare capture (recover after drop)
 *
 * Calls snd_pcm_prepare to reinitialize DMA transfer, starting capture from scratch.
 * Used: after alsa_capture_drop(), preparation before resuming capture.
 * @return 0=success, -1=failure
 */
int alsa_capture_prepare(alsa_capture_t *ctx);

/**
 * @brief Get capture parameters
 */
unsigned int alsa_capture_get_rate(alsa_capture_t *ctx);
unsigned int alsa_capture_get_channels(alsa_capture_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ALSA_CAPTURE_H */
