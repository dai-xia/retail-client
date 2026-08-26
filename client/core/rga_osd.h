#ifndef RGA_OSD_H
#define RGA_OSD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OSD rectangle style */
typedef struct {
    int   x, y, w, h;            /* rectangle region (pixels) */
    uint8_t r, g, b;             /* border color RGB */
    int   thickness;             /* line width (1~4 px), 0=fill */
    int   corner_radius;         /* corner radius (0=square) */
} rga_osd_rect_t;

/* OSD text label (pre-rendered bitmap, no font rendering needed) */
typedef struct {
    int       x, y;               /* top-left position */
    uint8_t  *bitmap;             /* RGBA8888 pre-rendered bitmap (caller renders via Qt/QPainter) */
    int       bmp_width;
    int       bmp_height;
} rga_osd_label_t;

/* OSD draw request (a batch of overlay elements) */
typedef struct {
    rga_osd_rect_t  *rects;
    int              rect_count;
    rga_osd_label_t *labels;
    int              label_count;
} rga_osd_draw_t;

/* OSD context (opaque) */
typedef struct rga_osd_ctx rga_osd_t;

/* Create / destroy */
rga_osd_t* rga_osd_create(int frame_width, int frame_height);
void       rga_osd_destroy(rga_osd_t **ctx);

/* Overlay OSD elements onto an NV12 frame
 * nv12_fd: DMA-BUF fd of the NV12 frame (RGA writes the HW buffer via fd)
 * overlay drawn by CPU, NV12 frame operated by RGA via fd; CPU never touches frame data */
int rga_osd_draw_nv12(rga_osd_t *ctx, int nv12_fd,
                       const rga_osd_draw_t *draw);

#ifdef __cplusplus
}
#endif
#endif
