#ifndef RKISP_CAMERA_H
#define RKISP_CAMERA_H

#include "v4l2_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open MIPI camera (rkaiq 3A + rkisp ISP, outputs NV12)
 *
 * Link: OV5695 sensor(RAW10) -> MIPI-CSI -> rkisp(demosaic+3A+format conv) -> NV12.
 * Internally does rkaiq init/prepare/start + v4l2 MPLANE open + mmap + DMA-BUF export.
 *
 * @param iq_dir  rkaiq IQ calibration file dir (e.g. "/oem/etc/iqfiles"); NULL = default
 * @param width   desired output width
 * @param height  desired output height
 * @param fps     desired framerate
 * @return reuses v4l2_capture_t (is_mplane=true), NULL on failure (caller may fall back to USB)
 *
 * @note rkaiq prepare configures the sensor->CSI->ISP main link, no manual media-ctl needed
 */
v4l2_capture_t *rkisp_camera_open(const char *iq_dir, int width, int height, int fps);

#ifdef __cplusplus
}
#endif

#endif /* RKISP_CAMERA_H */
