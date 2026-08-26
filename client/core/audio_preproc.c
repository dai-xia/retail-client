/**
 * @file audio_preproc.c
 * @brief Audio preprocessing pipeline: AEC + NS + AGC + VAD
 *
 * Sits between ALSA capture and Vosk recognition, providing far-field voice interaction for noisy retail environments:
 *
 *   ALSA mic_frame --> AEC(echo cancellation) --> NS(noise suppression)+AGC(gain) --> VAD(endpoint detection) --> Vosk
 *                         ^                                        |
 *                    speaker ref_frame                            speech/silence decision
 *
 * Processing chain order:
 *   mic_frame + ref_frame -> AEC -> NS+AGC (SpeexPreprocess) -> VAD -> out_frame
 *
 * Dependencies (all optional, degrades to passthrough when missing):
 *   - SpeexDSP: AEC/NS/AGC/Energy-VAD (lightweight C library, no external dependencies)
 *   - RKNN: Silero VAD model (NPU accelerated, high-precision endpoint detection)
 *
 * Compile macros:
 *   - USE_SPEEXDSP: enable SpeexDSP AEC/NS/AGC/Energy-VAD
 *   - USE_RKNN:     enable RKNN NPU inference for Silero VAD
 *   - Neither defined: module compiles as passthrough (memcpy mic->out, VAD=SPEECH)
 */

#include "audio_preproc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include "rknn_api.h"

/* ========== Constants ========== */

/**
 * Silero VAD model input length (fixed 512 samples)
 *
 * Silero VAD is a deep-learning based endpoint detection model, training uses fixed input window 30ms@16kHz=480 samples,
 * but RKNN export usually pads to 512 (power of 2, NPU alignment optimization).
 * Model input tensor shape: [1, 1, 1, 512], float32, normalized to [-1, 1]
 * Output: single float probability value, indicating the probability that the current frame contains speech
 */
#define SILERO_VAD_INPUT_SIZE 512

/* ========== Internal structure definitions ========== */

struct audio_preproc_ctx {
    /* Configuration parameters (copied at creation) */
    audio_preproc_config_t config;

    /*
     * AEC (Acoustic Echo Cancellation) context
     *
     * AEC principle: adaptive filter cancels echo
     *   In retail scenarios, the device speaker plays prompts/ads, and the microphone picks up the speaker's sound feedback,
     *   AEC estimates the echo path via the reference signal (ref_frame, i.e. the signal sent to the speaker),
     *   subtracts the estimated echo from the microphone signal, keeping only the local speech.
     *
     *   Core algorithm: NLMS (Normalized Least Mean Squares) adaptive filter
     *     mic_echo = H(z) * ref_frame     (H is the room impulse response)
     *     clean    = mic_frame - mic_echo  (echo cancellation)
     *     H(z)     approaches the true room response via NLMS iteration
     *
     *   filter_length parameter:
     *     = sample_rate * tail_length / 1000
     *     represents the order of the adaptive filter, determines how long the echo tail component can be cancelled
     *     Typical retail environment reverberation is about 100ms, so filter_length = sample_rate * 100 / 1000
     *     Note: filter_length must be an integer multiple of frame_size
     */
    SpeexEchoState *echo_state;

    /*
     * NS + AGC shared preprocessing context
     *
     * SpeexPreprocessState simultaneously provides:
     *   - NS (Noise Suppression): frequency-domain denoising
     *   - AGC (Automatic Gain Control): automatic gain control
     *   - VAD (Voice Activity Detection): energy endpoint detection
     *
     * NS principle: frequency-domain statistical denoising
     *   1. FFT each frame to get frequency-domain representation
     *   2. Estimate noise spectrum: sliding average over silence segments, get noise power per frequency band
     *   3. Wiener filter: gain(f) = max(1 - noise_power/signal_power, floor)
     *      When SNR is low for a band, gain approaches 0 (strong suppression); when SNR is high, gain approaches 1 (weak suppression)
     *   4. IFFT to restore time-domain signal
     *   Suppression level SPEEX_PREPROCESS_SET_NOISE_SUPPRESS unit is dB, larger negative value means stronger suppression
     *   Default -30dB, sufficient for noisy retail environment
     *
     * AGC principle: automatic gain control
     *   Dynamically adjusts gain based on signal energy, keeping output at target level:
     *   - Low speaking volume -> increase gain
     *   - High speaking volume -> decrease gain
     *   Target level SPEEX_PREPROCESS_SET_AGC_LEVEL default 8000
     *   (16-bit PCM range -32768~32767, 8000 is about 1/4 of full scale, suitable for speech)
     *
     * VAD principle: energy endpoint detection
     *   Judges whether the current frame contains speech based on short-time energy and zero-crossing rate:
     *   - Frame energy above noise floor -> speech
     *   - Frame energy below noise floor -> silence
     *   speex_preprocess_run() return value: 0=silence, 1=speech
     *   Pros: very small computation; Cons: high false positive rate in high-noise environments
     */
    SpeexPreprocessState *preproc_state;

    /*
     * RKNN Silero VAD inference context
     *
     * Silero VAD principle: deep-learning endpoint detection
     *   Based on a deep neural network (similar to the Silero team's model architecture), inputs 512 normalized audio samples,
     *   outputs a 0~1 speech probability value. Compared to energy VAD:
     *   - Pros: robust to noise, significantly lower false positive rate in noisy retail environments
     *   - Cons: requires NPU inference, although about 1ms on RK3568, still has overhead
     *
     *   Inference flow:
     *     int16 audio -> float normalize [-1,1] -> RKNN input [1,1,1,512] -> NPU inference -> output probability
     *
     *   Ring buffer (vad_buf):
     *     Silero requires 512 samples as input, but default frame length is 480 samples.
     *     Use a ring buffer to accumulate samples: write frame_size samples per frame,
     *     when 512 are accumulated, trigger one RKNN inference, output probability.
     *     When fewer than 512, reuse the last VAD result.
     */
    rknn_context  rknn_ctx;
    int           rknn_ready;           /**< Flag indicating whether model loaded successfully */
    unsigned char *model_data;          /**< Model file memory buffer */
    int           model_size;           /**< Model file size (bytes) */
    float         vad_buf[SILERO_VAD_INPUT_SIZE]; /**< VAD input normalized buffer */
    int           vad_buf_pos;          /**< Current write position in ring buffer */
    int           vad_buf_count;        /**< Accumulated sample count in buffer */

    float speech_prob;                  /**< Speech probability of the last frame 0.0~1.0 */
};

/* ========== Default configuration ========== */

/**
 * @brief Fill default configuration parameters
 *
 * Default value rationale:
 *   - sample_rate=16000: Vosk requires 16kHz, also standard telephony sample rate
 *   - channels=1: mono, far-field pickup usually doesn't need stereo
 *   - frame_size=480: 30ms@16kHz, SpeexDSP recommended frame length (must be <= filter_length)
 *   - enable_aec/ns/agc/vad=true: all enabled for noisy retail environment
 *   - vad_threshold=0.5: Silero VAD probability threshold, 0.5 is a balanced value
 *   - vad_rknn_path=NULL: default uses SpeexDSP energy VAD, no RKNN model needed
 */
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

/* ========== RKNN model loading ========== */

/**
 * @brief Load Silero VAD RKNN model
 *
 * @param ctx Preprocessing context
 * @param path RKNN model file path
 * @return 0=success, -1=failure
 *
 * Model file reading flow (consistent with face_landmark):
 *   1. fopen reads file into memory
 *   2. rknn_init takes memory pointer (faster than passing path)
 *   3. First init about 50-100ms (NPU graph compilation)
 *
 * Silero VAD RKNN model specs:
 *   - Input: [1, 1, 1, 512] float32, normalized to [-1, 1]
 *   - Output: [1, 1] float32, speech probability [0, 1]
 *   - Model size about 1-2MB
 *   - RK3568 NPU inference about 1ms
 */
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

/**
 * @brief RKNN Silero VAD inference
 *
 * @param ctx Preprocessing context
 * @param frame Preprocessed audio frame (int16, frame_size samples)
 * @param frame_size Frame length
 * @return Speech probability 0.0~1.0, returns 0.0 on error
 *
 * Inference flow:
 *   1. int16 -> float normalize: sample_f = sample_i16 / 32768.0f
 *   2. Accumulate into ring buffer vad_buf
 *   3. When SILERO_VAD_INPUT_SIZE (512) samples are accumulated:
 *      a. Set RKNN input: [1,1,1,512] float32
 *      b. rknn_run executes inference
 *      c. rknn_outputs_get gets output probability
 *      d. Update speech_prob
 *      e. Reset buffer count, keep unconsumed samples (overlapping window)
 *   4. When not full, return last probability
 */
static float run_rknn_vad(audio_preproc_t *ctx, const int16_t *frame, int frame_size)
{
    if (!ctx->rknn_ready)
        return 0.0f;

    /* Normalize current frame int16 samples to float and write to ring buffer */
    for (unsigned int i = 0; i < (unsigned int)frame_size && ctx->vad_buf_count < SILERO_VAD_INPUT_SIZE; i++) {
        ctx->vad_buf[ctx->vad_buf_pos] = (float)frame[i] / 32768.0f;
        ctx->vad_buf_pos++;
        ctx->vad_buf_count++;
    }

    /* Buffer not full, return last probability */
    if (ctx->vad_buf_count < SILERO_VAD_INPUT_SIZE)
        return ctx->speech_prob;

    /* Buffer full, execute RKNN inference */
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
        /* Inference failed, reset buffer, return last probability */
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
        /* Clamp to [0, 1] range to prevent abnormal model output values */
        if (prob < 0.0f) prob = 0.0f;
        if (prob > 1.0f) prob = 1.0f;
    }

    rknn_outputs_release(ctx->rknn_ctx, 1, &rknn_out);

    /* Reset buffer: keep the last (frame_size) samples as the start of the next frame (50% overlap) */
    int overlap = (SILERO_VAD_INPUT_SIZE > frame_size) ? frame_size : 0;
    if (overlap > 0) {
        /* Move the last overlap samples to the head of the buffer */
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

/* ========== Public API implementation ========== */

/**
 * @brief Create audio preprocessing context
 *
 * @param config Configuration parameters, uses default values when NULL
 * @return Context pointer, NULL on failure
 *
 * Initialization flow:
 *   1. Copy/fill configuration
 *   2. Initialize AEC (SpeexEchoState): adaptive filter cancels speaker echo
 *   3. Initialize NS+AGC (SpeexPreprocessState): frequency-domain denoising + automatic gain
 *   4. Initialize VAD: load RKNN Silero model (optional) or enable SpeexDSP energy VAD
 *
 * AEC filter_length calculation:
 *   filter_length = sample_rate * 100 / 1000
 *   100ms tail length covers typical retail environment reverberation (reverberation time RT60 about 0.3~0.8s,
 *   adaptive filter only needs to cover early reflections, 100ms is enough)
 *   filter_length must be an integer multiple of frame_size, auto-aligned via integer division:
 *   actual filter_length = (sample_rate * 100 / 1000 / frame_size) * frame_size
 */
audio_preproc_t* audio_preproc_create(const audio_preproc_config_t *config)
{
    audio_preproc_t *ctx = (audio_preproc_t *)calloc(1, sizeof(audio_preproc_t));
    if (!ctx)
        return NULL;

    /* Copy configuration, use defaults when NULL */
    if (config) {
        ctx->config = *config;
    } else {
        fill_default_config(&ctx->config);
    }

    /* Parameter validity check */
    if (ctx->config.sample_rate == 0 || ctx->config.frame_size == 0 || ctx->config.channels == 0) {
        fprintf(stderr, "[audio_preproc] Invalid params: rate=%u channels=%u frame_size=%u\n",
                ctx->config.sample_rate, ctx->config.channels, ctx->config.frame_size);
        free(ctx);
        return NULL;
    }

    ctx->speech_prob = 0.0f;

    /*
     * Initialize AEC echo cancellation
     *
     * speex_echo_state_init_mc(frame_size, filter_length, mic_channels, ref_channels)
     *   - frame_size: samples per frame (480)
     *   - filter_length: adaptive filter length, must be an integer multiple of frame_size
     *   - mic_channels: microphone channel count
     *   - ref_channels: reference signal channel count
     *
     * filter_length alignment: ensure it's an integer multiple of frame_size
     *   Example: 16000*100/1000=1600, 1600/480=3.33 -> round down to 3 -> 3*480=1440
     */
    if (ctx->config.enable_aec) {
        int filter_length = (int)(ctx->config.sample_rate * 100 / 1000);
        /* Align to an integer multiple of frame_size */
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

    /*
     * Initialize NS+AGC preprocessing (SpeexPreprocessState)
     *
     * SpeexPreprocessState simultaneously provides NS/AGC/VAD,
     * sharing one state, one speex_preprocess_run call processes all.
     *
     * speex_preprocess_state_init(frame_size, sample_rate)
     *   Internally allocates FFT work buffers, frame_size should be a power of 2 or an integer multiple thereof
     *   480 = 2^5 * 3 * 5, SpeexDSP handles it internally
     */
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

        /* Enable NS denoising */
        if (ctx->config.enable_ns) {
            int enabled = 1;
            speex_preprocess_ctl(ctx->preproc_state, SPEEX_PREPROCESS_SET_DENOISE, &enabled);
            /*
             * Noise suppression level: -30dB
             * Negative value indicates maximum attenuation, -30 means noise is attenuated by up to 30dB
             * Retail environment background noise (AC/crowd/shelves) about 40-60dB SPL,
             * -30dB suppression can compress noise to imperceptible levels
             * More negative values (e.g. -40) suppress more but may damage speech
             */
            int noise_suppress = -30;
            speex_preprocess_ctl(ctx->preproc_state, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_suppress);
        }

        /* Enable AGC automatic gain control */
        if (ctx->config.enable_agc) {
            int enabled = 1;
            speex_preprocess_ctl(ctx->preproc_state, SPEEX_PREPROCESS_SET_AGC, &enabled);
            /*
             * AGC target level: 8000
             * 16-bit PCM range [-32768, 32767], 8000 is about 1/4 of full scale
             * AGC dynamically adjusts gain, bringing signal RMS close to the target level
             * 8000 is suitable for normal speech volume; far-field quiet speech can also be amplified to a recognizable level
             */
            int agc_level = 8000;
            speex_preprocess_ctl(ctx->preproc_state, SPEEX_PREPROCESS_SET_AGC_LEVEL, &agc_level);
        }

        /*
         * Enable SpeexDSP energy VAD (only when RKNN VAD is not used)
         *
         * If vad_rknn_path is configured, RKNN Silero VAD (more accurate) is used,
         * and SpeexDSP's VAD is not enabled to avoid conflict between the two.
         * If there is no RKNN model, SpeexDSP's energy/zero-crossing-rate based VAD is used as a fallback.
         */
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

    /* Load RKNN Silero VAD model (optional) */
    if (ctx->config.enable_vad && ctx->config.vad_rknn_path) {
        if (load_rknn_model(ctx, ctx->config.vad_rknn_path) != 0) {
            /*
             * RKNN model loading failed, downgrade to SpeexDSP energy VAD
             * Don't return NULL, because AEC/NS/AGC can still work normally
             */
            fprintf(stderr, "[audio_preproc] RKNN VAD model loading failed, downgrading to SpeexDSP energy VAD\n");

            if (ctx->preproc_state) {
                int enabled = 1;
                speex_preprocess_ctl(ctx->preproc_state, SPEEX_PREPROCESS_SET_VAD, &enabled);
            }
        }
    }

    return ctx;
}

/**
 * @brief Destroy audio preprocessing context
 *
 * @param ctx Address of context pointer (double pointer, set to NULL after destruction)
 *
 * Release order: RKNN -> SpeexPreprocess -> SpeexEcho -> context itself
 * Reverse of creation order, avoids depending on already released resources
 */
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

/**
 * @brief Process one frame of audio
 *
 * @param ctx       Preprocessing context
 * @param mic_frame Near-end microphone signal (frame_size int16_t samples)
 * @param ref_frame Far-end reference signal/speaker playback (only needed for AEC, can be NULL)
 * @param out_frame Output processed audio (frame_size int16_t samples, can be same as mic_frame)
 * @return VAD detection result: AUDIO_VAD_SPEECH or AUDIO_VAD_SILENCE
 *
 * Processing pipeline:
 *
 *   +----------------------------------------------------------+
 *   |  1. AEC: mic_frame + ref_frame -> echo_cancelled         |
 *   |     Echo cancellation: subtract estimated speaker echo    |
 *   |     from microphone signal                                |
 *   |     Needs ref_frame (reference signal), skips AEC if NULL |
 *   |                                                          |
 *   |  2. NS+AGC: echo_cancelled -> preprocessed               |
 *   |     Denoising + gain: one speex_preprocess_run call      |
 *   |     NS: frequency-domain Wiener filter denoising          |
 *   |     AGC: dynamic gain adjustment to target level          |
 *   |                                                          |
 *   |  3. VAD: preprocessed -> speech/silence decision          |
 *   |     Endpoint detection: determine if current frame       |
 *   |     contains speech                                       |
 *   |     Prefer RKNN Silero VAD (accurate)                    |
 *   |     Fallback to SpeexDSP energy VAD (lightweight)        |
 *   |                                                          |
 *   |  4. Output: out_frame <- preprocessed                   |
 *   +----------------------------------------------------------+
 *
 * In-place processing: out_frame can equal mic_frame; AEC uses internal temp buffer,
 * no read/write conflict. SpeexPreprocess also processes in place.
 */
audio_vad_result_t audio_preproc_process(audio_preproc_t *ctx,
                                          const int16_t *mic_frame,
                                          const int16_t *ref_frame,
                                          int16_t *out_frame)
{
    if (!ctx || !mic_frame || !out_frame)
        return AUDIO_VAD_SILENCE;

    unsigned int frame_size = ctx->config.frame_size;
    audio_vad_result_t vad_result = AUDIO_VAD_SPEECH; /* Default speech, always returned when VAD is disabled */

    /*
     * Step 1: AEC echo cancellation
     *
     * speex_echo_cancellation(echo_state, mic, ref, out)
     *   - mic: near-end signal (mixed signal picked up by microphone: local speech + echo)
     *   - ref: far-end signal (reference signal sent to speaker)
     *   - out: near-end signal after echo cancellation
     *
     * Note: skip AEC when ref_frame is NULL (pure denoising/gain scenario)
     *       But if AEC is enabled yet no ref_frame, copy mic directly to out
     *
     * Timing requirement: ref_frame must be time-aligned with mic_frame,
     *   i.e. ref_frame[n] corresponds to the signal played by the speaker at mic_frame[n]'s time.
     *   In practice, the playback thread synchronously provides ref_frame.
     */
    if (ctx->config.enable_aec && ctx->echo_state) {
        if (ref_frame) {
            speex_echo_cancellation(ctx->echo_state, mic_frame, ref_frame, out_frame);
        } else {
            /* AEC enabled but no reference signal, passthrough microphone data */
            if (mic_frame != out_frame)
                memcpy(out_frame, mic_frame, frame_size * sizeof(int16_t));
        }
    } else {
        /* AEC not enabled, passthrough */
        if (mic_frame != out_frame)
            memcpy(out_frame, mic_frame, frame_size * sizeof(int16_t));
    }

    /*
     * Step 2: NS denoising + AGC gain control
     *
     * speex_preprocess_run(preproc_state, frame)
     *   - Input/output: frame (in-place processing, both input and output)
     *   - Return value: SpeexDSP VAD result (0=silence, 1=speech)
     *     Only valid when SET_VAD is enabled, otherwise always returns 1
     *
     * NS+AGC completed in the same function call:
     *   NS: FFT -> estimate noise spectrum -> Wiener filter gain -> IFFT
     *   AGC: compute frame energy -> compare with target level -> adjust gain
     *   Both share FFT result, no need to recompute
     */
    if (ctx->preproc_state) {
        int speex_vad = speex_preprocess_run(ctx->preproc_state, out_frame);

        /*
         * Step 3a: SpeexDSP energy VAD (fallback)
         *
         * When RKNN model is not configured, uses speex_preprocess_run's return value as VAD decision:
         *   Returns 0 = silence (frame energy below noise floor)
         *   Returns 1 = speech (frame energy above noise floor)
         *
         * Energy VAD has high accuracy in quiet environments, but in noisy retail environments:
         *   - Continuous noise like AC/music easily misjudged as speech
         *   - Far-field faint speech easily misjudged as silence
         * So RKNN Silero VAD exists as a more accurate alternative
         */
        if (ctx->config.enable_vad && !ctx->config.vad_rknn_path) {
            ctx->speech_prob = speex_vad ? 1.0f : 0.0f;
            vad_result = speex_vad ? AUDIO_VAD_SPEECH : AUDIO_VAD_SILENCE;
        }
    }

    /*
     * Step 3b: RKNN Silero VAD (accurate approach)
     *
     * When RKNN model is available and vad_rknn_path is configured, use Silero VAD:
     *   - Input: 512 normalized audio samples after AEC+NS+AGC processing
     *   - Output: speech probability 0.0~1.0
     *   - Decision: probability >= vad_threshold -> SPEECH, otherwise SILENCE
     *
     * Advantages of Silero VAD over SpeexDSP energy VAD:
     *   1. Based on deep-learning features, doesn't rely on simple energy thresholds
     *   2. Robust to noise: can distinguish "speech in noise" from "pure noise"
     *   3. Higher detection rate for far-field faint speech
     *   4. RK3568 NPU inference only about 1ms, meets real-time requirements
     */
    if (ctx->config.enable_vad && ctx->rknn_ready) {
        float prob = run_rknn_vad(ctx, out_frame, (int)frame_size);
        ctx->speech_prob = prob;
        vad_result = (prob >= ctx->config.vad_threshold) ? AUDIO_VAD_SPEECH : AUDIO_VAD_SILENCE;
    }

    return vad_result;
}

/**
 * @brief Get speech probability of the last frame
 *
 * @param ctx Preprocessing context
 * @return Speech probability 0.0~1.0, only valid when VAD is enabled
 *
 * RKNN Silero VAD: returns continuous probability value (e.g. 0.87 means 87% likely to be speech)
 * SpeexDSP energy VAD: returns 0.0 or 1.0 (binary decision)
 * VAD disabled: returns 1.0 (default speech)
 */
float audio_preproc_get_speech_prob(const audio_preproc_t *ctx)
{
    if (!ctx)
        return 1.0f;
    return ctx->speech_prob;
}
