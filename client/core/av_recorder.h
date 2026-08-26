#ifndef AV_RECORDER_H
#define AV_RECORDER_H

#include "video_encoder.h"
#include "audio_encoder.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct AVFormatContext AVFormatContext;
typedef struct AVStream AVStream;


typedef struct {
    char     filepath[256];    
    int      width;            
    int      height;           
    int      fps;              
    int      video_bit_rate;   
    int      audio_sample_rate;
    int      audio_channels;   
    int      audio_bit_rate;   
    const char *container_fmt; 
} av_recorder_config_t;


typedef struct {
    
    AVFormatContext *fmt_ctx;

    
    AVStream        *video_st;
    video_encoder_t *video_encoder;
    int64_t          video_pts;

    
    AVStream        *audio_st;
    audio_encoder_t *audio_encoder;
    int64_t          audio_pts;

    
    bool             header_written;
    bool             recording;
    av_recorder_config_t config;
} av_recorder_t;


av_recorder_t *av_recorder_open(const av_recorder_config_t *config);


int av_recorder_write_video_frame(av_recorder_t *ctx,
                                   const uint8_t *bgr_data, int data_size);


int av_recorder_write_audio_samples(av_recorder_t *ctx,
                                     const uint8_t *pcm_data, int data_size);


void av_recorder_close(av_recorder_t **ctx);

#ifdef __cplusplus
}
#endif

#endif /* AV_RECORDER_H */
