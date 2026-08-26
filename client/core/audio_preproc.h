#ifndef AUDIO_PREPROC_H
#define AUDIO_PREPROC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio preprocessing configuration */
typedef struct {
    unsigned int sample_rate;       /* Sample rate, default 16000 */
    unsigned int channels;          /* Channel count, default 1 */
    unsigned int frame_size;        /* Samples per frame, default 480 (30ms@16kHz) */
    bool         enable_aec;        /* Enable acoustic echo cancellation */
    bool         enable_ns;         /* Enable noise suppression */
    bool         enable_agc;        /* Enable automatic gain control */
    bool         enable_vad;        /* Enable voice activity detection */
    float        vad_threshold;     /* VAD threshold 0~1, default 0.5 */
    /* RKNN VAD model path (optional, when NULL uses SpeexDSP energy VAD) */
    const char  *vad_rknn_path;
} audio_preproc_config_t;

/* VAD detection result */
typedef enum {
    AUDIO_VAD_SILENCE = 0,   /* Silence */
    AUDIO_VAD_SPEECH  = 1,   /* Speech */
} audio_vad_result_t;

/* Audio preprocessing context (opaque) */
typedef struct audio_preproc_ctx audio_preproc_t;

/* Create/destroy */
audio_preproc_t* audio_preproc_create(const audio_preproc_config_t *config);
void             audio_preproc_destroy(audio_preproc_t **ctx);

/* Process one frame of audio
 * Input: mic_frame (near-end microphone, frame_size int16_t samples)
 *        ref_frame (far-end reference/speaker playback, only needed for AEC, can be NULL)
 * Output: out_frame (processed audio, frame_size int16_t samples, can be same as mic_frame for in-place processing)
 * Return: VAD result */
audio_vad_result_t audio_preproc_process(audio_preproc_t *ctx,
                                          const int16_t *mic_frame,
                                          const int16_t *ref_frame,
                                          int16_t *out_frame);

/* Get speech probability of the last frame (0.0~1.0, only valid when VAD is enabled) */
float audio_preproc_get_speech_prob(const audio_preproc_t *ctx);

#ifdef __cplusplus
}
#endif
#endif
