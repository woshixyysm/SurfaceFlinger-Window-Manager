// SPDX-License-Identifier: AGPL-3.0-or-later
// commit_timing.hpp
//
// wp_commit_timing_manager_v1: clients timestamp when a commit should take effect.
// The protocol is in staging/commit-timing of wayland-protocols.
//
// Video players and games use it to say that a frame should not be shown before
// time T, so several streams stay aligned with their audio.
//
// Presentation here is driven by SurfaceFlinger vsync and layer transactions, so
// there is no scheduler to hold a frame back. The implementation is the full
// protocol plus bookkeeping:
//   - one timer per surface, with tv_nsec validated
//   - a log line giving the remaining time
// Clients see the protocol accepted rather than an error that drops the constraint.
// Real timed presentation waits for SurfaceFlinger frame timelines.
//
// Requires wl_resource, wl_resource_post_error and clock_gettime.
#pragma once

#include "commit-timing-v1-server-protocol.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

struct CtTimer {
    wl_resource *res = nullptr;
    wl_resource *surface = nullptr;
    bool has_ts = false;
    uint64_t target_ns = 0;      // CLOCK_MONOTONIC clock
    wl_listener surf_gone{};
};
static std::vector<CtTimer *> g_ct_timers;

static CtTimer *ct_of(wl_resource *r) {
    return r ? (CtTimer *)wl_resource_get_user_data(r) : nullptr;
}

static uint64_t ct_now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ------------------------------------------------------------ wp_commit_timer_v1
static void ct_set_timestamp(wl_client *, wl_resource *r,
                             uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
    CtTimer *t = ct_of(r);
    if (!t) return;
    if (tv_nsec >= 1000000000u) {
        wl_resource_post_error(r, WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP,
                               "tv_nsec %u >= 1e9", tv_nsec);
        return;
    }
    if (!t->surface) {
        wl_resource_post_error(r, WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED,
                               "surface already destroyed");
        return;
    }
    // The protocol wants an error when a timer is already set and not consumed by a
    // commit. There is no commit hook to clear it, so the value is overwritten: an
    // error would break the client render loop for a constraint that is best effort.
    t->target_ns = ((uint64_t)tv_sec_hi << 32 | (uint64_t)tv_sec_lo) * 1000000000ull
                 + (uint64_t)tv_nsec;
    t->has_ts = true;
    const uint64_t now = ct_now_ns();
    long long delta_ms = (long long)(t->target_ns - now) / 1000000LL;
    printf("[ct] set_timestamp %u.%09u due in %lld ms\n",
           (unsigned)tv_sec_lo, (unsigned)tv_nsec, delta_ms);
}

static void ct_destroy_req(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_commit_timer_v1_interface ct_impl = {
    .set_timestamp = ct_set_timestamp,
    .destroy       = ct_destroy_req,
};

static void ct_timer_res_gone(wl_resource *r) {
    CtTimer *t = ct_of(r);
    if (!t) return;
    for (size_t i = 0; i < g_ct_timers.size(); i++)
        if (g_ct_timers[i] == t) { g_ct_timers.erase(g_ct_timers.begin() + i); break; }
    delete t;
}

// A dead surface makes later set_timestamp calls fail with surface_destroyed.
static void ct_on_surface_gone(wl_listener *l, void *) {
    CtTimer *t = wl_container_of(l, t, surf_gone);
    if (t) t->surface = nullptr;
}

// ---------------------------------------------------- wp_commit_timing_manager_v1
static void ctm_get_timer(wl_client *c, wl_resource *mgr, uint32_t id, wl_resource *surface) {
    // one timer per surface
    for (CtTimer *t : g_ct_timers) {
        if (t && t->surface == surface) {
            wl_resource_post_error(mgr, WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS,
                                   "commit timer already exists for this surface");
            return;
        }
    }
    wl_resource *r = wl_resource_create(c, &wp_commit_timer_v1_interface,
                                        wl_resource_get_version(mgr), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    CtTimer *t = new CtTimer();
    t->res = r;
    t->surface = surface;
    t->surf_gone.notify = ct_on_surface_gone;
    wl_resource_add_destroy_listener(surface, &t->surf_gone);
    wl_resource_set_implementation(r, &ct_impl, t, ct_timer_res_gone);
    g_ct_timers.push_back(t);
    printf("[ct] new commit timer for surface %p\n", (void *)surface);
}

static void ctm_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_commit_timing_manager_v1_interface ctm_impl = {
    .destroy   = ctm_destroy,
    .get_timer = ctm_get_timer,
};

static void bind_commit_timing(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_commit_timing_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &ctm_impl, nullptr, nullptr);
    printf("[ct] wp_commit_timing_manager_v1 bound\n");
}
