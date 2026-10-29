// SPDX-License-Identifier: AGPL-3.0-or-later
// xdg_shell.hpp
//
// xdg_wm_base / toplevel / popup / configure
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ---------------------------------------------------------------- xdg_wm_base
struct Positioner {
    int32_t w = 0, h = 0;
    int32_t ax = 0, ay = 0, aw = 0, ah = 0;
    int32_t ox = 0, oy = 0;
    uint32_t anchor = 0, gravity = 0;
};

static void xdg_positioner_destroy(wl_client *, wl_resource *r) {
    wl_resource_destroy(r);
}
static void xdg_positioner_set_size(wl_client *, wl_resource *r, int32_t w, int32_t h) {
    Positioner *p = (Positioner *)wl_resource_get_user_data(r);
    if (p) { p->w = w; p->h = h; }
}
static void xdg_positioner_set_anchor_rect(wl_client *, wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    Positioner *p = (Positioner *)wl_resource_get_user_data(r);
    if (p) { p->ax = x; p->ay = y; p->aw = w; p->ah = h; }
}
static void xdg_positioner_set_anchor(wl_client *, wl_resource *r, uint32_t a) {
    Positioner *p = (Positioner *)wl_resource_get_user_data(r);
    if (p) p->anchor = a;
}
static void xdg_positioner_set_gravity(wl_client *, wl_resource *r, uint32_t g) {
    Positioner *p = (Positioner *)wl_resource_get_user_data(r);
    if (p) p->gravity = g;
}
static void xdg_positioner_set_constraint_adjustment(wl_client *, wl_resource *, uint32_t) {}
static void xdg_positioner_set_offset(wl_client *, wl_resource *r, int32_t x, int32_t y) {
    Positioner *p = (Positioner *)wl_resource_get_user_data(r);
    if (p) { p->ox = x; p->oy = y; }
}
// The three requests added in v3; set_reactive takes no arguments in this header.
static void xdg_positioner_set_reactive(wl_client *, wl_resource *) {}
static void xdg_positioner_set_parent_size(wl_client *, wl_resource *, int32_t, int32_t) {}
static void xdg_positioner_set_parent_configure(wl_client *, wl_resource *, uint32_t) {}

static const struct xdg_positioner_interface positioner_impl = {
    .destroy = xdg_positioner_destroy,
    .set_size = xdg_positioner_set_size,
    .set_anchor_rect = xdg_positioner_set_anchor_rect,
    .set_anchor = xdg_positioner_set_anchor,
    .set_gravity = xdg_positioner_set_gravity,
    .set_constraint_adjustment = xdg_positioner_set_constraint_adjustment,
    .set_offset = xdg_positioner_set_offset,
    .set_reactive = xdg_positioner_set_reactive,
    .set_parent_size = xdg_positioner_set_parent_size,
    .set_parent_configure = xdg_positioner_set_parent_configure,
};

static void toplevel_destroy(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v) v->toplevel = nullptr;
    wl_resource_destroy(r);
}
static void toplevel_set_parent(wl_client *, wl_resource *, wl_resource *) {}
static void toplevel_set_title(wl_client *, wl_resource *r, const char *t) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v && t) {
        snprintf(v->title, sizeof(v->title), "%s", t);
        ftl_on_view_changed(v);    // toplevel lists see the new title immediately
        ftlm_on_view_changed(v);
    }
}
static void toplevel_set_app_id(wl_client *, wl_resource *r, const char *a) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v && a) {
        snprintf(v->app_id, sizeof(v->app_id), "%s", a);
        ftl_on_view_changed(v);
        ftlm_on_view_changed(v);
    }
}
static void toplevel_show_window_menu(wl_client *, wl_resource *, wl_resource *, uint32_t, int32_t, int32_t) {}
static void toplevel_move(wl_client *, wl_resource *r, wl_resource *, uint32_t) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    // Client-initiated move; the input layer performs the interactive drag.
    g_srv.move_view = v;
    printf("[input] client requested a move: %s\n", v->title[0] ? v->title : "view");
}
static void toplevel_resize(wl_client *, wl_resource *, wl_resource *, uint32_t, uint32_t) {}
static void toplevel_set_max_size(wl_client *, wl_resource *, int32_t, int32_t) {}
static void toplevel_set_min_size(wl_client *, wl_resource *, int32_t, int32_t) {}

// The render scale the client actually uses: buffer pixels per logical pixel. The
// reported buf_scale is preferred, but clients such as Qt and OBS never call
// set_buffer_scale and only follow the wl_output scale. Using the wrong factor produces
// a window wider than the output with the right edge cropped. From the second configure
// on, the measured buffer size divided by the requested size corrects the guess.
static int32_t client_scale(View *v) {
    if (!v) return g_scale > 0 ? g_scale : 1;
    if (v->buf_scale > 1) return v->buf_scale;
    if (v->req_w > 0 && v->bw > 0) {
        int32_t r = (int32_t)(((int64_t)v->bw + v->req_w / 2) / v->req_w);
        if (r < 1) r = 1;
        if (r > 4) r = 4;
        return r;
    }
    return g_scale > 0 ? g_scale : 1;
}

static void toplevel_send_configure(View *v) {
    // The window may be going away; touching it here would fault.
    if (!v || !v->toplevel || !v->xdg_surface) return;
    struct wl_array states;
    wl_array_init(&states);
    uint32_t *a = (uint32_t *)wl_array_add(&states, 4 * sizeof(uint32_t));
    int n = 0;
    if (a) {
        a[n++] = XDG_TOPLEVEL_STATE_ACTIVATED;
        if (v->want_maximized) a[n++] = XDG_TOPLEVEL_STATE_MAXIMIZED;
        if (v->want_fullscreen) a[n++] = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }

    // Never send the full output size, or dialogs get stretched. 0x0 lets the client decide.
    int32_t lw = 0, lh = 0;
    const int32_t cfgScale = client_scale(v);   // measured client scale
    if (v->want_maximized || v->want_fullscreen) {
        lw = g_out_w / cfgScale;
        lh = g_out_h / cfgScale;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
    } else if (v->req_w > 0 && v->req_h > 0) {
        // A size chosen by dragging an edge takes priority over the client's own choice.
        lw = v->req_w;
        lh = v->req_h;
    }

    xdg_toplevel_send_configure(v->toplevel, lw, lh, &states);
    wl_array_release(&states);

    const int tver = wl_resource_get_version(v->toplevel);
    // v4: report the maximum window size.
    if (tver >= 4) {
        int32_t mw = g_out_w / cfgScale;
        int32_t mh = g_out_h / cfgScale;
        xdg_toplevel_send_configure_bounds(v->toplevel, mw, mh);
    }
    // v5: window menu, maximise and fullscreen capabilities.
    if (tver >= 5) {
        struct wl_array caps;
        wl_array_init(&caps);
        const uint32_t vals[3] = {
            XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU,
            XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE,
            XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN,
        };
        for (int i = 0; i < 3; i++) {
            uint32_t *c = (uint32_t *)wl_array_add(&caps, sizeof(uint32_t));
            if (c) *c = vals[i];
        }
        xdg_toplevel_send_wm_capabilities(v->toplevel, &caps);
        wl_array_release(&caps);
    }

    xdg_surface_send_configure(v->xdg_surface, wl_display_next_serial(g_srv.dpy));
}

static void toplevel_set_maximized(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->want_maximized = true;
    v->x = 0; v->y = 0;
    if (v->mapped) v->place();
    toplevel_send_configure(v);
}
static void toplevel_unset_maximized(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->want_maximized = false;
    toplevel_send_configure(v);
}
static void toplevel_set_fullscreen(wl_client *, wl_resource *r, wl_resource *) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->want_fullscreen = true;
    v->x = 0; v->y = 0;
    if (v->mapped) v->place();
    toplevel_send_configure(v);
}
static void toplevel_unset_fullscreen(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->want_fullscreen = false;
    toplevel_send_configure(v);
}
static void toplevel_set_minimized(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v && v->layer.win) v->layer.hide();
}

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = toplevel_destroy,
    .set_parent = toplevel_set_parent,
    .set_title = toplevel_set_title,
    .set_app_id = toplevel_set_app_id,
    .show_window_menu = toplevel_show_window_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
    .set_max_size = toplevel_set_max_size,
    .set_min_size = toplevel_set_min_size,
    .set_maximized = toplevel_set_maximized,
    .unset_maximized = toplevel_unset_maximized,
    .set_fullscreen = toplevel_set_fullscreen,
    .unset_fullscreen = toplevel_unset_fullscreen,
    .set_minimized = toplevel_set_minimized,
};

static void popup_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void popup_grab(wl_client *, wl_resource *, wl_resource *, uint32_t) {}
static void popup_reposition(wl_client *, wl_resource *r, wl_resource *, uint32_t token) {
    xdg_popup_send_repositioned(r, token);
}
static const struct xdg_popup_interface popup_impl = {
    .destroy = popup_destroy,
    .grab = popup_grab,
    .reposition = popup_reposition,
};

static void xdg_surface_destroy(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v) v->xdg_surface = nullptr;
    wl_resource_destroy(r);
}

static void xdg_surface_get_toplevel(wl_client *c, wl_resource *r, uint32_t id) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    wl_resource *tr = wl_resource_create(c, &xdg_toplevel_interface,
                                         wl_resource_get_version(r), id);
    if (!tr) { wl_client_post_no_memory(c); return; }
    v->toplevel = tr;
    wl_resource_set_implementation(tr, &toplevel_impl, v, nullptr);
    toplevel_send_configure(v);
}

static void xdg_surface_get_popup(wl_client *c, wl_resource *r, uint32_t id,
                                 wl_resource *parent_res, wl_resource *pos_res) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    View *pv = parent_res ? (View *)wl_resource_get_user_data(parent_res) : nullptr;
    Positioner *pos = pos_res ? (Positioner *)wl_resource_get_user_data(pos_res) : nullptr;

    wl_resource *pr = wl_resource_create(c, &xdg_popup_interface, wl_resource_get_version(r), id);
    if (!pr) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(pr, &popup_impl, v, nullptr);

    // A popup appears only after a configure; without one the menu never opens.
    int32_t lw = (pos && pos->w > 0) ? pos->w : (v->bw > 0 ? v->bw : 200);
    int32_t lh = (pos && pos->h > 0) ? pos->h : (v->bh > 0 ? v->bh : 200);
    int32_t lx = pos ? (pos->ax + pos->ox) : 0;
    int32_t ly = pos ? (pos->ay + pos->oy) : 0;

    v->parent = pv;
    v->is_popup = true;
    int32_t sc = pv ? pv->buf_scale : 1;
    v->x = (pv ? pv->x : 0) + lx * sc;
    v->y = (pv ? pv->y : 0) + ly * sc;

    xdg_popup_send_configure(pr, lx, ly, lw, lh);
    xdg_surface_send_configure(v->xdg_surface, wl_display_next_serial(g_srv.dpy));
    printf("[sfwm] popup '%s' logical(%d,%d) %dx%d -> physical(%d,%d)\n",
           v->title[0] ? v->title : "menu", lx, ly, lw, lh, v->x, v->y);
}

static void xdg_surface_set_window_geometry(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}
static void xdg_surface_ack_configure(wl_client *, wl_resource *, uint32_t) {}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

static void wm_base_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void wm_base_create_positioner(wl_client *c, wl_resource *res, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &xdg_positioner_interface,
                                        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    Positioner *p = new Positioner();
    wl_resource_set_implementation(r, &positioner_impl, p, [](wl_resource *res2) {
        delete (Positioner *)wl_resource_get_user_data(res2);
    });
}

static void wm_base_get_xdg_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    View *v = (View *)wl_resource_get_user_data(surface);
    if (!v) return;
    wl_resource *r = wl_resource_create(c, &xdg_surface_interface,
                                        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    v->xdg_surface = r;
    wl_resource_set_implementation(r, &xdg_surface_impl, v, nullptr);
}

static void wm_base_pong(wl_client *, wl_resource *, uint32_t) {}

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = wm_base_destroy,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
};

static void bind_wm_base(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 6) version = 6;   // v5 => configure_bounds / wm_capabilities
    wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &wm_base_impl, nullptr, nullptr);
}

