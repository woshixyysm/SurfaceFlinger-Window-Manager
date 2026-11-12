// SPDX-License-Identifier: AGPL-3.0-or-later
// sfwm_sf_hook.c - optional SurfaceFlinger-side helper for sfwm.
//
// This library is LD_PRELOADed into surfaceflinger by the wrapper in system/bin. It exists
// for one reason: AHardwareBuffer_createFromHandle() with the IMPORT method needs the
// gralloc mapper, and the mapper is only reachable from SurfaceFlinger's own process (it is
// a passthrough HAL module, not a binder service). From any other process every IMPORT
// fails with EINVAL, which is what blocks handing a client dma-buf straight to
// SurfaceFlinger for scanout.
//
// The helper therefore does the smallest possible job and nothing else:
//
//   request : u32 magic, u32 width, u32 height, u32 hal_format, u32 stride_bytes,
//             plus one SCM_RIGHTS fd (the client dma-buf)
//   response: i32 status (0 = ok), then, when ok, an AHardwareBuffer sent with
//             AHardwareBuffer_sendHandleToUnixSocket()
//
// sfwm receives that handle with AHardwareBuffer_recvHandleFromUnixSocket(), which uses the
// CLONE method and needs no mapper, and then sets the buffer on its layer itself.
//
// Nothing here touches SurfaceFlinger state, links against its internals, or throws: a
// failure only means the request is answered with an error and sfwm falls back to its
// zero-copy blit path. Every entry point is wrapped so the helper cannot take the system UI
// down with it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>

typedef struct AHardwareBuffer AHardwareBuffer;

struct ahb_desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
};

typedef int (*fn_alloc)(const struct ahb_desc *, AHardwareBuffer **);
typedef int (*fn_from_handle)(const struct ahb_desc *, const void *, int32_t, AHardwareBuffer **);
typedef void (*fn_release)(AHardwareBuffer *);
typedef int (*fn_send)(const AHardwareBuffer *, int);
typedef const int *(*fn_get_native)(const AHardwareBuffer *);

static fn_alloc p_alloc;
static fn_from_handle p_from_handle;
static fn_release p_release;
static fn_send p_send;
static fn_get_native p_get_native;

#define SFWM_SF_SOCK "/data/adb/sfwm-sf/sf.sock"
#define SFWM_SF_MAGIC 0x5346574du   // 'SFWM'
#define AHB_USAGE_COMPOSER_OVERLAY 0x800ULL
#define AHB_METHOD_CLONE 2

struct request {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
};

// Copy FreeDesktop DRM fourcc to HAL_PIXEL_FORMAT_*. Kept local so the helper has no
// dependency beyond libc and libnativewindow.
static uint32_t hal_format_from_fourcc(uint32_t fourcc)
{
    switch (fourcc) {
    case 0x34325241:   // ARGB8888
    case 0x34325258:   // XRGB8888
        return 5;      // HAL_PIXEL_FORMAT_BGRA_8888
    case 0x34324241:   // ABGR8888
    case 0x34324258:   // XBGR8888
        return 1;      // HAL_PIXEL_FORMAT_RGBA_8888
    default:
        return 0;
    }
}

static int recv_fd(int sock, void *buf, size_t len)
{
    struct iovec iov = {.iov_base = buf, .iov_len = len};
    char ctl[CMSG_SPACE(sizeof(int))];
    memset(ctl, 0, sizeof ctl);
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = sizeof ctl;
    const ssize_t n = recvmsg(sock, &msg, 0);
    if (n != (ssize_t)len) return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            return *(int *)CMSG_DATA(c);
    }
    return -1;
}

// Import one client dma-buf. Returns 0 and keeps `out` on success. Every failure path
// leaves SurfaceFlinger untouched.
static int32_t import_dmabuf(uint32_t width, uint32_t height, uint32_t hal_format,
                             uint32_t stride_bytes, int fd, AHardwareBuffer **out)
{
    if (!p_alloc || !p_from_handle || !p_get_native) return -ENOSYS;
    if (width == 0 || height == 0 || hal_format == 0 || fd < 0) return -EINVAL;

    // The mapper only accepts handles shaped like the ones its own allocator produces, so
    // ask it for one of exactly this geometry first and then replace the descriptors with
    // the client's dma-buf. The placeholder keeps the buffer alive while we work.
    struct ahb_desc d;
    memset(&d, 0, sizeof d);
    d.width = width;
    d.height = height;
    d.layers = 1;
    d.format = hal_format;
    d.usage = AHB_USAGE_COMPOSER_OVERLAY;

    AHardwareBuffer *shape = NULL;
    const int ar = p_alloc(&d, &shape);
    if (ar != 0 || !shape) return ar ? ar : -ENOMEM;

    const int *tmpl = p_get_native(shape);
    if (!tmpl) {
        p_release(shape);
        return -EINVAL;
    }
    const int nfds = tmpl[1];
    const int nints = tmpl[2];
    if (nfds < 1 || nints < 14 || nints > 128) {
        p_release(shape);
        return -EINVAL;
    }

    int *nh = (int *)calloc(1, sizeof(int) * 3 + sizeof(int) * (size_t)(nfds + nints));
    if (!nh) {
        p_release(shape);
        return -ENOMEM;
    }
    nh[0] = 12;
    nh[1] = nfds;
    nh[2] = nints;
    nh[3] = fd;
    for (int i = 1; i < nfds; i++) nh[3 + i] = dup(fd);
    int *ints = nh + 3 + nfds;
    memcpy(ints, tmpl + 3 + nfds, sizeof(int) * (size_t)nints);

    const uint32_t stride_px = stride_bytes / 4u;
    ints[2] = (int)stride_px;
    ints[3] = (int)height;
    ints[4] = (int)width;
    ints[5] = (int)height;
    ints[6] = (int)hal_format;
    ints[13] = (int)(stride_bytes * height);
    if (nints > 24) ints[24] = (int)stride_bytes;

    // Address every pixel of the client buffer; the caller only asks for this when it is
    // going to hand the result to a SurfaceControl.
    struct ahb_desc rd = d;
    rd.usage = 0x100ULL | AHB_USAGE_COMPOSER_OVERLAY;
    rd.stride = stride_px;

    AHardwareBuffer *buffer = NULL;
    const int32_t res = p_from_handle(&rd, nh, 1 /* IMPORT: works here, in SurfaceFlinger */,
                                      &buffer);
    for (int i = 1; i < nfds; i++) close(nh[3 + i]);
    free(nh);
    p_release(shape);
    if (res != 0 || !buffer) return res ? res : -EINVAL;
    *out = buffer;
    return 0;
}

static void serve_client(int sock)
{
    struct request req;
    int fd = -1;
    AHardwareBuffer *buffer = NULL;
    int32_t status;

    do {
        fd = recv_fd(sock, &req, sizeof req);
        if (fd < 0) {
            status = -EBADF;
            break;
        }
        if (req.magic != SFWM_SF_MAGIC) {
            status = -EINVAL;
            break;
        }
        if (hal_format_from_fourcc(req.format) == 0) {
            // The client already knows the HAL format; accept either spelling.
            status = import_dmabuf(req.width, req.height, req.format, req.stride, fd, &buffer);
        } else {
            status = import_dmabuf(req.width, req.height, hal_format_from_fourcc(req.format),
                                   req.stride, fd, &buffer);
        }
    } while (0);

    if (send(sock, &status, sizeof status, MSG_NOSIGNAL) != (ssize_t)sizeof status) {
        if (buffer) p_release(buffer);
        if (fd >= 0) close(fd);
        return;
    }
    if (status == 0 && buffer && p_send && p_send(buffer, sock) != 0) {
        // The client sees a short read and falls back.
    }
    if (buffer) p_release(buffer);
    if (fd >= 0) close(fd);
}

static void *serve(void *unused)
{
    (void)unused;
    mkdir("/data/adb/sfwm-sf", 0700);
    unlink(SFWM_SF_SOCK);
    const int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0) return NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SFWM_SF_SOCK, sizeof addr.sun_path - 1);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(srv);
        return NULL;
    }
    chmod(SFWM_SF_SOCK, 0600);
    if (listen(srv, 4) != 0) {
        close(srv);
        return NULL;
    }
    for (;;) {
        const int c = accept4(srv, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno == EINTR) continue;
            usleep(50000);
            continue;
        }
        serve_client(c);
        close(c);
    }
    return NULL;
}

// Two ways to enable the helper: the module's wrapper sets SFWM_SF_HOOK, and the ptrace
// injector used while developing just creates ENABLE_FILE, because it cannot set the
// environment of the process it attaches to.
#define SFWM_SF_ENABLE "/data/adb/sfwm-sf/enable"

static int socket_alive(void)
{
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return 0;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SFWM_SF_SOCK, sizeof addr.sun_path - 1);
    const int ok = connect(s, (struct sockaddr *)&addr, sizeof addr) == 0;
    close(s);
    return ok;
}

__attribute__((constructor)) static void sfwm_sf_hook_init(void)
{
    // Never run this by accident: either the wrapper sets the variable, or the injector
    // creates the sentinel file. A stray LD_PRELOAD from somewhere else does nothing.
    if (!getenv("SFWM_SF_HOOK") && access(SFWM_SF_ENABLE, F_OK) != 0) return;
    // Hot injection can happen more than once; a second copy would fight for the socket.
    if (socket_alive()) return;

    void *nw = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    if (!nw) return;
    p_alloc = (fn_alloc)dlsym(nw, "AHardwareBuffer_allocate");
    p_from_handle = (fn_from_handle)dlsym(nw, "AHardwareBuffer_createFromHandle");
    p_release = (fn_release)dlsym(nw, "AHardwareBuffer_release");
    p_send = (fn_send)dlsym(nw, "AHardwareBuffer_sendHandleToUnixSocket");
    p_get_native = (fn_get_native)dlsym(nw, "AHardwareBuffer_getNativeHandle");
    if (!p_alloc || !p_from_handle || !p_release || !p_send || !p_get_native) return;

    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    // A tiny stack: this thread does nothing but hold a socket.
    pthread_attr_setstacksize(&a, 128 * 1024);
    pthread_create(&t, &a, serve, NULL);
    pthread_attr_destroy(&a);
}
