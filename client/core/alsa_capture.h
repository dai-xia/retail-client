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
 */
alsa_capture_t *alsa_capture_open(const char *device,
                                  unsigned int sample_rate,
                                  unsigned int channels,
                                  unsigned int period_size);

/**
 * @brief Read one frame of PCM audio (S16_LE interleaved, usable by Vosk directly)
 * @param frames Frames to read (1 frame = channels samples)
 * @return Frames read, 0=no data, -1=error
 */
int alsa_capture_read(alsa_capture_t *ctx, void *buffer, unsigned int frames);

/**
 * @brief Close ALSA capture and release resources
 */
void alsa_capture_close(alsa_capture_t **ctx);

/**
 * @brief Stop capture and discard buffered data (no close); must call alsa_capture_prepare() before reading again.
 */
void alsa_capture_drop(alsa_capture_t *ctx);

/**
 * @brief Re-prepare capture after drop() (or XRUN); 0=success, -1=failure.
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
