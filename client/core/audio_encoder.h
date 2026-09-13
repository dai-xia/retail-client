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

/** @brief Audio encoder configuration */
typedef struct {
    int      sample_rate;
    int      channels;
    int      bit_rate;          /* bps, 0=default 64000 */
} audio_encoder_config_t;

/** @brief AAC encoder + SwrContext resampler (S16LE interleaved PCM -> AAC). */
typedef struct {
    AVCodecContext  *codec_ctx;
    SwrContext      *swr_ctx;      /* S16 interleaved -> FLTP planar */
    AVFrame         *frame;
    AVPacket        *pkt;
    int              sample_rate;
    int              channels;
    bool             opened;
} audio_encoder_t;

/** @brief Initialize audio encoder (AAC + SwrContext); NULL on failure. */
audio_encoder_t *audio_encoder_open(const audio_encoder_config_t *config);

/** @brief Encode PCM -> AAC. pcm_data is S16LE interleaved; pts in codec time_base (1/sample_rate).
 *  @return 1=packet, 0=EAGAIN, -1=error */
int audio_encoder_encode_pcm(audio_encoder_t *ctx,
                              const uint8_t *pcm_data, int data_size,
                              int64_t pts,
                              AVPacket **out_pkt);

/** @brief Flush encoder; returns number of packets produced. */
int audio_encoder_flush(audio_encoder_t *ctx, AVPacket **out_pkts, int max_pkts);

/** @brief Close encoder and release resources. */
void audio_encoder_close(audio_encoder_t **ctx);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_ENCODER_H */
