#ifndef RTSP_STREAMER_H
#define RTSP_STREAMER_H

#include "video_encoder.h"
#include "audio_encoder.h"
#include "h264_parser.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FFmpeg forward declarations */
typedef struct AVFormatContext AVFormatContext;
typedef struct AVStream AVStream;

/** @brief RTSP streamer config */
typedef struct {
    char     rtsp_url[256];     /* RTSP URL, e.g. "rtsp://server:8554/live/cam01" */
    int      width;
    int      height;
    int      fps;
    int      video_bit_rate;    /* video bitrate (bps) */
    int      audio_sample_rate; /* audio sample rate (0 = video only) */
    int      audio_channels;
    int      audio_bit_rate;
    bool     use_tcp;           /* RTSP transport: true=TCP(reliable), false=UDP(low latency) */
    int64_t  base_time_ms;      /* PTS baseline (ms); 0=auto CLOCK_MONOTONIC; nonzero=reuse on reconnect to avoid PTS cliff */
} rtsp_streamer_config_t;

/** @brief RTSP streamer context */
typedef struct {
    AVFormatContext *fmt_ctx;          /* RTSP muxer context (RTP muxer) */
    AVStream        *video_st;
    video_encoder_t *video_encoder;    /* video encoder (VPU HW preferred) */
    AVStream        *audio_st;
    audio_encoder_t *audio_encoder;    /* audio encoder (AAC + SwrContext) */
    bool             connected;
    rtsp_streamer_config_t config;
    h264_parser_t   *h264_parser;      /* H264 stream stats */
    pthread_mutex_t  write_mutex;      /* protects av_interleaved_write_frame (A/V threads concurrent) */
    int64_t          start_time_ms;    /* stream start monotonic time (ms) for A/V PTS calibration */
} rtsp_streamer_t;


/** @brief Connect to RTSP server and initialize the streamer */
rtsp_streamer_t *rtsp_streamer_open(const rtsp_streamer_config_t *config);

/**
 * @brief Push one NV12 frame via DMA-BUF fd (HW zero-copy: fd -> h264_rkmpp VPU -> RTSP)
 * @return -1=failed, 0=sent, 1=encoder buffered, no output yet
 */
int rtsp_streamer_send_nv12(rtsp_streamer_t *ctx, int fd, int data_size);

/**
 * @brief Push one NV12 frame via virtual address pointer (SW libx264 fallback)
 * @return -1=failed, 0=sent, 1=encoder buffered, no output yet
 */
int rtsp_streamer_send_nv12_ptr(rtsp_streamer_t *ctx, const void *nv12_data, int data_size);

/**
 * @brief Push audio (S16LE PCM -> AAC -> RTP)
 */
int rtsp_streamer_send_audio(rtsp_streamer_t *ctx,
                              const uint8_t *pcm_data, int data_size);

/**
 * @brief Disconnect RTSP and release resources
 */
void rtsp_streamer_close(rtsp_streamer_t **ctx);

/**
 * @brief Get H264 stream statistics
 */
void rtsp_streamer_get_h264_stats(const rtsp_streamer_t *ctx, h264_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* RTSP_STREAMER_H */
