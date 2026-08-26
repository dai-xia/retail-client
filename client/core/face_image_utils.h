#ifndef FACE_IMAGE_UTILS_H
#define FACE_IMAGE_UTILS_H

#include <stdint.h>

/**
 * @file face_image_utils.h
 * @brief Face image preprocessing utilities (bilinear scaling + cropping, RGB passthrough)
 *
 * Shared by face_landmark.c and face_antispoof.c to avoid code duplication.
 *
 * Unified RGB convention:
 *   The whole face pipeline (capture -> RGA -> QImage -> RKNN) is RGB888,
 *   no more BGR<->RGB conversion (BGR is an OpenCV legacy convention, removed).
 */

/**
 * @brief Bilinear interpolation scale RGB image
 *
 * @param src   Source image RGB data (3-channel interleaved)
 * @param src_w Source image width
 * @param src_h Source image height
 * @param dst   Destination image buffer (must pre-allocate dst_w * dst_h * 3 bytes)
 * @param dst_w Destination width
 * @param dst_h Destination height
 */
void bilinear_resize_rgb(const uint8_t *src, int src_w, int src_h,
                         uint8_t *dst, int dst_w, int dst_h);

/**
 * @brief Crop ROI + bilinear scale RGB, in one step
 *
 * @param rgb_data Full-frame RGB24 data
 * @param width    Full-frame width
 * @param height   Full-frame height
 * @param roi      Crop region [x1, y1, x2, y2]
 * @param dst      Output RGB data (dst_size * dst_size * 3 bytes)
 * @param dst_size Target side length (square)
 * @return 0=success, -1=failure
 */
int crop_resize_rgb(const uint8_t *rgb_data, int width, int height,
                    const int roi[4],
                    uint8_t *dst, int dst_size);

#endif /* FACE_IMAGE_UTILS_H */
