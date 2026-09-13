#ifndef VIDEO_ENCODER_H
#define VIDEO_ENCODER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FFmpeg forward declarations */
typedef struct AVCodecContext AVCodecContext;
typedef struct AVFrame AVFrame;
typedef struct AVPacket AVPacket;

/**
 * @brief Video encoder config
 */
typedef struct {
    int      width;
    int      height;
    int      fps;
    int      gop_size;       /* keyframe interval (GOP size) */
    int      bit_rate;       /* bitrate (bps), 0 = use default CRF mode */
    int      crf;            /* CRF quality (0-51, lower = better; 23-28 recommended) */
    const char *codec_name;  /* encoder name: "h264_rkmpp" (VPU HW) */
} video_encoder_config_t;

/**
 * @brief Video encoder context
 */
typedef struct {
    AVCodecContext  *codec_ctx;
    AVFrame         *frame;        /* encode frame buffer */
    AVPacket        *pkt;          /* encode output packet */
    int              src_width;
    int              src_height;
    bool             opened;
    bool             is_hw_encoder;/* true=h264_rkmpp (DMA-BUF fd zero-copy), false=libx264 (virtual address) */
    /* HW-only: AVDRMFrameDescriptor layout is fixed, only fd changes per frame */
    void            *drm_desc;     /* AVDRMFrameDescriptor*, allocated in open, freed in close */
    void            *drm_buf;      /* AVBufferRef*, manages drm_desc lifetime */
} video_encoder_t;

/**
 * @brief Initialize the video encoder
 * @return encoder context, NULL on failure
 * @note codec_name="h264_rkmpp" uses VPU HW + DMA-BUF zero-copy (needs librockchip_mpp)
 */
video_encoder_t *video_encoder_open(const video_encoder_config_t *config);

/**
 * @brief Encode one NV12 frame (V4L2 native format, zero-copy)
 * @param pts capture-time PTS, passed through to output packet
 */
int video_encoder_encode_nv12(video_encoder_t *ctx,
                               const uint8_t *nv12_data, int data_size,
                               int64_t pts, AVPacket **out_pkt);

/**
 * @brief Encode one NV12 DMA-BUF fd (V4L2 fd -> VPU direct read, zero-copy)
 * @param pts capture-time PTS, passed through to output packet
 */
int video_encoder_encode_nv12_fd(video_encoder_t *ctx,
                                  int fd, int data_size,
                                  int64_t pts, AVPacket **out_pkt);

/**
 * @brief Drain remaining buffered frames
 * @return number of packets produced
 */
int video_encoder_flush(video_encoder_t *ctx, AVPacket **out_pkts, int max_pkts);

/** @brief Close the encoder and release resources */
void video_encoder_close(video_encoder_t **ctx);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_ENCODER_H */
