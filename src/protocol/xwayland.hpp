// SPDX-License-Identifier: AGPL-3.0-or-later
// xwayland.hpp
//
// xwayland-shell-v1
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ---------------------------------------------------------------- xwayland-shell-v1
// Xwayland 23.1 and later refuse to start rootless unless xwayland_shell_v1 can be bound.
// The protocol only marks a wl_surface as an X window, so accepting the requests is
// sufficient and no event has to be sent.
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
    // Record the surface as well, so log lines can be matched up.
    printf("[xwl] new X window surface#%u -> xwayland_surface#%u\n",
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
    printf("[xwl] Xwayland bound xwayland_shell_v1\n");
}

// ---------------------------------------------------------------- main
