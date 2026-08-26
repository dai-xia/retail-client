#ifndef RGA_TRANSFORM_H
#define RGA_TRANSFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RGA pixel formats (RK3568 RGA2 HW supported)
 *   NV12: Y+UV half-plane, ISP native output; NOT NV21 (UV order reversed)
 */
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
 * RGA image buffer descriptor
 *   fd >= 0  -> DMA-BUF zero-copy (caller guarantees a valid dma-buf)
 *   fd  < 0  -> user virtual address vir_addr
 *   wstride  -> line stride, must be 16-byte aligned (RGA HW constraint, else corruption)
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
 * RGA one-shot: format conversion + scaling + crop
 *
 * @param src      source image
 * @param dst      dest image (caller sets format/width/height)
 * @param src_rect ROI crop [x,y,w,h], NULL = whole frame
 * @return 0=success, -1=failure
 *
 * HW constraints:
 *   - stride must be 16-byte aligned
 *   - out-of-bounds src_rect does NOT error -> caller must ensure x+w<=width && y+h<=height
 *   - concurrent calls require an external mutex (im2d is not thread-safe)
 */
int rga_transform_process(const rga_image_t *src,
                          rga_image_t *dst,
                          const int *src_rect);

/* Runtime probe: /dev/rga existence + USE_RGA compile macro */
bool rga_transform_available(void);

/* Allocate DMA-BUF via DRM dumb buffer for zero-copy RGA(write)->RKNN(read) */
int  rga_dma_buf_alloc(size_t size);
void rga_dma_buf_free(int fd);

#ifdef __cplusplus
}
#endif

#endif /* RGA_TRANSFORM_H */
