// SPDX-License-Identifier: AGPL-3.0-or-later
// compositor.hpp
//
// wl_compositor: surfaces and regions.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
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
        // Detach the view from the other protocol objects before it goes away.
        if (vv->xdg_surface) wl_resource_set_user_data(vv->xdg_surface, nullptr);
        if (vv->toplevel) wl_resource_set_user_data(vv->toplevel, nullptr);
        if (vv->layer_surface) wl_resource_set_user_data(vv->layer_surface, nullptr);
        vv->xdg_surface = nullptr;
        vv->toplevel = nullptr;
        vv->layer_surface = nullptr;
        // Remove the subsurface from its parent list; either side may be destroyed first.
        if (vv->sub_parent) {
            auto &s = vv->sub_parent->subs;
            for (size_t i = 0; i < s.size(); i++) if (s[i] == vv) { s.erase(s.begin() + i); break; }
            vv->sub_parent = nullptr;
        }
        vv->subs.clear();
        // Drop the focus pointer, otherwise later enter and leave events use a stale view.
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
    if (version > 6) version = 6;   // v5 adds wl_surface.offset; v6 only adds events
    wl_resource *r = wl_resource_create(c, &wl_compositor_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &compositor_impl, nullptr, nullptr);
}

