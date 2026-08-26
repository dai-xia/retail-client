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

/*
 * Open encoder. Prefers h264_rkmpp VPU HW encoder, falls back to libx264.
 * HW path uses AV_PIX_FMT_DRM_PRIME for DMA-BUF fd zero-copy;
 * SW path uses AV_PIX_FMT_NV12 with virtual address. B-frames disabled for
 * low-latency live streaming. SPS/PPS exported to extradata for RTSP SDP.
 */
video_encoder_t *video_encoder_open(const video_encoder_config_t *config)
{
    if (!config) return NULL;

    video_encoder_t *ctx = (video_encoder_t *)calloc(1, sizeof(video_encoder_t));
    if (!ctx) return NULL;

    ctx->src_width  = config->width;
    ctx->src_height = config->height;
    ctx->opened     = false;

    /* Prefer config->codec_name, default h264_rkmpp VPU HW encoder */
    const char *enc_name = config->codec_name ? config->codec_name : "h264_rkmpp";
    const AVCodec *codec = avcodec_find_encoder_by_name(enc_name);
    if (codec) {
        ctx->is_hw_encoder = true;
        fprintf(stdout, "[VideoEncoder] using encoder: %s (VPU HW, NV12 DMA-BUF zero-copy)\n", enc_name);
    } else {
        /* Fall back to libx264 SW H264 encoder */
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

    /* bit_rate: external config has priority, otherwise auto-calc via bpp=0.10
     * (width*height*fps*0.10, standard surveillance quality) */
    ctx->codec_ctx->bit_rate     = config->bit_rate > 0 ? config->bit_rate
                                   : config->width * config->height * config->fps * 0.10;
    ctx->codec_ctx->width        = config->width;
    ctx->codec_ctx->height       = config->height;
    /* time_base = 1/fps; framerate for rate control / frame prediction */
    ctx->codec_ctx->time_base    = (AVRational){1, config->fps};
    ctx->codec_ctx->framerate    = (AVRational){config->fps, 1};
    ctx->codec_ctx->gop_size     = config->gop_size > 0 ? config->gop_size : 30;
    /* Disable B-frames for low-latency live streaming */
    ctx->codec_ctx->max_b_frames = 0;

    if (ctx->is_hw_encoder) {
        /* HW: AV_PIX_FMT_DRM_PRIME, AVFrame passes DMA-BUF fd via data[0];
         * VPU imports physical memory via DRM PRIME, zero CPU copy */
        ctx->codec_ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    } else {
        /* SW: AV_PIX_FMT_NV12, normal virtual address YUV data */
        ctx->codec_ctx->pix_fmt = AV_PIX_FMT_NV12;
        /* libx264 low-latency tuning: ultrafast + zerolatency */
        av_opt_set(ctx->codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "tune", "zerolatency", 0);
    }

    /* GLOBAL_HEADER: export SPS/PPS to extradata for RTSP SDP
     * (sprop-parameter-sets). Without it, SPS/PPS are only embedded in the
     * first I-frame and some players cannot initialize the decoder. */
    ctx->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(ctx->codec_ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "[VideoEncoder] avcodec_open2 failed (encoder: %s)\n",
                ctx->is_hw_encoder ? "h264_rkmpp" : "libx264");
        avcodec_free_context(&ctx->codec_ctx);
        free(ctx);
        return NULL;
    }

    /* Allocate AVFrame: set size/format only, do NOT pre-allocate buffer.
     * HW: external fills DMA-BUF fd; SW: external binds V4L2 mmap virtual
     * address. Zero-copy. */
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

    /* HW encoder: pre-allocate AVDRMFrameDescriptor once.
     * Format/plane/offset/pitch/size are fixed at open time; only fd changes
     * per frame. Allocated once here and reused for every frame. */
    if (ctx->is_hw_encoder)
    {
        AVDRMFrameDescriptor *desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
        if (!desc)
        {
            fprintf(stderr, "[VideoEncoder] av_mallocz DRM desc failed\n");
            video_encoder_close(&ctx);
            return NULL;
        }

        /* Single DMA buffer object holds the whole NV12 image */
        desc->nb_objects = 1;
        /* NV12: Y plane w*h, UV plane w*h/2, total = width*height*3/2 */
        desc->objects[0].size             = config->width * config->height * 3 / 2;
        desc->objects[0].format_modifier  = DRM_FORMAT_MOD_LINEAR;

        desc->nb_layers = 1;
        desc->layers[0].format            = DRM_FORMAT_NV12;
        /* NV12 has 2 planes: Y plane + interleaved UV plane */
        desc->layers[0].nb_planes         = 2;

        /* plane[0]: Y plane */
        desc->layers[0].planes[0].object_index = 0;
        desc->layers[0].planes[0].offset       = 0;
        desc->layers[0].planes[0].pitch        = config->width;

        /* plane[1]: UV interleaved plane, shares the same DMA object as Y */
        desc->layers[0].planes[1].object_index = 0;
        desc->layers[0].planes[1].offset       = config->width * config->height;
        desc->layers[0].planes[1].pitch        = config->width;

        /* Wrap desc into AVBufferRef so each frame only adds a reference.
         * av_frame_unref decrements refcount; desc memory is freed only when
         * all references are released. Wrapping avoids double-free on reuse. */
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

/* Encode one NV12 frame -> H264.
 * HW path: nv12_data is the mmap'd virtual address of the DMA buffer, bound
 *   directly to AVFrame->data[] (zero-copy). Distinct from encode_nv12_fd
 *   which uses AV_PIX_FMT_DRM_PRIME + fd.
 * SW path: copy external nv12_data into AVFrame buffer before libx264 encode.
 * Returns: 1=got packet, 0=EAGAIN (need more frames), -1=error. */
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

    /* Capture-time PTS, passed through to the output packet */
    ctx->frame->pts = pts;

    ret = avcodec_send_frame(ctx->codec_ctx, ctx->frame);
    if (ret < 0) return -1;

    ret = avcodec_receive_packet(ctx->codec_ctx, ctx->pkt);
    if (ret == 0) { *out_pkt = ctx->pkt; return 1; }
    if (ret == AVERROR(EAGAIN)) return 0;
    return -1;
}



/* Encode one NV12 frame via DMA-BUF fd (DRM-PRIME zero-copy HW path).
 * Requires ctx->drm_desc pre-allocated at open time; only fd changes per frame.
 * VPU reads DMA-BUF physical memory directly via fd, no CPU pixel access.
 * Returns: 1=got packet, 0=EAGAIN, -1=error. */
int video_encoder_encode_nv12_fd(video_encoder_t *ctx,
                                  int fd, int data_size,
                                  int64_t pts, AVPacket **out_pkt)
{
    if (!ctx || !ctx->opened || fd < 0 || !ctx->drm_desc)
        return -1;

    /* Release previous frame's reference; refcount--, desc stays alive */
    av_frame_unref(ctx->frame);

    /* Only fd changes per frame; layout fields set once at open */
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)ctx->drm_desc;
    desc->objects[0].fd = fd;

    /* Take a new reference on drm_buf for this frame. av_frame_unref only
     * decrements refcount; desc memory survives until all refs released. */
    ctx->frame->buf[0]  = av_buffer_ref((AVBufferRef *)ctx->drm_buf);

    /* DRM-PRIME convention: data[0] holds the AVDRMFrameDescriptor pointer,
     * not pixel memory. FFmpeg reads desc to obtain fd/plane/offset/pitch. */
    ctx->frame->data[0] = (uint8_t *)desc;

    ctx->frame->format  = AV_PIX_FMT_DRM_PRIME;
    ctx->frame->width   = ctx->src_width;
    ctx->frame->height  = ctx->src_height;

    /* Capture-time PTS, passed through to the output packet */
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
        /* send_frame succeeded but no packet yet (internal buffering);
         * caller should poll receive_packet again */
        return 0;
    }
    return -1;
}

/* Flush encoder: drain remaining buffered frames (B-frames etc.) */
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
