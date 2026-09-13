#ifndef FACE_IMAGE_UTILS_H
#define FACE_IMAGE_UTILS_H

#include <stdint.h>

/**
 * @file face_image_utils.h
 * @brief Face image preprocessing (bilinear scaling + cropping, RGB passthrough)
 */

/**
 * @brief Bilinear-interpolate scale an RGB image.
 * @param dst output buffer (pre-allocated dst_w * dst_h * 3 bytes)
 */
void bilinear_resize_rgb(const uint8_t *src, int src_w, int src_h,
                         uint8_t *dst, int dst_w, int dst_h);

/**
 * @brief Crop ROI and bilinear-scale RGB to a square, in one step.
 * @param roi [x1, y1, x2, y2]
 * @param dst output (dst_size * dst_size * 3 bytes)
 * @return 0=success, -1=failure
 */
int crop_resize_rgb(const uint8_t *rgb_data, int width, int height,
                    const int roi[4],
                    uint8_t *dst, int dst_size);

#endif /* FACE_IMAGE_UTILS_H */
