/**
 * @file face_antispoof.c
 * @brief MiniFASNet anti-spoofing/liveness detection (RKNN NPU accelerated)
 *
 * Core algorithm:
 *   1. Crop face ROI from full RGB frame, bilinear scale to 80x80
 *   2. RKNN NPU inference on MiniFASNet model, output classification probabilities
 *   3. Parse and decide the result based on output format:
 *      - 3-class output: argmax, class0=real, class1/2=attack
 *      - Single probability value: threshold 0.5
 *
 * Dependencies:
 *   - rknn_api.h (guarded by USE_RKNN macro)
 *   - Pure C implementation, no OpenCV/Qt dependency
 */

#include "face_antispoof.h"
#include "face_image_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rknn_api.h"

/* ========== Internal structure definitions ========== */

struct face_antispoof_ctx {
    rknn_context      rknn_ctx;       /**< RKNN NPU inference handle */
    int               rknn_ready;     /**< Flag indicating whether model loaded successfully */
    unsigned char    *model_data;     /**< Model file memory map (read via malloc) */
    int               model_size;     /**< Model file size (bytes) */
};

/* ========== Public API implementation ========== */

face_antispoof_t* face_antispoof_create(void)
{
    face_antispoof_t *ctx = (face_antispoof_t *)calloc(1, sizeof(face_antispoof_t));
    if (!ctx)
        return NULL;

    ctx->rknn_ctx = 0;
    ctx->rknn_ready = 0;
    ctx->model_data = NULL;
    ctx->model_size = 0;

    return ctx;
}

int face_antispoof_load_model(face_antispoof_t *ctx, const char *rknn_path)
{
    if (!ctx || !rknn_path)
        return -1;

    /* Read RKNN model file into memory */
    FILE *fp = fopen(rknn_path, "rb");
    if (!fp)
        return -1;

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

    /*
     * Initialize RKNN inference context (same flow as face_landmark)
     *
     * MiniFASNet inference characteristics on RK3568 NPU:
     *   - Extremely small model (80x80 input), inference about 1-2ms
     *   - Can be chained with face detection/landmark detection pipeline,
     *     total latency about 5-7ms (detection 2ms + landmark 2ms + anti-spoofing 1ms)
     */
    int ret = rknn_init(&ctx->rknn_ctx, ctx->model_data, ctx->model_size, 0, NULL);
    if (ret < 0) {
        free(ctx->model_data);
        ctx->model_data = NULL;
        ctx->model_size = 0;
        return -1;
    }

    ctx->rknn_ready = 1;

    return 0;
}

face_liveness_result face_antispoof_check(face_antispoof_t *ctx,
                                           const uint8_t *rgb_data,
                                           int width, int height,
                                           const int roi[4])
{
    if (!ctx || !rgb_data)
        return LIVENESS_UNKNOWN;

    if (roi[0] >= roi[2] || roi[1] >= roi[3])
        return LIVENESS_UNKNOWN;

    /* Step 1: Crop ROI + bilinear scale to 80x80 (RGB passthrough) */
    uint8_t input_rgb[ANTISPOOF_INPUT_SIZE * ANTISPOOF_INPUT_SIZE * 3];
    if (crop_resize_rgb(rgb_data, width, height, roi, input_rgb, ANTISPOOF_INPUT_SIZE) != 0)
        return LIVENESS_UNKNOWN;

    if (!ctx->rknn_ready)
        return LIVENESS_UNKNOWN;

    /*
     * Step 2: Set RKNN input
     *
     * MiniFASNet input format:
     *   - Size: 80x80x3 (smaller than PFLD's 112x112, faster inference)
     *   - Format: NHWC uint8 (SDK internally auto-quantizes to INT8)
     *   - Color: RGB (training uses RGB, inference must match)
     */
    rknn_input rknn_in;
    memset(&rknn_in, 0, sizeof(rknn_in));
    rknn_in.index = 0;
    rknn_in.buf = input_rgb;
    rknn_in.size = ANTISPOOF_INPUT_SIZE * ANTISPOOF_INPUT_SIZE * 3;
    rknn_in.pass_through = 0;
    rknn_in.type = RKNN_TENSOR_UINT8;
    rknn_in.fmt = RKNN_TENSOR_NHWC;

    int ret = rknn_inputs_set(ctx->rknn_ctx, 1, &rknn_in);
    if (ret < 0)
        return LIVENESS_UNKNOWN;

    /* Step 3: Execute RKNN inference */
    ret = rknn_run(ctx->rknn_ctx, NULL);
    if (ret < 0)
        return LIVENESS_UNKNOWN;

    /*
     * Step 4: Get RKNN output
     *
     * MiniFASNet output format (two common versions):
     *
     * Version A - 3-class output (recommended, better generalization):
     *   Output tensor: [1, 3] float
     *   [class0_score, class1_score, class2_score]
     *   - class0: real face probability
     *   - class1: planar attack probability (printed photo/screen)
     *   - class2: curved attack probability (bent photo/mask)
     *   Decision: argmax -> 0=real, 1 or 2=attack
     *
     * Version B - Single probability output:
     *   Output tensor: [1, 1] float
     *   [spoof_probability]
     *   Decision: <0.5=real, >=0.5=attack
     *
     * Here we auto-adapt to both formats via output element count
     */
    rknn_output rknn_out;
    memset(&rknn_out, 0, sizeof(rknn_out));
    rknn_out.index = 0;
    rknn_out.want_float = 1;

    ret = rknn_outputs_get(ctx->rknn_ctx, 1, &rknn_out, NULL);
    if (ret < 0)
        return LIVENESS_UNKNOWN;

    float *output = (float *)rknn_out.buf;
    if (!output) {
        rknn_outputs_release(ctx->rknn_ctx, 1, &rknn_out);
        return LIVENESS_UNKNOWN;
    }

    /*
     * Step 5: Parse output, decide liveness/attack
     *
     * rknn_out.size is the output data byte count, float size = 4 bytes,
     * so element count = size / sizeof(float)
     */
    int num_elements = (int)(rknn_out.size / sizeof(float));
    face_liveness_result result = LIVENESS_UNKNOWN;

    if (num_elements >= 3) {
        /*
         * 3-class output: argmax decision
         *
         * MiniFASNet trained with 3-class cross-entropy loss, output is not softmax-ed
         * (RKNN quantized models usually omit softmax, argmax directly, softmax doesn't change argmax result)
         *
         * Classification meaning:
         *   output[0] = real face score
         *   output[1] = planar attack score
         *   output[2] = curved attack score
         *
         * argmax=0 -> real, argmax=1 or 2 -> attack
         */
        int max_idx = 0;
        float max_val = output[0];
        for (int i = 1; i < 3; i++) {
            if (output[i] > max_val) {
                max_val = output[i];
                max_idx = i;
            }
        }

        result = (max_idx == 0) ? LIVENESS_REAL : LIVENESS_SPOOF;

    } else if (num_elements == 1) {
        /*
         * Single probability output: threshold decision
         *
         * output[0] = spoof probability (0.0=definitely real, 1.0=definitely attack)
         * Threshold 0.5: below 0.5 means real, equal or above 0.5 means attack
         *
         * Note: threshold can be adjusted in real deployment to reduce false rejection rate (FRR):
         *   - Strict mode: threshold 0.3 (more attacks blocked, but real users may be rejected)
         *   - Lenient mode: threshold 0.7 (reduces false rejects, but attack pass rate rises)
         *   - Default 0.5: balances FRR and FAR
         */
        result = (output[0] < 0.5f) ? LIVENESS_REAL : LIVENESS_SPOOF;

    } else {
        /* Abnormal output (0 or 2 elements), cannot determine */
        result = LIVENESS_UNKNOWN;
    }

    /* Release RKNN output buffer */
    rknn_outputs_release(ctx->rknn_ctx, 1, &rknn_out);

    return result;
}

void face_antispoof_destroy(face_antispoof_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->rknn_ready && ctx->rknn_ctx) {
        rknn_destroy(ctx->rknn_ctx);
        ctx->rknn_ctx = 0;
        ctx->rknn_ready = 0;
    }

    if (ctx->model_data) {
        free(ctx->model_data);
        ctx->model_data = NULL;
    }
    ctx->model_size = 0;

    free(ctx);
}
