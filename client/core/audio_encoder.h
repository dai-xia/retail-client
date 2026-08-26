#ifndef AUDIO_ENCODER_H
#define AUDIO_ENCODER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FFmpeg forward declarations */
typedef struct AVCodecContext AVCodecContext;
typedef struct AVFrame AVFrame;
typedef struct AVPacket AVPacket;
typedef struct SwrContext SwrContext;

/**
 * @brief Audio encoder configuration
 */
typedef struct {
    int      sample_rate;       /* Sample rate (e.g. 16000) */
    int      channels;          /* Channel count (1=mono, 2=stereo) */
    int      bit_rate;          /* Bit rate (bps), 0=default 64000 */
} audio_encoder_config_t;

/**
 * @brief Audio encoder context
 *
 * Wraps AAC encoder + SwrContext resampler, symmetrical with video_encoder:
 *   Input: S16LE interleaved PCM (ALSA capture format)
 *   Output: AAC AVPacket
 *
 * Encoding flow:
 *   1. audio_encoder_open() - find AAC encoder, initialize SwrContext
 *   2. audio_encoder_encode_pcm() - swr converts S16->FLTP -> send_frame -> receive_packet
 *   3. audio_encoder_flush() - flush encoder cache
 *   4. audio_encoder_close() - release resources
 */
typedef struct {
    AVCodecContext  *codec_ctx;    /* AAC encoder context */
    SwrContext      *swr_ctx;      /* Resampler: S16 interleaved -> FLTP planar */
    AVFrame         *frame;        /* Encoding frame buffer */
    AVPacket        *pkt;          /* Encoded output packet */
    int              sample_rate;  /* Sample rate */
    int              channels;     /* Channel count */
    bool             opened;       /* Whether encoder is opened */
} audio_encoder_t;

/**
 * @brief Initialize audio encoder (AAC + SwrContext)
 * @param config Encoder configuration
 * @return Encoder context, NULL on failure
 */
audio_encoder_t *audio_encoder_open(const audio_encoder_config_t *config);

/**
 * @brief Encode a chunk of PCM data -> AAC
 * @param pcm_data S16LE interleaved PCM data (ALSA capture format)
 * @param data_size Data byte count
 * @param pts Current frame PTS (based on codec_ctx->time_base, i.e. 1/sample_rate)
 * @param out_pkt [output] Encoded AAC packet
 * @return 1=encoded one frame, 0=need more input (EAGAIN), -1=error
 */
int audio_encoder_encode_pcm(audio_encoder_t *ctx,
                              const uint8_t *pcm_data, int data_size,
                              int64_t pts,
                              AVPacket **out_pkt);

/**
 * @brief Flush encoder, get remaining cached frames
 * @return Number of encoded packets
 */
int audio_encoder_flush(audio_encoder_t *ctx, AVPacket **out_pkts, int max_pkts);

/**
 * @brief Close encoder and release resources
 */
void audio_encoder_close(audio_encoder_t **ctx);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_ENCODER_H */
