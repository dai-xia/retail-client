/**
 * @file face_image_utils.c
 * @brief Face image preprocessing (bilinear scaling + cropping, RGB passthrough)
 */

#include "face_image_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void bilinear_resize_rgb(const uint8_t *src, int src_w, int src_h,
                         uint8_t *dst, int dst_w, int dst_h)
{
    const float x_scale = (float)src_w / (float)dst_w;
    const float y_scale = (float)src_h / (float)dst_h;
    const int src_stride = src_w * 3;

    for (int dy = 0; dy < dst_h; dy++) {
        float sy = (dy + 0.5f) * y_scale - 0.5f;
        int y0 = (int)floorf(sy);
        int y1 = y0 + 1;
        float v = sy - (float)y0;

        if (y0 < 0) { y0 = 0; v = 0.0f; }
        if (y1 >= src_h) { y1 = src_h - 1; v = 1.0f; }

        const uint8_t *src_row0 = src + y0 * src_stride;
        const uint8_t *src_row1 = src + y1 * src_stride;
        uint8_t *dst_row = dst + dy * dst_w * 3;

        for (int dx = 0; dx < dst_w; dx++) {
            float sx = (dx + 0.5f) * x_scale - 0.5f;
            int x0 = (int)floorf(sx);
            int x1 = x0 + 1;
            float u = sx - (float)x0;

            if (x0 < 0) { x0 = 0; u = 0.0f; }
            if (x1 >= src_w) { x1 = src_w - 1; u = 1.0f; }

            const uint8_t *p00 = src_row0 + x0 * 3;
            const uint8_t *p10 = src_row0 + x1 * 3;
            const uint8_t *p01 = src_row1 + x0 * 3;
            const uint8_t *p11 = src_row1 + x1 * 3;

            float w00 = (1.0f - u) * (1.0f - v);
            float w10 = u * (1.0f - v);
            float w01 = (1.0f - u) * v;
            float w11 = u * v;

            dst_row[dx * 3 + 0] = (uint8_t)(p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11 + 0.5f);
            dst_row[dx * 3 + 1] = (uint8_t)(p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11 + 0.5f);
            dst_row[dx * 3 + 2] = (uint8_t)(p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11 + 0.5f);
        }
    }
}

int crop_resize_rgb(const uint8_t *rgb_data, int width, int height,
                    const int roi[4],
                    uint8_t *dst, int dst_size)
{
    int x1 = roi[0], y1 = roi[1], x2 = roi[2], y2 = roi[3];

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > width) x2 = width;
    if (y2 > height) y2 = height;

    int roi_w = x2 - x1;
    int roi_h = y2 - y1;

    if (roi_w <= 0 || roi_h <= 0)
        return -1;

    const int frame_stride = width * 3;

    /* Fast path: ROI exactly matches target size, direct copy, no scaling needed */
    if (roi_w == dst_size && roi_h == dst_size) {
        for (int y = 0; y < dst_size; y++) {
            const uint8_t *src_row = rgb_data + (y1 + y) * frame_stride + x1 * 3;
            uint8_t *dst_row = dst + y * dst_size * 3;
            memcpy(dst_row, src_row, dst_size * 3);
        }
        return 0;
    }

    /* General path: crop ROI -> bilinear scale (RGB passthrough) */
    uint8_t *roi_buf = (uint8_t *)malloc(roi_w * roi_h * 3);
    if (!roi_buf)
        return -1;

    for (int y = 0; y < roi_h; y++) {
        const uint8_t *src_row = rgb_data + (y1 + y) * frame_stride + x1 * 3;
        uint8_t *dst_row = roi_buf + y * roi_w * 3;
        memcpy(dst_row, src_row, roi_w * 3);
    }

    bilinear_resize_rgb(roi_buf, roi_w, roi_h, dst, dst_size, dst_size);

    free(roi_buf);
    return 0;
}
