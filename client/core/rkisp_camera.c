/**
 * @file rkisp_camera.c
 * @brief MIPI camera capture (rkaiq 3A + rkisp ISP + V4L2 MPLANE)
 *
 * RK3568 MIPI cameras differ from USB (UVC) cameras:
 *   - MIPI sensors output RAW bayer data; ISP demosaic + 3A is required to
 *     produce NV12.
 *   - The ISP output node is MPLANE (multi-plane); v4l2_capture.c only handles
 *     single-plane, so this module uses the MPLANE path.
 *   - 3A (AE/AWB/AF) must be driven by rkaiq at runtime, otherwise the image is
 *     black or severely miscolored.
 *
 * Link: OV5695(RAW10) -> MIPI-CSI -> rkisp(demosaic+3A+NV12) -> /dev/videoX(MPLANE).
 * rkaiq prepare configures the sensor->CSI->ISP main link (equivalent to
 * media-ctl), so no manual media-ctl calls are needed.
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

#define RKISP_BUFFER_COUNT 6   /* Buffer count, same as USB path */

/* NV12 single plane (Y and UV contiguous), MPLANE mode plane count = 1 */
#define RKISP_NUM_PLANES 1

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

/* ---- rkaiq 3A cleanup callbacks (called by v4l2_capture_close to avoid
 * depending on the rkaiq header in the v4l2 module) ---- */
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

/* Scan /dev/video* for the rkisp ISP main output node (MPLANE capture device).
 * Returns 0=found, -1=not found */
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

        /* Must be an MPLANE capture device (rkisp output); skip single-plane */
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

/* MPLANE mmap buffer allocation + DMA-BUF export.
 * RKISP is a multi-plane device and only supports
 * V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; even though NV12 has a single plane, the
 * planes[] array interface must be used, otherwise ioctl returns -EINVAL. */
static bool init_mplane_buffers(v4l2_capture_t *ctx)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));

    /* REQBUFS: kernel may return fewer than requested count.
     * type MUST be MPLANE for RKISP; USERPTR/DMABUF are alternatives to MMAP. */
    req.count  = RKISP_BUFFER_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "[RKISP] REQBUFS failed: %s\n", strerror(errno));
        return false;
    }

    /* >=2 buffers needed for pipelining (1 filling, 1 processing); kernel may
     * lower count under resource pressure */
    if (req.count < 2) {
        fprintf(stderr, "[RKISP] kernel only allocated %d buffers\n", req.count);
        return false;
    }

    ctx->buffer_count = req.count;
    ctx->buffers = (v4l2_buffer_t *)calloc(req.count, sizeof(v4l2_buffer_t));
    if (!ctx->buffers) return false;


    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        /* MPLANE: offset/length are in planes[], not in buf top level.
         * NV12 has 1 plane but the array interface is still required. */
        struct v4l2_plane planes[RKISP_NUM_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.m.planes = planes;
        buf.length   = RKISP_NUM_PLANES;

        /* QUERYBUF: MPLANE stores m.mem_offset and length in planes[0], NOT
         * buf.m.offset (single-plane examples read the wrong field here). */
        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[RKISP] QUERYBUF[%u] failed: %s\n", i, strerror(errno));
            return false;
        }

        /* planes[0].m.mem_offset is the mmap offset (not a physical address).
         * MAP_SHARED is mandatory for V4L2 mmap. */
        ctx->buffers[i].length = planes[0].length;
        ctx->buffers[i].start  = mmap(NULL, planes[0].length,
                                      PROT_READ | PROT_WRITE, MAP_SHARED,
                                      ctx->fd, planes[0].m.mem_offset);
        if (ctx->buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "[RKISP] mmap[%u] failed: %s\n", i, strerror(errno));
            return false;
        }

        /* EXPBUF: export DMA-BUF fd so RGA/RKNN/VPU access the physical memory
         * directly (zero-copy). O_CLOEXEC avoids fd leak across exec.
         * Failure here is non-fatal; dma_fds[i] is marked -1. */
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

    /* QBUF all buffers into the kernel free queue before STREAMON. MPLANE QBUF
     * also requires buf.m.planes and buf.length to be filled. */
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

/* Open RKISP MIPI camera: AIQ init, link config, V4L2 format, buffers, start.
 * @param iq_dir IQ file dir; NULL/empty uses system default /oem/etc/iqfiles.
 * Pipeline:
 *   1. find rkisp main output /dev/videoX
 *   2. open the ISP YUV output node (MPLANE capture, not subdev)
 *   3. reverse-lookup the sensor entity name bound to the video node for rkaiq
 *   4. rk_aiq_uapi2_sysctl_init: load IQ files (3A, lens shading, ISP params)
 *   5. rk_aiq_uapi2_sysctl_prepare: configure media link (sensor-dphy-cif-isp),
 *      sensor resolution / MIPI timing
 *   6. rk_aiq_uapi2_sysctl_start: launch 3A threads (AE/AWB/AF)
 *   7. VIDIOC_S_FMT: set NV12 MPLANE output; kernel returns aligned WxH
 *   8. init_mplane_buffers: REQBUFS/QUERYBUF/mmap/EXPBUF/QBUF
 *   9. VIDIOC_STREAMON: start MIPI-CSI/ISP hardware, capture begins
 * On error all branches roll back (stop/deinit aiq, close fd, free memory).
 * Note: STREAMON is done inside open(); the caller must DQBUF in a loop
 * afterwards or frames will be dropped. */
v4l2_capture_t *rkisp_camera_open(const char *iq_dir, int width, int height, int fps)
{
    char dev_path[32];
    /* Scan v4l2 devices to find the rkisp_mainpath /dev/videoX node
     * (ISP NV12 output, MPLANE, not a subdev or rawrd node) */
    if (find_rkisp_device(dev_path, sizeof(dev_path)) != 0)
        return NULL;

    v4l2_capture_t *ctx = (v4l2_capture_t *)calloc(1, sizeof(v4l2_capture_t));
    if (!ctx) return NULL;
    ctx->fd = -1;
    ctx->is_mplane = true;       /* MPLANE capture device for the whole path */
    for (int i = 0; i < 8; i++) ctx->dma_fds[i] = -1;

    /* 1. Open ISP YUV output node; O_RDWR required (some ioctls fail read-only) */
    ctx->fd = open(dev_path, O_RDWR);
    if (ctx->fd < 0) {
        fprintf(stderr, "[RKISP] open %s failed: %s\n", dev_path, strerror(errno));
        free(ctx);
        return NULL;
    }

    /* 2. rkaiq: reverse-lookup the sensor entity name bound to the video node.
     * Each sensor subdev has an entity name (e.g. "m01_f_ov5695 1-0036");
     * rk_aiq_uapi2_sysctl_init needs it to bind the matching IQ file.
     * Without it rkaiq cannot tell which camera is being driven and fails. */
    const char *sns_name = rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(dev_path);
    if (!sns_name || sns_name[0] == '\0') {
        fprintf(stderr, "[RKISP] cannot get sensor entity name\n");
        close(ctx->fd);
        free(ctx);
        return NULL;
    }
    fprintf(stdout, "[RKISP] sensor entity: %s\n", sns_name);

    /* 3. rkaiq init: load IQ files (exposure, white balance, denoise, lens
     * shading). NULL/empty iq_dir falls back to /oem/etc/iqfiles. init does
     * NOT touch hardware. */
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

    /* 4. rkaiq prepare (critical): configures the media link sensor->csi2-dphy
     * ->cif->isp and the sensor subdev (resolution, MIPI lanes, MCLK, timing).
     * No manual subdev ioctls needed. After prepare hardware params are set
     * but no image output yet. */
    if (rk_aiq_uapi2_sysctl_prepare(aiq, (uint32_t)width, (uint32_t)height,
                                    RK_AIQ_WORKING_MODE_NORMAL) != XCAM_RETURN_NO_ERROR) {
        fprintf(stderr, "[RKISP] rkaiq prepare failed\n");
        rk_aiq_uapi2_sysctl_deinit(aiq);
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    /* 5. rkaiq start: launch the 3A background threads (AE/AWB/AF). They read
     * image statistics and adjust sensor exposure/gain and ISP params. MIPI
     * stream is NOT started yet. */
    if (rk_aiq_uapi2_sysctl_start(aiq) != XCAM_RETURN_NO_ERROR) {
        fprintf(stderr, "[RKISP] rkaiq start failed\n");
        rk_aiq_uapi2_sysctl_deinit(aiq);
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    /* 6. VIDIOC_S_FMT: RKISP only accepts MPLANE; use pix_mp, not pix.
     * S_FMT is bidirectional: pass desired WxH, kernel returns the actually
     * aligned WxH (e.g. width aligned to 16, height to 8). MUST save the
     * returned values, do not reuse the input width/height. */
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

    /* 7. MPLANE buffers: REQBUFS/QUERYBUF/mmap/EXPBUF/QBUF. After this buffers
     * are queued but hardware is not yet streaming. */
    if (!init_mplane_buffers(ctx)) {
        rk_aiq_uapi2_sysctl_stop(aiq, false);
        rk_aiq_uapi2_sysctl_deinit(aiq);
        v4l2_capture_close(&ctx);
        return NULL;
    }

    /* 8. STREAMON: starts MIPI-DPHY/CIF/ISP hardware, sensor outputs MIPI data.
     * Placed inside open() to align with the USB camera API; the caller must
     * DQBUF in a loop or the queue fills and frames drop. */
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

/* x86 dev host / no rkaiq: MIPI path unavailable, return NULL so the caller
 * falls back to USB */
v4l2_capture_t *rkisp_camera_open(const char *iq_dir, int width, int height, int fps)
{
    (void)iq_dir; (void)width; (void)height; (void)fps;
    return NULL;
}

#endif /* HAVE_RKAIQ */
