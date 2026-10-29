// SPDX-License-Identifier: AGPL-3.0-or-later
// misc_ext2.hpp
//
// viewporter, cursor-shape, tearing and related extensions.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ------------------------------------------------------- further protocols
//
// The goal is to provide the globals that common clients and nested compositors
// bind. Most are accepted and ignored, which is enough to keep clients starting.
// The ones that matter for running are implemented earlier: wl_compositor, wl_shm,
// wl_seat, xdg-shell and wl_output.

// ---- wp_viewporter: scaling and cropping, recorded but not applied
static void viewport_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void viewport_set_source(wl_client *, wl_resource *, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t) {}
static void viewport_set_destination(wl_client *, wl_resource *, int32_t, int32_t) {}
static const struct wp_viewport_interface viewport_impl = {
    .destroy = viewport_destroy,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};
static void viewporter_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void viewporter_get_viewport(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *v = wl_resource_create(c, &wp_viewport_interface, wl_resource_get_version(res), id);
    if (!v) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(v, &viewport_impl, nullptr, nullptr);
}
static const struct wp_viewporter_interface viewporter_impl = {
    .destroy = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};

// wp_presentation is not used here: the generated header in Termux lacks the
// interface definition, and clients do not require it.

// ---- wp_fractional_scale_v1
static void frac_scale_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wp_fractional_scale_v1_interface frac_scale_impl = {
    .destroy = frac_scale_destroy,
};
static void frac_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void frac_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    wl_resource *f = wl_resource_create(c, &wp_fractional_scale_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!f) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(f, &frac_scale_impl, nullptr, nullptr);
    // This has to be stored on the view: clients using fractional scale render at 2x
    // while buffer_scale stays 1. Ignoring it halves every logical to buffer
    // conversion and offsets the decoration into the middle of the window.
    if (View *v = surface ? (View *)wl_resource_get_user_data(surface) : nullptr) {
        v->frac_scale = g_scale > 0 ? g_scale : 1;
        printf("[frac] preferred_scale=%d for surface %p\n", v->frac_scale, (void *)surface);
    }
    wp_fractional_scale_v1_send_preferred_scale(f, (uint32_t)(120 * (g_scale > 0 ? g_scale : 1)));
}
static const struct wp_fractional_scale_manager_v1_interface frac_mgr_impl = {
    .destroy = frac_mgr_destroy,
    .get_fractional_scale = frac_mgr_get,
};

// ---- wp_cursor_shape_v1
static void cs_dev_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
// The shape is only an enumeration without pixels, so cursor_build() maps it.
static void cs_dev_set_shape(wl_client *, wl_resource *, uint32_t serial, uint32_t shape) {
    (void)serial;
    if (shape != g_cur_shape) {
        g_cur_shape = shape;
        cursor_build(shape);
        printf("[cursor] shape -> %u\n", shape);
    }
    if (!(g_srv.cursor_view && g_srv.cursor_view->layer.win)) cursor_present();
}
static const struct wp_cursor_shape_device_v1_interface cs_dev_impl = {
    .destroy = cs_dev_destroy,
    .set_shape = cs_dev_set_shape,
};
static void cs_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void cs_mgr_get_pointer(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *d = wl_resource_create(c, &wp_cursor_shape_device_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &cs_dev_impl, nullptr, nullptr);
}
static void cs_mgr_get_tablet_tool(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *d = wl_resource_create(c, &wp_cursor_shape_device_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &cs_dev_impl, nullptr, nullptr);
}
static const struct wp_cursor_shape_manager_v1_interface cs_mgr_impl = {
    .destroy = cs_mgr_destroy,
    .get_pointer = cs_mgr_get_pointer,
    .get_tablet_tool_v2 = cs_mgr_get_tablet_tool,
};

// ---- wp_single_pixel_buffer_v1: a 1x1 shm buffer created here
static void spb_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void spb_create_u32_rgba(wl_client *c, wl_resource *res, uint32_t id,
                                uint32_t r_, uint32_t g_, uint32_t b_, uint32_t a_) {
    int fd = sfwm_rt_file("sfwm-1px", 4);
    if (fd < 0) {
        wl_client_post_no_memory(c);
        return;
    }
    void *p = mmap(nullptr, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); wl_client_post_no_memory(c); return; }
    // ARGB8888 little endian: byte 0 blue, 1 green, 2 red, 3 alpha
    uint8_t *px = (uint8_t *)p;
    px[0] = (uint8_t)(b_ >> 24 ? b_ >> 24 : b_);
    px[1] = (uint8_t)g_;
    px[2] = (uint8_t)r_;
    px[3] = (uint8_t)a_;

    ShmPool *pool = new ShmPool();
    pool->fd = fd;
    pool->size = 4;
    pool->base = (uint8_t *)p;

    wl_resource *br = wl_resource_create(c, &wl_buffer_interface, wl_resource_get_version(res), id);
    if (!br) { shm_pool_unref(pool); wl_client_post_no_memory(c); return; }
    ShmBuf *sb = new ShmBuf();
    sb->pool = pool;
    sb->offset = 0;
    pool->refs++;
    sb->w = 1; sb->h = 1; sb->stride = 4; sb->format = WL_SHM_FORMAT_ARGB8888;
    wl_resource_set_implementation(br, &buffer_impl, sb, [](wl_resource *rr) {
        ShmBuf *bb = (ShmBuf *)wl_resource_get_user_data(rr);
        if (!bb) return;
        shm_pool_unref(bb->pool);
        delete bb;
    });
}
static const struct wp_single_pixel_buffer_manager_v1_interface spb_impl = {
    .destroy = spb_destroy,
    .create_u32_rgba_buffer = spb_create_u32_rgba,
};

// ---- wp_tearing_control_v1
static void tc_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void tc_set_hint(wl_client *, wl_resource *, uint32_t) {}
static const struct wp_tearing_control_v1_interface tc_impl = {
    .set_presentation_hint = tc_set_hint,
    .destroy = tc_destroy,
};
static void tc_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void tc_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &wp_tearing_control_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &tc_impl, nullptr, nullptr);
}
static const struct wp_tearing_control_manager_v1_interface tc_mgr_impl = {
    .destroy = tc_mgr_destroy,
    .get_tearing_control = tc_mgr_get,
};

// ---- zwp_relative_pointer_v1
static std::vector<wl_resource *> g_relative_ptrs;
static void rp_destroy(wl_client *, wl_resource *r) {
    for (size_t i = 0; i < g_relative_ptrs.size(); i++)
        if (g_relative_ptrs[i] == r) { g_relative_ptrs.erase(g_relative_ptrs.begin() + i); break; }
    wl_resource_destroy(r);
}
static const struct zwp_relative_pointer_v1_interface rp_impl = { .destroy = rp_destroy };
static void rp_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void rp_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zwp_relative_pointer_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &rp_impl, nullptr, nullptr);
    g_relative_ptrs.push_back(o);
}
static const struct zwp_relative_pointer_manager_v1_interface rp_mgr_impl = {
    .destroy = rp_mgr_destroy,
    .get_relative_pointer = rp_mgr_get,
};

// ---- zwp_pointer_constraints_v1: locks and confines are accepted as applied
static void lp_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void lp_set_hint(wl_client *, wl_resource *, wl_fixed_t, wl_fixed_t) {}
static void lp_set_region(wl_client *, wl_resource *, wl_resource *) {}
static const struct zwp_locked_pointer_v1_interface lp_impl = {
    .set_cursor_position_hint = lp_set_hint,
    .set_region = lp_set_region,
    .destroy = lp_destroy,
};
static void cp_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void cp_set_region(wl_client *, wl_resource *, wl_resource *) {}
static const struct zwp_confined_pointer_v1_interface cp_impl = {
    .set_region = cp_set_region,
    .destroy = cp_destroy,
};
static void pc_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void pc_mgr_lock(wl_client *c, wl_resource *res, uint32_t id, wl_resource *,
                        wl_resource *, wl_resource *, uint32_t) {
    wl_resource *o = wl_resource_create(c, &zwp_locked_pointer_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &lp_impl, nullptr, nullptr);
    zwp_locked_pointer_v1_send_locked(o);
}
static void pc_mgr_confine(wl_client *c, wl_resource *res, uint32_t id, wl_resource *,
                           wl_resource *, wl_resource *, uint32_t) {
    wl_resource *o = wl_resource_create(c, &zwp_confined_pointer_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &cp_impl, nullptr, nullptr);
    zwp_confined_pointer_v1_send_confined(o);
}
static const struct zwp_pointer_constraints_v1_interface pc_mgr_impl = {
    .destroy = pc_mgr_destroy,
    .lock_pointer = pc_mgr_lock,
    .confine_pointer = pc_mgr_confine,
};

// ---- zwp_idle_inhibit_v1
static void ii_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_idle_inhibitor_v1_interface ii_impl = { .destroy = ii_destroy };
static void ii_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void ii_mgr_create(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zwp_idle_inhibitor_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &ii_impl, nullptr, nullptr);
}
static const struct zwp_idle_inhibit_manager_v1_interface ii_mgr_impl = {
    .destroy = ii_mgr_destroy,
    .create_inhibitor = ii_mgr_create,
};

// ---- zwp_keyboard_shortcuts_inhibit_v1: shortcuts are given to the client
static void ksi_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_keyboard_shortcuts_inhibitor_v1_interface ksi_impl = { .destroy = ksi_destroy };
static void ksi_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void ksi_mgr_inhibit(wl_client *c, wl_resource *res, uint32_t id, wl_resource *, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zwp_keyboard_shortcuts_inhibitor_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &ksi_impl, nullptr, nullptr);
    zwp_keyboard_shortcuts_inhibitor_v1_send_active(o);
}
static const struct zwp_keyboard_shortcuts_inhibit_manager_v1_interface ksi_mgr_impl = {
    .destroy = ksi_mgr_destroy,
    .inhibit_shortcuts = ksi_mgr_inhibit,
};

// ---- zxdg_output_manager_v1
static void xo_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zxdg_output_v1_interface xo_impl = { .destroy = xo_destroy };
static void xo_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void xo_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *output) {
    uint32_t ver = wl_resource_get_version(res);
    wl_resource *o = wl_resource_create(c, &zxdg_output_v1_interface, ver, id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &xo_impl, output, nullptr);
    int32_t lw = g_out_w / (g_scale > 0 ? g_scale : 1);
    int32_t lh = g_out_h / (g_scale > 0 ? g_scale : 1);
    zxdg_output_v1_send_logical_position(o, 0, 0);
    zxdg_output_v1_send_logical_size(o, lw, lh);
    if (ver >= 2) {
        zxdg_output_v1_send_name(o, "SFWM-1");
        zxdg_output_v1_send_description(o, "sfwm output");
    }
    if (ver < 3) {
        zxdg_output_v1_send_done(o);
    } else if (output && wl_resource_get_version(output) >= 2) {
        // Since xdg-output v3 the done event moved to wl_output. done was already sent
        // from bind_output(), but before the client asked for the xdg output, when the
        // logical size was still unknown, so Qt saw incomplete information.
        
        
        
        //     qt.qpa.wayland: There are no outputs - creating placeholder screen
        
        
        wl_output_send_done(output);
    }
}
static const struct zxdg_output_manager_v1_interface xo_mgr_impl = {
    .destroy = xo_mgr_destroy,
    .get_xdg_output = xo_mgr_get,
};

// ---- zxdg_decoration_manager_v1
//
// SERVER_SIDE must be returned even though no decoration is drawn: with CLIENT_SIDE
// kitty draws its own borders as eight subsurfaces, each of which becomes a
// separate SurfaceFlinger layer, wasting composition time and breaking layout.







static void deco_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void deco_send_mode(wl_resource *r) {
    zxdg_toplevel_decoration_v1_send_configure(r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void deco_set_mode(wl_client *, wl_resource *r, uint32_t) { deco_send_mode(r); }
static void deco_unset_mode(wl_client *, wl_resource *r) { deco_send_mode(r); }
static const struct zxdg_toplevel_decoration_v1_interface deco_impl = {
    .destroy = deco_destroy,
    .set_mode = deco_set_mode,
    .unset_mode = deco_unset_mode,
};
static void deco_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void deco_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zxdg_toplevel_decoration_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &deco_impl, nullptr, nullptr);
    deco_send_mode(o);
}
static const struct zxdg_decoration_manager_v1_interface deco_mgr_impl = {
    .destroy = deco_mgr_destroy,
    .get_toplevel_decoration = deco_mgr_get,
};

// wl_data_device_manager / zwp_primary_selection_device_manager_v1
// Moved to protocol/data_device.hpp as a real implementation.
// Do not add the old stub back: it would shadow the current bind functions.

// Stateless globals get their bind functions from a macro.
#define DEFINE_BIND(NAME, IFACE, IMPL)                                            \
    static void bind_##NAME(wl_client *c, void *, uint32_t version, uint32_t id) { \
        if (version > IFACE.version) version = IFACE.version;                      \
        wl_resource *r = wl_resource_create(c, &IFACE, version, id);               \
        if (!r) { wl_client_post_no_memory(c); return; }                           \
        wl_resource_set_implementation(r, &IMPL, nullptr, nullptr);                \
    }

DEFINE_BIND(viewporter, wp_viewporter_interface, viewporter_impl)
DEFINE_BIND(fracescale, wp_fractional_scale_manager_v1_interface, frac_mgr_impl)
DEFINE_BIND(cursorshape, wp_cursor_shape_manager_v1_interface, cs_mgr_impl)
DEFINE_BIND(spbuffer, wp_single_pixel_buffer_manager_v1_interface, spb_impl)
DEFINE_BIND(tearing, wp_tearing_control_manager_v1_interface, tc_mgr_impl)
DEFINE_BIND(relptr, zwp_relative_pointer_manager_v1_interface, rp_mgr_impl)
DEFINE_BIND(ptrcon, zwp_pointer_constraints_v1_interface, pc_mgr_impl)
DEFINE_BIND(idleinhibit, zwp_idle_inhibit_manager_v1_interface, ii_mgr_impl)
DEFINE_BIND(ksinhibit, zwp_keyboard_shortcuts_inhibit_manager_v1_interface, ksi_mgr_impl)
DEFINE_BIND(xdgoutput, zxdg_output_manager_v1_interface, xo_mgr_impl)
DEFINE_BIND(decoration, zxdg_decoration_manager_v1_interface, deco_mgr_impl)

