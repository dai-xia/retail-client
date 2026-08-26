/**
 * @file rtsp_streamer.c
 * @brief FFmpeg-based RTSP live streaming (RK3568 VPU HW encode)
 *
 * Core flow:
 *   avformat_alloc_output_context2 -> av_opt_set(rtsp_transport) ->
 *   avformat_new_stream (H264 video + AAC audio) -> video_encoder_open ->
 *   avformat_write_header (RTSP ANNOUNCE/SETUP/RECORD + SDP) ->
 *   av_interleaved_write_frame (loop) -> av_write_trailer + release
 *
 * RK3568 HW: h264_rkmpp VPU encode, NV12 direct zero-copy, low CPU usage.
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



/* Open RTSP streamer: build the muxer, encoders, and RTSP session.
 * start_time_ms: if config->base_time_ms > 0 (reconnect scenario) reuse the
 *   previous baseline so PTS keeps increasing monotonically and avoids a PTS
 *   cliff (which would cause player seeking jumps / timestamp errors / A-V
 *   desync); otherwise read CLOCK_MONOTONIC (immune to NTP/manual time changes).
 * Audio path is optional (audio_sample_rate > 0).
 * write_mutex serializes av_interleaved_write_frame across A/V threads because
 * AVFormatContext is not thread-safe.
 * On failure all allocated resources are rolled back internally. */
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

    /* start_time_ms baseline: reuse on reconnect, else CLOCK_MONOTONIC boot time */
    if (config->base_time_ms > 0) {
        ctx->start_time_ms = config->base_time_ms;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ctx->start_time_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }

    /* === 1. Create RTSP muxer context === */
    int ret = avformat_alloc_output_context2(&ctx->fmt_ctx, NULL, "rtsp", config->rtsp_url);
    if (ret < 0 || !ctx->fmt_ctx) {
        fprintf(stderr, "[RTSP] create RTSP muxer context failed\n");
        free(ctx);
        return NULL;
    }

    /* === 2. RTSP transport === */
    if (config->use_tcp) {
        av_opt_set(ctx->fmt_ctx, "rtsp_transport", "tcp", 0);
        fprintf(stdout, "[RTSP] transport: TCP (reliable)\n");
    } else {
        av_opt_set(ctx->fmt_ctx, "rtsp_transport", "udp", 0);
        fprintf(stdout, "[RTSP] transport: UDP (low latency)\n");
    }

    /* === 3. H264 video stream + encoder (VPU HW preferred, libx264 fallback) ===
     * avformat_new_stream second arg NULL: encoder lookup is done inside
     * video_encoder_open; codecpar is filled by avcodec_parameters_from_context */
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

    /* === 4. AAC audio stream (optional, enabled when audio_sample_rate > 0) === */
    if (config->audio_sample_rate > 0) {
        /* Same as video: encoder lookup inside audio_encoder_open, pass NULL */
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

    /* avformat_write_header performs the full RTSP handshake (TCP connect ->
     * ANNOUNCE -> SETUP -> RECORD). The RTSP muxer manages its own socket, so
     * no manual avio_open is needed.
     * max_interleave_delta: A/V interleave buffer threshold to keep initial
     * A/V sync tight; opts only valid during this call. */
    av_opt_set_int(ctx->fmt_ctx, "max_interleave_delta", 100 * AV_TIME_BASE / 1000, 0);

    AVDictionary *opts = NULL;

    /* RTSP handshake */
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



/* Push one NV12 frame via DMA-BUF fd (true zero-copy).
 *   V4L2 EXPBUF -> DMA-BUF fd -> AVDRMFrameDescriptor -> h264_rkmpp VPU DMA
 *   read -> avcodec_receive_packet -> RTP packetize -> RTSP send.
 * PTS best practice: capture-time CLOCK_MONOTONIC -> AVFrame->pts -> encoder
 *   passes through to enc_pkt; the caller reuses enc_pkt->pts/dts directly.
 * CPU never touches the NV12->H264 data path.
 * Returns: -1=error, 0=sent, 1=EAGAIN (encoder buffered, no output yet) */
int rtsp_streamer_send_nv12(rtsp_streamer_t *ctx, int fd, int data_size)
{
    if (!ctx || !ctx->connected || fd < 0)
        return -1;

    /* 1. PTS at capture time (matches audio path, no encode latency) */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    int64_t elapsed_us = now_us - ctx->start_time_ms * 1000;
    AVRational us_tb = (AVRational){1, 1000000};
    int64_t video_pts = av_rescale_q(elapsed_us, us_tb, ctx->video_st->time_base);

    /* 2. VPU DMA-BUF zero-copy encode (PTS computed at capture time) */
    AVPacket *enc_pkt = NULL;
    int enc_ret = video_encoder_encode_nv12_fd(ctx->video_encoder, fd, data_size, video_pts, &enc_pkt);
    if (enc_ret != 1 || !enc_pkt)
        return (enc_ret < 0) ? -1 : 1;

    /* 3. Ref-copy to decouple lifetime */
    AVPacket *out_pkt = av_packet_alloc();
    av_packet_ref(out_pkt, enc_pkt);
    av_packet_unref(enc_pkt);

    /* 4. Reuse encoder-passed PTS (do not overwrite) */
    out_pkt->duration = av_rescale_q(1, (AVRational){1, ctx->config.fps}, ctx->video_st->time_base);
    out_pkt->stream_index = ctx->video_st->index;

    /* 5. H264 stream stats (frame type / bitrate, for logging only) */
    if (ctx->h264_parser && out_pkt->data && out_pkt->size > 0)
    {
        h264_parser_feed(ctx->h264_parser, out_pkt->data, out_pkt->size);
    }

    /* 6. Mutex-protected av_interleaved_write_frame: A/V threads share fmt_ctx
     * which is not thread-safe */
    int64_t save_pts = out_pkt->pts;
    pthread_mutex_lock(&ctx->write_mutex);
    int ret = av_interleaved_write_frame(ctx->fmt_ctx, out_pkt);
    pthread_mutex_unlock(&ctx->write_mutex);

    if (ret < 0)
    {
        /* Write failed: mark RTSP disconnected so the caller reconnects */
        fprintf(stderr, "[RTSP] send failed (pts=%lld): %s\n",
                (long long)save_pts, av_err2str(ret));
        ctx->connected = false;
    }

    av_packet_free(&out_pkt);

    return (ret < 0) ? -1 : 0;
}



/* Push one NV12 frame via virtual address pointer (SW encode path).
 * NV12 virtual address -> libx264 SW encode -> H264 -> RTP -> RTSP.
 * Used as fallback when h264_rkmpp is unavailable. */
int rtsp_streamer_send_nv12_ptr(rtsp_streamer_t *ctx, const void *nv12_data, int data_size)
{
    if (!ctx || !ctx->connected || !nv12_data) return -1;

    /* 1. PTS at capture time (matches audio path) */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    int64_t elapsed_us = now_us - ctx->start_time_ms * 1000;
    AVRational us_tb = (AVRational){1, 1000000};
    int64_t video_pts = av_rescale_q(elapsed_us, us_tb, ctx->video_st->time_base);

    /* 2. SW encode: NV12 virtual address -> libx264 */
    AVPacket *enc_pkt = NULL;
    int enc_ret = video_encoder_encode_nv12(ctx->video_encoder,
                                             (const uint8_t *)nv12_data, data_size, video_pts, &enc_pkt);
    if (enc_ret != 1 || !enc_pkt)
        return (enc_ret < 0) ? -1 : 1;

    /* 3. Ref-copy to decouple lifetime */
    AVPacket *out_pkt = av_packet_alloc();
    av_packet_ref(out_pkt, enc_pkt);
    av_packet_unref(enc_pkt);

    /* 4. Reuse encoder-passed PTS (do not overwrite) */
    out_pkt->duration = av_rescale_q(1, (AVRational){1, ctx->config.fps}, ctx->video_st->time_base);
    out_pkt->stream_index = ctx->video_st->index;

    /* 5. H264 stream stats */
    if (ctx->h264_parser && out_pkt->data && out_pkt->size > 0)
        h264_parser_feed(ctx->h264_parser, out_pkt->data, out_pkt->size);

    /* 6. Mutex-protected av_interleaved_write_frame */
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

    /* Audio PTS: shared CLOCK_MONOTONIC baseline with video */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    int64_t elapsed_ms = now_ms - ctx->start_time_ms;
    AVRational ms_tb = (AVRational){1, 1000};
    int64_t pts = av_rescale_q(elapsed_ms, ms_tb, ctx->audio_st->time_base);

    /* Encode: S16 PCM -> AAC (swr + send_frame + receive_packet inside) */
    AVPacket *enc_pkt = NULL;
    int enc_ret = audio_encoder_encode_pcm(ctx->audio_encoder, pcm_data, data_size, pts, &enc_pkt);

    if (enc_ret != 1 || !enc_pkt)
        return (enc_ret < 0) ? -1 : 0;

    /* Ref-copy + PTS rescale + write (symmetric with the video path) */
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
        /* Flush video encoder buffer */
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

    /* Flush audio encoder buffer */
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
