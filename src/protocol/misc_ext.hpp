// SPDX-License-Identifier: AGPL-3.0-or-later
// misc_ext.hpp
//
// content_type, alpha_modifier, fifo, pointer gestures and pointer warp.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ------------------------------------------------- small extension bundle
// Clients decide their behaviour from the presence of these globals, so providing
// them only has to be cheap.

// ---- wp_content_type_v1: content type for colour and timing policy ----
static void ct_surface_set_type(wl_client *, wl_resource *r, uint32_t type) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->content_type = type;
}
static void ct_surface_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_content_type_v1_interface ct_surface_impl = {
    .destroy         = ct_surface_destroy,
    .set_content_type = ct_surface_set_type,
};

static void ct_get_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    View *v = view_from_surface(surface);
    if (!v) {
        wl_resource_post_error(res, 0, "content_type on unknown surface");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wp_content_type_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &ct_surface_impl, v, nullptr);
}

static void ct_manager_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_content_type_manager_v1_interface ct_manager_impl = {
    .destroy     = ct_manager_destroy,
    .get_surface_content_type = ct_get_surface,
};

static void bind_content_type(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_content_type_manager_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &ct_manager_impl, nullptr, nullptr);
}

// ---- wp_alpha_modifier_v1: surface-wide alpha multiplier ----
static void am_surface_set_multiplier(wl_client *, wl_resource *r, uint32_t factor) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    // 32 bit fixed point where 0xFFFFFFFF is 1.0; 0..256 is used internally.
    v->alpha_mult = (int32_t)(((uint64_t)factor * 256ull) / 0xFFFFFFFFull);
    if (v->alpha_mult < 0) v->alpha_mult = 0;
    if (v->alpha_mult > 256) v->alpha_mult = 256;
}
static void am_surface_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_alpha_modifier_surface_v1_interface am_surface_impl = {
    .destroy        = am_surface_destroy,
    .set_multiplier = am_surface_set_multiplier,
};

static void am_get_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    View *v = view_from_surface(surface);
    if (!v) {
        wl_resource_post_error(res, 0, "alpha_modifier on unknown surface");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wp_alpha_modifier_surface_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &am_surface_impl, v, nullptr);
}

static void am_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_alpha_modifier_v1_interface am_impl = {
    .destroy     = am_destroy,
    .get_surface = am_get_surface,
};

static void bind_alpha_modifier(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_alpha_modifier_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &am_impl, nullptr, nullptr);
}

// ---- wp_fifo_v1: explicit queueing barriers ----
// Frames are copied on commit and therefore already ordered, so barriers pass
static void fifo_set_barrier(wl_client *, wl_resource *) {}
static void fifo_wait_barrier(wl_client *, wl_resource *) {}
static void fifo_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_fifo_v1_interface fifo_impl = {
    .destroy      = fifo_destroy,
    .set_barrier  = fifo_set_barrier,
    .wait_barrier = fifo_wait_barrier,
};

static void fifo_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    if (!view_from_surface(surface)) {
        wl_resource_post_error(res, 0, "fifo on unknown surface");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wp_fifo_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &fifo_impl, nullptr, nullptr);
}

static void fifo_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_fifo_manager_v1_interface fifo_mgr_impl = {
    .destroy  = fifo_mgr_destroy,
    .get_fifo = fifo_get,
};

static void bind_fifo(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_fifo_manager_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &fifo_mgr_impl, nullptr, nullptr);
}

// ---- zwp_pointer_gestures_v1: touchpad gesture objects ----
// Emitting real events needs touchpad input; creating the objects keeps clients
// from disabling gesture support.
static void gest_obj_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

// These interfaces only have destroy, so wayland-scanner emits no requests struct
// and the xyz_interface name is taken; the structs are declared here.
struct gest_iface_impl {
    void (*destroy)(struct wl_client *client, struct wl_resource *resource);
};
static const struct gest_iface_impl gest_impl = { .destroy = gest_obj_destroy };

static void pg_get_swipe(wl_client *c, wl_resource *res, uint32_t id, wl_resource *pointer) {
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gesture_swipe_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&gest_impl, nullptr, nullptr);
}
static void pg_get_pinch(wl_client *c, wl_resource *res, uint32_t id, wl_resource *pointer) {
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gesture_pinch_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&gest_impl, nullptr, nullptr);
}
static void pg_get_hold(wl_client *c, wl_resource *res, uint32_t id, wl_resource *pointer) {
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gesture_hold_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&gest_impl, nullptr, nullptr);
}
static void pg_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void pg_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

// This header has no requests struct either, so one is declared here.
// Member order must follow the request order in the XML, since the opcode is the index.
struct pg_iface_impl {
    void (*get_swipe_gesture)(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *);
    void (*get_pinch_gesture)(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *);
    void (*get_hold_gesture)(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *);
    void (*release)(struct wl_client *, struct wl_resource *);
    void (*destroy)(struct wl_client *, struct wl_resource *);
};
static const struct pg_iface_impl pg_impl = {
    .get_swipe_gesture = pg_get_swipe,
    .get_pinch_gesture = pg_get_pinch,
    .get_hold_gesture  = pg_get_hold,
    .release           = pg_release,
    .destroy           = pg_destroy,
};

static void bind_pointer_gestures(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 3) version = 3;
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gestures_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&pg_impl, nullptr, nullptr);
}

// ---- wp_pointer_warp_v1: move the pointer to surface coordinates ----
static int32_t g_pointer_x_abs = 0, g_pointer_y_abs = 0;   // see the existing pointer_x and pointer_y
// Signature: warp_pointer(pointer, surface, x, y, serial).
static void warp_warp(wl_client *, wl_resource *r, wl_resource *pointer, wl_resource *surface,
                      wl_fixed_t x, wl_fixed_t y, uint32_t serial) {
    (void)pointer;
    (void)serial;
    View *v = view_from_surface(surface);
    if (!v) return;
    // viewport coordinates to screen coordinates, scaled by buffer_scale
    const double lx = wl_fixed_to_double(x), ly = wl_fixed_to_double(y);
    const int32_t sc = v->buf_scale > 0 ? v->buf_scale : 1;
    g_cur_x = v->x + (int32_t)(lx * sc);
    g_cur_y = v->y + (int32_t)(ly * sc);
    cursor_present();
}
static void warp_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_pointer_warp_v1_interface warp_impl = {
    .destroy      = warp_destroy,
    .warp_pointer = warp_warp,
};

static void bind_pointer_warp(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_pointer_warp_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &warp_impl, nullptr, nullptr);
}


