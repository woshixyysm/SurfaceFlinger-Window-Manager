// SPDX-License-Identifier: AGPL-3.0-or-later
// screencopy.hpp - minimal zwlr_screencopy_manager_v1, shm path only.
//
// src/main.cpp wraps the protocol headers in extern "C", and this file is inside
// that block, so no C++ features may appear here; templates, namespaces and
// std::vector all fail to compile.
//
// Why screencap:
// the MediaProjection mirror drops layers created by non-application processes,
// so the stock recorder captures nothing, while screencap uses DisplayCapture
// and works. Clients only need shm XRGB8888.
#pragma once
#ifndef SFWM_SC_SCREENCOPY_HPP
#define SFWM_SC_SCREENCOPY_HPP

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "wlr-screencopy-unstable-v1-server-protocol.h"

static int32_t sc_w = 0, sc_h = 0;
static const char *SC_TMP = "/data/local/tmp/.sfwm-sc.raw";

typedef struct { int32_t w, h; } ScSize;

// wl_shm is implemented here, in core/config.hpp, because the libwayland pool has
// no offset support. wl_shm_buffer_get() matches on the implementation pointer,
//   `wl_resource_instance_of(res, &wl_buffer_interface, &shm_buffer_interface)`
// so buffers registered here never match and cannot be used to read pixels.
// so it always returns NULL for buffers created here.
// Any capture code relying on wl_shm_buffer_get() then fails silently.
//   layout, so the parameters are read with the ShmBuf layout.
//   The field order must match ShmPool and ShmBuf in core/config.hpp exactly.
struct SfwmShmPoolView { int fd; int32_t size; uint8_t *base; };
struct SfwmShmBufView {
    uint32_t magic;
    struct SfwmShmPoolView *pool;
    int32_t offset;
    int32_t w, h, stride;
    uint32_t format;
};
#define SFWM_SHM_MAGIC 0x53464246u   /* 'SBFB' */

// screencap output: u32 width, height, format, then pixels in memory order R,G,B,A.
static int sc_grab_into_shm(struct wl_resource *buffer) {
    if (!buffer) return 0;
    void *ud = wl_resource_get_user_data(buffer);
    if (!ud) return 0;
    struct SfwmShmBufView *sb = (struct SfwmShmBufView *)ud;
    if (sb->magic != SFWM_SHM_MAGIC || !sb->pool || !sb->pool->base) {
        printf("[screencopy] not an sfwm shm buffer, giving up\n");
        return 0;
    }
    const int32_t bw = sb->w, bh = sb->h, bstride = sb->stride;
    if (bw <= 0 || bh <= 0 || bstride <= 0) return 0;
    uint8_t *dst_base = sb->pool->base + sb->offset;

    char cmd[256];
    snprintf(cmd, sizeof cmd, "screencap > %s", SC_TMP);
    if (system(cmd) != 0) { remove(SC_TMP); printf("[screencopy] screencap failed\n"); return 0; }
    FILE *fp = fopen(SC_TMP, "rb");
    if (!fp) return 0;

    uint32_t hdr[3] = {0, 0, 0};
    int ok = 0;
    uint8_t *row = NULL;
    if (fread(hdr, sizeof hdr, 1, fp) == 1 && hdr[0] > 0 && hdr[1] > 0) {
        const int32_t sw = (int32_t)hdr[0], sh = (int32_t)hdr[1];
        const int32_t src_stride = sw * 4;
        row = (uint8_t *)malloc((size_t)src_stride);
        if (row) {
            const int32_t rows = bh < sh ? bh : sh;
            const int32_t cols_max = bw < sw ? bw : sw;
            for (int32_t y = 0; y < rows; y++) {
                if (fseek(fp, 12L + (long)((size_t)y * (size_t)src_stride), SEEK_SET) != 0) break;
                if (fread(row, 1, (size_t)src_stride, fp) != (size_t)src_stride) break;
                uint8_t *dst = dst_base + (size_t)y * (size_t)bstride;
                for (int32_t x = 0; x < cols_max; x++) {
                    const uint8_t *p = row + (size_t)x * 4;
                    dst[x * 4 + 0] = p[2];   // B
                    dst[x * 4 + 1] = p[1];   // G
                    dst[x * 4 + 2] = p[0];   // R
                    dst[x * 4 + 3] = 0xFF;
                }
            }
            ok = 1;
        }
    }
    if (row) free(row);
    fclose(fp);
    remove(SC_TMP);
    return ok;
}

static void sc_frame_copy(struct wl_client *c, struct wl_resource *res, struct wl_resource *buffer) {
    (void)c;
    if (!sc_grab_into_shm(buffer)) {
        zwlr_screencopy_frame_v1_send_failed(res);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    zwlr_screencopy_frame_v1_send_ready(res, (uint32_t)(ns >> 32),
                                        (uint32_t)(ns & 0xffffffffu), (uint32_t)ts.tv_nsec);
    if (buffer) wl_buffer_send_release(buffer);
    printf("[screencopy] frame returned\n");
}
static void sc_frame_destroy(struct wl_client *c, struct wl_resource *res) {
    (void)c; wl_resource_destroy(res);
}

static const struct zwlr_screencopy_frame_v1_interface sc_frame_impl = {
    .copy             = sc_frame_copy,
    .copy_with_damage = sc_frame_copy,
    .destroy          = sc_frame_destroy,
};

static void sc_frame_resource_destroyed(struct wl_resource *res) {
    ScSize *sz = (ScSize *)wl_resource_get_user_data(res);
    if (sz) free(sz);
}

static void sc_capture_common(struct wl_client *c, struct wl_resource *man, uint32_t id,
                              int region, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)region; (void)x; (void)y;
    const uint32_t ver = wl_resource_get_version(man);
    struct wl_resource *fr = wl_resource_create(c, &zwlr_screencopy_frame_v1_interface,
                                               (int)ver, id);
    if (!fr) { wl_client_post_no_memory(c); return; }
    ScSize *sz = (ScSize *)calloc(1, sizeof(ScSize));
    sz->w = w > 0 ? w : sc_w;
    sz->h = h > 0 ? h : sc_h;
    wl_resource_set_implementation(fr, &sc_frame_impl, sz, sc_frame_resource_destroyed);

    zwlr_screencopy_frame_v1_send_buffer(fr, WL_SHM_FORMAT_XRGB8888,
                                         (uint32_t)sz->w, (uint32_t)sz->h,
                                         (uint32_t)sz->w * 4u);
    if (ver >= 2)
        zwlr_screencopy_frame_v1_send_damage(fr, 0, 0, (uint32_t)sz->w, (uint32_t)sz->h);
    if (ver >= 3)
        zwlr_screencopy_frame_v1_send_buffer_done(fr);
}

static void sc_man_capture_output(struct wl_client *c, struct wl_resource *man, uint32_t id,
                                  int32_t overlay, struct wl_resource *out) {
    (void)overlay; (void)out;
    sc_capture_common(c, man, id, 0, 0, 0, sc_w, sc_h);
}
static void sc_man_capture_region(struct wl_client *c, struct wl_resource *man, uint32_t id,
                                  int32_t overlay, struct wl_resource *out,
                                  int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)overlay; (void)out;
    sc_capture_common(c, man, id, 1, x, y, w, h);
}
static void sc_man_destroy(struct wl_client *c, struct wl_resource *res) {
    (void)c; wl_resource_destroy(res);
}

static const struct zwlr_screencopy_manager_v1_interface sc_man_impl = {
    .capture_output        = sc_man_capture_output,
    .capture_output_region = sc_man_capture_region,
    .destroy               = sc_man_destroy,
};

static void sc_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    (void)data;
    const uint32_t v = version > 3 ? 3 : version;
    struct wl_resource *res = wl_resource_create(c, &zwlr_screencopy_manager_v1_interface,
                                                (int)v, id);
    if (!res) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(res, &sc_man_impl, NULL, NULL);
    printf("[screencopy] client bound v%u\n", v);
}

static void sc_screencopy_init(struct wl_display *dpy, int32_t w, int32_t h) {
    sc_w = w; sc_h = h;
    if (!wl_global_create(dpy, &zwlr_screencopy_manager_v1_interface, 3, NULL, sc_bind)) {
        fprintf(stderr, "[screencopy] wl_global_create failed\n");
        return;
    }
    printf("[screencopy] global registered (%dx%d, shm XRGB8888)\n", w, h);
}

#endif  // SFWM_SC_SCREENCOPY_HPP
