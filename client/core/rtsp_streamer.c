/**
 * @file rtsp_streamer.c
 * @brief FFmpeg-based RTSP live streaming (h264_rkmpp VPU HW encode, NV12 zero-copy)
 */

#include "rtsp_streamer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>



/* base_time_ms>0 (reconnect) reuses the baseline to keep PTS monotonic (avoids a PTS cliff);
 * otherwise CLOCK_MONOTONIC. write_mutex serializes av_interleaved_write_frame (fmt_ctx not thread-safe). */
rtsp_streamer_t *rtsp_streamer_open(const rtsp_streamer_config_t *config)
{
    if (!config) return NULL;

    rtsp_streamer_t *ctx = (rtsp_streamer_t *)calloc(1, sizeof(rtsp_streamer_t));
    if (!ctx) {
        fprintf(stderr, "[RTSP] memory allocation failed\n");
        return NULL;
    }

    memcpy(&ctx->config, config, sizeof(rtsp_streamer_config_t));
    ctx->connected = false;
    pthread_mutex_init(&ctx->write_mutex, NULL);

    /* reuse baseline on reconnect, else CLOCK_MONOTONIC */
    if (config->base_time_ms > 0) {
        ctx->start_time_ms = config->base_time_ms;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ctx->start_time_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }

    /* 1. RTSP muxer */
    int ret = avformat_alloc_output_context2(&ctx->fmt_ctx, NULL, "rtsp", config->rtsp_url);
    if (ret < 0 || !ctx->fmt_ctx) {
        fprintf(stderr, "[RTSP] create RTSP muxer context failed\n");
        free(ctx);
        return NULL;
    }

    /* 2. RTSP transport */
    if (config->use_tcp) {
        av_opt_set(ctx->fmt_ctx, "rtsp_transport", "tcp", 0);
        fprintf(stdout, "[RTSP] transport: TCP (reliable)\n");
    } else {
        av_opt_set(ctx->fmt_ctx, "rtsp_transport", "udp", 0);
        fprintf(stdout, "[RTSP] transport: UDP (low latency)\n");
    }

    /* 3. H264 video stream + encoder */
    ctx->video_st = avformat_new_stream(ctx->fmt_ctx, NULL);
    ctx->video_st->time_base = (AVRational){1, config->fps};

    video_encoder_config_t venc_cfg = {
        .width      = config->width,
        .height     = config->height,
        .fps        = config->fps,
        .gop_size   = 30,
        .bit_rate   = config->video_bit_rate > 0 ? config->video_bit_rate
                      : config->width * config->height * config->fps * 0.10,
        .crf        = 0,
        .codec_name = NULL,
    };

    ctx->video_encoder = video_encoder_open(&venc_cfg);
    if (ctx->video_encoder) {
        avcodec_parameters_from_context(ctx->video_st->codecpar,
                                        ctx->video_encoder->codec_ctx);
    }

    /* 4. AAC audio (optional when audio_sample_rate > 0) */
    if (config->audio_sample_rate > 0) {
        ctx->audio_st = avformat_new_stream(ctx->fmt_ctx, NULL);
        ctx->audio_st->time_base = (AVRational){1, config->audio_sample_rate};

        audio_encoder_config_t aenc_cfg = {
            .sample_rate = config->audio_sample_rate,
            .channels    = config->audio_channels,
            .bit_rate    = config->audio_bit_rate,
        };
        ctx->audio_encoder = audio_encoder_open(&aenc_cfg);
        if (ctx->audio_encoder) {
            avcodec_parameters_from_context(ctx->audio_st->codecpar,
                                            ctx->audio_encoder->codec_ctx);
        }
    }

    /* avformat_write_header does the RTSP handshake; max_interleave_delta keeps initial A/V sync tight */
    av_opt_set_int(ctx->fmt_ctx, "max_interleave_delta", 100 * AV_TIME_BASE / 1000, 0);

    AVDictionary *opts = NULL;

    ret = avformat_write_header(ctx->fmt_ctx, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        fprintf(stderr, "[RTSP] connect to RTSP server failed: %s (err=%d: %s)\n",
                config->rtsp_url, ret, av_err2str(ret));
        rtsp_streamer_close(&ctx);
        return NULL;
    }
    ctx->connected = true;
    ctx->h264_parser = h264_parser_create();
    fprintf(stdout, "[RTSP] stream connected: %s (%dx%d@%dfps)\n",
            config->rtsp_url, config->width, config->height, config->fps);

    return ctx;
}



/* Push NV12 via DMA-BUF fd (zero-copy): V4L2 EXPBUF -> AVDRMFrameDescriptor -> h264_rkmpp VPU -> RTSP.
 * PTS computed at capture time; encoder passes it through untouched.
 * Returns: -1=error, 0=sent, 1=EAGAIN (encoder buffered). */
int rtsp_streamer_send_nv12(rtsp_streamer_t *ctx, int fd, int data_size)
{
    if (!ctx || !ctx->connected || fd < 0)
        return -1;

    /* capture-time PTS */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    int64_t elapsed_us = now_us - ctx->start_time_ms * 1000;
    AVRational us_tb = (AVRational){1, 1000000};
    int64_t video_pts = av_rescale_q(elapsed_us, us_tb, ctx->video_st->time_base);

    AVPacket *enc_pkt = NULL;
    int enc_ret = video_encoder_encode_nv12_fd(ctx->video_encoder, fd, data_size, video_pts, &enc_pkt);
    if (enc_ret != 1 || !enc_pkt)
        return (enc_ret < 0) ? -1 : 1;

    AVPacket *out_pkt = av_packet_alloc();
    av_packet_ref(out_pkt, enc_pkt);
    av_packet_unref(enc_pkt);

    /* do not overwrite encoder-passed PTS */
    out_pkt->duration = av_rescale_q(1, (AVRational){1, ctx->config.fps}, ctx->video_st->time_base);
    out_pkt->stream_index = ctx->video_st->index;

    if (ctx->h264_parser && out_pkt->data && out_pkt->size > 0)
    {
        h264_parser_feed(ctx->h264_parser, out_pkt->data, out_pkt->size);
    }

    /* fmt_ctx not thread-safe; serialize A/V writes */
    int64_t save_pts = out_pkt->pts;
    pthread_mutex_lock(&ctx->write_mutex);
    int ret = av_interleaved_write_frame(ctx->fmt_ctx, out_pkt);
    pthread_mutex_unlock(&ctx->write_mutex);

    if (ret < 0)
    {
        /* mark disconnected so caller reconnects */
        fprintf(stderr, "[RTSP] send failed (pts=%lld): %s\n",
                (long long)save_pts, av_err2str(ret));
        ctx->connected = false;
    }

    av_packet_free(&out_pkt);

    return (ret < 0) ? -1 : 0;
}



/* Push NV12 via virtual address (libx264 SW fallback). */
int rtsp_streamer_send_nv12_ptr(rtsp_streamer_t *ctx, const void *nv12_data, int data_size)
{
    if (!ctx || !ctx->connected || !nv12_data) return -1;

    /* capture-time PTS */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    int64_t elapsed_us = now_us - ctx->start_time_ms * 1000;
    AVRational us_tb = (AVRational){1, 1000000};
    int64_t video_pts = av_rescale_q(elapsed_us, us_tb, ctx->video_st->time_base);

    AVPacket *enc_pkt = NULL;
    int enc_ret = video_encoder_encode_nv12(ctx->video_encoder,
                                             (const uint8_t *)nv12_data, data_size, video_pts, &enc_pkt);
    if (enc_ret != 1 || !enc_pkt)
        return (enc_ret < 0) ? -1 : 1;

    AVPacket *out_pkt = av_packet_alloc();
    av_packet_ref(out_pkt, enc_pkt);
    av_packet_unref(enc_pkt);

    /* do not overwrite encoder-passed PTS */
    out_pkt->duration = av_rescale_q(1, (AVRational){1, ctx->config.fps}, ctx->video_st->time_base);
    out_pkt->stream_index = ctx->video_st->index;

    if (ctx->h264_parser && out_pkt->data && out_pkt->size > 0)
        h264_parser_feed(ctx->h264_parser, out_pkt->data, out_pkt->size);

    /* fmt_ctx not thread-safe; serialize A/V writes */
    int64_t save_pts = out_pkt->pts;
    pthread_mutex_lock(&ctx->write_mutex);
    int ret = av_interleaved_write_frame(ctx->fmt_ctx, out_pkt);
    pthread_mutex_unlock(&ctx->write_mutex);

    if (ret < 0) {
        fprintf(stderr, "[RTSP] send failed (pts=%lld): %s\n",
                (long long)save_pts, av_err2str(ret));
        ctx->connected = false;
    }

    av_packet_free(&out_pkt);
    return (ret < 0) ? -1 : 0;
}

int rtsp_streamer_send_audio(rtsp_streamer_t *ctx,
                              const uint8_t *pcm_data, int data_size)
{
    if (!ctx || !ctx->connected || !pcm_data || !ctx->audio_encoder) return -1;

    /* shared CLOCK_MONOTONIC baseline with video */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    int64_t elapsed_ms = now_ms - ctx->start_time_ms;
    AVRational ms_tb = (AVRational){1, 1000};
    int64_t pts = av_rescale_q(elapsed_ms, ms_tb, ctx->audio_st->time_base);

    /* S16LE PCM -> AAC */
    AVPacket *enc_pkt = NULL;
    int enc_ret = audio_encoder_encode_pcm(ctx->audio_encoder, pcm_data, data_size, pts, &enc_pkt);

    if (enc_ret != 1 || !enc_pkt)
        return (enc_ret < 0) ? -1 : 0;

    AVPacket *out_pkt = av_packet_alloc();
    av_packet_ref(out_pkt, enc_pkt);
    av_packet_unref(enc_pkt);

    av_packet_rescale_ts(out_pkt, ctx->audio_encoder->codec_ctx->time_base, ctx->audio_st->time_base);
    out_pkt->stream_index = ctx->audio_st->index;

    pthread_mutex_lock(&ctx->write_mutex);
    int ret = av_interleaved_write_frame(ctx->fmt_ctx, out_pkt);
    pthread_mutex_unlock(&ctx->write_mutex);

    if (ret < 0) {
        fprintf(stderr, "[RTSP] audio frame send failed: %s\n", av_err2str(ret));
        ctx->connected = false;
    }

    av_packet_free(&out_pkt);
    return (ret < 0) ? -1 : 0;
}

void rtsp_streamer_close(rtsp_streamer_t **pctx)
{
    if (!pctx || !*pctx) return;
    rtsp_streamer_t *ctx = *pctx;

    if (ctx->connected && ctx->fmt_ctx) {
        if (ctx->video_encoder) {
            AVPacket *flush_pkts[16];
            int n = video_encoder_flush(ctx->video_encoder, flush_pkts, 16);
            for (int i = 0; i < n; i++) {
                av_packet_rescale_ts(flush_pkts[i],
                                     ctx->video_encoder->codec_ctx->time_base,
                                     ctx->video_st->time_base);
                flush_pkts[i]->stream_index = ctx->video_st->index;
                pthread_mutex_lock(&ctx->write_mutex);
                av_interleaved_write_frame(ctx->fmt_ctx, flush_pkts[i]);
                pthread_mutex_unlock(&ctx->write_mutex);
                av_packet_unref(flush_pkts[i]);
            }
        }
        av_write_trailer(ctx->fmt_ctx);
    }

    if (ctx->audio_encoder) {
        AVPacket *a_flush_pkts[8];
        int an = audio_encoder_flush(ctx->audio_encoder, a_flush_pkts, 8);
        for (int i = 0; i < an; i++) {
            av_packet_rescale_ts(a_flush_pkts[i],
                                 ctx->audio_encoder->codec_ctx->time_base,
                                 ctx->audio_st->time_base);
            a_flush_pkts[i]->stream_index = ctx->audio_st->index;
            pthread_mutex_lock(&ctx->write_mutex);
            av_interleaved_write_frame(ctx->fmt_ctx, a_flush_pkts[i]);
            pthread_mutex_unlock(&ctx->write_mutex);
            av_packet_unref(a_flush_pkts[i]);
        }
    }

    if (ctx->audio_encoder) audio_encoder_close(&ctx->audio_encoder);
    if (ctx->video_encoder) video_encoder_close(&ctx->video_encoder);
    pthread_mutex_destroy(&ctx->write_mutex);

    if (ctx->fmt_ctx) {
        if (ctx->fmt_ctx->pb) avio_closep(&ctx->fmt_ctx->pb);
        avformat_free_context(ctx->fmt_ctx);
    }

    if (ctx->h264_parser) {
        h264_stats_t stats;
        h264_parser_get_stats(ctx->h264_parser, &stats);
        if (stats.total_nalus > 0) {
            fprintf(stdout, "[RTSP] H264 stats: IDR=%d P=%d B=%d GOP_avg=%.1f bitrate=%.0fkbps\n",
                    stats.idr_count, stats.p_count, stats.b_count,
                    stats.avg_gop_size, stats.bitrate_kbps);
        }
        h264_parser_destroy(&ctx->h264_parser);
    }

    free(ctx);
    *pctx = NULL;
}

void rtsp_streamer_get_h264_stats(const rtsp_streamer_t *ctx, h264_stats_t *out_stats)
{
    if (!ctx || !out_stats) return;
    if (ctx->h264_parser) {
        h264_parser_get_stats(ctx->h264_parser, out_stats);
    } else {
        memset(out_stats, 0, sizeof(*out_stats));
    }
}
