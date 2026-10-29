// SPDX-License-Identifier: AGPL-3.0-or-later
// foreign_toplevel_mgr.hpp
//
// zwlr_foreign_toplevel_manager_v1, the older window list and control protocol.
// It is a wlroots protocol, with its XML in protocols/.
//
// Division of labour with ext_foreign_toplevel_list_v1:
//   ext_*  is read only and is what new clients use;
//   zwlr_* can also activate, maximise and close windows, which waybar,
//          quickshell, wlrctl and nwg-drawer rely on.
// Both are advertised; clients pick one.
//
// It mirrors the view list: a handle per manager on map, resent on title or app
// id changes, and closed when the surface goes away.
//
// Requires View, g_srv, kb_set_focus, raise_view and toplevel_send_configure from
// shell/xdg_shell.hpp and input/pointer.hpp.
#pragma once

#include "wlr-foreign-toplevel-management-unstable-v1-server-protocol.h"
#include <cstdio>
#include <cstring>
#include <vector>

struct FtlmHandle {
    View        *v = nullptr;
    wl_resource *res = nullptr;
    bool         surf_gone_fired = false;
    wl_listener  surf_gone{};
};
static std::vector<wl_resource *> g_ftlm_mgrs;
static std::vector<FtlmHandle *>  g_ftlm_handles;

static FtlmHandle *ftlm_of(wl_resource *r) {
    return r ? (FtlmHandle *)wl_resource_get_user_data(r) : nullptr;
}

// State array: 0 maximised, 1 minimised, 2 activated, 3 fullscreen.
static void ftlm_send_state(wl_resource *h, View *v) {
    if (!h || !v) return;
    struct wl_array a;
    wl_array_init(&a);
    if (v->want_maximized) {
        uint32_t *p = (uint32_t *)wl_array_add(&a, sizeof(uint32_t));
        if (p) *p = 0;
    }
    if (g_srv.kb_focus == v) {
        uint32_t *p = (uint32_t *)wl_array_add(&a, sizeof(uint32_t));
        if (p) *p = 2;
    }
    if (v->want_fullscreen) {
        uint32_t *p = (uint32_t *)wl_array_add(&a, sizeof(uint32_t));
        if (p) *p = 3;
    }
    // libwayland copies the array during send, so releasing it here is safe
    zwlr_foreign_toplevel_handle_v1_send_state(h, &a);
    wl_array_release(&a);
}

static void ftlm_send_full(wl_resource *h, View *v) {
    if (!h || !v) return;
    if (v->title[0])  zwlr_foreign_toplevel_handle_v1_send_title(h, v->title);
    if (v->app_id[0]) zwlr_foreign_toplevel_handle_v1_send_app_id(h, v->app_id);
    ftlm_send_state(h, v);
    zwlr_foreign_toplevel_handle_v1_send_done(h);
}

// ------------------------------------------------------- handle requests
static void ftlm_h_set_maximized(wl_client *, wl_resource *r) {
    FtlmHandle *fh = ftlm_of(r);
    if (!fh || !fh->v) return;
    fh->v->want_maximized = true;
    toplevel_send_configure(fh->v);
    ftlm_send_full(fh->res, fh->v);
    printf("[ftlm] maximise '%s'\n", fh->v->title[0] ? fh->v->title : "view");
}
static void ftlm_h_unset_maximized(wl_client *, wl_resource *r) {
    FtlmHandle *fh = ftlm_of(r);
    if (!fh || !fh->v) return;
    fh->v->want_maximized = false;
    toplevel_send_configure(fh->v);
    ftlm_send_full(fh->res, fh->v);
    printf("[ftlm] unmaximise '%s'\n", fh->v->title[0] ? fh->v->title : "view");
}
// SurfaceFlinger layers have no minimised state, only hidden, and a hidden layer
// cannot be restored from the taskbar, so the state is only recorded.
static void ftlm_h_set_minimized(wl_client *, wl_resource *r) {
    FtlmHandle *fh = ftlm_of(r);
    printf("[ftlm] minimise requested, recorded only: %s\n",
           (fh && fh->v && fh->v->title[0]) ? fh->v->title : "");
    if (fh && fh->res) zwlr_foreign_toplevel_handle_v1_send_done(fh->res);
}
static void ftlm_h_unset_minimized(wl_client *, wl_resource *r) {
    FtlmHandle *fh = ftlm_of(r);
    if (fh && fh->res) zwlr_foreign_toplevel_handle_v1_send_done(fh->res);
}
static void ftlm_h_activate(wl_client *, wl_resource *r, wl_resource *) {
    FtlmHandle *fh = ftlm_of(r);
    if (!fh || !fh->v) return;
    // equivalent to clicking the window: focus and raise
    kb_set_focus(fh->v);
    raise_view(fh->v);
    ftlm_send_full(fh->res, fh->v);
    printf("[ftlm] activate '%s'\n", fh->v->title[0] ? fh->v->title : "view");
}
static void ftlm_h_close(wl_client *, wl_resource *r) {
    FtlmHandle *fh = ftlm_of(r);
    if (!fh || !fh->v) return;
    if (fh->v->toplevel) {
        xdg_toplevel_send_close(fh->v->toplevel);
        printf("[ftlm] close requested for '%s'\n", fh->v->title[0] ? fh->v->title : "view");
    }
}
// The taskbar reports where the window button is; used for minimise animations.
// Minimising is unsupported, so the value is recorded only.
static void ftlm_h_set_rectangle(wl_client *, wl_resource *r, wl_resource *surface,
                                 int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)surface;
    printf("[ftlm] set_rectangle (%d,%d %dx%d)\n", x, y, w, h);
    (void)r;
}
static void ftlm_h_set_fullscreen(wl_client *, wl_resource *r, wl_resource *) {
    FtlmHandle *fh = ftlm_of(r);
    if (!fh || !fh->v) return;
    fh->v->want_fullscreen = true;
    toplevel_send_configure(fh->v);
    ftlm_send_full(fh->res, fh->v);
}
static void ftlm_h_unset_fullscreen(wl_client *, wl_resource *r) {
    FtlmHandle *fh = ftlm_of(r);
    if (!fh || !fh->v) return;
    fh->v->want_fullscreen = false;
    toplevel_send_configure(fh->v);
    ftlm_send_full(fh->res, fh->v);
}
static void ftlm_h_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct zwlr_foreign_toplevel_handle_v1_interface ftlm_h_impl = {
    .set_maximized    = ftlm_h_set_maximized,
    .unset_maximized  = ftlm_h_unset_maximized,
    .set_minimized    = ftlm_h_set_minimized,
    .unset_minimized  = ftlm_h_unset_minimized,
    .activate         = ftlm_h_activate,
    .close            = ftlm_h_close,
    .set_rectangle    = ftlm_h_set_rectangle,
    .destroy          = ftlm_h_destroy,
    .set_fullscreen   = ftlm_h_set_fullscreen,
    .unset_fullscreen = ftlm_h_unset_fullscreen,
};

static void ftlm_h_res_gone(wl_resource *r) {
    for (size_t i = 0; i < g_ftlm_handles.size(); i++) {
        if (g_ftlm_handles[i] && g_ftlm_handles[i]->res == r) {
            FtlmHandle *fh = g_ftlm_handles[i];
            g_ftlm_handles.erase(g_ftlm_handles.begin() + i);
            // If the surface died first the listener was already removed by libwayland, and
            // removing it again would corrupt the list.
            if (!fh->surf_gone_fired && fh->v && fh->v->surface && fh->surf_gone.link.next)
                wl_list_remove(&fh->surf_gone.link);
            delete fh;
            return;
        }
    }
}

static void ftlm_on_surface_gone(wl_listener *l, void *) {
    FtlmHandle *fh = wl_container_of(l, fh, surf_gone);
    if (!fh) return;
    fh->surf_gone_fired = true;
    if (fh->res) {
        zwlr_foreign_toplevel_handle_v1_send_closed(fh->res);
        wl_resource_destroy(fh->res);       // triggers ftlm_h_res_gone
    }
}

// output_enter needs the wl_output resource the client bound itself, since a
// server-created one has no id and the client reports an unknown object.
// Taskbars usually bind it; when absent the event is skipped.
static wl_resource *ftlm_output_for(wl_client *c) { return output_for_client(c); }

static void ftlm_make_handle(View *v, wl_resource *mgr) {
    if (!v || !mgr || !v->surface) return;
    wl_client *c = wl_resource_get_client(mgr);
    for (FtlmHandle *fh : g_ftlm_handles)
        if (fh && fh->v == v && fh->res && wl_resource_get_client(fh->res) == c) return;

    wl_resource *h = wl_resource_create(c, &zwlr_foreign_toplevel_handle_v1_interface,
                                        wl_resource_get_version(mgr), 0);
    if (!h) { wl_client_post_no_memory(c); return; }

    // user_data has to be the FtlmHandle, or activate, close and similar requests
    // cannot be resolved and are dropped silently.

    FtlmHandle *fh = new FtlmHandle();
    fh->v = v;
    fh->res = h;
    fh->surf_gone.notify = ftlm_on_surface_gone;
    wl_resource_set_implementation(h, &ftlm_h_impl, fh, ftlm_h_res_gone);
    wl_resource_add_destroy_listener(v->surface, &fh->surf_gone);
    g_ftlm_handles.push_back(fh);

    zwlr_foreign_toplevel_manager_v1_send_toplevel(mgr, h);
    ftlm_send_full(h, v);
    // a single output, so every window enters it
    if (wl_resource *o = ftlm_output_for(c))
        zwlr_foreign_toplevel_handle_v1_send_output_enter(h, o);
}

static void ftlm_on_view_mapped(View *v) {
    for (wl_resource *m : g_ftlm_mgrs) ftlm_make_handle(v, m);
}

static void ftlm_on_view_changed(View *v) {
    for (FtlmHandle *fh : g_ftlm_handles)
        if (fh && fh->v == v && fh->res) ftlm_send_full(fh->res, v);
}

// ------------------------------------------------------------ manager
static void ftlm_mgr_stop(wl_client *, wl_resource *r) {
    zwlr_foreign_toplevel_manager_v1_send_finished(r);
    wl_resource_destroy(r);
}
static const struct zwlr_foreign_toplevel_manager_v1_interface ftlm_mgr_impl = {
    .stop = ftlm_mgr_stop,
};

static void ftlm_mgr_gone(wl_resource *r) {
    for (size_t i = 0; i < g_ftlm_mgrs.size(); i++)
        if (g_ftlm_mgrs[i] == r) { g_ftlm_mgrs.erase(g_ftlm_mgrs.begin() + i); break; }
}

static void bind_foreign_toplevel_mgr(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 3) version = 3;
    wl_resource *mgr = wl_resource_create(c, &zwlr_foreign_toplevel_manager_v1_interface,
                                          version, id);
    if (!mgr) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(mgr, &ftlm_mgr_impl, nullptr, ftlm_mgr_gone);
    g_ftlm_mgrs.push_back(mgr);

    int n = 0;
    for (View *v : g_srv.views)
        if (v && v->mapped && !v->is_cursor && !v->is_subsurface) { ftlm_make_handle(v, mgr); n++; }
    printf("[ftlm] zwlr_foreign_toplevel_manager_v1 bound, %d windows announced\n", n);
}
