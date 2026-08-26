#ifndef V4L2_CAPTURE_H
#define V4L2_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <linux/videodev2.h>

#ifdef __cplusplus
extern "C" {
#endif

/* V4L2 frame buffer descriptor */
typedef struct {
    void  *start;       /* mmap'd user-space address */
    size_t length;      /* mapping length (original, do not reuse) */
} v4l2_buffer_t;

/* V4L2 capture context */
typedef struct {
    int              fd;
    int              width;               /* negotiated width */
    int              height;              /* negotiated height */
    int              fps;                 /* negotiated framerate */
    uint32_t         pixfmt;              /* negotiated pixel format (V4L2_PIX_FMT_*) */
    v4l2_buffer_t   *buffers;             /* mmap buffer array */
    int              buffer_count;
    int              last_dequeued_index; /* last DQBUF buffer index, for QBUF return */
    int              dma_fds[8];          /* per-buffer DMA-BUF fd (EXPBUF), -1=not exported */
    bool             streaming;
    bool             is_mplane;           /* true=MPLANE (rkisp MIPI); false=single-plane (USB UVC) */

    /* ---- MIPI only (filled by rkisp_camera) ---- */
    void            *aiq_ctx;             /* rk_aiq_sys_ctx_t* (opaque, avoids v4l2 module depending on rkaiq) */
    void           (*aiq_stop)(void *);   /* stop 3A callback (called before STREAMOFF) */
    void           (*aiq_deinit)(void *); /* release 3A context callback (called after STREAMOFF) */
} v4l2_capture_t;

/**
 * @brief Open a V4L2 device and initialize MMAP capture
 * @param devname device path, e.g. "/dev/video0"
 * @param width   desired width
 * @param height  desired height
 * @param fps     desired framerate
 * @return capture context, NULL on failure
 * @note prefers NV12/NV16 (RK3568 ISP native), falls back to YUYV/MJPEG
 */
v4l2_capture_t *v4l2_capture_open(const char *devname, int width, int height, int fps);

/**
 * @brief Read one frame from the capture stream (DQBUF)
 * @param ctx      capture context
 * @param out_buf  output: frame data pointer (mmap memory, no copy)
 * @param out_len  output: frame data length
 * @return 0=success, -1=failure
 * @note caller MUST call v4l2_capture_enqueue afterwards to return the buffer
 */
int v4l2_capture_dequeue(v4l2_capture_t *ctx, void **out_buf, size_t *out_len);

/**
 * @brief Return a processed buffer (QBUF), allowing DMA to write again
 * @param ctx  capture context
 * @return 0=success, -1=failure
 */
int v4l2_capture_enqueue(v4l2_capture_t *ctx);

/**
 * @brief Export the DMA-BUF fd of the currently dequeued buffer (zero-copy path)
 * @param ctx  capture context
 * @return DMA-BUF fd, -1=failure
 * @note must be called after DQBUF and before QBUF
 *
 * VIDIOC_EXPBUF exports the MMAP buffer as a cross-device DMA-BUF fd so
 * RGA (HW) / VPU (h264_rkmpp) can read the physical memory with no CPU copy.
 */
int v4l2_capture_get_fd(v4l2_capture_t *ctx);

/**
 * @brief Stop the capture stream and release all resources
 * @param ctx  address of the capture context pointer, set to NULL after release
 */
void v4l2_capture_close(v4l2_capture_t **ctx);

/**
 * @brief Get the string name of the current pixel format
 * @param pixfmt V4L2 pixel format fourcc
 * @return format name, e.g. "NV12", "YUYV", "MJPEG"
 */
const char *v4l2_pixfmt_name(uint32_t pixfmt);

/**
 * @brief Auto-discover a USB camera device node
 * @param out_path output buffer for the device path, e.g. "/dev/video5"
 * @param path_len buffer length
 * @return 0=found, -1=not found
 * @note scans /dev/video0~31, checks for a USB UVC single-plane capture device,
 *       skipping multi-plane (MPLANE) devices such as rkisp
 */
int v4l2_find_usb_camera(char *out_path, int path_len);

/**
 * @brief DQBUF timeout (ms): max poll wait for a camera frame
 *
 * A normal 30fps camera produces a frame every ~33ms; 2000ms covers ~60 frames.
 * Timeout indicates camera hot-unplug / USB fault / ISP hang and should trigger
 * fault recovery.
 */
#define V4L2_DQBUF_TIMEOUT_MS  2000

#ifdef __cplusplus
}
#endif

#endif /* V4L2_CAPTURE_H */
