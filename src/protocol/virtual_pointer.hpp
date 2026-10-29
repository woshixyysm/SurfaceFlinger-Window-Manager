// SPDX-License-Identifier: AGPL-3.0-or-later
// virtual_pointer.hpp
//
// zwlr_virtual_pointer_manager_v1, which lets a client create its own pointer.
// This is a wlroots protocol, with its XML in protocols/.
//
// Used by automation tools such as ydotool and wtype, by input replay and by bar
// scroll handlers. The Android input command injects physical events, which never
// reach a Wayland client; this protocol is the only path that does.

//
// The virtual pointer keeps a cursor position, converts motion to screen coordinates
// and feeds the existing pointer_motion(), reusing hit testing and enter/leave.
// Buttons and scroll events go to the wl_pointer of the focused client.
//
// Requires pointer_motion, raise_view, kb_set_focus and ptr_same_client from
// input/pointer.hpp, so it comes after it.
#pragma once

#include "wlr-virtual-pointer-unstable-v1-server-protocol.h"
#include <cmath>
#include <cstdio>
#include <vector>

// Virtual cursor position in screen pixels; double avoids losing fractional motion.
// -1 means unused; the first event initialises it to the centre of the output.
static double g_vp_x = -1.0, g_vp_y = -1.0;

static void vp_ensure_pos() {
    if (g_vp_x < 0.0 || g_vp_y < 0.0) {
        g_vp_x = g_out_w / 2.0;
        g_vp_y = g_out_h / 2.0;
    }
}

static void vp_clamp() {
    if (g_vp_x < 0) g_vp_x = 0;
    if (g_vp_y < 0) g_vp_y = 0;
    if (g_vp_x > g_out_w - 1) g_vp_x = g_out_w - 1;
    if (g_vp_y > g_out_h - 1) g_vp_y = g_out_h - 1;
}

// Feed the position to the real pointer path, with hit testing and enter/leave.
static void vp_apply(uint32_t time) {
    (void)time;
    vp_clamp();
    pointer_motion((int32_t)g_vp_x, (int32_t)g_vp_y, true);
}

static View *vp_focus() { return g_srv.pointer_focus; }

// ---------------------------------------------------------- zwlr_virtual_pointer_v1
static void vp_motion(wl_client *, wl_resource *, uint32_t time, wl_fixed_t dx, wl_fixed_t dy) {
    vp_ensure_pos();
    g_vp_x += wl_fixed_to_double(dx);
    g_vp_y += wl_fixed_to_double(dy);
    vp_apply(time);
}

static void vp_motion_absolute(wl_client *, wl_resource *, uint32_t time,
                               uint32_t x, uint32_t y, uint32_t x_extent, uint32_t y_extent) {
    if (x_extent == 0) x_extent = (uint32_t)g_out_w;
    if (y_extent == 0) y_extent = (uint32_t)g_out_h;
    g_vp_x = (double)x * (double)g_out_w / (double)x_extent;
    g_vp_y = (double)y * (double)g_out_h / (double)y_extent;
    vp_apply(time);
}

static void vp_button(wl_client *, wl_resource *, uint32_t time, uint32_t button, uint32_t state) {
    vp_ensure_pos();
    vp_apply(time);
    View *v = vp_focus();
    if (!v) { printf("[vptr] no window at (%.0f,%.0f) for the button\n", g_vp_x, g_vp_y); return; }
    if (state) {                        // press: take focus and raise, as a real mouse does
        kb_set_focus(v);
        raise_view(v);
    }
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    for (wl_resource *p : g_srv.pointers) {
        if (!ptr_same_client(p, v)) continue;
        wl_pointer_send_button(p, serial, time, button, state);
        if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
    }
    printf("[vptr] button 0x%x state=%u -> '%s'\n", button, state,
           v->title[0] ? v->title : "view");
}

// Scroll: value is wl_fixed; positive moves the content down or right.
static void vp_axis(wl_client *, wl_resource *, uint32_t time, uint32_t axis, wl_fixed_t value) {
    View *v = vp_focus();
    if (!v) return;
    for (wl_resource *p : g_srv.pointers) {
        if (!ptr_same_client(p, v)) continue;
        wl_pointer_send_axis(p, time, axis, value);
        if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
    }
}

static void vp_axis_source(wl_client *, wl_resource *, uint32_t src) {
    View *v = vp_focus();
    if (!v) return;
    for (wl_resource *p : g_srv.pointers) {
        if (!ptr_same_client(p, v)) continue;
        if (wl_resource_get_version(p) >= 5) wl_pointer_send_axis_source(p, src);
    }
}

static void vp_axis_stop(wl_client *, wl_resource *, uint32_t time, uint32_t axis) {
    View *v = vp_focus();
    if (!v) return;
    for (wl_resource *p : g_srv.pointers) {
        if (!ptr_same_client(p, v)) continue;
        if (wl_resource_get_version(p) >= 5) {
            wl_pointer_send_axis_stop(p, time, axis);
            wl_pointer_send_frame(p);
        }
    }
}

static void vp_axis_discrete(wl_client *, wl_resource *, uint32_t time, uint32_t axis,
                             wl_fixed_t value, int32_t discrete) {
    View *v = vp_focus();
    if (!v) return;
    for (wl_resource *p : g_srv.pointers) {
        if (!ptr_same_client(p, v)) continue;
        if (wl_resource_get_version(p) >= 5) {
            wl_pointer_send_axis_discrete(p, axis, discrete);
            wl_pointer_send_axis(p, time, axis, value);
            wl_pointer_send_frame(p);
        } else {
            wl_pointer_send_axis(p, time, axis, value);
        }
    }
}

// A frame request ends a batch of events. Every event is already followed by
// wl_pointer.frame here, matching the built-in mouse path, so there is nothing
// to do; an extra frame would run the client frame logic twice.
static void vp_frame(wl_client *, wl_resource *) {}

static void vp_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct zwlr_virtual_pointer_v1_interface vp_impl = {
    .motion          = vp_motion,
    .motion_absolute = vp_motion_absolute,
    .button          = vp_button,
    .axis            = vp_axis,
    .frame           = vp_frame,
    .axis_source     = vp_axis_source,
    .axis_stop       = vp_axis_stop,
    .axis_discrete   = vp_axis_discrete,
    .destroy         = vp_destroy,
};

// ------------------------------------------------ zwlr_virtual_pointer_manager_v1
static void vpm_new_pointer(wl_client *c, wl_resource *mgr, uint32_t id, wl_resource *seat,
                            wl_resource *output) {
    (void)seat; (void)output;   // a single seat and output, so the hint is ignored
    wl_resource *r = wl_resource_create(c, &zwlr_virtual_pointer_v1_interface,
                                        wl_resource_get_version(mgr), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &vp_impl, nullptr, nullptr);
    printf("[vptr] new virtual pointer\n");
}

static void vpm_create(wl_client *c, wl_resource *mgr, wl_resource *seat, uint32_t id) {
    vpm_new_pointer(c, mgr, id, seat, nullptr);
}
static void vpm_create_with_output(wl_client *c, wl_resource *mgr, wl_resource *seat,
                                   wl_resource *output, uint32_t id) {
    vpm_new_pointer(c, mgr, id, seat, output);
}
static void vpm_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct zwlr_virtual_pointer_manager_v1_interface vpm_impl = {
    .create_virtual_pointer             = vpm_create,
    .destroy                            = vpm_destroy,
    .create_virtual_pointer_with_output = vpm_create_with_output,
};

static void bind_virtual_pointer(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 2) version = 2;
    wl_resource *r = wl_resource_create(c, &zwlr_virtual_pointer_manager_v1_interface,
                                        version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &vpm_impl, nullptr, nullptr);
    printf("[vptr] zwlr_virtual_pointer_manager_v1 bound\n");
}
