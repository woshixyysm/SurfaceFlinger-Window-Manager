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
#include <android/log.h>
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

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "sfwm-sfhook", __VA_ARGS__)

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
// The abstract namespace needs no directory and no label, and it works across uids that
// cannot traverse /data/adb (which is 0700 root, so surfaceflinger as uid 1000 cannot).
#define SFWM_SF_ABSTRACT "sfwm-sf"
#define SFWM_SF_MAGIC 0x5346574du   // 'SFWM'
#define AHB_USAGE_COMPOSER_OVERLAY 0x800ULL
#define AHB_METHOD_CLONE 2

// uid of the Termux user that runs sfwm; root is always accepted.
#ifndef SFWM_SF_ALLOWED_UID
#define SFWM_SF_ALLOWED_UID 10391
#endif

struct request {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
};

// 'SFWP': allocate a pool of buffers inside SurfaceFlinger and hand them to the client,
// instead of the client handing a foreign dma-buf in. Everything stays an allocation the
// mapper made itself, so nothing has to be validated on the way back in, and the client can
// reuse the same few buffers for every frame.
#define SFWM_SF_MAGIC_ALLOC 0x53465750u
#define SFWM_SF_MAX_POOL 4

struct alloc_request {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t count;
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

// Receives one request. Reports the attached fd through `out_fd` (-1 when the request
// carries none) and only fails when the message itself could not be read.
static int recv_req(int sock, void *buf, size_t len, int *out_fd)
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
    *out_fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            *out_fd = *(int *)CMSG_DATA(c);
            break;
        }
    }
    return 0;
}

static int recv_fd(int sock, void *buf, size_t len)
{
    int fd = -1;
    if (recv_req(sock, buf, len, &fd) != 0) return -1;
    return fd;
}

// Allocates the pool the client is going to render into and sends each buffer back. The
// helper keeps no reference: the handles the client receives are its own.
static int32_t serve_alloc(int sock, const struct alloc_request *r)
{
    int32_t status = 0;
    AHardwareBuffer *pool[SFWM_SF_MAX_POOL] = {0};
    int n = 0;

    if (r->count == 0 || r->count > SFWM_SF_MAX_POOL || r->width == 0 || r->height == 0) {
        status = -EINVAL;
    } else {
        uint32_t hal = hal_format_from_fourcc(r->format);
        if (!hal) hal = r->format;
        struct ahb_desc d;
        memset(&d, 0, sizeof d);
        d.width = r->width;
        d.height = r->height;
        d.layers = 1;
        d.format = hal;
        // GPU_FRAMEBUFFER is what makes this renderable by us; COMPOSER_OVERLAY is what makes
        // it acceptable to SurfaceFlinger. Without the former the GPU silently draws nowhere
        // and the layer stays black while every frame is "presented".
        d.usage = 0x300ULL | AHB_USAGE_COMPOSER_OVERLAY;
        for (; n < (int)r->count; n++) {
            if (p_alloc(&d, &pool[n]) != 0 || !pool[n]) {
                status = -ENOMEM;
                break;
            }
        }
        if (status == 0) n = (int)r->count;
    }

    if (send(sock, &status, sizeof status, MSG_NOSIGNAL) != (ssize_t)sizeof status) status = -1;
    if (status == 0) {
        for (int i = 0; i < n; i++)
            if (p_send && p_send(pool[i], sock) != 0) break;
    }
    for (int i = 0; i < n; i++)
        if (pool[i]) p_release(pool[i]);
    return status;
}

// Why the last import failed. Logging from SurfaceFlinger turned out to be unreliable, so
// the stage is handed back to the client, which prints it.
enum { S_OK = 0, S_BADREQ = 1, S_ALLOC = 2, S_TEMPLATE = 3, S_IMPORT = 4, S_CLONE = 5 };
static int32_t g_stage;
static int32_t g_code;
// Copy of the template handle's ints, sent back on failure so the client can print the
// real private_handle_t layout of this device.
static int32_t g_ints[40];
static int32_t g_nints;

// Import one client dma-buf. Returns 0 and keeps `out` on success. Every failure path
// leaves SurfaceFlinger untouched.
static int32_t import_dmabuf(uint32_t width, uint32_t height, uint32_t hal_format,
                             uint32_t stride_bytes, int fd, AHardwareBuffer **out)
{
    g_stage = S_BADREQ;
    g_code = -EINVAL;
    if (!p_alloc || !p_from_handle || !p_get_native) return -ENOSYS;
    if (width == 0 || height == 0 || hal_format == 0 || fd < 0) {
        LOGI("bad request w=%u h=%u fmt=0x%x fd=%d", width, height, hal_format, fd);
        return -EINVAL;
    }

    // The mapper only accepts handles shaped like the ones its own allocator produces, so
    // ask it for one of exactly this geometry first and then replace the descriptors with
    // the client's dma-buf. The placeholder keeps the buffer alive while we work.
    const uint32_t stride_px = stride_bytes / 4u;

    // The template must be allocated with exactly the descriptor that is later handed to
    // createFromHandle: the mapper re-checks that descriptor against the buffer it holds
    // (gralloc::BufferManager::ValidateBufferSize) and reports any mismatch as BAD_VALUE,
    // which libui turns into -EINVAL. Allocating with the plain overlay usage was the
    // reason every import used to come back as -22.
    struct ahb_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.width = width;
    rd.height = height;
    rd.layers = 1;
    rd.format = hal_format;
    rd.usage = 0x100ULL | AHB_USAGE_COMPOSER_OVERLAY;
    rd.stride = stride_px;

    AHardwareBuffer *shape = NULL;
    const int ar = p_alloc(&rd, &shape);
    if (ar != 0 || !shape) {
        g_stage = S_ALLOC;
        g_code = ar;
        LOGI("allocate %ux%u fmt=%u usage=0x%llx failed: %d", width, height, hal_format,
             (unsigned long long)rd.usage, ar);
        return ar ? ar : -ENOMEM;
    }

    const int *tmpl = p_get_native(shape);
    if (!tmpl) {
        p_release(shape);
        g_stage = S_TEMPLATE;
        LOGI("template handle missing");
        return -EINVAL;
    }
    const int nfds = tmpl[1];
    const int nints = tmpl[2];
    LOGI("template nfds=%d nints=%d", nfds, nints);
    if (nfds < 1 || nints < 14 || nints > 128) {
        p_release(shape);
        g_stage = S_TEMPLATE;
        LOGI("template shape unusable");
        return -EINVAL;
    }

    int *nh = (int *)calloc(1, sizeof(int) * 3 + sizeof(int) * (size_t)(nfds + nints));
    if (!nh) {
        p_release(shape);
        g_stage = S_ALLOC;
        return -ENOMEM;
    }
    nh[0] = 12;
    nh[1] = nfds;
    nh[2] = nints;
    // Only the first descriptor is the pixel buffer; the extra fds belong to gralloc's own
    // bookkeeping (that is where the mapper reads the format from), so they stay the
    // template's. Replacing all of them made validateBufferSize see format 0.
    nh[3] = fd;
    for (int i = 1; i < nfds; i++) nh[3 + i] = dup(tmpl[3 + i]);
    int *ints = nh + 3 + nfds;
    memcpy(ints, tmpl + 3 + nfds, sizeof(int) * (size_t)nints);
    g_nints = nints > 40 ? 40 : nints;
    for (int i = 0; i < g_nints; i++) g_ints[i] = ints[i];

    ints[2] = (int)stride_px;
    ints[3] = (int)height;
    ints[4] = (int)width;
    ints[5] = (int)height;
    ints[6] = (int)hal_format;
    ints[13] = (int)(stride_bytes * height);
    if (nints > 24) ints[24] = (int)stride_bytes;

    // Address every pixel of the client buffer; the caller only asks for this when it is
    // going to hand the result to a SurfaceControl.
    AHardwareBuffer *buffer = NULL;
    // IMPORT asks the mapper to re-register the descriptors, which only ever works for the
    // mapper's own allocations (it returns -EINVAL for a foreign dma-buf, even in
    // SurfaceFlinger). CLONE just duplicates a handle it already knows, so that is the
    // second attempt; whichever succeeds is reported so the log shows what happened.
    int32_t res = p_from_handle(&rd, nh, 1 /* IMPORT */, &buffer);
    g_stage = S_IMPORT;
    g_code = res;
    if (res != 0 || !buffer) {
        LOGI("IMPORT failed (%d), trying CLONE", res);
        buffer = NULL;
        res = p_from_handle(&rd, nh, AHB_METHOD_CLONE, &buffer);
        g_stage = S_CLONE;
        g_code = res;
        LOGI("CLONE -> %d (buffer %p)", res, (void *)buffer);
    }
    for (int i = 1; i < nfds; i++) if (nh[3 + i] >= 0) close(nh[3 + i]);
    free(nh);
    p_release(shape);
    if (res != 0 || !buffer) {
        LOGI("both methods failed, last res=%d", res);
        return res ? res : -EINVAL;
    }
    g_stage = S_OK;
    LOGI("import ok %ux%u", width, height);
    *out = buffer;
    return 0;
}

// Serves requests on one accepted connection until the peer goes away. Returns 0 to keep
// the connection open; the caller closes it.
static int serve_client(int sock)
{
    // The SELinux rule for the app domain is domain-wide, so gate on the peer uid too:
    // only root and the Termux user are allowed to ask for an import.
    struct ucred cred;
    socklen_t clen = sizeof cred;
    if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0 && cred.uid != 0 &&
        cred.uid != SFWM_SF_ALLOWED_UID) {
        LOGI("peer uid %d rejected", (int)cred.uid);
        return -1;
    }
    struct request req;
    int fd = -1;
    AHardwareBuffer *buffer = NULL;
    int32_t status;

    if (recv_req(sock, &req, sizeof req, &fd) != 0) return -1;
    if (req.magic == SFWM_SF_MAGIC_ALLOC) {
        // Same message size as `request`; the fifth word is the count instead of the stride.
        struct alloc_request ar;
        memcpy(&ar, &req, sizeof ar);
        serve_alloc(sock, &ar);
        return 0;
    }
    do {
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
        return -1;
    }
    if (status != 0) {
        // Tell the client which step failed; the log channel inside SurfaceFlinger is not
        // dependable, so this is the only reliable diagnostic.
        const int32_t diag[2] = {g_stage, g_code};
        send(sock, diag, sizeof diag, MSG_NOSIGNAL);
        send(sock, &g_nints, sizeof g_nints, MSG_NOSIGNAL);
        if (g_nints > 0) send(sock, g_ints, sizeof(int32_t) * (size_t)g_nints, MSG_NOSIGNAL);
    }
    if (status == 0 && buffer && p_send && p_send(buffer, sock) != 0) {
        // The client sees a short read and falls back.
    }
    if (buffer) p_release(buffer);
    if (fd >= 0) close(fd);
    return 0;
}

// Fills addr with the abstract socket address; returns the length to pass to bind/connect.
static socklen_t abstract_addr(struct sockaddr_un *addr)
{
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    const size_t n = strlen(SFWM_SF_ABSTRACT);
    memcpy(addr->sun_path + 1, SFWM_SF_ABSTRACT, n);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

static void *serve(void *unused)
{
    (void)unused;
    const int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0) {
        LOGI("socket() failed: %d", errno);
        return NULL;
    }
    struct sockaddr_un addr;
    const socklen_t len = abstract_addr(&addr);
    if (bind(srv, (struct sockaddr *)&addr, len) != 0) {
        LOGI("bind(@%s) failed: %d", SFWM_SF_ABSTRACT, errno);
        close(srv);
        return NULL;
    }
    if (listen(srv, 4) != 0) {
        LOGI("listen failed: %d", errno);
        close(srv);
        return NULL;
    }
    LOGI("listening on @%s", SFWM_SF_ABSTRACT);
    for (;;) {
        const int c = accept4(srv, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno == EINTR) continue;
            usleep(50000);
            continue;
        }
        for (;;) {
            if (serve_client(c) != 0) break;
        }
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
    const socklen_t len = abstract_addr(&addr);
    const int ok = connect(s, (struct sockaddr *)&addr, len) == 0;
    close(s);
    return ok;
}

__attribute__((constructor)) static void sfwm_sf_hook_init(void)
{
    // Never run this by accident: either the wrapper sets the variable, or the injector
    // creates the sentinel file. A stray LD_PRELOAD from somewhere else does nothing.
    if (!getenv("SFWM_SF_HOOK") && access(SFWM_SF_ENABLE, F_OK) != 0) {
        LOGI("not enabled (no env, no sentinel), staying idle");
        return;
    }
    // Hot injection can happen more than once; a second copy would fight for the socket.
    if (socket_alive()) {
        LOGI("socket already served by another copy, staying idle");
        return;
    }

    void *nw = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    if (!nw) {
        LOGI("dlopen(libnativewindow.so) failed: %s", dlerror());
        return;
    }
    p_alloc = (fn_alloc)dlsym(nw, "AHardwareBuffer_allocate");
    p_from_handle = (fn_from_handle)dlsym(nw, "AHardwareBuffer_createFromHandle");
    p_release = (fn_release)dlsym(nw, "AHardwareBuffer_release");
    p_send = (fn_send)dlsym(nw, "AHardwareBuffer_sendHandleToUnixSocket");
    p_get_native = (fn_get_native)dlsym(nw, "AHardwareBuffer_getNativeHandle");
    if (!p_alloc || !p_from_handle || !p_release || !p_send || !p_get_native) {
        LOGI("missing symbols: alloc=%p fromHandle=%p release=%p send=%p getNative=%p",
             (void *)p_alloc, (void *)p_from_handle, (void *)p_release, (void *)p_send,
             (void *)p_get_native);
        return;
    }

    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    // A tiny stack: this thread does nothing but hold a socket.
    pthread_attr_setstacksize(&a, 128 * 1024);
    const int rc = pthread_create(&t, &a, serve, NULL);
    pthread_attr_destroy(&a);
    LOGI("started serving thread (pthread_create=%d)", rc);
}
