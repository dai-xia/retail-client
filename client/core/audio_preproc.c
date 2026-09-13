/**
 * @file audio_preproc.c
 * @brief Audio preprocessing pipeline: AEC + NS + AGC + VAD
 */

#include "audio_preproc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include "rknn_api.h"

/* Silero VAD input: 512 samples, tensor [1,1,1,512] float32 in [-1,1]; output = speech prob */
#define SILERO_VAD_INPUT_SIZE 512

struct audio_preproc_ctx {
    /* Configuration parameters (copied at creation) */
    audio_preproc_config_t config;

    /* AEC: NLMS adaptive filter; filter_length = sample_rate * tail_ms / 1000, must be a multiple of frame_size */
    SpeexEchoState *echo_state;

    /* NS + AGC: SpeexPreprocessState; NS = Wiener denoise (-30dB), AGC target level 8000 */
    SpeexPreprocessState *preproc_state;

    /* RKNN Silero VAD: 512-sample ring buffer, NPU inference, speech prob 0..1 */
    rknn_context  rknn_ctx;
    int           rknn_ready;
    unsigned char *model_data;
    int           model_size;
    float         vad_buf[SILERO_VAD_INPUT_SIZE];
    int           vad_buf_pos;
    int           vad_buf_count;

    float speech_prob;
};

/* Defaults: 16kHz mono, 480-sample frames, all stages on, VAD threshold 0.5 */
static void fill_default_config(audio_preproc_config_t *cfg)
{
    cfg->sample_rate   = 16000;
    cfg->channels      = 1;
    cfg->frame_size    = 480;
    cfg->enable_aec    = true;
    cfg->enable_ns     = true;
    cfg->enable_agc    = true;
    cfg->enable_vad    = true;
    cfg->vad_threshold = 0.5f;
    cfg->vad_rknn_path = NULL;
}

/* Load Silero VAD RKNN model from file into memory, then rknn_init */
static int load_rknn_model(audio_preproc_t *ctx, const char *path)
{
    if (!path)
        return -1;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[audio_preproc] Cannot open VAD model: %s\n", path);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    ctx->model_size = (int)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (ctx->model_size <= 0) {
        fclose(fp);
        return -1;
    }

    ctx->model_data = (unsigned char *)malloc(ctx->model_size);
    if (!ctx->model_data) {
        fclose(fp);
        ctx->model_size = 0;
        return -1;
    }

    if ((int)fread(ctx->model_data, 1, ctx->model_size, fp) != ctx->model_size) {
        free(ctx->model_data);
        ctx->model_data = NULL;
        ctx->model_size = 0;
        fclose(fp);
        return -1;
    }
    fclose(fp);

    int ret = rknn_init(&ctx->rknn_ctx, ctx->model_data, ctx->model_size, 0, NULL);
    if (ret < 0) {
        fprintf(stderr, "[audio_preproc] RKNN init failed: ret=%d\n", ret);
        free(ctx->model_data);
        ctx->model_data = NULL;
        ctx->model_size = 0;
        return -1;
    }

    ctx->rknn_ready = 1;
    return 0;
}

/* int16 -> float [-1,1] -> ring buffer -> NPU inference at 512 samples -> speech prob */
static float run_rknn_vad(audio_preproc_t *ctx, const int16_t *frame, int frame_size)
{
    if (!ctx->rknn_ready)
        return 0.0f;

    for (unsigned int i = 0; i < (unsigned int)frame_size && ctx->vad_buf_count < SILERO_VAD_INPUT_SIZE; i++) {
        ctx->vad_buf[ctx->vad_buf_pos] = (float)frame[i] / 32768.0f;
        ctx->vad_buf_pos++;
        ctx->vad_buf_count++;
    }

    if (ctx->vad_buf_count < SILERO_VAD_INPUT_SIZE)
        return ctx->speech_prob;

    rknn_input rknn_in;
    memset(&rknn_in, 0, sizeof(rknn_in));
    rknn_in.index = 0;
    rknn_in.buf = ctx->vad_buf;
    rknn_in.size = SILERO_VAD_INPUT_SIZE * sizeof(float);
    rknn_in.pass_through = 0;
    rknn_in.type = RKNN_TENSOR_FLOAT32;
    rknn_in.fmt = RKNN_TENSOR_NHWC;

    int ret = rknn_inputs_set(ctx->rknn_ctx, 1, &rknn_in);
    if (ret < 0) {
        ctx->vad_buf_pos = 0;
        ctx->vad_buf_count = 0;
        return ctx->speech_prob;
    }

    ret = rknn_run(ctx->rknn_ctx, NULL);
    if (ret < 0) {
        ctx->vad_buf_pos = 0;
        ctx->vad_buf_count = 0;
        return ctx->speech_prob;
    }

    rknn_output rknn_out;
    memset(&rknn_out, 0, sizeof(rknn_out));
    rknn_out.index = 0;
    rknn_out.want_float = 1;

    ret = rknn_outputs_get(ctx->rknn_ctx, 1, &rknn_out, NULL);
    if (ret < 0) {
        ctx->vad_buf_pos = 0;
        ctx->vad_buf_count = 0;
        return ctx->speech_prob;
    }

    float prob = 0.0f;
    float *out_buf = (float *)rknn_out.buf;
    if (out_buf) {
        prob = out_buf[0];
        if (prob < 0.0f) prob = 0.0f;
        if (prob > 1.0f) prob = 1.0f;
    }

    rknn_outputs_release(ctx->rknn_ctx, 1, &rknn_out);

    /* keep last frame_size samples as the next frame start (overlap) */
    int overlap = (SILERO_VAD_INPUT_SIZE > frame_size) ? frame_size : 0;
    if (overlap > 0) {
        memmove(ctx->vad_buf,
                ctx->vad_buf + SILERO_VAD_INPUT_SIZE - overlap,
                overlap * sizeof(float));
        ctx->vad_buf_pos = overlap;
        ctx->vad_buf_count = overlap;
    } else {
        ctx->vad_buf_pos = 0;
        ctx->vad_buf_count = 0;
    }

    return prob;
}

/* Create context. AEC filter_length = sample_rate*100/1000 (100ms tail), aligned to a multiple of frame_size */
audio_preproc_t* audio_preproc_create(const audio_preproc_config_t *config)
{
    audio_preproc_t *ctx = (audio_preproc_t *)calloc(1, sizeof(audio_preproc_t));
    if (!ctx)
        return NULL;

    if (config) {
        ctx->config = *config;
    } else {
        fill_default_config(&ctx->config);
    }

    if (ctx->config.sample_rate == 0 || ctx->config.frame_size == 0 || ctx->config.channels == 0) {
        fprintf(stderr, "[audio_preproc] Invalid params: rate=%u channels=%u frame_size=%u\n",
                ctx->config.sample_rate, ctx->config.channels, ctx->config.frame_size);
        free(ctx);
        return NULL;
    }

    ctx->speech_prob = 0.0f;

    /* AEC filter_length: 100ms tail, aligned to a multiple of frame_size */
    if (ctx->config.enable_aec) {
        int filter_length = (int)(ctx->config.sample_rate * 100 / 1000);
        filter_length = (filter_length / (int)ctx->config.frame_size) * (int)ctx->config.frame_size;
        if (filter_length < (int)ctx->config.frame_size)
            filter_length = (int)ctx->config.frame_size;

        ctx->echo_state = speex_echo_state_init_mc(
            (int)ctx->config.frame_size,
            filter_length,
            (int)ctx->config.channels,
            (int)ctx->config.channels
        );

        if (!ctx->echo_state) {
            fprintf(stderr, "[audio_preproc] AEC init failed\n");
            free(ctx);
            return NULL;
        }
    } else {
        ctx->echo_state = NULL;
    }

    /* NS + AGC (SpeexPreprocessState) */
    if (ctx->config.enable_ns || ctx->config.enable_agc ||
        (ctx->config.enable_vad && !ctx->config.vad_rknn_path)) {
        ctx->preproc_state = speex_preprocess_state_init(
            (int)ctx->config.frame_size,
            (int)ctx->config.sample_rate
        );

        if (!ctx->preproc_state) {
            fprintf(stderr, "[audio_preproc] SpeexPreprocess init failed\n");
            if (ctx->echo_state)
                speex_echo_state_destroy(ctx->echo_state);
            free(ctx);
            return NULL;
        }

        if (ctx->config.enable_ns) {
            int enabled = 1;
            speex_preprocess_ctl(ctx->preproc_state, SPEEX_PREPROCESS_SET_DENOISE, &enabled);
            /* NS level -30dB */
            int noise_suppress = -30;
            speex_preprocess_ctl(ctx->preproc_state, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_suppress);
        }

        if (ctx->config.enable_agc) {
            int enabled = 1;
            speex_preprocess_ctl(ctx->preproc_state, SPEEX_PREPROCESS_SET_AGC, &enabled);
            /* AGC target 8000 (~1/4 full scale) */
            int agc_level = 8000;
            speex_preprocess_ctl(ctx->preproc_state, SPEEX_PREPROCESS_SET_AGC_LEVEL, &agc_level);
        }

        /* energy VAD only when no RKNN model */
        if (ctx->config.enable_vad && !ctx->config.vad_rknn_path) {
            int enabled = 1;
            speex_preprocess_ctl(ctx->preproc_state, SPEEX_PREPROCESS_SET_VAD, &enabled);
        }
    } else {
        ctx->preproc_state = NULL;
    }

    ctx->rknn_ctx = 0;
    ctx->rknn_ready = 0;
    ctx->model_data = NULL;
    ctx->model_size = 0;
    ctx->vad_buf_pos = 0;
    ctx->vad_buf_count = 0;

    if (ctx->config.enable_vad && ctx->config.vad_rknn_path) {
        if (load_rknn_model(ctx, ctx->config.vad_rknn_path) != 0) {
            /* downgrade to energy VAD on failure */
            fprintf(stderr, "[audio_preproc] RKNN VAD model loading failed, downgrading to SpeexDSP energy VAD\n");

            if (ctx->preproc_state) {
                int enabled = 1;
                speex_preprocess_ctl(ctx->preproc_state, SPEEX_PREPROCESS_SET_VAD, &enabled);
            }
        }
    }

    return ctx;
}

/* Release RKNN -> SpeexPreprocess -> SpeexEcho -> ctx (reverse of create) */
void audio_preproc_destroy(audio_preproc_t **ctx)
{
    if (!ctx || !*ctx)
        return;

    audio_preproc_t *c = *ctx;

    if (c->rknn_ready && c->rknn_ctx) {
        rknn_destroy(c->rknn_ctx);
        c->rknn_ctx = 0;
        c->rknn_ready = 0;
    }
    if (c->model_data) {
        free(c->model_data);
        c->model_data = NULL;
    }
    c->model_size = 0;

    if (c->preproc_state) {
        speex_preprocess_state_destroy(c->preproc_state);
        c->preproc_state = NULL;
    }
    if (c->echo_state) {
        speex_echo_state_destroy(c->echo_state);
        c->echo_state = NULL;
    }

    free(c);
    *ctx = NULL;
}

/* mic + ref -> AEC -> NS/AGC -> VAD -> out (in-place ok; ref_frame may be NULL) */
audio_vad_result_t audio_preproc_process(audio_preproc_t *ctx,
                                          const int16_t *mic_frame,
                                          const int16_t *ref_frame,
                                          int16_t *out_frame)
{
    if (!ctx || !mic_frame || !out_frame)
        return AUDIO_VAD_SILENCE;

    unsigned int frame_size = ctx->config.frame_size;
    audio_vad_result_t vad_result = AUDIO_VAD_SPEECH;

    /* AEC: cancel speaker echo using ref_frame; passthrough if no ref */
    if (ctx->config.enable_aec && ctx->echo_state) {
        if (ref_frame) {
            speex_echo_cancellation(ctx->echo_state, mic_frame, ref_frame, out_frame);
        } else {
            if (mic_frame != out_frame)
                memcpy(out_frame, mic_frame, frame_size * sizeof(int16_t));
        }
    } else {
        if (mic_frame != out_frame)
            memcpy(out_frame, mic_frame, frame_size * sizeof(int16_t));
    }

    /* NS + AGC in one speex_preprocess_run */
    if (ctx->preproc_state) {
        int speex_vad = speex_preprocess_run(ctx->preproc_state, out_frame);

        /* SpeexDSP energy VAD fallback */
        if (ctx->config.enable_vad && !ctx->config.vad_rknn_path) {
            ctx->speech_prob = speex_vad ? 1.0f : 0.0f;
            vad_result = speex_vad ? AUDIO_VAD_SPEECH : AUDIO_VAD_SILENCE;
        }
    }

    /* RKNN Silero VAD: prob >= threshold -> SPEECH */
    if (ctx->config.enable_vad && ctx->rknn_ready) {
        float prob = run_rknn_vad(ctx, out_frame, (int)frame_size);
        ctx->speech_prob = prob;
        vad_result = (prob >= ctx->config.vad_threshold) ? AUDIO_VAD_SPEECH : AUDIO_VAD_SILENCE;
    }

    return vad_result;
}

/* last frame speech probability (0..1); RKNN=continuous, Speex=0/1, VAD off=1.0 */
float audio_preproc_get_speech_prob(const audio_preproc_t *ctx)
{
    if (!ctx)
        return 1.0f;
    return ctx->speech_prob;
}
