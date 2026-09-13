#ifndef FACE_ANTISPOOF_H
#define FACE_ANTISPOOF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MiniFASNet model input size (80x80) */
#define ANTISPOOF_INPUT_SIZE 80

/**
 * @brief Liveness detection result.
 *   - LIVENESS_REAL: real face
 *   - LIVENESS_SPOOF: attack (photo/video/mask)
 *   - LIVENESS_UNKNOWN: cannot determine
 */
typedef enum {
    LIVENESS_REAL    = 0,  /**< Real face */
    LIVENESS_SPOOF   = 1,  /**< Attack/spoof */
    LIVENESS_UNKNOWN = 2   /**< Cannot determine */
} face_liveness_result;

/** @brief Anti-spoofing/liveness detection context (opaque). */
typedef struct face_antispoof_ctx face_antispoof_t;

/**
 * @brief Create anti-spoofing detection context.
 * @return Context pointer, NULL on failure
 */
face_antispoof_t* face_antispoof_create(void);

/**
 * @brief Load MiniFASNet RKNN model.
 * @return 0=success, -1=failure
 */
int face_antispoof_load_model(face_antispoof_t *ctx, const char *rknn_path);

/**
 * @brief Check whether the face region is a real face.
 * @param rgb_data full frame, RGB24 interleaved (NHWC uint8 raw pixels)
 * @param roi face ROI [x1, y1, x2, y2] in original-frame coords
 * @return liveness result (3-class argmax, or 0.5 threshold on single prob)
 */
face_liveness_result face_antispoof_check(face_antispoof_t *ctx,
                                           const uint8_t *rgb_data,
                                           int width, int height,
                                           const int roi[4]);

/** @brief Destroy anti-spoofing detection context. */
void face_antispoof_destroy(face_antispoof_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FACE_ANTISPOOF_H */
