/**
 * @file face_landmark.c
 * @brief PFLD 5-point landmark detection + affine alignment (RKNN NPU).
 */

#include "face_landmark.h"
#include "face_image_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rknn_api.h"

struct face_landmark_ctx {
    rknn_context      rknn_ctx;
    int               rknn_ready;
    unsigned char    *model_data;
    int               model_size;
};

/* Compute 2x3 similarity-transform matrix from 5 landmarks. */
static void compute_similarity_transform(const face_5point_t *points, float out_mat[6])
{
    /* 112x112 standard template eye coords (300W/LFPW). */
    static const float dst_left_eye_x  = 38.2946f;
    static const float dst_left_eye_y  = 51.6963f;
    static const float dst_right_eye_x = 73.5318f;
    static const float dst_right_eye_y = 51.6963f;

    float lex = points->left_eye_x,  ley = points->left_eye_y;
    float rex = points->right_eye_x, rey = points->right_eye_y;

    float src_dist = sqrtf((rex - lex) * (rex - lex) + (rey - ley) * (rey - ley));
    float dst_dist = sqrtf((dst_right_eye_x - dst_left_eye_x) * (dst_right_eye_x - dst_left_eye_x)
                         + (dst_right_eye_y - dst_left_eye_y) * (dst_right_eye_y - dst_left_eye_y));

    float scale = src_dist / dst_dist;

    float angle = atan2f(rey - ley, rex - lex);
    float cos_a = cosf(angle);
    float sin_a = sinf(angle);

    float a00 =  scale * cos_a;
    float a01 = -scale * sin_a;
    float a10 =  scale * sin_a;
    float a11 =  scale * cos_a;

    float src_cx = (lex + rex) * 0.5f;
    float src_cy = (ley + rey) * 0.5f;
    float dst_cx = (dst_left_eye_x + dst_right_eye_x) * 0.5f;
    float dst_cy = (dst_left_eye_y + dst_right_eye_y) * 0.5f;

    float tx = dst_cx - a00 * src_cx - a01 * src_cy;
    float ty = dst_cy - a10 * src_cx - a11 * src_cy;

    out_mat[0] = a00;
    out_mat[1] = a01;
    out_mat[2] = tx;
    out_mat[3] = a10;
    out_mat[4] = a11;
    out_mat[5] = ty;
}

/* Apply affine transform (inverse mapping + bilinear) to extract aligned 112x112 face. */
static void apply_affine_transform(const uint8_t *rgb_data, int width, int height,
                                    const float mat[6],
                                    uint8_t *out)
{
    float a00 = mat[0], a01 = mat[1], tx = mat[2];
    float a10 = mat[3], a11 = mat[4], ty = mat[5];

    float det = a00 * a11 - a01 * a10;
    if (fabsf(det) < 1e-6f) {
        /* Degenerate matrix: black fill */
        memset(out, 0, FACE_LANDMARK_SIZE * FACE_LANDMARK_SIZE * 3);
        return;
    }

    float inv_det = 1.0f / det;
    float ia00 =  a11 * inv_det;
    float ia01 = -a01 * inv_det;
    float ia10 = -a10 * inv_det;
    float ia11 =  a00 * inv_det;

    const int frame_stride = width * 3;

    for (int oy = 0; oy < FACE_LANDMARK_SIZE; oy++) {
        uint8_t *out_row = out + oy * FACE_LANDMARK_SIZE * 3;
        for (int ox = 0; ox < FACE_LANDMARK_SIZE; ox++) {
            float dx = (float)ox - tx;
            float dy = (float)oy - ty;
            float sx = ia00 * dx + ia01 * dy;
            float sy = ia10 * dx + ia11 * dy;

            int x0 = (int)floorf(sx);
            int y0 = (int)floorf(sy);
            int x1 = x0 + 1;
            int y1 = y0 + 1;
            float u = sx - (float)x0;
            float v = sy - (float)y0;

            /* Out-of-bounds pixels: black fill. */
            if (x0 < 0 || x1 >= width || y0 < 0 || y1 >= height) {
                out_row[ox * 3 + 0] = 0;
                out_row[ox * 3 + 1] = 0;
                out_row[ox * 3 + 2] = 0;
                continue;
            }

            const uint8_t *p00 = rgb_data + y0 * frame_stride + x0 * 3;
            const uint8_t *p10 = rgb_data + y0 * frame_stride + x1 * 3;
            const uint8_t *p01 = rgb_data + y1 * frame_stride + x0 * 3;
            const uint8_t *p11 = rgb_data + y1 * frame_stride + x1 * 3;

            float w00 = (1.0f - u) * (1.0f - v);
            float w10 = u * (1.0f - v);
            float w01 = (1.0f - u) * v;
            float w11 = u * v;

            out_row[ox * 3 + 0] = (uint8_t)(p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11 + 0.5f);
            out_row[ox * 3 + 1] = (uint8_t)(p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11 + 0.5f);
            out_row[ox * 3 + 2] = (uint8_t)(p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11 + 0.5f);
        }
    }
}

face_landmark_t* face_landmark_create(void)
{
    face_landmark_t *ctx = (face_landmark_t *)calloc(1, sizeof(face_landmark_t));
    if (!ctx)
        return NULL;

    ctx->rknn_ctx = 0;
    ctx->rknn_ready = 0;
    ctx->model_data = NULL;
    ctx->model_size = 0;

    return ctx;
}

int face_landmark_load_model(face_landmark_t *ctx, const char *rknn_path)
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

int face_landmark_detect(face_landmark_t *ctx,
                         const uint8_t *rgb_data,
                         int width, int height,
                         const int roi[4],
                         face_5point_t *out_points)
{
    if (!ctx || !rgb_data || !out_points)
        return -1;

    if (roi[0] >= roi[2] || roi[1] >= roi[3])
        return -1;

    /* Step 1: crop ROI + bilinear scale to 112x112 RGB. */
    uint8_t input_rgb[FACE_LANDMARK_SIZE * FACE_LANDMARK_SIZE * 3];
    if (crop_resize_rgb(rgb_data, width, height, roi, input_rgb, FACE_LANDMARK_SIZE) != 0)
        return -1;

    if (!ctx->rknn_ready)
        return -1;

    /* Step 2: set input (NHWC uint8, pass_through=0). */
    rknn_input rknn_in;
    memset(&rknn_in, 0, sizeof(rknn_in));
    rknn_in.index = 0;
    rknn_in.buf = input_rgb;
    rknn_in.size = FACE_LANDMARK_SIZE * FACE_LANDMARK_SIZE * 3;
    rknn_in.pass_through = 0;
    rknn_in.type = RKNN_TENSOR_UINT8;
    rknn_in.fmt = RKNN_TENSOR_NHWC;

    int ret = rknn_inputs_set(ctx->rknn_ctx, 1, &rknn_in);
    if (ret < 0)
        return -1;

    /* Step 3: run inference (sync). */
    ret = rknn_run(ctx->rknn_ctx, NULL);
    if (ret < 0)
        return -1;

    /* Step 4: get output ([1,212] landmarks normalized to [0,1] rel 112x112). */
    rknn_output rknn_out;
    memset(&rknn_out, 0, sizeof(rknn_out));
    rknn_out.index = 0;
    rknn_out.want_float = 1;

    ret = rknn_outputs_get(ctx->rknn_ctx, 1, &rknn_out, NULL);
    if (ret < 0)
        return -1;

    /* Step 5: map first 5 landmarks back to frame coords. */
    float *landmarks = (float *)rknn_out.buf;
    if (!landmarks) {
        rknn_outputs_release(ctx->rknn_ctx, 1, &rknn_out);
        return -1;
    }

    /* landmarks normalized to [0,1]; map to frame via roi offset/scale. */
    int roi_w = roi[2] - roi[0];
    int roi_h = roi[3] - roi[1];

    out_points->left_eye_x    = roi[0] + landmarks[0] * roi_w;
    out_points->left_eye_y    = roi[1] + landmarks[1] * roi_h;
    out_points->right_eye_x   = roi[0] + landmarks[2] * roi_w;
    out_points->right_eye_y   = roi[1] + landmarks[3] * roi_h;
    out_points->nose_x        = roi[0] + landmarks[4] * roi_w;
    out_points->nose_y        = roi[1] + landmarks[5] * roi_h;
    out_points->left_mouth_x  = roi[0] + landmarks[6] * roi_w;
    out_points->left_mouth_y  = roi[1] + landmarks[7] * roi_h;
    out_points->right_mouth_x = roi[0] + landmarks[8] * roi_w;
    out_points->right_mouth_y = roi[1] + landmarks[9] * roi_h;

    /* must call, else leak */
    rknn_outputs_release(ctx->rknn_ctx, 1, &rknn_out);

    return 0;
}

int face_landmark_align(face_landmark_t *ctx,
                        const uint8_t *rgb_data,
                        int width, int height,
                        const face_5point_t *points,
                        uint8_t *aligned_rgb_112x112)
{
    if (!rgb_data || !points || !aligned_rgb_112x112)
        return -1;

    (void)ctx;

    float mat[6];
    compute_similarity_transform(points, mat);

    apply_affine_transform(rgb_data, width, height, mat, aligned_rgb_112x112);

    return 0;
}

void face_landmark_destroy(face_landmark_t *ctx)
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
