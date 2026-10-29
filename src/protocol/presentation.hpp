// SPDX-License-Identifier: AGPL-3.0-or-later
// presentation.hpp
//
// wp_presentation_feedback
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ---------------------------------------------------------------- wp_presentation
// Clients use it to learn when a frame was presented. Without it compositors such as
// Hyprland guess the timing and frame scheduling jitters. A 60 Hz refresh is reported
// and timestamps use CLOCK_MONOTONIC.
static uint64_t pres_refresh_ns(void) {
    return 16666666ull;
}

static void pres_fb_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

// wayland-scanner emits no requests struct for wp_presentation_feedback, whose only
// request is destroy, so one is declared here with the same layout.
struct pres_fb_iface_impl {
    void (*destroy)(struct wl_client *client, struct wl_resource *resource);
};
static const struct pres_fb_iface_impl pres_fb_impl = {
    .destroy = pres_fb_destroy,
};

// Events are posted directly, without the generated helpers.
static void pres_fb_post_discarded(wl_resource *fr) {
    wl_resource_post_event(fr, 2 /* discarded */);
}
static void pres_fb_post_presented(wl_resource *fr, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec,
                                   uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
    // Event numbers: 0 is sync_output, which takes a wl_output object, 1 presented and
    // 2 discarded. Posting the timestamp as event 0 marshals an object argument, and
    // libwayland disconnects the client on the resulting protocol error.
    wl_resource_post_event(fr, 1 /* presented */, sec_hi, sec_lo, nsec, refresh, seq_hi, seq_lo, flags);
}

static void pres_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void pres_feedback(wl_client *c, wl_resource *res, wl_resource *surface, uint32_t id) {
    View *v = view_from_surface(surface);
    wl_resource *fr = wl_resource_create(c, &wp_presentation_feedback_interface, wl_resource_get_version(res), id);
    if (!fr) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(fr, (const void *)&pres_fb_impl, nullptr, nullptr);
    if (!v) {
        pres_fb_post_discarded(fr);
        wl_resource_destroy(fr);
        return;
    }
    v->feedbacks.push_back(fr);
}

static const struct wp_presentation_interface pres_impl = {
    .destroy  = pres_destroy,
    .feedback = pres_feedback,
};

static void bind_presentation(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 2) version = 2;
    wl_resource *r = wl_resource_create(c, &wp_presentation_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &pres_impl, nullptr, nullptr);
    // The clock_id event is required: it tells clients which clock the presented
    // timestamps use. Without it Mesa's wsi_common_wayland, vkcube and mpv receive an
    // invalid clock source and abort in wsi_wl_convert_wayland_to_swapchain_time().
    wp_presentation_send_clock_id(r, (uint32_t)CLOCK_MONOTONIC);
}

// Called after blit() put the pixels on screen: report every pending feedback.
static void pres_send_presented(View *v) {
    if (!v || v->feedbacks.empty()) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    for (wl_resource *fr : v->feedbacks) {
        pres_fb_post_presented(
            fr,
            (uint32_t)((uint64_t)ts.tv_sec >> 32), (uint32_t)((uint64_t)ts.tv_sec & 0xffffffffu),
            (uint32_t)ts.tv_nsec, (uint32_t)pres_refresh_ns(),
            0, 0,
            WP_PRESENTATION_FEEDBACK_KIND_VSYNC);
        wl_resource_destroy(fr);
    }
    v->feedbacks.clear();
}

