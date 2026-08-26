/**
 * @file face_landmark.c
 * @brief PFLD 5-point face landmark detection + affine alignment (RKNN NPU accelerated)
 *
 * Core algorithm:
 *   1. Crop face ROI from full RGB frame, bilinear scale to 112x112
 *   2. RKNN NPU inference on PFLD model, output 106-point normalized coordinates
 *   3. Take the first 5 points and map back to original frame coordinates
 *   4. Compute similarity transform matrix based on 5 points, affine align face to standard pose
 *
 * Dependencies:
 *   - rknn_api.h (guarded by USE_RKNN macro)
 *   - Pure C implementation, no OpenCV/Qt dependency
 */

#include "face_landmark.h"
#include "face_image_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rknn_api.h"

/* ========== Internal structure definitions ========== */

struct face_landmark_ctx {
    rknn_context      rknn_ctx;       /**< RKNN NPU inference handle */
    int               rknn_ready;     /**< Flag indicating whether model loaded successfully */
    unsigned char    *model_data;     /**< Model file memory map (read via malloc) */
    int               model_size;     /**< Model file size (bytes) */
};

/* ========== Affine transform related ========== */

/**
 * @brief Compute 2x3 affine transform matrix (similarity transform)
 *
 * @param points   5-point landmarks (only uses both eye coordinates)
 * @param out_mat  Output 2x3 matrix [a00,a01,a10,a11,a20,a21] (row-major)
 *
 * Similarity transform = uniform scale + rotation + translation, only 4 degrees of freedom (s, theta, tx, ty),
 * more stable than a general affine transform (6 DOF), does not introduce shear distortion.
 *
 * Computation method:
 *   1. Eye midpoint as transform reference center src_center
 *   2. Target template eye midpoint dst_center = (38.2946, 51.6963) (112x112 standard template)
 *   3. Rotation angle theta = atan2(right_eye_y - left_eye_y, right_eye_x - left_eye_x)
 *   4. Scale ratio s = eye distance ratio (source/target)
 *   5. M = [s*cos(theta), -s*sin(theta), tx; s*sin(theta), s*cos(theta), ty]
 *
 * Target template coordinates (112x112 standard face):
 *   Left eye (30.2946, 51.6963), right eye (65.5318, 51.6963)
 *   These values come from the standard alignment template of the 300W dataset
 */
static void compute_similarity_transform(const face_5point_t *points, float out_mat[6])
{
    /* Standard template: 5-point coordinates in 112x112 alignment template (from 300W/LFPW dataset) */
    static const float dst_left_eye_x  = 38.2946f;
    static const float dst_left_eye_y  = 51.6963f;
    static const float dst_right_eye_x = 73.5318f;
    static const float dst_right_eye_y = 51.6963f;

    /* Both eye coordinates in source image */
    float lex = points->left_eye_x,  ley = points->left_eye_y;
    float rex = points->right_eye_x, rey = points->right_eye_y;

    /* Source and target eye distance, used to compute scale ratio */
    float src_dist = sqrtf((rex - lex) * (rex - lex) + (rey - ley) * (rey - ley));
    float dst_dist = sqrtf((dst_right_eye_x - dst_left_eye_x) * (dst_right_eye_x - dst_left_eye_x)
                         + (dst_right_eye_y - dst_left_eye_y) * (dst_right_eye_y - dst_left_eye_y));

    /* Scale ratio: source distance / target distance */
    float scale = src_dist / dst_dist;

    /* Rotation angle: angle between source eye line and horizontal */
    float angle = atan2f(rey - ley, rex - lex);
    /* Target template eye line is horizontal, target angle = 0, so rotation is the source angle */
    float cos_a = cosf(angle);
    float sin_a = sinf(angle);

    /* Scale + rotation part (2x2) */
    float a00 =  scale * cos_a;
    float a01 = -scale * sin_a;
    float a10 =  scale * sin_a;
    float a11 =  scale * cos_a;

    /* Source and target eye midpoints */
    float src_cx = (lex + rex) * 0.5f;
    float src_cy = (ley + rey) * 0.5f;
    float dst_cx = (dst_left_eye_x + dst_right_eye_x) * 0.5f;
    float dst_cy = (dst_left_eye_y + dst_right_eye_y) * 0.5f;

    /* Translation: map source center to target center
     * dst_cx = a00 * src_cx + a01 * src_cy + tx
     * => tx = dst_cx - a00 * src_cx - a01 * src_cy
     */
    float tx = dst_cx - a00 * src_cx - a01 * src_cy;
    float ty = dst_cy - a10 * src_cx - a11 * src_cy;

    /* Output 2x3 matrix [row-major]:
     * | a00  a01  tx |     | s*cos(theta)  -s*sin(theta)  tx |
     * | a10  a11  ty |  =  | s*sin(theta)   s*cos(theta)  ty |
     */
    out_mat[0] = a00;
    out_mat[1] = a01;
    out_mat[2] = tx;
    out_mat[3] = a10;
    out_mat[4] = a11;
    out_mat[5] = ty;
}

/**
 * @brief Apply affine transform, extract aligned face from original frame
 *
 * @param rgb_data     Original RGB frame
 * @param width        Frame width
 * @param height       Frame height
 * @param mat          2x3 affine transform matrix (forward: source -> target)
 * @param out          Output 112x112 RGB data
 *
 * Inverse transform sampling:
 *   For each output image pixel (ox, oy), compute the corresponding source image coordinates (sx, sy):
 *     [sx]   [a00 a01]^-1   [ox - tx]   [a11  -a01]   [ox - tx]
 *     [sy] = [a10 a11]     [oy - ty] = ---------- * [-a10  a00] * [ox - ty]
 *                                         det(A)     det(A) = a00*a11 - a01*a10
 *   Then bilinear interpolation samples source image RGB values.
 */
static void apply_affine_transform(const uint8_t *rgb_data, int width, int height,
                                    const float mat[6],
                                    uint8_t *out)
{
    /* Inverse transform matrix elements (A^-1 = adj(A)/det(A)) */
    float a00 = mat[0], a01 = mat[1], tx = mat[2];
    float a10 = mat[3], a11 = mat[4], ty = mat[5];

    float det = a00 * a11 - a01 * a10;
    if (fabsf(det) < 1e-6f) {
        /* Degenerate matrix, fall back to black fill */
        memset(out, 0, FACE_LANDMARK_SIZE * FACE_LANDMARK_SIZE * 3);
        return;
    }

    float inv_det = 1.0f / det;
    /* Inverse matrix 2x2 part */
    float ia00 =  a11 * inv_det;
    float ia01 = -a01 * inv_det;
    float ia10 = -a10 * inv_det;
    float ia11 =  a00 * inv_det;

    const int frame_stride = width * 3;

    for (int oy = 0; oy < FACE_LANDMARK_SIZE; oy++) {
        uint8_t *out_row = out + oy * FACE_LANDMARK_SIZE * 3;
        for (int ox = 0; ox < FACE_LANDMARK_SIZE; ox++) {
            /* Inverse transform: target coordinates -> source coordinates */
            float dx = (float)ox - tx;
            float dy = (float)oy - ty;
            float sx = ia00 * dx + ia01 * dy;
            float sy = ia10 * dx + ia11 * dy;

            /* Bilinear interpolation sampling */
            int x0 = (int)floorf(sx);
            int y0 = (int)floorf(sy);
            int x1 = x0 + 1;
            int y1 = y0 + 1;
            float u = sx - (float)x0;
            float v = sy - (float)y0;

            /* Boundary check: pixels outside original image are filled with black */
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

/* ========== Public API implementation ========== */

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
     * Initialize RKNN inference context
     *
     * rknn_init parameters:
     *   - model: model data memory pointer (can also be a .rknn file path, but memory mode is faster)
     *   - size:  model data size
     *   - flag:  0=default (async mode, needs manual sync)
     *   - config: NULL (no extended config)
     *
     * RK3568 NPU features:
     *   - 3 compute cores, supports INT8/UINT8 quantized inference
     *   - rknn_init parses the model graph and allocates NPU resources
     *   - First init takes about 50-100ms (graph compilation), subsequent inference only 2-3ms
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

    /* Step 1: Crop ROI + bilinear scale to 112x112 (RGB passthrough) */
    uint8_t input_rgb[FACE_LANDMARK_SIZE * FACE_LANDMARK_SIZE * 3];
    if (crop_resize_rgb(rgb_data, width, height, roi, input_rgb, FACE_LANDMARK_SIZE) != 0)
        return -1;

    if (!ctx->rknn_ready)
        return -1;

    /*
     * Step 2: Set RKNN input
     *
     * rknn_input struct field descriptions:
     *   - index:    Input tensor index (PFLD has only 1 input, index=0)
     *   - buf:      Input data pointer (RGB uint8 array)
     *   - size:     Input data byte count (112*112*3)
     *   - pass_through: 0=SDK does normalization/quantization internally (recommended)
     *                    1=user handles it (advanced usage)
     *   - type:     RKNN_TENSOR_UINT8 (uint8 input, SDK internally auto-quantizes to INT8)
     *   - fmt:      RKNN_TENSOR_NHWC (batch=1, height=112, width=112, channel=3)
     *
     * NPU internal quantization flow:
     *   uint8 input -> quantize to INT8 via model scale/zero_point -> INT8 conv computation -> dequantize output to float
     */
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

    /* Step 3: Execute RKNN inference (synchronous mode) */
    ret = rknn_run(ctx->rknn_ctx, NULL);
    if (ret < 0)
        return -1;

    /*
     * Step 4: Get RKNN output
     *
     * PFLD model output:
     *   - Output tensor shape: [1, 212] (106 landmarks, x/y coordinates each)
     *   - Coordinate values normalized to [0, 1], relative to 112x112 input image
     *   - Model may output 2 tensors: [landmarks, angle], only take the 1st
     *
     * rknn_output settings:
     *   - want_float: 1=output float type (SDK internally dequantizes)
     *                 0=output raw INT8 (needs manual dequantize, saves one copy)
     *   Here we use float for convenience
     */
    rknn_output rknn_out;
    memset(&rknn_out, 0, sizeof(rknn_out));
    rknn_out.index = 0;
    rknn_out.want_float = 1;

    ret = rknn_outputs_get(ctx->rknn_ctx, 1, &rknn_out, NULL);
    if (ret < 0)
        return -1;

    /* Step 5: Parse output, take the first 5 landmarks */
    float *landmarks = (float *)rknn_out.buf;
    if (!landmarks) {
        rknn_outputs_release(ctx->rknn_ctx, 1, &rknn_out);
        return -1;
    }

    /*
     * PFLD output format: 212 floats = 106 landmarks x 2 (x, y)
     * landmarks[0], landmarks[1] = 1st point (x, y)  -> left eye
     * landmarks[2], landmarks[3] = 2nd point (x, y)  -> right eye
     * landmarks[4], landmarks[5] = 3rd point (x, y)  -> nose tip
     * landmarks[6], landmarks[7] = 4th point (x, y)  -> left mouth corner
     * landmarks[8], landmarks[9] = 5th point (x, y)  -> right mouth corner
     *
     * Normalized coordinates -> original frame coordinates:
     *   original x = roi_x1 + landmark_x * roi_w
     *   original y = roi_y1 + landmark_y * roi_h
     */
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

    /* Release RKNN output buffer (must call, otherwise memory leak) */
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

    (void)ctx; /* Context only used for logging, not for computation */

    /* Compute similarity transform matrix: source coordinates (original frame) -> target coordinates (112x112 standard template) */
    float mat[6];
    compute_similarity_transform(points, mat);

    /* Inverse transform sampling: for each output pixel of 112x112, inverse map to source image coordinates, bilinear interpolation */
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
