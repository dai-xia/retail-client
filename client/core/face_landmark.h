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
 * @brief 5-point facial landmarks
 *
 * PFLD model outputs 106 landmarks, we only take the first 5 for affine alignment:
 *   - Left eye center, right eye center: compute eye midpoint and rotation angle
 *   - Nose tip: auxiliary verification
 *   - Left mouth corner, right mouth corner: auxiliary verification
 *
 * These 5 points are enough to compute a similarity transform matrix, aligning the face to a standard pose,
 * eliminating yaw/pitch/roll differences, significantly improving MobileFaceNet feature extraction accuracy.
 */
typedef struct {
    float left_eye_x, left_eye_y;     /**< Left eye center coordinates */
    float right_eye_x, right_eye_y;   /**< Right eye center coordinates */
    float nose_x, nose_y;             /**< Nose tip coordinates */
    float left_mouth_x, left_mouth_y; /**< Left mouth corner coordinates */
    float right_mouth_x, right_mouth_y; /**< Right mouth corner coordinates */
} face_5point_t;

/**
 * @brief Face landmark detection context (opaque struct)
 *
 * Internally holds:
 *   - rknn_context: RKNN NPU inference handle
 *   - Model data buffer
 * Created with face_landmark_create(), destroyed with face_landmark_destroy()
 */
typedef struct face_landmark_ctx face_landmark_t;

/**
 * @brief Create face landmark detection context
 * @return Context pointer, NULL on failure
 *
 * Only allocates memory, does not load model. Must then call face_landmark_load_model().
 */
face_landmark_t* face_landmark_create(void);

/**
 * @brief Load PFLD RKNN model
 * @param ctx       Landmark detection context
 * @param rknn_path PFLD model file path (.rknn format)
 * @return 0=success, -1=failure
 *
 * PFLD (Practical Facial Landmark Detector) model features:
 *   - Input: 112x112x3 RGB uint8
 *   - Output: 212 floats (106 landmarks, x/y coordinates each, normalized to [0,1])
 *   - RKNN quantized inference about 2-3ms (RK3568 NPU)
 *   - Model size about 2.5MB
 */
int face_landmark_load_model(face_landmark_t *ctx, const char *rknn_path);

/**
 * @brief Detect 5-point face landmarks
 * @param ctx        Landmark detection context
 * @param rgb_data   Raw RGB frame data (full frame, 3-channel interleaved RGB24)
 * @param width      Frame width (pixels)
 * @param height     Frame height (pixels)
 * @param roi        Face ROI region [x1, y1, x2, y2], coordinates based on original frame
 * @param out_points Output 5 landmarks (coordinates mapped back to original frame)
 * @return 0=success, -1=failure
 *
 * Processing flow:
 *   1. Crop ROI region from full RGB frame (pointer arithmetic, no OpenCV dependency)
 *   2. Bilinear interpolation scale to 112x112
 *   3. RKNN inference, get 106 landmarks (212 floats)
 *   4. Take the first 5 landmarks, de-normalize and map back to original frame coordinates
 *
 * Coordinate mapping:
 *   Original frame coordinates = roi origin + landmark normalized coordinates * roi width/height
 */
int face_landmark_detect(face_landmark_t *ctx,
                         const uint8_t *rgb_data,
                         int width, int height,
                         const int roi[4],
                         face_5point_t *out_points);

/**
 * @brief Affine alignment based on landmarks
 * @param ctx              Landmark detection context (only for logging, can pass NULL)
 * @param rgb_data         Raw RGB frame data
 * @param width            Frame width
 * @param height           Frame height
 * @param points           5-point landmarks (output from face_landmark_detect)
 * @param aligned_rgb_112x112 Output aligned face (112x112 RGB, must pre-allocate 112*112*3 bytes)
 * @return 0=success, -1=failure
 *
 * Affine alignment principle:
 *   1. Compute the midpoint of both eyes as the reference center
 *   2. Compute rotation angle from the line connecting both eyes (similarity transform: uniform scale + rotation + translation)
 *   3. Build a 2x3 affine transformation matrix M
 *   4. For each output pixel of 112x112, use M inverse to map back to original coordinates, bilinear interpolation sampling
 *
 * After alignment, the face eliminates yaw differences, MobileFaceNet feature extraction accuracy can improve by 5-10%.
 */
int face_landmark_align(face_landmark_t *ctx,
                        const uint8_t *rgb_data,
                        int width, int height,
                        const face_5point_t *points,
                        uint8_t *aligned_rgb_112x112);

/**
 * @brief Destroy face landmark detection context
 * @param ctx Context pointer
 *
 * Release RKNN context, model data and all other resources
 */
void face_landmark_destroy(face_landmark_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FACE_LANDMARK_H */
