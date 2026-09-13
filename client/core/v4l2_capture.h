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

    /* MIPI only (filled by rkisp_camera) */
    void            *aiq_ctx;             /* rk_aiq_sys_ctx_t* (opaque, avoids v4l2 module depending on rkaiq) */
    void           (*aiq_stop)(void *);   /* stop 3A callback (called before STREAMOFF) */
    void           (*aiq_deinit)(void *); /* release 3A context callback (called after STREAMOFF) */
} v4l2_capture_t;

/**
 * @brief Open a V4L2 device and initialize MMAP capture
 * @note prefers NV12/NV16 (RK3568 ISP native), falls back to YUYV/MJPEG
 */
v4l2_capture_t *v4l2_capture_open(const char *devname, int width, int height, int fps);

/**
 * @brief Read one frame from the capture stream (DQBUF)
 * @note caller MUST call v4l2_capture_enqueue afterwards to return the buffer
 */
int v4l2_capture_dequeue(v4l2_capture_t *ctx, void **out_buf, size_t *out_len);

/** @brief Return a processed buffer (QBUF), allowing DMA to write again */
int v4l2_capture_enqueue(v4l2_capture_t *ctx);

/**
 * @brief Export the DMA-BUF fd of the currently dequeued buffer (zero-copy path)
 * @note must be called after DQBUF and before QBUF; lets RGA/VPU read the MMAP buffer with no CPU copy
 */
int v4l2_capture_get_fd(v4l2_capture_t *ctx);

/** @brief Stop the capture stream and release all resources */
void v4l2_capture_close(v4l2_capture_t **ctx);

/** @brief Get the string name of the current pixel format */
const char *v4l2_pixfmt_name(uint32_t pixfmt);

/**
 * @brief Auto-discover a USB camera device node
 * @note skips MPLANE devices (rkisp); returns 0=found, -1=not found
 */
int v4l2_find_usb_camera(char *out_path, int path_len);

/** @brief DQBUF timeout (ms): max poll wait for a camera frame */
#define V4L2_DQBUF_TIMEOUT_MS  2000

#ifdef __cplusplus
}
#endif

#endif /* V4L2_CAPTURE_H */
