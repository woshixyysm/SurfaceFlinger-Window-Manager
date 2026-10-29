// SPDX-License-Identifier: AGPL-3.0-or-later
// workspace.hpp
//
// ext_workspace_manager_v1, workspace enumeration and switching.
//
// Used by bars such as quickshell and niri, by the waybar workspace module
// and by desktop indicators.
//
// Workspaces are real here: g_srv.views is the window table and every view carries
// a workspace number. Switching hides the layers of the views that do not belong
// to the active workspace and makes View::blit() drop their frames, because
// place() would otherwise show them again.

//
// Policy:
//   - four workspaces named 1 to 4, with a one dimensional coordinate
//   - one group; output_enter uses the wl_output the client bound
//   - only activate, deactivate and assign are advertised
//   - new windows join the active workspace
//   - panels, the input method and the cursor are always visible
//
// Requires View and g_srv through compositor/surface.hpp, and output_for_client
// from protocol/output.hpp.
#pragma once

#include "ext-workspace-v1-server-protocol.h"
#include <cstdio>
#include <cstring>
#include <vector>

#define WS_MAX 8
static int g_ws_count  = 4;   // number of workspaces
static int g_ws_active = 0;   // active workspace, zero based

// Called from notify_mapped(): a new window joins the active workspace.
static void ws_assign_new(View *v) { if (v) v->ws = g_ws_active; }

// Whether this view should be visible; View::blit() skips the ones that are not.
static bool ws_visible(View *v) {
    if (!v) return false;
    // The cursor stays visible, including while locked.
    if (v->is_cursor) return true;
    // While locked only the lock surface is drawn.
    if (g_lock_active) return v->is_lock;
    // Panels, the input method and subsurfaces ignore workspaces.
    if (v->is_subsurface || v->is_layer || v->is_ime) return true;
    return v->ws == g_ws_active;
}

static const char *ws_name(int idx) {
    static char buf[WS_MAX][16];
    if (idx < 0 || idx >= WS_MAX) return "?";
    snprintf(buf[idx], sizeof(buf[idx]), "%d", idx + 1);
    return buf[idx];
}

struct WsClient;
struct WsHandle {
    WsClient *cli = nullptr;
    int idx = 0;
};
struct WsClient {
    wl_resource *mgr = nullptr;
    wl_resource *group = nullptr;
    WsHandle *h[WS_MAX] = {nullptr};
    wl_resource *hres[WS_MAX] = {nullptr};
};
static std::vector<WsClient *> g_ws_clients;

// --------------------------------------------------------------- send state
static void ws_send_handle_state(WsHandle *wh) {
    if (!wh || !wh->cli || !wh->cli->hres[wh->idx]) return;
    wl_resource *r = wh->cli->hres[wh->idx];
    ext_workspace_handle_v1_send_id(r, ws_name(wh->idx));      // a stable id, the index itself
    ext_workspace_handle_v1_send_name(r, ws_name(wh->idx));
    struct wl_array a;
    wl_array_init(&a);
    uint32_t *p = (uint32_t *)wl_array_add(&a, sizeof(uint32_t));
    if (p) *p = (uint32_t)wh->idx;                              // one dimensional coordinate
    ext_workspace_handle_v1_send_coordinates(r, &a);
    wl_array_release(&a);
    ext_workspace_handle_v1_send_state(r, wh->idx == g_ws_active
                                            ? EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE : 0);
    ext_workspace_handle_v1_send_capabilities(r,
        EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE |
        EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE |
        EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ASSIGN);
}

static void ws_broadcast() {
    std::vector<WsClient *> snap = g_ws_clients;
    for (WsClient *c : snap) {
        if (!c || !c->mgr) continue;
        for (int i = 0; i < g_ws_count; i++) ws_send_handle_state(c->h[i]);
        ext_workspace_manager_v1_send_done(c->mgr);
    }
}

// ------------------------------------------------------------------ switch
static void ws_apply_visibility() {
    for (View *v : g_srv.views) {
        if (!v || !v->mapped) continue;
        if (v->is_cursor || v->is_subsurface || v->is_layer || v->is_ime) continue;
        const bool on = (v->ws == g_ws_active);
        if (v->layer.win) {
            if (on) {
                v->place();                  // back in view: restore the geometry, which shows the layer
                printf("[ws] show '%s' (ws %d)\n",
                       v->title[0] ? v->title : "view", v->ws + 1);
            } else {
                v->layer.setVisible(false);  // leaving: hide explicitly
                printf("[ws] hide '%s' (ws %d)\n",
                       v->title[0] ? v->title : "view", v->ws + 1);
            }
        }
    }
}

static void ws_activate(int idx) {
    if (idx < 0 || idx >= g_ws_count) return;
    if (idx == g_ws_active) { ws_broadcast(); return; }
    printf("[ws] switch workspace %d -> %d\n", g_ws_active + 1, idx + 1);
    g_ws_active = idx;
    ws_apply_visibility();
    ws_broadcast();
}

// -------------------------------------------------------- handle requests
static void ws_h_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void ws_h_activate(wl_client *, wl_resource *r) {
    WsHandle *wh = (WsHandle *)wl_resource_get_user_data(r);
    if (wh) ws_activate(wh->idx);
}
static void ws_h_deactivate(wl_client *, wl_resource *r) {
    WsHandle *wh = (WsHandle *)wl_resource_get_user_data(r);
    // Deactivating an inactive workspace is a no-op, and deactivating the active one is
    // refused, which would leave no window at all. A single done keeps clients moving.
    if (wh && wh->cli && wh->cli->mgr) ext_workspace_manager_v1_send_done(wh->cli->mgr);
}
static void ws_h_assign(wl_client *, wl_resource *r, wl_resource *) {
    WsHandle *wh = (WsHandle *)wl_resource_get_user_data(r);
    if (wh && wh->cli && wh->cli->mgr) ext_workspace_manager_v1_send_done(wh->cli->mgr);
}
static void ws_h_remove(wl_client *, wl_resource *r) {
    WsHandle *wh = (WsHandle *)wl_resource_get_user_data(r);
    if (wh && wh->cli && wh->cli->mgr) ext_workspace_manager_v1_send_done(wh->cli->mgr);
}

static const struct ext_workspace_handle_v1_interface ws_h_impl = {
    .destroy    = ws_h_destroy,
    .activate   = ws_h_activate,
    .deactivate = ws_h_deactivate,
    .assign     = ws_h_assign,
    .remove     = ws_h_remove,
};

static void ws_h_res_gone(wl_resource *r) {
    WsHandle *wh = (WsHandle *)wl_resource_get_user_data(r);
    if (!wh) return;
    if (wh->cli && wh->idx >= 0 && wh->idx < WS_MAX) wh->cli->hres[wh->idx] = nullptr;
    delete wh;
}

// --------------------------------------------------------- group requests
static void ws_g_create_workspace(wl_client *, wl_resource *r, const char *) {
    // create_workspace is not advertised, so the request is ignored after a done.
    WsClient *c = (WsClient *)wl_resource_get_user_data(r);
    if (c && c->mgr) ext_workspace_manager_v1_send_done(c->mgr);
}
static void ws_g_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct ext_workspace_group_handle_v1_interface ws_g_impl = {
    .create_workspace = ws_g_create_workspace,
    .destroy          = ws_g_destroy,
};
static void ws_g_res_gone(wl_resource *r) {
    WsClient *c = (WsClient *)wl_resource_get_user_data(r);
    if (c) c->group = nullptr;
}

// ------------------------------------------------------- manager requests
static void ws_m_commit(wl_client *, wl_resource *r) {
    // The client finished a batch, so done lets it update its workspace view at once.
    ext_workspace_manager_v1_send_done(r);
}
static void ws_m_stop(wl_client *, wl_resource *r) {
    ext_workspace_manager_v1_send_finished(r);
    wl_resource_destroy(r);
}
static const struct ext_workspace_manager_v1_interface ws_m_impl = {
    .commit = ws_m_commit,
    .stop   = ws_m_stop,
};

static void ws_m_res_gone(wl_resource *r) {
    WsClient *c = (WsClient *)wl_resource_get_user_data(r);
    if (!c) return;
    for (size_t i = 0; i < g_ws_clients.size(); i++)
        if (g_ws_clients[i] == c) { g_ws_clients.erase(g_ws_clients.begin() + i); break; }
    // The client destroys handles, or libwayland reclaims them on disconnect.
    for (int i = 0; i < WS_MAX; i++) if (c->h[i]) { c->h[i]->cli = nullptr; }
    delete c;
}

// ---------------------------------------------------------------- bind
static void bind_workspace(wl_client *cl, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *mgr = wl_resource_create(cl, &ext_workspace_manager_v1_interface, version, id);
    if (!mgr) { wl_client_post_no_memory(cl); return; }

    WsClient *c = new WsClient();
    c->mgr = mgr;
    g_ws_clients.push_back(c);
    wl_resource_set_implementation(mgr, &ws_m_impl, c, ws_m_res_gone);

    // ---- one group, for the single output
    wl_resource *g = wl_resource_create(cl, &ext_workspace_group_handle_v1_interface, version, 0);
    if (!g) { wl_client_post_no_memory(cl); return; }
    c->group = g;
    wl_resource_set_implementation(g, &ws_g_impl, c, ws_g_res_gone);
    ext_workspace_manager_v1_send_workspace_group(mgr, g);
    ext_workspace_group_handle_v1_send_capabilities(g, 0);   // create_workspace is not supported
    if (wl_resource *o = output_for_client(cl))
        ext_workspace_group_handle_v1_send_output_enter(g, o);

    // ---- N workspaces
    for (int i = 0; i < g_ws_count; i++) {
        wl_resource *h = wl_resource_create(cl, &ext_workspace_handle_v1_interface, version, 0);
        if (!h) { wl_client_post_no_memory(cl); return; }
        WsHandle *wh = new WsHandle();
        wh->cli = c;
        wh->idx = i;
        c->h[i] = wh;
        c->hres[i] = h;
        wl_resource_set_implementation(h, &ws_h_impl, wh, ws_h_res_gone);
        ext_workspace_manager_v1_send_workspace(mgr, h);
        ws_send_handle_state(wh);
        ext_workspace_group_handle_v1_send_workspace_enter(g, h);
    }
    ext_workspace_manager_v1_send_done(mgr);
    printf("[ws] ext_workspace_manager_v1 bound (%d workspaces, %d active)\n",
           g_ws_count, g_ws_active + 1);
}
