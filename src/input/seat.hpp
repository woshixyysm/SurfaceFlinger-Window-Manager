// SPDX-License-Identifier: AGPL-3.0-or-later
// seat.hpp
//
// wl_seat, wl_pointer and wl_touch bindings.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ------------------------------------------------------------- wl_seat and input
static void ptr_set_cursor(wl_client *c, wl_resource *, uint32_t, wl_resource *surface, int32_t hx, int32_t hy) {
    (void)c;
    if (!surface) { g_srv.cursor_view = nullptr; return; }
    View *v = (View *)wl_resource_get_user_data(surface);
    g_srv.cursor_view = v;
    g_srv.cursor_hot_x = hx;
    g_srv.cursor_hot_y = hy;
    if (v) v->is_cursor = true;
}
static void ptr_destroyed(wl_resource *r) {
    auto &v = g_srv.pointers;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == r) { v.erase(v.begin() + i); printf("[input] pointer destroyed, %zu left\n", v.size()); break; }
}
// A resource is destroyed either by a release request or by the client disconnecting.
// Handling release alone leaves a stale pointer in the vector, and sending events to it
// raises a protocol error and the peer disconnects.
static void ptr_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = ptr_set_cursor,
    .release = ptr_release,
};

static void touch_destroyed(wl_resource *r) {
    auto &v = g_srv.touches;
    for (size_t i = 0; i < v.size(); i++) if (v[i] == r) { v.erase(v.begin() + i); break; }
}
static void touch_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_touch_interface touch_impl = {
    .release = touch_release,
};

static bool ptr_same_client(wl_resource *p, View *v);

static void seat_get_pointer(wl_client *c, wl_resource *seat, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(seat), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &pointer_impl, nullptr, ptr_destroyed);
    g_srv.pointers.push_back(r);
    printf("[input] new pointer (v%u), %zu total\n", wl_resource_get_version(r), g_srv.pointers.size());

    // An existing focus has to be followed by enter, or the client never uses the pointer.
    View *v = g_srv.pointer_focus;
    if (v && v->surface && ptr_same_client(r, v)) {
        uint32_t serial = wl_display_next_serial(g_srv.dpy);
        wl_pointer_send_enter(r, serial, v->surface,
                              wl_fixed_from_int((g_srv.pointer_x - v->x) / v->buf_scale),
                              wl_fixed_from_int((g_srv.pointer_y - v->y) / v->buf_scale));
        if (wl_resource_get_version(r) >= 5) wl_pointer_send_frame(r);
        printf("[input] sending enter to the new pointer -> (%d,%d)\n",
               g_srv.pointer_x - v->x, g_srv.pointer_y - v->y);
    }
}

static void seat_get_keyboard(wl_client *, wl_resource *, uint32_t);
static void seat_get_touch(wl_client *c, wl_resource *seat, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_touch_interface, wl_resource_get_version(seat), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &touch_impl, nullptr, touch_destroyed);
    g_srv.touches.push_back(r);
}
static void seat_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static bool g_have_input = false;
static bool g_touch_enabled = false;   // pointers only by default: Qt Widgets ignores QTouchEvent and would swallow clicks
                                       // when receiving wl_touch. Enabled by --touch.

static void bind_seat(wl_client *c, void *, uint32_t version, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_seat_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &seat_impl, nullptr, nullptr);
    // The keyboard does not depend on a touch device, so it is always advertised.
    uint32_t caps = WL_SEAT_CAPABILITY_KEYBOARD;
    if (g_have_input) {
        caps |= WL_SEAT_CAPABILITY_POINTER;
        if (g_touch_enabled) caps |= WL_SEAT_CAPABILITY_TOUCH;
    }
    wl_seat_send_capabilities(r, caps);
    if (version >= 2) wl_seat_send_name(r, "seat0");
}

// --------------------------------------------------------------- evdev touch
struct MtSlot { int32_t id = -1; int32_t x = 0, y = 0; };

struct InputDev {
    int fd = -1;
    int32_t xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    MtSlot slots[10];
    int32_t cur_slot = 0;
    bool down = false;
    int pending = 0;      // +1 press pending, -1 release pending, 0 idle
    int32_t screen_x = 0, screen_y = 0;
    // Two-finger window drag.
    bool dragging = false;
    bool drag_started = false;         // only a drag once the start threshold is passed
    int32_t grab_off_x = 0, grab_off_y = 0;
    int32_t last_cx = 0, last_cy = 0;
    View *drag_view = nullptr;
    // Single-finger resize by dragging an edge.
    bool resizing = false;
    View *resize_view = nullptr;
    int32_t resize_x0 = 0, resize_y0 = 0;   // finger origin
    int32_t resize_w0 = 0, resize_h0 = 0;   // initial window size in logical pixels
    int32_t resize_left = 0, resize_top = 0; // initial window position
    int resize_corner = 0;                   // 1 bottom right, 2 bottom left, 3 top right, 4 top left
};
static InputDev g_in;

static View *hit_test(int32_t sx, int32_t sy) {
    View *best = nullptr;
    int32_t bestz = -0x7fffffff;
    for (View *v : g_srv.views) {
        if (!v->mapped || v->is_cursor || v->is_subsurface || v->bw <= 0 || v->bh <= 0) continue;
        if (sx < v->x || sy < v->y || sx >= v->x + v->ew() || sy >= v->y + v->eh()) continue;
        int32_t zz = g_base_z + v->stack_key();
        if (!best || zz > bestz) { best = v; bestz = zz; }
    }
    return best;
}

// Only pointers of the same client may receive enter, motion and button for a view;
// otherwise the client sees a foreign surface id and the connection is dropped.
static bool ptr_same_client(wl_resource *p, View *v) {
    if (!p || !v || !v->surface) return false;
    return wl_resource_get_client(p) == wl_resource_get_client(v->surface);
}

