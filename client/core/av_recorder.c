

#include "av_recorder.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

av_recorder_t *av_recorder_open(const av_recorder_config_t *config)
{
    if (!config) return NULL;

    av_recorder_t *ctx = (av_recorder_t *)calloc(1, sizeof(av_recorder_t));
    if (!ctx) return NULL;

    memcpy(&ctx->config, config, sizeof(av_recorder_config_t));
    ctx->video_pts = 0;
    ctx->audio_pts = 0;
    ctx->header_written = false;
    ctx->recording = false;

    
    int ret = avformat_alloc_output_context2(&ctx->fmt_ctx, NULL,
                                              config->container_fmt,
                                              config->filepath);
    if (ret < 0 || !ctx->fmt_ctx) {
        fprintf(stderr, "[AVRecorder] avformat_alloc_output_context2 失败\n");
        free(ctx);
        return NULL;
    }

    
    
    ctx->video_st = avformat_new_stream(ctx->fmt_ctx, NULL);
    if (!ctx->video_st) {
        fprintf(stderr, "[AVRecorder] 创建视频流失败\n");
        av_recorder_close(&ctx);
        return NULL;
    }

    ctx->video_st->time_base = (AVRational){1, config->fps};

    video_encoder_config_t venc_cfg = {
        .width      = config->width,
        .height     = config->height,
        .fps        = config->fps,
        .gop_size   = 30,
        .bit_rate   = config->video_bit_rate > 0 ? config->video_bit_rate
                      : config->width * config->height * config->fps * 0.10,
        .crf        = 25,
        .codec_name = NULL,
    };
    ctx->video_encoder = video_encoder_open(&venc_cfg);
    if (!ctx->video_encoder) {
        fprintf(stderr, "[AVRecorder] 视频编码器初始化失败\n");
        av_recorder_close(&ctx);
        return NULL;
    }

    avcodec_parameters_from_context(ctx->video_st->codecpar, ctx->video_encoder->codec_ctx);

    
    
    ctx->audio_st = avformat_new_stream(ctx->fmt_ctx, NULL);
    if (!ctx->audio_st) {
        fprintf(stderr, "[AVRecorder] 创建音频流失败\n");
        av_recorder_close(&ctx);
        return NULL;
    }

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

    
    if (!(ctx->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ctx->fmt_ctx->pb, config->filepath, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "[AVRecorder] avio_open 失败: %s\n", config->filepath);
            av_recorder_close(&ctx);
            return NULL;
        }
    }

    
    ret = avformat_write_header(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "[AVRecorder] write_header 失败\n");
        av_recorder_close(&ctx);
        return NULL;
    }
    ctx->header_written = true;
    ctx->recording = true;

    fprintf(stdout, "[AVRecorder] 录像开始: %s (%dx%d@%dfps)\n",
            config->filepath, config->width, config->height, config->fps);
    return ctx;
}

int av_recorder_write_video_frame(av_recorder_t *ctx,
                                   const uint8_t *bgr_data, int data_size)
{
    if (!ctx || !ctx->recording || !bgr_data) return -1;

    AVPacket *pkt = NULL;
    int ret = video_encoder_encode_bgr(ctx->video_encoder, bgr_data, data_size, &pkt);

    if (ret == 1 && pkt) {
        
        av_packet_rescale_ts(pkt, ctx->video_encoder->codec_ctx->time_base,
                             ctx->video_st->time_base);
        pkt->stream_index = ctx->video_st->index;

        
        ret = av_interleaved_write_frame(ctx->fmt_ctx, pkt);
        av_packet_unref(pkt);

        ctx->video_pts++;
        return (ret < 0) ? -1 : 0;
    }

    return (ret < 0) ? -1 : 0;
}

int av_recorder_write_audio_samples(av_recorder_t *ctx,
                                     const uint8_t *pcm_data, int data_size)
{
    if (!ctx || !ctx->recording || !pcm_data) return -1;
    if (!ctx->audio_encoder) return -1;

    
    AVPacket *enc_pkt = NULL;
    int ret = audio_encoder_encode_pcm(ctx->audio_encoder, pcm_data, data_size,
                                        ctx->audio_pts, &enc_pkt);
    if (ret == 1 && enc_pkt) {
        int nb_samples = data_size / (ctx->config.audio_channels * 2);
        ctx->audio_pts += nb_samples;

        av_packet_rescale_ts(enc_pkt, ctx->audio_encoder->codec_ctx->time_base,
                             ctx->audio_st->time_base);
        enc_pkt->stream_index = ctx->audio_st->index;
        av_interleaved_write_frame(ctx->fmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }

    return (ret < 0) ? -1 : 0;
}

void av_recorder_close(av_recorder_t **pctx)
{
    if (!pctx || !*pctx) return;
    av_recorder_t *ctx = *pctx;

    if (ctx->recording && ctx->fmt_ctx) {
        
        if (ctx->video_encoder) {
            AVPacket *flush_pkts[16];
            int n = video_encoder_flush(ctx->video_encoder, flush_pkts, 16);
            for (int i = 0; i < n; i++) {
                av_packet_rescale_ts(flush_pkts[i],
                                      ctx->video_encoder->codec_ctx->time_base,
                                      ctx->video_st->time_base);
                flush_pkts[i]->stream_index = ctx->video_st->index;
                av_interleaved_write_frame(ctx->fmt_ctx, flush_pkts[i]);
                av_packet_unref(flush_pkts[i]);
            }
        }

        
        if (ctx->header_written) {
            av_write_trailer(ctx->fmt_ctx);
        }
    }

    
    if (ctx->audio_encoder) {
        AVPacket *a_flush_pkts[8];
        int an = audio_encoder_flush(ctx->audio_encoder, a_flush_pkts, 8);
        for (int i = 0; i < an; i++) {
            av_packet_rescale_ts(a_flush_pkts[i],
                                 ctx->audio_encoder->codec_ctx->time_base,
                                 ctx->audio_st->time_base);
            a_flush_pkts[i]->stream_index = ctx->audio_st->index;
            av_interleaved_write_frame(ctx->fmt_ctx, a_flush_pkts[i]);
            av_packet_unref(a_flush_pkts[i]);
        }
    }

    if (ctx->audio_encoder) audio_encoder_close(&ctx->audio_encoder);
    if (ctx->video_encoder) video_encoder_close(&ctx->video_encoder);

    if (ctx->fmt_ctx) {
        if (!(ctx->fmt_ctx->oformat->flags & AVFMT_NOFILE) && ctx->fmt_ctx->pb) {
            avio_closep(&ctx->fmt_ctx->pb);
        }
        avformat_free_context(ctx->fmt_ctx);
    }

    fprintf(stdout, "[AVRecorder] 录像结束: %s\n", ctx->config.filepath);
    free(ctx);
    *pctx = NULL;
}
