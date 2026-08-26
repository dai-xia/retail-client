/**
 * @file audio_encoder.c
 * @brief FFmpeg AAC audio encoder wrapper (S16->FLTP->AAC)
 *
 * Symmetrical with video_encoder.c, wraps AAC encoder + SwrContext resampler:
 *   Input: S16LE interleaved PCM (ALSA capture format)
 *   Output: AAC AVPacket
 *
 * Encoding flow:
 *   1. audio_encoder_open()  - find AAC encoder, configure params, initialize SwrContext
 *   2. audio_encoder_encode_pcm() - swr convert -> fill frame -> send_frame -> receive_packet
 *   3. audio_encoder_flush() - send NULL frame -> drain remaining packets
 *   4. audio_encoder_close() - release all resources
 */

#include "audio_encoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Create and initialize audio encoder (AAC + SwrContext)
 *
 * Steps:
 *   1. Find AAC encoder
 *   2. Allocate AVCodecContext, configure sample rate/channels/bit rate/FLTP format
 *   3. avcodec_open2 opens the encoder
 *   4. Initialize SwrContext: S16 interleaved -> FLTP planar
 *   5. Allocate AVFrame + AVPacket
 */
audio_encoder_t *audio_encoder_open(const audio_encoder_config_t *config)
{
    if (!config) return NULL;

    audio_encoder_t *ctx = (audio_encoder_t *)calloc(1, sizeof(audio_encoder_t));
    if (!ctx) return NULL;

    ctx->sample_rate = config->sample_rate;
    ctx->channels    = config->channels;
    ctx->opened      = false;

    /* 1. Find AAC encoder */
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        fprintf(stderr, "[AudioEncoder] AAC encoder not available\n");
        free(ctx);
        return NULL;
    }

    /* 2. Allocate encoder context */
    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        fprintf(stderr, "[AudioEncoder] avcodec_alloc_context3 failed\n");
        free(ctx);
        return NULL;
    }

    /* 3. Configure AAC encoding parameters */
    ctx->codec_ctx->bit_rate       = config->bit_rate > 0 ? config->bit_rate : 64000;
    ctx->codec_ctx->sample_rate    = config->sample_rate;
    ctx->codec_ctx->channels       = config->channels;
    ctx->codec_ctx->channel_layout = (config->channels == 1) ?
        AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    /* AAC requires FLTP (float32 planar) format */
    ctx->codec_ctx->sample_fmt     = AV_SAMPLE_FMT_FLTP;
    ctx->codec_ctx->time_base      = (AVRational){1, config->sample_rate};

    /* Some AAC encoders only support specific sample rates, auto-adapt */
    if (codec->supported_samplerates) {
        int found = 0;
        for (int i = 0; codec->supported_samplerates[i]; i++) {
            if (codec->supported_samplerates[i] == config->sample_rate) {
                found = 1;
                break;
            }
        }
        if (!found) {
            ctx->codec_ctx->sample_rate = codec->supported_samplerates[0];
            fprintf(stdout, "[AudioEncoder] Sample rate %d not supported, using %d\n",
                    config->sample_rate, codec->supported_samplerates[0]);
        }
    }

    /* 4. Open encoder */
    int ret = avcodec_open2(ctx->codec_ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "[AudioEncoder] avcodec_open2 failed\n");
        avcodec_free_context(&ctx->codec_ctx);
        free(ctx);
        return NULL;
    }

    /* 5. Initialize resampler: S16 interleaved -> FLTP planar */
    ctx->swr_ctx = swr_alloc_set_opts(NULL,
        ctx->codec_ctx->channel_layout,
        ctx->codec_ctx->sample_fmt,
        ctx->codec_ctx->sample_rate,
        (config->channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO,
        AV_SAMPLE_FMT_S16,
        config->sample_rate,
        0, NULL);

    if (ctx->swr_ctx) {
        swr_init(ctx->swr_ctx);
    } else {
        fprintf(stderr, "[AudioEncoder] swr_alloc_set_opts failed\n");
    }

    /* 6. Allocate frame and packet */
    ctx->frame = av_frame_alloc();
    ctx->pkt   = av_packet_alloc();
    if (!ctx->frame || !ctx->pkt) {
        fprintf(stderr, "[AudioEncoder] av_frame_alloc/packet_alloc failed\n");
        audio_encoder_close(&ctx);
        return NULL;
    }

    ctx->opened = true;
    fprintf(stdout, "[AudioEncoder] Encoder initialized (AAC, %dHz, %dch, %dkbps)\n",
            config->sample_rate, config->channels,
            ctx->codec_ctx->bit_rate / 1000);
    return ctx;
}

/**
 * @brief Encode a chunk of PCM data -> AAC
 *
 * Data flow: S16LE PCM -> swr_convert(FLTP) -> AVFrame -> avcodec_send_frame -> avcodec_receive_packet
 *
 * @param pts PTS of input frame, based on codec_ctx->time_base (1/sample_rate)
 */
int audio_encoder_encode_pcm(audio_encoder_t *ctx,
                              const uint8_t *pcm_data, int data_size,
                              int64_t pts,
                              AVPacket **out_pkt)
{
    if (!ctx || !ctx->opened || !pcm_data) return -1;

    int nb_samples = data_size / (ctx->channels * 2);  /* S16 = 2 bytes/sample */

    /* 1. S16 -> FLTP resample */
    uint8_t *dst_data = NULL;
    int dst_linesize = 0;
    int ret = av_samples_alloc(&dst_data, &dst_linesize,
                                ctx->channels, nb_samples,
                                ctx->codec_ctx->sample_fmt, 0);
    if (ret < 0) return -1;

    if (ctx->swr_ctx) {
        const uint8_t *src_data[1] = { pcm_data };
        nb_samples = swr_convert(ctx->swr_ctx, &dst_data, nb_samples,
                                  src_data, nb_samples);
    }

    /* 2. Fill AVFrame */
    av_frame_unref(ctx->frame);
    ctx->frame->nb_samples     = nb_samples;
    ctx->frame->format         = ctx->codec_ctx->sample_fmt;
    ctx->frame->channel_layout = ctx->codec_ctx->channel_layout;
    ctx->frame->channels       = ctx->codec_ctx->channels;
    ctx->frame->sample_rate    = ctx->codec_ctx->sample_rate;
    ctx->frame->pts            = pts;

    ret = av_frame_get_buffer(ctx->frame, 0);
    if (ret < 0) {
        av_freep(&dst_data);
        return -1;
    }

    /* Copy resampled FLTP data to frame */
    if (ctx->codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
        memcpy(ctx->frame->data[0], dst_data, nb_samples * sizeof(float));
    } else {
        memcpy(ctx->frame->data[0], dst_data, dst_linesize);
    }
    av_freep(&dst_data);

    /* 3. Send frame to encoder */
    ret = avcodec_send_frame(ctx->codec_ctx, ctx->frame);
    if (ret < 0) return -1;

    /* 4. Get encoded packet */
    ret = avcodec_receive_packet(ctx->codec_ctx, ctx->pkt);
    if (ret == 0) {
        *out_pkt = ctx->pkt;
        return 1;
    } else if (ret == AVERROR(EAGAIN)) {
        return 0;
    } else {
        return -1;
    }
}

/**
 * @brief Flush encoder: get remaining cached frames
 */
int audio_encoder_flush(audio_encoder_t *ctx, AVPacket **out_pkts, int max_pkts)
{
    if (!ctx || !ctx->opened) return 0;

    avcodec_send_frame(ctx->codec_ctx, NULL);

    int count = 0;
    while (count < max_pkts) {
        int ret = avcodec_receive_packet(ctx->codec_ctx, ctx->pkt);
        if (ret == 0) {
            out_pkts[count++] = ctx->pkt;
        } else {
            break;
        }
    }
    return count;
}

/**
 * @brief Close encoder, release all resources
 */
void audio_encoder_close(audio_encoder_t **pctx)
{
    if (!pctx || !*pctx) return;
    audio_encoder_t *ctx = *pctx;

    if (ctx->frame)   av_frame_free(&ctx->frame);
    if (ctx->pkt)     av_packet_free(&ctx->pkt);
    if (ctx->swr_ctx) swr_free(&ctx->swr_ctx);
    if (ctx->codec_ctx) avcodec_free_context(&ctx->codec_ctx);

    free(ctx);
    *pctx = NULL;
    fprintf(stdout, "[AudioEncoder] Encoder released\n");
}
