#ifndef MP4_RECORDER_H
#define MP4_RECORDER_H

#include "h264_parser.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif




typedef struct AVFormatContext AVFormatContext;
typedef struct AVStream AVStream;
typedef struct AVCodecContext AVCodecContext;
typedef struct SwrContext SwrContext;


typedef enum {
    RECORDER_IDLE = 0,       
    RECORDER_RECORDING,      
    RECORDER_PAUSED,         
    RECORDER_ERROR           
} recorder_state_t;


typedef struct {
    char     output_path[256];    
    int      width;               
    int      height;              
    int      fps;                 
    int      video_bit_rate;      
    int      gop_size;            
    int      audio_sample_rate;   
    int      audio_channels;      
    int      audio_bit_rate;      
    bool     enable_audio;        
    bool     faststart;           
    int      max_duration_sec;    
} mp4_recorder_config_t;


typedef struct {
    
    mp4_recorder_config_t config;
    recorder_state_t      state;

    
    AVFormatContext  *fmt_ctx;         
    AVStream         *video_st;        
    AVCodecContext   *video_codec_ctx; 
    AVStream         *audio_st;        
    AVCodecContext   *audio_codec_ctx; 
    SwrContext       *swr_ctx;         

    
    int64_t   video_pts;          
    int64_t   audio_pts;          
    int64_t   first_video_dts;    
    int64_t   first_audio_dts;    
    bool      has_video;          
    bool      has_audio;          

    
    int64_t   audio_clock;        
    int64_t   video_clock;        
    int64_t   sync_threshold_us;  
    int       video_drop_count;   
    int       video_dup_count;    

    
    h264_parser_t *h264_parser;

    
    int64_t   start_time_ms;      
    int64_t   total_video_frames; 
    int64_t   total_audio_frames; 
    int64_t   total_bytes;        
} mp4_recorder_t;


mp4_recorder_t* mp4_recorder_open(const mp4_recorder_config_t *config);


void mp4_recorder_close(mp4_recorder_t **ctx);


int mp4_recorder_write_nv12(mp4_recorder_t *ctx, const uint8_t *nv12_data, int size);


int mp4_recorder_write_bgr(mp4_recorder_t *ctx, const uint8_t *bgr_data, int size);


int mp4_recorder_write_audio(mp4_recorder_t *ctx, const int16_t *pcm_data, int samples);


void mp4_recorder_pause(mp4_recorder_t *ctx);


void mp4_recorder_resume(mp4_recorder_t *ctx);


double mp4_recorder_get_duration(const mp4_recorder_t *ctx);


int64_t mp4_recorder_get_file_size(const mp4_recorder_t *ctx);


void mp4_recorder_get_h264_stats(const mp4_recorder_t *ctx, h264_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* MP4_RECORDER_H */
