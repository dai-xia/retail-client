/**
 * @file video_encoder.c
 * @brief FFmpeg H264 encoder (h264_rkmpp VPU HW encode, NV12 input)
 */

#include "video_encoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext_drm.h>
#include <libswscale/swscale.h>
#include <libdrm/drm_fourcc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Prefers h264_rkmpp (AV_PIX_FMT_DRM_PRIME DMA-BUF zero-copy), falls back to libx264 (NV12).
 * B-frames disabled for low latency; SPS/PPS exported to extradata for RTSP SDP. */
video_encoder_t *video_encoder_open(const video_encoder_config_t *config)
{
    if (!config) return NULL;

    video_encoder_t *ctx = (video_encoder_t *)calloc(1, sizeof(video_encoder_t));
    if (!ctx) return NULL;

    ctx->src_width  = config->width;
    ctx->src_height = config->height;
    ctx->opened     = false;

    /* default h264_rkmpp */
    const char *enc_name = config->codec_name ? config->codec_name : "h264_rkmpp";
    const AVCodec *codec = avcodec_find_encoder_by_name(enc_name);
    if (codec) {
        ctx->is_hw_encoder = true;
        fprintf(stdout, "[VideoEncoder] using encoder: %s (VPU HW, NV12 DMA-BUF zero-copy)\n", enc_name);
    } else {
        fprintf(stdout, "[VideoEncoder] %s unavailable, fall back to libx264 SW encoder\n", enc_name);
        codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) {
            fprintf(stderr, "[VideoEncoder] libx264 also unavailable, cannot encode\n");
            free(ctx);
            return NULL;
        }
        ctx->is_hw_encoder = false;
        fprintf(stdout, "[VideoEncoder] using encoder: libx264 (SW, NV12 virtual address)\n");
    }

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        fprintf(stderr, "[VideoEncoder] avcodec_alloc_context3 failed\n");
        free(ctx);
        return NULL;
    }

    /* bitrate = width*height*fps*0.10 when not configured */
    ctx->codec_ctx->bit_rate     = config->bit_rate > 0 ? config->bit_rate
                                   : config->width * config->height * config->fps * 0.10;
    ctx->codec_ctx->width        = config->width;
    ctx->codec_ctx->height       = config->height;
    /* time_base = 1/fps */
    ctx->codec_ctx->time_base    = (AVRational){1, config->fps};
    ctx->codec_ctx->framerate    = (AVRational){config->fps, 1};
    ctx->codec_ctx->gop_size     = config->gop_size > 0 ? config->gop_size : 30;
    ctx->codec_ctx->max_b_frames = 0;

    if (ctx->is_hw_encoder) {
        /* HW: DRM_PRIME, fd via data[0], zero CPU copy */
        ctx->codec_ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    } else {
        /* SW: NV12 virtual address */
        ctx->codec_ctx->pix_fmt = AV_PIX_FMT_NV12;
        /* ultrafast + zerolatency */
        av_opt_set(ctx->codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "tune", "zerolatency", 0);
    }

    /* GLOBAL_HEADER: put SPS/PPS in extradata (sprop-parameter-sets) so players can init without an I-frame */
    ctx->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(ctx->codec_ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "[VideoEncoder] avcodec_open2 failed (encoder: %s)\n",
                ctx->is_hw_encoder ? "h264_rkmpp" : "libx264");
        avcodec_free_context(&ctx->codec_ctx);
        free(ctx);
        return NULL;
    }

    /* AVFrame: set size/format only, no buffer (zero-copy) */
    ctx->frame = av_frame_alloc();
    if (!ctx->frame) {
        fprintf(stderr, "[VideoEncoder] av_frame_alloc failed\n");
        video_encoder_close(&ctx);
        return NULL;
    }
    ctx->frame->width  = config->width;
    ctx->frame->height = config->height;
    ctx->frame->format = AV_PIX_FMT_NV12;

    ctx->pkt = av_packet_alloc();
    if (!ctx->pkt) {
        fprintf(stderr, "[VideoEncoder] av_packet_alloc failed\n");
        video_encoder_close(&ctx);
        return NULL;
    }

    /* pre-allocate AVDRMFrameDescriptor once; only fd changes per frame */
    if (ctx->is_hw_encoder)
    {
        AVDRMFrameDescriptor *desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
        if (!desc)
        {
            fprintf(stderr, "[VideoEncoder] av_mallocz DRM desc failed\n");
            video_encoder_close(&ctx);
            return NULL;
        }

        /* one DMA object holds the whole NV12 image */
        desc->nb_objects = 1;
        /* NV12 total = width*height*3/2 */
        desc->objects[0].size             = config->width * config->height * 3 / 2;
        desc->objects[0].format_modifier  = DRM_FORMAT_MOD_LINEAR;

        desc->nb_layers = 1;
        desc->layers[0].format            = DRM_FORMAT_NV12;
        desc->layers[0].nb_planes         = 2;

        desc->layers[0].planes[0].object_index = 0;
        desc->layers[0].planes[0].offset       = 0;
        desc->layers[0].planes[0].pitch        = config->width;

        /* plane[1]: UV, same DMA object */
        desc->layers[0].planes[1].object_index = 0;
        desc->layers[0].planes[1].offset       = config->width * config->height;
        desc->layers[0].planes[1].pitch        = config->width;

        /* wrap desc in AVBufferRef to manage lifetime */
        AVBufferRef *buf = av_buffer_create((uint8_t *)desc, sizeof(*desc),
                                             av_buffer_default_free, NULL, 0);
        if (!buf)
        {
            av_free(desc);
            fprintf(stderr, "[VideoEncoder] av_buffer_create DRM buf failed\n");
            video_encoder_close(&ctx);
            return NULL;
        }

        ctx->drm_desc = desc;
        ctx->drm_buf  = buf;
    }

    ctx->opened = true;
    fprintf(stdout, "[VideoEncoder] encoder initialized (%s, %dx%d, %s)\n",
            ctx->is_hw_encoder ? "VPU HW" : "libx264 SW",
            config->width, config->height,
            ctx->is_hw_encoder ? "DMA-BUF zero-copy" : "NV12 virtual address");
    return ctx;
}

/* Encode NV12 -> H264. HW: mmap virtual address bound to data[] (zero-copy); SW: copy into frame.
 * Returns: 1=got packet, 0=EAGAIN, -1=error. */
int video_encoder_encode_nv12(video_encoder_t *ctx,
                               const uint8_t *nv12_data, int data_size,
                               int64_t pts, AVPacket **out_pkt)
{
    if (!ctx || !ctx->opened || !nv12_data)
        return -1;

    int ret;

    if (ctx->is_hw_encoder) {
        av_frame_unref(ctx->frame);
        ctx->frame->data[0]     = (uint8_t *)nv12_data;
        ctx->frame->data[1]     = (uint8_t *)nv12_data + ctx->src_width * ctx->src_height;
        ctx->frame->linesize[0] = ctx->src_width;
        ctx->frame->linesize[1] = ctx->src_width;
        ctx->frame->format      = AV_PIX_FMT_NV12;
        ctx->frame->width       = ctx->src_width;
        ctx->frame->height      = ctx->src_height;
    } else {
        if (!ctx->frame->data[0]) {
            ctx->frame->format = AV_PIX_FMT_NV12;
            ret = av_frame_get_buffer(ctx->frame, 0);
            if (ret < 0) return -1;
        }
        ret = av_frame_make_writable(ctx->frame);
        if (ret < 0) return -1;
        memcpy(ctx->frame->data[0], nv12_data, ctx->src_width * ctx->src_height);
        memcpy(ctx->frame->data[1], nv12_data + ctx->src_width * ctx->src_height,
               ctx->src_width * ctx->src_height / 2);
    }

    /* capture-time PTS, passed through */
    ctx->frame->pts = pts;

    ret = avcodec_send_frame(ctx->codec_ctx, ctx->frame);
    if (ret < 0) return -1;

    ret = avcodec_receive_packet(ctx->codec_ctx, ctx->pkt);
    if (ret == 0) { *out_pkt = ctx->pkt; return 1; }
    if (ret == AVERROR(EAGAIN)) return 0;
    return -1;
}



/* Encode NV12 via DMA-BUF fd (DRM-PRIME zero-copy). Only fd changes per frame.
 * Returns: 1=got packet, 0=EAGAIN, -1=error. */
int video_encoder_encode_nv12_fd(video_encoder_t *ctx,
                                  int fd, int data_size,
                                  int64_t pts, AVPacket **out_pkt)
{
    if (!ctx || !ctx->opened || fd < 0 || !ctx->drm_desc)
        return -1;

    av_frame_unref(ctx->frame);

    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)ctx->drm_desc;
    desc->objects[0].fd = fd;

    ctx->frame->buf[0]  = av_buffer_ref((AVBufferRef *)ctx->drm_buf);

    /* DRM-PRIME: data[0] = AVDRMFrameDescriptor*, not pixels */
    ctx->frame->data[0] = (uint8_t *)desc;

    ctx->frame->format  = AV_PIX_FMT_DRM_PRIME;
    ctx->frame->width   = ctx->src_width;
    ctx->frame->height  = ctx->src_height;

    /* capture-time PTS, passed through */
    ctx->frame->pts = pts;

    int ret = avcodec_send_frame(ctx->codec_ctx, ctx->frame);
    if (ret < 0) {
        av_frame_unref(ctx->frame);
        return -1;
    }

    ret = avcodec_receive_packet(ctx->codec_ctx, ctx->pkt);
    if (ret == 0) {
        *out_pkt = ctx->pkt;
        return 1;
    }
    if (ret == AVERROR(EAGAIN)) {
        return 0;
    }
    return -1;
}

/* drain remaining buffered frames */
int video_encoder_flush(video_encoder_t *ctx, AVPacket **out_pkts, int max_pkts)
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

void video_encoder_close(video_encoder_t **pctx)
{
    if (!pctx || !*pctx) return;
    video_encoder_t *ctx = *pctx;

    if (ctx->frame)   av_frame_free(&ctx->frame);
    if (ctx->pkt)     av_packet_free(&ctx->pkt);
    if (ctx->drm_buf) av_buffer_unref((AVBufferRef **)&ctx->drm_buf);
    if (ctx->codec_ctx) avcodec_free_context(&ctx->codec_ctx);

    free(ctx);
    *pctx = NULL;
    fprintf(stdout, "[VideoEncoder] encoder released\n");
}
