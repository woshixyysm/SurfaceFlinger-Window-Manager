// syncobj.hpp
//
// wp_linux_drm_syncobj_v1（显式同步）
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- wp_linux_drm_syncobj_manager_v1
// 真显式同步：客户端提交 dmabuf 时给出「acquire 时间点」，我们必须等它 signaled
// 才能读这块 buffer；读完再 signal 它给的「release 时间点」，客户端才知道能复用。
// 拿不到 syncobj 就退化成隐式同步（不阻塞），保证不会把整个合成器卡死。
static int g_sync_drm_fd = -1;

static int sync_drm_fd(void) {
    if (g_sync_drm_fd >= 0) return g_sync_drm_fd;
    const char *dev = getenv("SFWM_DRM_DEVICE");
    g_sync_drm_fd    = open(dev && *dev ? dev : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (g_sync_drm_fd < 0)
        fprintf(stderr, "[syncobj] 打不开 drm 节点: %s\n", strerror(errno));
    return g_sync_drm_fd;
}

struct SyncTimeline {
    uint32_t handle = 0;
};

// timeline 接口只有 destroy 一个请求 → 生成头没有 requests 结构，自己声明
struct sync_timeline_iface_impl {
    void (*destroy)(struct wl_client *client, struct wl_resource *resource);
};
static void stl_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct sync_timeline_iface_impl stl_impl = { .destroy = stl_destroy };
static void stl_resource_destroyed(wl_resource *r) { delete (SyncTimeline *)wl_resource_get_user_data(r); }

static void sync_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void sync_import_timeline(wl_client *c, wl_resource *res, uint32_t id, int32_t fd) {
    wl_resource *r = wl_resource_create(c, &wp_linux_drm_syncobj_timeline_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        close(fd);
        wl_client_post_no_memory(c);
        return;
    }

    const int drmfd = sync_drm_fd();
    auto     *tl    = new SyncTimeline();

    if (drmfd < 0 || drmSyncobjFDToHandle(drmfd, fd, &tl->handle) != 0) {
        printf("[syncobj] import_timeline 失败（drv 不支持 timeline syncobj？）\n");
        close(fd);
        delete tl;
        wl_resource_post_error(res, 0, "failed to import timeline");
        wl_resource_destroy(r);
        return;
    }
    close(fd);
    wl_resource_set_implementation(r, (const void *)&stl_impl, tl, stl_resource_destroyed);
    printf("[syncobj] timeline imported, handle=%u\n", tl->handle);
}

static void sync_surf_destroy(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v) v->syncobj_surface = nullptr;
    wl_resource_destroy(r);
}

static void sync_set_points(View *v, bool release, wl_resource *timeline, uint32_t hi, uint32_t lo) {
    if (!v || !timeline) return;
    SyncTimeline *tl = (SyncTimeline *)wl_resource_get_user_data(timeline);
    if (!tl) return;
    const uint64_t point = ((uint64_t)hi << 32) | lo;
    if (release) {
        v->sync_rel_handle = tl->handle;
        v->sync_rel_point  = point;
        v->sync_has_rel    = true;
    } else {
        v->sync_acq_handle = tl->handle;
        v->sync_acq_point  = point;
        v->sync_has_acq    = true;
    }
}

static void sync_set_acquire(wl_client *, wl_resource *r, wl_resource *timeline, uint32_t hi, uint32_t lo) {
    sync_set_points((View *)wl_resource_get_user_data(r), false, timeline, hi, lo);
}

static void sync_set_release(wl_client *, wl_resource *r, wl_resource *timeline, uint32_t hi, uint32_t lo) {
    sync_set_points((View *)wl_resource_get_user_data(r), true, timeline, hi, lo);
}

static const struct wp_linux_drm_syncobj_surface_v1_interface sync_surf_impl = {
    .destroy           = sync_surf_destroy,
    .set_acquire_point = sync_set_acquire,
    .set_release_point = sync_set_release,
};

static void sync_get_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    View *v = view_from_surface(surface);
    if (!v) {
        wl_resource_post_error(res, 0, "syncobj on unknown surface");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wp_linux_drm_syncobj_surface_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    v->syncobj_surface = r;
    wl_resource_set_implementation(r, &sync_surf_impl, v, nullptr);
}

static const struct wp_linux_drm_syncobj_manager_v1_interface sync_mgr_impl = {
    .destroy         = sync_mgr_destroy,
    .get_surface     = sync_get_surface,
    .import_timeline = sync_import_timeline,
};

// 探一下驱动到底支不支持 timeline syncobj。
// 注意：本机 msm_drm（Adreno）两个节点都是 SYNCOBJ=0 / TIMELINE=0，
// drmSyncobjCreate 直接 EOPNOTSUPP —— 驱动没开这个特性，内核 core 有也没用。
// 支持才宣告，否则客户端会以为能用然后卡在等待上。
static bool syncobj_supported(void) {
    const int fd = sync_drm_fd();
    if (fd < 0) return false;

    uint64_t cap = 0;
    if (drmGetCap(fd, DRM_CAP_SYNCOBJ, &cap) != 0 || !cap) return false;
    if (drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || !cap) return false;

    uint32_t so = 0;
    if (drmSyncobjCreate(fd, 0, &so) != 0) return false;
    drmSyncobjDestroy(fd, so);
    return true;
}

static void bind_syncobj(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_linux_drm_syncobj_manager_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &sync_mgr_impl, nullptr, nullptr);
}

// blit 前等 acquire 点，blit 后 signal release 点
static void sync_wait_acquire(View *v) {
    if (!v || !v->sync_has_acq) return;
    const int drmfd = sync_drm_fd();
    if (drmfd < 0) return;
    uint32_t handles[1] = {v->sync_acq_handle};
    uint64_t points[1]  = {v->sync_acq_point};
    // 最多等 100ms：客户端异常时宁可画个旧帧也不能把事件循环卡死
    const int ret = drmSyncobjTimelineWait(drmfd, handles, points, 1, 100ull * 1000ull * 1000ull,
                                          DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, nullptr);
    if (ret != 0) {
        static int warn = 0;
        if (warn++ < 3)
            printf("[syncobj] acquire 等待超时/失败 (point=%llu): %s\n",
                   (unsigned long long)v->sync_acq_point, strerror(errno));
    }
    v->sync_has_acq = false;
}

static void sync_signal_release(View *v) {
    if (!v || !v->sync_has_rel) return;
    const int drmfd = sync_drm_fd();
    if (drmfd < 0) return;
    uint32_t handles[1] = {v->sync_rel_handle};
    uint64_t points[1]  = {v->sync_rel_point};
    if (drmSyncobjTimelineSignal(drmfd, handles, points, 1) != 0) {
        static int warn = 0;
        if (warn++ < 3)
            printf("[syncobj] signal release 失败 (point=%llu): %s\n",
                   (unsigned long long)v->sync_rel_point, strerror(errno));
    }
    v->sync_has_rel = false;
}

