#ifndef RKISP_CAMERA_H
#define RKISP_CAMERA_H

#include "v4l2_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open MIPI camera (rkaiq 3A + rkisp ISP, outputs NV12)
 * @param iq_dir rkaiq IQ file dir; NULL = default
 * @return v4l2_capture_t (is_mplane=true), NULL on failure (fall back to USB)
 */
v4l2_capture_t *rkisp_camera_open(const char *iq_dir, int width, int height, int fps);

#ifdef __cplusplus
}
#endif

#endif /* RKISP_CAMERA_H */
