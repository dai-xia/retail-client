#ifndef FACE_ANTISPOOF_H
#define FACE_ANTISPOOF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MiniFASNet model input size (80x80) */
#define ANTISPOOF_INPUT_SIZE 80

/**
 * @brief Liveness detection result
 *
 * MiniFASNet output classification:
 *   - LIVENESS_REAL:    real face (live)
 *   - LIVENESS_SPOOF:   attack sample (photo/video/3D mask)
 *   - LIVENESS_UNKNOWN: cannot determine (model output abnormal/inference failed)
 *
 * MiniFASNet principle:
 *   Lightweight anti-spoofing network improved from FASNet, inputs 80x80 RGB face,
 *   distinguishes real person from attack medium via depth/texture features. Unlike binary classification,
 *   MiniFASNet uses 3-class training: real person/planar attack/curved attack,
 *   with stronger generalization (still has some defense against unseen attack types).
 */
typedef enum {
    LIVENESS_REAL    = 0,  /**< Real face */
    LIVENESS_SPOOF   = 1,  /**< Attack/spoof */
    LIVENESS_UNKNOWN = 2   /**< Cannot determine */
} face_liveness_result;

/**
 * @brief Anti-spoofing/liveness detection context (opaque struct)
 *
 * Internally holds:
 *   - rknn_context: RKNN NPU inference handle
 *   - Model data buffer
 * Created with face_antispoof_create(), destroyed with face_antispoof_destroy()
 */
typedef struct face_antispoof_ctx face_antispoof_t;

/**
 * @brief Create anti-spoofing detection context
 * @return Context pointer, NULL on failure
 *
 * Only allocates memory, does not load model. Must then call face_antispoof_load_model().
 */
face_antispoof_t* face_antispoof_create(void);

/**
 * @brief Load MiniFASNet RKNN model
 * @param ctx       Anti-spoofing detection context
 * @param rknn_path MiniFASNet model file path (.rknn format)
 * @return 0=success, -1=failure
 *
 * MiniFASNet model features:
 *   - Input: 80x80x3 RGB uint8
 *   - Output: 3 float classification values [0_class, 1_class, 2_class]
 *     or 1 float probability value (depending on model version)
 *   - RKNN quantized inference about 1-2ms (RK3568 NPU)
 *   - Model size about 1.5MB
 *
 * 3-class meaning:
 *   - class 0: real face
 *   - class 1: planar attack (printed photo/phone screen)
 *   - class 2: curved attack (bent photo/3D mask)
 *   argmax result 0 means real person, 1 or 2 means attack
 */
int face_antispoof_load_model(face_antispoof_t *ctx, const char *rknn_path);

/**
 * @brief Detect whether the face region is a real face
 * @param ctx      Anti-spoofing detection context
 * @param rgb_data Raw RGB frame data (full frame, 3-channel interleaved RGB24)
 * @param width    Frame width (pixels)
 * @param height   Frame height (pixels)
 * @param roi      Face ROI region [x1, y1, x2, y2], coordinates based on original frame
 * @return Liveness detection result enum value
 *
 * Processing flow:
 *   1. Crop ROI region from full RGB frame (pointer arithmetic, no OpenCV dependency)
 *   2. Bilinear interpolation scale to 80x80
 *   3. RKNN inference, get output classification values
 *   4. For 3-class output: argmax -> 0=real, 1/2=attack
 *   5. For single probability value: threshold 0.5, <0.5=real, >=0.5=attack
 */
face_liveness_result face_antispoof_check(face_antispoof_t *ctx,
                                           const uint8_t *rgb_data,
                                           int width, int height,
                                           const int roi[4]);

/**
 * @brief Destroy anti-spoofing detection context
 * @param ctx Context pointer
 *
 * Release RKNN context, model data and all other resources
 */
void face_antispoof_destroy(face_antispoof_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FACE_ANTISPOOF_H */
