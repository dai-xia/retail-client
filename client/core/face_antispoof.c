/**
 * @file face_antispoof.c
 * @brief MiniFASNet anti-spoofing/liveness detection (RKNN NPU accelerated)
 */

#include "face_antispoof.h"
#include "face_image_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rknn_api.h"

struct face_antispoof_ctx {
    rknn_context      rknn_ctx;       /**< RKNN NPU inference handle */
    int               rknn_ready;     /**< 1 if model loaded */
    unsigned char    *model_data;     /**< model file memory */
    int               model_size;     /**< model size (bytes) */
};

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

    uint8_t input_rgb[ANTISPOOF_INPUT_SIZE * ANTISPOOF_INPUT_SIZE * 3];
    if (crop_resize_rgb(rgb_data, width, height, roi, input_rgb, ANTISPOOF_INPUT_SIZE) != 0)
        return LIVENESS_UNKNOWN;

    if (!ctx->rknn_ready)
        return LIVENESS_UNKNOWN;

    /* MiniFASNet input: 80x80x3 RGB, NHWC uint8 */
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

    ret = rknn_run(ctx->rknn_ctx, NULL);
    if (ret < 0)
        return LIVENESS_UNKNOWN;

    /* Output: 3-class [real, planar, curved] (argmax) or single spoof prob (0.5 threshold); auto-adapt by element count */
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

    int num_elements = (int)(rknn_out.size / sizeof(float));
    face_liveness_result result = LIVENESS_UNKNOWN;

    if (num_elements >= 3) {
        /* 3-class: argmax; 0=real, 1/2=attack (not softmaxed, argmax unaffected) */
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
        /* single spoof probability: <0.5 real, >=0.5 attack */
        result = (output[0] < 0.5f) ? LIVENESS_REAL : LIVENESS_SPOOF;

    } else {
        result = LIVENESS_UNKNOWN;
    }

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
