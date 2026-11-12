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
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
    uint32_t rfu0;
    uint64_t rfu1;
};

enum {
    AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1,
    AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM = 2,
    AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM = 4,
    AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM = 5,
    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE = 0x100,
    AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY = 0x800,
    // IMPORT goes through the mapper, which SurfaceFlinger keeps to itself: from an
    // unprivileged process every IMPORT fails with EINVAL. CLONE only duplicates the
    // handle, so the buffer is imported later, by SurfaceFlinger, in its own process.
    AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_IMPORT = 1,
    AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE = 2,
};

// Returns a status code (-errno on failure) and hands the buffer back through outBuffer.
typedef int32_t (*PFN_createFromHandle)(const AHardwareBuffer_Desc *, const void *, int32_t,
                                        AHardwareBuffer **);
typedef int32_t (*PFN_allocHb)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
typedef const int *(*PFN_getNativeHandle)(const AHardwareBuffer *);
typedef int32_t (*PFN_recvHandle)(int32_t, AHardwareBuffer **);
typedef void (*PFN_hbRelease)(AHardwareBuffer *);
typedef ASurfaceControl *(*PFN_ascFromWindow)(ANativeWindow *);
typedef void (*PFN_ascRelease)(ASurfaceControl *);
typedef ASurfaceTransaction *(*PFN_txCreate)(void);
typedef void (*PFN_txSetBuffer)(ASurfaceTransaction *, ASurfaceControl *, const AHardwareBuffer *,
                                int32_t);
// android/rect.h's ARect, declared here so this header needs no extra include.
struct TxRect {
    int32_t left, top, right, bottom;
};
typedef void (*PFN_txSetGeometry)(ASurfaceTransaction *, ASurfaceControl *, const TxRect &,
                                  const TxRect &, int32_t);
typedef void (*PFN_txSetFrameRate)(ASurfaceTransaction *, ASurfaceControl *, float, int8_t);
typedef void (*PFN_txSetTransparency)(ASurfaceTransaction *, ASurfaceControl *, int8_t);
typedef void (*PFN_txSetAlpha)(ASurfaceTransaction *, ASurfaceControl *, float);
typedef void (*PFN_txApply)(ASurfaceTransaction *);
typedef void (*PFN_txDelete)(ASurfaceTransaction *);
typedef void *(*PFN_nhCreate)(int, int);
typedef void (*PFN_nhDelete)(void *);

struct Api {
    bool ok = false;
    PFN_createFromHandle createFromHandle = nullptr;
    PFN_allocHb allocHb = nullptr;
    PFN_getNativeHandle getNativeHandle = nullptr;
    PFN_recvHandle recvHandle = nullptr;
    PFN_hbRelease hbRelease = nullptr;
    PFN_ascFromWindow ascFromWindow = nullptr;
    PFN_ascRelease ascRelease = nullptr;
    PFN_txCreate txCreate = nullptr;
    PFN_txSetBuffer txSetBuffer = nullptr;
    PFN_txSetGeometry txSetGeometry = nullptr;
    PFN_txSetFrameRate txSetFrameRate = nullptr;
    PFN_txSetTransparency txSetTransparency = nullptr;
    PFN_txSetAlpha txSetAlpha = nullptr;
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
    a.allocHb = (PFN_allocHb)dlsym(nw, "AHardwareBuffer_allocate");
    a.getNativeHandle = (PFN_getNativeHandle)dlsym(nw, "AHardwareBuffer_getNativeHandle");
    a.recvHandle = (PFN_recvHandle)dlsym(nw, "AHardwareBuffer_recvHandleFromUnixSocket");
    a.hbRelease = (PFN_hbRelease)dlsym(nw, "AHardwareBuffer_release");
    a.ascFromWindow = (PFN_ascFromWindow)dlsym(an, "ASurfaceControl_createFromWindow");
    a.ascRelease = (PFN_ascRelease)dlsym(an, "ASurfaceControl_release");
    a.txCreate = (PFN_txCreate)dlsym(an, "ASurfaceTransaction_create");
    a.txSetBuffer = (PFN_txSetBuffer)dlsym(an, "ASurfaceTransaction_setBuffer");
    a.txSetGeometry = (PFN_txSetGeometry)dlsym(an, "ASurfaceTransaction_setGeometry");
    a.txSetFrameRate = (PFN_txSetFrameRate)dlsym(an, "ASurfaceTransaction_setFrameRate");
    a.txSetTransparency = (PFN_txSetTransparency)dlsym(an, "ASurfaceTransaction_setBufferTransparency");
    a.txSetAlpha = (PFN_txSetAlpha)dlsym(an, "ASurfaceTransaction_setAlpha");
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
            a.createFromHandle(&d, nh, AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE, &nb);
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
        ints[2] = (int)(db->stride[0] / 4u);                 // stride in pixels
        ints[3] = (int)db->h;
        ints[4] = (int)db->w;
        ints[5] = (int)db->h;
        ints[6] = (int)fmt;
        ints[13] = (int)((uint32_t)db->stride[0] * (uint32_t)db->h);   // allocation size
        if (tmpl_nints > 24) ints[24] = (int)db->stride[0];            // stride in bytes

        AHardwareBuffer_Desc d = {};
        d.width = (uint32_t)db->w;
        d.height = (uint32_t)db->h;
        d.layers = 1;
        d.format = fmt;
        d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
        d.stride = db->stride[0] / 4u;
        AHardwareBuffer *nb = nullptr;
        const int32_t res =
            a.createFromHandle(&d, nh, AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE, &nb);
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

// Allocate a throwaway buffer of exactly the client's geometry so gralloc generates a legal
// handle for it, then swap in the client's dmabuf. A handle cloned from some other buffer is
// rejected: the vendor mapper validates the whole layout against the allocation it made.
inline bool present_graft(ANativeWindow *win, const DmabufBuf *db) {
    Api &a = api();
    if (!a.ok || !win || !db || !a.allocHb || !a.getNativeHandle) return false;
    if (db->nplanes != 1 || db->fds[0] < 0 || db->modifier != 0) return false;
    const uint32_t fmt = hal_format(db->drm_format);
    if (!fmt) return false;

    struct Graft {
        AHardwareBuffer *src = nullptr;   // the throwaway allocation that owns the handle shape
        int *ints = nullptr;
        int nfds = 0, nints = 0;
        uint32_t w = 0, h = 0, fmt = 0;
    };
    static Graft g[4];
    static ANativeWindow *gw[4] = {nullptr};
    int slot = -1;
    for (int i = 0; i < 4; i++) {
        if (gw[i] == win) { slot = i; break; }
        if (!gw[i]) { gw[i] = win; slot = i; break; }
    }
    if (slot < 0) return false;
    Graft &gr = g[slot];

    if (!gr.src || gr.w != (uint32_t)db->w || gr.h != (uint32_t)db->h || gr.fmt != fmt) {
        if (gr.ints) { free(gr.ints); gr.ints = nullptr; }
        if (gr.src) { a.hbRelease(gr.src); gr.src = nullptr; }
        AHardwareBuffer_Desc d = {};
        d.width = (uint32_t)db->w;
        d.height = (uint32_t)db->h;
        d.layers = 1;
        d.format = fmt;
        d.usage = 0x800ULL;   // COMPOSER_OVERLAY: enough for a handle, no GPU mapping needed
        AHardwareBuffer *src = nullptr;
        if (a.allocHb(&d, &src) != 0 || !src) {
            static int w0 = 0;
            if (w0++ < 2) printf("[direct] graft allocation failed %dx%d\n", db->w, db->h);
            return false;
        }
        const int *h = a.getNativeHandle(src);
        if (!h) { a.hbRelease(src); return false; }
        gr.src = src;
        gr.nfds = h[1];
        gr.nints = h[2];
        gr.ints = (int *)malloc(sizeof(int) * (size_t)gr.nints);
        memcpy(gr.ints, h + 3 + h[1], sizeof(int) * (size_t)gr.nints);
        gr.w = (uint32_t)db->w;
        gr.h = (uint32_t)db->h;
        gr.fmt = fmt;
        printf("[direct] graft template %dx%d fmt=%u nfds=%d nints=%d\n", db->w, db->h, fmt,
               gr.nfds, gr.nints);
    }

    // Handle sized for the template's layout; only the fds and the stride/size change.
    const size_t nhsz = sizeof(int) * 3 + sizeof(int) * (size_t)(gr.nfds + gr.nints);
    int *nh = (int *)malloc(nhsz);
    if (!nh) return false;
    nh[0] = 12;
    nh[1] = gr.nfds;
    nh[2] = gr.nints;
    nh[3] = db->fds[0];
    for (int i = 1; i < gr.nfds; i++) nh[3 + i] = dup(db->fds[0]);
    int *ints = nh + 3 + gr.nfds;
    memcpy(ints, gr.ints, sizeof(int) * (size_t)gr.nints);
    const uint32_t stride_px = db->stride[0] / 4u;
    if (gr.nints > 2)  ints[2] = (int)stride_px;
    if (gr.nints > 5)  { ints[3] = db->h; ints[4] = db->w; ints[5] = db->h; }
    if (gr.nints > 6)  ints[6] = (int)fmt;
    if (gr.nints > 13) ints[13] = (int)(db->stride[0] * (uint32_t)db->h);
    if (gr.nints > 24) ints[24] = (int)db->stride[0];

    AHardwareBuffer_Desc d = {};
    d.width = (uint32_t)db->w;
    d.height = (uint32_t)db->h;
    d.layers = 1;
    d.format = fmt;
    d.usage = 0x100ULL | 0x800ULL;
    d.stride = stride_px;
    AHardwareBuffer *dst = nullptr;
    const int32_t res = a.createFromHandle(&d, nh, AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE, &dst);
    for (int i = 0; i < gr.nfds; i++)
        if (i < gr.nfds && nh[3 + i] >= 0 && i > 0) close(nh[3 + i]);
    free(nh);
    {
        static int n = 0;
        if (n++ < 4)
            printf("[direct] graft %dx%d stride_px=%u -> res=%d hb=%p\n", db->w, db->h, stride_px,
                   res, (void *)dst);
    }
    if (res != 0 || !dst) return false;

    static ASurfaceControl *asc[4] = {nullptr};
    if (!asc[slot]) asc[slot] = a.ascFromWindow(win);
    if (!asc[slot]) { a.hbRelease(dst); return false; }
    ASurfaceTransaction *tx = a.txCreate();
    if (!tx) { a.hbRelease(dst); return false; }
    a.txSetBuffer(tx, asc[slot], dst, -1);
    a.txApply(tx);
    a.txDelete(tx);
    a.hbRelease(dst);
    return true;
}

// Puts `hb` on the layer and scales it to fill it. Split out so the cached path can reuse an
// already imported buffer without touching the socket.
inline bool present_attach(Api &a, ANativeWindow *win, const DmabufBuf *db, AHardwareBuffer *hb,
                           int dst_w, int dst_h) {
    static ASurfaceControl *asc = nullptr;
    if (!asc) asc = a.ascFromWindow(win);
    if (!asc) {
        static int once = 0;
        if (!once++) printf("[direct] attach: ASurfaceControl_createFromWindow failed\n");
        return false;
    }
    ASurfaceTransaction *tx = a.txCreate();
    if (!tx) {
        static int once = 0;
        if (!once++) printf("[direct] attach: transaction allocation failed\n");
        return false;
    }
    a.txSetBuffer(tx, asc, hb, -1);
    // SurfaceFlinger picks the display mode from the votes of the visible layers. The layer
    // the legacy SurfaceComposerClient path votes on is not the one this transaction touches,
    // and without a vote here the display falls back to 60 Hz whatever SFWM_FPS asked for.
    if (a.txSetFrameRate) {
        static float rate = -1.f;
        if (rate < 0.f) {
            const char *e = getenv("SFWM_FPS");
            const int fps = e ? atoi(e) : 0;
            rate = fps > 0 ? (float)fps : 120.f;
        }
        a.txSetFrameRate(tx, asc, rate, 1 /* EXACT */);
    }
    // The layer now carries the client's own buffer instead of a composited one, so the
    // geometry has to scale it up; without it SurfaceFlinger shows it 1:1 in the corner.
    if (a.txSetGeometry) {
        // dst_w/dst_h are the size of the buffer we would otherwise composite into, i.e.
        // the layer's own size; the window is not reliable here because SurfaceFlinger is
        // free to hand out a differently sized buffer.
        const TxRect src{0, 0, db->w, db->h};
        const TxRect dst{0, 0, dst_w > 0 ? dst_w : db->w, dst_h > 0 ? dst_h : db->h};
        a.txSetGeometry(tx, asc, src, dst, 0);
    }
    a.txApply(tx);
    a.txDelete(tx);
    return true;
}

// Lazily connects to the helper inside SurfaceFlinger. Abstract namespace by default:
// /data/adb is 0700 root, so a file socket is not reachable for surfaceflinger's uid.
// SFWM_SF_HOOK_SOCK still allows pointing it at a path socket.
inline int sf_socket()
{
    static int sock = -2;
    if (sock != -2) return sock;
    const char *path = getenv("SFWM_SF_HOOK_SOCK");
    const char *name = "sfwm-sf";
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr;
    socklen_t len;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path) {
        strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path) + 1);
    } else {
        memcpy(addr.sun_path + 1, name, strlen(name));
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name));
    }
    if (s < 0) {
        sock = -1;
    } else if (connect(s, (struct sockaddr *)&addr, len) != 0) {
        close(s);
        sock = -1;
        printf("[direct] sf-hook socket not available, using the blit path\n");
    } else {
        sock = s;
        printf("[direct] sf-hook connected\n");
    }
    return sock;
}

// Pool mode: SurfaceFlinger allocates the buffers, the client renders into them and hands
// them straight back. Nothing is imported per frame, so no per-frame IPC and no reliance on
// the mapper accepting a foreign dma-buf.
struct SfPool {
    bool ready = false;
    int n = 0;
    int w = 0, h = 0;
    uint32_t fourcc = 0;
    AHardwareBuffer *hb[4] = {};
    int fd[4] = {-1, -1, -1, -1};
    int stride_bytes[4] = {};
};

inline SfPool &sf_pool()
{
    static SfPool p;
    return p;
}

inline bool sf_pool_ensure(int want_w, int want_h, uint32_t fourcc)
{
    SfPool &p = sf_pool();
    if (p.ready && p.w == want_w && p.h == want_h && p.fourcc == fourcc) return true;
    Api &a = api();
    const int sock = sf_socket();
    if (sock < 0 || !a.recvHandle || !a.getNativeHandle) return false;

    struct {
        uint32_t magic, width, height, format, count;
    } req = {0x53465750u, (uint32_t)want_w, (uint32_t)want_h, fourcc, 4};
    if (send(sock, &req, sizeof req, MSG_NOSIGNAL) != (ssize_t)sizeof req) return false;
    int32_t status = -1;
    if (recv(sock, &status, sizeof status, MSG_WAITALL) != (ssize_t)sizeof status) return false;
    if (status != 0) {
        printf("[direct] sf pool alloc rejected (%d)\n", status);
        return false;
    }
    p.n = 0;
    for (int i = 0; i < 4; i++) {
        AHardwareBuffer *hb = nullptr;
        if (a.recvHandle(sock, &hb) != 0 || !hb) break;
        const int *nh = a.getNativeHandle(hb);
        if (!nh) break;
        p.hb[p.n] = hb;
        p.fd[p.n] = nh[1] >= 1 ? nh[3] : -1;
        p.stride_bytes[p.n] = nh[2] > 24 ? nh[3 + nh[1] + 24] : 0;
        p.n++;
    }
    if (p.n == 0) return false;
    p.w = want_w;
    p.h = want_h;
    p.fourcc = fourcc;
    p.ready = true;
    printf("[direct] sf pool ready: %d buffers %dx%d\n", p.n, p.w, p.h);
    return true;
}

// Hands one pooled buffer to the layer. The caller has already rendered into it.
inline bool sf_pool_present(ANativeWindow *win, int idx, int fence_fd, int dst_w, int dst_h)
{
    SfPool &p = sf_pool();
    if (!p.ready || idx < 0 || idx >= p.n) return false;
    Api &a = api();
    static ASurfaceControl *asc = nullptr;
    if (!asc) asc = a.ascFromWindow(win);
    if (!asc) return false;
    ASurfaceTransaction *tx = a.txCreate();
    if (!tx) return false;
    a.txSetBuffer(tx, asc, p.hb[idx], fence_fd);
    // The layer is the only thing on screen, so say so: without an explicit opaque flag
    // SurfaceFlinger kept blending it against whatever was underneath.
    if (a.txSetTransparency) a.txSetTransparency(tx, asc, 2 /* OPAQUE */);
    if (a.txSetAlpha) a.txSetAlpha(tx, asc, 1.0f);
    if (a.txSetFrameRate) {
        static float rate = -1.f;
        if (rate < 0.f) {
            const char *e = getenv("SFWM_FPS");
            const int fps = e ? atoi(e) : 0;
            rate = fps > 0 ? (float)fps : 120.f;
        }
        a.txSetFrameRate(tx, asc, rate, 1 /* EXACT */);
    }
    if (a.txSetGeometry) {
        const TxRect src{0, 0, p.w, p.h};
        const TxRect dst{0, 0, dst_w > 0 ? dst_w : p.w, dst_h > 0 ? dst_h : p.h};
        a.txSetGeometry(tx, asc, src, dst, 0);
    }
    a.txApply(tx);
    a.txDelete(tx);
    return true;
}

// Optional: hand the client dma-buf to SurfaceFlinger through the helper that the KSU module
// preloads into surfaceflinger (android/ksu). IMPORT needs the gralloc mapper and the mapper
// is a passthrough module that only exists inside SurfaceFlinger, so the import has to happen
// there; the resulting AHardwareBuffer comes back over the same socket with the CLONE method,
// which needs no mapper, and is set on our layer here. Returns false whenever the module is
// not installed or the import failed, and the caller falls back to the blit path.
inline bool present_via_sfhook(ANativeWindow *win, const DmabufBuf *db, int dst_w, int dst_h) {
    Api &a = api();
    if (!a.ok || !a.recvHandle || !win || !db) return false;
    if (db->nplanes != 1 || db->fds[0] < 0 || db->modifier != 0) {
        static int once = 0;
        if (!once++)
            printf("[direct] sfhook refused: planes=%d fd=%d modifier=0x%llx\n", db->nplanes,
                   db->fds[0], (unsigned long long)db->modifier);
        return false;
    }
    if (!hal_format(db->drm_format)) return false;

    const int sock = sf_socket();
    static int failures = 0;
    if (sock < 0) return false;
    // Retry after a cooldown instead of giving up for the whole run: a burst of failures
    // during client startup used to disable the fast path permanently.
    if (failures > 3) {
        static int cooldown = 0;
        if (++cooldown < 120) return false;
        cooldown = 0;
        failures = 0;
    }

    // No cache here on purpose: the client recycles dmabuf fds when it recreates its
    // swapchain, and an AHardwareBuffer imported for the previous owner keeps pointing at a
    // buffer nobody draws into any more (the screen simply goes black). Importing per frame
    // costs one short socket round trip, which is far cheaper than the composite it replaces.

    struct Req {
        uint32_t magic, width, height, fourcc, stride;
    } req = {0x5346574du, (uint32_t)db->w, (uint32_t)db->h, db->drm_format, db->stride[0]};

    struct iovec iov = {&req, sizeof req};
    char ctl[CMSG_SPACE(sizeof(int))];
    memset(ctl, 0, sizeof ctl);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = sizeof ctl;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &db->fds[0], sizeof(int));
    msg.msg_controllen = c->cmsg_len;

    if (sendmsg(sock, &msg, MSG_NOSIGNAL) < 0) {
        static int once = 0;
        if (!once++) printf("[direct] sf-hook sendmsg failed: %d\n", errno);
        failures++;
        return false;
    }
    int32_t status = -1;
    ssize_t n = recv(sock, &status, sizeof status, MSG_WAITALL);
    if (n != (ssize_t)sizeof status || status != 0) {
        int32_t diag[2] = {0, 0};
        if (n == (ssize_t)sizeof status && recv(sock, diag, sizeof diag, MSG_WAITALL) > 0 &&
            failures++ < 3) {
            printf("[direct] sf-hook stage=%d code=%d (blit path)\n", diag[0], diag[1]);
            int32_t ni = 0;
            if (recv(sock, &ni, sizeof ni, MSG_WAITALL) == (ssize_t)sizeof ni && ni > 0 && ni <= 40) {
                int32_t buf[40];
                if (recv(sock, buf, sizeof(int32_t) * (size_t)ni, MSG_WAITALL) ==
                    (ssize_t)(sizeof(int32_t) * (size_t)ni)) {
                    printf("[direct] template ints:");
                    for (int i = 0; i < ni; i++) printf(" [%d]=0x%x", i, buf[i]);
                    printf("\n");
                }
            }
        } else {
            failures++;
        }
        return false;
    }
    AHardwareBuffer *hb = nullptr;
    const int rc = a.recvHandle(sock, &hb);
    if (rc != 0 || !hb) {
        static int once = 0;
        if (!once++) printf("[direct] sf-hook recvHandle failed: rc=%d hb=%p\n", rc, (void *)hb);
        failures++;
        return false;
    }
    static int once = 0;
    if (!once++) printf("[direct] sf-hook import ok, scanout without compositing\n");
    return present_attach(a, win, db, hb, dst_w, dst_h);
}

}   // namespace direct
