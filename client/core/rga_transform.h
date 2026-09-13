#ifndef RGA_TRANSFORM_H
#define RGA_TRANSFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RGA pixel formats (RK3568 RGA2 HW). NV12: Y+UV half-plane, NOT NV21. */
typedef enum {
    RGA_FMT_RGBA8888  = 0,
    RGA_FMT_BGRA8888,
    RGA_FMT_RGB888,
    RGA_FMT_BGR888,
    RGA_FMT_RGB565,
    RGA_FMT_NV12,
    RGA_FMT_NV16,
    RGA_FMT_YUYV,
    RGA_FMT_UYVY,
} rga_pixel_format;

/*
 * RGA image buffer descriptor.
 *   fd >= 0 -> DMA-BUF zero-copy; fd < 0 -> vir_addr.
 *   wstride must be 16-byte aligned (RGA HW constraint).
 */
typedef struct {
    int              fd;
    void            *vir_addr;
    int              width;
    int              height;
    int              wstride;   /* 0=compact, nonzero must be 16-aligned */
    int              hstride;   /* 0=height */
    rga_pixel_format format;
} rga_image_t;

/*
 * RGA one-shot: format conversion + scaling + crop.
 *   src_rect = ROI crop [x,y,w,h], NULL = whole frame.
 * Returns 0=success, -1=failure.
 * HW constraints: stride 16-byte aligned; OOB src_rect does not error
 * (caller ensures bounds); im2d not thread-safe -> external mutex.
 */
int rga_transform_process(const rga_image_t *src,
                          rga_image_t *dst,
                          const int *src_rect);

/* Runtime probe: /dev/rga existence + USE_RGA compile macro */
bool rga_transform_available(void);

/* Allocate DMA-BUF for zero-copy RGA(write)->RKNN(read). */
int  rga_dma_buf_alloc(size_t size);
void rga_dma_buf_free(int fd);

#ifdef __cplusplus
}
#endif

#endif /* RGA_TRANSFORM_H */
