/**
 * @file rga_transform.c
 * @brief RK3568 RGA2 image format conversion (librga im2d API)
 *
 * librga im2d is stateless; no instance handle is needed. Call
 * rga_transform_available() to probe, then rga_transform_process() to convert.
 *
 * WARNING: im2d is not thread-safe; concurrent rga_transform_process calls
 * must be serialized with an external mutex, otherwise HW racing causes
 * screen corruption / crash.
 *
 * Build: USE_RGA macro + librga library.
 */

#include "rga_transform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "im2d.h"
#include "RgaUtils.h"
#include "rga.h"

/* ======================== Internal helpers ======================== */

static int to_rga_format(rga_pixel_format fmt)
{
    switch (fmt) {
    case RGA_FMT_RGBA8888: return RK_FORMAT_RGBA_8888;
    case RGA_FMT_BGRA8888: return RK_FORMAT_BGRA_8888;
    case RGA_FMT_RGB888:   return RK_FORMAT_RGB_888;
    case RGA_FMT_BGR888:   return RK_FORMAT_BGR_888;
    case RGA_FMT_RGB565:   return RK_FORMAT_RGB_565;
    /* RK_FORMAT_YCbCr_420_SP = NV12 (Y+UV half-plane), NOT NV21 */
    case RGA_FMT_NV12:     return RK_FORMAT_YCbCr_420_SP;
    case RGA_FMT_NV16:     return RK_FORMAT_YCbCr_422_SP;
    case RGA_FMT_YUYV:     return RK_FORMAT_YUYV_422;
    case RGA_FMT_UYVY:     return RK_FORMAT_UYVY_422;
    default:               return -1;
    }
}

/* ======================== Public API ======================== */

/*
 * RGA one-shot: format conversion + scaling + crop.
 *   rga_image_t.fd >= 0 -> DMA-BUF zero-copy mode (wrapbuffer_fd)
 *   rga_image_t.fd  < 0 -> user virtual address mode (wrapbuffer_virtualaddr)
 *
 * HW constraints (must be obeyed):
 *   - wstride must be 16-byte aligned, otherwise improcess corrupts or fails
 *   - out-of-bounds src_rect does NOT error, image will be garbled; caller
 *     must guarantee bounds
 *   - concurrent calls require an external lock
 * Returns 0=success, -1=failure.
 */
int rga_transform_process(const rga_image_t *src,
                          rga_image_t *dst,
                          const int *src_rect)
{
    if (!src || !dst) return -1;

    int src_fmt = to_rga_format(src->format);
    int dst_fmt = to_rga_format(dst->format);
    if (src_fmt < 0 || dst_fmt < 0) {
        fprintf(stderr, "[RGA] unsupported format: src=%d dst=%d\n", src->format, dst->format);
        return -1;
    }

    int src_wstride = src->wstride > 0 ? src->wstride : src->width;
    int src_hstride = src->hstride > 0 ? src->hstride : src->height;
    int dst_wstride = dst->wstride > 0 ? dst->wstride : dst->width;
    int dst_hstride = dst->hstride > 0 ? dst->hstride : dst->height;

    rga_buffer_t rga_src, rga_dst;
    if (src->fd >= 0) {
        rga_src = wrapbuffer_fd(src->fd, src->width, src->height, src_fmt,
                                src_wstride, src_hstride);
    } else {
        rga_src = wrapbuffer_virtualaddr(src->vir_addr, src->width, src->height,
                                         src_fmt, src_wstride, src_hstride);
        if (!rga_src.vir_addr) {
            fprintf(stderr, "[RGA] src wrapbuffer failed: vir_addr=%p w=%d h=%d fmt=%d\n",
                    src->vir_addr, src->width, src->height, src_fmt);
            return -1;
        }
    }
    if (dst->fd >= 0) {
        rga_dst = wrapbuffer_fd(dst->fd, dst->width, dst->height, dst_fmt,
                                dst_wstride, dst_hstride);
    } else {
        rga_dst = wrapbuffer_virtualaddr(dst->vir_addr, dst->width, dst->height,
                                         dst_fmt, dst_wstride, dst_hstride);
        if (!rga_dst.vir_addr) {
            fprintf(stderr, "[RGA] dst wrapbuffer failed: vir_addr=%p w=%d h=%d fmt=%d\n",
                    dst->vir_addr, dst->width, dst->height, dst_fmt);
            return -1;
        }
    }

    /* ROI crop: crop_rect {x,y,w,h} all zero -> whole frame; otherwise take the
     * w x h region starting at [x,y]. Caller guarantees x+w<=width && y+h<=height. */
    im_rect crop_rect = {};
    if (src_rect) {
        crop_rect.x      = src_rect[0];
        crop_rect.y      = src_rect[1];
        crop_rect.width  = src_rect[2];
        crop_rect.height = src_rect[3];
    }

    /* improcess(src, dst, pat, srect, drect, prect, usage)
     *   srect: source crop region (zero = whole frame)
     *   drect: dest region (zero = fill dst)
     *   usage: IM_SYNC (blocking) */
    rga_buffer_t pat = {0};
    im_rect srect = crop_rect;
    im_rect drect = {0};
    im_rect prect = {0};
    IM_STATUS status = improcess(rga_src, rga_dst, pat,
                                 srect, drect, prect, IM_SYNC);
    if (status != IM_STATUS_SUCCESS) {
        fprintf(stderr, "[RGA] improcess failed: status=%d src=%dx%d->dst=%dx%d\n",
                status, src->width, src->height, dst->width, dst->height);
        return -1;
    }
    return 0;
}

/* Runtime probe: check /dev/rga existence + USE_RGA compile macro */
bool rga_transform_available(void)
{
    int fd = open("/dev/rga", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return false;
}

/*
 * Allocate DMA-BUF via dma_heap for zero-copy sharing across RGA/RKNN/VPU.
 *
 * Principle:
 *   1. open /dev/dma_heap/cma (contiguous physical memory; RGA/RKNN require it)
 *   2. DMA_HEAP_IOCTL_ALLOC returns a dma_buf fd directly
 *   3. close the heap fd (the dma_buf fd holds the reference; memory stays)
 *
 * Why not DRM dumb buffer:
 *   - dma_heap is the standard Linux 5.6+ DMA-BUF allocator
 *   - supports alignment + memory type selection; RGA/VPU usually need 64/128B
 *   - no need to open /dev/dri/card0, cleaner permissions
 *
 * DMA-BUF usage:
 *   - RGA writes (improcess via wrapbuffer_fd output)
 *   - RKNN reads (rknn_input.fd = dma_buf_fd, pass_through=1)
 *   - FFmpeg rkmpp encode (AVDRMFrameDescriptor)
 */

/* <linux/dma-heap.h> may be missing from headers, inline the ABI */
#include <linux/ioctl.h>
#define DMA_HEAP_IOC_MAGIC       'H'
#define DMA_HEAP_IOCTL_ALLOC     _IOWR(DMA_HEAP_IOC_MAGIC, 0, struct dma_heap_allocation_data)

struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
    __u64 alignment;
};

static int try_dma_heap_alloc(const char *name, size_t size, size_t alignment)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/dma_heap/%s", name);

    int heap_fd = open(path, O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) return -1;

    struct dma_heap_allocation_data alloc = {0};
    alloc.len        = size;
    alloc.fd_flags   = O_RDWR | O_CLOEXEC;
    alloc.heap_flags = 0;
    alloc.alignment  = alignment;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        fprintf(stderr, "[RGA] dma_heap %s ALLOC failed (size=%zu): %s\n",
                name, size, strerror(errno));
        close(heap_fd);
        return -1;
    }

    close(heap_fd);
    return alloc.fd;
}

int rga_dma_buf_alloc(size_t size)
{
    /* RGA2 usually needs 64/128B alignment; RKNN/VPU also prefer 128B */
    const size_t alignment = 128;

    /* Prefer cma (contiguous physical memory), best for RGA/RKNN zero-copy */
    int fd = try_dma_heap_alloc("cma", size, alignment);
    if (fd >= 0) {
        fprintf(stdout, "[RGA] DMA-BUF alloc(cma): size=%zu fd=%d\n", size, fd);
        return fd;
    }

    /* Try rk-named heap */
    fd = try_dma_heap_alloc("rk-dma-heap-cma", size, alignment);
    if (fd >= 0) {
        fprintf(stdout, "[RGA] DMA-BUF alloc(rk-dma-heap-cma): size=%zu fd=%d\n", size, fd);
        return fd;
    }

    /* Fallback to system heap (may be non-contiguous; some HW may not support) */
    fd = try_dma_heap_alloc("system", size, alignment);
    if (fd >= 0) {
        fprintf(stdout, "[RGA] DMA-BUF alloc(system): size=%zu fd=%d\n", size, fd);
        return fd;
    }

    fprintf(stderr, "[RGA] dma_heap all allocations failed (cma/rk/system), trying DRM fallback\n");

    /* Final fallback: legacy DRM dumb buffer */
    int drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "[RGA] DRM fallback also failed: %s\n", strerror(errno));
        return -1;
    }

    struct drm_mode_create_dumb create = {0};
    create.width  = size;
    create.height = 1;
    create.bpp    = 8;

    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        fprintf(stderr, "[RGA] DRM CREATE_DUMB failed: %s\n", strerror(errno));
        close(drm_fd);
        return -1;
    }

    int dma_fd = -1;
    if (drmPrimeHandleToFD(drm_fd, create.handle, DRM_CLOEXEC | O_RDWR, &dma_fd) < 0) {
        fprintf(stderr, "[RGA] DRM PrimeHandleToFD failed: %s\n", strerror(errno));
        struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        close(drm_fd);
        return -1;
    }

    struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
    drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    close(drm_fd);

    fprintf(stdout, "[RGA] DMA-BUF alloc(drm-fallback): size=%zu fd=%d\n", size, dma_fd);
    return dma_fd;
}

void rga_dma_buf_free(int fd)
{
    if (fd >= 0) close(fd);
}
