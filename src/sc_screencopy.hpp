// sc_screencopy.hpp —— zwlr_screencopy_manager_v1 的最简实现（只做 shm 路径）
//
// ⚠️ sfwm.cpp 顶部把协议头包在 extern "C" { } 里，本文件也被那个块包住，
//    所以这里**不能出现任何 C++ 特性**（模板/namespace/std::vector 都会报
//    "templates must have C++ linkage"）。全部用 C 写法。
//
// 为什么用 screencap：
//   本机 SF 的 MediaProjection 镜像会过滤掉非 app 进程（root/ksu 域）创建的图层，
//   所以安卓自带录屏抓不到我们的窗口；而 screencap 走整屏 DisplayCapture，
//   实测能抓到。xdg-desktop-portal-wlr / OBS 的「屏幕捕捉」只要 shm XRGB8888。
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

// screencap 裸输出：u32 width, u32 height, u32 format, 然后像素（内存 R,G,B,A）
static int sc_grab_into_shm(struct wl_resource *buffer) {
    struct wl_shm_buffer *shm = buffer ? wl_shm_buffer_get(buffer) : NULL;
    if (!shm) return 0;
    const int32_t bw = wl_shm_buffer_get_width(shm);
    const int32_t bh = wl_shm_buffer_get_height(shm);
    const int32_t bstride = wl_shm_buffer_get_stride(shm);
    if (bw <= 0 || bh <= 0 || bstride <= 0) return 0;

    char cmd[256];
    snprintf(cmd, sizeof cmd, "screencap > %s", SC_TMP);
    if (system(cmd) != 0) { remove(SC_TMP); return 0; }
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
            wl_shm_buffer_begin_access(shm);
            uint8_t *base = (uint8_t *)wl_shm_buffer_get_data(shm);
            const int32_t rows = bh < sh ? bh : sh;
            const int32_t cols_max = bw < sw ? bw : sw;
            for (int32_t y = 0; y < rows; y++) {
                if (fseek(fp, 12L + (long)((size_t)y * (size_t)src_stride), SEEK_SET) != 0) break;
                if (fread(row, 1, (size_t)src_stride, fp) != (size_t)src_stride) break;
                uint8_t *dst = base + (size_t)y * (size_t)bstride;
                for (int32_t x = 0; x < cols_max; x++) {
                    const uint8_t *p = row + (size_t)x * 4;
                    dst[x * 4 + 0] = p[2];   // B
                    dst[x * 4 + 1] = p[1];   // G
                    dst[x * 4 + 2] = p[0];   // R
                    dst[x * 4 + 3] = 0xFF;
                }
            }
            wl_shm_buffer_end_access(shm);
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
    printf("[screencopy] 已回传一帧\n");
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
    printf("[screencopy] 客户端绑定 v%u\n", v);
}

static void sc_screencopy_init(struct wl_display *dpy, int32_t w, int32_t h) {
    sc_w = w; sc_h = h;
    if (!wl_global_create(dpy, &zwlr_screencopy_manager_v1_interface, 3, NULL, sc_bind)) {
        fprintf(stderr, "[screencopy] wl_global_create 失败\n");
        return;
    }
    printf("[screencopy] global 已注册（%dx%d, shm XRGB8888）\n", w, h);
}

#endif  // SFWM_SC_SCREENCOPY_HPP
