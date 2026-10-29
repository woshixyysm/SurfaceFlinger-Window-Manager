// SPDX-License-Identifier: AGPL-3.0-or-later
// session_lock.hpp
//
// ext_session_lock_manager_v1, used by swaylock, hyprlock and quickshell.
//
// The protocol expects this sequence:
//   1. lock() returns an ext_session_lock_v1 object;
//   2. get_lock_surface() per output, then configure, ack and a frame;
//   3. once every output is covered the compositor sends locked;
//   4. from then on only the lock surface may be shown;
//   5. unlock_and_destroy() restores the previous state.
//
// The existing model is reused: g_srv.views plus SurfaceFlinger layers.
// Hiding the other windows is Layer::setVisible(false), and ws_visible() blocks
// their frames in View::blit(), because place() would otherwise show them again
// through setGeometry().
//
// Requires View, g_srv, g_lock_active and ws_apply_visibility, so it comes after
// core/config.hpp and protocol/workspace.hpp.
#pragma once

#include "ext-session-lock-v1-server-protocol.h"
#include <cstdio>
#include <cstring>

struct LockState {
    wl_resource *lock = nullptr;      // ext_session_lock_v1
    wl_resource *sres = nullptr;      // ext_session_lock_surface_v1
    View        *view = nullptr;
    bool acked = false;
    bool locked_sent = false;
};
static LockState *g_lock = nullptr;

static void lock_on_mapped(View *v);   // defined below

// --------------------------------------------------------------- visibility
static void lock_hide_others() {
    for (View *o : g_srv.views) {
        if (!o || !o->mapped || o->is_lock) continue;
        if (o->is_cursor || o->is_subsurface) continue;
        if (o->layer.win) o->layer.setVisible(false);
    }
}

static void lock_restore_others() {
    for (View *o : g_srv.views) {
        if (!o || !o->mapped || o->is_lock) continue;
        if (o->is_cursor || o->is_subsurface) continue;
        const bool show = (o->is_layer || o->is_ime) ? true : (o->ws == g_ws_active);
        if (!o->layer.win) continue;
        if (show) o->place();
        else      o->layer.setVisible(false);
    }
}

// --------------------------------------------------------------- lock surface
static void ls_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void ls_ack_configure(wl_client *, wl_resource *r, uint32_t serial) {
    (void)serial;
    if (!g_lock || g_lock->sres != r) return;
    g_lock->acked = true;
    printf("[lock] lock surface ack_configure\n");
    // Already mapped, so enter the locked state now.
    if (g_lock->view && g_lock->view->mapped) lock_on_mapped(g_lock->view);
}

static const struct ext_session_lock_surface_v1_interface ls_impl = {
    .destroy       = ls_destroy,
    .ack_configure = ls_ack_configure,
};

static void ls_res_gone(wl_resource *r) {
    if (!g_lock || g_lock->sres != r) return;
    if (g_lock->view) {
        g_lock->view->is_lock = false;
        if (g_lock->view->layer.win) g_lock->view->layer.setVisible(false);
    }
    g_lock->sres = nullptr;
    g_lock->view = nullptr;
}

// First map of the lock surface, called from View::notify_mapped().
static void lock_on_mapped(View *v) {
    if (!g_lock || !g_lock->lock || !v || !v->is_lock || g_lock->locked_sent) return;
    if (!g_lock->acked) {
        printf("[lock] lock surface mapped before ack_configure, not locking yet\n");
        return;
    }
    g_lock_active = true;
    lock_hide_others();
    ext_session_lock_v1_send_locked(g_lock->lock);
    g_lock->locked_sent = true;
    printf("[lock] locked, all other windows hidden\n");
}

// ------------------------------------------------------------------ unlock
static void lock_unlock(bool send_finished) {
    if (!g_lock) return;
    LockState *st = g_lock;
    g_lock = nullptr;
    if (send_finished && st->lock)
        ext_session_lock_v1_send_finished(st->lock);   // tell the client the lock was withdrawn
    g_lock_active = false;
    lock_restore_others();
    printf("[lock] unlocked, windows visible again\n");
    // The lock object is destroyed by the client; only the state is cleared here.
}

// ---------------------------------------------------------------- session lock
static void sl_destroy(wl_client *, wl_resource *r) {
    if (g_lock && g_lock->lock == r && g_lock->locked_sent) {
        // Destroying a lock while locked is not allowed; unlock_and_destroy is required.
        wl_resource_post_error(r, EXT_SESSION_LOCK_V1_ERROR_INVALID_DESTROY,
                               "locked session must be unlocked with unlock_and_destroy");
        return;
    }
    wl_resource_destroy(r);
}

static void sl_get_lock_surface(wl_client *c, wl_resource *r, uint32_t id,
                                wl_resource *surface, wl_resource *output) {
    (void)output;   // there is a single output
    if (!g_lock || g_lock->lock != r) return;
    if (g_lock->sres) {
        wl_resource_post_error(r, EXT_SESSION_LOCK_V1_ERROR_DUPLICATE_OUTPUT,
                               "only one output, one lock surface");
        return;
    }
    View *v = (View *)wl_resource_get_user_data(surface);
    if (!v) { wl_client_post_no_memory(c); return; }
    wl_resource *lr = wl_resource_create(c, &ext_session_lock_surface_v1_interface,
                                         wl_resource_get_version(r), id);
    if (!lr) { wl_client_post_no_memory(c); return; }
    g_lock->sres = lr;
    g_lock->view = v;
    g_lock->acked = false;
    v->is_lock = true;
    wl_resource_set_implementation(lr, &ls_impl, nullptr, ls_res_gone);

    // One output only, covered completely. The size is logical, as for a toplevel.
    const int32_t sc = g_scale > 0 ? g_scale : 1;
    const uint32_t serial = wl_display_next_serial(g_srv.dpy);
    ext_session_lock_surface_v1_send_configure(lr, serial,
                                              (uint32_t)(g_out_w / sc),
                                              (uint32_t)(g_out_h / sc));
    printf("[lock] new lock surface (configure %dx%d logical)\n", g_out_w / sc, g_out_h / sc);
}

static void sl_unlock_and_destroy(wl_client *, wl_resource *r) {
    if (!g_lock || g_lock->lock != r) { wl_resource_destroy(r); return; }
    lock_unlock(false);
    wl_resource_destroy(r);
}

static const struct ext_session_lock_v1_interface sl_impl = {
    .destroy             = sl_destroy,
    .get_lock_surface    = sl_get_lock_surface,
    .unlock_and_destroy  = sl_unlock_and_destroy,
};

static void sl_res_gone(wl_resource *r) {
    if (!g_lock || g_lock->lock != r) return;
    // A client that disconnects or destroys early releases the lock.
    bool was_locked = g_lock->locked_sent;
    if (was_locked) lock_unlock(false);
    else {
        g_lock->lock = nullptr;
        if (g_lock->sres) wl_resource_destroy(g_lock->sres);
        delete g_lock;
        g_lock = nullptr;
    }
}

// ---------------------------------------------------------------- manager
static void slm_lock(wl_client *c, wl_resource *mgr, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &ext_session_lock_v1_interface,
                                        wl_resource_get_version(mgr), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    if (g_lock) {
        // A lock already exists; the new one is destroyed, as the protocol has no error
        printf("[lock] session already locked, ignoring the extra lock()\n");
        wl_resource_destroy(r);
        return;
    }
    LockState *st = new LockState();
    st->lock = r;
    g_lock = st;
    wl_resource_set_implementation(r, &sl_impl, nullptr, sl_res_gone);
    printf("[lock] new session lock request\n");
}

static void slm_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct ext_session_lock_manager_v1_interface slm_impl = {
    .destroy = slm_destroy,
    .lock    = slm_lock,
};

static void bind_session_lock(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &ext_session_lock_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &slm_impl, nullptr, nullptr);
    printf("[lock] ext_session_lock_manager_v1 bound\n");
}
