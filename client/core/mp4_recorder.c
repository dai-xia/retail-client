

#include "mp4_recorder.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>



mp4_recorder_t* mp4_recorder_open(const mp4_recorder_config_t *config)
{
    if (!config || !config->output_path[0]) return NULL;

    mp4_recorder_t *ctx = (mp4_recorder_t *)calloc(1, sizeof(mp4_recorder_t));
    if (!ctx) return NULL;

    ctx->config = *config;
    ctx->state = RECORDER_IDLE;
    ctx->sync_threshold_us = 50000; 

    
    int ret = avformat_alloc_output_context2(&ctx->fmt_ctx, NULL, "mp4", config->output_path);
    if (ret < 0 || !ctx->fmt_ctx) {
        fprintf(stderr, "[MP4Rec] 创建MP4封装上下文失败\n");
        free(ctx);
        return NULL;
    }

    
    const AVCodec *video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!video_codec) {
        fprintf(stderr, "[MP4Rec] 未找到H264编码器\n");
        goto fail;
    }

    ctx->video_st = avformat_new_stream(ctx->fmt_ctx, video_codec);
    if (!ctx->video_st) goto fail;

    ctx->video_codec_ctx = avcodec_alloc_context3(video_codec);
    if (!ctx->video_codec_ctx) goto fail;

    ctx->video_codec_ctx->codec_id   = AV_CODEC_ID_H264;
    ctx->video_codec_ctx->width      = config->width;
    ctx->video_codec_ctx->height     = config->height;
    ctx->video_codec_ctx->time_base  = (AVRational){1, config->fps};
    ctx->video_codec_ctx->framerate  = (AVRational){config->fps, 1};
    ctx->video_codec_ctx->pix_fmt    = AV_PIX_FMT_YUV420P;
    ctx->video_codec_ctx->bit_rate   = config->video_bit_rate;
    ctx->video_codec_ctx->gop_size   = config->gop_size;
    ctx->video_codec_ctx->max_b_frames = 0;  
    ctx->video_codec_ctx->qmin       = 10;
    ctx->video_codec_ctx->qmax       = 30;

    
    av_opt_set(ctx->video_codec_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->video_codec_ctx->priv_data, "tune", "zerolatency", 0);

    ret = avcodec_open2(ctx->video_codec_ctx, video_codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "[MP4Rec] H264编码器打开失败\n");
        goto fail;
    }
    ret = avcodec_parameters_from_context(ctx->video_st->codecpar, ctx->video_codec_ctx);
    if (ret < 0) goto fail;

    
    if (config->enable_audio) {
        const AVCodec *audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (audio_codec) {
            ctx->audio_st = avformat_new_stream(ctx->fmt_ctx, audio_codec);
            if (ctx->audio_st) {
                ctx->audio_codec_ctx = avcodec_alloc_context3(audio_codec);
                if (ctx->audio_codec_ctx) {
                    ctx->audio_codec_ctx->codec_id      = AV_CODEC_ID_AAC;
                    ctx->audio_codec_ctx->sample_rate    = config->audio_sample_rate;
                    ctx->audio_codec_ctx->channels       = config->audio_channels;
                    ctx->audio_codec_ctx->channel_layout = (config->audio_channels == 1)
                        ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
                    ctx->audio_codec_ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
                    ctx->audio_codec_ctx->bit_rate     = config->audio_bit_rate;
                    ctx->audio_codec_ctx->time_base    = (AVRational){1, config->audio_sample_rate};

                    if (avcodec_open2(ctx->audio_codec_ctx, audio_codec, NULL) >= 0) {
                        avcodec_parameters_from_context(ctx->audio_st->codecpar, ctx->audio_codec_ctx);

                        
                        ctx->swr_ctx = swr_alloc_set_opts(NULL,
                            ctx->audio_codec_ctx->channel_layout, AV_SAMPLE_FMT_FLTP, config->audio_sample_rate,
                            ctx->audio_codec_ctx->channel_layout, AV_SAMPLE_FMT_S16,  config->audio_sample_rate,
                            0, NULL);
                        if (ctx->swr_ctx) swr_init(ctx->swr_ctx);
                    }
                }
            }
        }
    }

    
    if (config->faststart) {
        av_opt_set(ctx->fmt_ctx->priv_data, "movflags", "faststart", 0);
    }

    
    if (!(ctx->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ctx->fmt_ctx->pb, config->output_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "[MP4Rec] 文件打开失败: %s\n", config->output_path);
            goto fail;
        }
    }

    
    ret = avformat_write_header(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "[MP4Rec] 写入MP4头失败\n");
        goto fail;
    }

    
    ctx->h264_parser = h264_parser_create();

    ctx->state = RECORDER_RECORDING;
    ctx->start_time_ms = av_gettime() / 1000;
    fprintf(stdout, "[MP4Rec] 录像开始: %s (%dx%d@%dfps, audio=%s)\n",
            config->output_path, config->width, config->height, config->fps,
            config->enable_audio ? "on" : "off");

    return ctx;

fail:
    if (ctx->video_codec_ctx) avcodec_free_context(&ctx->video_codec_ctx);
    if (ctx->audio_codec_ctx) avcodec_free_context(&ctx->audio_codec_ctx);
    if (ctx->swr_ctx) swr_free(&ctx->swr_ctx);
    if (ctx->fmt_ctx) {
        if (ctx->fmt_ctx->pb) avio_closep(&ctx->fmt_ctx->pb);
        avformat_free_context(ctx->fmt_ctx);
    }
    free(ctx);
    return NULL;
}




static bool check_video_sync(mp4_recorder_t *ctx, int64_t video_pts_us)
{
    if (!ctx->has_audio) return false;  

    ctx->video_clock = video_pts_us;
    int64_t diff = ctx->video_clock - ctx->audio_clock;

    if (diff > ctx->sync_threshold_us) {
        ctx->video_drop_count++;
        return true;  
    }
    return false;
}



static int encode_and_write_video(mp4_recorder_t *ctx, AVFrame *frame)
{
    if (!ctx || !frame) return -1;

    
    int64_t video_pts_us = av_rescale_q(ctx->video_pts,
        ctx->video_codec_ctx->time_base, (AVRational){1, 1000000});

    
    if (check_video_sync(ctx, video_pts_us)) {
        ctx->video_pts++;
        return 1;  
    }

    frame->pts = ctx->video_pts;
    int ret = avcodec_send_frame(ctx->video_codec_ctx, frame);
    if (ret < 0) return -1;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return -1;

    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx->video_codec_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) { av_packet_free(&pkt); return -1; }

        
        if (ctx->h264_parser && pkt->data && pkt->size > 0) {
            h264_parser_feed(ctx->h264_parser, pkt->data, pkt->size);
        }

        av_packet_rescale_ts(pkt, ctx->video_codec_ctx->time_base, ctx->video_st->time_base);
        pkt->stream_index = ctx->video_st->index;
        av_interleaved_write_frame(ctx->fmt_ctx, pkt);
        av_packet_unref(pkt);
        ctx->total_video_frames++;
    }

    av_packet_free(&pkt);
    ctx->video_pts++;
    ctx->has_video = true;
    return 0;
}

int mp4_recorder_write_nv12(mp4_recorder_t *ctx, const uint8_t *nv12_data, int size)
{
    if (!ctx || ctx->state != RECORDER_RECORDING || !nv12_data) return -1;

    int w = ctx->config.width;
    int h = ctx->config.height;
    int expected = w * h * 3 / 2;
    if (size < expected) return -1;

    /* NV12 → YUV420P */
    AVFrame *frame = av_frame_alloc();
    if (!frame) return -1;

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width  = w;
    frame->height = h;
    av_frame_get_buffer(frame, 0);
    av_frame_make_writable(frame);

    
    memcpy(frame->data[0], nv12_data, w * h);

    
    const uint8_t *nv12_uv = nv12_data + w * h;
    for (int i = 0; i < h / 2; i++) {
        for (int j = 0; j < w / 2; j++) {
            frame->data[1][i * frame->linesize[1] + j] = nv12_uv[(i * w + j * 2)];
            frame->data[2][i * frame->linesize[2] + j] = nv12_uv[(i * w + j * 2 + 1)];
        }
    }

    int ret = encode_and_write_video(ctx, frame);
    av_frame_free(&frame);
    return ret;
}

int mp4_recorder_write_bgr(mp4_recorder_t *ctx, const uint8_t *bgr_data, int size)
{
    if (!ctx || ctx->state != RECORDER_RECORDING || !bgr_data) return -1;

    int w = ctx->config.width;
    int h = ctx->config.height;
    int expected = w * h * 3;
    if (size < expected) return -1;

    /* BGR24 → YUV420P (sws_scale) */
    static struct SwsContext *sws_ctx = NULL;
    if (!sws_ctx) {
        sws_ctx = sws_getContext(w, h, AV_PIX_FMT_BGR24,
                                 w, h, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, NULL, NULL, NULL);
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) return -1;

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width  = w;
    frame->height = h;
    av_frame_get_buffer(frame, 0);

    const uint8_t *src_data[1] = { bgr_data };
    int src_linesize[1] = { w * 3 };
    sws_scale(sws_ctx, src_data, src_linesize, 0, h,
              frame->data, frame->linesize);

    int ret = encode_and_write_video(ctx, frame);
    av_frame_free(&frame);
    return ret;
}



int mp4_recorder_write_audio(mp4_recorder_t *ctx, const int16_t *pcm_data, int samples)
{
    if (!ctx || ctx->state != RECORDER_RECORDING) return -1;
    if (!ctx->audio_codec_ctx || !pcm_data || samples <= 0) return -1;

    
    uint8_t *audio_buf = NULL;
    int out_samples = swr_convert(ctx->swr_ctx, &audio_buf, samples,
                                   (const uint8_t **)&pcm_data, samples);
    if (out_samples <= 0) return 0;

    
    ctx->audio_pts += out_samples;
    ctx->audio_clock = av_rescale_q(ctx->audio_pts,
        ctx->audio_codec_ctx->time_base, (AVRational){1, 1000000});
    ctx->has_audio = true;

    
    AVFrame *frame = av_frame_alloc();
    if (!frame) return -1;

    frame->format      = AV_SAMPLE_FMT_FLTP;
    frame->channel_layout = ctx->audio_codec_ctx->channel_layout;
    frame->sample_rate = ctx->config.audio_sample_rate;
    frame->nb_samples  = out_samples;
    frame->pts         = ctx->audio_pts - out_samples;  
    av_frame_get_buffer(frame, 0);

    
    int ch = ctx->config.audio_channels;
    for (int c = 0; c < ch; c++) {
        memcpy(frame->data[c], audio_buf + c * out_samples * sizeof(float),
               out_samples * sizeof(float));
    }

    int ret = avcodec_send_frame(ctx->audio_codec_ctx, frame);
    av_frame_free(&frame);
    if (ret < 0) return -1;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return -1;

    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx->audio_codec_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) { av_packet_free(&pkt); return -1; }

        av_packet_rescale_ts(pkt, ctx->audio_codec_ctx->time_base, ctx->audio_st->time_base);
        pkt->stream_index = ctx->audio_st->index;
        av_interleaved_write_frame(ctx->fmt_ctx, pkt);
        av_packet_unref(pkt);
        ctx->total_audio_frames++;
    }

    av_packet_free(&pkt);
    return 0;
}



void mp4_recorder_pause(mp4_recorder_t *ctx)
{
    if (ctx && ctx->state == RECORDER_RECORDING) {
        ctx->state = RECORDER_PAUSED;
    }
}

void mp4_recorder_resume(mp4_recorder_t *ctx)
{
    if (ctx && ctx->state == RECORDER_PAUSED) {
        ctx->state = RECORDER_RECORDING;
    }
}



double mp4_recorder_get_duration(const mp4_recorder_t *ctx)
{
    if (!ctx) return 0.0;
    return (double)ctx->total_video_frames / (double)ctx->config.fps;
}

int64_t mp4_recorder_get_file_size(const mp4_recorder_t *ctx)
{
    if (!ctx) return 0;
    
    if (!ctx->config.output_path[0]) return 0;
    FILE *f = fopen(ctx->config.output_path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    int64_t sz = ftell(f);
    fclose(f);
    return sz;
}

void mp4_recorder_get_h264_stats(const mp4_recorder_t *ctx, h264_stats_t *out_stats)
{
    if (!ctx || !out_stats) return;
    if (ctx->h264_parser) {
        h264_parser_get_stats(ctx->h264_parser, out_stats);
    } else {
        memset(out_stats, 0, sizeof(*out_stats));
    }
}



void mp4_recorder_close(mp4_recorder_t **pctx)
{
    if (!pctx || !*pctx) return;
    mp4_recorder_t *ctx = *pctx;

    if (ctx->state == RECORDER_RECORDING || ctx->state == RECORDER_PAUSED) {
        
        if (ctx->video_codec_ctx) {
            avcodec_send_frame(ctx->video_codec_ctx, NULL);
            AVPacket *pkt = av_packet_alloc();
            while (avcodec_receive_packet(ctx->video_codec_ctx, pkt) == 0) {
                av_packet_rescale_ts(pkt, ctx->video_codec_ctx->time_base, ctx->video_st->time_base);
                pkt->stream_index = ctx->video_st->index;
                av_interleaved_write_frame(ctx->fmt_ctx, pkt);
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }

        
        if (ctx->audio_codec_ctx) {
            avcodec_send_frame(ctx->audio_codec_ctx, NULL);
            AVPacket *pkt = av_packet_alloc();
            while (avcodec_receive_packet(ctx->audio_codec_ctx, pkt) == 0) {
                av_packet_rescale_ts(pkt, ctx->audio_codec_ctx->time_base, ctx->audio_st->time_base);
                pkt->stream_index = ctx->audio_st->index;
                av_interleaved_write_frame(ctx->fmt_ctx, pkt);
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }

        
        av_write_trailer(ctx->fmt_ctx);

        
        double duration = mp4_recorder_get_duration(ctx);
        int64_t file_size = mp4_recorder_get_file_size(ctx);
        fprintf(stdout, "\n[MP4Rec] 录像结束: %s\n", ctx->config.output_path);
        fprintf(stdout, "  时长: %.1f秒, 文件大小: %.1fMB\n",
                duration, (double)file_size / 1048576.0);
        fprintf(stdout, "  视频帧: %lld, 音频帧: %lld\n",
                (long long)ctx->total_video_frames, (long long)ctx->total_audio_frames);
        fprintf(stdout, "  同步丢帧: %d, 同步重复帧: %d\n",
                ctx->video_drop_count, ctx->video_dup_count);

        if (ctx->h264_parser) {
            h264_stats_t stats;
            h264_parser_get_stats(ctx->h264_parser, &stats);
            h264_parser_print_stats(&stats);
            h264_parser_destroy(&ctx->h264_parser);
        }
    }

    
    if (ctx->video_codec_ctx) avcodec_free_context(&ctx->video_codec_ctx);
    if (ctx->audio_codec_ctx) avcodec_free_context(&ctx->audio_codec_ctx);
    if (ctx->swr_ctx) swr_free(&ctx->swr_ctx);
    if (ctx->fmt_ctx) {
        if (ctx->fmt_ctx->pb && !(ctx->fmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&ctx->fmt_ctx->pb);
        avformat_free_context(ctx->fmt_ctx);
    }

    free(ctx);
    *pctx = NULL;
}
