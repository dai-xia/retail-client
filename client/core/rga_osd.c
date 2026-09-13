/**
 * @file rga_osd.c
 * @brief RK3568 RGA2 OSD overlay onto NV12/BGR frames.
 */

#include "rga_osd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "im2d.h"
#include "RgaUtils.h"
#include "rga.h"

struct rga_osd_ctx {
    int  frame_width;
    int  frame_height;

    uint8_t *overlay_buf;  /* RGBA8888 overlay buffer */
    int      overlay_len;
};

/* True if (px,py) is inside the rounded rectangle. */
static bool is_in_rounded_rect(int px, int py, int w, int h, int radius)
{
    if (radius <= 0) return true;

    if (px < radius && py < radius) {
        int dx = radius - 1 - px;
        int dy = radius - 1 - py;
        return (dx * dx + dy * dy) <= (radius * radius);
    }
    if (px >= w - radius && py < radius) {
        int dx = px - (w - radius);
        int dy = radius - 1 - py;
        return (dx * dx + dy * dy) <= (radius * radius);
    }
    if (px < radius && py >= h - radius) {
        int dx = radius - 1 - px;
        int dy = py - (h - radius);
        return (dx * dx + dy * dy) <= (radius * radius);
    }
    if (px >= w - radius && py >= h - radius) {
        int dx = px - (w - radius);
        int dy = py - (h - radius);
        return (dx * dx + dy * dy) <= (radius * radius);
    }

    return true;
}

/* Draw a rect onto the RGBA overlay (border/fill fully opaque). */
static void overlay_draw_rect(uint8_t *overlay, int fw, int fh,
                              const rga_osd_rect_t *rect)
{
    int x0 = rect->x < 0 ? 0 : rect->x;
    int y0 = rect->y < 0 ? 0 : rect->y;
    int x1 = rect->x + rect->w;
    int y1 = rect->y + rect->h;
    if (x1 > fw) x1 = fw;
    if (y1 > fh) y1 = fh;
    if (x0 >= x1 || y0 >= y1) return;

    int thickness = rect->thickness;
    int radius   = rect->corner_radius;

    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            int lx = px - rect->x;
            int ly = py - rect->y;
            int rw = rect->w;
            int rh = rect->h;

            if (thickness > 0) {
                bool is_border = (ly < thickness) ||
                                 (ly >= rh - thickness) ||
                                 (lx < thickness) ||
                                 (lx >= rw - thickness);
                if (!is_border) continue;
            }

            if (!is_in_rounded_rect(lx, ly, rw, rh, radius)) continue;

            int off = (py * fw + px) * 4;
            overlay[off + 0] = rect->r;
            overlay[off + 1] = rect->g;
            overlay[off + 2] = rect->b;
            overlay[off + 3] = 0xFF;
        }
    }
}

/* Draw a pre-rendered label bitmap onto the overlay (blends alpha>0 pixels). */
static void overlay_draw_label(uint8_t *overlay, int fw, int fh,
                               const rga_osd_label_t *label)
{
    if (!label->bitmap || label->bmp_width <= 0 || label->bmp_height <= 0)
        return;

    for (int ly = 0; ly < label->bmp_height; ly++) {
        int fy = label->y + ly;
        if (fy < 0 || fy >= fh) continue;

        for (int lx = 0; lx < label->bmp_width; lx++) {
            int fx = label->x + lx;
            if (fx < 0 || fx >= fw) continue;

            int src_off = (ly * label->bmp_width + lx) * 4;
            uint8_t a = label->bitmap[src_off + 3];
            if (a == 0) continue;

            int dst_off = (fy * fw + fx) * 4;

            if (a == 255) {
                overlay[dst_off + 0] = label->bitmap[src_off + 0];
                overlay[dst_off + 1] = label->bitmap[src_off + 1];
                overlay[dst_off + 2] = label->bitmap[src_off + 2];
                overlay[dst_off + 3] = 0xFF;
            } else {
                int inv_a = 255 - a;
                uint8_t dst_a = overlay[dst_off + 3];

                /* Transparent dest: write src directly; else standard over-blend. */
                if (dst_a == 0) {
                    overlay[dst_off + 0] = label->bitmap[src_off + 0];
                    overlay[dst_off + 1] = label->bitmap[src_off + 1];
                    overlay[dst_off + 2] = label->bitmap[src_off + 2];
                    overlay[dst_off + 3] = a;
                } else {
                    overlay[dst_off + 0] = (uint8_t)((label->bitmap[src_off + 0] * a + overlay[dst_off + 0] * inv_a + 127) / 255);
                    overlay[dst_off + 1] = (uint8_t)((label->bitmap[src_off + 1] * a + overlay[dst_off + 1] * inv_a + 127) / 255);
                    overlay[dst_off + 2] = (uint8_t)((label->bitmap[src_off + 2] * a + overlay[dst_off + 2] * inv_a + 127) / 255);
                    overlay[dst_off + 3] = (uint8_t)(255 - (255 - a) * (255 - dst_a) / 255);
                }
            }
        }
    }
}

/* Blend RGBA overlay onto NV12 frame via RGA2 (alpha=0 leaves pixel, 255 overwrites). */
static int rga_blend_overlay_to_nv12(rga_osd_t *ctx, int nv12_fd)
{
    int fw = ctx->frame_width;
    int fh = ctx->frame_height;

    rga_buffer_t src = wrapbuffer_virtualaddr(ctx->overlay_buf,
                                               fw, fh,
                                               RK_FORMAT_RGBA_8888);
    if (src.vir_addr == NULL) {
        fprintf(stderr, "[RGA_OSD] wrapbuffer source overlay failed\n");
        return -1;
    }

    rga_buffer_t dst = wrapbuffer_fd(nv12_fd, fw, fh,
                                      RK_FORMAT_YCbCr_420_SP);
    if (dst.fd < 0) {
        fprintf(stderr, "[RGA_OSD] wrapbuffer_fd dest NV12 failed\n");
        return -1;
    }

    int usage_blend = IM_ALPHA_BLEND_SRC_OVER | IM_SYNC;
    IM_STATUS status = improcess(src, dst, (rga_buffer_t){0},
                                 (im_rect){0}, (im_rect){0}, (im_rect){0},
                                 usage_blend);

    if (status != IM_STATUS_SUCCESS) {
        fprintf(stderr, "[RGA_OSD] improcess RGBA->NV12 alpha blend failed, status=%d\n", status);
        return -1;
    }

    return 0;
}

rga_osd_t *rga_osd_create(int frame_width, int frame_height)
{
    if (frame_width <= 0 || frame_height <= 0) {
        fprintf(stderr, "[RGA_OSD] invalid frame size: %dx%d\n", frame_width, frame_height);
        return NULL;
    }

    rga_osd_t *ctx = (rga_osd_t *)calloc(1, sizeof(rga_osd_t));
    if (!ctx) {
        fprintf(stderr, "[RGA_OSD] memory allocation failed\n");
        return NULL;
    }

    ctx->frame_width  = frame_width;
    ctx->frame_height = frame_height;

    /* RGBA8888 overlay reused across frames; cleared (alpha=0) before each draw. */
    ctx->overlay_len = frame_width * frame_height * 4;
    ctx->overlay_buf = (uint8_t *)calloc(1, ctx->overlay_len);
    if (!ctx->overlay_buf) {
        fprintf(stderr, "[RGA_OSD] overlay buffer allocation failed (%d bytes)\n", ctx->overlay_len);
        free(ctx);
        return NULL;
    }
    fprintf(stdout, "[RGA_OSD] RGA HW OSD initialized, frame size=%dx%d, overlay=%d bytes\n",
            frame_width, frame_height, ctx->overlay_len);

    return ctx;
}

void rga_osd_destroy(rga_osd_t **pctx)
{
    if (!pctx || !*pctx) return;
    rga_osd_t *ctx = *pctx;

    if (ctx->overlay_buf) {
        free(ctx->overlay_buf);
        ctx->overlay_buf = NULL;
    }

    free(ctx);
    *pctx = NULL;
    fprintf(stdout, "[RGA_OSD] resources released\n");
}

int rga_osd_draw_nv12(rga_osd_t *ctx, int nv12_fd,
                       const rga_osd_draw_t *draw)
{
    if (!ctx || nv12_fd < 0 || !draw) return -1;

    int fw = ctx->frame_width;
    int fh = ctx->frame_height;

    /* Clear overlay, draw rects/labels, RGA-blend onto NV12 fd. */
    memset(ctx->overlay_buf, 0, ctx->overlay_len);

    for (int i = 0; i < draw->rect_count; i++) {
        overlay_draw_rect(ctx->overlay_buf, fw, fh, &draw->rects[i]);
    }

    for (int i = 0; i < draw->label_count; i++) {
        overlay_draw_label(ctx->overlay_buf, fw, fh, &draw->labels[i]);
    }

    return rga_blend_overlay_to_nv12(ctx, nv12_fd);
}
