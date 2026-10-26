// compositor.hpp
//
// wl_compositor（建 surface/region）
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- wl_compositor
static View *view_from_surface(wl_resource *r) { return (View *)wl_resource_get_user_data(r); }

static void compositor_create_surface(wl_client *c, wl_resource *res, uint32_t id) {
    View *v = new View();
    v->srv = &g_srv;
    v->z = g_srv.next_z++;
    v->x = (int32_t)(40 * (v->z % 5));
    v->y = (int32_t)(40 * (v->z % 5));

    wl_resource *sr = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(res), id);
    if (!sr) { delete v; wl_client_post_no_memory(c); return; }
    v->surface = sr;
    wl_resource_set_implementation(sr, &surface_impl, v, [](wl_resource *r) {
        View *vv = view_from_surface(r);
        if (!vv) return;
        for (size_t i = 0; i < g_srv.views.size(); i++) {
            if (g_srv.views[i] == vv) { g_srv.views.erase(g_srv.views.begin() + i); break; }
        }
        // 其它协议对象还挂着这个 View，先断开，避免悬空指针
        if (vv->xdg_surface) wl_resource_set_user_data(vv->xdg_surface, nullptr);
        if (vv->toplevel) wl_resource_set_user_data(vv->toplevel, nullptr);
        if (vv->layer_surface) wl_resource_set_user_data(vv->layer_surface, nullptr);
        vv->xdg_surface = nullptr;
        vv->toplevel = nullptr;
        vv->layer_surface = nullptr;
        // 从父表面的子列表里摘掉（子表面或父表面任意一方先销毁都要清）
        if (vv->sub_parent) {
            auto &s = vv->sub_parent->subs;
            for (size_t i = 0; i < s.size(); i++) if (s[i] == vv) { s.erase(s.begin() + i); break; }
            vv->sub_parent = nullptr;
        }
        vv->subs.clear();
        // 焦点指向的 View 没了 → 立刻清掉，否则后面拿野指针发 enter/leave
        if (g_srv.kb_focus == vv) g_srv.kb_focus = nullptr;
        if (g_srv.pointer_focus == vv) g_srv.pointer_focus = nullptr;
        if (g_srv.cursor_view == vv) g_srv.cursor_view = nullptr;
        if (vv->layer.win) vv->layer.hide();
        delete vv;
    });
    g_srv.views.push_back(v);
}

static void compositor_create_region(wl_client *c, wl_resource *res, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_region_interface, wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &region_impl, nullptr, region_handle_destroyed);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void bind_compositor(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 6) version = 6;   // v5 => wl_surface.offset；v6 只加了事件，没有新请求
    wl_resource *r = wl_resource_create(c, &wl_compositor_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &compositor_impl, nullptr, nullptr);
}

