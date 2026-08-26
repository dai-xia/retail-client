/**
 * @file rga_x86_stub.cpp - librga im2d API stub (x86 desktop dev only)
 *
 * wrapbuffer_* are inline/macros in the RGA header, no stub needed.
 * Only improcess (declared in im2d_single.h) is stubbed.
 */
#include <cstddef>
#include <cstring>
#include "im2d.hpp"
#include "RgaUtils.h"
#include "rga.h"

IM_STATUS improcess(rga_buffer_t src, rga_buffer_t dst, rga_buffer_t pat,
                    im_rect srect, im_rect drect, im_rect prect, int usage) {
    (void)src; (void)dst; (void)pat;
    (void)srect; (void)drect; (void)prect; (void)usage;
    return IM_STATUS_SUCCESS;
}

extern "C" {
rga_buffer_t wrapbuffer_virtualaddr_t(void* vir_addr, int width, int height,
                                       int wstride, int hstride, int format) {
    rga_buffer_t buf;
    memset(&buf, 0, sizeof(buf));
    (void)vir_addr; (void)width; (void)height; (void)wstride; (void)hstride; (void)format;
    return buf;
}

rga_buffer_t wrapbuffer_fd_t(int fd, int width, int height,
                              int wstride, int hstride, int format) {
    rga_buffer_t buf;
    memset(&buf, 0, sizeof(buf));
    (void)fd; (void)width; (void)height; (void)wstride; (void)hstride; (void)format;
    return buf;
}
}