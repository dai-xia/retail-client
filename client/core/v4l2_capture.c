/**
 * @file v4l2_capture.c
 * @brief V4L2 MMAP zero-copy camera capture
 *
 * MMAP mode: kernel allocates DMA-capable contiguous physical memory and
 * mmap() maps it to user space; DMA writes directly, zero copy on read.
 */

#include "v4l2_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <linux/videodev2.h>

/* MMAP buffer count: >=6 for pipelining + AI detection to avoid buffer
 * exhaustion / frame drop when OSD or face detection is slow */
#define V4L2_BUFFER_COUNT  6

/* ======================== Internal helpers ======================== */

/* ioctl wrapper that retries on EINTR (frequent signals on embedded) */
static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

static bool check_capability(int fd)
{
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "[V4L2] VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "[V4L2] device does not support video capture\n");
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "[V4L2] device does not support streaming IO\n");
        return false;
    }

    fprintf(stdout, "[V4L2] device: %s, driver: %s, bus: %s\n",
            cap.card, cap.driver, cap.bus_info);
    return true;
}

/* Negotiate pixel format: prefer NV12 (RK ISP native), fall back to YUYV.
 * USB cameras usually only support YUYV; RGA converts it to NV12 afterwards. */
static bool negotiate_format(int fd, int width, int height, uint32_t *out_fmt)
{
    static const uint32_t try_fmts[] = {
        V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_YUYV,
    };
    static const char *try_names[] = { "NV12", "YUYV" };

    for (int i = 0; i < (int)(sizeof(try_fmts) / sizeof(try_fmts[0])); i++) {
        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));

        fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = width;
        fmt.fmt.pix.height      = height;
        fmt.fmt.pix.pixelformat = try_fmts[i];
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;

        if (xioctl(fd, VIDIOC_TRY_FMT, &fmt) < 0) {
            fprintf(stdout, "[V4L2] device does not support %s, trying next\n", try_names[i]);
            continue;
        }

        /* Driver may accept TRY_FMT but change pixelformat (e.g. UVC maps NV12
         * to MJPG). Verify the returned format matches the request. */
        if (fmt.fmt.pix.pixelformat != try_fmts[i]) {
            fprintf(stdout, "[V4L2] requested %s but driver returned %s, trying next\n",
                    try_names[i], v4l2_pixfmt_name(fmt.fmt.pix.pixelformat));
            continue;
        }

        if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            fprintf(stderr, "[V4L2] set %s format failed: %s\n", try_names[i], strerror(errno));
            continue;
        }

        /* S_FMT may also change the format; re-verify */
        if (fmt.fmt.pix.pixelformat != try_fmts[i]) {
            fprintf(stdout, "[V4L2] S_FMT requested %s but driver forced %s, trying next\n",
                    try_names[i], v4l2_pixfmt_name(fmt.fmt.pix.pixelformat));
            continue;
        }

        *out_fmt = fmt.fmt.pix.pixelformat;
        fprintf(stdout, "[V4L2] format negotiated: %ux%u %s\n",
                fmt.fmt.pix.width, fmt.fmt.pix.height,
                v4l2_pixfmt_name(*out_fmt));
        fprintf(stdout, "[V4L2] driver reports sizeimage=%u bytes (%.1f KB)\n",
                fmt.fmt.pix.sizeimage, fmt.fmt.pix.sizeimage / 1024.0);
        fprintf(stdout, "[V4L2] theoretical size=%u bytes (%.1f KB), bytesperline=%u\n",
                fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12
                    ? fmt.fmt.pix.width * fmt.fmt.pix.height * 3 / 2
                    : fmt.fmt.pix.width * fmt.fmt.pix.height * 2,
                fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12
                    ? (fmt.fmt.pix.width * fmt.fmt.pix.height * 3.0f / 2.0f) / 1024.0f
                    : (fmt.fmt.pix.width * fmt.fmt.pix.height * 2.0f) / 1024.0f,
                fmt.fmt.pix.bytesperline);

        if (*out_fmt != V4L2_PIX_FMT_NV12) {
            fprintf(stdout, "[V4L2] non-NV12 format (%s), RGA will convert to NV12\n",
                    v4l2_pixfmt_name(*out_fmt));
        }
        return true;
    }

    fprintf(stderr, "[V4L2] all format negotiations failed (tried NV12, YUYV)\n");
    return false;
}

/* Auto-discover a USB camera node by scanning /dev/video0~31:
 *   1. must support V4L2_CAP_VIDEO_CAPTURE (single-plane)
 *   2. skip V4L2_CAP_VIDEO_CAPTURE_MPLANE (rkisp virtual devices)
 *   3. must support V4L2_CAP_STREAMING (MMAP)
 *   4. prefer bus_info containing "usb" (real USB camera)
 * Returns 0=found, -1=not found */
int v4l2_find_usb_camera(char *out_path, int path_len)
{
    char devpath[32];
    int found = -1;

    for (int i = 0; i < 32; i++) {
        snprintf(devpath, sizeof(devpath), "/dev/video%d", i);
        int fd = open(devpath, O_RDWR);
        if (fd < 0) continue;

        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            close(fd);
            continue;
        }

        bool has_capture  = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;
        /* Skip multi-plane devices (rkisp virtual camera) */
        bool has_mplane   = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
        bool has_streaming = (cap.capabilities & V4L2_CAP_STREAMING) != 0;

        if (has_capture && !has_mplane && has_streaming) {
            fprintf(stdout, "[V4L2] found USB camera: %s (%s, bus=%s)\n",
                    devpath, cap.card, cap.bus_info);
            snprintf(out_path, path_len, "%s", devpath);
            found = 0;
            close(fd);
            break;
        }

        close(fd);
    }

    if (found < 0) {
        fprintf(stderr, "[V4L2] no USB camera found (scanned /dev/video0~31)\n");
    }
    return found;
}

/* Set capture framerate. VIDIOC_S_PARM is an OPTIONAL ioctl; many MIPI sensor
 * and USB camera drivers do not support runtime framerate change, so a failure
 * here is common and non-fatal — the program continues running. */
static bool set_framerate(int fd, int fps)
{
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));

    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    /* timeperframe is a fraction (numerator/denominator) to avoid float
     * precision loss; fps = denominator/numerator, e.g. 30fps -> 1/30 s */
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = fps;

    if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
        fprintf(stderr, "[V4L2] set framerate failed: %s (non-fatal, continuing)\n", strerror(errno));
        return false;
    }

    /* The driver rewrites parm in place with the actually effective framerate,
     * which may differ from the requested fps (e.g. request 30, sensor only
     * supports 25). Read the struct, do not trust the input value. */
    fprintf(stdout, "[V4L2] effective framerate: %d/%d fps\n",
            parm.parm.capture.timeperframe.denominator,
            parm.parm.capture.timeperframe.numerator);
    return true;
}


/* Initialize the MMAP buffer pool.
 * Flow: REQBUFS -> verify count -> allocate user array -> QUERYBUF + mmap per
 * buffer -> QBUF all into the kernel free queue. V4L2_MEMORY_MMAP means the
 * kernel allocates DMA-capable physical memory and user space mmap()s it
 * (preferred over USERPTR which requires CPU copy). */
static bool init_mmap_buffers(v4l2_capture_t *ctx)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));

    req.count  = V4L2_BUFFER_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    /* REQBUFS: the kernel may adjust req.count based on hardware limits */
    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "[V4L2] VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        return false;
    }

    /* Pipelining needs >=2 buffers: one for DMA to fill, one for the app to
     * process in parallel */
    if (req.count < 2) {
        fprintf(stderr, "[V4L2] kernel only allocated %d buffers (need at least 2)\n", req.count);
        return false;
    }

    /* Save the actually allocated count; do not reuse V4L2_BUFFER_COUNT */
    ctx->buffer_count = req.count;
    ctx->buffers = (v4l2_buffer_t *)calloc(req.count, sizeof(v4l2_buffer_t));
    if (!ctx->buffers) {
        fprintf(stderr, "[V4L2] buffer descriptor array alloc failed\n");
        return false;
    }

    /* Per-buffer: QUERYBUF + mmap */
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));

        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        /* QUERYBUF returns buf.m.offset (MMAP offset) and buf.length;
         * without the offset mmap cannot be done. */
        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[V4L2] VIDIOC_QUERYBUF[%u] failed: %s\n", i, strerror(errno));
            goto fail_mmap;
        }

        ctx->buffers[i].length = buf.length;

        fprintf(stdout, "[V4L2] buffer[%u]: offset=0x%x length=%u bytes (%.1f KB)\n",
                i, buf.m.offset, buf.length, buf.length / 1024.0f);

        /* mmap maps kernel physical DMA memory to user virtual address.
         * MAP_SHARED so kernel DMA and user app access the same physical page. */
        ctx->buffers[i].start  = mmap(
            NULL,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            ctx->fd,
            buf.m.offset
        );

        if (ctx->buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "[V4L2] mmap[%u] failed: %s\n", i, strerror(errno));
            /* Roll back already-mapped buffers */
            for (unsigned int j = 0; j < i; j++) {
                munmap(ctx->buffers[j].start, ctx->buffers[j].length);
            }
            goto fail_mmap;
        }
    }

    /* QBUF all buffers into the kernel free queue; DMA fills them and moves
     * them to the ready queue for DQBUF */
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[V4L2] VIDIOC_QBUF[%u] failed: %s\n", i, strerror(errno));
            goto fail_mmap;
        }
    }

    fprintf(stdout, "[V4L2] MMAP %d buffers mapped, each %u bytes (%.1f KB)\n",
            ctx->buffer_count, (unsigned)ctx->buffers[0].length,
            ctx->buffers[0].length / 1024.0f);
    fprintf(stdout, "[V4L2] expected NV12 size=%u bytes (%.1f KB), actual/theory=%.1fx\n",
            ctx->width * ctx->height * 3 / 2,
            (ctx->width * ctx->height * 3.0f / 2.0f) / 1024.0f,
            (float)ctx->buffers[0].length / (ctx->width * ctx->height * 1.5f));
    return true;

fail_mmap:
    free(ctx->buffers);
    ctx->buffers = NULL;
    ctx->buffer_count = 0;
    return false;
}

/* ======================== Public API ======================== */

v4l2_capture_t *v4l2_capture_open(const char *devname, int width, int height, int fps)
{
    if (!devname) return NULL;

    v4l2_capture_t *ctx = (v4l2_capture_t *)calloc(1, sizeof(v4l2_capture_t));
    if (!ctx) return NULL;

    ctx->fd = -1;
    ctx->last_dequeued_index = -1;
    for (int i = 0; i < 8; i++) ctx->dma_fds[i] = -1;
    ctx->streaming = false;

    ctx->fd = open(devname, O_RDWR);
    if (ctx->fd < 0) {
        fprintf(stderr, "[V4L2] open device %s failed: %s\n", devname, strerror(errno));
        goto fail;
    }

    if (!check_capability(ctx->fd)) goto fail;

    if (!negotiate_format(ctx->fd, width, height, &ctx->pixfmt)) goto fail;

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_G_FMT, &fmt) == 0) {
        ctx->width  = fmt.fmt.pix.width;
        ctx->height = fmt.fmt.pix.height;
    } else {
        ctx->width  = width;
        ctx->height = height;
    }

    set_framerate(ctx->fd, fps);
    ctx->fps = fps;

    if (!init_mmap_buffers(ctx)) goto fail;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "[V4L2] VIDIOC_STREAMON failed: %s\n", strerror(errno));
        goto fail;
    }
    ctx->streaming = true;

    fprintf(stdout, "[V4L2] capture started: %dx%d@%dfps %s (MMAP zero-copy)\n",
            ctx->width, ctx->height, ctx->fps, v4l2_pixfmt_name(ctx->pixfmt));
    return ctx;

fail:
    v4l2_capture_close(&ctx);
    return NULL;
}


/* DQBUF one captured frame (zero-copy via mmap).
 * Polls with timeout before DQBUF to avoid blocking forever when the camera
 * is disconnected. After processing, the caller MUST call v4l2_capture_enqueue
 * to return the buffer to the kernel, otherwise capture stalls. */
int v4l2_capture_dequeue(v4l2_capture_t *ctx, void **out_buf, size_t *out_len)
{
    if (!ctx || !ctx->streaming || !out_buf || !out_len)
        return -1;

    /* Poll with timeout: in blocking mode DQBUF would sleep forever if no
     * frame is ready, so a camera hot-unplug / USB disconnect would hang the
     * thread. Poll first, then DQBUF once a frame is ready. */
    struct pollfd pfd = {
        .fd = ctx->fd,
        .events = POLLIN | POLLRDNORM
    };

    int poll_ret = poll(&pfd, 1, V4L2_DQBUF_TIMEOUT_MS);
    if (poll_ret == 0) {
        /* Timeout: likely camera disconnected / stream broken */
        fprintf(stderr, "[V4L2] DQBUF poll timeout (%dms), camera may be disconnected\n",
                V4L2_DQBUF_TIMEOUT_MS);
        return -1;
    }
    if (poll_ret < 0) {
        fprintf(stderr, "[V4L2] poll failed: %s\n", strerror(errno));
        return -1;
    }

    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    memset(&buf, 0, sizeof(buf));
    buf.type   = ctx->is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ctx->is_mplane) {
        memset(planes, 0, sizeof(planes));
        buf.m.planes = planes;
        buf.length   = 1;
    }

    if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
        fprintf(stderr, "[V4L2] VIDIOC_DQBUF failed: %s\n", strerror(errno));
        return -1;
    }

    if (buf.index >= (unsigned)ctx->buffer_count) {
        fprintf(stderr, "[V4L2] DQBUF returned invalid index: %u\n", buf.index);
        return -1;
    }

    *out_buf = ctx->buffers[buf.index].start;
    /* MPLANE: bytesused is in planes[0]; single-plane: in buf.bytesused */
    *out_len = ctx->is_mplane ? buf.m.planes[0].bytesused : buf.bytesused;
    ctx->last_dequeued_index = (int)buf.index;

    return 0;
}

int v4l2_capture_enqueue(v4l2_capture_t *ctx)
{
    if (!ctx || !ctx->streaming) return -1;
    if (ctx->last_dequeued_index < 0) return -1;

    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    memset(&buf, 0, sizeof(buf));
    buf.type   = ctx->is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = (unsigned)ctx->last_dequeued_index;
    if (ctx->is_mplane) {
        memset(planes, 0, sizeof(planes));
        buf.m.planes = planes;
        buf.length   = 1;
    }

    /* QBUF: return the buffer to the kernel so DMA can write into it again */
    if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "[V4L2] VIDIOC_QBUF failed: %s\n", strerror(errno));
        return -1;
    }

    ctx->last_dequeued_index = -1;
    return 0;
}


int v4l2_capture_get_fd(v4l2_capture_t *ctx)
{
    if (!ctx) return -1;
    int idx = ctx->last_dequeued_index;
    if (idx < 0) return -1;

    /* Already exported, reuse the cached fd */
    if (ctx->dma_fds[idx] > 0) return ctx->dma_fds[idx];

    /* VIDIOC_EXPBUF: export the MMAP buffer as a cross-device DMA-BUF fd.
     * RGA and VPU access the same physical memory via fd with no CPU copy. */
    struct v4l2_exportbuffer exp;
    memset(&exp, 0, sizeof(exp));
    exp.type  = ctx->is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                               : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    exp.index = idx;
    exp.flags = O_CLOEXEC;

    if (ioctl(ctx->fd, VIDIOC_EXPBUF, &exp) < 0) {
        fprintf(stderr, "[V4L2] VIDIOC_EXPBUF failed idx=%d: %s\n", idx, strerror(errno));
        return -1;
    }
    ctx->dma_fds[idx] = exp.fd;
    return exp.fd;
}

void v4l2_capture_close(v4l2_capture_t **pctx)
{
    if (!pctx || !*pctx) return;
    v4l2_capture_t *ctx = *pctx;

    /* MIPI path: stop rkaiq 3A loop BEFORE STREAMOFF (so 3A stops touching ISP) */
    if (ctx->aiq_ctx && ctx->aiq_stop)
        ctx->aiq_stop(ctx->aiq_ctx);

    if (ctx->streaming && ctx->fd >= 0) {
        enum v4l2_buf_type type = ctx->is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                                 : V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        ctx->streaming = false;
    }

    /* MIPI path: release rkaiq context */
    if (ctx->aiq_ctx && ctx->aiq_deinit)
        ctx->aiq_deinit(ctx->aiq_ctx);

    /* munmap: length field always holds the original mapping length */
    if (ctx->buffers) {
        for (int i = 0; i < ctx->buffer_count; i++) {
            if (ctx->buffers[i].start && ctx->buffers[i].start != MAP_FAILED) {
                munmap(ctx->buffers[i].start, ctx->buffers[i].length);
            }
        }
        free(ctx->buffers);
        ctx->buffers = NULL;
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }

    /* Close exported DMA-BUF fds */
    for (int i = 0; i < 8; i++) {
        if (ctx->dma_fds[i] > 0) close(ctx->dma_fds[i]);
    }

    free(ctx);
    *pctx = NULL;
    fprintf(stdout, "[V4L2] device closed, resources released\n");
}

const char *v4l2_pixfmt_name(uint32_t pixfmt)
{
    switch (pixfmt) {
    case V4L2_PIX_FMT_NV12:  return "NV12";
    case V4L2_PIX_FMT_NV16:  return "NV16";
    case V4L2_PIX_FMT_YUYV:  return "YUYV";
    case V4L2_PIX_FMT_UYVY:  return "UYVY";
    case V4L2_PIX_FMT_MJPEG: return "MJPEG";
    case V4L2_PIX_FMT_JPEG:  return "JPEG";
    case V4L2_PIX_FMT_RGB24: return "RGB24";
    case V4L2_PIX_FMT_BGR24: return "BGR24";
    default: {
        static char unknown[8];
        snprintf(unknown, sizeof(unknown), "%c%c%c%c",
                 pixfmt & 0xFF, (pixfmt >> 8) & 0xFF,
                 (pixfmt >> 16) & 0xFF, (pixfmt >> 24) & 0xFF);
        return unknown;
    }
    }
}
