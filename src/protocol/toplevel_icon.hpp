// SPDX-License-Identifier: AGPL-3.0-or-later
// toplevel_icon.hpp
//
// xdg-toplevel-icon-v1, taskbar icons for ext_foreign_toplevel_list_v1.
//
// The protocol is complete but icon data is not stored:
//   - set_icon() is accepted and ignored
//   - get_icon() creates an icon object and sends done immediately
//   - icon.add_buffer() is accepted and discarded
//   - icon.set_name() records the name
//   - icon.destroy() destroys the object
//
// Clients complete the negotiation instead of falling back because the global is
// missing, and pick an icon by app id themselves.
// Storing the icons requires reading the wl_buffer contents and remains future work.
#pragma once

#include "xdg-toplevel-icon-v1-server-protocol.h"
#include <cstring>
#include <vector>

static void ti_icon_destroy_req(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void ti_icon_set_name(wl_client *, wl_resource *r, const char *name) {
    if (name) wl_resource_set_user_data(r, strdup(name));   // recorded for diagnostics only
}

static void ti_icon_add_buffer(wl_client *, wl_resource *r, wl_resource *buffer, int32_t scale) {
    (void)r; (void)buffer; (void)scale;
    // The buffer is not kept, which keeps its lifetime simple; the protocol stays valid.
}

static const struct xdg_toplevel_icon_v1_interface ti_icon_impl = {
    .destroy    = ti_icon_destroy_req,
    .set_name   = ti_icon_set_name,
    .add_buffer = ti_icon_add_buffer,
};

static void ti_icon_resource_gone(wl_resource *r) {
    char *n = (char *)wl_resource_get_user_data(r);
    free(n);
}

static void ti_manager_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

// opcode 1 is create_icon: without a handler the requests struct entry is NULL and
// libwayland aborts the whole compositor when a client calls it.


static void ti_manager_create_icon(wl_client *c, wl_resource *r, uint32_t id) {
    int32_t version = wl_resource_get_version(r);
    wl_resource *icon = wl_resource_create(c, &xdg_toplevel_icon_v1_interface, version, id);
    if (!icon) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(icon, &ti_icon_impl, nullptr, ti_icon_resource_gone);
    printf("[icon] create_icon -> v%d\n", version);
}

static void ti_manager_set_icon(wl_client *, wl_resource *r, wl_resource *toplevel, wl_resource *icon) {
    (void)toplevel; (void)icon;
}

static const struct xdg_toplevel_icon_manager_v1_interface ti_manager_impl = {
    .destroy     = ti_manager_destroy,
    .create_icon = ti_manager_create_icon,
    .set_icon    = ti_manager_set_icon,
};

static void bind_toplevel_icon(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &xdg_toplevel_icon_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &ti_manager_impl, nullptr, nullptr);
    printf("[icon] xdg_toplevel_icon_v1 bound; icons are not stored, only the negotiation works\n");
}
