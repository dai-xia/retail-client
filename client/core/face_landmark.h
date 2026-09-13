#ifndef FACE_LANDMARK_H
#define FACE_LANDMARK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PFLD model input size (112x112, aligned with MobileFaceNet) */
#define FACE_LANDMARK_SIZE 112

/* Total number of landmarks output by PFLD model */
#define FACE_LANDMARK_POINTS 106

/**
 * @brief 5-point facial landmarks (first 5 of the 106 PFLD outputs).
 */
typedef struct {
    float left_eye_x, left_eye_y;     /**< Left eye center coordinates */
    float right_eye_x, right_eye_y;   /**< Right eye center coordinates */
    float nose_x, nose_y;             /**< Nose tip coordinates */
    float left_mouth_x, left_mouth_y; /**< Left mouth corner coordinates */
    float right_mouth_x, right_mouth_y; /**< Right mouth corner coordinates */
} face_5point_t;

/** @brief Face landmark detection context (opaque). */
typedef struct face_landmark_ctx face_landmark_t;

/**
 * @brief Create face landmark detection context.
 * @return Context pointer, NULL on failure
 */
face_landmark_t* face_landmark_create(void);

/**
 * @brief Load PFLD RKNN model.
 * @return 0=success, -1=failure
 */
int face_landmark_load_model(face_landmark_t *ctx, const char *rknn_path);

/**
 * @brief Detect 5-point landmarks (PFLD NPU).
 * @param rgb_data full frame, RGB24 interleaved (NHWC uint8 raw pixels)
 * @param roi face ROI [x1, y1, x2, y2] in original-frame coords
 * @param out_points landmarks mapped back to original-frame coords
 * @return 0=success, -1=failure
 */
int face_landmark_detect(face_landmark_t *ctx,
                         const uint8_t *rgb_data,
                         int width, int height,
                         const int roi[4],
                         face_5point_t *out_points);

/**
 * @brief Align face to 112x112 via similarity transform.
 * @param points 5-point landmarks (from face_landmark_detect)
 * @param aligned_rgb_112x112 output 112x112 RGB (pre-allocated 112*112*3 bytes)
 * @return 0=success, -1=failure
 */
int face_landmark_align(face_landmark_t *ctx,
                        const uint8_t *rgb_data,
                        int width, int height,
                        const face_5point_t *points,
                        uint8_t *aligned_rgb_112x112);

/** @brief Destroy face landmark detection context. */
void face_landmark_destroy(face_landmark_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FACE_LANDMARK_H */
