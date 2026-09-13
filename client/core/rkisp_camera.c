/**
 * @file rkisp_camera.c
 * @brief MIPI camera capture (rkaiq 3A + rkisp ISP + V4L2 MPLANE)
 */

#include "rkisp_camera.h"

#ifdef HAVE_RKAIQ

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "rk_aiq_user_api2_sysctl.h"

#define RKISP_BUFFER_COUNT 6   /* same as USB path */

/* NV12: single plane */
#define RKISP_NUM_PLANES 1

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

/* rkaiq 3A cleanup callbacks (avoid rkaiq header dep in v4l2 module) */
static void rkisp_aiq_stop(void *aiq_ctx)
{
    if (aiq_ctx)
        rk_aiq_uapi2_sysctl_stop((rk_aiq_sys_ctx_t *)aiq_ctx, false);
}

static void rkisp_aiq_deinit(void *aiq_ctx)
{
    if (aiq_ctx)
        rk_aiq_uapi2_sysctl_deinit((rk_aiq_sys_ctx_t *)aiq_ctx);
}

/* find rkisp MPLANE capture node; 0=found */
static int find_rkisp_device(char *out_path, int path_len)
{
    for (int i = 0; i < 32; i++) {
        char devpath[32];
        snprintf(devpath, sizeof(devpath), "/dev/video%d", i);
        int fd = open(devpath, O_RDWR);
        if (fd < 0) continue;

        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            close(fd);
            continue;
        }

        bool is_mplane = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
        bool is_rkisp  = strstr((const char *)cap.driver, "rkisp") != NULL;

        close(fd);

        if (is_mplane && is_rkisp) {
            snprintf(out_path, path_len, "%s", devpath);
            fprintf(stdout, "[RKISP] found MIPI ISP output node: %s (driver=%s, card=%s)\n",
                    devpath, cap.driver, cap.card);
            return 0;
        }
    }
    fprintf(stderr, "[RKISP] no rkisp MPLANE device found (scanned /dev/video0~31)\n");
    return -1;
}

/* MPLANE mmap + DMA-BUF export. NV12 single plane, but the planes[] interface is required. */
static bool init_mplane_buffers(v4l2_capture_t *ctx)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));

    /* kernel may return fewer buffers than requested */
    req.count  = RKISP_BUFFER_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "[RKISP] REQBUFS failed: %s\n", strerror(errno));
        return false;
    }

    /* need >=2 buffers */
    if (req.count < 2) {
        fprintf(stderr, "[RKISP] kernel only allocated %d buffers\n", req.count);
        return false;
    }

    ctx->buffer_count = req.count;
    ctx->buffers = (v4l2_buffer_t *)calloc(req.count, sizeof(v4l2_buffer_t));
    if (!ctx->buffers) return false;


    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        /* MPLANE: offset/length live in planes[] */
        struct v4l2_plane planes[RKISP_NUM_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.m.planes = planes;
        buf.length   = RKISP_NUM_PLANES;

        /* MPLANE: m.mem_offset/length in planes[0] */
        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[RKISP] QUERYBUF[%u] failed: %s\n", i, strerror(errno));
            return false;
        }

        /* MAP_SHARED required for V4L2 mmap */
        ctx->buffers[i].length = planes[0].length;
        ctx->buffers[i].start  = mmap(NULL, planes[0].length,
                                      PROT_READ | PROT_WRITE, MAP_SHARED,
                                      ctx->fd, planes[0].m.mem_offset);
        if (ctx->buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "[RKISP] mmap[%u] failed: %s\n", i, strerror(errno));
            return false;
        }

        /* EXPBUF: DMA-BUF fd for zero-copy VPU access; failure non-fatal */
        struct v4l2_exportbuffer exp;
        memset(&exp, 0, sizeof(exp));
        exp.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        exp.index = i;
        exp.flags = O_CLOEXEC;
        if (xioctl(ctx->fd, VIDIOC_EXPBUF, &exp) < 0) {
            fprintf(stderr, "[RKISP] EXPBUF[%u] failed: %s\n", i, strerror(errno));
            ctx->dma_fds[i] = -1;
        } else {
            ctx->dma_fds[i] = exp.fd;
        }
    }

    /* QBUF all before STREAMON */
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[RKISP_NUM_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.m.planes = planes;
        buf.length   = RKISP_NUM_PLANES;

        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[RKISP] QBUF[%u] failed: %s\n", i, strerror(errno));
            return false;
        }
    }

    fprintf(stdout, "[RKISP] MPLANE %d buffers ready, per-frame %u bytes\n",
            ctx->buffer_count, (unsigned)ctx->buffers[0].length);
    return true;
}

/* rkaiq init/prepare/start + S_FMT(NV12 MPLANE) + buffers + STREAMON.
 * On error all branches roll back. Caller must DQBUF in a loop after open. */
v4l2_capture_t *rkisp_camera_open(const char *iq_dir, int width, int height, int fps)
{
    char dev_path[32];
    if (find_rkisp_device(dev_path, sizeof(dev_path)) != 0)
        return NULL;

    v4l2_capture_t *ctx = (v4l2_capture_t *)calloc(1, sizeof(v4l2_capture_t));
    if (!ctx) return NULL;
    ctx->fd = -1;
    ctx->is_mplane = true;       /* MPLANE path */
    for (int i = 0; i < 8; i++) ctx->dma_fds[i] = -1;

    /* O_RDWR required */
    ctx->fd = open(dev_path, O_RDWR);
    if (ctx->fd < 0) {
        fprintf(stderr, "[RKISP] open %s failed: %s\n", dev_path, strerror(errno));
        free(ctx);
        return NULL;
    }

    /* rkaiq needs the sensor entity name to bind the IQ file */
    const char *sns_name = rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(dev_path);
    if (!sns_name || sns_name[0] == '\0') {
        fprintf(stderr, "[RKISP] cannot get sensor entity name\n");
        close(ctx->fd);
        free(ctx);
        return NULL;
    }
    fprintf(stdout, "[RKISP] sensor entity: %s\n", sns_name);

    /* load IQ files; NULL iq_dir -> /oem/etc/iqfiles */
    const char *iq_path = (iq_dir && iq_dir[0]) ? iq_dir : "/oem/etc/iqfiles";
    rk_aiq_sys_ctx_t *aiq = rk_aiq_uapi2_sysctl_init(sns_name, iq_path, NULL, NULL);
    if (!aiq) {
        fprintf(stderr, "[RKISP] rkaiq init failed (iq_dir=%s)\n", iq_path);
        close(ctx->fd);
        free(ctx);
        return NULL;
    }
    ctx->aiq_ctx    = aiq;
    ctx->aiq_stop   = rkisp_aiq_stop;
    ctx->aiq_deinit = rkisp_aiq_deinit;

    /* prepare: configures sensor->CSI->ISP media link; no manual subdev ioctls */
    if (rk_aiq_uapi2_sysctl_prepare(aiq, (uint32_t)width, (uint32_t)height,
                                    RK_AIQ_WORKING_MODE_NORMAL) != XCAM_RETURN_NO_ERROR) {
        fprintf(stderr, "[RKISP] rkaiq prepare failed\n");
        rk_aiq_uapi2_sysctl_deinit(aiq);
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    /* start 3A threads */
    if (rk_aiq_uapi2_sysctl_start(aiq) != XCAM_RETURN_NO_ERROR) {
        fprintf(stderr, "[RKISP] rkaiq start failed\n");
        rk_aiq_uapi2_sysctl_deinit(aiq);
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    /* S_FMT is bidirectional: save the returned aligned WxH */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = width;
    fmt.fmt.pix_mp.height      = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "[RKISP] S_FMT NV12 failed: %s\n", strerror(errno));
        rk_aiq_uapi2_sysctl_stop(aiq, false);
        rk_aiq_uapi2_sysctl_deinit(aiq);
        close(ctx->fd);
        free(ctx);
        return NULL;
    }
    ctx->width  = fmt.fmt.pix_mp.width;
    ctx->height = fmt.fmt.pix_mp.height;
    ctx->pixfmt = fmt.fmt.pix_mp.pixelformat;
    ctx->fps    = fps;

    if (!init_mplane_buffers(ctx)) {
        rk_aiq_uapi2_sysctl_stop(aiq, false);
        rk_aiq_uapi2_sysctl_deinit(aiq);
        v4l2_capture_close(&ctx);
        return NULL;
    }

    /* caller must DQBUF in a loop or frames drop */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "[RKISP] STREAMON failed: %s\n", strerror(errno));
        rk_aiq_uapi2_sysctl_stop(aiq, false);
        rk_aiq_uapi2_sysctl_deinit(aiq);
        v4l2_capture_close(&ctx);
        return NULL;
    }
    ctx->streaming = true;

    fprintf(stdout, "[RKISP] MIPI capture started: %dx%d@%dfps NV12\n",
            ctx->width, ctx->height, ctx->fps);
    return ctx;
}

#else  /* !HAVE_RKAIQ */

/* no rkaiq: return NULL, caller falls back to USB */
v4l2_capture_t *rkisp_camera_open(const char *iq_dir, int width, int height, int fps)
{
    (void)iq_dir; (void)width; (void)height; (void)fps;
    return NULL;
}

#endif /* HAVE_RKAIQ */
