// SPDX-License-Identifier: AGPL-3.0-or-later
// image_capture.hpp
//
// ext_image_copy_capture_v1, the current capture standard used by grim,
// wl-screenrec and OBS.
//
// Relation to zwlr_screencopy_v1 in protocol/screencopy.hpp:
// the same backend, running screencap and writing into the client shm buffer.
// The new protocol is translated into that action, not a second pipeline.
//
// Scope:
//   - only ext_output_image_capture_source_v1, since there is one output
//   - only shm formats are advertised, so clients fall back to shm

//   - paint_cursors is ignored: the cursor is a separate layer and screencap
//     captures the composited output without it
//   - per-window capture is not advertised
//
// Requires only core/prelude.hpp, which brings in sc_grab_into_shm.
#pragma once

#include "ext-image-capture-source-v1-server-protocol.h"
#include "ext-image-copy-capture-v1-server-protocol.h"
#include <cstdio>
#include <cstring>
#include <ctime>

// ------------------------------------------------- ext_image_capture_source_v1
static void ics_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct ext_image_capture_source_v1_interface ics_impl = {
    .destroy = ics_destroy,
};

// --------------------------------- ext_output_image_capture_source_manager_v1
static void oicsm_create_source(wl_client *c, wl_resource *m, uint32_t id, wl_resource *output) {
    (void)output;   // there is a single output
    wl_resource *r = wl_resource_create(c, &ext_image_capture_source_v1_interface,
                                        wl_resource_get_version(m), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &ics_impl, nullptr, nullptr);
    printf("[capture] new capture source for the whole output\n");
}
static void oicsm_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct ext_output_image_capture_source_manager_v1_interface oicsm_impl = {
    .create_source = oicsm_create_source,
    .destroy       = oicsm_destroy,
};

static void bind_output_capture_source(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &ext_output_image_capture_source_manager_v1_interface,
                                        version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &oicsm_impl, nullptr, nullptr);
    printf("[capture] ext_output_image_capture_source_manager_v1 bound\n");
}

// ------------------------------------------------------ ext_image_copy_capture_frame_v1
struct CapFrame {
    wl_resource *res = nullptr;
    wl_resource *buffer = nullptr;
    bool captured = false;
};

static CapFrame *cf_of(wl_resource *r) {
    return r ? (CapFrame *)wl_resource_get_user_data(r) : nullptr;
}

static void cf_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void cf_attach_buffer(wl_client *, wl_resource *r, wl_resource *buffer) {
    CapFrame *f = cf_of(r);
    if (f) f->buffer = buffer;
}

static void cf_damage_buffer(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}

static void cf_capture(wl_client *, wl_resource *r) {
    CapFrame *f = cf_of(r);
    printf("[capture] capture request (frame=%p buf=%p)\n", (void *)r, f ? (void *)f->buffer : nullptr);
    if (!f) return;
    if (f->captured) {
        wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
                               "capture already sent");
        return;
    }
    if (!f->buffer) {
        ext_image_copy_capture_frame_v1_send_failed(r,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }
    f->captured = true;

    // The actual capture, sharing the zwlr_screencopy backend.
    if (!sc_grab_into_shm(f->buffer)) {
        printf("[capture] capture failed, check the shm buffer\n");
        ext_image_copy_capture_frame_v1_send_failed(r,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
        return;
    }

    // Protocol order: transform, damage events, presentation_time, ready.
    ext_image_copy_capture_frame_v1_send_transform(r, WL_OUTPUT_TRANSFORM_NORMAL);
    ext_image_copy_capture_frame_v1_send_damage(r, 0, 0, g_out_w, g_out_h);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ext_image_copy_capture_frame_v1_send_presentation_time(
        r, (uint32_t)((uint64_t)ts.tv_sec >> 32), (uint32_t)ts.tv_sec, (uint32_t)ts.tv_nsec);
    ext_image_copy_capture_frame_v1_send_ready(r);
    printf("[capture] frame ready %dx%d\n", g_out_w, g_out_h);
}

static const struct ext_image_copy_capture_frame_v1_interface cf_impl = {
    .destroy       = cf_destroy,
    .attach_buffer = cf_attach_buffer,
    .damage_buffer = cf_damage_buffer,
    .capture       = cf_capture,
};

static void cf_res_gone(wl_resource *r) {
    CapFrame *f = cf_of(r);
    if (f) delete f;
}

// ---------------------------------------------------- ext_image_copy_capture_session_v1
struct CapSession {
    wl_resource *res = nullptr;
};

static CapSession *cs_of(wl_resource *r) {
    return r ? (CapSession *)wl_resource_get_user_data(r) : nullptr;
}

// Send the session constraints once, after create_session.
static void cs_send_caps(wl_resource *s) {
    ext_image_copy_capture_session_v1_send_buffer_size(s, (uint32_t)g_out_w, (uint32_t)g_out_h);
    // Only shm is advertised, which makes clients use the shm path.
    ext_image_copy_capture_session_v1_send_shm_format(s, WL_SHM_FORMAT_XRGB8888);
    ext_image_copy_capture_session_v1_send_shm_format(s, WL_SHM_FORMAT_ARGB8888);
    ext_image_copy_capture_session_v1_send_done(s);
}

static void cs_create_frame(wl_client *c, wl_resource *r, uint32_t id) {
    CapSession *s = cs_of(r);
    if (!s) return;
    wl_resource *fr = wl_resource_create(c, &ext_image_copy_capture_frame_v1_interface,
                                         wl_resource_get_version(r), id);
    if (!fr) { wl_client_post_no_memory(c); return; }
    CapFrame *f = new CapFrame();
    f->res = fr;
    wl_resource_set_implementation(fr, &cf_impl, f, cf_res_gone);
    printf("[capture] create_frame -> %p\n", (void *)fr);
}

static void cs_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct ext_image_copy_capture_session_v1_interface cs_impl = {
    .create_frame = cs_create_frame,
    .destroy      = cs_destroy,
};

static void cs_res_gone(wl_resource *r) {
    CapSession *s = cs_of(r);
    if (s) delete s;
}

// ------------------------------------------- ext_image_copy_capture_cursor_session_v1
// The cursor is not drawn into the frame: it is a separate layer and screencap
// does not include it. The object still has to exist, so this is a minimal but
struct CapCursor {
    wl_resource *res = nullptr;
};

static void cc_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void cc_get_capture_session(wl_client *c, wl_resource *r, uint32_t id) {
    wl_resource *sr = wl_resource_create(c, &ext_image_copy_capture_session_v1_interface,
                                         wl_resource_get_version(r), id);
    if (!sr) { wl_client_post_no_memory(c); return; }
    CapSession *s = new CapSession();
    s->res = sr;
    wl_resource_set_implementation(sr, &cs_impl, s, cs_res_gone);
    cs_send_caps(sr);
    printf("[capture] cursor capture session treated as a full output session without the cursor\n");
}

static const struct ext_image_copy_capture_cursor_session_v1_interface cc_impl = {
    .destroy              = cc_destroy,
    .get_capture_session  = cc_get_capture_session,
};

static void cc_res_gone(wl_resource *r) {
    CapCursor *c = (CapCursor *)wl_resource_get_user_data(r);
    if (c) delete c;
}

// -------------------------------------------------- ext_image_copy_capture_manager_v1
static void iccm_create_session(wl_client *c, wl_resource *m, uint32_t id,
                                wl_resource *source, uint32_t options) {
    wl_resource *sr = wl_resource_create(c, &ext_image_copy_capture_session_v1_interface,
                                         wl_resource_get_version(m), id);
    if (!sr) { wl_client_post_no_memory(c); return; }
    CapSession *s = new CapSession();
    s->res = sr;
    wl_resource_set_implementation(sr, &cs_impl, s, cs_res_gone);
    (void)source;
    cs_send_caps(sr);
    printf("[capture] new capture session (options=0x%x, source=%p)\n", options, (void *)source);
}

static void iccm_create_cursor_session(wl_client *c, wl_resource *m, uint32_t id,
                                       wl_resource *source, wl_resource *pointer) {
    (void)source; (void)pointer;
    wl_resource *r = wl_resource_create(c, &ext_image_copy_capture_cursor_session_v1_interface,
                                        wl_resource_get_version(m), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    CapCursor *cc = new CapCursor();
    cc->res = r;
    wl_resource_set_implementation(r, &cc_impl, cc, cc_res_gone);
}

static void iccm_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct ext_image_copy_capture_manager_v1_interface iccm_impl = {
    .create_session             = iccm_create_session,
    .create_pointer_cursor_session = iccm_create_cursor_session,
    .destroy                    = iccm_destroy,
};

static void bind_image_copy_capture(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &ext_image_copy_capture_manager_v1_interface,
                                        version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &iccm_impl, nullptr, nullptr);
    printf("[capture] ext_image_copy_capture_manager_v1 bound\n");
}
