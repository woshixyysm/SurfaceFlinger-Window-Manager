// xwayland.hpp
//
// xwayland-shell-v1
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- xwayland-shell-v1
// Xwayland（>= 23.1）在 rootless 模式下必须能绑到 xwayland_shell_v1，
// 否则直接拒绝启动 rootless。协议本身只是给 wl_surface 打一个「这是 X 窗口」的标记，
// 合成器这边不需要发任何事件，接受请求即可。
static void xwlsurf_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void xwlsurf_set_serial(wl_client *, wl_resource *r, uint32_t lo, uint32_t hi) {
    printf("[xwl] set_serial %08x%08x (xwayland_surface#%u)\n", hi, lo, wl_resource_get_id(r));
}
static const struct xwayland_surface_v1_interface xwlsurf_impl = {
    .destroy = xwlsurf_destroy,
    .set_serial = xwlsurf_set_serial,
};

static void xwlshell_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void xwlshell_get_surface(wl_client *c, wl_resource *, uint32_t id, wl_resource *surface) {
    wl_resource *r = wl_resource_create(c, &xwayland_surface_v1_interface, 1, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &xwlsurf_impl, nullptr, nullptr);
    // 表面角色标记：把 surface 也记一下，方便日志对照
    printf("[xwl] 新 X 窗口 surface#%u -> xwayland_surface#%u\n",
           surface ? wl_resource_get_id(surface) : 0, id);
}
static const struct xwayland_shell_v1_interface xwlshell_impl = {
    .destroy = xwlshell_destroy,
    .get_xwayland_surface = xwlshell_get_surface,
};

static void bind_xwlshell(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &xwayland_shell_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &xwlshell_impl, nullptr, nullptr);
    printf("[xwl] Xwayland 绑定了 xwayland_shell_v1\n");
}

// ---------------------------------------------------------------- main
