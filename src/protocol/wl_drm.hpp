// wl_drm.hpp
//
// wl_drm（老客户端/nvidia 兼容路径）
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- wl_drm
// Mesa 的 wayland EGL 在 drm_handle_device() 里会：
//   device_name = wl_drm.device 事件给的字符串
//   fd_render_gpu = open(device_name)        ← 我们要把 /dev/kgsl-3d0 给它
// 然后 freedreno 的 fd_device_new(fd) 看 drmGetVersion()->name：
//   "msm" → 上游 DRM 后端；"kgsl" → KGSL 后端（这台机器的 GPU 就在 kgsl 后面）
// 这正是硬件加速能跑起来的关键：默认它只会去开 /dev/dri/renderD128（显示器节点）。
static const char *g_drm_device = "/dev/kgsl-3d0";

static void drm_authenticate_cb(wl_client *, wl_resource *r, uint32_t id) {
    printf("[sfwm] wl_drm 认证请求 id=%u → 直接放行\n", id);
    wl_drm_send_authenticated(r);   // 这个版本的协议 authenticated 事件不带参数
}

static void drm_create_buffer_cb(wl_client *c, wl_resource *r, uint32_t id, uint32_t name,
                                 int32_t width, int32_t height, uint32_t stride,
                                 uint32_t format) {
    printf("[sfwm] wl_drm.create_buffer %dx%d name=%u（不支持，回错误）\n",
           width, height, name);
    wl_resource_post_error(r, WL_DRM_ERROR_INVALID_NAME, "不支持 name 缓冲区，请用 dmabuf");
    (void)c; (void)id; (void)stride; (void)format;
}

static void drm_create_planar_buffer_cb(wl_client *, wl_resource *r, uint32_t id, uint32_t name,
                                        int32_t width, int32_t height, uint32_t format,
                                        int32_t offset0, int32_t stride0,
                                        int32_t offset1, int32_t stride1,
                                        int32_t offset2, int32_t stride2) {
    wl_resource_post_error(r, WL_DRM_ERROR_INVALID_NAME, "不支持 planar name 缓冲区");
    (void)id; (void)name; (void)width; (void)height; (void)format;
    (void)offset0; (void)stride0; (void)offset1; (void)stride1; (void)offset2; (void)stride2;
}

static const struct wl_drm_interface drm_impl = {
    .authenticate = drm_authenticate_cb,
    .create_buffer = drm_create_buffer_cb,
    .create_planar_buffer = drm_create_planar_buffer_cb,
};

static void bind_drm(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 2) version = 2;
    wl_resource *r = wl_resource_create(c, &wl_drm_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &drm_impl, nullptr, nullptr);

    const char *dev = getenv("SFWM_DRM_DEVICE");
    if (dev && *dev) g_drm_device = dev;
    printf("[sfwm] wl_drm 绑定，告知设备 %s\n", g_drm_device);
    wl_drm_send_device(r, g_drm_device);
    wl_drm_send_format(r, WL_DRM_FORMAT_ARGB8888);
    wl_drm_send_format(r, WL_DRM_FORMAT_XRGB8888);
    wl_drm_send_capabilities(r, WL_DRM_CAPABILITY_PRIME);
}

