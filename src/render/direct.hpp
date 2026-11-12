// SPDX-License-Identifier: AGPL-3.0-or-later
// direct.hpp
//
// Hands a client dmabuf straight to SurfaceFlinger instead of compositing it.
//
// The GL path imports the client's buffer as a texture, draws it into an FBO that matches
// the layer, and reads the result back into the buffer returned by ANativeWindow_lock. That
// read-back costs about 14 MB per frame at 2800x1272 and was the reason a 120 fps capable
// GPU only reached 20 fps.
//
// This path wraps the dmabuf in an AHardwareBuffer and sets it on a shadow SurfaceControl
// bound to the layer's window, so SurfaceFlinger composites the client's buffer directly
// (hardware overlay where possible). No pixel is copied.
//
// Only buffers that map to a plain linear single-plane allocation are handled; anything else
// returns false and the caller falls back to the GL path.
#pragma once

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace direct {

typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct ASurfaceControl ASurfaceControl;
typedef struct ASurfaceTransaction ASurfaceTransaction;

// android/hardware_buffer.h, repeated here to avoid a build dependency on the NDK headers
struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint64_t rfu0;
    uint64_t rfu1;
};

enum {
    AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1,
    AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM = 2,
    AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM = 4,
    AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM = 5,
    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE = 0x100,
    AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY = 0x800,
    AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_IMPORT = 1,
};

// Returns a status code (-errno on failure) and hands the buffer back through outBuffer.
typedef int32_t (*PFN_createFromHandle)(const AHardwareBuffer_Desc *, const void *, int32_t,
                                        AHardwareBuffer **);
typedef void (*PFN_hbRelease)(AHardwareBuffer *);
typedef ASurfaceControl *(*PFN_ascFromWindow)(ANativeWindow *);
typedef void (*PFN_ascRelease)(ASurfaceControl *);
typedef ASurfaceTransaction *(*PFN_txCreate)(void);
typedef void (*PFN_txSetBuffer)(ASurfaceTransaction *, ASurfaceControl *, const AHardwareBuffer *,
                                int32_t);
typedef void (*PFN_txApply)(ASurfaceTransaction *);
typedef void (*PFN_txDelete)(ASurfaceTransaction *);
typedef void *(*PFN_nhCreate)(int, int);
typedef void (*PFN_nhDelete)(void *);

struct Api {
    bool ok = false;
    PFN_createFromHandle createFromHandle = nullptr;
    PFN_hbRelease hbRelease = nullptr;
    PFN_ascFromWindow ascFromWindow = nullptr;
    PFN_ascRelease ascRelease = nullptr;
    PFN_txCreate txCreate = nullptr;
    PFN_txSetBuffer txSetBuffer = nullptr;
    PFN_txApply txApply = nullptr;
    PFN_txDelete txDelete = nullptr;
    PFN_nhCreate nhCreate = nullptr;
    PFN_nhDelete nhDelete = nullptr;
};

inline Api &api() {
    static Api a;
    static bool tried = false;
    if (tried) return a;
    tried = true;
    void *nw = dlopen("libnativewindow.so", RTLD_NOW);
    void *an = dlopen("libandroid.so", RTLD_NOW);
    if (!nw || !an) {
        fprintf(stderr, "[direct] dlopen failed: nativewindow=%p android=%p (%s)\n",
                nw, an, dlerror());
        return a;
    }
    a.createFromHandle = (PFN_createFromHandle)dlsym(nw, "AHardwareBuffer_createFromHandle");
    a.hbRelease = (PFN_hbRelease)dlsym(nw, "AHardwareBuffer_release");
    a.ascFromWindow = (PFN_ascFromWindow)dlsym(an, "ASurfaceControl_createFromWindow");
    a.ascRelease = (PFN_ascRelease)dlsym(an, "ASurfaceControl_release");
    a.txCreate = (PFN_txCreate)dlsym(an, "ASurfaceTransaction_create");
    a.txSetBuffer = (PFN_txSetBuffer)dlsym(an, "ASurfaceTransaction_setBuffer");
    a.txApply = (PFN_txApply)dlsym(an, "ASurfaceTransaction_apply");
    a.txDelete = (PFN_txDelete)dlsym(an, "ASurfaceTransaction_delete");
    // native_handle_create lives in libcutils, which is not always reachable from here; the
    // handle is a plain int array (version, numFds, numInts, data), so allocate it directly.
    a.ok = a.createFromHandle && a.hbRelease && a.ascFromWindow && a.txCreate &&
           a.txSetBuffer && a.txApply && a.txDelete;
    if (!a.ok)
        printf("[direct] SurfaceControl C API incomplete (createFromHandle=%p asc=%p tx=%p)\n",
               (void *)a.createFromHandle, (void *)a.ascFromWindow, (void *)a.txCreate);
    return a;
}

// native_handle_t: version, numFds, numInts, then the fds followed by the ints.
inline void *nh_alloc(int nfds, int nints) {
    int *p = (int *)malloc(sizeof(int) * 3 + sizeof(int) * (size_t)(nfds + nints));
    if (!p) return nullptr;
    p[0] = 12;          // native_handle version
    p[1] = nfds;
    p[2] = nints;
    return p;
}
inline void nh_free(void *h) { free(h); }

// DRM fourcc to HAL_PIXEL_FORMAT_*. Returns 0 for formats this path does not handle.
inline uint32_t hal_format(uint32_t fourcc) {
    switch (fourcc) {
    case 0x34325241:   // DRM_FORMAT_ARGB8888, bytes B,G,R,A
    case 0x34325258:   // DRM_FORMAT_XRGB8888
        return AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM;
    case 0x34324241:   // DRM_FORMAT_ABGR8888, bytes R,G,B,A
    case 0x34324258:   // DRM_FORMAT_XBGR8888
        return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    case 0x36314752:   // DRM_FORMAT_RGB565
        return AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
    default:
        return 0;
    }
}

struct Target {
    ASurfaceControl *asc = nullptr;
    AHardwareBuffer *hb = nullptr;
    int hb_fd = -1;
    ino_t hb_ino = 0;
    uint32_t hb_w = 0, hb_h = 0;
};

// Shows one dmabuf buffer of `db` on `win`. Returns false when the buffer cannot be wrapped;
// the caller then uses the GL path for this frame.
inline bool present(ANativeWindow *win, const DmabufBuf *db) {
    Api &a = api();
    if (!a.ok || !win || !db) return false;
    if (db->nplanes != 1 || db->fds[0] < 0) return false;
    if (db->modifier != 0 || db->offset[0] != 0) return false;
    const uint32_t fmt = hal_format(db->drm_format);
    if (!fmt) return false;
    const int bpp = (fmt == AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM) ? 2 : 4;
    if (db->stride[0] % (uint32_t)bpp) return false;

    struct stat st;
    if (fstat(db->fds[0], &st) != 0) return false;

    static int dbg = 0;
    const bool log = dbg < 6;

    static Target t[16];
    static ANativeWindow *slot_win[16] = {nullptr};
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (slot_win[i] == win) { slot = i; break; }
        if (!slot_win[i]) { slot_win[i] = win; slot = i; break; }
    }
    if (slot < 0) return false;
    Target &tg = t[slot];

    const bool same = tg.hb && tg.hb_fd == db->fds[0] && tg.hb_ino == st.st_ino &&
                      tg.hb_w == (uint32_t)db->w && tg.hb_h == (uint32_t)db->h;
    if (log) {
        dbg++;
        printf("[direct] present %p db=%dx%d fd=%d ino=%llu same=%d hb=%p\n", (void *)win, db->w,
               db->h, db->fds[0], (unsigned long long)st.st_ino, (int)same, (void *)tg.hb);
    }
    if (!same) {
        AHardwareBuffer_Desc d = {};
        d.width = (uint32_t)db->w;
        d.height = (uint32_t)db->h;
        d.layers = 1;
        d.format = fmt;
        d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
        d.stride = db->stride[0] / (uint32_t)bpp;
        void *nh = nh_alloc(1, 0);
        if (!nh) return false;
        ((int *)nh)[3] = db->fds[0];   // native_handle_t::data[0]
        AHardwareBuffer *nb = nullptr;
        const int32_t res =
            a.createFromHandle(&d, nh, AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_IMPORT, &nb);
        nh_free(nh);
        if (res != 0 || !nb) {
            static int warned = 0;
            if (warned++ < 3)
                printf("[direct] createFromHandle failed (%d) %dx%d fmt=%u stride=%u\n",
                       res, db->w, db->h, fmt, d.stride);
            return false;
        }
        if (tg.hb) a.hbRelease(tg.hb);
        tg.hb = nb;
        tg.hb_fd = db->fds[0];
        tg.hb_ino = st.st_ino;
        tg.hb_w = (uint32_t)db->w;
        tg.hb_h = (uint32_t)db->h;
        if (log) printf("[direct] wrapped fd=%d -> hb=%p\n", db->fds[0], (void *)nb);
    }

    if (!tg.asc) {
        tg.asc = a.ascFromWindow(win);
        if (!tg.asc) return false;
        static int once = 0;
        if (!once++) printf("[direct] shadow SurfaceControl ready, direct scanout active\n");
    }

    ASurfaceTransaction *tx = a.txCreate();
    if (!tx) return false;
    if (log) printf("[direct] tx=%p asc=%p hb=%p, setting buffer\n", (void *)tx, (void *)tg.asc,
                    (void *)tg.hb);
    a.txSetBuffer(tx, tg.asc, tg.hb, -1);
    a.txApply(tx);
    a.txDelete(tx);
    if (log) printf("[direct] applied\n");
    return true;
}

// Wrap the client's dmabuf in a handle cloned from a real SurfaceFlinger buffer handle and
// hand it to SurfaceFlinger. A bare fd-only handle is rejected with EINVAL because the
// vendor mapper only accepts its own layout; this copies that layout field for field and
// only rewrites the geometry plus the fd itself.
//
//   tmpl_ints/tmpl_nfds/tmpl_nints come from an ANativeWindowBuffer dequeued on the layer.
//   Font offsets into the int array are the ones measured on Android 16.
inline bool present_shaped(ANativeWindow *win, const DmabufBuf *db, const uint32_t *tmpl_ints,
                           int tmpl_nfds, int tmpl_nints) {
    Api &a = api();
    if (!a.ok || !win || !db || !tmpl_ints) return false;
    if (db->nplanes != 1 || db->fds[0] < 0 || db->modifier != 0) return false;
    if (tmpl_nfds < 1 || tmpl_nints < 14) return false;
    const uint32_t fmt = hal_format(db->drm_format);
    if (!fmt) return false;

    static int dbg = 0;
    const bool log = dbg < 4;

    static Target t[16];
    static ANativeWindow *slot_win[16] = {nullptr};
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (slot_win[i] == win) { slot = i; break; }
        if (!slot_win[i]) { slot_win[i] = win; slot = i; break; }
    }
    if (slot < 0) return false;
    Target &tg = t[slot];

    if (tg.hb_fd != db->fds[0] || tg.hb_w != (uint32_t)db->w || tg.hb_h != (uint32_t)db->h) {
        void *nh = nh_alloc(tmpl_nfds, tmpl_nints);
        if (!nh) return false;
        int *p = (int *)nh;
        // native_handle_t: version, numFds, numInts, then fds and ints.
        p[3] = db->fds[0];
        for (int i = 1; i < tmpl_nfds; i++) p[3 + i] = dup(db->fds[0]);
        int *ints = p + 3 + tmpl_nfds;
        memcpy(ints, tmpl_ints, (size_t)tmpl_nints * sizeof(int));
        ints[2] = (int)db->stride[0];                        // stride in bytes
        ints[3] = (int)db->h;
        ints[4] = (int)db->w;
        ints[6] = (int)fmt;
        ints[13] = (int)((uint32_t)db->stride[0] * (uint32_t)db->h);   // allocation size

        AHardwareBuffer_Desc d = {};
        d.width = (uint32_t)db->w;
        d.height = (uint32_t)db->h;
        d.layers = 1;
        d.format = fmt;
        d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
        d.stride = db->stride[0] / 4u;
        AHardwareBuffer *nb = nullptr;
        const int32_t res =
            a.createFromHandle(&d, nh, AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_IMPORT, &nb);
        const int err = errno;
        if (log) {
            dbg++;
            printf("[direct] shaped %dx%d fmt=%u stride=%u fd=%d nfds=%d nints=%d -> res=%d hb=%p errno=%d\n",
                   db->w, db->h, fmt, db->stride[0], db->fds[0], tmpl_nfds, tmpl_nints, res,
                   (void *)nb, err);
        }
        // The extra fds were duplicated only for the handle; data[0] is the caller's own.
        for (int i = 1; i < tmpl_nfds; i++)
            if (p[3 + i] >= 0) close(p[3 + i]);
        nh_free(nh);
        if (res != 0 || !nb) return false;
        if (tg.hb) a.hbRelease(tg.hb);
        tg.hb = nb;
        tg.hb_fd = db->fds[0];
        tg.hb_w = (uint32_t)db->w;
        tg.hb_h = (uint32_t)db->h;
    }

    if (!tg.asc) {
        tg.asc = a.ascFromWindow(win);
        if (!tg.asc) return false;
        static int once = 0;
        if (!once++) printf("[direct] shadow SurfaceControl ready\n");
    }
    ASurfaceTransaction *tx = a.txCreate();
    if (!tx) return false;
    a.txSetBuffer(tx, tg.asc, tg.hb, -1);
    a.txApply(tx);
    a.txDelete(tx);
    if (log) printf("[direct] shaped buffer applied\n");
    return true;
}

}   // namespace direct
