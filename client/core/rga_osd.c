/**
 * @file rga_osd.c
 * @brief RK3568 RGA2 OSD (On-Screen Display) overlay
 *
 * Overlays face detection boxes, text labels and status info onto video
 * frames before encoding, for real-time visual feedback on the surveillance
 * stream.
 *
 * Render backends:
 *   1) RGA HW (USE_RGA): draw elements into an RGBA overlay buffer, then
 *      blend onto the NV12/BGR frame via RGA2 alpha blending (low CPU load).
 *   2) CPU fallback (no USE_RGA): operate directly on YUV/BGR pixels, manually
 *      compute YUV chroma and do per-pixel alpha blending.
 *
 * NV12 color space:
 *   NV12 = Y plane (full resolution) + UV half-plane (2x1 downsampled, CbCr
 *   interleaved). Each UV byte pair covers two horizontally adjacent pixels,
 *   so when drawing a rectangle the UV value must be written to the position
 *   matching both adjacent pixels.
 *
 *   Common YUV values (BT.601 full range):
 *     white:  Y=0xFF, Cb=0x80, Cr=0x80
 *     green:  Y=0x96, Cb=0x40, Cr=0x5D
 *     red:    Y=0x54, Cb=0x6A, Cr=0x34
 *     blue:   Y=0x1D, Cb=0xFF, Cr=0x6B
 *     yellow: Y=0xE1, Cb=0x50, Cr=0x22
 *
 * Text rendering:
 *   No font rendering here (no FreeType/Pango). Text is pre-rendered to an
 *   RGBA8888 bitmap by the caller (e.g. Qt/QPainter) and passed in via
 *   rga_osd_label_t. RGA path blends via improcess; CPU path blends per pixel.
 */

#include "rga_osd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "im2d.h"
#include "RgaUtils.h"
#include "rga.h"

/* ======================== OSD context ======================== */

struct rga_osd_ctx {
    int  frame_width;
    int  frame_height;

    uint8_t *overlay_buf;  /* RGBA8888 overlay buffer (frame size, allocated in create) */
    int      overlay_len;
};

/* ======================== Internal helper: rounded-rect test ======================== */

/* Test whether pixel (px,py) is inside a rounded rectangle.
 * The four corners are (radius x radius) squares; pixels whose distance from
 * the corner center exceeds radius are clipped. */
static bool is_in_rounded_rect(int px, int py, int w, int h, int radius)
{
    if (radius <= 0) return true;

    /* top-left */
    if (px < radius && py < radius) {
        int dx = radius - 1 - px;
        int dy = radius - 1 - py;
        return (dx * dx + dy * dy) <= (radius * radius);
    }
    /* top-right */
    if (px >= w - radius && py < radius) {
        int dx = px - (w - radius);
        int dy = radius - 1 - py;
        return (dx * dx + dy * dy) <= (radius * radius);
    }
    /* bottom-left */
    if (px < radius && py >= h - radius) {
        int dx = radius - 1 - px;
        int dy = py - (h - radius);
        return (dx * dx + dy * dy) <= (radius * radius);
    }
    /* bottom-right */
    if (px >= w - radius && py >= h - radius) {
        int dx = px - (w - radius);
        int dy = py - (h - radius);
        return (dx * dx + dy * dy) <= (radius * radius);
    }

    return true;
}

/* ======================== RGA path: draw onto overlay ======================== */

/* Draw a rectangle onto the RGBA overlay (CPU draws to overlay, RGA blends
 * afterwards). Overlay is RGBA8888 (4 bytes/pixel [R,G,B,A]); border/fill are
 * drawn fully opaque (alpha=255) and blended with IM_ALPHA_BLEND_SRC_OVER. */
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
            overlay[off + 3] = 0xFF;  /* fully opaque */
        }
    }
}

/* Draw a pre-rendered label bitmap onto the overlay; blends pixels whose
 * alpha > 0. */
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
                /* alpha blend onto overlay */
                int inv_a = 255 - a;
                uint8_t dst_a = overlay[dst_off + 3];

                /* Simplified: if overlay pixel is transparent, write src
                 * directly; otherwise standard over-blend */
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

/* Blend the RGBA overlay onto an NV12 frame via RGA2.
 * IM_ALPHA_BLEND_SRC_OVER: standard Porter-Duff "source over"
 *   out = src * src_alpha + dst * (1 - src_alpha)
 *   overlay alpha=0 leaves the target pixel untouched, alpha=255 overwrites it. */
static int rga_blend_overlay_to_nv12(rga_osd_t *ctx, int nv12_fd)
{
    int fw = ctx->frame_width;
    int fh = ctx->frame_height;

    /* RGA source: RGBA8888 overlay (CPU-drawn, virtual address) */
    rga_buffer_t src = wrapbuffer_virtualaddr(ctx->overlay_buf,
                                               fw, fh,
                                               RK_FORMAT_RGBA_8888);
    if (src.vir_addr == NULL) {
        fprintf(stderr, "[RGA_OSD] wrapbuffer source overlay failed\n");
        return -1;
    }

    /* RGA destination: NV12 frame (DMA-BUF fd, RGA writes HW buffer directly) */
    rga_buffer_t dst = wrapbuffer_fd(nv12_fd, fw, fh,
                                      RK_FORMAT_YCbCr_420_SP);
    if (dst.fd < 0) {
        fprintf(stderr, "[RGA_OSD] wrapbuffer_fd dest NV12 failed\n");
        return -1;
    }

    /* RGA improcess: alpha blend + format conversion
     * RGA2 reads overlay RGBA and the matching NV12 YUV per pixel, converts
     * overlay RGB to YUV, blends by alpha, and writes back to the NV12 frame. */
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

/* ======================== Public API ======================== */

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

    /* Allocate the RGBA8888 overlay once and reuse across frames (avoids
     * per-frame malloc/free). Size = width * height * 4.
     * Cleared to fully transparent (alpha=0) before each draw. */
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

    /* RGA path:
     * 1. clear overlay to fully transparent (alpha=0)
     * 2. draw all rects and labels onto overlay (CPU writes overlay)
     * 3. RGA improcess alpha-blends overlay onto the NV12 frame (DMA-BUF fd)
     * overlay uses virtual address (CPU), NV12 frame uses fd (RGA HW): the two
     * roles are separated. */
    memset(ctx->overlay_buf, 0, ctx->overlay_len);

    for (int i = 0; i < draw->rect_count; i++) {
        overlay_draw_rect(ctx->overlay_buf, fw, fh, &draw->rects[i]);
    }

    for (int i = 0; i < draw->label_count; i++) {
        overlay_draw_label(ctx->overlay_buf, fw, fh, &draw->labels[i]);
    }

    /* RGA alpha blend: overlay(RGBA8888 vir_addr) -> NV12(fd) */
    return rga_blend_overlay_to_nv12(ctx, nv12_fd);
}
