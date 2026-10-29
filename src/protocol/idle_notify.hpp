// SPDX-License-Identifier: AGPL-3.0-or-later
// idle_notify.hpp
//
// ext-idle-notifier-v1, used by swayidle, hypridle and display timeouts.
//
// Semantics:
// A client creates a notifier with a seat and a timeout; idled is sent after that
// much inactivity and resumed on the next input.
//
// The last input time lives here and is updated through idle_note_activity()
// from the entry points in input/touch.hpp and input/pointer.hpp.
#pragma once

#include "ext-idle-notify-v1-server-protocol.h"
#include <vector>

static uint32_t g_idle_last_input = 0;      // now_ms()
static wl_event_source *g_idle_timer = nullptr;

struct IdleNotifier {
    wl_resource *res = nullptr;
    uint32_t     timeout_ms = 0;
    bool         idled = false;
};
static std::vector<IdleNotifier *> g_idle_notifiers;

static void idle_note_activity() {
    g_idle_last_input = now_ms();
    for (IdleNotifier *n : g_idle_notifiers) {
        if (n && n->idled) {
            ext_idle_notification_v1_send_resumed(n->res);
            n->idled = false;
        }
    }
}

static void idle_notifier_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct ext_idle_notification_v1_interface idle_notif_impl = {
    .destroy = idle_notifier_destroy,
};

static void idle_notif_resource_gone(wl_resource *r) {
    for (size_t i = 0; i < g_idle_notifiers.size(); i++) {
        if (g_idle_notifiers[i] && g_idle_notifiers[i]->res == r) {
            delete g_idle_notifiers[i];
            g_idle_notifiers.erase(g_idle_notifiers.begin() + i);
            return;
        }
    }
}

// Polled every 200 ms, which costs nothing.
static int on_idle_tick(void *) {
    const uint32_t now = now_ms();
    for (IdleNotifier *n : g_idle_notifiers) {
        if (!n || n->idled) continue;
        if ((uint32_t)(now - g_idle_last_input) >= n->timeout_ms) {
            ext_idle_notification_v1_send_idled(n->res);
            n->idled = true;
        }
    }
    if (g_idle_timer) wl_event_source_timer_update(g_idle_timer, 200);
    return 0;
}

static void idle_get_notification(wl_client *c, wl_resource *res, uint32_t id,
                                  uint32_t timeout_ms, wl_resource *seat) {
    (void)seat;   // a single seat, so the argument is ignored
    wl_resource *r = wl_resource_create(c, &ext_idle_notification_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &idle_notif_impl, nullptr, idle_notif_resource_gone);

    IdleNotifier *n = new IdleNotifier();
    n->res = r;
    n->timeout_ms = timeout_ms ? timeout_ms : 1;
    g_idle_notifiers.push_back(n);

    // Already idle for long enough, so send idled immediately.
    if ((uint32_t)(now_ms() - g_idle_last_input) >= n->timeout_ms) {
        ext_idle_notification_v1_send_idled(r);
        n->idled = true;
    }
    printf("[idle] new notifier timeout=%u ms, %d notifiers\n",
           n->timeout_ms, (int)g_idle_notifiers.size());
}

static void idle_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct ext_idle_notifier_v1_interface idle_notifier_impl = {
    .destroy            = idle_destroy,
    .get_idle_notification = idle_get_notification,
};

static void bind_idle_notify(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &ext_idle_notifier_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &idle_notifier_impl, nullptr, nullptr);

    if (!g_idle_last_input) g_idle_last_input = now_ms();
    if (!g_idle_timer) {
        g_idle_timer = wl_event_loop_add_timer(g_srv.loop, on_idle_tick, nullptr);
        wl_event_source_timer_update(g_idle_timer, 200);
        printf("[idle] ext_idle_notifier_v1 ready, polling every 200 ms\n");
    }
}
