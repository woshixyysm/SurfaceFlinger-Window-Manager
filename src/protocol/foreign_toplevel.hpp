// SPDX-License-Identifier: AGPL-3.0-or-later
// foreign_toplevel.hpp
//
// ext-foreign-toplevel-list-v1: taskbars and window lists.
//
// Semantics:
// each client binds once to a manager, and every toplevel gets a handle
// the handle carries identifier, title and app_id, followed by done
// a closed window sends closed and destroys the handle
//
// Requires View and g_srv, so it comes after compositor/view.hpp.
#pragma once

#include "ext-foreign-toplevel-list-v1-server-protocol.h"
#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<wl_resource *> g_ftl_managers;   // the manager resource of each bind

struct FtlHandle {
    View        *v = nullptr;
    wl_resource *res = nullptr;
    wl_listener  surf_gone{};
};
static std::vector<FtlHandle *> g_ftl_handles;

// ---------------------------------------------------------------- handle
static void ftl_handle_destroy_req(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct ext_foreign_toplevel_handle_v1_interface ftl_handle_impl = {
    .destroy = ftl_handle_destroy_req,
};

static void ftl_handle_resource_gone(wl_resource *r) {
    for (size_t i = 0; i < g_ftl_handles.size(); i++) {
        if (g_ftl_handles[i] && g_ftl_handles[i]->res == r) {
            FtlHandle *fh = g_ftl_handles[i];
            g_ftl_handles.erase(g_ftl_handles.begin() + i);
            if (fh->v && fh->v->surface && fh->surf_gone.link.next)
                wl_list_remove(&fh->surf_gone.link);
            delete fh;
            return;
        }
    }
}

// The window is gone: send closed, then destroy the handle.
static void ftl_on_surface_gone(wl_listener *l, void *) {
    FtlHandle *fh = wl_container_of(l, fh, surf_gone);
    if (!fh) return;
    if (fh->res) {
        ext_foreign_toplevel_handle_v1_send_closed(fh->res);
        wl_resource_destroy(fh->res);   // triggers ftl_handle_resource_gone
    }
}

static void ftl_send_state(wl_resource *h, View *v) {
    if (!h || !v) return;
    // The identifier is stable for the lifetime of the handle and unique.
    // The View pointer works, since a window does not move in memory.
    char ident[40];
    snprintf(ident, sizeof(ident), "sfwm-%llx", (unsigned long long)(uintptr_t)v);
    ext_foreign_toplevel_handle_v1_send_identifier(h, ident);
    if (v->title[0]) ext_foreign_toplevel_handle_v1_send_title(h, v->title);
    if (v->app_id[0]) ext_foreign_toplevel_handle_v1_send_app_id(h, v->app_id);
    ext_foreign_toplevel_handle_v1_send_done(h);
}

static void ftl_make_handle(View *v, wl_resource *mgr) {
    if (!v || !mgr || !v->surface) return;
    wl_client *c = wl_resource_get_client(mgr);

    // Already sent to this client?
    for (FtlHandle *fh : g_ftl_handles)
        if (fh && fh->v == v && fh->res && wl_resource_get_client(fh->res) == c) return;

    wl_resource *h = wl_resource_create(c, &ext_foreign_toplevel_handle_v1_interface,
                                        wl_resource_get_version(mgr), 0);
    if (!h) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(h, &ftl_handle_impl, nullptr, ftl_handle_resource_gone);

    ext_foreign_toplevel_list_v1_send_toplevel(mgr, h);
    ftl_send_state(h, v);

    FtlHandle *fh = new FtlHandle();
    fh->v = v;
    fh->res = h;
    fh->surf_gone.notify = ftl_on_surface_gone;
    wl_resource_add_destroy_listener(v->surface, &fh->surf_gone);
    g_ftl_handles.push_back(fh);
}

// Called on the first map of a view.
static void ftl_on_view_mapped(View *v) {
    for (wl_resource *m : g_ftl_managers) ftl_make_handle(v, m);
}

// Called when the title or app id changes, to resend the state.
static void ftl_on_view_changed(View *v) {
    for (FtlHandle *fh : g_ftl_handles)
        if (fh && fh->v == v && fh->res) ftl_send_state(fh->res, v);
}

// ---------------------------------------------------------------- manager
static void ftl_manager_stop(wl_client *, wl_resource *r) {
    ext_foreign_toplevel_list_v1_send_finished(r);
    wl_resource_destroy(r);
}

static const struct ext_foreign_toplevel_list_v1_interface ftl_manager_impl = {
    .stop = ftl_manager_stop,
};

static void ftl_manager_gone(wl_resource *r) {
    for (size_t i = 0; i < g_ftl_managers.size(); i++)
        if (g_ftl_managers[i] == r) { g_ftl_managers.erase(g_ftl_managers.begin() + i); break; }
    // Handles are reclaimed by libwayland when the client disconnects.
}

static void bind_foreign_toplevel_list(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *mgr = wl_resource_create(c, &ext_foreign_toplevel_list_v1_interface, version, id);
    if (!mgr) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(mgr, &ftl_manager_impl, nullptr, ftl_manager_gone);
    g_ftl_managers.push_back(mgr);

    // Existing windows are announced immediately.
    for (View *v : g_srv.views)
        if (v && v->mapped) ftl_make_handle(v, mgr);
    printf("[ftl] ext_foreign_toplevel_list_v1 bound, %d windows announced\n", (int)g_srv.views.size());
}
